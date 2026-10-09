//===-- EJitRegisterPeriod.cpp - EmbeddedJIT Period Registration ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  PASS2: Scan the module for ejit_period and ejit_period_arr global
//  variables and generate runtime registration calls in ejit_auto_register.
//  The existing small-table opt-in also inventories exact .mc_shared object
//  definitions into the existing static registry, without new callbacks.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"

using namespace llvm;
using namespace llvm::ejit;

extern cl::opt<bool> EnableEJitGlobalCtors;
extern cl::opt<bool> EnableEJitSmallTableHooks;

namespace {
struct SharedObjectEntry {
  GlobalVariable *object;
  uint64_t bytes;
  ejit_reg_type_t kind;
};

bool hasSharedObjectSection(const GlobalVariable &GV) {
  return GV.getSection() == ".mc_shared" ||
         GV.getSection().starts_with(".mc_shared.");
}

// Look at real constant records rather than global names: multiple TUs use
// private arrays with the same stem, and the pass may run again after linking.
// No constructor or public arbitrary-address registration path is involved.
bool alreadyInventoried(const Module &M, const SharedObjectEntry &Object) {
  for (const GlobalVariable &Table : M.globals()) {
    if (Table.getSection() != SECT_EJIT_PERIOD || !Table.hasInitializer())
      continue;
    const auto *Rows = dyn_cast<ConstantArray>(Table.getInitializer());
    if (!Rows)
      continue;
    for (const Use &RowUse : Rows->operands()) {
      const auto *Row = dyn_cast<ConstantStruct>(RowUse.get());
      if (!Row || Row->getNumOperands() != 5)
        continue;
      const auto *Kind = dyn_cast<ConstantInt>(Row->getOperand(0));
      const auto *Bytes = dyn_cast<ConstantInt>(Row->getOperand(4));
      if (Kind && Bytes && Kind->getBitWidth() == 32 &&
          Bytes->getBitWidth() == 64 &&
          Kind->getZExtValue() == Object.kind &&
          Bytes->getZExtValue() == Object.bytes &&
          Row->getOperand(3)->stripPointerCasts() == Object.object)
        return true;
    }
  }
  return false;
}
} // namespace

static void
generateRegistryTablePeriod(
    Module &M,
    const SmallVectorImpl<std::tuple<GlobalVariable *, std::string,
                                     std::string, uint32_t>> &PeriodArrays,
    const SmallVectorImpl<std::pair<GlobalVariable *, std::string>> &StaticVars,
    const SmallVectorImpl<SharedObjectEntry> &SharedObjects);

#define DEBUG_TYPE "ejit-register-period"

