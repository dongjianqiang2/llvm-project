//===-- EJitWrapperGen.cpp - EmbeddedJIT Wrapper Code Generation ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  PASS3: Insert wrapper prologue in every ejit_entry function. Uses the
//  single-function mixed scheme: wraps the original body in a fallback
//  block with a JIT dispatch path. No separate wrapper function.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include <map>
#include <string>
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-wrapper-gen"

extern cl::opt<bool> EnableEJitGlobalCtors;

// Emit fixed-dimension taskpool fast-path C ABI calls
// (ejit_taskpool_compile_or_get_Nd, N = dim count) for entries with <= 2 dims
// (0-2 dims fit in 8 integer arg registers, no stack spill). Entries with > 2
// dims use the generic ejit_taskpool_compile_or_get.
static cl::opt<bool> EJitWrapperFixedDimEntry(
    "ejit-wrapper-fixed-dim-entry", cl::init(true), cl::Hidden,
    cl::desc("Emit fixed-dimension taskpool fast-path calls "
             "(ejit_taskpool_compile_or_get_Nd) for ejit_entry functions with "
             "<= 4 dims instead of the generic ejit_taskpool_compile_or_get"));

static cl::opt<bool> EJitWrapperTiming(
    "ejit-wrapper-timing", cl::init(false), cl::Hidden,
    cl::desc("Emit diagnostic timing probes around taskpool lookup, indirect "
             "JIT call, and read-token release in ejit_entry wrappers"));

// Emit a per-function inline-cache probe DIRECTLY in the ejit_entry wrapper
// (not a call). On a hit the wrapper loads its per-function @__ejit_icache_fn_
// <name> slot (one plain load), null-checks it, confirms the shared epoch has
// not moved since this core last drained, and tail-calls the cached
// specialization - NO ejit_icache_try call, NO read-token, NO per-call guards,
// NO funcIndex/IdxValid on the hit path.
// On a miss it falls through to jit_slow -> ejit_taskpool_compile_or_get, which
// fills the slot on success (icacheFill). v2: sticky monomorphic - the slot is
// filled once (first resolution) and read forever, so the probe needs no
// version/dims re-validation. Default off. Requires the runtime to be built
// with EJIT_SRE_SHARED_CODE_POINTERS (production preset): the inline probe has
// no per-call cross-core gate, so it is only safe when a cached pointer is
// callable on any core. When combined with -ejit-wrapper-timing the icache hit
// path is instrumented too (its own sentinel-status report line) so the wrapper
// log still shows the fast path's ejit/fn overhead.
static cl::opt<bool> EJitInlineCache(
    "ejit-inline-cache", cl::init(false), cl::Hidden,
    cl::desc("Emit a per-function inline-cache probe (a direct load of the "
             "cached specialization pointer) before the taskpool "
             "compile_or_get call in ejit_entry wrappers"));

// When the inline-cache probe is enabled, put the frame-less dispatcher
// (probe + two tail calls, ~16-48 bytes) into a dedicated
// .text.ejit_dispatch section so all dispatchers share cache lines for
// spatial locality and reduced iTLB pressure.  No effect without
// -ejit-inline-cache (the non-icache wrapper is too large to benefit).
static cl::opt<bool> EJitDispatcherCluster(
    "ejit-dispatcher-cluster", cl::init(false), cl::Hidden,
    cl::desc("Cluster frame-less icache dispatchers into .text.ejit_dispatch "
             "section for I-cache spatial locality"));

// Mark the per-function MissFn (which contains the full AOT fallback body)
// as cold so the backend places it in .text.unlikely, keeping KBs of cold
// code out of hot I-cache lines.  Miss is rare, so the cold attribute also
// enables OptSize-level optimization on the fallback body.
static cl::opt<bool> EJitMissFnCold(
    "ejit-missfn-cold", cl::init(false), cl::Hidden,
    cl::desc("Mark the icache MissFn as cold to isolate the AOT fallback "
             "body from hot I-cache"));

// Wrapper generation now unconditionally uses the unified taskpool API
// (ejit_taskpool_compile_or_get + ejit_taskpool_release_read). Both Sync
// and Async modes are runtime-configurable — the AOT wrapper code is
// identical for both. The -ejit-wrapper-async flag has been retired.

namespace {

struct PeriodArrIndInfo {
  std::string PeriodName;
  unsigned ArgIndex;
  uint32_t DimType;
};

static SmallVector<PeriodArrIndInfo, 4> getPeriodArrIndInfo(const Function &F) {
  SmallVector<PeriodArrIndInfo, 4> Result;
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return Result;

  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0))) {
      if (Tag->getString() == TAG_EJIT_PERIOD_ARR_IND) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        auto *IdxC = dyn_cast<ConstantAsMetadata>(Sub->getOperand(2));
        if (PN && IdxC)
          if (auto *CI = dyn_cast<ConstantInt>(IdxC->getValue()))
            Result.push_back({PN->getString().str(),
                              static_cast<unsigned>(CI->getZExtValue()), 0});
      }
    }
  }
  return Result;
}

// Per-lifecycle i32 global holding the dimType slot. Internal linkage so the
// same-named global in another module stays independent (each is filled with
// the same registry-assigned slot at registration). Initialized to the
// "unassigned" sentinel so a missing registration cleanly disables the path.
static GlobalVariable *getOrCreateDimTypeGlobal(Module &M,
                                                StringRef PeriodName) {
  std::string GVName = ("__ejit_dimtype_" + PeriodName).str();
  if (auto *Existing = M.getGlobalVariable(GVName))
    return Existing;
  auto *I32Ty = Type::getInt32Ty(M.getContext());
  return new GlobalVariable(
      M, I32Ty, /*isConstant=*/false, GlobalValue::InternalLinkage,
      ConstantInt::get(I32Ty, kEJitInvalidDimType), GVName);
}

