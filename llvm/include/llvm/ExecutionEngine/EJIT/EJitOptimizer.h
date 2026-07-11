//===-- EJitOptimizer.h - JIT Optimization Pipeline -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITOPTIMIZER_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITOPTIMIZER_H

#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/EJIT/EJitPassBuilder.h"

namespace llvm {
namespace ejit {

/// JIT optimization pipeline. Runs on the extracted bitcode module during
/// JIT compilation to specialize the code for the current time-window values.
/// Holds persistent AnalysisManagers to avoid re-registering analyses on
/// every compilation.
class EJitOptimizer {
public:
  EJitOptimizer(PeriodArrayRegistry &reg);

  /// Run the full JIT specialization pipeline:
  ///   1. Parameter substitution (ejit_period_arr_ind → constants)
  ///   2. InstCombine (fold GEP chains from substituted params)
  ///   3. Inline (L2+: expand callee bodies so may_const GEPs are traceable)
  ///   4. StructFieldPass (may_const loads → runtime constants)
  ///   5. Core optimization pipeline (L1/L2/L3)
  ///
  /// ctx.tier selects the PGO branch (EJIT_ONLINE_PGO.md §4): Baseline (no
  /// PGO), Instrumented (Tier-1: + PGOGen/Lowering/capture), PGOUse (Tier-2:
  /// + PGOUse(profile) then the core pipeline).
  void runPipeline(Module &M, const SpecializationContext &ctx);

  /// Clear all cached analysis results. Must be called between compilations
  /// to avoid dangling pointers to IR units from previous modules.
  void clearAnalyses();

  /// PGO counter global names captured during the last Instrumented (Tier-1)
  /// compile (PGOFuncName suffix of each __profc_<name>). Empty for
  /// Baseline/PGOUse. The compile driver looks up __profc_/__profd_ by these
  /// names to capture counter addresses for Tier-2 profile synthesis.
  ArrayRef<std::string> getLastCounterNames() const {
    return lastCounterNames_;
  }

private:
  /// Replace ejit_period_arr_ind parameters with their runtime constants.
  void preReplacePeriodIndices(Module &M, const SpecializationContext &ctx);

  /// Run InstCombine on all functions (single pass).
  void runInstCombine(Module &M);

  /// Run EJitStructFieldPass on all functions.
  void runStructFieldPass(Module &M);

  /// Push the specialized constants across call edges. The AOT inliner keeps a
  /// call edge wherever it chose not to inline, so after phase 1 every call
  /// site passes the period dims (and values derived from them) as ordinary
  /// constant arguments — but the callee bodies still re-derive cell addressing
  /// and re-test guards the entry already resolved. Internalizes every defined
  /// non-ejit_entry function (IPSCCP only reasons about arguments of functions
  /// whose call sites it can enumerate: local linkage, not address-taken), then
  /// runs IPSCCP to propagate constant arguments into callee bodies and
  /// constant returns back to call sites.
  void runInterproceduralPropagation(Module &M);

  /// Light fold pass run at the PGO Gen/Use point (InstCombine + SimplifyCFG)
  /// to fold branches exposed by specialization. Identical prefix for Tier-1
  /// and Tier-2 keeps the CFG (and thus the PGO hash) aligned.
  void runLightOptPipeline(Module &M);

  /// After PGOInstrumentationGen + InstrProfilingLoweringPass, force the
  /// __profc_*/__profd_* counter globals to ExternalLinkage (default
  /// InternalLinkage is invisible to ORC J->lookup, P0-3) and record each
  /// PGOFuncName (suffix of __profc_<name>) in lastCounterNames_.
  void captureCounterGlobals(Module &M);

  /// Run the EJIT optimization pipeline: a single fused sequence that exploits
  /// the just-substituted period-index / may_const constants to their fixed
  /// point (scalar fold/propagate/simplify), folds loops whose bounds became
  /// constant, re-specializes the array accesses that unrolling turns into
  /// constant-index GEPs, then does a final cleanup. `level` is accepted for ABI
  /// compatibility and does not affect the pipeline.
  void runOptimizationPipeline(Module &M, OptimizationLevel level,
                               CompileTier tier);

  /// Pick the cached function-simplification FPM for an EJIT optimization tier.
  FunctionPassManager &simplifyFPMForLevel(OptimizationLevel level);

  PeriodArrayRegistry &registry_;

  // Persistent analysis managers — registered once, reused across compilations.
  // Invalidated per-function by the pass infrastructure as needed.
  LoopAnalysisManager LAM_;
  FunctionAnalysisManager FAM_;
  CGSCCAnalysisManager CGAM_;
  ModuleAnalysisManager MAM_;

  // Cached pass managers, built once and reused across compilations:
  //   lowerExpectFPM_ lower llvm.expect (not in buildFunctionSimplification
  //                   Pipeline); runs before the O2 pipeline (Phase 2).
  //   simplifyO1/2/3_ the real LLVM -O1/-O2/-O3 function-simplification pipeline
  //                   (Phase 3), one per tier; NO vectorization.
  //   cleanupFPM_     light fold after the second StructFieldPass (Phase 5).
  FunctionPassManager lowerExpectFPM_;
  FunctionPassManager simplifyO1_;
  FunctionPassManager simplifyO2_;
  FunctionPassManager simplifyO3_;
  FunctionPassManager cleanupFPM_;
  // Tier-2-only profile-guided memory-operation specialization. The main
  // O1/O2/O3 simplification pipeline already contains profile-aware unrolling.
  FunctionPassManager pgoUseFPM_;

  // PGO: PGOFuncNames captured by the last Tier-1 compile (see
  // captureCounterGlobals). Cleared at the start of each runPipeline.
  SmallVector<std::string, 4> lastCounterNames_;

  // Grant the unit-test accessor visibility into the private pipeline steps.
  // runPipeline() remains the only production entry point; this friend keeps
  // the per-step API private while letting tests exercise steps in isolation.
  friend struct EJitOptimizerTestAccess;
};

} // namespace ejit
} // namespace llvm

#endif
