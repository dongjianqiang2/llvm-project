//===-- EJitFreeDim.h - Local value boundaries ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_EMBEDDEDJIT_EJITFREEDIM_H
#define LLVM_TRANSFORMS_EMBEDDEDJIT_EJITFREEDIM_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/ModRef.h"

namespace llvm {

// Not a runtime API: extraction keeps this opaque identity operation; every
// AOT/JIT codegen path must lower it to its actual input after consumption.
inline Function *getEJitLocalFreeDim(Module &M, IntegerType *Ty) {
  std::string Name =
      "__ejit_local_free_dim_v1_i" + std::to_string(Ty->getBitWidth());
  auto *FT = FunctionType::get(Ty, {Ty}, false);
  auto *F = cast<Function>(M.getOrInsertFunction(Name, FT).getCallee());
  F->addFnAttr("ejit.local_free_dim", "v1");
  F->setMemoryEffects(MemoryEffects::none());
  F->addFnAttr(Attribute::NoUnwind);
  F->addFnAttr(Attribute::WillReturn);
  F->addFnAttr(Attribute::NoCallback);
  F->addFnAttr(Attribute::NoSync);
  return F;
}

inline bool isEJitLocalFreeDimFunction(const Function &F) {
  auto *Ty = dyn_cast<IntegerType>(F.getReturnType());
  if (!Ty || !F.isDeclaration() || F.isVarArg() || F.arg_size() != 1 ||
      F.getFunctionType()->getParamType(0) != Ty)
    return false;
  return F.getName() ==
             "__ejit_local_free_dim_v1_i" + std::to_string(Ty->getBitWidth()) &&
         F.hasFnAttribute("ejit.local_free_dim") &&
         F.getFnAttribute("ejit.local_free_dim").getValueAsString() == "v1";
}

inline bool isEJitLocalFreeDim(const Value *V) {
  auto *Call = dyn_cast<CallInst>(V);
  auto *F = Call ? Call->getCalledFunction() : nullptr;
  return F && isEJitLocalFreeDimFunction(*F) && Call->arg_size() == 1 &&
         Call->getType() == Call->getArgOperand(0)->getType();
}

// Inspect nodes through operand distance 16, including markers at that
// boundary. Breadth-first order makes the first visit the shortest path, so a
// shared node reached at the depth limit cannot hide a later, shallower marker
// path. Visited state belongs to this query only: callers may mutate IR between
// calls. VisitedValues optionally exposes deterministic work counts for
// regressions.
inline bool usesEJitLocalFreeDim(const Value *Root, bool HasLocalMarkers,
                                 unsigned *VisitedValues = nullptr) {
  if (VisitedValues)
    *VisitedValues = 0;
  if (!HasLocalMarkers)
    return false;

  SmallPtrSet<const Value *, 32> Seen;
  SmallVector<std::pair<const Value *, unsigned>, 32> Work;
  Seen.insert(Root);
  Work.emplace_back(Root, 0);
  for (size_t I = 0; I != Work.size(); ++I) {
    const auto [V, Depth] = Work[I];
    if (VisitedValues)
      ++*VisitedValues;
    if (isEJitLocalFreeDim(V))
      return true;
    if (Depth == 16 || (!isa<Instruction>(V) && !isa<ConstantExpr>(V)))
      continue;
    for (const Value *Op : cast<User>(V)->operand_values())
      if (Seen.insert(Op).second)
        Work.emplace_back(Op, Depth + 1);
  }
  return false;
}

inline bool lowerEJitLocalFreeDims(Module &M) {
  SmallVector<CallInst *, 8> Calls;
  SmallVector<Function *, 4> Declarations;
  for (Function &F : M) {
    if (!isEJitLocalFreeDimFunction(F))
      continue;
    Declarations.push_back(&F);
    for (User *U : F.users())
      if (auto *Call = dyn_cast<CallInst>(U))
        if (isEJitLocalFreeDim(Call))
          Calls.push_back(Call);
  }
  for (CallInst *Call : Calls) {
    Call->replaceAllUsesWith(Call->getArgOperand(0));
    Call->eraseFromParent();
  }
  for (Function *F : Declarations)
    if (F->use_empty())
      F->eraseFromParent();
  return !Declarations.empty();
}

} // namespace llvm

#endif
