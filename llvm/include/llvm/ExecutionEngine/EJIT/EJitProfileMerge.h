//===-- EJitProfileMerge.h - in-memory PGO profile synthesis --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Synthesizes an indexed InstrProf profile buffer in memory from Tier-1's
// captured counter addresses, for consumption by Tier-2's
// PGOInstrumentationUse via an InMemoryFileSystem (EJIT_ONLINE_PGO.md §5.3).
// No file I/O, no compiler-rt runtime: reads __profc_*/__profd_* directly and
// hands records to InstrProfWriter.
//
// With EJIT_SRE_PGO_VALUE_PROFILE this is extended to value profiling
// (EJIT_VALUE_PROFILE.md): the same writer carries edge counters AND per-site
// InstrProfValueData (indirect-call targets, dynamic memop sizes) in ONE legal
// profile. Indirect-call target values are runtime addresses in the collector
// shards; aggregateValueSamples maps them to MD5 hashes of the target's
// IR-level PGO name (the representation LLVM's IndirectCallPromotion resolves
// through the module symtab) and DROPS values it cannot verify — a raw address
// is never written into the profile as if it were a function hash.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITPROFILEMERGE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITPROFILEMERGE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ProfileData/InstrProf.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#endif

//===----------------------------------------------------------------------===//
// Scalar/loop-bound specialization policy (EJIT_VALUE_PROFILE.md §7.2): a site
// is specialized only with at least EJIT_SRE_VP_MIN_SAMPLES observations and a
// top-1 dominance of EJIT_SRE_VP_MIN_CONF_PERCENT percent (top1/total). Applied
// by the compile driver when building the side table, and re-checked by the
// Tier-2 transform as defense in depth. Overridable by the build via -D.
//===----------------------------------------------------------------------===//
#ifndef EJIT_SRE_VP_MIN_SAMPLES
#define EJIT_SRE_VP_MIN_SAMPLES 100u
#endif
#ifndef EJIT_SRE_VP_MIN_CONF_PERCENT
#define EJIT_SRE_VP_MIN_CONF_PERCENT 99u
#endif

