//===-- EJitSharedTaskPoolState.h - POD cross-core shared taskpool state --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  EJitSharedTaskPoolState: the single, fixed-layout, POD-style state blob that
//  lives in cross-core shared memory (EJIT_SHARED_SECTION). EVERY field is a
//  fixed-width scalar or an array of them, accessed exclusively through
//  EJitAtomic (acquire/release); there are no bitfields, no STL, no virtual
//  functions, and no core-private raw pointers. It is therefore safe to place a
//  single instance in shared memory mapped at the SAME virtual address on every
//  participating core.
//
//  What lives here (shared across cores):
//   * init/owner state machine, generation, owner core, worker task id, errors
//   * the MPSC request queue ring storage
//   * the flat dedup in-flight bits
//   * the SwitchController enabled/version arrays + mode
//   * the result-cache metadata + (optionally shareable) fnPtr
//   * statistics counters
//
//  What does NOT live here (owner-core private, never shared): EJit,
//  EJitCompileDriver, LLVMContext, ORC/JITLink, std::string/vector/map, any C++
//  object with a vtable or unique_ptr, and all transient compile state. Those
//  stay in the worker-owner core's private memory.
//
//  Endianness: only fixed-width scalars accessed by value. No byte-wise
//  parsing, no native-layout persistence to a cross-endian file. The same
//  definition is correct on aarch64_be and little-endian hosts. See spec §10.5
//  / §11.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOLSTATE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOLSTATE_H

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h" // EJitCompileRequest, EJitDimPair
#include <cstddef>
#include <cstdint>
#include <type_traits>

