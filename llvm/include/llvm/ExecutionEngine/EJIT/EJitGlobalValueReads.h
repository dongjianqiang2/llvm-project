//===-- EJitGlobalValueReads.h - Address-insensitive table reads -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITGLOBALVALUEREADS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITGLOBALVALUEREADS_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"

namespace llvm::ejit {
/// Duplicating immutable bytes is OK if the address is used only to read
/// those bytes. Unknown users (including other initializers) fail closed.
inline bool isValueOnlyFinalTable(const GlobalVariable &G) {
  if (!G.isConstant() || !G.hasLocalLinkage() || G.isThreadLocal() ||
      G.isExternallyInitialized() || !G.hasDefinitiveInitializer())
    return false;
  SmallVector<const Value *, 8> Work{&G};
  SmallPtrSet<const Value *, 16> Seen;
  while (!Work.empty()) {
    const Value *V = Work.pop_back_val();
    if (!Seen.insert(V).second)
      continue;
    for (const User *U : V->users()) {
      if (const auto *L = dyn_cast<LoadInst>(U)) {
        if (L->isSimple() && L->getPointerOperand() == V)
          continue;
      } else if (isa<GEPOperator, BitCastOperator>(U) &&
                 U->getOperand(0) == V) {
        Work.push_back(U);
        continue;
      }
      return false;
    }
  }
  return true;
}
} // namespace llvm::ejit

#endif
