//===-- EJitPasses.h - EmbeddedJIT AOT Pass Declarations ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_EMBEDDEDJIT_EJITPASSES_H
#define LLVM_TRANSFORMS_EMBEDDEDJIT_EJITPASSES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/IR/PassManager.h"
#include <cstdint>

namespace llvm {

namespace ejit::detail {

struct EJitPreSerializationDCEStats {
  uint64_t FunctionDefinitionsBefore = 0;
  uint64_t FunctionDefinitionsAfter = 0;
  uint64_t InstructionsBefore = 0;
  uint64_t InstructionsAfter = 0;
};

/// Remove internal definitions that became unreachable after AOT extraction
/// and internalization. Entry definitions are retained even when their source
/// linkage is discardable.
EJitPreSerializationDCEStats
runEJitPreSerializationGlobalDCE(Module &M,
                                 ArrayRef<StringRef> AdditionalRootNames = {});

} // namespace ejit::detail

struct EJitRegisterBitcodePass
    : public PassInfoMixin<EJitRegisterBitcodePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct EJitRegisterPeriodPass
    : public PassInfoMixin<EJitRegisterPeriodPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct EJitWrapperGenPass
    : public PassInfoMixin<EJitWrapperGenPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct EJitPeriodHandlerPass
    : public PassInfoMixin<EJitPeriodHandlerPass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

struct EJitAotModulePass
    : public PassInfoMixin<EJitAotModulePass> {
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
};

} // namespace llvm

#endif
