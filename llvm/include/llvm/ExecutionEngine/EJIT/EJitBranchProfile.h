//===-- EJitBranchProfile.h - Online-PGO branch audit -----------*- C++ -*-===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H

#include "llvm/ADT/ArrayRef.h"

#include <cstdint>
#include <string>
#include <vector>

namespace llvm {

class Module;

namespace ejit {

struct EJitBranchProfileSummary {
  std::string functionName;
  uint64_t entryCount = 0;
  uint64_t instructionCount = 0;
  uint32_t branchSites = 0;
  uint32_t profiledSites = 0;
  uint32_t biasedSites95 = 0;
  uint32_t balancedSites60 = 0;
  uint32_t zeroCountEdges = 0;
  bool isRoot = false;
};

/// Stable description of one source period_const load. Tier-1 records the same
/// deterministic module-order inventory that Tier-2 uses to correlate runtime
/// hits with the final specialized IR.
struct EJitPeriodConstLoadSite {
  std::string functionName;
  std::string globalName;
  std::string sourceFile;
  /// Stable module-order identity attached to the load as IR metadata before
  /// optimization. Zero means that provenance could not be recovered.
  uint64_t siteId = 0;
  uint64_t fieldOffset = 0;
  uint64_t runtimeHits = 0;
  uint32_t sourceLine = 0;
  uint32_t sourceColumn = 0;
  bool hasFieldOffset = false;
};

struct EJitPeriodConstBenefitSample {
  uint64_t cacheKey = 0;
  uint64_t inputPeriodConstLoads = 0;
  uint64_t specializedPeriodConstLoads = 0;
  uint64_t finalPeriodConstLoads = 0;
  uint64_t runtimeHits = 0;
  uint64_t hitSites = 0;
  uint64_t removedRuntimeHits = 0;
  uint64_t removedHitSites = 0;
  uint64_t sampledEntries = 0;
  uint64_t sampleCycles = 0;
};

struct EJitPeriodConstBenefitSummary {
  uint64_t versions = 0;
  uint64_t inputPeriodConstLoads = 0;
  uint64_t specializedPeriodConstLoads = 0;
  uint64_t finalPeriodConstLoads = 0;
  int64_t totalRemoved = 0;
  int64_t directRemoved = 0;
  int64_t pipelineRemoved = 0;
  int64_t averageRemoved = 0;
  int64_t weightedRemovedPermille = 0;
  int64_t minimumRemoved = 0;
  int64_t maximumRemoved = 0;
  uint64_t runtimeHits = 0;
  uint64_t hitSites = 0;
  uint64_t removedRuntimeHits = 0;
  uint64_t removedHitSites = 0;
  uint64_t sampledEntries = 0;
  uint64_t sampleCycles = 0;
  /// Average dynamically-reached period_const sites per unique JIT version,
  /// scaled by 1000 so freestanding diagnostics need no floating point.
  uint64_t averageActiveSitesPermille = 0;
  /// Removed dynamic load executions per sampled root entry, scaled by 1000.
  uint64_t removedHitsPerEntryPermille = 0;
  /// Removed dynamic load executions per million platform timestamp units,
  /// scaled by 1000 for three decimal places.
  uint64_t benefitPerMillionCyclesMilli = 0;
};

/// Summarize the branch weights attached by PGOInstrumentationUse. This is a
/// read-only audit: it does not alter IR or add runtime instrumentation.
std::vector<EJitBranchProfileSummary>
analyzeBranchProfiles(const Module &M, const std::string &rootName);

/// Aggregate unique specialization samples for one EJIT entry.
EJitPeriodConstBenefitSummary
summarizePeriodConstBenefits(ArrayRef<EJitPeriodConstBenefitSample> Samples);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H