// Emit registration that fills each per-lifecycle dimType global with the slot
// the process-global EJitLifecycleRegistry assigns by name: ejit_register_
// lifecycle() calls in ejit_auto_register (constructor path) plus private
// .ejit_period section entries (bare-metal / test fallback). Mirrors the period
// pass. Idempotent: skips if the static section payload already exists.
static void
emitLifecycleRegistration(Module &M,
                          const std::map<std::string, GlobalVariable *> &LCs) {
  if (LCs.empty() || M.getGlobalVariable(".ejit.registry.lifecycle"))
    return;
  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  // void ejit_register_lifecycle(const char *name, uint32_t *slotOut)
  M.getOrInsertFunction(
      FN_REGISTER_LIFECYCLE,
      FunctionType::get(Type::getVoidTy(Ctx), {PtrTy, PtrTy}, false));

  Function *AutoReg = M.getFunction(FN_AUTO_REGISTER);
  bool CreatedAutoReg = false;
  if (!AutoReg) {
    auto *AutoRegTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    AutoReg = Function::Create(AutoRegTy, GlobalValue::InternalLinkage,
                               FN_AUTO_REGISTER, &M);
    BasicBlock::Create(Ctx, "entry", AutoReg);
    ReturnInst::Create(Ctx, &AutoReg->getEntryBlock());
    CreatedAutoReg = true;
  }
  Instruction *Ret = AutoReg->getEntryBlock().getTerminator();
  FunctionCallee FnRegLc = M.getFunction(FN_REGISTER_LIFECYCLE);
  for (auto &KV : LCs) {
    IRBuilder<> Builder(Ret);
    Value *Name = Builder.CreateGlobalString(KV.first);
    Builder.CreateCall(FnRegLc,
                       {Name, Builder.CreateBitCast(KV.second, PtrTy)});
  }

  // Only register the constructor when WE created ejit_auto_register: if PASS2
  // (period registration) already created and appended it, reusing it here and
  // appending again would run the whole constructor twice.
  if (EnableEJitGlobalCtors && CreatedAutoReg)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Static registry entries for bare-metal / testing fallback. They use the
  // same linker-concatenated section model as PASS2: private arrays in
  // ".ejit_period", no sentinel and no fixed external symbol, so multiple TUs
  // can all contribute lifecycle fixups without duplicate-symbol errors.
  StructType *EntryTy = StructType::get(
      Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, /*isPacked=*/false);
  auto makeStrGV = [&](const std::string &S) -> Constant * {
    Constant *Str = ConstantDataArray::getString(Ctx, S, true);
    auto *GV =
        new GlobalVariable(M, Str->getType(), true, GlobalValue::PrivateLinkage,
                           Str, ".ejit.str.");
    return ConstantExpr::getBitCast(GV, PtrTy);
  };
  SmallVector<Constant *, 16> Entries;
  for (auto &KV : LCs) {
    Entries.push_back(ConstantStruct::get(
        EntryTy, {ConstantInt::get(I32Ty, EJIT_REG_LIFECYCLE),
                  makeStrGV(KV.first), ConstantPointerNull::get(PtrTy),
                  ConstantExpr::getBitCast(KV.second, PtrTy),
                  ConstantInt::get(I64Ty, 0)}));
  }
  if (Entries.empty())
    return;
  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  auto *GV = new GlobalVariable(
      M, ArrayTy, /*isConstant=*/true, GlobalValue::PrivateLinkage,
      ConstantArray::get(ArrayTy, Entries), ".ejit.registry.lifecycle");
  GV->setSection(".ejit_period");
  GV->setAlignment(M.getDataLayout().getABITypeAlign(EntryTy));
  appendToUsed(M, {GV});
}

// Per-function i32 global holding the dense funcIndex. Internal linkage so the
// same-named global in another module stays independent (each is filled with
// the same registry-assigned index at registration). Initialized to the
// "unregistered" sentinel so a missing/overflowing registration cleanly falls
// back without entering the taskpool.
static GlobalVariable *getOrCreateFuncIndexGlobal(Module &M,
                                                  StringRef FuncName) {
  std::string GVName = ("__ejit_funcidx_" + FuncName).str();
  if (auto *Existing = M.getGlobalVariable(GVName))
    return Existing;
  auto *I32Ty = Type::getInt32Ty(M.getContext());
  return new GlobalVariable(
      M, I32Ty, /*isConstant=*/false, GlobalValue::InternalLinkage,
      ConstantInt::get(I32Ty, kEJitInvalidFuncIndex), GVName);
}

// A per-function inline-cache global plus its dimensionality (number of
// ejit_dim params). The runtime needs NumDims to linearize the [D]^NumDims
// array on fill, and the hit-path emitter needs it to build the GEP.
struct IcacheSlotInfo {
  GlobalVariable *GV = nullptr;
  unsigned NumDims = 0;
};

// Per-function pointer-typed global holding the frozen inline-cache slot: the
// specialization pointer once resolved, null until then. Internal linkage (each
// module's copy is wired into the runtime slot-pointer table by name at
// registration). 8-byte aligned so the load is a single lock-free access on
// aarch64. The wrapper reads it DIRECTLY (plain load + null-check, then the
// shared-epoch check, then the indirect call) - no ejit_icache_try call, no
// per-call guards. The cell is core-private, so the fill and the read are
// same-core and no atomic/acquire is needed.
//
// Multi-version: the global is a [D]^NumDims array (D = EJIT_ICACHE_DIM_SIZE,
// power-of-2) indexed by the ejit_dim argument values, so each dim identity
// gets its own frozen fnPtr cell. NumDims=0 is a scalar ptr (= v2 monomorphic).
static GlobalVariable *getOrCreateIcacheFnGlobal(Module &M,
                                                 StringRef FuncName,
                                                 unsigned NumDims) {
  std::string GVName = ("__ejit_icache_fn_" + FuncName).str();
  if (auto *Existing = M.getGlobalVariable(GVName))
    return Existing;
  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);
  // Build [D]^NumDims (row-major); scalar ptr for NumDims==0.
  Type *SlotTy = PtrTy;
  const uint64_t D = EJIT_ICACHE_DIM_SIZE;
  for (unsigned I = 0; I < NumDims; ++I)
    SlotTy = ArrayType::get(SlotTy, D);
  Constant *Init = nullptr;
  if (NumDims == 0)
    Init = ConstantPointerNull::get(PtrTy);
  else
    Init = ConstantAggregateZero::get(SlotTy);
  auto *GV = new GlobalVariable(M, SlotTy, /*isConstant=*/false,
                                GlobalValue::InternalLinkage, Init, GVName);
  GV->setAlignment(Align(8));
  return GV;
}