//===----------------------------------------------------------------------===//
// Compile-time capacities (overridable by the build via -D). Defaults mirror
// the non-shared taskpool so the two stay aligned.
//===----------------------------------------------------------------------===//
#ifndef EJIT_SRE_TASKPOOL_BUCKETS
#define EJIT_SRE_TASKPOOL_BUCKETS 32u
#endif
#ifndef EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX
#define EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX 4096u
#endif
#ifndef EJIT_SRE_TASKPOOL_QUEUE_CAPACITY
#define EJIT_SRE_TASKPOOL_QUEUE_CAPACITY 1024u
#endif
// Fixed slots per cache bucket. The shared cache is a fixed-capacity POD table
// (no std::unordered_map can live in shared memory), so each bucket holds a
// fixed array of slots. A bucket that fills evicts its oldest-generation slot.
#ifndef EJIT_SRE_SHARED_TASKPOOL_CACHE_SLOTS
#define EJIT_SRE_SHARED_TASKPOOL_CACHE_SLOTS 16u
#endif
// Number of distinct 2MiB code pools whose per-core 4K split state is tracked
// in the shared blob. Open-addressed by pool base, so this is a hard capacity:
// once full, a peer hitting an untracked pool cleanly falls back rather than
// risking an unbounded table or a duplicate split entry.
#ifndef EJIT_SRE_SHARED_TASKPOOL_POOL_SLOTS
#define EJIT_SRE_SHARED_TASKPOOL_POOL_SLOTS 16u
#endif
#ifndef EJIT_SRE_SHARED_DUMP_NAME_BYTES
#define EJIT_SRE_SHARED_DUMP_NAME_BYTES 128u
#endif
namespace llvm {
namespace ejit {

//===----------------------------------------------------------------------===//
// Fixed capacities and the cache-line size used to avoid false sharing.
//===----------------------------------------------------------------------===//
/// Max dims in one identity; matches EJitSharedCacheSlot::dims.
constexpr uint32_t kEJitSharedMaxDims = 4u;
constexpr uint32_t kEJitSharedDimTypes = 8u;
constexpr uint32_t kEJitSharedInstances = 256u;
constexpr uint32_t kEJitSharedMaxFuncIndex = EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX;
constexpr uint32_t kEJitSharedCacheBuckets = EJIT_SRE_TASKPOOL_BUCKETS;

/// Out-bucket for a lookup served without a read token (the per-core L0).
/// releaseRead() ignores it, so the caller shape -- call fn, then release --
/// needs no wrapper change.
constexpr uint32_t kEJitNoBucket = 0xFFFFFFFFu;
constexpr uint32_t kEJitSharedCacheSlots = EJIT_SRE_SHARED_TASKPOOL_CACHE_SLOTS;
constexpr uint32_t kEJitSharedQueueSlots = EJIT_SRE_TASKPOOL_QUEUE_CAPACITY;
constexpr uint32_t kEJitSharedPoolSlots = EJIT_SRE_SHARED_TASKPOOL_POOL_SLOTS;
constexpr uint32_t kEJitSharedDumpNameBytes = EJIT_SRE_SHARED_DUMP_NAME_BYTES;
constexpr uint32_t kEJitSharedCacheLine = 64u;
/// Execute-permission seal granularity (the platform's per-page enable_ex unit)
/// and the large-page / split granularity. Fixed platform constants.
constexpr uint64_t kEJitSharedSealPage = 4096u;
constexpr uint64_t kEJitSharedSplitGranule =
    static_cast<uint64_t>(2) * 1024 * 1024;
/// Highest core id whose per-core readiness can be memoized in a 64-bit mask.
constexpr uint32_t kEJitSharedMaxMemoCores = 64u;

static_assert((kEJitSharedQueueSlots & (kEJitSharedQueueSlots - 1)) == 0 &&
                  kEJitSharedQueueSlots >= 2,
              "shared queue slot count must be a power of two >= 2");

//===----------------------------------------------------------------------===//
// Init / owner-election state machine (spec §11). A single EJitAtomicU32 holds
// exactly one of these — never an ambiguous bool.
//===----------------------------------------------------------------------===//
enum class EJitSharedInitState : uint32_t {
  Uninitialized = 0, ///< No core has claimed ownership yet.
  Initializing = 1,  ///< The owner won the CAS and is building shared state.
  Ready = 2,         ///< Shared state usable; exactly one worker owner exists.
  Failed = 3,        ///< Owner init failed; producers clean-fall back, no wait.
  Stopping = 4,      ///< Owner is tearing down; producers stop enqueuing.
};

/// Diagnostic reason recorded in lastInitError when init reaches Failed. Kept
/// independent of the C-ABI status codes so the facade pulls no runtime header.
enum class EJitSharedInitError : uint32_t {
  None = 0,
  WorkerStartFailed = 1,
};

//===----------------------------------------------------------------------===//
// Per-cache-slot publish state. The fnPtr is read only after state==Ready was
// observed with an acquire load (publish stores Ready with release last).
//===----------------------------------------------------------------------===//
enum class EJitSharedSlotState : uint32_t {
  Empty = 0,      ///< Slot free.
  Publishing = 1, ///< Owner is mid-write; readers must skip.
  Ready = 2,      ///< fnPtr/identity/versions valid for an acquiring reader.
};

//===----------------------------------------------------------------------===//
// EJitSharedCacheSlot: one POD result-cache entry.
//===----------------------------------------------------------------------===//
struct EJitSharedCacheSlot {
  EJitAtomicU32 state;  ///< EJitSharedSlotState
  uint32_t funcIndex;   ///< identity
  uint32_t numDims;     ///< identity (<= 4)
  uint32_t generation;  ///< owner generation that wrote this slot
  EJitDimPair dims[4];  ///< identity
  uint32_t versions[4]; ///< per-instance version snapshot at publish
  uint64_t identityHash; ///< hash(funcIndex, dims) — fast reject before compare
  EJitAtomicUPtr fnPtr; ///< compiled function pointer (cross-core read gated)
  /// Bit N means core N has successfully installed execute permission for this
  /// code address. Core ids >= 64 are supported but cannot be memoized here,
  /// so they run the preparation callback on every hit.
  EJitAtomicU64 executableCoreMask;
  /// Real executable extent of the published code, copied from the owner's
  /// finalized code-pool allocation (v5). A non-owner core in 4K-seal mode
  /// needs the full [codeStart, codeStart + codeSize) — not just fnPtr — to
  /// split its pool and seal every 4KiB page the code covers in its own
  /// translation context. All plain scalars, written under the bucket write
  /// lock BEFORE state=Ready is released, so an acquiring reader sees them
  /// consistently. 0 codeSize => no range metadata (peer cleanly falls back).
  uintptr_t codeStart;    ///< start of the RX-sealed executable allocation
  uint64_t codeSize;      ///< size in bytes of that allocation (0 = none)
  uintptr_t poolBase;     ///< 2MiB pool base (split_2m_to_4k granule)
  uint64_t poolSize;      ///< usable pool size
  uint32_t poolId;        ///< stable pool index (diagnostic / convenience key)
  uint32_t rangeReserved; ///< reserved, keeps the tail explicit (must be 0)
  /// PGO (v7): per-slot hotspot counter for the Tier-2 auto-trigger (§6).
  /// Incremented on cache hit; reset to 0 when Tier-2 publishes over Tier-1
  /// (§7.1).  Counter addresses (__profc_/__profd_) are resolved by the
  /// compile driver via ORC lookup and kept driver-private — they do not
  /// need to live in the shared slot.
  EJitAtomicU64 hitCount;
  /// PGO (§7.1): current compile tier of the published code.  0 = Baseline /
  /// not yet published, 1 = Instrumented (Tier-1), 2 = PGOUse (Tier-2).
  /// Used to suppress the Tier-2 auto-trigger on slots that are already
  /// Tier-2 — a Tier-2 hit should never request another Tier-2 compile.
  EJitAtomicU8 tier;
};

//===----------------------------------------------------------------------===//
// EJitSharedCacheBucket: a fixed array of slots guarded by an embedded
// two-word reader/writer lock (same protocol as EJitRwLock, but inline in the
// shared blob). Buckets are cache-line aligned so a writer to one bucket never
// false-shares with readers of another.
//===----------------------------------------------------------------------===//
struct alignas(kEJitSharedCacheLine) EJitSharedCacheBucket {
  EJitAtomicU32 writeFlag; ///< 0 = free, 1 = writer holds/pending
  EJitAtomicU32 readers;   ///< active reader count (token model)
  /// Monotonic publish sequence for the EJIT_SRE_TASKPOOL_NO_RECLAIM seqlock
  /// reader: the publisher makes it ODD before writing a slot and EVEN after
  /// (see bucketWrite/bucketWriteRelease). A load-only reader captures it before
  /// its scan and re-checks it after loading fnPtr; an unequal/odd value means a
  /// publish raced the read, so the reader discards and cleanly falls back. Zero
  /// per-hit atomic RMW on this line (unlike the token's readers counter). Only
  /// bumped in a NO_RECLAIM build; stays 0 otherwise, so the default token path
  /// is byte-for-byte unchanged.
  EJitAtomicU32 publishSeq;
  EJitSharedCacheSlot slots[kEJitSharedCacheSlots];
};

//===----------------------------------------------------------------------===//
// EJitSharedQueueCell: one Vyukov ring cell carrying a full request by value.
//===----------------------------------------------------------------------===//
struct EJitSharedQueueCell {
  EJitAtomicU32 sequence;
  EJitCompileRequest data;
};

//===----------------------------------------------------------------------===//
// EJitSharedPoolSplit: per-core readiness for ONE 2MiB code pool's 4K split.
//
// split_2m_to_4k and enable_ex affect only the CALLING core's stage-1 page
// table, so each core must split a given pool exactly once before it may seal
// any 4K page inside it. This POD tracks, per pool (open-addressed by base),
// which cores have completed the split (splitDoneMask) and which are mid-split
// (splitPreparingMask), so concurrent first-touch across cores and on one core
// is coordinated without a lock and a successful split publishes ready only
// after it actually succeeds. Core ids >= 64 cannot be memoized in the 64-bit
// masks and re-run the split on every hit.
//===----------------------------------------------------------------------===//
struct EJitSharedPoolSplit {
  EJitAtomicUPtr poolBase;          ///< claimed 2MiB pool base (0 = empty slot)
  EJitAtomicU64 splitDoneMask;      ///< bit c => core c finished the split
  EJitAtomicU64 splitPreparingMask; ///< bit c => core c is mid-split
};

//===----------------------------------------------------------------------===//
// EJitSharedDumpState: fixed-size cross-core diagnostic metadata.
//
// Full IR/ASM text remains in the owner worker's private gDumpStore. Shared
// memory carries only the active filter and enough metadata to direct a
// non-owner caller to the worker core that owns the latest capture.
//===----------------------------------------------------------------------===//
struct alignas(kEJitSharedCacheLine) EJitSharedDumpState {
  EJitAtomicU32 lock;          ///< 0 free, 1 locked by filter/capture/print
  EJitAtomicU32 filterEnabled; ///< 1 => filterName is active
  EJitAtomicU32 hasDump;       ///< 1 => latest-capture metadata is valid
  EJitAtomicU32 status;        ///< bit2 => resultName was truncated
  uint32_t filterLen;
  uint32_t resultNameLen;
  uint32_t irSize;
  uint32_t asmSize;
  uint32_t keyHi;
  uint32_t keyLo;
  uint32_t workerCore;
  uint32_t reserved0;
  char filterName[kEJitSharedDumpNameBytes];
  char resultName[kEJitSharedDumpNameBytes];
};

//===----------------------------------------------------------------------===//
// EJitSharedCounters: lock-free statistics, all monotonic.
//
// The increments are gated by EJIT_STATS_ENABLE (see EJitStats.h): when that
// macro is undefined the EJIT_STAT_INC* call sites compile to nothing, so the
// per-call cacheHits RMW - the steady-state hot-path cost - vanishes. The
// FIELDS always remain here (shared-memory layout / stats ABI is stable); with
// stats off they simply stay zero and ejit_taskpool_get_stats() reports zeros.
//===----------------------------------------------------------------------===//
struct EJitSharedCounters {
  EJitAtomicU64 cacheHits;
  EJitAtomicU64 asyncCompiles;
  EJitAtomicU64 asyncEnqueues;
  EJitAtomicU64 alreadyPending;
  EJitAtomicU64 queueFull;
  EJitAtomicU64 compileFailed;
  EJitAtomicU64 publishFailed;
  EJitAtomicU64 instanceDisabled;
  EJitAtomicU64 instanceDisabledPreActivate; ///< Subset of instanceDisabled that
                                             ///< occurred before the first
                                             ///< setInstanceEnabled(true) — i.e.
                                             ///< the init→activate window.
  EJitAtomicU64 executePrepareFailed;
  /// PGO (v7, EJIT_ONLINE_PGO.md §10): Tier-1/Tier-2 compile counts + profile
  /// synthesis failures. Zero when PGO is off; fields always present for ABI.
  EJitAtomicU64 tier1Compiles;
  EJitAtomicU64 tier2Compiles;
  EJitAtomicU64 profileMergeFails;
};

//===----------------------------------------------------------------------===//
// EJitSharedCodePoolStats: owner-published mirror of the owner-core
// EJitCodePoolManager stats. The code pool itself is owner-private (only the
// worker core allocates/seals), so a non-owner core reading its own per-core
// manager sees pools=0. The owner publishes a fresh snapshot here after every
// successful compile (runCompile / sync publish), and every core reads this for
// ejit_print_code_pool_stats / ejit_get_code_pool_stats — so the diagnostic is
// consistent cross-core. Relaxed loads/stores: diagnostic only, monotone-ish.
//===----------------------------------------------------------------------===//
struct EJitSharedCodePoolStats {
  EJitAtomicU64 poolCount;
  EJitAtomicU64 sealedCount;
  EJitAtomicU64 activeCount;
  EJitAtomicU64 usedBytes;
  EJitAtomicU64 reservedBytes;
  EJitAtomicU64 wastedBytes;
  EJitAtomicU64 sealInvocations;
  EJitAtomicU64 splitInvocations;
  EJitAtomicU64 finalizedRangeCount;
};

/// Plain (non-atomic) snapshot of code-pool stats, used as the callback
/// out-struct for the owner-private provider and as the reader return shape.
/// Mirrors EJitCodePoolManager::Stats field-for-field but stays decoupled from
/// the code-pool header so the shared-taskpool ABI does not depend on it.
struct EJitCodePoolStatsOut {
  uint64_t poolCount = 0;
  uint64_t sealedCount = 0;
  uint64_t activeCount = 0;
  uint64_t usedBytes = 0;
  uint64_t reservedBytes = 0;
  uint64_t wastedBytes = 0;
  uint64_t sealInvocations = 0;
  uint64_t splitInvocations = 0;
  uint64_t finalizedRangeCount = 0;
};

//===----------------------------------------------------------------------===//
// EJitSharedTaskPoolState: the whole shared blob. One instance per shared
// memory region. Cache-line aligned, fields grouped to keep hot producer state
// (queue head/tail) off the same line as cold state.
//===----------------------------------------------------------------------===//
struct alignas(kEJitSharedCacheLine) EJitSharedTaskPoolState {
  //--- header: plain scalars written once by the owner BEFORE publishing Ready,
  //    validated by every other core. Compared by value (endian-safe).
  uint32_t magic;
  uint32_t abiVersion;
  uint32_t structSize;
  uint32_t headerReserved;

