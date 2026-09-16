//===-- EJitVpCollector.h - Online-PGO runtime value collector ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Tier-1 runtime value collector for online PGO (EJIT_VALUE_PROFILE.md).
//
//  One process-global EJitVpSharedState holds MAX_CORES per-core shards. Each
//  shard is a fixed-capacity, zero-malloc K-way heavy-hitter table with a
//  double-buffered payload pair:
//
//    * producers (the shared Tier-1 machine code) pick the active half from
//      shard.generation (one ACQUIRE load) and update ONLY their own core's
//      cells with RELAXED RMWs. Those payload lines remain core-private. A
//      session-aware producer also acquires/releases the bounded session gate
//      so retirement cannot reuse its identity while the write is in flight.
//    * the collector (the single owner worker, pre Tier-2) flips each shard's
//      generation (ACQ_REL), drains a bounded straggler window, then copies a
//      retired half only after its registered writer count reaches zero. A
//      late producer rechecks generation and backs out before touching the old
//      half, so each copied (value, count, total) tuple is stable.
//
//  The whole feature compiles only under EJIT_SRE_PGO_VALUE_PROFILE (default
//  OFF): no footprint, no shared-memory ABI change otherwise. The state blob
//  lives in EJIT_SHARED_SECTION (ordinary .bss on host) with its OWN
//  magic/version/size ABI identity - the taskpool blob (v10) is untouched.
//
//  Endianness: fixed-width scalars accessed by value only (aarch64_be safe).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITVPCOLLECTOR_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITVPCOLLECTOR_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <vector>

//===----------------------------------------------------------------------===//
// Compile-time configuration (overridable by the build via -D). All bounds are
// positive integers; the memory bound is computed from them in the static
// asserts below and documented in EJIT_VALUE_PROFILE.md §3.1.
//===----------------------------------------------------------------------===//
#ifndef EJIT_SRE_VP_K
#define EJIT_SRE_VP_K 2u ///< heavy-hitter candidates per site
#endif
#ifndef EJIT_SRE_VP_SITES_PER_CORE
#define EJIT_SRE_VP_SITES_PER_CORE 64u ///< fixed sites per core shard
#endif
#ifndef EJIT_SRE_VP_MAX_CORES
#define EJIT_SRE_VP_MAX_CORES 32u ///< per-core shard slots (core ids < this)
#endif
#ifndef EJIT_SRE_VP_TOP_N
#define EJIT_SRE_VP_TOP_N EJIT_SRE_VP_K ///< top values carried per site
#endif
#ifndef EJIT_SRE_VP_DRAIN_TICKS
#define EJIT_SRE_VP_DRAIN_TICKS 4096u ///< straggler-drain bound per snapshot
#endif

