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
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
#include "llvm/IR/CFG.h"
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
#include <limits>
#include <map>
#include <string>

// Hard-lock the AOT-side contract constants against each other (the runtime
// side locks its own copies in EJitRuntime.cpp / EJitSharedTaskPool.cpp):
// a drift between the wrapper dim cap and the Sema param cap would let one
// stage emit what the other rejects, and a non-pow2 D corrupts the [D]^N
// icache array stride.
static_assert(EJIT_ICACHE_MAX_DIMS == llvm::ejit::MAX_PERIOD_ARR_IND_PARAMS,
              "EJIT_ICACHE_MAX_DIMS must equal MAX_PERIOD_ARR_IND_PARAMS "
              "(wrapper dim cap vs Sema ejit_period_arr_ind cap).");
static_assert((EJIT_ICACHE_DIM_SIZE & (EJIT_ICACHE_DIM_SIZE - 1)) == 0,
              "EJIT_ICACHE_DIM_SIZE must be a power of 2 "
              "([D]^numDims icache array stride).");

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

static cl::opt<bool> EJitFunctionBodyTiming(
    "ejit-function-body-timing", cl::init(false), cl::Hidden,
    cl::desc("Measure the selected AOT/JIT function body and its enclosing "
             "wrapper overhead using the SRE cycle counter"));

// Emit a per-function inline-cache probe DIRECTLY in the ejit_entry wrapper
// (not a call). On a hit the wrapper loads its cell out of the per-function
// @__ejit_icache_fn_<name> table (one load), null-checks it, and tail-calls the
// cached specialization - NO ejit_icache_try call, NO read-token, NO per-call
// guards, NO funcIndex/IdxValid on the hit path. Just "pointer non-null? jump".
// On a miss it falls through to jit_slow -> ejit_taskpool_compile_or_get, which
// fills the cell on success (icacheFill). The probe carries NO freshness check:
// the table is SHARED across cores (EJitIcacheSection), so an activate or
// deactivate zeroes the cells directly and the next probe on any core misses.
// Default off. Requires the runtime to be built
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

// Section for the per-function @__ejit_icache_fn_<name> cell table. In the
// inter-core SHARED section (.mc_shared, where the taskpool state blob also
// lives) the table is ONE object every core reads, so a deactivate zeroes a
// peer's cells directly. Left empty it lands in .bss, which on a multi-core
// target is per-core private.
//
// Placement is a link-time contract with no runtime check behind it: the symbol
// resolves to the same virtual address either way, so a per-core table under a
// cross-core runtime is undetectable. Default comes from the CMake
// EJIT_ICACHE_SECTION var; the flag lets tests pin a value.
#ifndef EJIT_ICACHE_SECTION
#define EJIT_ICACHE_SECTION ""
#endif
static cl::opt<std::string> EJitIcacheSection(
    "ejit-icache-section", cl::init(EJIT_ICACHE_SECTION), cl::Hidden,
    cl::desc("Section for the @__ejit_icache_fn_<name> inline-cache cell "
             "table. Must name the inter-core SHARED section on a multi-core "
             "target; empty leaves the table in per-core .bss"));

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

// One specialization dimension, in metadata (= source parameter) order.
// PeriodName is empty exactly when IsConst.
struct DimInfo {
  std::string PeriodName;
  unsigned ArgIndex;
  uint32_t DimType;
  bool IsConst;
};

struct BoundPtrInfo {
  std::string PeriodName;
  unsigned ArgIndex;
  uint64_t PointeeSize;
};

static SmallVector<BoundPtrInfo, 8> getBoundPtrInfos(const Function &F) {
  SmallVector<BoundPtrInfo, 8> Result;
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return Result;
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 4)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
    auto *Idx = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(2));
    auto *Size = mdconst::dyn_extract<ConstantInt>(Sub->getOperand(3));
    if (Tag && Tag->getString() == TAG_EJIT_BOUND_PTR && PN && Idx && Size)
      Result.push_back(BoundPtrInfo{PN->getString().str(),
                                    static_cast<unsigned>(Idx->getZExtValue()),
                                    Size->getZExtValue()});
  }
  return Result;
}

static SmallVector<DimInfo, 4> getDimInfo(const Function &F) {
  SmallVector<DimInfo, 4> Result;
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return Result;

  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    auto *Tag = dyn_cast<MDString>(Sub->getOperand(0));
    if (!Tag)
      continue;
    bool IsConst = Tag->getString() == TAG_EJIT_CONST_DIM;
    if (!IsConst && Tag->getString() != TAG_EJIT_PERIOD_ARR_IND)
      continue;
    auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
    auto *IdxC = dyn_cast<ConstantAsMetadata>(Sub->getOperand(2));
    if (PN && IdxC)
      if (auto *CI = dyn_cast<ConstantInt>(IdxC->getValue()))
        Result.push_back({PN->getString().str(),
                          static_cast<unsigned>(CI->getZExtValue()), 0,
                          IsConst});
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
  GV->setSection(SECT_EJIT_PERIOD);
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
// array on fill, and the hit-path emitter needs it to build the GEP. The
// slot's sentinel (when the table is defined pre-filled with &MissFn) is
// derived from the table's initializer at registration-emission time, so no
// bookkeeping is kept here.
struct IcacheSlotInfo {
  GlobalVariable *GV = nullptr;
  unsigned NumDims = 0;
};

// Per-function pointer-typed global holding the inline-cache cell table: the
// specialization pointer for each dim identity once resolved, null until then
// and null again after a period toggle drains it. Internal linkage (each
// module's copy is wired into the runtime slot-pointer table by name at
// registration). 8-byte aligned so each cell is one naturally-aligned,
// tear-free access: a drain on another core zeroing a cell under a reading
// probe yields the old pointer or 0, never a torn value.
//
// Placed in EJitIcacheSection when set - SHARED across cores, partitioned by
// dim identity so writers never collide.
//
// Multi-version: the global is a [D]^NumDims array (D = EJIT_ICACHE_DIM_SIZE,
// power-of-2) indexed by the ejit_dim argument values, so each dim identity
// gets its own cell. NumDims=0 is a scalar ptr (one cell).
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
  if (!EJitIcacheSection.empty())
    GV->setSection(EJitIcacheSection);
  return GV;
}

