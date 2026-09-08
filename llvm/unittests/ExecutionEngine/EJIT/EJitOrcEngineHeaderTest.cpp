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

static_assert(llvm::ejit::detail::codeGenOptLevelFor(
                  llvm::ejit::OptimizationLevel::L1) ==
                  llvm::CodeGenOptLevel::Less,
              "EJIT L1 must select less backend optimization");
static_assert(llvm::ejit::detail::codeGenOptLevelFor(
                  llvm::ejit::OptimizationLevel::L2) ==
                  llvm::CodeGenOptLevel::Default,
              "EJIT L2 must select default backend optimization");
static_assert(llvm::ejit::detail::codeGenOptLevelFor(
                  llvm::ejit::OptimizationLevel::L3) ==
                  llvm::CodeGenOptLevel::Aggressive,
              "EJIT L3 must select aggressive backend optimization");
static_assert(llvm::ejit::detail::codeGenOptLevelFor(
                  static_cast<llvm::ejit::OptimizationLevel>(0)) ==
                  llvm::CodeGenOptLevel::Default,
              "invalid internal levels must use the safe backend default");