  //--- owner / init state machine (its own cache line)
  alignas(kEJitSharedCacheLine)
      EJitAtomicU32 initState; ///< EJitSharedInitState
  EJitAtomicU32
      ownerCoreId; ///< core that won election (kEJitInvalidCoreId if none)
  EJitAtomicU32 generation;         ///< bumps each (re)initialization
  EJitAtomicU32 lastInitError;      ///< error code recorded on Failed
  EJitAtomicU32 initAttempts;       ///< total election attempts (diagnostic)
  EJitAtomicU32 codeSharingEnabled; ///< 1 => any core may read cache fnPtr
  EJitAtomicU64 workerTaskId;       ///< platform worker task id (diagnostic)
  EJitAtomicU64 registrationFingerprint; ///< owner funcIndex/dimType mapping
                                         ///< digest; peers validate on attach
  /// Bumps on any event that can invalidate a cached (identity -> fnPtr)
  /// mapping: publish, eviction, version change, (re)initialization. Per-core
  /// L0 entries record the epoch they were filled at and are discarded on a
  /// mismatch. Read once per dispatch, written only on those rare events, so
  /// the line stays Shared in every core's L1 rather than bouncing. Sits in
  /// this cache line's padding, so sizeof() and later offsets are unchanged.
  EJitAtomicU32 dispatchEpoch;

