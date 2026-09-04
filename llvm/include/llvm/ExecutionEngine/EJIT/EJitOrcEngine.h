//===-- EJitOrcEngine.h - OrcJIT Engine Wrapper ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITORCENGINE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITORCENGINE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodeRange.h"
#include "llvm/ExecutionEngine/EJIT/EJitBoundPtr.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#endif
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"
#include <array>
#include <memory>
#include <string>
#include <vector>

#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#endif

namespace llvm {
class Module;

namespace ejit {

#ifdef EJIT_SRE_CODE_POOL
struct EJitTieredCodePoolStats {
  EJitCodePoolManager::Stats total;
  EJitCodePoolManager::Stats near;
  std::array<EJitCodePoolManager::Stats, kEJitNearHotPoolCount> nearHot;
  EJitCodePoolManager::Stats far;
};
#endif

class PeriodArrayRegistry;
class EJitRuntimeState;
struct EJitSharedTaskPoolState;
struct EJitVpFunctionInfo; // defined in EJitOptimizer.h (value-profile capture)
enum class CompileTier : uint8_t;

namespace detail {
/// Render one function definition without the rest of its specialization
/// module. Exposed for focused dump-path testing; callers should use the
/// ejit_dump_* APIs.
bool renderDumpFunctionIR(const Module &M, StringRef fnName, std::string &out);
void renderDumpModuleIR(const Module &M, std::string &out);
/// Return whether a filtered dump should capture this compilation tier.
/// Instrumented Tier-1 is temporary and must never replace a final dump.
bool shouldCaptureDump(CompileTier tier, StringRef filter, StringRef fnName);
} // namespace detail

/// Set a function-name filter for JIT IR+ASM diagnostic capture. When non-
/// empty, the engine captures (saves in memory) the post-optimization IR and
/// emitted assembly for both the matching entry function and its complete
/// specialization module, the next time it is compiled. "*" matches every
/// specialization. With staged PGO, temporary Instrumented code is skipped and
/// the final Tier-2 code is captured. Empty/null disables further capture
/// (already-captured entries are retained). Use printDumped() for the entry-only
/// view and printDumpedModule() for the complete module.
void setDumpFuncFilter(const std::string &name);

/// Bind the optional shared taskpool dump state. In a shared-taskpool build
/// this lets any core set the dump filter and discover which worker core owns
/// the latest capture. Full IR/ASM payloads remain in the worker-local dump
/// store. Passing nullptr disables shared filter and ownership metadata.
void setDumpSharedState(EJitSharedTaskPoolState *state);

/// Print the saved IR+ASM for \p name (or all saved entries when \p name is
/// null/empty) through EJIT_DIAG, one line per IR/ASM line. Names with no
/// saved capture are reported as missing. Paired with setDumpFuncFilter():
/// capture at compile time, print selectively later.
/// Returns true when a local payload was printed or matching remote-owner
/// metadata was found.
bool printDumped(const char *name);

/// Print the complete specialization module captured for \p name (or every
/// saved module when \p name is null/empty). Payloads are worker-local; unlike
/// printDumped(), this function does not consult cross-core metadata.
bool printDumpedModule(const char *name);

/// Compile tier for online PGO (EJIT_ONLINE_PGO.md §3). Baseline is the
/// existing no-PGO pipeline; the other two are opt-in via Config::enablePgo.
enum class CompileTier : uint8_t {
  Baseline = 0,     ///< No PGO: specialize + opt pipeline (current behavior).
  Instrumented = 1, ///< Tier-1: specialize + PGOInstrumentationGen + Lowering.
  PGOUse = 2, ///< Tier-2: specialize + PGOInstrumentationUse(profile) + opts.
};

struct SpecializationContext {
  std::string fnName;
  uint64_t cacheKey = 0;
  struct DimInfo {
    /// Empty for a const dim, which names no lifecycle and is instead
    /// identified by argIndex.
    std::string periodName;
    uint8_t cellIdx;
    /// The lifecycle slot read from registration metadata. The invalid
    /// sentinel is retained so fixed near-hot routing can reject malformed
    /// metadata instead of silently placing it in the public pool. A const
    /// dim names no lifecycle, so it keeps the sentinel.
    uint32_t dimType = 0xFFFFFFFFu;
    bool isConst = false;
    unsigned argIndex = 0;
  };
  SmallVector<DimInfo, 4> dimensions;
  /// Borrowed bound-pointer views. This vector is used only during the
  /// compile callback and never owns or frees the pointed-to objects.
  SmallVector<EJitBoundPointerView, kEJitMaxBoundPointers> boundPointers;
  OptimizationLevel optLevel = OptimizationLevel::L2;
  /// PGO tier (Baseline when PGO is disabled or for the first compile).
  CompileTier tier = CompileTier::Baseline;
  /// Tier-2 indexed profile buffer (synthesized from Tier-1 counters by
  /// EJitProfileMerge before loadBitcode). Empty for Baseline/Instrumented.
  /// Owned by the context; lives through the JIT transform that consumes it.
  std::string profileData;
  /// Scalar/loop-bound specialization side table (EJIT_VALUE_PROFILE.md §7):
  /// filled by the Tier-2 merge with the top-1 dominant value per qualifying
  /// site (min samples + confidence thresholds applied by the driver). Empty
  /// for Baseline/Instrumented and when value profiling is not built.
  std::vector<PgoScalarSite> scalarValueSites;
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
  /// Runtime hit snapshot from the temporary Instrumented tier. Populated by
  /// the compile driver before the final compile starts.
  std::vector<EJitMayConstLoadSite> mayConstLoadSites;
  /// Platform timestamp distance from Tier-1 counter capture to the Tier-2
  /// snapshot. SRE uses cycle counter ticks; hosts use steady-clock ns.
  uint64_t mayConstSampleCycles = 0;
  /// True when profile data is collected for diagnostics only. The optimizer
  /// restores weights for reporting but must publish ordinary Baseline code.
  bool profileAuditOnly = false;
#endif
};

/// Wraps an LLJIT instance with EmbeddedJIT-specific configuration:
/// custom memory manager and IR transform layer for the JIT pipeline.
class EJitOrcEngine {
public:
  EJitOrcEngine();
  ~EJitOrcEngine();