namespace llvm {
namespace ejit {

/// Forward declaration: only a pointer to the fixed-layout Tier-2 request is
/// needed here, so this header stays independent of the queue header/layout.
struct EJitCompileRequest;

/// A Tier-1 captured counter reference: the PGO function name (suffix of
/// __profc_<pgoName>) and the raw addresses of the counter/data globals that
/// captureCounterGlobals recorded + forced external.
struct PgoCounterRef {
  const char *pgoName = nullptr;
  uintptr_t profcAddr = 0; ///< __profc_<pgoName>: i64 counter array
  uintptr_t profdAddr = 0; ///< __profd_<pgoName>: __llvm_profile_data struct
};

/// Aggregated value data for one official value site (kind =
/// IPVK_IndirectCallTarget or IPVK_MemOPSize), sorted by count descending.
struct PgoValueSite {
  uint64_t pgoNameHash = 0;
  uint32_t valueKind = 0; ///< IPVK_*
  uint32_t siteIndex = 0;
  std::vector<InstrProfValueData> values;
};

/// Top-1 scalar/loop-bound specialization request for Tier-2 (side table; LLVM
/// value kinds are fixed to IPVK_* so scalar data cannot ride the official
/// profile - EJIT_VALUE_PROFILE.md §7).
struct PgoScalarSite {
  uint64_t funcHash = 0;
  uint32_t siteIndex = 0;
  uint64_t topValue = 0;
  uint64_t topCount = 0;
  uint64_t total = 0;
};

/// Per-function value-site inventory used to probe the collector snapshot by
/// re-computed site keys. Every value kind uses the function-unique IR-PGO
/// NameRef hash; FuncHash remains the CFG hash required by InstrProf.
/// numIcSites/ numMemSites come from the __profd_ NumValueSites[] at merge
/// time; numScalarSites is captured by the optimizer during the Tier-1 compile.
struct PgoValueFunction {
  uint64_t funcHash = 0;    ///< CFG hash required by InstrProf
  uint64_t pgoNameHash = 0; ///< function-unique key for all value sites
  uint32_t numIcSites = 0;
  uint32_t numMemSites = 0;
  uint32_t numScalarSites = 0;
};

/// Exact schema captured with a representative Tier-1 and checked before a
/// frozen profile is consumed by another logical member. The PGO name is kept
/// alongside both hashes because neither hash is accepted as proof of equality.
struct PgoFunctionSchema {
  std::string pgoName;
  uint64_t funcHash = 0;
  uint64_t pgoNameHash = 0;
  uint32_t numCounters = 0;
  uint32_t numIcSites = 0;
  uint32_t numMemSites = 0;
  uint32_t numScalarSites = 0;
};

enum class ProfileSnapshotQuality : uint8_t {
  Complete,
  ApproximateInFlight,
  EdgeOnly,
  ValueDataDropped,
};

/// Quality of the observed Tier-1 dispatch boundary carried by a profile
/// bundle. It is deliberately separate from the snapshot quality: the snapshot
/// says how complete the collected profile is, this says whether the dispatch
/// window that produced it was actually observed.
enum class T1DispatchObservationQuality : uint8_t {
  /// No trustworthy observation (legacy/tokenless mode, non-shared pool, or a
  /// request whose identity no longer matches the profile). count/quotaEnd are
  /// reported as 0; the configured threshold is never substituted.
  Unavailable = 0,
  /// The observed dispatch count reached the admission limit and the final
  /// allowed dispatch was observed. quotaEnd holds the frozen timestamp, or 0
  /// when no clock was configured (timestamp unknown).
  FrozenAtQuota = 1,
  /// The count was observed but admission had not closed when the bundle was
  /// built (in-flight/approximate window). quotaEnd is 0: the boundary is not
  /// known yet and must not be inferred from the compile time.
  PartialOpenQuota = 2,
};

/// Immutable handoff from one representative sampling session to every Tier-2
/// consumer in its candidate group. Ownership is shared and const after
/// construction; no consumer borrows the representative's temporary vectors.
struct EJitProfileBundle {
  uint64_t groupId = 0;
  uint64_t groupGeneration = 0;
  uint64_t profileEpoch = 0;
  uint64_t samplingSessionId = 0;
  uint64_t representativeLogicalKey = 0;
  uint64_t representativeAttemptToken = 0;
  /// Observed real Tier-1 dispatches for the representative attempt: only a
  /// granted Tier-1 pointer return counts. 0 when unavailable.
  uint64_t actualDispatchCount = 0;
  /// Timestamp of the final allowed dispatch (the end of dispatch admission).
  /// 0 means unknown; it is never the Tier-2 queue or compile time.
  uint64_t quotaEnd = 0;
  /// When the bounded snapshot finished (schema validated). A deliberately
  /// different instant from quotaEnd: queue wait and compilation are not part
  /// of the dispatch window.
  uint64_t freezeCompletedAt = 0;
  /// Admission quota the observed count was measured against (0 = unavailable).
  uint64_t dispatchLimit = 0;
  /// Whether actualDispatchCount/quotaEnd are real observations.
  T1DispatchObservationQuality dispatchQuality =
      T1DispatchObservationQuality::Unavailable;
  ProfileSnapshotQuality quality = ProfileSnapshotQuality::ApproximateInFlight;
  bool hasEdgeProfile = false;
  bool valueProfileEnabled = false;
  bool valueProfileComplete = false;
  std::string indexedProfile;
  std::vector<PgoScalarSite> scalarSites;
  std::vector<PgoFunctionSchema> schema;
  struct VerifiedTarget {
    uintptr_t address = 0;
    uint64_t pgoNameHash = 0;
  };
  std::vector<VerifiedTarget> verifiedTargets;
};

using EJitFrozenProfileBundle = std::shared_ptr<const EJitProfileBundle>;

/// Synthesize an indexed profile buffer from captured Tier-1 counters.
/// Returns an empty string on failure (caller skips Tier-2 / falls back to
/// Tier-1). Reads the __llvm_profile_data layout via InstrProfData.inc
/// (same LLVM build -> identical layout) for FuncHash + NumCounters + the
/// counter values at profcAddr. When \p valueSites is non-empty the same
/// buffer carries the value profile records (edge + value in one profile).
std::string synthesizeProfileBuffer(ArrayRef<PgoCounterRef> counters,
                                    ArrayRef<PgoValueSite> valueSites);

/// Capture the exact counter/value-site schema paired with a Tier-1 object.
/// Returns false on malformed runtime data; callers must not freeze a bundle
/// when this validation fails.
bool readProfileSchema(ArrayRef<PgoCounterRef> counters,
                       ArrayRef<PgoValueFunction> valueFunctions,
                       std::vector<PgoFunctionSchema> &schema);

/// Edge-only convenience overload (no value profile data).
inline std::string synthesizeProfileBuffer(ArrayRef<PgoCounterRef> counters) {
  return synthesizeProfileBuffer(counters, {});
}

/// Verified target mapping: a runtime target address -> the MD5 hash of the
/// target function's IR-level PGO name (IndexedInstrProf::ComputeHash(
/// getIRPGOFuncName(F))), built at Tier-1 compile time from the ORC engine's
/// symbol table. Values not present in this table are dropped by
/// aggregateValueSamples.
struct PgoValueTarget {
  uintptr_t addr = 0;
  uint64_t md5Hash = 0;
};

#ifdef EJIT_SRE_PGO_VALUE_PROFILE

/// Read the per-function value-site inventory (both hashes + NumValueSites for
/// the indirect-call and memop kinds) directly from the captured __profd_
/// structs: FuncHash@8 is the CFG hash, NameRef@0 is the IR-PGO-name hash.
/// numScalarSites stays 0 - the driver patches it from the Tier-1 compile
/// capture. Returns false on a malformed profd (layout drift guard).
bool readValueSiteInventory(ArrayRef<PgoCounterRef> counters,
                            SmallVectorImpl<PgoValueFunction> &funcs);

/// Aggregate raw per-core collector samples into per-site value data.
/// - indirect-call values are mapped through \p targets (addr -> IR-PGO-name
///   MD5); unmapped values are dropped (never written raw into the profile);
/// - memop sizes pass through by value;
/// - scalar sites produce top-1 + total entries (dominance thresholds are the
///   caller's policy).
/// \p funcs enumerates every function's site inventory, so probing is exact
/// and samples of unknown keys are ignored. Returns false only on invalid
/// input (e.g. a kind/site-count overrun); an empty result is a valid outcome.
bool aggregateValueSamples(ArrayRef<EJitVpSiteSample> samples,
                           ArrayRef<PgoValueFunction> funcs,
                           ArrayRef<PgoValueTarget> targets,
                           SmallVectorImpl<PgoValueSite> &icMemSites,
                           SmallVectorImpl<PgoScalarSite> &scalarSites);

#endif // EJIT_SRE_PGO_VALUE_PROFILE

/// The representative Tier-1 attempt identity of one profile session: captured
/// from the real Tier-1 compile request when the Instrumented tier is compiled
/// and later validated against the physical Tier-2 request before its frozen
/// dispatch observation is allowed into the profile bundle.
struct Tier1ProfileAttemptIdentity {
  uint64_t representativeAttemptToken = 0;
  uint32_t generation = 0;
};

/// Production capture used by EJitCompileDriver::compileCold for the
/// Instrumented tier. A null request (non-shared/legacy compile) captures the
/// zero identity, which makes applyT1DispatchObservation() report Unavailable
/// rather than attach an observation to the wrong session.
Tier1ProfileAttemptIdentity
captureTier1ProfileAttemptIdentity(const EJitCompileRequest *Tier1Request);

/// Apply the frozen observed Tier-1 dispatch metadata carried by a Tier-2
/// request to \p Bundle (experimental sharing contract, ABI v21).
///
/// The observation is accepted only when it belongs to the exact attempt and
/// generation that produced the profile: \p Request must carry attemptToken
/// \p expectedAttemptToken (nonzero) and generation \p expectedGeneration, the
/// same identity the caller resolved from its Tier-1 sampling session. A stale
/// request (cancel/replace, generation change, or a request built before the
/// observation existed) therefore reports Unavailable with count 0 and
/// quotaEnd 0 instead of attaching another session's numbers. A malformed
/// count > limit is rejected the same way.
///
/// On success actualDispatchCount is the observed count (never the configured
/// threshold), dispatchLimit is the admission quota, and dispatchQuality is
/// FrozenAtQuota when the count reached the limit (quotaEnd then holds the
/// frozen timestamp, 0 = unknown clock) or PartialOpenQuota otherwise.
void applyT1DispatchObservation(EJitProfileBundle &Bundle,
                                const EJitCompileRequest *Request,
                                uint64_t expectedAttemptToken,
                                uint32_t expectedGeneration);

/// Overload of the call above with the captured Tier-1 identity; the driver
/// uses this form so the whole chain (real Tier-1 request -> recorded identity
/// -> physical Tier-2 request -> bundle) is one production call pair.
void applyT1DispatchObservation(
    EJitProfileBundle &Bundle, const EJitCompileRequest *Request,
    const Tier1ProfileAttemptIdentity &Tier1Attempt);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITPROFILEMERGE_H