  //--- SwitchController state (own cache line)
  alignas(kEJitSharedCacheLine)
      EJitAtomicU8 enabled[kEJitSharedDimTypes][kEJitSharedInstances];
  EJitAtomicU32 version[kEJitSharedDimTypes][kEJitSharedInstances];
  EJitAtomicU32 mode; ///< EJitCompileMode (Off=0, Async=1)
  EJitAtomicU32 anyInstanceActivated; ///< 1 once any instance first
                                      ///< setInstanceEnabled(true); gates the
                                      ///< instanceDisabledPreActivate counter.
                                      ///< Reset on each (re)initialization.
                                      ///< Stats-only: this field and the
                                      ///< instanceDisabledPreActivate increment
                                      ///< are read/written only under
                                      ///< EJIT_STATS_ENABLE (see EJitStats.h);
                                      ///< with stats off the acquire-load gate
                                      ///< on the disabled path is compiled out.

  //--- flat dedup slots (own cache line). Each slot stores the OWNER GENERATION
  //    that claimed it (0 = free), not a 1-bit flag: a dedupMark CASes 0->gen
  //    and a dedupClear CASes gen->0, so a stale worker from an earlier
  //    generation can never clear a slot a newer generation re-claimed for the
  //    same funcIndex (spec §11 generation-aware dedup).
  alignas(kEJitSharedCacheLine) EJitAtomicU32 inFlight[kEJitSharedMaxFuncIndex];

