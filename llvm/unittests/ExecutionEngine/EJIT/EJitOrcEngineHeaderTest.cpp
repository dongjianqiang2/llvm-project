//===-- EJitOrcEngineHeaderTest.cpp - EJitOrcEngine include test ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM
// Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"

static_assert(llvm::ejit::kEJitMaxBoundPointers == 8u,
              "EJitOrcEngine must include the bound-pointer limit header");
