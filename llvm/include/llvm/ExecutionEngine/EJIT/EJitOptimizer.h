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
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTable.h"
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#endif
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LoopAnalysisManager.h"
#include "llvm/ExecutionEngine/EJIT/EJitPassBuilder.h"
#include "llvm/IR/Module.h"
#include <memory>

namespace llvm {
namespace ejit {

struct EJitMayConstLoadSite;

/// One function of the last Tier-1 compile, captured for the value-profile
/// merge: its symbol name, the MD5 hash of its IR-level PGO name (the same
/// value LLVM's IndirectCallPromotion resolves through the module symtab),
/// and how many scalar/loop-bound value sites the Tier-1 instrumentation
/// created in it (zero for functions without sites).
struct EJitVpFunctionInfo {
  std::string name;
  uint64_t pgoHash = 0;
  uint32_t numScalarSites = 0;
};

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

  /// Install the small-table plan set the optimizer consults (PR231 §6.5/§6.6).
  /// Default is an empty set: the small-table pass is then never constructed
  /// and every compile takes the unmodified baseline pipeline. The optimizer
  /// keeps the shared_ptr alive for the lifetime of the plan lookup; plans are
  /// immutable value snapshots. Production wiring of this seam (plan
  /// construction from a confirmed-ready member set, admission validation and
  /// row publication) is a later milestone; the host tests drive it directly.
  void setSmallTablePlans(std::shared_ptr<const EJitSmallTablePlanSet> Plans) {
    smallTablePlans_ = std::move(Plans);
  }
  const EJitSmallTablePlanSet *getSmallTablePlans() const {
    return smallTablePlans_.get();
  }

  /// PGO counter global names captured during the last Instrumented (Tier-1)
  /// compile (PGOFuncName suffix of each __profc_<name>). Empty for
  /// Baseline/PGOUse. The compile driver looks up __profc_/__profd_ by these
  /// names to capture counter addresses for Tier-2 profile synthesis.
  ArrayRef<std::string> getLastCounterNames() const {
    return lastCounterNames_;
  }

  /// Names of the small-table column globals materialized by the last compile
  /// (PR231 §6.5). The globals are created inside the IR transform, after
  /// addIRModule, so the materialization responsibility computed from the
  /// original module does not include them; the engine claims these names as
  /// exported exactly like the PGO counters, which is what lets the runtime
  /// resolve each table's stable address and publish later rows into it.
  /// Empty when no plan is installed (feature OFF) or for a uniform-only plan.
  ArrayRef<std::string> getLastSmallTableColumnNames() const {
    return lastSmallTableColumns_;
  }

  /// Value-profile capture of the last Instrumented (Tier-1) compile: every
  /// function of the module with its symbol name, IR-PGO-name MD5 hash and
  /// scalar site count (EJIT_VALUE_PROFILE.md §5.1). The compile driver
  /// resolves each name to its runtime address to build the verified
  /// address -> hash map used by the Tier-2 merge. Empty for Baseline/PGOUse.
  ArrayRef<EJitVpFunctionInfo> getLastVpFunctions() const {
    return lastVpFunctions_;
  }

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  ArrayRef<EJitMayConstLoadSite> getLastMayConstLoadSites() const {
    return lastMayConstLoadSites_;
  }
#endif

  /// Record the scalar/loop-bound value-site count the Tier-1 instrumentation
  /// created in \p funcName (called by the value-profile instrumentation pass
  /// before captureCounterGlobals merges the counts into lastVpFunctions_).
  void recordScalarSiteCount(StringRef funcName, uint32_t count) {
    scalarSiteCountsByFunc_[funcName] = count;
  }

  /// Print a descending per-entry ranking of dynamically removed may_const
  /// loads per million sample ticks, with per-entry and active-site context.
  /// Returns false when no completed specialization sample is available.
  bool printMayConstRanking() const;

private:
  /// Replace ejit_period_arr_ind parameters with their runtime constants.
  void preReplacePeriodIndices(Module &M, const SpecializationContext &ctx);

  /// Run InstCombine on all functions (single pass).
  void runInstCombine(Module &M);

