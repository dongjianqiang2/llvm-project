//===-- EJitVersionMerge.cpp - Final member body assembly -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVersionMerge.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/ExecutionEngine/EJIT/EJitFinalReplay.h"
#include "llvm/ExecutionEngine/EJIT/EJitGlobalValueReads.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Linker/Linker.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/SHA256.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include <map>
#include <set>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {
AttributeList scalarABI(const Function &F);
}

std::array<uint8_t, 32> EJitFinalReplay::digest(StringRef Bytes) {
  SHA256 Hash;
  Hash.update(Bytes);
  return Hash.final();
}

static unsigned replayWidth(Type *T, const DataLayout &DL) {
  if (T->isPointerTy())
    return DL.getPointerSizeInBits(T->getPointerAddressSpace());
  if (T->isIntegerTy() || T->isFloatTy() || T->isDoubleTy())
    return T->getScalarSizeInBits();
  return 0;
}

void EJitFinalReplay::record(Function &F, ArrayRef<Replacement> Roots,
                             ArrayRef<Replacement> Fields) {
  const uint32_t Frame = frames++;
  if (!usable || (Roots.empty() && Fields.empty()))
    return;
  if (Roots.size() + Fields.size() >
      kEJitFinalMergeMaxMemberBytes / sizeof(Fold) - folds.size()) {
    usable = false;
    return;
  }
  DenseMap<LoadInst *, Constant *> Values;
  for (auto R : Roots)
    Values.try_emplace(R.LI, R.ConstVal);
  for (auto R : Fields)
    Values.try_emplace(R.LI, R.ConstVal);
  uint32_t Ordinal = 0;
  for (Instruction &I : instructions(F)) {
    auto *L = dyn_cast<LoadInst>(&I);
    if (!L)
      continue;
    auto N = Ordinal++;
    Constant *C = Values.lookup(L);
    if (!C)
      continue;
    Type *T = L->getType();
    unsigned W = replayWidth(T, F.getParent()->getDataLayout());
    if (!L->isSimple() || !W || W > 64) {
      usable = false;
      return;
    }
    uint64_t V;
    if (auto *CI = dyn_cast<ConstantInt>(C))
      V = CI->getZExtValue();
    else if (auto *CF = dyn_cast<ConstantFP>(C))
      V = CF->getValueAPF().bitcastToAPInt().getZExtValue();
    else if (isa<ConstantPointerNull>(C))
      V = 0;
    else if (auto *CE = dyn_cast<ConstantExpr>(C);
             CE && CE->getOpcode() == Instruction::IntToPtr &&
             isa<ConstantInt>(CE->getOperand(0)) &&
             CE->getOperand(0)->getType()->getIntegerBitWidth() <= 64)
      V = cast<ConstantInt>(CE->getOperand(0))->getZExtValue();
    else {
      usable = false;
      return;
    }
    folds.push_back({Frame, N, V, T->getTypeID(), W});
  }
}

bool EJitFinalReplayCursor::apply(Function &F) {
  const unsigned Frame = frame++;
  if (!valid || next == recipe.folds.size() ||
      recipe.folds[next].frame != Frame)
    return false;
  SmallVector<LoadInst *, 8> Erase;
  uint32_t Ordinal = 0;
  for (Instruction &I : instructions(F)) {
    auto *L = dyn_cast<LoadInst>(&I);
    if (!L)
      continue;
    const auto &R = recipe.folds[next];
    if (Ordinal++ != R.ordinal)
      continue;
    Type *T = L->getType();
    if (!L->isSimple() || T->getTypeID() != R.typeID ||
        replayWidth(T, F.getParent()->getDataLayout()) != R.width) {
      valid = false;
      break;
    }
    APInt Bits(R.width, R.bits);
    Constant *C;
    if (T->isIntegerTy())
      C = ConstantInt::get(T, Bits);
    else if (T->isPointerTy())
      C = ConstantExpr::getIntToPtr(ConstantInt::get(T->getContext(), Bits), T);
    else
      C = ConstantFP::get(T->getContext(), APFloat(T->getFltSemantics(), Bits));
    L->replaceAllUsesWith(C);
    Erase.push_back(L);
    if (++next == recipe.folds.size() || recipe.folds[next].frame != Frame)
      break;
  }
  if (next != recipe.folds.size() && recipe.folds[next].frame == Frame)
    valid = false;
  for (auto *L : Erase)
    L->eraseFromParent();
  return !Erase.empty();
}

