//===-- EJitPassBuilder.cpp - Minimal PassBuilder for EJIT ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitPassBuilder.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/DemandedBits.h"
#include "llvm/Analysis/EphemeralValuesCache.h"
#include "llvm/Analysis/InlineAdvisor.h"
#include "llvm/Analysis/LazyCallGraph.h"
#include "llvm/Analysis/LastRunTrackingAnalysis.h"
#include "llvm/Analysis/LazyValueInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"

using namespace llvm;

void ejit::EJitPassBuilder::registerFunctionAnalyses(
    FunctionAnalysisManager &FAM) {
  // AAManager and its sub-analyses.  AAManager() runs its internal
  // sub-analyses (BasicAA etc.) via FAM.  Register them all in FAM
  // so the AAManager Result can access them during run().
  FAM.registerPass([&] { return AAManager(); });
  FAM.registerPass([&] { return BasicAA(); });
  FAM.registerPass([&] { return ScopedNoAliasAA(); });
  FAM.registerPass([&] { return TypeBasedAA(); });

  FAM.registerPass([&] { return AssumptionAnalysis(); });
  // BranchProbabilityAnalysis + BlockFrequencyAnalysis are required by the L3
  // loop-unroll pipeline (LoopFullUnrollPass queries them via getCachedResult).
  // Without them an EJIT_OPT_L3 compile asserts/crashes. Registering them keeps
  // the default (L2) behavior unchanged while making the L3 path functional.
  FAM.registerPass([&] { return BranchProbabilityAnalysis(); });
  FAM.registerPass([&] { return BlockFrequencyAnalysis(); });
  FAM.registerPass([&] { return DemandedBitsAnalysis(); });
  FAM.registerPass([&] { return DominatorTreeAnalysis(); });
  // PGO stage 3: required by InlinerPass (Inliner.cpp:395 fetches it per
  // function). Upstream PassRegistry.def registers it, but EJIT's minimal
  // PassBuilder did not - register it now for the Tier-2 PGO inline.
  FAM.registerPass([&] { return EphemeralValuesAnalysis(); });
  FAM.registerPass([&] { return LazyValueAnalysis(); });
  FAM.registerPass([&] { return LoopAnalysis(); });
  // LastRunTrackingAnalysis needed by SimplifyCFG pass infrastructure.
  FAM.registerPass([&] { return LastRunTrackingAnalysis(); });
  // InlineSizeEstimatorAnalysis and PhiValuesAnalysis intentionally not
  // registered: not used by any pass in the JIT pipeline.
  FAM.registerPass([&] { return MemoryDependenceAnalysis(); });
  FAM.registerPass([&] { return MemorySSAAnalysis(); });
  FAM.registerPass([&] { return OptimizationRemarkEmitterAnalysis(); });
  FAM.registerPass([&] { return PassInstrumentationAnalysis(); });
  FAM.registerPass([&] { return PostDominatorTreeAnalysis(); });
  FAM.registerPass([&] { return ScalarEvolutionAnalysis(); });
  FAM.registerPass([&] { return TargetIRAnalysis(); });
  FAM.registerPass([&] { return TargetLibraryAnalysis(); });
}

void ejit::EJitPassBuilder::registerLoopAnalyses(LoopAnalysisManager &LAM) {
  // Loop passes obtain DT/LI/SE/TTI via the FAM proxy (FunctionToLoopPassAdaptor).
  // Only register analyses that operate directly on Loop&.
  LAM.registerPass([&] { return PassInstrumentationAnalysis(); });
  // IndVarSimplifyPass queries ShouldRunExtraSimpleLoopUnswitch via the LAM
  // when it decides the loop is worth re-unswitching. Without it registered,
  // AnalysisManager::getResult dereferences a null pass and crashes.
  LAM.registerPass([&] { return ShouldRunExtraSimpleLoopUnswitch(); });
}

void ejit::EJitPassBuilder::registerCGSCCAnalyses(
    CGSCCAnalysisManager &CGAM) {
  CGAM.registerPass([&] { return FunctionAnalysisManagerCGSCCProxy(); });
  CGAM.registerPass([&] { return PassInstrumentationAnalysis(); });
}

void ejit::EJitPassBuilder::registerModuleAnalyses(
    ModuleAnalysisManager &MAM) {
  MAM.registerPass([&] { return PassInstrumentationAnalysis(); });
  MAM.registerPass([&] { return ProfileSummaryAnalysis(); });
  // PGO stage 3: ModuleInlinerWrapperPass fetches InlineAdvisorAnalysis from
  // the MAM (Inliner.cpp wrapper::run). Required for the Tier-2 PGO inline.
  MAM.registerPass([&] { return InlineAdvisorAnalysis(); });
  // LazyCallGraphAnalysis: required by the ModuleToPostOrderCGSCCPassAdaptor
  // the inline wrapper runs (builds the call graph for CGSCC traversal).
  MAM.registerPass([&] { return LazyCallGraphAnalysis(); });
}

void ejit::EJitPassBuilder::crossRegisterProxies(
    LoopAnalysisManager &LAM, FunctionAnalysisManager &FAM,
    CGSCCAnalysisManager &CGAM, ModuleAnalysisManager &MAM) {
  MAM.registerPass([&] { return FunctionAnalysisManagerModuleProxy(FAM); });
  MAM.registerPass([&] { return CGSCCAnalysisManagerModuleProxy(CGAM); });
  CGAM.registerPass([&] { return ModuleAnalysisManagerCGSCCProxy(MAM); });
  FAM.registerPass([&] { return CGSCCAnalysisManagerFunctionProxy(CGAM); });
  FAM.registerPass([&] { return ModuleAnalysisManagerFunctionProxy(MAM); });
  FAM.registerPass([&] { return LoopAnalysisManagerFunctionProxy(LAM); });
  LAM.registerPass([&] { return FunctionAnalysisManagerLoopProxy(FAM); });
}
