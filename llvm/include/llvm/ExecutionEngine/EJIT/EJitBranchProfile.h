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

/// Stable description of one source may_const load. Tier-1 records the same
/// deterministic module-order inventory that Tier-2 uses to correlate runtime
/// hits with the final specialized IR.
struct EJitMayConstLoadSite {
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

struct EJitMayConstBenefitSample {
  uint64_t cacheKey = 0;
  uint64_t inputMayConstLoads = 0;
  uint64_t specializedMayConstLoads = 0;
  uint64_t finalMayConstLoads = 0;
  uint64_t runtimeHits = 0;
  uint64_t hitSites = 0;
  uint64_t removedRuntimeHits = 0;
  uint64_t removedHitSites = 0;
  uint64_t sampledEntries = 0;
  uint64_t sampleCycles = 0;
  /// Finalized executable bytes published for this JIT version.
  uint64_t publishedHotCodeBytes = 0;
  /// True only when publishedHotCodeBytes came from a validated fnPtr/fnSize.
  bool publishedHotCodeSizeValid = false;
  /// Fingerprints of non-zero 64-byte executable lines. Used only by the
  /// diagnostic cross-version Partial JIT audit.
  std::vector<uint64_t> publishedHotLineFingerprints;
};

struct EJitMayConstBenefitSummary {
  uint64_t versions = 0;
  uint64_t inputMayConstLoads = 0;
  uint64_t specializedMayConstLoads = 0;
  uint64_t finalMayConstLoads = 0;
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
  uint64_t publishedHotCodeBytes = 0;
  uint64_t validPublishedHotCodeVersions = 0;
  bool entryBenefitDensityValid = false;
  /// Sum of per-version ceil(publishedHotCodeBytes / 64).
  uint64_t publishedHotICacheLines = 0;
  /// Non-zero executable lines included in the cross-version audit.
  uint64_t fingerprintedHotICacheLines = 0;
  /// Line instances whose fingerprint occurs in at least two JIT versions.
  uint64_t crossVersionMatchingICacheLines = 0;
  /// Matching line instances beyond one retained copy per fingerprint.
  uint64_t partialJitCandidateICacheLines = 0;
  /// partialJitCandidateICacheLines / publishedHotICacheLines, permille.
  uint64_t partialJitCandidatePermille = 0;
  /// Average dynamically-reached may_const sites per unique JIT version,
  /// scaled by 1000 so freestanding diagnostics need no floating point.
  uint64_t averageActiveSitesPermille = 0;
  /// Removed dynamic load executions per sampled root entry, scaled by 1000.
  uint64_t removedHitsPerEntryPermille = 0;
  /// Removed dynamic load executions per million platform timestamp units,
  /// scaled by 1000 for three decimal places.
  uint64_t benefitPerMillionCyclesMilli = 0;
  /// benefitPerMillionCycles divided by published hot I-cache lines, scaled
  /// by 1000 for three decimal places.
  uint64_t entryBenefitDensityMilli = 0;
};

/// Summarize the branch weights attached by PGOInstrumentationUse. This is a
/// read-only audit: it does not alter IR or add runtime instrumentation.
std::vector<EJitBranchProfileSummary>
analyzeBranchProfiles(const Module &M, const std::string &rootName);

/// Aggregate unique specialization samples for one EJIT entry.
EJitMayConstBenefitSummary
summarizeMayConstBenefits(ArrayRef<EJitMayConstBenefitSample> Samples);

/// Fingerprint non-zero 64-byte executable lines for the Partial JIT audit.
std::vector<uint64_t>
fingerprintPublishedHotICacheLines(ArrayRef<uint8_t> CodeBytes);

/// Validate that fnPtr/fnSize names a complete function body inside its
/// finalized executable allocation. Out is empty on failure and no bytes are
/// read, which keeps RW/NX, missing-size, and neighboring allocations out of
/// the ranking audit.
bool getPublishedFunctionBytes(const void *FnPtr, uint64_t FnSize,
                               uintptr_t AllocationStart,
                               uint64_t AllocationSize, ArrayRef<uint8_t> &Out);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITBRANCHPROFILE_H