Error llvm::ejit::validateFinalVersionModule(const Module &M, StringRef Entry) {
  const Function *Root = M.getFunction(Entry);
  if (!Root || Root->isDeclaration() || !M.alias_empty() || !M.ifunc_empty() ||
      !M.getModuleInlineAsm().empty())
    return make_error<StringError>("unsupported final module identity",
                                  inconvertibleErrorCode());
  for (const GlobalVariable &G : M.globals())
    if (G.isThreadLocal() ||
        (!G.isDeclaration() && (!G.isConstant() || !G.hasLocalLinkage() ||
                                (!G.hasGlobalUnnamedAddr() &&
                                 !isValueOnlyFinalTable(G)))))
      return make_error<StringError>("private state or observable global: " +
                                        G.getName(),
                                    inconvertibleErrorCode());
  unsigned Instructions = 0;
  uint64_t ConstantBytes = 0;
  for (const GlobalVariable &G : M.globals()) {
    if (G.isDeclaration())
      continue;
    if (!G.getValueType()->isSized())
      return make_error<StringError>("unsized constant", inconvertibleErrorCode());
    TypeSize Size = M.getDataLayout().getTypeAllocSize(G.getValueType());
    if (Size.isScalable() || Size.getFixedValue() > kEJitFinalMergeMaxMemberBytes ||
        (ConstantBytes += Size.getFixedValue()) > kEJitFinalMergeMaxMemberBytes)
      return make_error<StringError>("constant byte budget", inconvertibleErrorCode());
  }
  for (const Function &F : M) {
    if (F.isDeclaration())
      continue;
    if (F.hasAddressTaken() || F.hasComdat())
      return make_error<StringError>("observable function identity",
                                    inconvertibleErrorCode());
    for (const Instruction &I : instructions(F)) {
      if (++Instructions > kEJitFinalMergeMaxMemberInstructions)
        return make_error<StringError>("member instruction budget",
                                      inconvertibleErrorCode());
      // Absolute indirect VP targets may point into a retiring private T1.
      // Keep those members independent until target remapping is implemented.
      if (const auto *CB = dyn_cast<CallBase>(&I))
        if (!CB->getCalledFunction())
          return make_error<StringError>("indirect call target identity",
                                        inconvertibleErrorCode());
    }
  }
  return Error::success();
}

Expected<SmallVector<unsigned, 2>> llvm::ejit::finalVersionDimensionArgs(
    const Function &F, ArrayRef<std::string> Periods) {
  if (Periods.empty() || Periods.size() > 2)
    return make_error<StringError>("expected one or two cell/TRP dimensions",
                                  inconvertibleErrorCode());
  auto *MD = F.getMetadata("ejit.metadata");
  if (!MD)
    return make_error<StringError>("missing entry metadata",
                                  inconvertibleErrorCode());
  SmallVector<unsigned, 2> Result;
  for (const std::string &Period : Periods) {
    if (Period != "cell" && Period != "trp")
      return make_error<StringError>("not an ordinary cell/TRP dimension",
                                    inconvertibleErrorCode());
    unsigned Arg = F.arg_size();
    for (const MDOperand &Op : MD->operands()) {
      auto *N = dyn_cast_or_null<MDNode>(Op.get());
      if (!N || N->getNumOperands() < 3)
        continue;
      auto *Tag = dyn_cast<MDString>(N->getOperand(0));
      auto *PN = dyn_cast<MDString>(N->getOperand(1));
      if (!Tag || !PN || Tag->getString() != "ejit_period_arr_ind" ||
          PN->getString() != Period)
        continue;
      auto *Idx = mdconst::dyn_extract<ConstantInt>(N->getOperand(2));
      if (!Idx || Arg != F.arg_size() || Idx->getZExtValue() >= F.arg_size())
        return make_error<StringError>("ambiguous dimension argument",
                                      inconvertibleErrorCode());
      Arg = Idx->getZExtValue();
    }
    if (Arg == F.arg_size() || !F.getArg(Arg)->getType()->isIntegerTy() ||
        F.getArg(Arg)->getType()->getIntegerBitWidth() > 32)
      return make_error<StringError>("unsupported dimension argument",
                                    inconvertibleErrorCode());
    Result.push_back(Arg);
  }
  return Result;
}

