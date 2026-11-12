//===-- EJitVersionMerge.cpp - Final member body assembly
//-------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVersionMerge.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <map>
#include <set>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {
Error reject(const Twine &Reason) {
  return make_error<StringError>("final-version-merge: " + Reason,
                                 inconvertibleErrorCode());
}

bool supportedScalar(Type *Ty) {
  return Ty->isIntegerTy() || Ty->isFloatingPointTy() || Ty->isPointerTy();
}

// Restrict the prototype until aggregate/special calling-convention lowering
// has dedicated coverage. Do not copy inferred range/nonnull/memory attributes
// from a specialized member onto the generic dispatcher or its AOT edge.
bool supportedPrototype(const Function &F) {
  if (F.isIntrinsic() || F.isVarArg() || F.getCallingConv() != CallingConv::C ||
      F.getAddressSpace() != 0 || F.hasPersonalityFn() || F.hasGC() ||
      F.hasFnAttribute(Attribute::Naked) ||
      F.hasFnAttribute(Attribute::Convergent) ||
      F.hasFnAttribute(Attribute::NoDuplicate) ||
      (!F.getReturnType()->isVoidTy() && !supportedScalar(F.getReturnType())))
    return false;
  for (const Argument &A : F.args()) {
    if (!supportedScalar(A.getType()))
      return false;
    AttributeSet AS = F.getAttributes().getParamAttrs(A.getArgNo());
    for (auto Kind :
         {Attribute::ByVal, Attribute::StructRet, Attribute::ByRef,
          Attribute::InAlloca, Attribute::Preallocated, Attribute::InReg,
          Attribute::SwiftSelf, Attribute::SwiftError, Attribute::SwiftAsync,
          Attribute::Nest, Attribute::ImmArg})
      if (AS.hasAttribute(Kind))
        return false;
  }
  return !F.getAttributes().getRetAttrs().hasAttribute(Attribute::InReg);
}

AttributeList scalarABI(const Function &F) {
  AttributeList Result;
  for (unsigned I = AttributeList::ReturnIndex; I <= F.arg_size(); ++I)
    for (auto Kind : {Attribute::SExt, Attribute::ZExt})
      if (F.getAttributes().hasAttributeAtIndex(I, Kind))
        Result = Result.addAttributeAtIndex(F.getContext(), I, Kind);
  return Result;
}

const char *checkMemberCalls(Function &F,
                             const SmallPtrSetImpl<Function *> &Members,
                             unsigned Budget) {
  SmallPtrSet<Function *, 16> Seen;
  SmallVector<Function *, 16> Work{&F};
  uint64_t Visited = 0;
  while (!Work.empty()) {
    Function *Current = Work.pop_back_val();
    if (!Seen.insert(Current).second)
      continue;
    for (Instruction &I : instructions(*Current)) {
      if (++Visited > Budget)
        return "callgraph-budget";
      if (auto *CB = dyn_cast<CallBase>(&I))
        if (auto *Callee = dyn_cast<Function>(
                CB->getCalledOperand()->stripPointerCasts())) {
          if (Members.contains(Callee))
            return "member-recursion-or-cross-call";
          if (!Callee->isDeclaration())
            Work.push_back(Callee);
        }
    }
  }
  return nullptr;
}
} // namespace