// The probe's epoch window: { i64 seen, ptr shared }, DEFINED here.
//
// Owned by the AOT object, like the @__ejit_icache_fn_ cells, and for the same
// reason: the probe must read the very bytes the runtime writes. Declaring it
// extern and letting the linker match it up invites the two to land on
// different storage (visibility, GOT, copy relocations), and the failure is
// silent -- a zeroed window reads seen == *shared == 0, i.e. "always fresh",
// so every core keeps hitting a stale cell.
//
// linkonce_odr: every TU with a probe emits one; the linker keeps a single
// copy. Ordinary .bss, so it is core-private exactly like the cells. The
// runtime is handed this address at registration (name2 of the icache entry)
// and writes seen/shared through it.
static GlobalVariable *getOrCreateIcacheEpochGlobal(Module &M) {
  const char *GVName = "__ejit_icache_epoch";
  if (auto *Existing = M.getGlobalVariable(GVName)) {
    // The TU may already have DECLARED it (a test that prints the window, say).
    // Upgrade the declaration to a definition rather than returning it as-is:
    // leaving it undefined puts the probe back on cross-module resolution,
    // which is exactly the failure this ownership change removes -- and it
    // would be silent, because an unbound window reads as "always fresh".
    if (Existing->isDeclaration()) {
      Existing->setLinkage(GlobalValue::LinkOnceODRLinkage);
      Existing->setInitializer(
          ConstantAggregateZero::get(Existing->getValueType()));
      Existing->setVisibility(GlobalValue::HiddenVisibility);
      Existing->setDSOLocal(true);
      if (Existing->getAlign().valueOrOne() < Align(8))
        Existing->setAlignment(Align(8));
    }
    return Existing;
  }
  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);
  auto *Ty = StructType::get(Ctx, {I64Ty, PtrTy});
  auto *GV = new GlobalVariable(M, Ty, /*isConstant=*/false,
                                GlobalValue::LinkOnceODRLinkage,
                                ConstantAggregateZero::get(Ty), GVName);
  GV->setAlignment(Align(8));
  // A local definition, so the probe reaches it with adrp+add rather than a
  // GOT load -- one fewer dependent load on the hit path.
  GV->setVisibility(GlobalValue::HiddenVisibility);
  GV->setDSOLocal(true);
  return GV;
}

// Emit registration that fills each per-function dense-funcIndex global with
// the index the process-global EJitFuncRegistry assigns by name: ejit_register_
// funcindex() calls in ejit_auto_register (constructor path) plus private
// .ejit_period section entries (bare-metal / test fallback). Mirrors the
// lifecycle registration. Idempotent: skips if the static section payload
// already exists.
static void
emitFuncIndexRegistration(Module &M,
                          const std::map<std::string, GlobalVariable *> &Fns) {
  if (Fns.empty() || M.getGlobalVariable(".ejit.registry.funcindex"))
    return;
  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  // void ejit_register_funcindex(const char *name, uint32_t *slotOut)
  M.getOrInsertFunction(
      FN_REGISTER_FUNCINDEX,
      FunctionType::get(Type::getVoidTy(Ctx), {PtrTy, PtrTy}, false));

  Function *AutoReg = M.getFunction(FN_AUTO_REGISTER);
  bool CreatedAutoReg = false;
  if (!AutoReg) {
    auto *AutoRegTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    AutoReg = Function::Create(AutoRegTy, GlobalValue::InternalLinkage,
                               FN_AUTO_REGISTER, &M);
    BasicBlock::Create(Ctx, "entry", AutoReg);
    ReturnInst::Create(Ctx, &AutoReg->getEntryBlock());
    CreatedAutoReg = true;
  }
  Instruction *Ret = AutoReg->getEntryBlock().getTerminator();
  FunctionCallee FnReg = M.getFunction(FN_REGISTER_FUNCINDEX);
  for (auto &KV : Fns) {
    IRBuilder<> Builder(Ret);
    Value *Name = Builder.CreateGlobalString(KV.first);
    Builder.CreateCall(FnReg, {Name, Builder.CreateBitCast(KV.second, PtrTy)});
  }

  // Only register the constructor when WE created ejit_auto_register (else
  // PASS2 / lifecycle emission already appended it).
  if (EnableEJitGlobalCtors && CreatedAutoReg)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Static registry entries for bare-metal / testing fallback. They use the
  // same linker-concatenated section model as PASS2: private arrays in
  // ".ejit_period", no sentinel and no fixed external symbol, so multiple TUs
  // can all contribute funcIndex fixups without duplicate-symbol errors.
  StructType *EntryTy = StructType::get(
      Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, /*isPacked=*/false);
  auto makeStrGV = [&](const std::string &S) -> Constant * {
    Constant *Str = ConstantDataArray::getString(Ctx, S, true);
    auto *GV =
        new GlobalVariable(M, Str->getType(), true, GlobalValue::PrivateLinkage,
                           Str, ".ejit.str.");
    return ConstantExpr::getBitCast(GV, PtrTy);
  };
  SmallVector<Constant *, 16> Entries;
  for (auto &KV : Fns) {
    Entries.push_back(ConstantStruct::get(
        EntryTy, {ConstantInt::get(I32Ty, EJIT_REG_FUNCINDEX),
                  makeStrGV(KV.first), ConstantPointerNull::get(PtrTy),
                  ConstantExpr::getBitCast(KV.second, PtrTy),
                  ConstantInt::get(I64Ty, 0)}));
  }
  if (Entries.empty())
    return;
  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  auto *GV = new GlobalVariable(
      M, ArrayTy, /*isConstant=*/true, GlobalValue::PrivateLinkage,
      ConstantArray::get(ArrayTy, Entries), ".ejit.registry.funcindex");
  GV->setSection(".ejit_period");
  GV->setAlignment(M.getDataLayout().getABITypeAlign(EntryTy));
  appendToUsed(M, {GV});
}

