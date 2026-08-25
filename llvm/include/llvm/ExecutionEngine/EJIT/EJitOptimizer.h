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

class TargetMachine;

namespace ejit {

/// JIT optimization pipeline. Runs on the extracted bitcode module during
/// JIT compilation to specialize the code for the current time-window values.
/// Holds persistent AnalysisManagers to avoid re-registering analyses on
/// every compilation.
class EJitOptimizer {
public:
  /// \p TM feeds PassBuilder's TargetIRAnalysis: with it, TTI is
  /// backend-accurate (real vector register widths) and the Phase 5
  /// vectorizers can fire; without it the default TTI reports a 32-bit
  /// baseline, which disables vectorization (and skews loop-unroll cost
  /// models). The engine passes the same JITTargetMachineBuilder-derived TM
  /// the JIT compiles with.
  EJitOptimizer(PeriodArrayRegistry &reg, TargetMachine *TM = nullptr);

  /// Run the full JIT specialization pipeline:
  ///   1a. Parameter substitution (ejit_period_arr_ind → constants)
  ///   1b. InstCombine (fold GEP chains from substituted params)
  ///   1c. StructFieldPass (may_const loads → runtime constants)
  ///   1d. IPSCCP (push constants across call edges)
  ///   1e/1f. InstCombine + StructFieldPass for callees
  ///   1g. Module cleanup (RPO attrs + dead-arg elimination + GlobalDCE)
  ///   2-4. LowerExpect → O1/O2/O3 simplification → re-substitution + cleanup
  ///   5. Vectorization (L2+)
  ///   6. Final GlobalDCE sweep (callees freed by phases 2-5)
  void runPipeline(Module &M, const SpecializationContext &ctx);

  /// Clear all cached analysis results. Must be called between compilations
  /// to avoid dangling pointers to IR units from previous modules.
  void clearAnalyses();

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

  /// Module-level cleanup after specialization (Phase 1g). Runs on every tier:
  /// RPO function-attribute inference feeds downstream folds, dead-argument
  /// elimination drops parameters IPSCCP replaced with constants (their call
  /// sites stop computing them), and GlobalDCE deletes functions whose call
  /// sites the folded guards deleted — shrinking what the JIT backend must
  /// compile. DAE only rewrites local-linkage functions, so the external
  /// ejit_entry is untouched.
  void runModuleCleanup(Module &M);

  /// Post-specialization vectorization (Phase 5). Runs after the final
  /// StructFieldPass so the vectorizers see the fully-specialized loops.
  /// L2: SLP + partial loop unrolling; L3 adds the loop vectorizer. L1 skips
  /// vectorization entirely.
  void runVectorization(Module &M, OptimizationLevel level);

  /// Run the EJIT optimization pipeline: a single fused sequence that exploits
  /// the just-substituted period-index / may_const constants to their fixed
  /// point (scalar fold/propagate/simplify), folds loops whose bounds became
  /// constant, re-specializes the array accesses that unrolling turns into
  /// constant-index GEPs, then does a final cleanup. `level` selects the
  /// function-simplification tier (L1→O1, L2→O2, L3→O3) and gates the
  /// Phase 5 vectorization (L2+).
  void runOptimizationPipeline(Module &M, OptimizationLevel level);

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
  //   cleanupFPM_     light fold after the second StructFieldPass (Phase 4).
  //   cleanupMPM_     RPO attrs + DAE + GlobalDCE module cleanup (Phase 1g).
  //   vectorizeL2/3_  post-specialization vectorization (Phase 5); L2 has SLP +
  //                   partial unrolling, L3 adds the loop vectorizer.
  //   finalDCEMPM_    final GlobalDCE (Phase 6): phases 2-5 delete call sites
  //                   (expect-guard folding, unrolled-loop leftovers) that
  //                   phase 1g's DCE could not yet see as dead, so sweep the
  //                   now-unreferenced callees before the backend compiles.
  FunctionPassManager lowerExpectFPM_;
  FunctionPassManager simplifyO1_;
  FunctionPassManager simplifyO2_;
  FunctionPassManager simplifyO3_;
  FunctionPassManager cleanupFPM_;
  ModulePassManager cleanupMPM_;
  FunctionPassManager vectorizeL2_;
  FunctionPassManager vectorizeL3_;
  ModulePassManager finalDCEMPM_;

  // Grant the unit-test accessor visibility into the private pipeline steps.
  // runPipeline() remains the only production entry point; this friend keeps
  // the per-step API private while letting tests exercise steps in isolation.
  friend struct EJitOptimizerTestAccess;
};

} // namespace ejit
} // namespace llvm

#endif