Expected<Function *> llvm::ejit::assembleFinalVersionSwitch(
    StringRef Name, Function &AotFallback, ArrayRef<unsigned> DimensionArgs,
    ArrayRef<EJitFinalMergeArm> Arms, const EJitFinalMergeLimits &Limits) {
  Module *M = AotFallback.getParent();
  if (!M || Name.empty() || M->getNamedValue(Name))
    return reject("missing-module-or-duplicate-name");
  if (!AotFallback.isDeclaration() || !supportedPrototype(AotFallback))
    return reject("unsupported-aot-prototype");
  if (Arms.empty() || Arms.size() > Limits.maxArms || DimensionArgs.empty() ||
      DimensionArgs.size() > 2)
    return reject("arm-or-dimension-limit");

  SmallVector<unsigned, 2> Widths;
  std::set<unsigned> SeenArgs;
  for (unsigned Arg : DimensionArgs) {
    if (Arg >= AotFallback.arg_size() || !SeenArgs.insert(Arg).second)
      return reject("invalid-dimension-argument");
    auto *Ty = dyn_cast<IntegerType>(AotFallback.getArg(Arg)->getType());
    if (!Ty || Ty->getBitWidth() > 32)
      return reject("unsupported-dimension-type");
    Widths.push_back(Ty->getBitWidth());
  }

  SmallPtrSet<Function *, 8> Members;
  std::set<std::vector<uint32_t>> Tuples;
  uint64_t Instructions = 0;
  const AttributeList ABI = scalarABI(AotFallback);
  for (const auto &Arm : Arms) {
    Function *F = Arm.body;
    if (!F || F->getParent() != M || F->isDeclaration() ||
        F->getFunctionType() != AotFallback.getFunctionType() ||
        !supportedPrototype(*F) || scalarABI(*F) != ABI)
      return reject("member-prototype-mismatch");
    if (Arm.dimensions.size() != DimensionArgs.size())
      return reject("member-dimension-count");
    for (unsigned I = 0; I != Widths.size(); ++I)
      if (uint64_t{Arm.dimensions[I]} >= (uint64_t{1} << Widths[I]))
        return reject("dimension-value-does-not-fit");
    if (!Tuples.insert({Arm.dimensions.begin(), Arm.dimensions.end()}).second)
      return reject("duplicate-member-tuple");
    if (F->hasAddressTaken())
      return reject("member-address-observable");
    Members.insert(F);
    for (BasicBlock &BB : *F) {
      if (BB.hasAddressTaken() || BB.isEHPad())
        return reject("unsupported-member-control-flow");
      for (Instruction &I : BB) {
        ++Instructions;
        if (isa<InvokeInst, CallBrInst, IndirectBrInst>(I) ||
            I.getType()->isTokenTy())
          return reject("unsupported-member-control-flow");
        if (auto *CB = dyn_cast<CallBase>(&I))
          if (CB->isConvergent() || CB->cannotDuplicate())
            return reject("nonduplicable-call");
      }
    }
    if (Instructions > Limits.maxInstructions)
      return reject("instruction-budget");
  }
  for (const auto &Arm : Arms) {
    if (const char *Reason =
            checkMemberCalls(*Arm.body, Members, Limits.maxInstructions))
      return reject(Reason);
    for (StringRef Attr : {"target-cpu", "target-features"})
      if (Arm.body->getFnAttribute(Attr) !=
          Arms.front().body->getFnAttribute(Attr))
        return reject("target-attribute-mismatch");
  }

  auto *Merged = Function::Create(AotFallback.getFunctionType(),
                                  GlobalValue::ExternalLinkage, Name, M);
  Merged->setAttributes(ABI);
  for (StringRef Attr : {"target-cpu", "target-features"})
    if (Attribute A = Arms.front().body->getFnAttribute(Attr); A.isValid())
      Merged->addFnAttr(A);
  SmallVector<Value *, 12> Args;
  for (Argument &A : Merged->args())
    Args.push_back(&A);
  auto *Entry = BasicBlock::Create(M->getContext(), "dispatch", Merged);
  auto *Default = BasicBlock::Create(M->getContext(), "aot", Merged);
  IRBuilder<> B(Default);
  auto *FallbackCall = B.CreateCall(&AotFallback, Args);
  FallbackCall->setAttributes(ABI);
  if (Merged->getReturnType()->isVoidTy())
    B.CreateRetVoid();
  else
    B.CreateRet(FallbackCall);

  SmallVector<CallInst *, 8> Calls;
  SmallVector<const EJitFinalMergeArm *, 8> Selected;
  for (const auto &Arm : Arms)
    Selected.push_back(&Arm);
  auto Build = [&](auto &&Self, BasicBlock *Block, unsigned Depth,
                   ArrayRef<const EJitFinalMergeArm *> Group) -> void {
    B.SetInsertPoint(Block);
    if (Depth == DimensionArgs.size()) {
      auto *Call = B.CreateCall(Group.front()->body, Args);
      Call->setAttributes(Group.front()->body->getAttributes());
      Calls.push_back(Call);
      if (Merged->getReturnType()->isVoidTy())
        B.CreateRetVoid();
      else
        B.CreateRet(Call);
      return;
    }
    std::map<uint32_t, SmallVector<const EJitFinalMergeArm *, 8>> Groups;
    for (const auto *Arm : Group)
      Groups[Arm->dimensions[Depth]].push_back(Arm);
    Value *Selector = Merged->getArg(DimensionArgs[Depth]);
    auto *Switch = B.CreateSwitch(Selector, Default, Groups.size());
    for (const auto &KV : Groups) {
      auto *Case = BasicBlock::Create(M->getContext(), "member", Merged);
      Switch->addCase(
          ConstantInt::get(cast<IntegerType>(Selector->getType()), KV.first),
          Case);
      Self(Self, Case, Depth + 1, KV.second);
    }
  };
  Build(Build, Entry, 0, Selected);

  for (CallInst *Call : Calls) {
    InlineFunctionInfo IFI;
    InlineResult Result = InlineFunction(*Call, IFI, false);
    if (!Result.isSuccess()) {
      std::string Why = Result.getFailureReason();
      Merged->eraseFromParent();
      return reject("member-inline-failed: " + Why);
    }
  }
  if (Merged->getInstructionCount() > Limits.maxInstructions) {
    Merged->eraseFromParent();
    return reject("assembled-instruction-budget");
  }
  if (verifyFunction(*Merged)) {
    Merged->eraseFromParent();
    return reject("invalid-merged-ir");
  }
  return Merged;
}