  /// Run EJitStructFieldPass on all functions.
  void runStructFieldPass(Module &M);
  void runStructFieldPass(Module &M, const SpecializationContext &ctx);
  /// The existing may_const replacement without re-entering the small-table
  /// pass. The context overload runs the small-table pass first; the no-context
  /// overload (phase 4, after `runSmallTablePass(M, EntryName)`) must not run it
  /// again, or it would clear the recorded column names with an empty entry.
  /// \p BlockLegacyFoldEntry, when non-empty, names the entry whose legacy
  /// compile-time may_const fold must be blocked because a small-table plan was
  /// lowered for it in this round (PR231 whole-entry readiness contract).
  void runStructFieldPassImpl(Module &M, const SpecializationContext &ctx,
                              StringRef BlockLegacyFoldEntry = {});

  /// Run the small-table pass for \p EntryName, if a plan is installed for it.
  /// Called immediately before the existing struct-field pass in every replace
  /// round, so all three rounds see the table form and no round can regress it.
  /// Returns true when a plan was materialized (lowered) for this entry; the
  /// caller then blocks the legacy compile-time fold for that entry, so the
  /// compiled code depends only on the plan's recorded contract and on real
  /// dynamic reads.
  bool runSmallTablePass(Module &M, StringRef EntryName);

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
  /// constant-index GEPs, then does a final cleanup. `level` is accepted for
  /// ABI compatibility and does not affect the pipeline.
  void runOptimizationPipeline(Module &M, OptimizationLevel level,
                               CompileTier tier);
  void runOptimizationPipeline(Module &M, OptimizationLevel level,
                               CompileTier tier, StringRef EntryName);

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  void recordMayConstBenefit(const SpecializationContext &ctx,
                             ArrayRef<EJitMayConstLoadSite> inputSites,
                             uint64_t specializedMayConstLoads,
                             ArrayRef<EJitMayConstLoadSite> finalSites,
                             uint64_t sampledEntries);
#endif

  /// Pick the cached function-simplification FPM for an EJIT optimization tier.
  FunctionPassManager &simplifyFPMForLevel(OptimizationLevel level);

  PeriodArrayRegistry &registry_;

  /// Small-table plans (PR231). Empty by default: the feature is OFF and no
  /// pass is constructed, so the baseline pipeline is unchanged.
  std::shared_ptr<const EJitSmallTablePlanSet> smallTablePlans_;

  /// Column globals created by the last small-table materialization (PR231);
  /// the engine claims them in the materialization responsibility.
  SmallVector<std::string, 8> lastSmallTableColumns_;

  // Persistent analysis managers — registered once, reused across compilations.
  // Invalidated per-function by the pass infrastructure as needed.
  LoopAnalysisManager LAM_;
  FunctionAnalysisManager FAM_;
  CGSCCAnalysisManager CGAM_;
  ModuleAnalysisManager MAM_;

  // Cached pass managers, built once and reused across compilations:
  //   lowerExpectFPM_ lower llvm.expect (not in buildFunctionSimplification
  //                   Pipeline); runs before the O2 pipeline (Phase 2).
  //   simplifyO1/2/3_ the real LLVM -O1/-O2/-O3 function-simplification
  //   pipeline
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
  // Value profile (EJIT_VALUE_PROFILE.md §5.1): function table captured by the
  // last Tier-1 compile. Cleared at the start of each runPipeline. The scalar
  // instrumentation pass records per-function site counts into
  // scalarSiteCountsByFunc_ before capture; captureCounterGlobals merges them.
  SmallVector<EJitVpFunctionInfo, 8> lastVpFunctions_;
  StringMap<uint32_t> scalarSiteCountsByFunc_;

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  SmallVector<EJitMayConstLoadSite, 16> lastMayConstLoadSites_;
#endif

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  StringMap<DenseMap<uint64_t, EJitMayConstBenefitSample>>
      mayConstBenefitSamples_;
  mutable EJitAtomicU32 mayConstBenefitLock_{0};
#endif

  // Grant the unit-test accessor visibility into the private pipeline steps.
  // runPipeline() remains the only production entry point; this friend keeps
  // the per-step API private while letting tests exercise steps in isolation.
  friend struct EJitOptimizerTestAccess;
};

} // namespace ejit
} // namespace llvm

#endif