namespace llvm {
namespace ejit {

constexpr uint32_t kEJitVpK = EJIT_SRE_VP_K;
constexpr uint32_t kEJitVpSitesPerCore = EJIT_SRE_VP_SITES_PER_CORE;
constexpr uint32_t kEJitVpMaxCores = EJIT_SRE_VP_MAX_CORES;
constexpr uint32_t kEJitVpTopN = EJIT_SRE_VP_TOP_N;
constexpr uint32_t kEJitVpDrainTicks = EJIT_SRE_VP_DRAIN_TICKS;
constexpr uint32_t kEJitVpCacheLine = 64u;

static_assert(kEJitVpK >= 1 && kEJitVpK <= 8,
              "EJIT_SRE_VP_K must be in [1, 8]");
static_assert(kEJitVpTopN >= 1 && kEJitVpTopN <= kEJitVpK,
              "EJIT_SRE_VP_TOP_N must be in [1, K]");
static_assert(kEJitVpSitesPerCore >= 8,
              "EJIT_SRE_VP_SITES_PER_CORE must be >= 8");
static_assert((kEJitVpSitesPerCore & (kEJitVpSitesPerCore - 1)) == 0,
              "EJIT_SRE_VP_SITES_PER_CORE must be a power of two "
              "(the site index is hash & mask)");
static_assert(kEJitVpMaxCores >= 1 && kEJitVpMaxCores <= 256,
              "EJIT_SRE_VP_MAX_CORES must be in [1, 256]");

//===----------------------------------------------------------------------===//
// Shared-state ABI identity (EJIT_VALUE_PROFILE.md §8). Distinct magic from the
// taskpool blob; bump abiVersion on any layout change.
//===----------------------------------------------------------------------===//
constexpr uint32_t kEJitVpAbiMagic = 0x5650524Fu; // "VPRO"
constexpr uint32_t kEJitVpAbiVersion = 7u;
constexpr uint32_t kEJitVpMaxSessions = 16u;
constexpr uint64_t kEJitVpLegacySessionId = ~uint64_t(0);
constexpr uint32_t kEJitVpMaxProfdBindings = 256u;

/// Our value-site kinds. 0/1 mirror LLVM's IPVK_IndirectCallTarget /
/// IPVK_MemOPSize; kind 2 is the EJIT-only scalar/loop-bound site that travels
/// in a side table instead of the official profile (LLVM supports only the
/// three IPVK_* kinds).
enum EJitVpKind : uint32_t {
  kEJitVpIndirectCall = 0,
  kEJitVpMemOpSize = 1,
  kEJitVpScalar = 2,
  kEJitVpKindCount = 3,
};

/// Mix (funcHash, kind, siteIdx) into the 64-bit site key. The collector stores
/// this key per site; the merge side re-computes it to probe the snapshot (no
/// inversion needed). A pure function of its inputs, so Tier-1 records and the
/// Tier-2 merge always agree. siteIdx feeds the LOW bits so consecutive sites
/// of one function land in distinct direct-mapped slots; the xor-shift-multiply
/// chain then whitens the whole word. Collisions between different
/// (hash, kind, idx) triples are astronomically unlikely and, if they ever
/// happened, only perturb the approximate profile, never the runtime guard
/// (Tier-2 guards are checked at runtime).
inline uint64_t ejitVpSiteKey(uint64_t FunctionIdentity, uint32_t Kind,
                              uint32_t SiteIdx) {
  // Kind occupies bits 32-33 (kinds < 4); siteIdx bits 0-31 (capped at 2^16 by
  // kMaxValueSitesPerKind) - no overlap, low bits vary with the site index.
  uint64_t X = FunctionIdentity ^ (static_cast<uint64_t>(Kind) << 32) ^
               static_cast<uint64_t>(SiteIdx);
  X ^= X >> 30;
  X *= 0xBF58476D1CE4E5B9ULL;
  X ^= X >> 27;
  X *= 0x94D049BB133111EBULL;
  X ^= X >> 31;
  return X;
}

//===----------------------------------------------------------------------===//
// POD shared layout (EJIT_VALUE_PROFILE.md §3.1). Fixed-width scalars only; no
// bitfields, no STL, no pointers into private memory.
//===----------------------------------------------------------------------===//
struct EJitVpCandidate {
  EJitAtomicU64 value; ///< observed value (ICP: runtime target address)
  EJitAtomicU64 count; ///< hits for this value in the current window
};

/// One site of one payload half: K candidates + a window total.
struct EJitVpSite {
  EJitAtomicU64 siteKey; ///< session-mixed storage key; 0 = free
  EJitAtomicU64 samplingSessionId;
  EJitAtomicU64 functionIdentity;
  EJitAtomicU32 kind;
  EJitAtomicU32 siteIndex;
  EJitAtomicU64 total; ///< records observed in this window
  EJitVpCandidate cand[kEJitVpK];
};

struct EJitVpPayload {
  EJitVpSite sites[kEJitVpSitesPerCore];
};

/// One core's shard. generation bit0 selects the payload half producers write;
/// only the collector (owner worker) ever increments it, producers only read it
/// (ACQUIRE) - a read-only shared line, no RMW on the hot path.
struct alignas(kEJitVpCacheLine) EJitVpShard {
  EJitAtomicU64 generation;
  /// Writers currently committed to each payload half. A producer registers,
  /// then rechecks generation before touching the payload. After a flip, zero
  /// proves that the retired half is immutable for the snapshot.
  EJitAtomicU32 writers[2];
  EJitVpPayload payload[2];
};

/// Merge/transform observability counters (EJIT_VALUE_PROFILE.md §9). Written
/// only by the single owner worker (cold path: merge + Tier-2 transform);
/// read by any core through ejit_vp_get_stats with relaxed loads. Living in
/// the shared blob keeps the view consistent cross-core on SRE.
struct EJitVpSessionSlot {
  EJitAtomicU64 samplingSessionId;   ///< zero means unused/reusable
  EJitAtomicU64 requestAttemptToken; ///< exact logical cancellation owner
  /// Bit zero admits producers; each admitted producer adds two. Ending a
  /// session atomically clears bit zero, after which gate==0 proves no reader
  /// can still cross into a reused slot.
  EJitAtomicU64 gate;
  /// 0 normal, 1 incomplete T2 freeze retained, 2 terminal discard/cancel.
  /// State 2 is monotonic until the slot is exclusively retired.
  EJitAtomicU32 retireState;
  uint32_t reserved;
};

struct EJitVpProfdBinding {
  /// Even and stable while the address/session pair may be consumed. Writers
  /// bracket replacement with odd/even increments so readers cannot combine
  /// fields from different bindings.
  EJitAtomicU64 sequence;
  EJitAtomicUPtr profdAddr;
  EJitAtomicU64 samplingSessionId;
};

struct EJitVpStats {
  EJitAtomicU64 merges;            ///< Tier-2 value-profile merges performed
  EJitAtomicU64 icValueSites;      ///< indirect-call value sites merged
  EJitAtomicU64 memopValueSites;   ///< memop-size value sites merged
  EJitAtomicU64 scalarValueSites;  ///< scalar sites merged (pre-threshold)
  EJitAtomicU64 scalarDropped;     ///< scalar sites dropped by thresholds
  EJitAtomicU64 scalarSpecialized; ///< guarded scalar specializations created
};

/// Plain (non-atomic) snapshot of the VP stats (reader return shape).
struct EJitVpStatsOut {
  uint64_t merges = 0;
  uint64_t icValueSites = 0;
  uint64_t memopValueSites = 0;
  uint64_t scalarValueSites = 0;
  uint64_t scalarDropped = 0;
  uint64_t scalarSpecialized = 0;
};

/// The whole cross-core blob, placed via EJIT_SHARED_SECTION (ordinary .bss on
/// host). Trivially default constructible => zero-filled by the loader with no
/// .init_array; first user field-initializes idempotently (see
/// ejitVpEnsureInitialized).
struct alignas(kEJitVpCacheLine) EJitVpSharedState {
  // Header: written once by the initializing core (release stores); validated
  // by peers with acquire loads - atomic cells so the hot path never performs
  // a non-atomic read racing the one-time initialization.
  EJitAtomicU32 magic;
  EJitAtomicU32 abiVersion;
  EJitAtomicU32 structSize;
  uint32_t headerReserved; // write-once, never read by peers
  /// Global collection gate (read-mostly). Set once PGO+Tier-1 collection is
  /// armed; this is the first fast rejection before ABI/session lookup.
  EJitAtomicU32 armed;
  uint32_t headerAlignPad;
  /// Monotonic high-water mark. Retired IDs are never accepted again even
  /// though their bounded active slot is safely reused after snapshot drain.
  EJitAtomicU64 lastIssuedSessionId;
  uint32_t shardStride;  ///< sizeof(EJitVpShard), for cross-core validation
  uint32_t sitesPerCore; ///< kEJitVpSitesPerCore
  uint32_t k;            ///< kEJitVpK
  uint32_t maxCores;     ///< kEJitVpMaxCores
  uint32_t drainTicks;   ///< kEJitVpDrainTicks
  uint32_t headerPad[3];
  EJitVpSessionSlot sessions[kEJitVpMaxSessions];
  EJitVpProfdBinding profdBindings[kEJitVpMaxProfdBindings];
  /// Merge/transform counters (cold path; single worker writer).
  EJitVpStats stats;
  EJitVpShard shards[kEJitVpMaxCores];
};

static_assert(std::is_standard_layout<EJitVpSharedState>::value,
              "EJitVpSharedState must be standard-layout for shared placement");
static_assert(std::is_trivially_destructible<EJitVpSharedState>::value,
              "EJitVpSharedState must be trivially destructible");
static_assert(std::is_trivially_default_constructible<EJitVpSharedState>::value,
              "EJitVpSharedState must be trivially default constructible "
              "(no .init_array, loader zero-fill)");
static_assert(std::is_standard_layout<EJitVpSite>::value &&
                  std::is_trivially_destructible<EJitVpSite>::value,
              "EJitVpSite must be POD-style");
static_assert(alignof(EJitVpSharedState) == kEJitVpCacheLine &&
                  alignof(EJitVpShard) == kEJitVpCacheLine,
              "shards must be cache-line aligned to avoid false sharing");
static_assert(
    offsetof(EJitVpSharedState, magic) == 0,
    "magic must be the first word so a foreign/zero blob is rejected");

// Explicit, computable memory bound (EJIT_VALUE_PROFILE.md §3.1):
//   perCoreBytes = align64(header) + 2 * sites * sizeof(EJitVpSite)
//   totalBytes   = maxCores * perCoreBytes
// Defaults (K=2, sites=64, cores=32): 6,272 B/core => ~200 KB of shards,
// plus the fixed session registry, profd bindings, header, and statistics.
constexpr uint64_t kEJitVpPerCoreBytes =
    kEJitVpCacheLine +
    2u * kEJitVpSitesPerCore * static_cast<uint64_t>(sizeof(EJitVpSite));
constexpr uint64_t kEJitVpTotalBytes = kEJitVpMaxCores * kEJitVpPerCoreBytes;
static_assert(kEJitVpPerCoreBytes >= sizeof(EJitVpShard),
              "per-core memory bound must cover the shard layout");

/// One site sample copied out of a snapshot (top-K by count, descending).
struct EJitVpSiteSample {
  uint64_t siteKey = 0;
  uint64_t samplingSessionId = 0;
  uint64_t total = 0;
  uint64_t values[kEJitVpK] = {0};
  uint64_t counts[kEJitVpK] = {0};
};

//===----------------------------------------------------------------------===//
// Runtime record functions. These are the symbols the instrumented Tier-1
// machine code calls; the compile driver registers them as JIT user symbols.
// All three are allocation-free and lock-free. Session-aware records perform
// bounded registry lookup before touching only the calling core's shard lines;
// official LLVM hooks also perform a bounded profd-binding lookup.
//===----------------------------------------------------------------------===//

/// LLVM InstrProfiling lowering hook for indirect-call target value sites.
/// \p value is the runtime target address (mapped to the IR-PGO-name MD5 by the
/// merge, EJIT_VALUE_PROFILE.md §5.3 - never written raw into the profile).
/// \p data is the function's __profd_<name> global; \p index is the flat
/// value-site index (indirect-call sites first, then memop sites).
extern "C" void __llvm_profile_instrument_target(uint64_t value, void *data,
                                                 uint32_t index);

/// LLVM InstrProfiling lowering hook for dynamic memop size value sites.
extern "C" void __llvm_profile_instrument_memop(uint64_t value, void *data,
                                                uint32_t index);

/// EJIT scalar/loop-bound value site (kind 2, side table).
extern "C" void ejit_vp_record_scalar(uint64_t funcHash, uint32_t siteIdx,
                                      uint64_t value);
extern "C" void ejit_vp_record_scalar_session(uint64_t samplingSessionId,
                                              uint64_t funcHash,
                                              uint32_t siteIdx, uint64_t value);

//===----------------------------------------------------------------------===//
// Cold-path collector API (compile driver / merge). Runs on the single owner
// worker; never on the hot path.
//===----------------------------------------------------------------------===//

/// Idempotent field-initialization of the shared blob (header + capacities +
/// zeroed shards). Safe to race: every initializer writes identical constants,
/// and producers never touch the blob before it is armed. Returns false only on
/// an ABI mismatch against an already-initialized foreign blob.
bool ejitVpEnsureInitialized();

/// Arm / disarm the global collection gate. Records are no-ops while disarmed.
void ejitVpSetArmed(bool armed);
bool ejitVpIsArmed();

/// Allocate and arm an exact session without affecting other collecting groups.
/// Explicit IDs must increase monotonically; retired IDs can never be reopened.
/// ejitVpCreateSession is the production allocator and safely reuses only fully
/// drained inactive slots. Exhaustion fails before Tier-1 code generation.
bool ejitVpBeginSession(uint64_t samplingSessionId);
uint64_t ejitVpCreateSession(uint64_t requestAttemptToken = 0);
void ejitVpEndSession(uint64_t samplingSessionId,
                      bool preserveForRetry = false);
/// Close only the session owned by an exact request-attempt token. Safe from
/// any producer core; draining remains an owner cold-path responsibility.
void ejitVpCancelAttempt(uint64_t requestAttemptToken);
bool ejitVpSessionDiscarded(uint64_t samplingSessionId);
bool ejitVpBindProfileData(uint64_t samplingSessionId, uintptr_t profdAddr);

/// Per-kind site count of one function, used to recompute the function's exact
/// site keys (the stored key is a mixed hash, so reset/merge probe by
/// re-computation - EJIT_VALUE_PROFILE.md §3.2/§5.2).
struct EJitVpKindSiteCount {
  uint32_t kind;  ///< EJitVpKind
  uint32_t count; ///< number of sites of this kind in the function
};

/// Forget every site of a function (both payload halves of every core).
/// \p nameHash is the function-unique IR-PGO NameRef identity used by every
/// site kind. Called when a Tier-1 round starts and after Tier-2 consumes data.
void ejitVpResetFunction(uint64_t nameHash,
                         ArrayRef<EJitVpKindSiteCount> siteCounts);

/// Take the double-buffered snapshot: flip every core's generation (producers
/// switch halves), drain the bounded straggler window, and copy only retired
/// halves whose registered writer count reached zero. A busy retired half is
/// recovered before reuse by the next snapshot. Returns false when the blob is
/// not initialized (ABI mismatch included). Appends one sample per non-empty
/// site observed.
bool ejitVpTakeSnapshot(std::vector<EJitVpSiteSample> &out);

/// Snapshot and clear only one exact session. Other active sessions remain in
/// both payload halves and are delivered by their own later snapshot.
bool ejitVpTakeSessionSnapshot(uint64_t samplingSessionId,
                               std::vector<EJitVpSiteSample> &out);

/// Merge-side observability bump (owner worker, cold path).
void ejitVpBumpMergeCounts(uint64_t icSites, uint64_t memopSites,
                           uint64_t scalarSites, uint64_t scalarDropped);

/// Transform-side observability bump: \p n guarded specializations created.
void ejitVpBumpScalarSpecialized(uint64_t n);

/// Snapshot the VP stats into \p out (atomic reads; any core).
void ejitVpStatsSnapshot(EJitVpStatsOut &out);

/// Process-global collector blob. Placed in the cross-core shared section on
/// real multi-core builds (EJIT_SHARED_SECTION); ordinary .bss on host.
extern EJitVpSharedState gEJitVpState;

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITVPCOLLECTOR_H
