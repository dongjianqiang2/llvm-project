//===-- EJitAutoRegister.h - AOT registration constructor -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_EMBEDDEDJIT_EJITAUTOREGISTER_H
#define LLVM_LIB_TRANSFORMS_EMBEDDEDJIT_EJITAUTOREGISTER_H

#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include <utility>

namespace llvm::ejit {

// The caller owns the policy for adding the function to llvm.global_ctors.
inline std::pair<Function *, bool> getOrCreateAutoRegister(Module &M) {
  if (auto *F = M.getFunction(FN_AUTO_REGISTER))
    return {F, false};
  LLVMContext &Ctx = M.getContext();
  auto *F = Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                             GlobalValue::InternalLinkage, FN_AUTO_REGISTER, &M);
  ReturnInst::Create(Ctx, BasicBlock::Create(Ctx, "entry", F));
  return {F, true};
}

} // namespace llvm::ejit

#endif
