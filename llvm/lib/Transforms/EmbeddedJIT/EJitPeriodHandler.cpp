//===-- EJitPeriodHandler.cpp - EmbeddedJIT Lifecycle Handler -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  PASS4: Insert ejit_deactivate at function entry and ejit_activate before
//  every return in ejit_period_lc functions. Activation is keyed by
//  lifecycle/period name + instance index only (no array-pointer dimension).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/EmbeddedJIT/EJitPasses.h"
#include "llvm/ADT/SmallVector.h"
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

using namespace llvm;
using namespace llvm::ejit;

#define DEBUG_TYPE "ejit-guard-handler"

namespace {

struct LifecycleInfo {
  std::string PeriodName;
  unsigned ArgIndex;
  GlobalVariable *ArrayGV;
  bool HasArrInd = false; // true if matched by ejit_dim
};

static SmallVector<LifecycleInfo, 4>
collectLifecycleInfo(Module &M, Function &F) {
  SmallVector<LifecycleInfo, 4> Result;
  MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
  if (!MD)
    return Result;

  // Collect period names from ejit_period_lc entries
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 2)
      continue;
    if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0))) {
      if (Tag->getString() == TAG_EJIT_GUARD) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        if (PN)
          Result.push_back({PN->getString().str(), 0, nullptr});
      }
    }
  }

  // Find arg index from ejit_dim metadata entries
  for (const MDOperand &Op : MD->operands()) {
    auto *Sub = dyn_cast<MDNode>(Op.get());
    if (!Sub || Sub->getNumOperands() < 3)
      continue;
    if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0))) {
      if (Tag->getString() == TAG_EJIT_DIM) {
        auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
        if (auto *IdxC = dyn_cast<ConstantAsMetadata>(Sub->getOperand(2)))
          if (auto *CI = dyn_cast<ConstantInt>(IdxC->getValue()))
            for (auto &LC : Result)
              if (LC.PeriodName == PN->getString()) {
                LC.ArgIndex = static_cast<unsigned>(CI->getZExtValue());
                LC.HasArrInd = true;
              }
      }
    }
  }

  // Find matching global array for each period
  for (GlobalVariable &GV : M.globals()) {
    MDNode *GMD = GV.getMetadata(MD_EJIT_METADATA);
    if (!GMD)
      continue;
    for (auto &LC : Result) {
      for (const MDOperand &Op : GMD->operands()) {
        auto *Sub = dyn_cast<MDNode>(Op.get());
        if (!Sub || Sub->getNumOperands() < 2)
          continue;
        if (auto *Tag = dyn_cast<MDString>(Sub->getOperand(0))) {
          if (Tag->getString() == TAG_EJIT_GROUP_INDEXED) {
            auto *PN = dyn_cast<MDString>(Sub->getOperand(1));
            if (PN && PN->getString() == LC.PeriodName)
              LC.ArrayGV = &GV;
          }
        }
      }
    }
  }

  return Result;
}

} // anonymous namespace

PreservedAnalyses
EJitGuardHandlerPass::run(Module &M, ModuleAnalysisManager &AM) {
  bool Changed = false;
  SmallVector<Function *, 4> LcFuncs;
  for (Function &F : M.functions()) {
    MDNode *MD = F.getMetadata(MD_EJIT_METADATA);
    if (hasMDStringEntry(MD, TAG_EJIT_GUARD) && !F.isDeclaration())
      LcFuncs.push_back(&F);
  }

  if (LcFuncs.empty()) {
    LLVM_DEBUG(dbgs() << "ejit-guard-handler: no lifecycle functions\n");
    return PreservedAnalyses::all();
  }
  LLVM_DEBUG(dbgs() << "ejit-guard-handler: " << LcFuncs.size()
                    << " lifecycle function(s)\n");

  LLVMContext &Ctx = M.getContext();
  auto *PtrTy = PointerType::getUnqual(Ctx);
  auto *I32Ty = Type::getInt32Ty(Ctx);
  auto *VoidTy = Type::getVoidTy(Ctx);

  // Declare runtime functions (only if we have lc functions). Activation is
  // name-level: (const char *periodName, uint8_t cellIdx). The cellIdx is
  // widened to i32 in IR to match the register ABI used at the call site.
  M.getOrInsertFunction(FN_DEACTIVATE,
      FunctionType::get(VoidTy, {PtrTy, I32Ty}, false));
  M.getOrInsertFunction(FN_ACTIVATE,
      FunctionType::get(VoidTy, {PtrTy, I32Ty}, false));

  for (Function *F : LcFuncs) {
    auto LcInfos = collectLifecycleInfo(M, *F);
    if (LcInfos.empty())
      continue;

    // Diagnose ejit_guard without matching ejit_dim.
    for (auto &LC : LcInfos) {
      if (!LC.HasArrInd) {
        errs() << "EJit warning: function '" << F->getName()
               << "' has ejit_guard(\"" << LC.PeriodName
               << "\") but no matching ejit_dim(\""
               << LC.PeriodName << "\") parameter; "
               << "cellIdx will default to arg 0 which is likely wrong\n";
      }
    }

    FunctionCallee DeactivateFn = M.getFunction(FN_DEACTIVATE);
    FunctionCallee ActivateFn = M.getFunction(FN_ACTIVATE);

    // Insert deactivate at entry (after allocas, before first real instruction)
    BasicBlock &EntryBB = F->getEntryBlock();
    Instruction *InsertPt = &*EntryBB.begin();
    while (isa<AllocaInst>(InsertPt) && InsertPt != EntryBB.getTerminator())
      InsertPt = InsertPt->getNextNode();

    for (auto &LC : LcInfos) {
      IRBuilder<> Builder(InsertPt);
      Value *PN = Builder.CreateGlobalString(LC.PeriodName);
      // Use the actual argument value, not the argument index.
      // LC.ArgIndex is the position of the ejit_dim parameter.
      Value *Idx = Builder.CreateZExtOrTrunc(F->getArg(LC.ArgIndex), I32Ty);
      Builder.CreateCall(DeactivateFn, {PN, Idx});
    }

    // Insert activate before each return (same order as deactivate)
    SmallVector<ReturnInst *, 4> Returns;
    for (BasicBlock &BB : *F)
      if (auto *RI = dyn_cast<ReturnInst>(BB.getTerminator()))
        Returns.push_back(RI);

    for (ReturnInst *RI : Returns) {
      for (auto &LC : LcInfos) {
        IRBuilder<> Builder(RI);
        Value *PN = Builder.CreateGlobalString(LC.PeriodName);
        Value *Idx = Builder.CreateZExtOrTrunc(F->getArg(LC.ArgIndex), I32Ty);
        Builder.CreateCall(ActivateFn, {PN, Idx});
      }
    }

    Changed = true;
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}