PreservedAnalyses
EJitRegisterPeriodPass::run(Module &M, ModuleAnalysisManager &AM) {
  // Collect period variables
  SmallVector<std::tuple<GlobalVariable *, std::string, std::string, uint32_t>, 8>
      PeriodArrays; // {GV, periodName, varName, arraySize}
  SmallVector<std::pair<GlobalVariable *, std::string>, 8>
      StaticVars; // {GV, varName}
  SmallVector<SharedObjectEntry, 8> SharedObjects;

  for (GlobalVariable &GV : M.globals()) {
    // The controller's source-state/stage/flags need not be referenced by an
    // ejit_entry closure or carry EJIT metadata. Inventory all eligible real
    // definitions in this TU, not just the period/JIT subset. The product's
    // existing .mc_shared coherent mapping contract remains required; these
    // records establish object identity, extent and declared access only.
    if (EnableEJitSmallTableHooks && hasSharedObjectSection(GV) &&
        !GV.isDeclaration() && !GV.hasAvailableExternallyLinkage() &&
        !GV.isThreadLocal() && GV.getAddressSpace() == 0 &&
        GV.getValueType()->isSized()) {
      const TypeSize Bytes = M.getDataLayout().getTypeAllocSize(GV.getValueType());
      if (!Bytes.isScalable() && Bytes.getFixedValue()) {
        SharedObjectEntry Object{&GV, Bytes.getFixedValue(),
                                 GV.isConstant() ? EJIT_REG_SHARED_OBJECT_RO
                                                 : EJIT_REG_SHARED_OBJECT_RW};
        if (!alreadyInventoried(M, Object))
          SharedObjects.push_back(Object);
      }
    }
    MDNode *MD = GV.getMetadata(MD_EJIT_METADATA);
    if (!MD)
      continue;

    std::string varName = GV.getName().str();

    if (hasMDStringEntry(MD, TAG_EJIT_PERIOD_ARR)) {
      StringRef periodName = getMDStringValue(MD, TAG_EJIT_PERIOD_ARR);
      uint32_t size = getMDIntValue(MD, TAG_EJIT_PERIOD_ARR);
      PeriodArrays.push_back({&GV, periodName.str(), varName, size});
    }

    if (hasMDStringEntry(MD, TAG_EJIT_PERIOD)) {
      StaticVars.push_back({&GV, varName});
    }
  }

  if (PeriodArrays.empty() && StaticVars.empty() && SharedObjects.empty()) {
    LLVM_DEBUG(dbgs() << "ejit-register-period: no period vars\n");
    return PreservedAnalyses::all();
  }
  LLVM_DEBUG(dbgs() << "ejit-register-period: " << PeriodArrays.size()
                    << " arrays, " << StaticVars.size() << " static vars, "
                    << SharedObjects.size() << " shared objects\n");

  // An inventory-only TU needs no registration constructor or new callback.
  // The bridge reads these immutable records before ejit_init on both cores.
  if (PeriodArrays.empty() && StaticVars.empty()) {
    generateRegistryTablePeriod(M, PeriodArrays, StaticVars, SharedObjects);
    return PreservedAnalyses::none();
  }

  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);

  // Declare runtime functions
  M.getOrInsertFunction(FN_REGISTER_PERIOD_ARRAY,
      FunctionType::get(Type::getVoidTy(Ctx),
                        {PtrTy, PtrTy, PtrTy, Type::getInt64Ty(Ctx)}, false));
  M.getOrInsertFunction(FN_REGISTER_STATIC_VAR,
      FunctionType::get(Type::getVoidTy(Ctx), {PtrTy, PtrTy}, false));

  // Find or create ejit_auto_register
  Function *AutoReg = M.getFunction(FN_AUTO_REGISTER);
  if (!AutoReg) {
    auto *AutoRegTy = FunctionType::get(Type::getVoidTy(Ctx), false);
    AutoReg = Function::Create(AutoRegTy, GlobalValue::InternalLinkage,
                               FN_AUTO_REGISTER, &M);
    BasicBlock::Create(Ctx, "entry", AutoReg);
    ReturnInst::Create(Ctx, &AutoReg->getEntryBlock());
  }

  // Insert calls before return
  BasicBlock *EntryBB = &AutoReg->getEntryBlock();
  Instruction *Ret = EntryBB->getTerminator();
  FunctionCallee FnRegArr = M.getFunction(FN_REGISTER_PERIOD_ARRAY);
  FunctionCallee FnRegSV = M.getFunction(FN_REGISTER_STATIC_VAR);

  for (auto &[GV, PeriodName, VarName, Size] : PeriodArrays) {
    IRBuilder<> Builder(Ret);
    Value *PN = Builder.CreateGlobalString(PeriodName);
    Value *VN = Builder.CreateGlobalString(VarName);
    Value *BA = Builder.CreateBitCast(GV, PtrTy);
    Builder.CreateCall(FnRegArr, {PN, VN, BA, ConstantInt::get(Type::getInt64Ty(Ctx), Size)});
  }

  for (auto &[GV, VarName] : StaticVars) {
    IRBuilder<> Builder(Ret);
    Value *VN = Builder.CreateGlobalString(VarName);
    Value *VA = Builder.CreateBitCast(GV, PtrTy);
    Builder.CreateCall(FnRegSV, {VN, VA});
  }

  if (EnableEJitGlobalCtors)
    appendToGlobalCtors(M, AutoReg, EJIT_CTOR_PRIORITY);

  // Always build the static registry table for bare-metal / testing fallback.
  generateRegistryTablePeriod(M, PeriodArrays, StaticVars, SharedObjects);

  return PreservedAnalyses::none();
}