// Emit registration that wires each per-function inline-cache slot global
// (@__ejit_icache_fn_<name>) into the runtime slot-pointer table by name:
// ejit_register_icache_slot() calls in ejit_auto_register (constructor path)
// plus private .ejit_period section entries (bare-metal / test fallback). The
// runtime keys the slot by the SAME registry funcIndex ejit_register_funcindex
// assigns, then writes the frozen specialization pointer through it on resolve
// (icacheFill); the wrapper reads it directly on the hit path. Idempotent:
// skips if the static section payload already exists.
static void
emitIcacheSlotRegistration(Module &M,
                           const std::map<std::string, IcacheSlotInfo> &Fns) {
  if (Fns.empty() || M.getGlobalVariable(".ejit.registry.icache"))
    return;
  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  // void ejit_register_icache_slot(const char *name, void *slot,
  //                                 uint32_t numDims, void *window,
  //                                 uint32_t probeAbi)
  // numDims gives the [D]^numDims shape for icacheFill; window + probeAbi are
  // this object's proof that its probe carries the shared-epoch check.
  M.getOrInsertFunction(FN_REGISTER_ICACHE_SLOT,
                        FunctionType::get(Type::getVoidTy(Ctx),
                                          {PtrTy, PtrTy, I32Ty, PtrTy, I32Ty},
                                          false));

  Function *AutoReg = M.getFunction(FN_AUTO_REGISTER);
  bool CreatedAutoReg = false;
  if (!AutoReg) {
    auto *AutoRegTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    AutoReg = Function::Create(AutoRegTy, GlobalValue::InternalLinkage,
                               FN_AUTO_REGISTER, &M);
    BasicBlock::Create(Ctx, "entry", AutoReg);
    ReturnInst::Create(Ctx, &AutoReg->getEntryBlock());
    CreatedAutoReg = true;
  }
  Instruction *Ret = AutoReg->getEntryBlock().getTerminator();
  // Each entry carries its own window + probe version: the constructor path's
  // counterpart of the name2/size fields the static registry entries carry.
  Constant *EpochWindow =
      ConstantExpr::getBitCast(getOrCreateIcacheEpochGlobal(M), PtrTy);
  FunctionCallee FnReg = M.getFunction(FN_REGISTER_ICACHE_SLOT);
  for (auto &KV : Fns) {
    IRBuilder<> Builder(Ret);
    Value *Name = Builder.CreateGlobalString(KV.first);
    Builder.CreateCall(FnReg, {Name, Builder.CreateBitCast(KV.second.GV, PtrTy),
                               ConstantInt::get(I32Ty, KV.second.NumDims),
                               EpochWindow,
                               ConstantInt::get(I32Ty, kEJitIcacheProbeAbi)});
  }

  if (EnableEJitGlobalCtors && CreatedAutoReg)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Static registry entries for bare-metal / testing fallback (same linker-
  // concatenated .ejit_period model as funcindex above). The `size` (i64) field
  // carries NumDims; the ejit_init walker forwards it to ejitIcacheRegisterSlot.
  StructType *EntryTy = StructType::get(
      Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, /*isPacked=*/false);
  auto makeStrGV = [&](const std::string &S) -> Constant * {
    Constant *Str = ConstantDataArray::getString(Ctx, S, true);
    auto *GV =
        new GlobalVariable(M, Str->getType(), true, GlobalValue::PrivateLinkage,
                           Str, ".ejit.str.");
    return ConstantExpr::getBitCast(GV, PtrTy);
  };
  SmallVector<Constant *, 16> Entries;
  for (auto &KV : Fns) {
    // name2 (spare for icache entries) carries the probe's epoch window, so the
    // runtime writes seen/shared into the object the probe actually reads.
    Constant *EpochWin =
        ConstantExpr::getBitCast(getOrCreateIcacheEpochGlobal(M), PtrTy);
    Entries.push_back(ConstantStruct::get(
        EntryTy, {ConstantInt::get(I32Ty, EJIT_REG_ICACHE_SLOT),
                  makeStrGV(KV.first), EpochWin,
                  ConstantExpr::getBitCast(KV.second.GV, PtrTy),
                  // low 32: numDims. high 32: the probe contract version, so
                  // the runtime can tell an epoch-checking probe from one built
                  // before the check existed (see kEJitIcacheProbeAbi).
                  ConstantInt::get(I64Ty,
                                   static_cast<uint64_t>(KV.second.NumDims) |
                                       (static_cast<uint64_t>(
                                            kEJitIcacheProbeAbi)
                                        << 32))}));
  }
  if (Entries.empty())
    return;
  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  auto *GV = new GlobalVariable(
      M, ArrayTy, /*isConstant=*/true, GlobalValue::PrivateLinkage,
      ConstantArray::get(ArrayTy, Entries), ".ejit.registry.icache");
  GV->setSection(".ejit_period");
  GV->setAlignment(M.getDataLayout().getABITypeAlign(EntryTy));
  appendToUsed(M, {GV});
}

} // anonymous namespace