Expected<std::string> llvm::ejit::assembleFinalVersionModules(
    StringRef Entry, uintptr_t AotFallback, ArrayRef<unsigned> DimensionArgs,
    ArrayRef<EJitFinalModuleArm> Inputs) {
  if (!AotFallback || Inputs.size() < 2 ||
      Inputs.size() > kEJitFinalMergeMaxArms)
    return make_error<StringError>("invalid final merge cohort",
                                  inconvertibleErrorCode());
  LLVMContext C;
  auto Combined = std::make_unique<Module>("ejit.final.merge", C);
  SmallVector<std::string, 8> Names;
  size_t Bytes = 0;
  for (unsigned I = 0; I < Inputs.size(); ++I) {
    if (Inputs[I].bitcode.size() > kEJitFinalMergeMaxMemberBytes ||
        Inputs[I].bitcode.size() > kEJitFinalMergeMaxArtifactBytes - Bytes)
      return make_error<StringError>("artifact budget", inconvertibleErrorCode());
    Bytes += Inputs[I].bitcode.size();
    auto Parsed = parseBitcodeFile(MemoryBufferRef(Inputs[I].bitcode, Entry), C);
    if (!Parsed)
      return Parsed.takeError();
    auto &M = **Parsed;
    if (auto E = validateFinalVersionModule(M, Entry))
      return E;
    if (I == 0) {
      Combined->setDataLayout(M.getDataLayout());
      Combined->setTargetTriple(M.getTargetTriple());
    } else if (M.getDataLayout() != Combined->getDataLayout() ||
               M.getTargetTriple() != Combined->getTargetTriple()) {
      return make_error<StringError>("member target mismatch",
                                    inconvertibleErrorCode());
    }
    Function *Root = M.getFunction(Entry);
    std::string Prefix = "__ejit_merge_" + std::to_string(I) + "_";
    // Namespace every private definition before LLVM's structured linker.
    for (GlobalVariable &G : M.globals())
      if (!G.isDeclaration())
        G.setName(Prefix + G.getName().str());
    for (Function &F : M)
      if (!F.isDeclaration()) {
        F.setName(Prefix + F.getName().str());
        F.setLinkage(GlobalValue::ExternalLinkage);
        F.setVisibility(GlobalValue::DefaultVisibility);
      }
    Names.push_back(Root->getName().str());
    // Keep target/ABI flags and debug compile units. Only member-local EJIT
    // registration and whole-module profile summaries do not describe a cohort.
    SmallVector<NamedMDNode *, 4> Registration;
    for (NamedMDNode &MD : M.named_metadata())
      if (MD.getName().starts_with("ejit."))
        Registration.push_back(&MD);
    for (auto *MD : Registration)
      M.eraseNamedMetadata(MD);
    if (auto *Flags = M.getNamedMetadata("llvm.module.flags")) {
      SmallVector<MDNode *, 8> Kept;
      for (MDNode *Flag : Flags->operands()) {
        auto *Key = Flag->getNumOperands() > 1
                        ? dyn_cast_or_null<MDString>(Flag->getOperand(1))
                        : nullptr;
        if (!Key || Key->getString() != "ProfileSummary")
          Kept.push_back(Flag);
      }
      Flags->clearOperands();
      for (auto *Flag : Kept)
        Flags->addOperand(Flag);
    }
    if (Linker::linkModules(*Combined, std::move(*Parsed)))
      return make_error<StringError>("member link failed",
                                    inconvertibleErrorCode());
  }
  SmallVector<EJitFinalMergeArm, 8> Arms;
  for (unsigned I = 0; I < Inputs.size(); ++I)
    Arms.push_back({Combined->getFunction(Names[I]), Inputs[I].dimensions});
  auto *Fallback = Function::Create(Arms[0].body->getFunctionType(),
                                    GlobalValue::ExternalLinkage,
                                    "__ejit_merge_aot", *Combined);
  Fallback->setAttributes(scalarABI(*Arms[0].body));
  auto Merged = assembleFinalVersionSwitch(Entry, *Fallback, DimensionArgs, Arms);
  if (!Merged)
    return Merged.takeError();
  auto *PtrTy = Fallback->getType();
  auto *IntTy = Combined->getDataLayout().getIntPtrType(C);
  Fallback->replaceAllUsesWith(ConstantExpr::getIntToPtr(
      ConstantInt::get(IntTy, AotFallback), PtrTy));
  Fallback->eraseFromParent();
  for (auto &Arm : Arms)
    Arm.body->eraseFromParent();
  for (Function &F : *Combined)
    if (&F != *Merged && !F.isDeclaration()) {
      F.setLinkage(GlobalValue::InternalLinkage);
      F.setVisibility(GlobalValue::DefaultVisibility);
    }
  if (verifyModule(*Combined))
    return make_error<StringError>("invalid final module",
                                  inconvertibleErrorCode());
  std::string Result;
  raw_string_ostream OS(Result);
  WriteBitcodeToFile(*Combined, OS);
  if (Result.size() > kEJitFinalMergeMaxArtifactBytes)
    return make_error<StringError>("combined artifact budget",
                                  inconvertibleErrorCode());
  return Result;
}

namespace {
Error reject(const Twine &Reason) {
  return make_error<StringError>("final-version-merge: " + Reason,
                                 inconvertibleErrorCode());
}

bool supportedScalar(Type *Ty) {
  return Ty->isIntegerTy() || Ty->isFloatingPointTy() || Ty->isPointerTy();
}

// Restrict the ABI until aggregate/special calling-convention lowering
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
    for (auto Kind : {Attribute::ByVal, Attribute::StructRet, Attribute::ByRef,
                      Attribute::InAlloca, Attribute::Preallocated,
                      Attribute::InReg, Attribute::SwiftSelf,
                      Attribute::SwiftError, Attribute::SwiftAsync,
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
    if (const char *Reason = checkMemberCalls(*Arm.body, Members,
                                             Limits.maxInstructions))
      return reject(Reason);
    for (StringRef Attr : {"target-cpu", "target-features"})
      if (Arm.body->getFnAttribute(Attr) != Arms.front().body->getFnAttribute(Attr))
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
      Switch->addCase(ConstantInt::get(cast<IntegerType>(Selector->getType()),
                                      KV.first), Case);
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