/// Emit this translation unit's period/static registry entries as a private
/// array in the ".ejit_period" section. The linker concatenates these across
/// all TUs; a linker script brackets the section so ejit_init() can walk the
/// [__start_ejit_period, __stop_ejit_period) range on bare-metal where global
/// constructors are unavailable.
static void
generateRegistryTablePeriod(
    Module &M,
    const SmallVectorImpl<std::tuple<GlobalVariable *, std::string,
                                     std::string, uint32_t>> &PeriodArrays,
    const SmallVectorImpl<std::pair<GlobalVariable *, std::string>> &StaticVars,
    const SmallVectorImpl<SharedObjectEntry> &SharedObjects) {
  LLVMContext &Ctx = M.getContext();
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I64Ty = Type::getInt64Ty(Ctx);

  StructType *EntryTy = StructType::get(
      Ctx, {I32Ty, PtrTy, PtrTy, PtrTy, I64Ty}, /*isPacked=*/false);

  SmallVector<Constant *, 16> Entries;

  // Helper: create a private global string constant.
  auto makeStrGV = [&](const std::string &S) -> Constant * {
    Constant *Str = ConstantDataArray::getString(Ctx, S, true);
    auto *GV = new GlobalVariable(M, Str->getType(), true,
        GlobalValue::PrivateLinkage, Str, ".ejit.str.");
    return ConstantExpr::getBitCast(GV, PtrTy);
  };

  // Period array entries
  for (auto &[GV, PeriodName, VarName, Size] : PeriodArrays) {
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, EJIT_REG_PERIOD_ARRAY),       // EJIT_REG_PERIOD_ARRAY
        makeStrGV(PeriodName),
        makeStrGV(VarName),
        ConstantExpr::getBitCast(GV, PtrTy),         // base address
        ConstantInt::get(I64Ty, Size),
    }));
  }

  // Static var entries
  for (auto &[GV, VarName] : StaticVars) {
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, EJIT_REG_STATIC_VAR),           // EJIT_REG_STATIC_VAR
        makeStrGV(VarName),
        ConstantPointerNull::get(PtrTy),
        ConstantExpr::getBitCast(GV, PtrTy),
        ConstantInt::get(I64Ty, 0),
    }));
  }

  // Additive tags, original 40-byte record on the 64-bit targets. In
  // particular size is the true target allocation extent, not array member
  // count; aliases, TLS, externs, non-default address spaces and unsized or
  // scalable objects were excluded while collecting real GlobalVariables.
  for (const SharedObjectEntry &Object : SharedObjects) {
    Entries.push_back(ConstantStruct::get(EntryTy, {
        ConstantInt::get(I32Ty, Object.kind),
        makeStrGV(Object.object->getName().str()),
        ConstantPointerNull::get(PtrTy),
        ConstantExpr::getBitCast(Object.object, PtrTy),
        ConstantInt::get(I64Ty, Object.bytes),
    }));
  }

  // No sentinel entry: the runtime iterates the linker-provided
  // [__start_ejit_period, __stop_ejit_period) range over the dedicated
  // section, so each translation unit contributes only its own entries.
  if (Entries.empty())
    return;

  ArrayType *ArrayTy = ArrayType::get(EntryTy, Entries.size());
  Constant *ArrayInit = ConstantArray::get(ArrayTy, Entries);

  // Private linkage + a dedicated section. Every TU emits its own *local*
  // array into ".ejit_period"; the linker concatenates them across TUs. The
  // leading-dot name is the conventional ELF spelling but is NOT a valid C
  // identifier, so the linker does NOT auto-synthesize __start_/__stop_ — a
  // linker script must bracket the section (see ejit_registry.ld). A fixed
  // *external* symbol here would instead produce "duplicate symbol" link
  // errors as soon as more than one TU declares period globals.
  // llvm.used keeps the array alive under --gc-sections.
  auto *GV = new GlobalVariable(M, ArrayTy, /*isConstant=*/true,
                                GlobalValue::PrivateLinkage, ArrayInit,
                                ".ejit.registry.period");
  GV->setSection(SECT_EJIT_PERIOD);
  GV->setAlignment(M.getDataLayout().getABITypeAlign(EntryTy));
  appendToUsed(M, {GV});
}