  /// Create a configured engine. Thread-safe to call once.
  static Expected<std::unique_ptr<EJitOrcEngine>>
  Create(const Config &config, PeriodArrayRegistry &periodReg,
         EJitRuntimeState &runtimeState);

  /// Load a bitcode module into a per-specialization JITDylib identified
  /// by cacheKey. Each specialization gets its own JITDylib so symbols
  /// from the same TU bitcode can be defined multiple times without conflict.
  Error loadBitcodeModule(StringRef bitcodeData, uint64_t cacheKey,
                          const std::string &origFnName,
                          uint32_t poolId = kEJitNearHotPublicPoolId);

  /// Look up a compiled function symbol in the specialization JITDylib
  /// identified by cacheKey.
  Expected<void *> lookup(uint64_t cacheKey, const std::string &name);

  /// Set the active specialization context (used during compilation).
  void setActiveContext(const SpecializationContext *ctx);
  const SpecializationContext *getActiveContext() const;

  /// PGO: PGOFuncNames captured by the last Tier-1 compile (the suffix of each
  /// __profc_<name> that captureCounterGlobals forced external). The compile
  /// driver looks up __profc_/__profd_ by these names after a Tier-1 compile to
  /// capture counter addresses for Tier-2 profile synthesis (§5.2).
  ArrayRef<std::string> getLastCounterNames() const;

  /// Value profile: function table captured by the last Tier-1 compile (see
  /// EJitOptimizer::getLastVpFunctions). Empty unless the Tier-1 compile ran
  /// with EJIT_SRE_PGO_VALUE_PROFILE.
  ArrayRef<EJitVpFunctionInfo> getLastVpFunctions() const;

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  ArrayRef<EJitMayConstLoadSite> getLastMayConstLoadSites() const;
#endif

  /// Print completed per-entry may_const benefit samples, sorted by average
  /// runtime-active sites per specialization.
  bool printMayConstRanking() const;

  /// Attach finalized executable bytes to a completed may_const sample.
  bool recordMayConstPublishedCode(const std::string &Entry, uint64_t CacheKey,
                                   const void *CodeStart, uint64_t CodeBytes);

  /// Register a user-defined external symbol (function or global) that the
  /// JIT can resolve when compiling bitcode modules. Required for bare-metal
  /// environments where dynamic symbol lookup is unavailable.
  void addUserSymbol(const std::string &name, void *addr);

#ifdef EJIT_SRE_CODE_POOL
  /// Snapshot of the SRE code-pool statistics (pool / sealed counts, used /
  /// wasted bytes, enable_ex invocations) for diagnostics and tests. Returns a
  /// zeroed snapshot if no pool is active. Available only with
  /// EJIT_SRE_CODE_POOL.
  EJitCodePoolManager::Stats getCodePoolStats() const;

  /// Snapshot aggregate and placement-specific statistics. Tier-1 uses the
  /// far dynamic pool; final Baseline/Tier-2 code uses the near fixed pool.
  EJitTieredCodePoolStats getTieredCodePoolStats() const;

  /// Resolve a compiled function pointer to its real, finalized executable
  /// range + owning code pool (for cross-core 4K execute-permission
  /// preparation). Returns false if \p FnPtr is not pool-backed code with a
  /// recorded finalized range. Available only with EJIT_SRE_CODE_POOL.
  bool findCodeRange(const void *FnPtr, EJitCompiledCodeInfo &Out) const;
  bool findPendingCodeRange(const void *FnPtr, EJitCompiledCodeInfo &Out) const;
  bool isCodeReady(const void *FnPtr) const;
  Error flushPendingCode(uint32_t poolId = 0xFFFFFFFFu);
#endif

private:
  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ejit
} // namespace llvm

#endif