  //--- MPSC queue: head and tail on SEPARATE cache lines (false-sharing), ring
  //    storage on its own.
  alignas(kEJitSharedCacheLine) EJitAtomicU32 enqueuePos;
  alignas(kEJitSharedCacheLine) EJitAtomicU32 dequeuePos;
  alignas(kEJitSharedCacheLine) EJitSharedQueueCell ring[kEJitSharedQueueSlots];

  //--- counters (own cache line)
  alignas(kEJitSharedCacheLine) EJitSharedCounters counters;

  //--- owner-published code-pool stats mirror (own cache line). Read by every
  //    core for ejit_print_code_pool_stats so the view is consistent cross-core
  //    (the real pools live owner-side only).
  alignas(kEJitSharedCacheLine) EJitSharedCodePoolStats codePoolStats;

  //--- per-core 4K split readiness, one entry per tracked 2MiB pool (own cache
  //    line). Open-addressed by pool base; see EJitSharedPoolSplit.
  alignas(kEJitSharedCacheLine)
      EJitSharedPoolSplit poolSplits[kEJitSharedPoolSlots];

  //--- bounded cross-core IR/ASM dump diagnostics (own cache line)
  alignas(kEJitSharedCacheLine) EJitSharedDumpState dump;

  //--- result cache (own cache line; each bucket is itself cache-line aligned)
  alignas(kEJitSharedCacheLine)
      EJitSharedCacheBucket buckets[kEJitSharedCacheBuckets];
};

//===----------------------------------------------------------------------===//
// Layout/ABI guarantees. NOTE on trivially-copyable: EJitAtomic<T> deliberately
// deletes its copy/move (an atomic cell identifies a fixed memory slot), so the
// blob is NOT trivially copyable and must NEVER be memcpy'd. It is instead
// initialized field-by-field in place (EJitSharedTaskPool::ownerInit) so it
// works on raw, uninitialized shared memory too. We therefore assert the
// properties that DO hold and matter for shared placement: standard layout (no
// vtable, predictable field offsets across cores) and trivial destruction (no
// teardown side effects when the shared region is reclaimed).
//===----------------------------------------------------------------------===//
static_assert(std::is_standard_layout<EJitSharedTaskPoolState>::value,
              "EJitSharedTaskPoolState must be standard-layout for shared "
              "placement at a common virtual address");
static_assert(std::is_trivially_destructible<EJitSharedTaskPoolState>::value,
              "EJitSharedTaskPoolState must be trivially destructible (no "
              "teardown side effects in shared memory)");
// The blob MUST be trivially default constructible: a namespace-scope global of
// this type then lands in .bss (zero-filled by the loader) with NO C++ dynamic
// initializer / _GLOBAL__sub_I / .init_array / startup memset. That is what
// guarantees no core ever re-zeros the shared queue/cache/owner state at load;
// only the elected owner field-initializes it via initSharedStorage (spec §11).
// It also makes the blob an implicit-lifetime type, so a real object exists in
// the shared region without UB.
static_assert(
    std::is_trivially_default_constructible<EJitSharedTaskPoolState>::value,
    "EJitSharedTaskPoolState must be trivially default constructible so its "
    "global needs no dynamic initialization (.init_array)");
static_assert(
    std::is_standard_layout<EJitSharedCacheSlot>::value &&
        std::is_trivially_destructible<EJitSharedCacheSlot>::value &&
        std::is_trivially_default_constructible<EJitSharedCacheSlot>::value,
    "EJitSharedCacheSlot must be POD-style");
static_assert(
    std::is_standard_layout<EJitSharedQueueCell>::value &&
        std::is_trivially_destructible<EJitSharedQueueCell>::value &&
        std::is_trivially_default_constructible<EJitSharedQueueCell>::value,
    "EJitSharedQueueCell must be POD-style");
static_assert(
    std::is_standard_layout<EJitSharedPoolSplit>::value &&
        std::is_trivially_destructible<EJitSharedPoolSplit>::value &&
        std::is_trivially_default_constructible<EJitSharedPoolSplit>::value,
    "EJitSharedPoolSplit must be POD-style");
static_assert(
    std::is_standard_layout<EJitSharedDumpState>::value &&
        std::is_trivially_destructible<EJitSharedDumpState>::value &&
        std::is_trivially_default_constructible<EJitSharedDumpState>::value,
    "EJitSharedDumpState must be POD-style");
static_assert(alignof(EJitSharedTaskPoolState) == kEJitSharedCacheLine,
              "EJitSharedTaskPoolState must be cache-line aligned");
static_assert(
    alignof(EJitSharedCacheBucket) == kEJitSharedCacheLine,
    "cache buckets must be cache-line aligned to avoid false sharing");
static_assert(
    offsetof(EJitSharedTaskPoolState, magic) == 0,
    "magic must be the first word so a foreign/zero blob is rejected");

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOLSTATE_H