PreservedAnalyses EJitWrapperGenPass::run(Module &M,
                                          ModuleAnalysisManager &AM) {
  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);

  SmallVector<Function *, 4> EntryFuncs;
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (hasMDStringEntry(MD, TAG_EJIT_ENTRY) && !F.isDeclaration())
      EntryFuncs.push_back(&F);
  }

  if (EntryFuncs.empty()) {
    return PreservedAnalyses::all();
  }

  auto *I32Ty = Type::getInt32Ty(Ctx);
  // Unified taskpool API: always declare ejit_taskpool_compile_or_get +
  // ejit_taskpool_release_read. Both Sync and Async modes share the same
  // AOT wrapper — the runtime compile mode controls whether the taskpool
  // does inline compilation or background worker dispatch.
  //   ejit_taskpool_compile_or_get(i32 funcIndex, ptr dims, i32 numDims,
  //                                ptr outFn, ptr outBucket)
  //   ejit_taskpool_release_read(i32 bucketIndex)
  M.getOrInsertFunction(
      FN_TASKPOOL_COMPILE_OR_GET,
      FunctionType::get(I32Ty, {I32Ty, PtrTy, I32Ty, PtrTy, PtrTy}, false));
  M.getOrInsertFunction(
      FN_TASKPOOL_RELEASE_READ,
      FunctionType::get(Type::getVoidTy(Ctx), {I32Ty}, false));
  // With -ejit-inline-cache the wrapper reads its per-function
  // @__ejit_icache_fn_<name> slot directly (one plain load + null-check, the
  // shared-epoch check, then the indirect call) - no ejit_icache_try call, no
  // per-call guards.

  auto isAlreadyWrapped = [](Function &F) -> bool {
    if (!F.getEntryBlock().getName().starts_with("jit_entry"))
      return false;
    // The wrapper's jit_entry references a per-function global that uniquely
    // identifies an already-wrapped function: @__ejit_icache_fn_<name> (the
    // icache probe -- a direct load for 0-dim, or a GEP for numDims>0) or, with
    // the cache off, @__ejit_funcidx_<name> (the funcIndex load in jit_entry).
    for (Instruction &I : F.getEntryBlock()) {
      Value *Ptr = nullptr;
      if (auto *LI = dyn_cast<LoadInst>(&I))
        Ptr = LI->getPointerOperand();
      else if (auto *GEP = dyn_cast<GetElementPtrInst>(&I))
        Ptr = GEP->getPointerOperand();
      if (!Ptr)
        continue;
      if (auto *GV = dyn_cast<GlobalVariable>(Ptr))
        if (GV->getName().starts_with("__ejit_funcidx_") ||
            GV->getName().starts_with("__ejit_icache_fn_"))
          return true;
    }
    return false;
  };

  // Idempotency: if a previous PASS3 run already wrapped every entry function
  // (EJitAotModulePass may invoke PASS3 several times), there is nothing to do
  // — and re-emitting the module-level lifecycle registration would duplicate
  // it.
  if (llvm::all_of(EntryFuncs,
                   [&](Function *F) { return isAlreadyWrapped(*F); }))
    return PreservedAnalyses::all();

  // Cross-module-stable dimType: gather the distinct lifecycle (period) names
  // this module references and give each a per-lifecycle i32 global seeded with
  // the "unassigned" sentinel. The slot is assigned ONCE, by name, in the
  // process-global EJitLifecycleRegistry at registration time and written into
  // this global; the wrapper LOADS it instead of baking a per-module sorted
  // guess, so two modules sharing a lifecycle observe the same slot and two
  // different lifecycles never collide (EJitLifecycleRegistry.h). funcIndex is
  // assigned the same way by the process-global EJitFuncRegistry (below).
  std::map<std::string, GlobalVariable *> DimTypeGlobals;
  for (Function *F : EntryFuncs)
    for (auto &PI : getPeriodArrIndInfo(*F))
      if (!PI.PeriodName.empty())
        DimTypeGlobals.emplace(PI.PeriodName, nullptr);
  if (DimTypeGlobals.size() > kEJitMaxDimTypes) {
    Ctx.emitError("ejit-wrapper-gen: module references " +
                  Twine(DimTypeGlobals.size()) +
                  " distinct lifecycle dimensions but at most " +
                  Twine(kEJitMaxDimTypes) + " are supported (spec §5.1)");
    return PreservedAnalyses::all();
  }
  for (auto &KV : DimTypeGlobals)
    KV.second = getOrCreateDimTypeGlobal(M, KV.first);
  emitLifecycleRegistration(M, DimTypeGlobals);

  // Explicit, registration-time dense funcIndex: give each entry function a
  // per-function i32 global seeded with kEJitInvalidFuncIndex. The dense index
  // is assigned ONCE, by name, in the process-global EJitFuncRegistry and
  // backfilled into this global; the wrapper LOADS it and falls back WITHOUT
  // entering the taskpool while it is still invalid (unregistered / capacity
  // exhausted). The loader keys its table by the SAME registry index, so no two
  // functions can alias one slot (EJitFuncRegistry.h).
  std::map<std::string, GlobalVariable *> FuncIndexGlobals;
  for (Function *F : EntryFuncs)
    FuncIndexGlobals.emplace(F->getName().str(),
                             getOrCreateFuncIndexGlobal(M, F->getName()));
  emitFuncIndexRegistration(M, FuncIndexGlobals);

  // Per-function inline-cache slot globals (@__ejit_icache_fn_<name>), created
  // and registered only when -ejit-inline-cache is on. The wrapper reads its
  // slot directly (one atomic load + null-check + indirect call) on the hit
  // path; the runtime writes the frozen specialization pointer through it on
  // resolve. Requires the runtime to be built with EJIT_SRE_SHARED_CODE_POINTERS
  // (production preset) - the inline probe has no per-call cross-core gate, so
  // it is only safe when a cached pointer is callable on any core.
  std::map<std::string, IcacheSlotInfo> IcacheFnGlobals;
  if (EJitInlineCache) {
    for (Function *F : EntryFuncs) {
      unsigned NumDims = getPeriodArrIndInfo(*F).size();
      // Skip functions exceeding the cache dimensionality cap from the icache
      // map. They are still wrapped (taskpool path) but without an icache probe;
      // the per-function DimCount > EJIT_ICACHE_MAX_DIMS check below emits the
      // compile error, so this just avoids emitting a wasteful [D]^N global.
      if (NumDims > EJIT_ICACHE_MAX_DIMS)
        continue;
      IcacheSlotInfo Info;
      Info.GV = getOrCreateIcacheFnGlobal(M, F->getName(), NumDims);
      Info.NumDims = NumDims;
      IcacheFnGlobals.emplace(F->getName().str(), Info);
    }
    emitIcacheSlotRegistration(M, IcacheFnGlobals);
  }

  LLVM_DEBUG(dbgs() << "ejit-wrapper-gen: " << EntryFuncs.size()
                    << " entry function(s)\n");
  bool Changed = false;
  for (Function *F : EntryFuncs) {
    LLVM_DEBUG(dbgs() << "ejit-wrapper-gen: wrapping " << F->getName() << "\n");
    // Idempotency guard: skip functions already wrapped by an earlier pass run.
    // PASS3 may be invoked multiple times via EJitAotModulePass (e.g. O1+O2
    // pipelines), and re-wrapping produces broken PHI nodes referencing stale
    // predecessor blocks.
    if (isAlreadyWrapped(*F))
      continue;

    // Prevent the CGSCC inliner from inlining the wrapped function into
    // callers. Each call site would duplicate the JIT dispatch logic
    // (cacheKey computation, ejit_compile_or_get call, indirect call) and
    // the inliner may produce inconsistent AOT fallback code depending on
    // call-site context. This is unconditional: the entry must stay
    // out-of-line for LTO correctness. Sema rejects always_inline on
    // ejit_entry; the guard backstops hand-written IR (noinline +
    // alwaysinline is illegal and aborts the verifier).
    if (!F->hasFnAttribute(Attribute::AlwaysInline))
      F->addFnAttr(Attribute::NoInline);

    auto PeriodInds = getPeriodArrIndInfo(*F);
    unsigned DimCount = PeriodInds.size();

    if (DimCount > 4) {
      F->getContext().emitError("ejit-wrapper-gen: more than 4 "
                                "ejit_period_arr_ind dimensions are not "
                                "supported");
      continue;
    }

    // Validate the metadata: every dim must name a non-empty lifecycle that has
    // a per-lifecycle dimType global (created above for every distinct name the
    // module references), no two dims may name the SAME lifecycle (a duplicated
    // dimension — distinct names are guaranteed distinct slots at runtime), and
    // arg indices/types must be in range. The dimType slot itself is resolved
    // at runtime via the global, never baked here.
    bool Invalid = false;
    SmallVector<StringRef, 4> SeenNames;
    unsigned ArgCount = F->arg_size();
    for (unsigned I = 0; I < DimCount; ++I) {
      auto GIt = DimTypeGlobals.find(PeriodInds[I].PeriodName);
      if (PeriodInds[I].PeriodName.empty() || GIt == DimTypeGlobals.end()) {
        F->getContext().emitError("ejit-wrapper-gen: invalid period name in "
                                  "ejit_period_arr_ind: " +
                                  PeriodInds[I].PeriodName);
        Invalid = true;
        break;
      }
      if (llvm::is_contained(SeenNames, StringRef(PeriodInds[I].PeriodName))) {
        F->getContext().emitError("ejit-wrapper-gen: duplicated lifecycle "
                                  "dimension in ejit_period_arr_ind metadata");
        Invalid = true;
        break;
      }
      SeenNames.push_back(PeriodInds[I].PeriodName);

      if (PeriodInds[I].ArgIndex >= ArgCount) {
        F->getContext().emitError("ejit-wrapper-gen: ejit_period_arr_ind "
                                  "argument index out of range");
        Invalid = true;
        break;
      }
      Value *ArgVal = F->getArg(PeriodInds[I].ArgIndex);
      if (!ArgVal->getType()->isIntegerTy()) {
        F->getContext().emitError("ejit-wrapper-gen: ejit_period_arr_ind "
                                  "argument must be an integer type");
        Invalid = true;
        break;
      }
    }

    if (Invalid)
      continue;

    // Save original entry block
    BasicBlock &OrigEntry = F->getEntryBlock();

    // Whether this function gets an icache probe: the flag is on AND the
    // function is in the icache map.
    auto IcacheIt = IcacheFnGlobals.find(F->getName().str());
    bool EmitIcacheProbe =
        EJitInlineCache && IcacheIt != IcacheFnGlobals.end();

    auto *DimPairTy = StructType::get(I32Ty, I32Ty);
    auto *I64Ty = Type::getInt64Ty(Ctx);
    bool UseFixed = EJitWrapperFixedDimEntry && DimCount <= 2;

    // Timing callees (shared by hit and slow paths).
    FunctionCallee TraceNow{};
    FunctionCallee TraceWrapper{};
    if (EJitWrapperTiming) {
      TraceNow = M.getOrInsertFunction(FN_TASKPOOL_TRACE_NOW,
                                       FunctionType::get(I64Ty, false));
      SmallVector<Type *, 8> TraceTys = {I32Ty, I32Ty, PtrTy, I32Ty, I64Ty,
                                         I64Ty, I64Ty, I64Ty};
      TraceWrapper = M.getOrInsertFunction(
          FN_TASKPOOL_TRACE_WRAPPER,
          FunctionType::get(Type::getVoidTy(Ctx), TraceTys, false));
    }

    // emitSlowPath: emit the funcIndex guard (EntryBB) + compile_or_get
    // (CallBB) + dispatch (DispatchBB) into Fn, using Fn's args. FallbackBB
    // already holds the spliced original body (with its own ret). Shared by
    // icache-off (in F) and icache-on (in MissFn).
    auto emitSlowPath = [&](Function &Fn, BasicBlock *EntryBB,
                            BasicBlock *CallBB, BasicBlock *DispatchBB,
                            BasicBlock *FallbackBB) {
      IRBuilder<> B(EntryBB);
      // Allocas live in the entry block so they dominate the call/dispatch
      // blocks (and match the original layout: allocas precede the funcidx
      // guard).
      Value *DimsAlloca = UseFixed ? nullptr
                                   : B.CreateAlloca(ArrayType::get(DimPairTy, 4),
                                                    nullptr, "ejit_dims");
      Value *OutFnAlloca = B.CreateAlloca(PtrTy, nullptr, "ejit_out_fn");
      Value *OutBucketAlloca = B.CreateAlloca(I32Ty, nullptr, "ejit_out_bucket");
      Value *OutFnArg = B.CreatePointerCast(OutFnAlloca, PtrTy);
      Value *OutBucketArg = B.CreatePointerCast(OutBucketAlloca, PtrTy);
      Value *FuncIdx = B.CreateLoad(
          I32Ty, FuncIndexGlobals[F->getName().str()], "ejit_funcidx");
      Value *IdxValid = B.CreateICmpNE(
          FuncIdx, ConstantInt::get(I32Ty, kEJitInvalidFuncIndex), "ejit_idx_ok");
      B.CreateCondBr(IdxValid, CallBB, FallbackBB);

      B.SetInsertPoint(CallBB);
      auto emitDimTypeVal = [&](unsigned I) {
        return B.CreateLoad(I32Ty, DimTypeGlobals[PeriodInds[I].PeriodName],
                            "ejit_dimtype");
      };
      auto emitInstanceVal = [&](unsigned I) -> Value * {
        Value *ArgVal = Fn.getArg(PeriodInds[I].ArgIndex);
        unsigned BW = cast<IntegerType>(ArgVal->getType())->getBitWidth();
        if (BW > 32) return B.CreateTrunc(ArgVal, I32Ty);
        if (BW < 32) return B.CreateZExt(ArgVal, I32Ty);
        return ArgVal;
      };
      Value *TBeforeLookup = nullptr, *TAfterLookup = nullptr;
      if (EJitWrapperTiming)
        TBeforeLookup = B.CreateCall(TraceNow, {}, "ejit_t_before_lookup");
      Value *Status = nullptr;
      if (UseFixed) {
        static const char *const FixedNames[] = {
            FN_TASKPOOL_COMPILE_OR_GET_0D, FN_TASKPOOL_COMPILE_OR_GET_1D,
            FN_TASKPOOL_COMPILE_OR_GET_2D, FN_TASKPOOL_COMPILE_OR_GET_3D,
            FN_TASKPOOL_COMPILE_OR_GET_4D};
        SmallVector<Type *, 12> ParamTys;
        SmallVector<Value *, 12> Args;
        ParamTys.push_back(I32Ty);
        Args.push_back(FuncIdx);
        for (unsigned I = 0; I < DimCount; ++I) {
          ParamTys.push_back(I32Ty);
          ParamTys.push_back(I32Ty);
          Args.push_back(emitDimTypeVal(I));
          Args.push_back(emitInstanceVal(I));
        }
        ParamTys.push_back(PtrTy);
        ParamTys.push_back(PtrTy);
        Args.push_back(OutFnArg);
        Args.push_back(OutBucketArg);
        FunctionCallee FixedFn = M.getOrInsertFunction(
            FixedNames[DimCount], FunctionType::get(I32Ty, ParamTys, false));
        Status = B.CreateCall(FixedFn, Args);
      } else {
        for (unsigned I = 0; I < DimCount; ++I) {
          Value *Idxs[] = {ConstantInt::get(I32Ty, 0), ConstantInt::get(I32Ty, I)};
          Value *PairPtr = B.CreateInBoundsGEP(ArrayType::get(DimPairTy, 4),
                                               DimsAlloca, Idxs);
          B.CreateStore(emitDimTypeVal(I), B.CreateStructGEP(DimPairTy, PairPtr,
                                                              0, "dim_type_ptr"));
          B.CreateStore(emitInstanceVal(I),
                        B.CreateStructGEP(DimPairTy, PairPtr, 1, "instance_ptr"));
        }
        Value *DimsPtr = DimCount > 0 ? B.CreatePointerCast(DimsAlloca, PtrTy)
                                      : ConstantPointerNull::get(PtrTy);
        Status = B.CreateCall(M.getFunction(FN_TASKPOOL_COMPILE_OR_GET),
                              {FuncIdx, DimsPtr,
                               ConstantInt::get(I32Ty, DimCount), OutFnArg,
                               OutBucketArg});
      }
      if (EJitWrapperTiming)
        TAfterLookup = B.CreateCall(TraceNow, {}, "ejit_t_after_lookup");
      Value *OutFn = B.CreateLoad(PtrTy, OutFnAlloca, "ejit_fn");
      Value *HitStatus = B.CreateICmpEQ(Status, ConstantInt::get(I32Ty, 0));
      B.CreateCondBr(B.CreateAnd(HitStatus, B.CreateIsNotNull(OutFn)),
                     DispatchBB, FallbackBB);

      B.SetInsertPoint(DispatchBB);
      SmallVector<Value *, 8> Args;
      for (auto &A : Fn.args())
        Args.push_back(&A);
      auto releaseAndTrace = [&]() {
        if (EJitWrapperTiming) {
          Value *TAfterFn = B.CreateCall(TraceNow, {}, "ejit_t_after_fn");
          Value *Bucket = B.CreateLoad(I32Ty, OutBucketAlloca);
          B.CreateCall(M.getFunction(FN_TASKPOOL_RELEASE_READ), {Bucket});
          Value *TAfterRelease = B.CreateCall(TraceNow, {}, "ejit_t_after_release");
          B.CreateCall(TraceWrapper, {FuncIdx, Status, OutFn, Bucket,
                                      TBeforeLookup, TAfterLookup, TAfterFn,
                                      TAfterRelease});
        } else {
          Value *Bucket = B.CreateLoad(I32Ty, OutBucketAlloca);
          B.CreateCall(M.getFunction(FN_TASKPOOL_RELEASE_READ), {Bucket});
        }
      };
      if (F->getReturnType()->isVoidTy()) {
        B.CreateCall(F->getFunctionType(), OutFn, Args);
        releaseAndTrace();
        B.CreateRetVoid();
      } else {
        Value *RetVal = B.CreateCall(F->getFunctionType(), OutFn, Args);
        releaseAndTrace();
        B.CreateRet(RetVal);
      }
    };

    // Helper: splice the original body into Dst, fixing PHI/uses, then erase
    // the now-empty OrigEntry.
    auto spliceOriginalBody = [&](BasicBlock *Dst) {
      OrigEntry.replaceSuccessorsPhiUsesWith(Dst);
      Dst->splice(Dst->end(), &OrigEntry, OrigEntry.begin(), OrigEntry.end());
      OrigEntry.replaceAllUsesWith(Dst);
      OrigEntry.eraseFromParent();
    };

    if (EmitIcacheProbe) {
      //=== LEVER B: frame-less wrapper (F) + noinline MissFn (slow path) =====
      // The hit path (F) is just the probe + two tail calls (br spec on hit, br
      // MissFn on miss) -- no allocas, no calls, no frame. MissFn holds the
      // funcidx guard + compile_or_get + dispatch + the AOT fallback (original
      // body), with its own frame. Miss is rare, so the call overhead is fine.
      Function *MissFn = Function::Create(F->getFunctionType(),
                                          GlobalValue::InternalLinkage,
                                          F->getName() + "_miss", &M);
      // Function::Create only inherits module-default uwtable/frame-pointer from
      // the context, NOT F's target-cpu/target-features/tune-cpu. Without those,
      // TTI::areInlineCompatible(Caller=MissFn, Callee=helper) fails the subtarget
      // feature-bit superset check, so InlinerPass rejects every cost-based
      // inlining into MissFn with "conflicting attributes" (always_inline still
      // inlines via the InlineCost.cpp AlwaysInline short-circuit). The AOT
      // fallback body then loses ~all helper inlining vs the original function.
      // Copy F's attributes so MissFn matches F's subtarget; re-affirm NoInline.
      MissFn->setAttributes(F->getAttributes());
      MissFn->addFnAttr(Attribute::NoInline);
      if (EJitMissFnCold)
        MissFn->addFnAttr(Attribute::Cold);
      MissFn->setSection(F->getSection());

      // Move ALL of F's original blocks to MissFn (not just the entry block --
      // the original function has multiple BBs; splicing only the entry leaves
      // the rest dangling in F with undef references -> IPSCCP folds to
      // unreachable). F becomes empty; MissFn's entry is the original entry.
      SmallVector<BasicBlock *, 16> OrigBlocks;
      for (BasicBlock &BB : *F)
        OrigBlocks.push_back(&BB);
      for (BasicBlock *BB : OrigBlocks)
        BB->removeFromParent();
      for (BasicBlock *BB : OrigBlocks)
        MissFn->insert(MissFn->end(), BB);
      BasicBlock *MissFallback = &MissFn->getEntryBlock();
      MissFallback->setName("miss_fallback");

      // Remap F's args to MissFn's args in ALL moved blocks (cross-function
      // arg references would crash SelectionDAG).
      for (unsigned Ai = 0; Ai < F->arg_size(); ++Ai)
        F->getArg(Ai)->replaceAllUsesWith(MissFn->getArg(Ai));

      // Create the slow-path entry (funcidx guard) BEFORE MissFallback so it
      // becomes MissFn's entry block.
      auto *MissEntry = BasicBlock::Create(Ctx, "miss_entry", MissFn,
                                            MissFallback);
      auto *MissCall = BasicBlock::Create(Ctx, "miss_call", MissFn);
      auto *MissDispatch = BasicBlock::Create(Ctx, "miss_dispatch", MissFn);
      if (!MissFallback->getTerminator()) {
        IRBuilder<> B(MissFallback);
        if (F->getReturnType()->isVoidTy())
          B.CreateRetVoid();
        else
          B.CreateRet(PoisonValue::get(F->getReturnType()));
      }
      emitSlowPath(*MissFn, MissEntry, MissCall, MissDispatch, MissFallback);

      // Wrapper (F): frame-less probe + tail calls.
      auto *JitEntry = BasicBlock::Create(Ctx, "jit_entry", F);
      // Created in control-flow order so the emitted IR reads top to bottom:
      // entry -> epoch -> dispatch -> miss.
      auto *JitIcacheEpoch = BasicBlock::Create(Ctx, "jit_icache_epoch", F);
      auto *JitIcacheDispatch = BasicBlock::Create(Ctx, "jit_icache_dispatch", F);
      auto *JitMiss = BasicBlock::Create(Ctx, "jit_miss", F);
      IRBuilder<> B(JitEntry);
      Value *TBeforeIcache = nullptr, *FuncIdxForTiming = nullptr;
      if (EJitWrapperTiming) {
        TBeforeIcache = B.CreateCall(TraceNow, {}, "ejit_t_before_icache");
        FuncIdxForTiming = B.CreateLoad(I32Ty,
                                        FuncIndexGlobals[F->getName().str()],
                                        "ejit_funcidx_t");
      }
      GlobalVariable *IcacheSlot = IcacheIt->second.GV;
      unsigned NumDims = IcacheIt->second.NumDims;
      Value *SlotPtr = IcacheSlot;
      if (NumDims > 0) {
        SmallVector<Value *, 5> Indices;
        Indices.push_back(ConstantInt::get(I32Ty, 0));
        for (unsigned I = 0; I < NumDims; ++I) {
          Value *ArgVal = F->getArg(PeriodInds[I].ArgIndex);
          unsigned BW = cast<IntegerType>(ArgVal->getType())->getBitWidth();
          Value *Idx = ArgVal;
          if (BW > 32) Idx = B.CreateTrunc(ArgVal, I32Ty);
          else if (BW < 32) Idx = B.CreateZExt(ArgVal, I32Ty);
          Indices.push_back(Idx);
        }
        SlotPtr = B.CreateInBoundsGEP(IcacheSlot->getValueType(), IcacheSlot,
                                      Indices, "ejit_ic_slot");
      }
      LoadInst *ICSlotLoad = B.CreateLoad(PtrTy, SlotPtr, "ejit_ic_fn");
      ICSlotLoad->setAlignment(Align(8));
      Value *TAfterIcache = nullptr;
      if (EJitWrapperTiming)
        TAfterIcache = B.CreateCall(TraceNow, {}, "ejit_t_after_icache");
      Value *IHit = B.CreateIsNotNull(ICSlotLoad, "ejit_icache_hit");
      IHit = B.CreateIntrinsic(Intrinsic::expect, {IHit->getType()},
                               {IHit, ConstantInt::getTrue(Ctx)});
      B.CreateCondBr(IHit, JitIcacheEpoch, JitMiss);

      // Freshness: seen == *shared. A mismatch means a period toggled since this
      // core last drained, so fall into the miss path, which drains and refills.
      // Checked after the null test so `shared` is known bound (see
      // EJitIcacheEpochRef).
      B.SetInsertPoint(JitIcacheEpoch);
      GlobalVariable *EpochGV = getOrCreateIcacheEpochGlobal(M);
      auto *EpochTy = cast<StructType>(EpochGV->getValueType());
      Value *SeenPtr = B.CreateStructGEP(EpochTy, EpochGV, 0, "ejit_ic_seen_p");
      auto *SeenLoad =
          B.CreateLoad(Type::getInt64Ty(Ctx), SeenPtr, "ejit_ic_seen");
      SeenLoad->setAlignment(Align(8)); // 8 so seen+shared can pair in one ldp
      Value *SharedPP =
          B.CreateStructGEP(EpochTy, EpochGV, 1, "ejit_ic_shared_pp");
      auto *SharedPLoad = B.CreateLoad(PtrTy, SharedPP, "ejit_ic_shared_p");
      SharedPLoad->setAlignment(Align(8));
      Value *Seen = SeenLoad;
      Value *SharedP = SharedPLoad;
      // ATOMIC: `seen` and `shared` are core-private, but peers update *shared
      // with an RMW, so a plain load here would be a data race rather than
      // bounded staleness. monotonic is enough -- single-location coherence is
      // the whole question, the cell it guards was written by this core, and
      // the slow path re-reads with acquire. Lowers to the same LDR on AArch64;
      // acquire would cost an LDAR per hit.
      auto *CurLoad = B.CreateLoad(I32Ty, SharedP, "ejit_ic_epoch");
      CurLoad->setAtomic(AtomicOrdering::Monotonic);
      CurLoad->setAlignment(Align(4));
      Value *CurEpoch = CurLoad;
      Value *IFresh = B.CreateICmpEQ(
          B.CreateZExt(CurEpoch, Type::getInt64Ty(Ctx)), Seen,
          "ejit_icache_fresh");
      IFresh = B.CreateIntrinsic(Intrinsic::expect, {IFresh->getType()},
                                 {IFresh, ConstantInt::getTrue(Ctx)});
      B.CreateCondBr(IFresh, JitIcacheDispatch, JitMiss);

      // Hit: tail-call the cached specialization directly (no release_read).
      // With -ejit-wrapper-timing the wrapper is NOT frame-less (JitEntry emits
      // bl @ejit_taskpool_trace_now), so the hit path cannot be a musttail tail
      // call: emitHitTiming() inserts calls BETWEEN the call and the ret, which
      // would violate the musttail rule ("musttail call must precede a ret") and
      // yield broken IR -> codegen silently drops the function -> boot
      // translation fault. Use a plain (framed) call + trace + ret instead; the
      // frame-less musttail fast path is reserved for the no-timing case.
      B.SetInsertPoint(JitIcacheDispatch);
      SmallVector<Value *, 8> ICArgs;
      for (auto &A : F->args()) ICArgs.push_back(&A);
      auto emitHitTiming = [&]() {
        if (!EJitWrapperTiming) return;
        Value *TAfterFn = B.CreateCall(TraceNow, {}, "ejit_t_after_fn");
        B.CreateCall(TraceWrapper,
                     {FuncIdxForTiming,
                      ConstantInt::get(I32Ty, kEJitIcacheHitTimingStatus),
                      ICSlotLoad, ConstantInt::get(I32Ty, 0), TBeforeIcache,
                      TAfterIcache, TAfterFn, TAfterFn});
      };
      if (F->getReturnType()->isVoidTy()) {
        CallInst *CI = B.CreateCall(F->getFunctionType(), ICSlotLoad, ICArgs);
        if (!EJitWrapperTiming)
          CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
        emitHitTiming();
        B.CreateRetVoid();
      } else {
        CallInst *CI = B.CreateCall(F->getFunctionType(), ICSlotLoad, ICArgs);
        if (!EJitWrapperTiming)
          CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
        emitHitTiming();
        B.CreateRet(CI);
      }

      // Miss: tail-call MissFn (which does compile_or_get + dispatch/fallback).
      B.SetInsertPoint(JitMiss);
      SmallVector<Value *, 8> MissArgs;
      for (auto &A : F->args()) MissArgs.push_back(&A);
      if (F->getReturnType()->isVoidTy()) {
        CallInst *CI = B.CreateCall(MissFn->getFunctionType(), MissFn, MissArgs);
        CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
        B.CreateRetVoid();
      } else {
        CallInst *CI = B.CreateCall(MissFn->getFunctionType(), MissFn, MissArgs);
        CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
        B.CreateRet(CI);
      }

      // Cluster all frame-less dispatchers into a dedicated section so they
      // share cache lines (spatial locality when the workload switches among
      // multiple ejit_entry functions) and reduce iTLB pressure.  MissFn
      // already copied F's original section above; only the tiny dispatcher
      // (probe + two tail calls) moves here.
      if (EJitDispatcherCluster)
        F->setSection(".text.ejit_dispatch");
    } else {
      //=== icache OFF: single-function wrapper (funcidx guard -> slow path) ==
      auto *JitEntry = BasicBlock::Create(Ctx, "jit_entry", F, &OrigEntry);
      auto *JitCall = BasicBlock::Create(Ctx, "jit_call", F);
      auto *JitFallback = BasicBlock::Create(Ctx, "jit_fallback", F);
      auto *JitDispatch = BasicBlock::Create(Ctx, "jit_dispatch", F);
      spliceOriginalBody(JitFallback);
      if (!JitFallback->getTerminator()) {
        IRBuilder<> B(JitFallback);
        if (F->getReturnType()->isVoidTy()) B.CreateRetVoid();
        else B.CreateRet(PoisonValue::get(F->getReturnType()));
      }
      emitSlowPath(*F, JitEntry, JitCall, JitDispatch, JitFallback);
    }

    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