// Sentinel initializer for an icache cell table: the [D]^NumDims shape (or the
// scalar ptr for NumDims == 0) with every element set to &MissFn. This
// definition-time value is what makes the branchless probe safe: before the
// first fill, after a drain, or before registration has run, the load yields
// MissFn and the tail call lands in the slow path instead of a null pointer.
static Constant *buildIcacheSentinelInit(Type *SlotTy, Function *MissFn,
                                          unsigned NumDims) {
  Constant *MissFnC = MissFn;
  if (NumDims == 0)
    return MissFnC;
  auto *ArrTy = cast<ArrayType>(SlotTy);
  Constant *Inner =
      buildIcacheSentinelInit(ArrTy->getElementType(), MissFn, NumDims - 1);
  SmallVector<Constant *, EJIT_ICACHE_DIM_SIZE> Elems(
      ArrTy->getNumElements(), Inner);
  return ConstantArray::get(ArrTy, Elems);
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
  GV->setSection(SECT_EJIT_PERIOD);
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

  // void ejit_register_icache_slot(const char *name, void *slot, uint32_t numDims,
  //                                void *missFn)
  // numDims tells the runtime the [D]^numDims shape so icacheFill can linearize.
  // missFn is non-null exactly for sentinel-form slots: the runtime writes it
  // back into the cells on drain / fill-retract so the branchless probe never
  // BLRs 0. Null for guarded (3D/4D, timing) slots, whose drain value stays 0.
  M.getOrInsertFunction(
      FN_REGISTER_ICACHE_SLOT,
      FunctionType::get(Type::getVoidTy(Ctx), {PtrTy, PtrTy, I32Ty, PtrTy},
                        false));

  // Derive each slot's sentinel from its table's DEFINITION rather than from
  // wrap-loop bookkeeping: a function skipped by the idempotency guard still
  // carries its sentinel initializer from an earlier pass run, and registering
  // it as guarded would let a later drain write 0 into a branchless table.
  // A 0D table initializes to the MissFn pointer itself; a [D]^NumDims table
  // splats it into every element, so element [0][0]...[0] carries it either
  // way. Zero-init tables (3D/4D, timing) yield null.
  auto missFnOf = [](const IcacheSlotInfo &Info) -> Constant * {
    Constant *Elem = Info.GV->getInitializer();
    while (Elem && Elem->getType()->isArrayTy())
      Elem = Elem->getAggregateElement(0u);
    return (Elem && !Elem->isNullValue()) ? Elem : nullptr;
  };

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
  FunctionCallee FnReg = M.getFunction(FN_REGISTER_ICACHE_SLOT);
  for (auto &KV : Fns) {
    IRBuilder<> Builder(Ret);
    Value *Name = Builder.CreateGlobalString(KV.first);
    Constant *MissFnArg = missFnOf(KV.second);
    if (!MissFnArg)
      MissFnArg = ConstantPointerNull::get(PtrTy);
    Builder.CreateCall(FnReg, {Name, Builder.CreateBitCast(KV.second.GV, PtrTy),
                               ConstantInt::get(I32Ty, KV.second.NumDims),
                               MissFnArg});
  }

  if (EnableEJitGlobalCtors && CreatedAutoReg)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Static registry entries for bare-metal / testing fallback (same linker-
  // concatenated .ejit_period model as funcindex above). The `size` (i64) field
  // carries NumDims and the spare name2 ptr carries the sentinel MissFn; the
  // ejit_init walker forwards both to ejitIcacheRegisterSlot.
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
    // name2 (unused by icache entries until now) carries the sentinel MissFn
    // pointer; null for guarded slots.
    Constant *MissFnConst = missFnOf(KV.second);
    if (!MissFnConst)
      MissFnConst = ConstantPointerNull::get(PtrTy);
    Entries.push_back(ConstantStruct::get(
        EntryTy, {ConstantInt::get(I32Ty, EJIT_REG_ICACHE_SLOT),
                  makeStrGV(KV.first), MissFnConst,
                  ConstantExpr::getBitCast(KV.second.GV, PtrTy),
                  ConstantInt::get(I64Ty, KV.second.NumDims)}));
  }
  if (Entries.empty())
    return;
  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  auto *GV = new GlobalVariable(
      M, ArrayTy, /*isConstant=*/true, GlobalValue::PrivateLinkage,
      ConstantArray::get(ArrayTy, Entries), ".ejit.registry.icache");
  GV->setSection(SECT_EJIT_PERIOD);
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
  bool HasBoundPtr = llvm::any_of(EntryFuncs, [](const Function *F) {
    return !getBoundPtrInfos(*F).empty();
  });
  if (HasBoundPtr)
    M.getOrInsertFunction(FN_TASKPOOL_COMPILE_OR_GET_BOUND,
                          FunctionType::get(I32Ty,
                                            {I32Ty, PtrTy, I32Ty, PtrTy, I32Ty,
                                             I32Ty, PtrTy, PtrTy},
                                            false));
  if (HasBoundPtr)
    M.getOrInsertFunction(
        FN_TASKPOOL_COMPILE_OR_GET_BOUND_V,
        FunctionType::get(
            I32Ty, {I32Ty, PtrTy, I32Ty, PtrTy, I32Ty, PtrTy, PtrTy}, false));
  M.getOrInsertFunction(
      FN_TASKPOOL_RELEASE_READ,
      FunctionType::get(Type::getVoidTy(Ctx), {I32Ty}, false));
  // With -ejit-inline-cache the wrapper reads its per-function
  // @__ejit_icache_fn_<name> slot directly (one atomic load + null-check +
  // indirect call) - no ejit_icache_try call, no per-call guards.

  auto isAlreadyWrapped = [](Function &F) -> bool {
    if (!F.getEntryBlock().getName().starts_with("jit_entry"))
      return false;
    // A jit_entry-headed function that references a per-function wrapper global
    // is already wrapped: @__ejit_icache_fn_<name> (the icache probe -- a direct
    // load for 0-dim, or a GEP for numDims>0) or, with the cache off,
    // @__ejit_funcidx_<name> (the funcIndex load in jit_entry). The search spans
    // the whole function, not just the entry block: an ejit_const_dim probe puts
    // its bound guard in jit_entry and the GEP in the guarded successor.
    for (BasicBlock &BB : F)
      for (Instruction &I : BB) {
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
  // Const dims are skipped here: they have no lifecycle, so no global and no
  // registration — the wrapper passes the reserved kEJitConstDimType instead.
  std::map<std::string, GlobalVariable *> DimTypeGlobals;
  for (Function *F : EntryFuncs)
    for (auto &PI : getDimInfo(*F))
      if (!PI.IsConst && !PI.PeriodName.empty())
        DimTypeGlobals.emplace(PI.PeriodName, nullptr);
  if (DimTypeGlobals.size() > kEJitMaxLifecycles) {
    Ctx.emitError("ejit-wrapper-gen: module references " +
                  Twine(DimTypeGlobals.size()) +
                  " distinct lifecycle dimensions but at most " +
                  Twine(kEJitMaxLifecycles) + " are supported (spec §5.1, "
                  "one dimType slot reserved for ejit_const_dim)");
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
      unsigned NumDims = getDimInfo(*F).size();
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
    // The slot registration is emitted AFTER the wrap loop below: it now
    // carries &MissFn for sentinel-form slots, and MissFn is created inside
    // that loop.
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

    auto Dims = getDimInfo(*F);
    SmallVector<BoundPtrInfo, 8> BoundPtrs = getBoundPtrInfos(*F);
    unsigned DimCount = Dims.size();

    if (BoundPtrs.size() > MAX_BOUND_PTR_PARAMS) {
      F->getContext().emitError("ejit-wrapper-gen: function has " +
                                Twine(BoundPtrs.size()) +
                                " ejit_bound_ptr parameters; at most " +
                                Twine(MAX_BOUND_PTR_PARAMS) + " are supported");
      continue;
    }

    if (DimCount > EJIT_ICACHE_MAX_DIMS) {
      F->getContext().emitError("ejit-wrapper-gen: more than "
                                + Twine(EJIT_ICACHE_MAX_DIMS) +
                                " specialization dimensions are not "
                                "supported");
      continue;
    }

    // Validate the metadata: every LIFECYCLE dim must name a non-empty period
    // that has a per-lifecycle dimType global (created above for every distinct
    // name the module references), no two may name the SAME lifecycle (a
    // duplicated dimension — distinct names are guaranteed distinct slots at
    // runtime), and arg indices/types must be in range. The dimType slot itself
    // is resolved at runtime via the global, never baked here.
    //
    // Const dims are exempt from the name checks: they carry no period, and any
    // number of them legitimately share the reserved kEJitConstDimType (the
    // cache compares dim identity positionally, so they stay distinct).
    bool Invalid = false;
    SmallVector<StringRef, 4> SeenNames;
    unsigned ArgCount = F->arg_size();
    for (unsigned I = 0; I < DimCount; ++I) {
      if (!Dims[I].IsConst) {
        auto GIt = DimTypeGlobals.find(Dims[I].PeriodName);
        if (Dims[I].PeriodName.empty() || GIt == DimTypeGlobals.end()) {
          F->getContext().emitError("ejit-wrapper-gen: invalid period name in "
                                    "ejit_period_arr_ind: " +
                                    Dims[I].PeriodName);
          Invalid = true;
          break;
        }
        if (llvm::is_contained(SeenNames, StringRef(Dims[I].PeriodName))) {
          F->getContext().emitError("ejit-wrapper-gen: duplicated lifecycle "
                                    "dimension in ejit_period_arr_ind metadata");
          Invalid = true;
          break;
        }
        SeenNames.push_back(Dims[I].PeriodName);
      } else if (!Dims[I].PeriodName.empty()) {
        F->getContext().emitError("ejit-wrapper-gen: ejit_const_dim metadata "
                                  "must carry an empty period name, got: " +
                                  Dims[I].PeriodName);
        Invalid = true;
        break;
      }

      if (Dims[I].ArgIndex >= ArgCount) {
        F->getContext().emitError("ejit-wrapper-gen: specialization dimension "
                                  "argument index out of range");
        Invalid = true;
        break;
      }
      Value *ArgVal = F->getArg(Dims[I].ArgIndex);
      if (!ArgVal->getType()->isIntegerTy()) {
        F->getContext().emitError("ejit-wrapper-gen: specialization dimension "
                                  "argument must be an integer type");
        Invalid = true;
        break;
      }
    }

    if (Invalid)
      continue;

    for (const BoundPtrInfo &BoundPtr : BoundPtrs) {
      unsigned MatchingDims =
          llvm::count_if(Dims, [&](const DimInfo &I) {
            return I.PeriodName == BoundPtr.PeriodName;
          });
      if (BoundPtr.ArgIndex >= ArgCount ||
          !F->getArg(BoundPtr.ArgIndex)->getType()->isPointerTy() ||
          BoundPtr.PointeeSize == 0 ||
          BoundPtr.PointeeSize > std::numeric_limits<uint32_t>::max() ||
          MatchingDims != 1) {
        F->getContext().emitError(
            "ejit-wrapper-gen: invalid ejit_bound_ptr metadata");
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
    bool UseFixed =
        EJitWrapperFixedDimEntry && DimCount <= 2 && BoundPtrs.empty();

    SmallVector<ReturnInst *, 8> OriginalReturns;
    for (BasicBlock &BB : *F)
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
        OriginalReturns.push_back(RI);
    bool HasMustTailAotExit = false;
    for (BasicBlock &BB : *F) {
      for (Instruction &I : BB) {
        auto *CI = dyn_cast<CallInst>(&I);
        if (CI && CI->isMustTailCall()) {
          HasMustTailAotExit = true;
          break;
        }
      }
      if (HasMustTailAotExit)
        break;
    }

    // Timing callees (shared by hit and slow paths).
    FunctionCallee TraceNow{};
    FunctionCallee TraceWrapper{};
    FunctionCallee TraceFunctionBody{};
    Constant *FunctionName = nullptr;
    if (EJitWrapperTiming || EJitFunctionBodyTiming) {
      TraceNow = M.getOrInsertFunction(FN_TASKPOOL_TRACE_NOW,
                                       FunctionType::get(I64Ty, false));
    }
    if (EJitWrapperTiming) {
      SmallVector<Type *, 8> TraceTys = {I32Ty, I32Ty, PtrTy, I32Ty,
                                         I64Ty, I64Ty, I64Ty, I64Ty};
      TraceWrapper = M.getOrInsertFunction(
          FN_TASKPOOL_TRACE_WRAPPER,
          FunctionType::get(Type::getVoidTy(Ctx), TraceTys, false));
    }
    if (EJitFunctionBodyTiming) {
      TraceFunctionBody = M.getOrInsertFunction(
          FN_FUNCTION_BODY_CYCLES_RECORD,
          FunctionType::get(Type::getVoidTy(Ctx),
                            {PtrTy, I32Ty, I64Ty, I64Ty, I64Ty, I64Ty}, false));
      IRBuilder<> NameBuilder(Ctx);
      FunctionName = NameBuilder.CreateGlobalString(
          F->getName(), ".ejit.function_body_name", 0, &M);
    }

    auto emitFunctionBodyRecord = [&](IRBuilder<> &B, uint32_t Path,
                                      Value *WrapperBegin, Value *BodyBegin,
                                      Value *BodyEnd, Value *WrapperEnd) {
      if (!EJitFunctionBodyTiming)
        return;
      B.CreateCall(TraceFunctionBody,
                   {FunctionName, ConstantInt::get(I32Ty, Path), WrapperBegin,
                    BodyBegin, BodyEnd, WrapperEnd});
    };

    auto instrumentAotBody = [&](BasicBlock *FallbackEntry,
                                 Value *WrapperBegin) {
      if (!EJitFunctionBodyTiming)
        return;

      SmallPtrSet<BasicBlock *, 16> Reachable;
      SmallVector<BasicBlock *, 16> Worklist{FallbackEntry};
      while (!Worklist.empty()) {
        BasicBlock *BB = Worklist.pop_back_val();
        if (!Reachable.insert(BB).second)
          continue;
        llvm::append_range(Worklist, successors(BB));
      }

      SmallVector<ReturnInst *, 8> TimedReturns;
      for (ReturnInst *RI : OriginalReturns) {
        if (!Reachable.contains(RI->getParent()))
          continue;
        // A musttail call must remain immediately adjacent to its return. Keep
        // such an uncommon AOT exit valid rather than changing its ABI/stack
        // behaviour for a diagnostic option.
        if (auto *CI = dyn_cast_or_null<CallInst>(RI->getPrevNode());
            CI && CI->isMustTailCall())
          continue;
        TimedReturns.push_back(RI);
      }
      if (TimedReturns.empty())
        return;

      IRBuilder<> StartBuilder(&*FallbackEntry->getFirstInsertionPt());
      Value *Begin =
          StartBuilder.CreateCall(TraceNow, {}, "ejit_aot_body_begin");
      for (ReturnInst *RI : TimedReturns) {
        IRBuilder<> EndBuilder(RI);
        Value *BodyEnd =
            EndBuilder.CreateCall(TraceNow, {}, "ejit_aot_body_end");
        Value *WrapperEnd =
            EndBuilder.CreateCall(TraceNow, {}, "ejit_wrapper_end");
        emitFunctionBodyRecord(EndBuilder, kEJitFunctionBodyPathAOT,
                               WrapperBegin, Begin, BodyEnd, WrapperEnd);
      }
    };

    // emitSlowPath: emit the funcIndex guard (EntryBB) + compile_or_get
    // (CallBB) + dispatch (DispatchBB) into Fn, using Fn's args. FallbackBB
    // already holds the spliced original body (with its own ret). Shared by
    // icache-off (in F) and icache-on (in MissFn).
    auto emitSlowPath = [&](Function &Fn, BasicBlock *EntryBB,
                            BasicBlock *CallBB, BasicBlock *DispatchBB,
                            BasicBlock *FallbackBB,
                            Value *WrapperBegin) -> Value * {
      IRBuilder<> B(EntryBB);
      // Allocas live in the entry block so they dominate the call/dispatch
      // blocks (and match the original layout: allocas precede the funcidx
      // guard).
      Value *DimsAlloca = UseFixed
                              ? nullptr
                              : B.CreateAlloca(ArrayType::get(DimPairTy, 4),
                                               nullptr, "ejit_dims");
      Value *OutFnAlloca = B.CreateAlloca(PtrTy, nullptr, "ejit_out_fn");
      Value *OutBucketAlloca =
          B.CreateAlloca(I32Ty, nullptr, "ejit_out_bucket");
      Value *OutFnArg = B.CreatePointerCast(OutFnAlloca, PtrTy);
      Value *OutBucketArg = B.CreatePointerCast(OutBucketAlloca, PtrTy);
      ArrayType *BoundDescArrayTy = nullptr;
      Value *BoundDescAlloca = nullptr;
      if (BoundPtrs.size() > 1) {
        auto *BoundDescTy = StructType::get(PtrTy, I32Ty, I32Ty);
        BoundDescArrayTy = ArrayType::get(BoundDescTy, BoundPtrs.size());
        BoundDescAlloca =
            B.CreateAlloca(BoundDescArrayTy, nullptr, "ejit_bound_ptrs");
      }
      if (EJitFunctionBodyTiming && !WrapperBegin)
        WrapperBegin = B.CreateCall(TraceNow, {}, "ejit_wrapper_begin");
      Value *FuncIdx = B.CreateLoad(I32Ty, FuncIndexGlobals[F->getName().str()],
                                    "ejit_funcidx");
      Value *IdxValid = B.CreateICmpNE(
          FuncIdx, ConstantInt::get(I32Ty, kEJitInvalidFuncIndex),
          "ejit_idx_ok");
      B.CreateCondBr(IdxValid, CallBB, FallbackBB);

      B.SetInsertPoint(CallBB);
      auto emitDimTypeVal = [&](unsigned I) -> Value * {
        if (Dims[I].IsConst)
          return ConstantInt::get(I32Ty, kEJitConstDimType);
        return B.CreateLoad(I32Ty, DimTypeGlobals[Dims[I].PeriodName],
                            "ejit_dimtype");
      };
      auto emitInstanceVal = [&](unsigned I) -> Value * {
        Value *ArgVal = Fn.getArg(Dims[I].ArgIndex);
        unsigned BW = cast<IntegerType>(ArgVal->getType())->getBitWidth();
        if (BW > 32)
          return B.CreateTrunc(ArgVal, I32Ty);
        if (BW < 32)
          return B.CreateZExt(ArgVal, I32Ty);
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
          Value *Idxs[] = {ConstantInt::get(I32Ty, 0),
                           ConstantInt::get(I32Ty, I)};
          Value *PairPtr = B.CreateInBoundsGEP(ArrayType::get(DimPairTy, 4),
                                               DimsAlloca, Idxs);
          B.CreateStore(
              emitDimTypeVal(I),
              B.CreateStructGEP(DimPairTy, PairPtr, 0, "dim_type_ptr"));
          B.CreateStore(
              emitInstanceVal(I),
              B.CreateStructGEP(DimPairTy, PairPtr, 1, "instance_ptr"));
        }
        Value *DimsPtr = DimCount > 0 ? B.CreatePointerCast(DimsAlloca, PtrTy)
                                      : ConstantPointerNull::get(PtrTy);
        if (BoundPtrs.size() == 1) {
          const BoundPtrInfo &BoundPtr = BoundPtrs.front();
          Value *RawPtr = Fn.getArg(BoundPtr.ArgIndex);
          if (RawPtr->getType() != PtrTy)
            RawPtr = B.CreatePointerCast(RawPtr, PtrTy);
          Status = B.CreateCall(M.getFunction(FN_TASKPOOL_COMPILE_OR_GET_BOUND),
                                {FuncIdx, DimsPtr,
                                 ConstantInt::get(I32Ty, DimCount), RawPtr,
                                 ConstantInt::get(I32Ty, BoundPtr.PointeeSize),
                                 ConstantInt::get(I32Ty, BoundPtr.ArgIndex),
                                 OutFnArg, OutBucketArg});
        } else if (BoundPtrs.size() > 1) {
          auto *BoundDescTy =
              cast<StructType>(BoundDescArrayTy->getElementType());
          for (unsigned I = 0; I < BoundPtrs.size(); ++I) {
            const BoundPtrInfo &BoundPtr = BoundPtrs[I];
            Value *Idxs[] = {ConstantInt::get(I32Ty, 0),
                             ConstantInt::get(I32Ty, I)};
            Value *DescPtr =
                B.CreateInBoundsGEP(BoundDescArrayTy, BoundDescAlloca, Idxs);
            Value *RawPtr = Fn.getArg(BoundPtr.ArgIndex);
            if (RawPtr->getType() != PtrTy)
              RawPtr = B.CreatePointerCast(RawPtr, PtrTy);
            B.CreateStore(RawPtr, B.CreateStructGEP(BoundDescTy, DescPtr, 0));
            B.CreateStore(ConstantInt::get(I32Ty, BoundPtr.PointeeSize),
                          B.CreateStructGEP(BoundDescTy, DescPtr, 1));
            B.CreateStore(ConstantInt::get(I32Ty, BoundPtr.ArgIndex),
                          B.CreateStructGEP(BoundDescTy, DescPtr, 2));
          }
          Status =
              B.CreateCall(M.getFunction(FN_TASKPOOL_COMPILE_OR_GET_BOUND_V),
                           {FuncIdx, DimsPtr, ConstantInt::get(I32Ty, DimCount),
                            B.CreatePointerCast(BoundDescAlloca, PtrTy),
                            ConstantInt::get(I32Ty, BoundPtrs.size()), OutFnArg,
                            OutBucketArg});
        } else {
          Status = B.CreateCall(M.getFunction(FN_TASKPOOL_COMPILE_OR_GET),
                                {FuncIdx, DimsPtr,
                                 ConstantInt::get(I32Ty, DimCount), OutFnArg,
                                 OutBucketArg});
        }
      }
      if (EJitWrapperTiming)
        TAfterLookup = B.CreateCall(TraceNow, {}, "ejit_t_after_lookup");
      Value *OutFn = B.CreateLoad(PtrTy, OutFnAlloca, "ejit_fn");
      Value *HitStatus = B.CreateICmpEQ(Status, ConstantInt::get(I32Ty, 0));
      B.CreateCondBr(B.CreateAnd(HitStatus, B.CreateIsNotNull(OutFn)),
                     DispatchBB, FallbackBB);

      B.SetInsertPoint(DispatchBB);
      SmallVector<Value *, 8> Args;
      for (unsigned I = 0; I < F->arg_size(); ++I)
        Args.push_back(Fn.getArg(I));
      Value *BodyBegin = nullptr;
      if (EJitFunctionBodyTiming)
        BodyBegin = B.CreateCall(TraceNow, {}, "ejit_jit_body_begin");
      auto releaseAndTrace = [&]() {
        Value *BodyEnd = nullptr;
        if (EJitFunctionBodyTiming)
          BodyEnd = B.CreateCall(TraceNow, {}, "ejit_jit_body_end");
        Value *TAfterFn = nullptr;
        if (EJitWrapperTiming) {
          TAfterFn =
              BodyEnd ? BodyEnd : B.CreateCall(TraceNow, {}, "ejit_t_after_fn");
          Value *Bucket = B.CreateLoad(I32Ty, OutBucketAlloca);
          B.CreateCall(M.getFunction(FN_TASKPOOL_RELEASE_READ), {Bucket});
          Value *TAfterRelease =
              B.CreateCall(TraceNow, {}, "ejit_t_after_release");
          B.CreateCall(TraceWrapper,
                       {FuncIdx, Status, OutFn, Bucket, TBeforeLookup,
                        TAfterLookup, TAfterFn, TAfterRelease});
          if (EJitFunctionBodyTiming)
            emitFunctionBodyRecord(B, kEJitFunctionBodyPathJIT, WrapperBegin,
                                   BodyBegin, BodyEnd, TAfterRelease);
        } else {
          Value *Bucket = B.CreateLoad(I32Ty, OutBucketAlloca);
          B.CreateCall(M.getFunction(FN_TASKPOOL_RELEASE_READ), {Bucket});
          if (EJitFunctionBodyTiming) {
            Value *WrapperEnd = B.CreateCall(TraceNow, {}, "ejit_wrapper_end");
            emitFunctionBodyRecord(B, kEJitFunctionBodyPathJIT, WrapperBegin,
                                   BodyBegin, BodyEnd, WrapperEnd);
          }
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
      return WrapperBegin;
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
      // Moving a musttail AOT exit into a function with one extra fixed
      // parameter would invalidate its caller/callee ABI match. Such entries
      // keep an ABI-identical MissFn; their slow JIT path starts timing locally
      // and their AOT fallback is deliberately left uninstrumented.
      const bool ThreadWrapperBegin =
          EJitFunctionBodyTiming && !HasMustTailAotExit;
      FunctionType *MissFnTy = F->getFunctionType();
      if (ThreadWrapperBegin) {
        SmallVector<Type *, 8> MissParamTys(F->getFunctionType()->params());
        MissParamTys.push_back(I64Ty);
        MissFnTy =
            FunctionType::get(F->getReturnType(), MissParamTys, F->isVarArg());
      }
      Function *MissFn = Function::Create(
          MissFnTy, GlobalValue::InternalLinkage, F->getName() + "_miss", &M);
      // Function::Create only inherits module-default uwtable/frame-pointer
      // from the context, NOT F's target-cpu/target-features/tune-cpu. Without
      // those, TTI::areInlineCompatible(Caller=MissFn, Callee=helper) fails the
      // subtarget feature-bit superset check, so InlinerPass rejects every
      // cost-based inlining into MissFn with "conflicting attributes"
      // (always_inline still inlines via the InlineCost.cpp AlwaysInline
      // short-circuit). The AOT fallback body then loses ~all helper inlining
      // vs the original function. Copy F's attributes so MissFn matches F's
      // subtarget; re-affirm NoInline.
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
      auto *MissEntry =
          BasicBlock::Create(Ctx, "miss_entry", MissFn, MissFallback);
      auto *MissCall = BasicBlock::Create(Ctx, "miss_call", MissFn);
      auto *MissDispatch = BasicBlock::Create(Ctx, "miss_dispatch", MissFn);
      if (!MissFallback->getTerminator()) {
        IRBuilder<> B(MissFallback);
        if (F->getReturnType()->isVoidTy())
          B.CreateRetVoid();
        else
          B.CreateRet(PoisonValue::get(F->getReturnType()));
      }
      Value *MissWrapperBegin =
          ThreadWrapperBegin ? MissFn->getArg(F->arg_size()) : nullptr;
      emitSlowPath(*MissFn, MissEntry, MissCall, MissDispatch, MissFallback,
                   MissWrapperBegin);
      if (!HasMustTailAotExit)
        instrumentAotBody(MissFallback, MissWrapperBegin);

      // Wrapper (F): frame-less probe + tail calls. Sentinel-form tables
      // (NumDims <= 2, timing off) are defined pre-filled with &MissFn, so
      // their wrapper is ONE block - probe + musttail BLR, no guard; the
      // dispatch/miss blocks below exist only for the guarded shapes.
      auto *JitEntry = BasicBlock::Create(Ctx, "jit_entry", F);
      BasicBlock *JitIcacheDispatch = nullptr;
      BasicBlock *JitMiss = nullptr;
      IRBuilder<> B(JitEntry);
      Value *WrapperBegin = nullptr;
      if (EJitFunctionBodyTiming)
        WrapperBegin = B.CreateCall(TraceNow, {}, "ejit_wrapper_begin");
      Value *TBeforeIcache = nullptr, *FuncIdxForTiming = nullptr;
      if (EJitWrapperTiming) {
        TBeforeIcache =
            WrapperBegin ? WrapperBegin
                         : B.CreateCall(TraceNow, {}, "ejit_t_before_icache");
        FuncIdxForTiming = B.CreateLoad(
            I32Ty, FuncIndexGlobals[F->getName().str()], "ejit_funcidx_t");
      }
      GlobalVariable *IcacheSlot = IcacheIt->second.GV;
      unsigned NumDims = IcacheIt->second.NumDims;
      // The sentinel initializer is a [D]^NumDims splat of &MissFn: at D=16
      // that is 1/16/256 relocations for 0/1/2 dims, but 4096 and 65536 for
      // 3 and 4 - so 3D/4D keep the zero-init table and the null guard.
      // Function-body timing is also excluded: it threads an extra wrapper-
      // begin argument into MissFn (changing its signature away from F's) and
      // emits trace calls around the specialization call, neither of which
      // survives a single-block musttail BLR. A const dim is excluded too: its
      // range guard below needs a miss block to branch to, so the wrapper is
      // never one block anyway, and the null check it keeps costs one cbz.
      const bool HasConstDim =
          llvm::any_of(Dims, [](const DimInfo &I) { return I.IsConst; });
      const bool Sentinelize = !EJitWrapperTiming && !EJitFunctionBodyTiming &&
                               NumDims <= 2 && !HasConstDim;
      if (!Sentinelize) {
        JitIcacheDispatch = BasicBlock::Create(Ctx, "jit_icache_dispatch", F);
        JitMiss = BasicBlock::Create(Ctx, "jit_miss", F);
      }
      Value *SlotPtr = IcacheSlot;
      if (NumDims > 0) {
        SmallVector<Value *, 5> Indices;
        Indices.push_back(ConstantInt::get(I32Ty, 0));
        // An ejit_dim value is contractually a dense period-array index in
        // [0, D), so its axis is indexed unguarded. A const dim carries no such
        // contract — nothing bounds the value a caller passes — so it gets a
        // dynamic guard; without it an out-of-range value would index past the
        // [D]^NumDims table and indirect-call whatever it read. The runtime's
        // icacheFill declines the same values, so both sides agree on which
        // identities own a cell.
        Value *InBounds = nullptr;
        for (unsigned I = 0; I < NumDims; ++I) {
          Value *ArgVal = F->getArg(Dims[I].ArgIndex);
          unsigned BW = cast<IntegerType>(ArgVal->getType())->getBitWidth();
          Value *Idx = ArgVal;
          if (BW > 32)
            Idx = B.CreateTrunc(ArgVal, I32Ty);
          else if (BW < 32)
            Idx = B.CreateZExt(ArgVal, I32Ty);
          Indices.push_back(Idx);
          if (Dims[I].IsConst) {
            Value *Lt = B.CreateICmpULT(
                Idx, ConstantInt::get(I32Ty, EJIT_ICACHE_DIM_SIZE),
                "ejit_ic_inrange");
            InBounds = InBounds ? B.CreateAnd(InBounds, Lt) : Lt;
          }
        }
        if (InBounds) {
          auto *JitIcacheProbe =
              BasicBlock::Create(Ctx, "jit_icache_probe", F, JitIcacheDispatch);
          InBounds = B.CreateIntrinsic(Intrinsic::expect, {InBounds->getType()},
                                       {InBounds, ConstantInt::getTrue(Ctx)});
          B.CreateCondBr(InBounds, JitIcacheProbe, JitMiss);
          B.SetInsertPoint(JitIcacheProbe);
        }
        SlotPtr = B.CreateInBoundsGEP(IcacheSlot->getValueType(), IcacheSlot,
                                      Indices, "ejit_ic_slot");
      }
      LoadInst *ICSlotLoad = B.CreateLoad(PtrTy, SlotPtr, "ejit_ic_fn");
      ICSlotLoad->setAlignment(Align(8));
      // Monotonic, nothing stronger: the cell is shared, so a peer's drain can
      // store the drain value (the &MissFn sentinel for sentinel-form tables,
      // 0 for guarded ones) concurrently and the atomic makes that a defined
      // race (old pointer or drain value) instead of UB. The value is
      // self-contained and the call below carries a data dependency on it, so
      // acquire would buy nothing and cost an LDAR per hit. Lowers to the same
      // LDR on AArch64.
      ICSlotLoad->setAtomic(AtomicOrdering::Monotonic);
      Value *TAfterIcache = nullptr;
      if (EJitWrapperTiming)
        TAfterIcache = B.CreateCall(TraceNow, {}, "ejit_t_after_icache");
      if (Sentinelize) {
        // Definition-time sentinel: every cell of the table is DEFINED as
        // &MissFn, so the loaded value is always a callable pointer - the
        // specialization on a hit, MissFn itself before the first fill, after
        // a drain, or before registration has run (whose funcidx guard then
        // falls to the AOT body, exactly like the guarded form). The wrapper
        // is the probe plus one musttail BLR - no guard, no branch. The
        // registration derives the sentinel from this initializer, so no
        // per-slot bookkeeping is needed here.
        IcacheSlot->setInitializer(buildIcacheSentinelInit(
            IcacheSlot->getValueType(), MissFn, NumDims));
        SmallVector<Value *, 8> ICArgs;
        for (auto &A : F->args())
          ICArgs.push_back(&A);
        CallInst *CI = B.CreateCall(F->getFunctionType(), ICSlotLoad, ICArgs);
        CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
        if (F->getReturnType()->isVoidTy())
          B.CreateRetVoid();
        else
          B.CreateRet(CI);
      } else {
        Value *IHit = B.CreateIsNotNull(ICSlotLoad, "ejit_icache_hit");
        IHit = B.CreateIntrinsic(Intrinsic::expect, {IHit->getType()},
                                 {IHit, ConstantInt::getTrue(Ctx)});
        B.CreateCondBr(IHit, JitIcacheDispatch, JitMiss);

        // Hit: tail-call the cached specialization directly (no release_read).
        // With -ejit-wrapper-timing the wrapper is NOT frame-less (JitEntry emits
        // bl @ejit_taskpool_trace_now), so the hit path cannot be a musttail tail
        // call: emitHitTiming() inserts calls BETWEEN the call and the ret, which
        // would violate the musttail rule ("musttail call must precede a ret")
        // and yield broken IR -> codegen silently drops the function -> boot
        // translation fault. Use a plain (framed) call + trace + ret instead; the
        // frame-less musttail fast path is reserved for the no-timing case.
        B.SetInsertPoint(JitIcacheDispatch);
        SmallVector<Value *, 8> ICArgs;
        for (auto &A : F->args())
          ICArgs.push_back(&A);
        Value *BodyBegin = nullptr;
        if (EJitFunctionBodyTiming)
          BodyBegin = B.CreateCall(TraceNow, {}, "ejit_jit_body_begin");
        auto emitHitTiming = [&]() {
          Value *BodyEnd = nullptr;
          if (EJitFunctionBodyTiming)
            BodyEnd = B.CreateCall(TraceNow, {}, "ejit_jit_body_end");
          Value *WrapperEnd = nullptr;
          if (EJitFunctionBodyTiming)
            WrapperEnd = B.CreateCall(TraceNow, {}, "ejit_wrapper_end");
          if (EJitWrapperTiming) {
            Value *TAfterFn =
                BodyEnd ? BodyEnd : B.CreateCall(TraceNow, {}, "ejit_t_after_fn");
            B.CreateCall(TraceWrapper,
                         {FuncIdxForTiming,
                          ConstantInt::get(I32Ty, kEJitIcacheHitTimingStatus),
                          ICSlotLoad, ConstantInt::get(I32Ty, 0), TBeforeIcache,
                          TAfterIcache, TAfterFn, TAfterFn});
          }
          if (EJitFunctionBodyTiming) {
            emitFunctionBodyRecord(B, kEJitFunctionBodyPathJIT, WrapperBegin,
                                   BodyBegin, BodyEnd, WrapperEnd);
          }
        };
        if (F->getReturnType()->isVoidTy()) {
          CallInst *CI = B.CreateCall(F->getFunctionType(), ICSlotLoad, ICArgs);
          if (!EJitWrapperTiming && !EJitFunctionBodyTiming)
            CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
          emitHitTiming();
          B.CreateRetVoid();
        } else {
          CallInst *CI = B.CreateCall(F->getFunctionType(), ICSlotLoad, ICArgs);
          if (!EJitWrapperTiming && !EJitFunctionBodyTiming)
            CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
          emitHitTiming();
          B.CreateRet(CI);
        }

        // Miss: tail-call MissFn (which does compile_or_get + dispatch/fallback).
        B.SetInsertPoint(JitMiss);
        SmallVector<Value *, 8> MissArgs;
        for (auto &A : F->args())
          MissArgs.push_back(&A);
        if (ThreadWrapperBegin)
          MissArgs.push_back(WrapperBegin);
        if (F->getReturnType()->isVoidTy()) {
          CallInst *CI =
              B.CreateCall(MissFn->getFunctionType(), MissFn, MissArgs);
          if (!EJitFunctionBodyTiming)
            CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
          B.CreateRetVoid();
        } else {
          CallInst *CI =
              B.CreateCall(MissFn->getFunctionType(), MissFn, MissArgs);
          if (!EJitFunctionBodyTiming)
            CI->setTailCallKind(CallInst::TailCallKind::TCK_MustTail);
          B.CreateRet(CI);
      }
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
        if (F->getReturnType()->isVoidTy())
          B.CreateRetVoid();
        else
          B.CreateRet(PoisonValue::get(F->getReturnType()));
      }
      Value *WrapperBegin = emitSlowPath(*F, JitEntry, JitCall, JitDispatch,
                                         JitFallback, nullptr);
      instrumentAotBody(JitFallback, WrapperBegin);
    }

    Changed = true;
  }

  // Icache-slot registration, emitted after wrapping so sentinel-form slots
  // can carry their MissFn pointer (no-ops when the map is empty).
  emitIcacheSlotRegistration(M, IcacheFnGlobals);

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
