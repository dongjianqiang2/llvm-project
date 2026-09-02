//===-- EJitSharedTaskPool.h - Cross-core shared single-worker facade -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  EJitSharedTaskPool drives a single EJitSharedTaskPoolState shared across
//  cores. It does NOT own the blob (the blob lives in shared memory / a test
//  fixture); it binds to it and provides:
//
//   * owner election: the first core to CAS Uninitialized->Initializing becomes
//     the worker owner, builds the shared state, optionally starts the ONE
//     worker, and publishes Ready (or Failed). Every other core observes the
//     outcome with an acquire load and binds — it never creates a second
//     worker. A build may instead PIN the worker to one designated core (CMake
//     EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE, consumed in
//     EJitSharedTaskPool.cpp): only that core may run the CAS; a non-designated
//     core waits — bounded, yielding — for the designated core to initialize,
//     then attaches.
//   * the producer path compileOrGet() operating purely on shared state.
//   * the consumer path pollOne()/pollBudget() (the worker, or a test, drives
//     it) with the two version checkpoints and the commit-gated cache publish.
//   * read-only diagnostics.
//
//  The compile callback, the physical-code release callback, and the worker
//  start/stop hooks are all OWNER-CORE-PRIVATE function pointers injected
//  before init(): they reach the owner's private EJit/ORC objects, which must
//  never be placed in shared memory.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOL_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOL_H

#include "llvm/ExecutionEngine/EJIT/EJitCodeRange.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPoolState.h"
#include "llvm/ExecutionEngine/EJIT/EJitStats.h"
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h" // EJitCompileMode, status enum
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <vector>

namespace llvm {
namespace ejit {

class EJitModuleLoader;

//===----------------------------------------------------------------------===//
// Read-only diagnostics snapshot (spec §11 observability). Every field is a
// plain copy of an atomic load; no field exposes a raw shared pointer.
//===----------------------------------------------------------------------===//
struct EJitSharedDiagnostics {
  uint32_t initState;     ///< EJitSharedInitState
  uint32_t ownerCoreId;   ///< kEJitInvalidCoreId until elected
  uint32_t generation;    ///< bumps each (re)init
  uint32_t lastInitError; ///< error code recorded on Failed
  uint32_t initAttempts;  ///< total election attempts
  uint32_t codeSharingEnabled;
  uint64_t workerTaskId;
  uint64_t registrationFingerprint;
  uint32_t queueDepth;      ///< approximate in-ring requests
  uint32_t pendingCount;    ///< in-flight dedup slots
  uint32_t cacheReadyCount; ///< Ready cache slots
  uint64_t cacheHits;
  uint64_t asyncEnqueues;
  uint64_t asyncCompiles;
  uint64_t alreadyPending;
  uint64_t queueFull;
  uint64_t compileFailed;
  uint64_t publishFailed;
  uint64_t instanceDisabled;
  uint64_t
      instanceDisabledPreActivate; ///< instanceDisabled before first activate.
  uint64_t executePrepareFailed;
  uint32_t pgoActiveFunctionCount;
  uint32_t pgoMaxActiveFunctions;
  uint64_t pgoCompletedFunctions;
  uint64_t pgoDeferredMisses;
  uint64_t tier1Compiles;
  uint64_t tier2Compiles;
  uint64_t profileMergeFails;
};

//===----------------------------------------------------------------------===//
// One step of the worker state machine (spec §11 worker startup timing). The
// worker MUST NOT exit when it observes a not-yet-Ready owner (the SRE task may
// be scheduled before the owner publishes Ready); it waits instead.
//===----------------------------------------------------------------------===//
enum class EJitWorkerStep : uint32_t {
  WaitForReady =
      0,    ///< Owner still Initializing: wait, do NOT read queue/cache.
  Consumed, ///< Ready: dequeued and ran one compile.
  Idle,     ///< Ready: queue empty this iteration.
  Exit,     ///< Failed/Stopping/Uninitialized: leave the loop.
};

//===----------------------------------------------------------------------===//
// Per-function inline cache: SHARED PARTITIONED CELL TABLE.
//
// One cell per (funcIndex, dim identity) holding a specialization pointer, read
// with no version / dims / generation re-validation and no release_read: the
// probe is one load + null check + indirect call.
//
// Where the cells LIVE is the design. The wrapper's @__ejit_icache_fn_<name>
// array is emitted into the inter-core shared section (EJIT_ICACHE_SECTION,
// .mc_shared) rather than per-core .bss, so ONE table backs every core:
//
//  * PARTITIONED, so no locking. A cell's index is icacheLinearize(dims), the
//    ejit_dim argument values (the ejit_period_lc instance indices). Cores
//    drive disjoint instance ranges, so they write disjoint cells and the hit
//    path is what it was when the table was private: no gate, no contention,
//    one load.
//
//  * DRAINABLE ACROSS CORES, which a private table is not. activate/deactivate
//    rewrites every registered cell to its empty value in place (icacheDrainAll
//    -- the &MissFn sentinel for sentinel-form tables, 0 for guarded ones),
//    reaching a peer's partition directly. A permanently-hot core -- one that
//    never misses and never calls the runtime again -- stops running the stale
//    specialization on its next call, with no invalidation epoch on the probe,
//    no per-core drain rendezvous, and no sync entry point for the application
//    to call.
//
// PREREQUISITE, and it is a CORRECTNESS one: the partitioning must be real --
// each core drives its OWN ejit_period_lc instance indices, so no two cores
// call one ejit_entry with the same ejit_dim values.
//
// Disjointness is what makes a cell safe to jump to. A core only ever reads
// cells it filled itself, and it filled them after resolving through the
// taskpool, which is where per-core execute preparation happens (4K seal /
// prepareCodeFn_). So the pointer it loads is always code it has already
// prepared -- exactly the guarantee a per-core .bss table gave for free.
//
// If two cores DO share an identity they share a cell, and the second one loads
// a pointer the first one prepared. The value is the same specialization, so
// the result is right, but the translation is not: under per-core sealing that
// core branches into a page it never sealed. So a collision is NOT benign here,
// and this is the one thing about the design that cannot be checked from the
// runtime side: the cell carries no record of which core wrote it, and adding
// one would put a per-core gate back on a probe whose whole point is not having
// any. It is a deployment contract, and it is on the application to hold it.
//
// (icacheCrossCoreExecutable() is NOT consulted for a DIMENSIONED entry. It
// would decline every fill in every build the probe is allowed in, since
// EJIT_SRE_SHARED_CODE_POINTERS always wires either fourKSeal_ or
// prepareCodeFn_. The taskpool's own cross-core path still uses it.)
//
// THE 0-DIM EXCEPTION. An entry with no ejit_dim params has ONE cell and no
// identity to partition it by, so everything above simply does not apply to it:
// every core reads and writes that same scalar however well behaved the
// deployment is. It is not a contract violation, it is the shape. So for
// numDims == 0 icacheFill DOES consult icacheCrossCoreExecutable() and declines
// unless a resolved pointer is callable on every core the instant it exists.
// Where it declines the cell stays empty, the probe keeps missing, and the
// taskpool -- which prepares -- serves every call. The AOT pass still emits the
// 0D probe: the code is platform independent, only the fill is not.
//
// THE RACE, and why it is bounded: core A's drain stores 0 while core B's probe
// loads the cell. Both are naturally-aligned word accesses, so B reads the old
// pointer (calls the previous specialization once more, misses next call) or 0
// (misses now) -- never a torn value.
//
// The one race that is NOT benign is a fill landing AFTER a drain: the pointer
// was resolved pre-toggle, so the cell would come back holding a specialization
// baked from the old period values and nothing would clear it. icacheDrainSeq
// closes exactly that -- icacheBeginResolve() snapshots it at the taskpool
// entry point and icacheFill drops the fill if it moved.
//
// Lifetime safety: JIT code is never physically freed in production (NO_RECLAIM
// + no releaser wired), so a cached pointer can never dangle -- including one a
// probe loaded an instruction before the drain zeroed its cell. There is no
// hazard-pointer retire: if a releaser IS wired the safety gate
// (icacheReclamationSafe_) disables the cache outright to avoid UAF.
//
// The code-sharing gate is retained: a non-owner core may only read a cached
// pointer when EJIT_SRE_SHARED_CODE_POINTERS is platform-validated.
//===----------------------------------------------------------------------===//
#ifndef EJIT_ICACHE_FUNC_SLOTS
// Fallback ONLY for a non-CMake compile. The default must cover the full
// dense funcIndex space (EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX, default 4096) —
// the historical 64-slot default silently dropped icacheFill for every
// funcIndex >= 64 and defeated the inline cache in production. The
// static_assert in EJitSharedTaskPool.cpp hard-fails any build where
// FUNC_SLOTS < MAX_FUNC_INDEX.
#define EJIT_ICACHE_FUNC_SLOTS 4096u
#endif

// Per-dim bound D of the multi-version inline cache (@__ejit_icache_fn_<name>
// is a [D]^numDims array). MUST be a power of 2 (the hit path indexes with
// shifts, no multiply). The CMake EJIT_ICACHE_DIM_SIZE var overrides this
// default; the same CMake var feeds the AOT pass, and configure time
// cross-checks the built compiler via -mllvm -print-ejit-icache-dim-size,
// so array layout and runtime linearization agree.
#ifndef EJIT_ICACHE_DIM_SIZE
#define EJIT_ICACHE_DIM_SIZE 16u
#endif
// Maximum number of ejit_dim params a cached ejit_entry may have. An entry
// with more is a compile error (the wrapper is not emitted). Must equal
// kEJitSharedMaxDims (EJitSharedTaskPoolState.h) and MAX_PERIOD_ARR_IND_PARAMS
// (EJitCommon.h); hard-locked by static_asserts in EJitSharedTaskPool.cpp and
// EJitRuntime.cpp.
#ifndef EJIT_ICACHE_MAX_DIMS
#define EJIT_ICACHE_MAX_DIMS 4u
#endif

/// Resolve token (see EJitSharedTaskPool::icacheBeginResolve): bit 32 marks a
/// usable token, the low 32 bits carry the drain sequence it was taken at. A
/// plain integer so the taskpool C ABI can hold one in a local without dragging
/// this header into non-shared builds.
constexpr uint64_t kEJitIcacheNoResolve = 0;
constexpr uint64_t kEJitIcacheResolveValid = uint64_t{1} << 32;

// Test/diagnostic: UNREGISTER every icache slot, WITHOUT touching the cells the
// bases point at (a test-local base may already be gone). The slot table is
// process-static, so tests clear it between cases. To empty the cells of slots
// that stay registered, use EJitSharedTaskPool::icacheDrainAll().
void ejitIcacheClearAll();

// Register a per-function icache slot: \p base is the address of the wrapper's
// @__ejit_icache_fn_<name> global (a uintptr_t cell, or a [D]^numDims array of
// them for a multi-version entry), \p numDims is its dimensionality, and \p
// missFn is the slot's SENTINEL value when the wrapper's probe is branchless
// (the table is defined pre-filled with &MissFn): non-null tells
// icacheDrainAll and icacheFill's retract to write \p missFn back instead of
// 0, so the cell never holds a non-callable value. Null keeps the historical
// zero semantics for guarded (3D/4D, timing) slots. The runtime writes the
// specialization pointer through the cell at [i0][i1]... (linearized from
// dims) on a successful resolve (icacheFill), and empties the whole array on
// a period toggle (icacheDrainAll); the wrapper reads the cell directly.
// Called from ejit_register_icache_slot (name->funcIndex resolution) at
// ejit_auto_register / .ejit_period time.
//
// Every core registers the SAME base: with EJIT_ICACHE_SECTION set the array is
// one shared object, which is what lets a drain on any core clear the cells
// every other core reads.
//
// Outcome of a slot registration. The two failure kinds must NOT be conflated:
//
//   CapacityMiss  past EJIT_ICACHE_FUNC_SLOTS (4096) while the function
//   registry
//                 holds 4096. Normal in a large application: the cell stays
//                 null and the taskpool serves the function. Callers must
//                 degrade, never error, or the 65th ejit_entry stops ejit_init.
//   Invalid       null base, or numDims above the cap (it sizes the array the
//                 drain walks). A real defect, and reported.
enum class EJitIcacheRegResult { Ok, CapacityMiss, Invalid };

EJitIcacheRegResult ejitIcacheRegisterSlot(uint32_t funcIndex, void *base,
                                           uint32_t numDims,
                                           const void *missFn = nullptr);

/// Diagnostic: dump every registered icache slot to the diagnostic log.
/// Shows funcIndex, base pointer, numDims, the per-slot cell capacity
/// (16^numDims), and cell[0] (the scalar or [0]...[0] cell) so the caller
/// can quickly see which slots are wired, which are null, and whether the
/// first cell has been filled. When \p loader is given the slot's
/// funcIndex is resolved to a function name ("<unknown>" when the registry
/// has none; "?" without a loader).
/// NOTE: dereferences base[0] of every registered slot — bases must be
/// module-lifetime storage, never stack locals (see ejitIcacheClearAll).
void ejitDumpIcacheSlots(const EJitModuleLoader *loader = nullptr);

/// Retire one in-flight drain that was announced under generation \p gen.
///
/// Split out of icacheDrainAll() because it is the half of the drain protocol
/// that has to survive a (re)initialization landing in the middle of a walk,
/// and that interleaving is only reachable deterministically by calling this
/// directly. Two rules:
///
///  * If the blob's generation has moved on, do nothing. A re-init discards the
///    whole in-flight count, so this drain's increment is already gone and
///    subtracting again would wrap the counter.
///  * Never go below zero regardless, so no ordering anywhere can leave
///    icacheBeginResolve() permanently refusing tokens.
void ejitIcacheRetireDrain(EJitSharedTaskPoolState *st, uint32_t gen);

/// One L0 slot, sized to a cache line. The identity is stored in full and
/// re-checked on every hit, because the index hash is not injective: it selects
/// a slot and nothing more, so a collision must evict, never answer.
struct EJitL0Entry {
  uint32_t seq; ///< even = stable, odd = mid-write
  uint32_t epoch;
  uint32_t core;
  uint32_t funcIndex;
  uint32_t numDims;
  EJitDimPair dims[kEJitSharedMaxDims];
  void *fn; ///< null = empty
};
constexpr uint32_t kEJitL0Slots = 64; // 4KB/core

// The two globals below are core-private: deliberately NOT in
// EJIT_SHARED_SECTION_ATTR, so each core's image holds its own copy and
// probing generates no coherence traffic.
extern EJitL0Entry gEJitL0[kEJitL0Slots];

/// The shared state this core's table was armed against, null if it has not
/// passed the L0 gates. Not a member of EJitSharedTaskPool: that object lives
/// in the compile driver and is SHARED, so one core would arm the probe for
/// cores that never qualified. Holding the pointer rather than a bool also
/// rejects entries armed against a DIFFERENT blob, which the epoch cannot: a
/// fresh blob starts from a low epoch a stale entry can match by coincidence.
extern const void *gEJitL0State;

inline bool dimsEqual(const EJitDimPair *a, const EJitDimPair *b,
                      uint32_t numDims) {
  for (uint32_t i = 0; i < numDims; ++i)
    if (a[i].dimType != b[i].dimType || a[i].instanceId != b[i].instanceId)
      return false;
  return true;
}

/// Slot selection only; correctness never depends on this being injective.
inline uint32_t ejitL0Index(uint32_t funcIndex, const EJitDimPair *dims,
                            uint32_t numDims) {
  uint32_t k = funcIndex * 2654435761u;
  for (uint32_t i = 0; i < numDims; ++i)
    k = (k ^ (dims[i].dimType * 2654435761u) ^ dims[i].instanceId) *
        2654435761u;
  return (k >> 26) & (kEJitL0Slots - 1);
}

class EJitSharedTaskPool {
public:
  /// Owner-private compile callback (reaches the owner's EJit/ORC). Returns
  /// true and *outFn on success.
  using CompileCallback = bool (*)(void *ctx, const EJitCompileRequest &req,
                                   void **outFn);
  using PublishCallback = void (*)(void *ctx, const EJitCompileRequest &req,
                                   bool published);
  /// Owner-private physical-code release callback for an overwritten/retired
  /// pointer. Optional; a purely logical drop happens when unset.
  using ReleaseCallback = void (*)(void *ctx, void *oldFn);
  /// Install execute permission for \p fnPtr in the calling core's translation
  /// context. Required before a non-owner core may consume a shared fnPtr.
  /// Used for the legacy whole-2MiB-pool seal mode (the bare pointer is aligned
  /// to its 2MiB pool base and that page is sealed). In 4K page-seal mode the
  /// split + per-page seal callbacks below are used instead.
  using PrepareCodeCallback = bool (*)(void *ctx, const void *fnPtr);
  /// Owner-private provider that resolves a freshly compiled function pointer
  /// to its real, finalized executable range + owning pool (from the code-pool
  /// allocation metadata). The owner records it into the shared cache slot at
  /// publish so a peer core can later seal exactly the 4KiB pages the code
  /// covers. Returns false when no range is known (clean fallback). Optional:
  /// when unset, slots carry no range and 4K peer preparation cleanly fails.
  using CodeRangeCallback = bool (*)(void *ctx, const void *fnPtr,
                                     EJitCompiledCodeInfo *outInfo);
  /// Owner-private provider that snapshots the owner-core code-pool manager
  /// stats. The owner publishes this into the shared mirror after every
  /// successful compile so every core's ejit_print_code_pool_stats is
  /// consistent (the real pools are owner-private). Returns false when no code
  /// pool exists (clean fallback). Optional: when unset, the shared mirror
  /// stays zero and readers fall back to their per-core (empty) view.
  using CodePoolStatsCallback = bool (*)(void *ctx, EJitCodePoolStatsOut *out);
  /// Batched code-pool hooks. isReady is false while a linked address remains
  /// RW/NX; flush seals the current owner-private batch and makes every staged
  /// address resolvable through CodeRangeCallback.
  using CodeReadyCallback = bool (*)(void *ctx, const void *fnPtr);
  using CodeBatchFlushCallback = bool (*)(void *ctx);
  /// Per-core platform primitive: split a 2MiB-aligned [poolBase, poolBase +
  /// poolSize) window into 4KiB mappings in the CALLING core's translation
  /// context (split_2m_to_4k). Returns true on success. Used only in 4K
  /// page-seal mode.
  using SplitPoolCallback = bool (*)(void *ctx, uintptr_t poolBase,
                                     uint64_t poolSize);
  /// Per-core platform primitive: seal one 4KiB page at \p pageVA to RX in the
  /// CALLING core's translation context (enable_ex). Returns true on success.
  using SealPageCallback = bool (*)(void *ctx, uintptr_t pageVA);
  /// Per-core platform primitive: make one 4KiB page at \p pageVA writable
  /// (RX -> RW, enable_rw) in the CALLING core's translation context. Returns
  /// true on success. Used only in 4K page-seal mode, for the runtime-writable
  /// data pages of a JIT function (e.g. the Tier-1 __profc_ counters), so a
  /// non-owner core running from the fixed RX .text.ejit segment may execute
  /// code that writes them without a write-permission abort. Applied ONLY to
  /// writable, non-executable pages (page-disjoint from the code), so it never
  /// makes an executable page writable (no RWX).
  using EnableRwPageCallback = bool (*)(void *ctx, uintptr_t pageVA);

  /// Worker loop entry (provided by this class, run on the injected task).
  using WorkerEntryFn = void (*)(void *ctx);
  /// Owner-injected worker starter: create ONE task running \p entry(\p
  /// entryCtx); store its id in *outTaskId; return false on failure.
  using WorkerStartFn = bool (*)(void *startCtx, WorkerEntryFn entry,
                                 void *entryCtx, uint64_t *outTaskId);
  /// Owner-injected worker stopper: soft-stop and JOIN the task. Must not
  /// return until the worker has exited (no use-after-free of owner-private
  /// state).
  using WorkerStopFn = void (*)(void *startCtx);
  /// Idle/delay hook the worker calls in two situations: (1) whenever it has no
  /// work to do (waiting on the owner to publish Ready, or Ready with an empty
  /// queue), passed \p ticks=1 for a single yield; (2) after EVERY consumed
  /// compile task as a throttle delay, passed \p ticks = MULT*DELAY_TICKS -- a
  /// single delay(ticks) call, NOT ticks separate yield() calls. A producer
  /// side init() also uses it with \p ticks=1 while waiting on an Initializing
  /// owner or (fixed worker core build) on the designated core to initialize.
  /// The production
  /// build injects EJitSreTask::delay(ticks) (delay(1) == yield():
  /// SRE_TaskDelay on freestanding, std::this_thread::yield on host) so a
  /// high-priority worker never busy-spins and starves the core trying to
  /// publish Ready. MUST NOT be called while holding a bucket lock / queue slot
  /// / dedup critical state.
  using WorkerIdleFn = void (*)(void *ctx, uint32_t ticks);
  /// Owner-only setup hook (see setOwnerElectedCallback). Return false to fail
  /// init. Runs on the elected owner, inside init(), before the worker starts.
  using OwnerElectedFn = bool (*)(void *ctx);
  /// Owner-only teardown hook (see setOwnerReleasedCallback). Runs on the core
  /// giving up ownership, inside ownerShutdown().
  using OwnerReleasedFn = void (*)(void *ctx);
  /// Owner-private diagnostic callback. It runs only on the owner worker and
  /// may access owner-local optimizer state.
  using MayConstRankingCallback = bool (*)(void *ctx);
#ifdef EJIT_SRE_TASKPOOL_TESTING
  using TestHookFn = void (*)(void *ctx);
#endif

  enum class InitResult : uint32_t {
    BecameOwner =
        0,         ///< Won election; built state; worker started (if injected).
    AttachedReady, ///< Bound to an already-Ready shared state.
    OwnerFailed,   ///< State is Failed/Stopping: clean fallback, no wait.
    InitInProgress, ///< Another core still Initializing; bounded retry hit.
                    ///< Under a fixed worker core build this is also returned
                    ///< when the designated core has not initialized yet (or
                    ///< never does) - pending, never a hang, never an election
                    ///< by a non-designated core.
    AbiMismatch,    ///< magic/version/size mismatch — refuse to use the blob.
    FingerprintMismatch, ///< owner/peer registration mapping differs — clean
                         ///< fail.
    NoState,             ///< bind() not called.
  };

  /// Returned by every lookup, so its size is on the per-call hot path: AAPCS64
  /// returns an aggregate <= 16 bytes in x0:x1 but passes anything larger via
  /// sret, i.e. the callee stores the fields to caller stack and the caller
  /// loads them straight back. Two things keep this at 16: fnPtr comes first,
  /// so its alignment forces no padding ahead of it, and bucketIndex is a byte,
  /// which leaves the three flags exactly filling the tail.
  struct CompileOrGetResult {
    void *fnPtr = nullptr;
    EJitCompileOrGetStatus status = EJitCompileOrGetStatus::CompileFailed;
    /// Narrowed to a byte purely to close the struct at 16 bytes (see above);
    /// it holds 0..kEJitSharedCacheBuckets, the latter being the out-of-range
    /// sentinel. A byte field costs a plain ldrb/strb, whereas packing these
    /// four into a bit-field would trade the sret round-trip for ubfx/bfi
    /// masking on every access.
    uint8_t bucketIndex = 0;
    bool hasReadToken = false;
    /// True when a Ready result exists but this core may not read the
    /// cross-core pointer (code sharing not platform-validated): a clean
    /// fallback that did NOT re-enqueue.
    bool readyButNotShareable = false;
    /// Set by tryCacheHit() when the request was fully resolved on the fast
    /// cache-hit path (a cache hit, a disabled instance, a not-yet-Ready pool,
    /// or a ready-but-not-shareable fallback). When true the caller returns
    /// this result directly and MUST NOT enter the compileOrGet() slow path.
    /// When false the request is a true miss that still needs compileOrGet().
    /// This flag is an internal control signal; it does not affect status
    /// mapping or the C ABI.
    bool fastPathTerminal = false;
  };
  static_assert(kEJitSharedCacheBuckets < 255,
                "bucketIndex is a uint8_t: the bucket count and its sentinel "
                "must fit in a byte");
  static_assert(sizeof(CompileOrGetResult) <= 16,
                "CompileOrGetResult must stay <= 16 bytes so AAPCS returns it "
                "in registers rather than through sret memory");

  EJitSharedTaskPool() = default;
  EJitSharedTaskPool(const EJitSharedTaskPool &) = delete;
  EJitSharedTaskPool &operator=(const EJitSharedTaskPool &) = delete;

  /// Bind to the shared blob (not owned). Call before init().
  void bind(EJitSharedTaskPoolState *state) {
    state_ = state;
    // Seal mode / prepareCode may have been configured before the blob existed.
    publishIcachePrepareMode();
    // Likewise a releaser wired pre-bind must count against the shared table.
    syncIcacheReleaserCount();
  }
  EJitSharedTaskPoolState *state() const { return state_; }

  /// Callback type for forEachCompiled: receives the Ready cache slot itself
  /// (funcIndex/dims/numDims/fnPtr plus publish metadata such as versions,
  /// codeSize, poolId, generation) and the caller-provided context.
  using CompiledFuncCallback = void (*)(const EJitSharedCacheSlot &slot,
                                        void *ctx);

  /// Walk outcome of forEachCompiled, so diagnostics can report completeness
  /// instead of silently missing contended buckets.
  struct ForEachCompiledStats {
    uint32_t visitedSlots = 0;   ///< Ready slots the callback ran for.
    uint32_t skippedBuckets = 0; ///< Buckets skipped (write lock contention).
  };

  /// Invoke \p cb once for every successfully compiled (Ready) cache entry.
  /// For diagnostics (e.g. ejit_taskpool_print_compiled). Best-effort: a slot
  /// mid-publish is skipped, and this is not a snapshot — concurrent publishes
  /// may add entries during iteration. A bucket whose write lock stays held
  /// after a brief retry is skipped and reported in the returned stats. In a
  /// NO_RECLAIM build the slot is read without the hit-path seqlock snapshot,
  /// so a publish racing the write-flag check can tear a printed field;
  /// diagnostics only, never a hit path.
  ForEachCompiledStats forEachCompiled(CompiledFuncCallback cb,
                                       void *ctx) const;

  //--- owner-only configuration (applied if this core wins election) ----------
  void setCompiler(CompileCallback fn, void *ctx) {
    compileFn_ = fn;
    compileCtx_ = ctx;
  }
  void setPublishCallback(PublishCallback fn, void *ctx) {
    publishFn_ = fn;
    publishCtx_ = ctx;
  }
  void setMayConstRankingCallback(MayConstRankingCallback fn, void *ctx) {
    mayConstRankingFn_ = fn;
    mayConstRankingCtx_ = ctx;
  }
  void setReleaser(ReleaseCallback fn, void *ctx) {
    const bool had = (releaseFn_ != nullptr);
    releaseFn_ = fn;
    releaseCtx_ = ctx;
    // v2 inline cache never reclaims (no HP-scan retire, ever). A wired
    // releaser means code may be freed while a cached fnPtr still pins it ->
    // UAF. Auto-disable the cache while a releaser is wired. Production wires
    // no releaser, so the gate stays open and the cache is unconditionally
    // safe.
    //
    // The disable state is SHARED, not just this facade's: the cells are one
    // table backing every core, so a peer facade with its own flag still clear
    // would happily refill what this one just drained, and the probe consults
    // no gate at all. Track the transition as a count in the blob so the cache
    // re-arms only when the LAST releaser goes away.
    (void)had;
    icacheReclamationSafe_ = (fn == nullptr);
    syncIcacheReleaserCount();
    if (fn) {
      // The gate is evaluated at fill, so blocking new fills is not enough:
      // entries armed earlier would keep serving code the releaser may free.
      // retireDispatchCache() drains the cell table as part of retiring the L0,
      // so this needs no second icacheDrainAll(): that would walk the whole
      // table twice and bump icacheDrainSeq twice for one event.
      gEJitL0State = nullptr;
      retireDispatchCache();
    }
  }
  void setPrepareCodeCallback(PrepareCodeCallback fn, void *ctx) {
    prepareCodeFn_ = fn;
    prepareCodeCtx_ = ctx;
    publishIcachePrepareMode();
  }
  /// Whether a resolved fnPtr is callable on EVERY core the instant it exists,
  /// with no per-core work. False when the platform wires per-core execute
  /// preparation (legacy 2M `prepareCodeFn_`, or 4K-seal mode, where a core
  /// must split its pool and seal the code's pages before executing a peer's
  /// code).
  ///
  /// This is the precondition of a SHARED cell table, and what the per-core
  /// table gave for free: a non-null cell there implied THIS core had resolved,
  /// hence prepared. A shared fill publishes to every core at once, and the
  /// probe has no per-core gate to restore the implication -- so when per-core
  /// preparation is required icacheFill declines and the taskpool, which
  /// prepares, serves every call.
  /// The reclamation gate as every core sees it: this facade's own releaser
  /// AND any peer's. The cells are shared, so a peer wiring a releaser must
  /// stop THIS core filling and serving them too.
  bool icacheReclamationSafeShared() const {
    return icacheReclamationSafe_ &&
           (!state_ || state_->icacheReleasersWired.loadAcquire() == 0);
  }
  bool icacheCrossCoreExecutable() const {
    // The shared answer wins. A facade that was never handed the seal mode has
    // fourKSeal_ == false and no prepareCodeFn_, and would otherwise conclude
    // "callable everywhere" for a platform on which it is not -- see
    // icachePerCorePrepare.
    if (state_ && state_->icachePerCorePrepare.loadAcquire())
      return false;
    // 4K-seal mode is NOT counted as per-core preparation. On the target this
    // runs on, the seal is an operation on an address space every core
    // translates through, so a page sealed by one core is executable on all of
    // them; the per-core split the taskpool still performs is bookkeeping, not
    // a precondition for the jump. Counting it here left the 0-dim shared
    // scalar permanently unfillable on the only platform the inline cache ships
    // on, which costs every 0-dim entry the full taskpool path on every call.
    //
    // A wired prepareCodeFn_ (the legacy whole-2MiB path) IS per-core work and
    // still closes the gate.
    //
    // NOTE, and it is the assumption to re-check if a 0-dim entry ever faults:
    // EJitSharedPoolSplit tracks splitDoneMask per core, which is the runtime
    // modelling the split as per-core state. That is consistent with the split
    // being per-core bookkeeping over a shared mapping, but it is also what you
    // would see if the mapping were NOT shared. Verified on the board by
    // ejit_icache_multiverify_test's 0-dim entry (f_0) executing on cores that
    // never resolved it themselves.
    return prepareCodeFn_ == nullptr;
  }
  /// Owner: provide the finalized code-range resolver (see CodeRangeCallback).
  void setCodeRangeProvider(CodeRangeCallback fn, void *ctx) {
    codeRangeFn_ = fn;
    codeRangeCtx_ = ctx;
  }
  /// Owner: provide the code-pool stats snapshotter (see
  /// CodePoolStatsCallback).
  void setCodePoolStatsProvider(CodePoolStatsCallback fn, void *ctx) {
    codePoolStatsFn_ = fn;
    codePoolStatsCtx_ = ctx;
  }
  void setCodeBatchCallbacks(CodeReadyCallback ready,
                             CodeBatchFlushCallback flush, void *ctx) {
    // Readiness without a flush would leave slots Pending forever. Treat any
    // partial configuration as batching disabled.
    if (!ready || !flush) {
      codeReadyFn_ = nullptr;
      codeBatchFlushFn_ = nullptr;
      codeBatchCtx_ = nullptr;
      return;
    }
    codeReadyFn_ = ready;
    codeBatchFlushFn_ = flush;
    codeBatchCtx_ = ctx;
  }
  /// Read the shared code-pool stats mirror (last owner-published snapshot).
  /// Returns false if the shared state is not bound. Every core sees the same
  /// values. Use this for ejit_get/print_code_pool_stats in shared builds.
  bool readCodePoolStats(EJitCodePoolStatsOut *out) const;
  /// Select the execute-permission seal granularity for non-owner preparation:
  /// true = 4KiB page seal (split the pool once per core, then enable_ex every
  /// page the code covers), false = legacy whole-2MiB-pool seal. Must match the
  /// owner's code pool. Default false.
  void setSealMode(bool fourKSeal) {
    fourKSeal_ = fourKSeal;
    publishIcachePrepareMode();
  }
  /// 4K mode: per-core split primitive (see SplitPoolCallback).
  void setSplitPoolCallback(SplitPoolCallback fn, void *ctx) {
    splitPoolFn_ = fn;
    splitPoolCtx_ = ctx;
  }
  /// 4K mode: per-core per-page seal primitive (see SealPageCallback).
  void setSealPageCallback(SealPageCallback fn, void *ctx) {
    sealPageFn_ = fn;
    sealPageCtx_ = ctx;
  }
  /// 4K mode: per-core per-page enable_rw primitive for runtime-writable data
  /// (see EnableRwPageCallback). Optional: when unset, a slot that carries
  /// runtime-writable ranges cannot be prepared on a peer core (clean fallback,
  /// no fnPtr) rather than executing code whose counter writes would fault.
  void setEnableRwPageCallback(EnableRwPageCallback fn, void *ctx) {
    enableRwPageFn_ = fn;
    enableRwPageCtx_ = ctx;
  }
  void setWorkerHooks(WorkerStartFn start, WorkerStopFn stop, void *ctx) {
    workerStart_ = start;
    workerStop_ = stop;
    workerCtx_ = ctx;
  }
  /// Owner-only setup, run inside init() by the core that WINS the election,
  /// after the blob is built but before the worker is started and before Ready
  /// is published. Returning false is a clean init failure (Failed +
  /// OwnerSetupFailed), exactly like a failed worker start.
  ///
  /// This exists so the JIT engine is built only on the core that will actually
  /// compile. Only the owner's worker ever invokes the compile callback, so
  /// constructing an LLJIT on every core wastes one per non-owner. Ownership is
  /// won by CAS and is not permanent (see ownerShutdown), so the engine cannot
  /// be assigned to a fixed core -- it has to follow whoever wins, which is
  /// what this hook expresses.
  ///
  /// Placement is load-bearing: the worker may compile the instant it starts,
  /// and peers may enqueue the instant Ready is published, so the engine must
  /// exist before both.
  ///
  /// LIFETIME: \p fn and \p ctx MUST stay valid for the pool's whole lifetime,
  /// and \p fn MUST be idempotent. An election is not a one-shot event:
  /// ownerShutdown() returns the blob to Uninitialized precisely so a later
  /// init() can elect a different existing peer, which runs this hook on ITS
  /// pool object. So ctx must be a lifetime-stable object, never a caller's
  /// stack, and the hook must not be cleared after the first election.
  void setOwnerElectedCallback(OwnerElectedFn fn, void *ctx) {
    ownerElected_ = fn;
    ownerElectedCtx_ = ctx;
  }
  /// The counterpart of setOwnerElectedCallback: release whatever that hook
  /// built. Runs inside ownerShutdown() AFTER the worker is joined and BEFORE
  /// the blob returns to Uninitialized, so no compile can be in flight and no
  /// peer can have been elected yet.
  ///
  /// Quiescence: what this releases is the COMPILER, not the code. Code-pool
  /// memory is never recycled (sealed RX pages are not reused), so
  /// specializations already published stay executable for peers still running
  /// them after the engine is gone. Same lifetime rules as the elected hook:
  /// stable ctx, armed for the pool's lifetime.
  void setOwnerReleasedCallback(OwnerReleasedFn fn, void *ctx) {
    ownerReleased_ = fn;
    ownerReleasedCtx_ = ctx;
  }
  /// Inject the worker idle/delay hook (see WorkerIdleFn). When unset the loop
  /// falls back to a compiler reordering barrier only (used by step tests). The
  /// hook receives a tick count: 1 for a yield, MULT*DELAY_TICKS for the
  /// post-task throttle delay.
  void setWorkerIdleHook(WorkerIdleFn fn, void *ctx) {
    workerIdle_ = fn;
    workerIdleCtx_ = ctx;
  }
  /// Owner publishes this digest of its funcIndex/dimType registration mapping
  /// into the shared state; a peer attaching to a Ready blob compares its own
  /// digest and cleanly fails (FingerprintMismatch) on any divergence, so a
  /// core with a different mapping never submits requests against the wrong
  /// indices (spec §11). 0 means "unknown / not checked".
  void setRegistrationFingerprint(uint64_t fp) { regFingerprint_ = fp; }
  /// Platform capability: may a NON-owner core read a cache fnPtr? Only true
  /// when the code pool is mapped at the same VA on every core, sealed, and
  /// I/D-cache coherent for cross-core execution (spec §11 fnPtr
  /// prerequisites).
  void setCodeSharingEnabled(bool enabled) { codeSharingEnabled_ = enabled; }
  /// Owner-only PRE-INIT configuration: the mode the owner publishes into the
  /// shared state during init(). After the blob is Ready this only stages the
  /// desired mode; use setSharedMode() to change the live cross-core mode.
  void setMode(EJitCompileMode mode) { configuredMode_ = mode; }

  /// PGO (§6): enable the online-PGO Tier-2 auto-trigger on the shared
  /// taskpool.  When enabled, every cache hit atomically increments the
  /// slot's hitCount; the hit that crosses \p threshold arms a one-shot
  /// Tier-2 (PGOUse) lazy recompile via enqueue. \p threshold 0 disables
  /// the trigger (hits are still counted).
  void setPgoEnabled(
      bool enable, uint32_t threshold,
      uint32_t maxConcurrentProfiles = EJIT_SRE_PGO_MAX_CONCURRENT_PROFILES) {
    maxConcurrentProfiles = std::max(
        1u, std::min(maxConcurrentProfiles, kEJitSharedMaxConcurrentProfiles));
    pgoEnabled_.storeRelaxed(enable ? 1 : 0);
    tier2Threshold_.storeRelaxed(enable ? threshold : 0u);
    pgoMaxConcurrentProfiles_.storeRelaxed(maxConcurrentProfiles);
    if (!state_ || state_->initState.loadAcquire() !=
                       static_cast<uint32_t>(EJitSharedInitState::Ready))
      return;

    // Publish the threshold before enabling so a peer that acquires the
    // enabled flag also observes the matching threshold. Disable first when
    // turning PGO off so no new hit can arm a Tier-2 request.
    if (enable) {
      state_->tier2Threshold.storeRelease(threshold);
      state_->pgoMaxActiveFunctions.storeRelease(maxConcurrentProfiles);
      state_->pgoEnabled.storeRelease(1);
    } else {
      state_->pgoEnabled.storeRelease(0);
      state_->tier2Threshold.storeRelease(0);
    }
    // L0 hits bypass the shared slot hitCount. Retire every core's existing L0
    // entries whenever PGO control changes; l0Fill() remains disabled while
    // PGO is enabled so calls continue through the Tier-2 trigger path.
    state_->dispatchEpoch.fetchAdd(1);
  }

#ifdef EJIT_SRE_TASKPOOL_TESTING
  void setPgoAdmissionTestHook(TestHookFn fn, void *ctx) {
    pgoAdmissionTestHook_ = fn;
    pgoAdmissionTestHookCtx_ = ctx;
  }
#endif

  /// True when the shared PGO auto-trigger is armed.
  bool isPgoEnabled() const {
    if (state_ && state_->initState.loadAcquire() ==
                      static_cast<uint32_t>(EJitSharedInitState::Ready))
      return state_->pgoEnabled.loadAcquire() != 0;
    return pgoEnabled_.loadRelaxed() != 0;
  }

  /// Return true when this miss may start a staged PGO function. Only one
  /// specialization of a funcIndex may own admission at a time; later versions
  /// stay on AOT until the current Tier-2 finishes.
  bool admitPgoFunction(uint32_t funcIndex, bool &newlyAdmitted);

  //--- compile mode: CROSS-CORE SHARED runtime state --------------------------
  /// Publish the compile/taskpool mode as cross-core shared runtime state.
  /// Compile mode is a shared control flag (engine/worker ownership stays
  /// owner-private): once the blob is Ready any core may flip it and every
  /// core's compileOrGet() observes it through an acquire load of state_->mode.
  /// A mode flip is a pure control flag — it never touches the queue, dedup,
  /// cache, owner election, or the single worker.
  ///   * If the blob is Ready, write state_->mode with RELEASE semantics so the
  ///     new mode is visible to every core (including peers/other cores), not
  ///     only to the owner object.
  ///   * If the blob is not yet initialized, only stage configuredMode_ so the
  ///     owner publishes the desired mode during init().
  void setSharedMode(EJitCompileMode mode) {
    configuredMode_ = mode;
    if (state_ && state_->initState.loadAcquire() ==
                      static_cast<uint32_t>(EJitSharedInitState::Ready))
      state_->mode.storeRelease(static_cast<uint32_t>(mode));
  }
  /// Publish \p mode only if the blob is still Ready at generation \p gen --
  /// the one asyncServiceAvailable() validated. An ownerShutdown that lands in
  /// between bumps the generation, so the stale mode is never committed and the
  /// caller learns the switch did not take. Returns false without writing then.
  bool publishSharedMode(EJitCompileMode mode, uint32_t gen) {
    if (!state_)
      return false;
    if (state_->initState.loadAcquire() !=
            static_cast<uint32_t>(EJitSharedInitState::Ready) ||
        state_->generation.loadAcquire() != gen)
      return false;
    configuredMode_ = mode;
    state_->mode.storeRelease(static_cast<uint32_t>(mode));
    // Re-check: if the owner went Stopping between the gate and the store, the
    // write is stale. Producers gate on Ready before enqueuing so nothing can
    // act on it, but report the failure rather than claim the switch took.
    return state_->initState.loadAcquire() ==
               static_cast<uint32_t>(EJitSharedInitState::Ready) &&
           state_->generation.loadAcquire() == gen;
  }

  /// The current cross-core compile mode: the shared state's mode (acquire
  /// load) once the blob is Ready, otherwise the staged configuredMode_.
  EJitCompileMode getSharedMode() const {
    if (state_ && state_->initState.loadAcquire() ==
                      static_cast<uint32_t>(EJitSharedInitState::Ready))
      return static_cast<EJitCompileMode>(state_->mode.loadAcquire());
    return configuredMode_;
  }

  /// Run owner election + bind. Idempotent: re-observes the same outcome.
  /// Under a fixed worker core build (EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE)
  /// only the designated core may claim ownership; every other core waits
  /// bounded for it and then attaches (see the file header).
  InitResult init();
  bool isOwner() const { return isOwner_; }

  /// Can an async request submitted from THIS core actually be compiled? True
  /// when the blob is Ready, an owner is elected, and that owner's single
  /// worker started. Says nothing about a LOCAL engine: peers have none by
  /// design and the owner compiles for every core (see EJit::setCompileMode).
  ///
  /// The fields are re-read after the check and the whole thing is rejected if
  /// the generation or state moved, so the answer describes ONE generation
  /// rather than a mix of before- and after-shutdown reads. \p outGeneration
  /// receives that generation; pass it to publishSharedMode() so the mode can
  /// only commit against the same one.
  bool asyncServiceAvailable(uint32_t *outGeneration = nullptr) const {
    if (!state_)
      return false;
    const uint32_t gen = state_->generation.loadAcquire();
    if (state_->initState.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedInitState::Ready))
      return false;
    if (state_->ownerCoreId.loadAcquire() == kEJitInvalidCoreId)
      return false;
    // Published only after a successful worker start, cleared by
    // ownerShutdown: the one field separating "a worker is running" from "the
    // blob merely looks Ready".
    if (state_->workerTaskId.loadAcquire() == 0)
      return false;
    // Re-validate: an ownerShutdown concurrent with the reads above moves the
    // state to Stopping and bumps the generation.
    if (state_->initState.loadAcquire() !=
            static_cast<uint32_t>(EJitSharedInitState::Ready) ||
        state_->generation.loadAcquire() != gen)
      return false;
    if (outGeneration)
      *outGeneration = gen;
    return true;
  }

  /// Owner-only orderly shutdown: stop+join the worker, then return the state
  /// to Uninitialized so a later init() can re-elect. No-op for a non-owner.
  void ownerShutdown();

  //--- producer path ----------------------------------------------------------
  /// Submit a request carrying borrowed raw bound-pointer descriptors. Only
  /// the fixed descriptors are copied into the queue; pointee bytes are read
  /// by the compile callback and are never owned by this pool.
  CompileOrGetResult compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                  uint32_t numDims, void *fallback,
                                  const EJitBoundPtrDescriptor *boundPointers,
                                  uint32_t boundCount);

  /// Source-compatible single-pointer entry point. A zero size means no bound
  /// pointer; a nonzero size borrows the pointed-to object through compilation.
  CompileOrGetResult compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                  uint32_t numDims, void *fallback,
                                  const void *rawPtr = nullptr,
                                  uint32_t rawSize = 0,
                                  uint32_t boundArgIndex = 0) {
    EJitBoundPtrDescriptor Bound{rawPtr, rawSize, boundArgIndex};
    return compileOrGet(funcIndex, dims, numDims, fallback,
                        rawSize ? &Bound : nullptr, rawSize ? 1u : 0u);
  }
  /// Flattened fast cache-hit path (spec §5.2 steps 0-1). Performs ONLY the
  /// terminal front half of compileOrGet(): the Ready check, the
  /// instance-enabled check, and the cache lookup, then classifies the outcome:
  ///   * CacheHit           — returns fnPtr + bucketIndex + a held read token
  ///                          (caller releases via releaseRead), cacheHits++.
  ///   * InstanceDisabled   — a disabled dim, instanceDisabled++.
  ///   * OffMode            — the pool is not Ready (clean fallback).
  ///   * OffMode + readyButNotShareable — Ready code this core may not read;
  ///                          NO enqueue / dedup.
  /// Each of the above sets fastPathTerminal = true; the caller returns the
  /// result directly and never enters the slow path. A true miss (Ready,
  /// enabled, no shareable cached code) returns fastPathTerminal = false and
  /// the caller must fall through to compileOrGet(). compileOrGet() itself
  /// calls this so the ordering/counters/semantics stay identical.
  CompileOrGetResult
  tryCacheHit(uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims,
              const EJitBoundPtrDescriptor *boundPointers = nullptr,
              uint32_t boundCount = 0);
  /// Fixed-dimension fast cache-hit entries (0-4 dims). Same terminal
  /// semantics as tryCacheHit() but the instance-enabled check is unrolled and
  /// the dim identity is built directly on the stack (no numDims loop / no
  /// variable-length array handling), so the C ABI fixed-dimension entries
  /// (ejit_taskpool_compile_or_get_Nd) reach the cache lookup with the least
  /// overhead. The cache lookup itself is still the shared generic
  /// cacheLookup(). 4 is the maximum dimension count (numDims > 4 is rejected
  /// by the C ABI); higher-dimension callers keep using tryCacheHit().
  CompileOrGetResult tryCacheHit0D(uint32_t funcIndex);
  CompileOrGetResult tryCacheHit1D(uint32_t funcIndex, uint32_t dim0,
                                   uint32_t inst0);
  CompileOrGetResult tryCacheHit2D(uint32_t funcIndex, uint32_t dim0,
                                   uint32_t inst0, uint32_t dim1,
                                   uint32_t inst1);
  CompileOrGetResult tryCacheHit3D(uint32_t funcIndex, uint32_t dim0,
                                   uint32_t inst0, uint32_t dim1,
                                   uint32_t inst1, uint32_t dim2,
                                   uint32_t inst2);
  CompileOrGetResult tryCacheHit4D(uint32_t funcIndex, uint32_t dim0,
                                   uint32_t inst0, uint32_t dim1,
                                   uint32_t inst1, uint32_t dim2,
                                   uint32_t inst2, uint32_t dim3,
                                   uint32_t inst3);
  void releaseRead(uint32_t bucketIndex);
  /// Drive one end of a period-value mutation window for a lifecycle instance.
  ///
  /// The shared `enabled` bit is the JIT compile gate and is CAS'd, so only the
  /// first caller in each direction moves it.
  ///
  /// The INVALIDATIONS (icache drain + L0 dispatchEpoch bump) run on EVERY
  /// call. Period data is core-private while the specialization is SHARED, so N
  /// cores each bracket their own writes over one shared bit: a caller that
  /// LOST the CAS may still have rewritten its own copy, and neither cache
  /// stores a version.
  ///
  /// version[] moves ONLY on a real transition. Its consumer is runCompile's
  /// checkpoints, which DISCARD a finished compile when it changes, and nothing
  /// re-enqueues a dropped compile -- so an unconditional bump would let N
  /// cores activating the same instance at startup stall the JIT permanently.
  ///
  /// \returns whether the enabled BIT flipped. The invalidations run either
  /// way.
  bool setInstanceEnabled(uint32_t dimType, uint32_t instanceId, bool enabled);

  /// Toggle EVERY instance of \p dimType, draining ONCE at the end.
  ///
  /// setInstanceEnabled drains the whole table per call, so looping it cost
  /// MAX_INSTANCES full drains (256 x 256 cells per function for a 2-dim entry)
  /// with the in-flight counter raised throughout, blocking concurrent fills.
  /// The drain is global, not per instance, so one covers the batch.
  ///
  /// \returns the number of instances whose enabled bit actually flipped.
  uint32_t setAllInstancesEnabled(uint32_t dimType, bool enabled);
  /// Query the shared activation bit for a lifecycle instance — the read
  /// counterpart of setInstanceEnabled, and the single cross-core source of
  /// truth the compile gate (compileCold) and ejit_is_active consult. Returns
  /// false for an out-of-range dimType/instanceId (never reads out of bounds).
  bool isInstanceActive(uint32_t dimType, uint32_t instanceId) const;

  //--- per-function inline cache (multi-version direct-indexed) --------------
  // NOTE: the production hit path does NOT use icacheTry. With -ejit-inline-cache
  // the ejit_entry wrapper reads its per-function @__ejit_icache_fn_<name> slot
  // directly - a GEP into the [D]^numDims array by the ejit_dim arg values, one
  // load + indirect call, NO ejit_icache_try call, NO
  // per-call guards; for NumDims <= 2 (no timing) the table is sentinel-formed,
  // so even the null-check is gone - a miss BLRs into MissFn itself. icacheTry
  // is retained for unit tests / diagnostics: on a
  // hit it sets *outFn to the cached specialization for the given dims (call
  // with NO releaseRead) and returns true; on a miss returns false. It keeps
  // the reclamation-safety, pool-Ready, range, and cross-core code-sharing gates
  // (the latter matters in non-shared test builds; the wrapper's inline probe is
  // only enabled under EJIT_SRE_SHARED_CODE_POINTERS, where the gate is
  // compile-time true).
  bool icacheTry(uint32_t funcIndex, const EJitDimPair *dims,
                 uint32_t numDims, void **outFn);
  // Fill the per-function icache cell at [i0][i1]... (linearized from \p dims,
  // row-major, dim0 = leftmost ejit_dim param - MUST match the AOT array order)
  // with a freshly resolved specialization (call on a taskpool cache hit or a
  // successful compile). One-shot per cell: the first resolver for an identity
  // wins; later resolves of the same identity (same invariant pointer) no-op;
  // a different identity fills a different cell. No-op when reclamation is not
  // safe, the function is unregistered (no base wired) or numDims mismatches,
  // funcIndex is out of range, or fnPtr is null.
  /// Per-core L0 dispatch cache: serves the (funcIndex, dims) identity the
  /// bucket lookup would resolve, skipping the rwlock and the 16-slot scan.
  ///
  /// A hit hands back a raw fnPtr with NO read token, so it runs under the
  /// same gate as the inline cache (icacheReclamationSafe_): with a releaser
  /// wired, code can be freed under the caller and the cache auto-disables.
  /// Callers receive kEJitNoBucket, which releaseRead() ignores.
  bool l0Try(uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims,
             void **outFn) {
    // Inline: an out-of-line call costs more than the probe it performs.
    if (!state_ || gEJitL0State != state_ || numDims > kEJitSharedMaxDims)
      return false;
    EJitL0Entry &e = gEJitL0[ejitL0Index(funcIndex, dims, numDims)];

    // Seqlock: core-private storage excludes other CORES, not preemption or
    // interrupts on this one, which could otherwise leave a stale identity
    // beside a new fnPtr. Signal fences suffice -- same-core ordering, not
    // cross-core visibility.
    const uint32_t s0 = e.seq;
    if (s0 & 1u)
      return false;
    std::atomic_signal_fence(std::memory_order_acquire);

    void *fn = e.fn;
    const bool match =
        fn != nullptr && e.epoch == state_->dispatchEpoch.loadRelaxed() &&
        e.core == EJitCoreId::current() && e.funcIndex == funcIndex &&
        e.numDims == numDims && dimsEqual(e.dims, dims, numDims);

    std::atomic_signal_fence(std::memory_order_acquire);
    if (e.seq != s0 || !match)
      return false;

    EJIT_STAT_INC(state_->counters.cacheHits);
    *outFn = fn;
    return true;
  }

  void l0Fill(uint32_t funcIndex, void *fnPtr, const EJitDimPair *dims,
              uint32_t numDims);

  /// Retire every core's L0 AND drain the shared inline cache. Must be called
  /// from every path that can invalidate an (identity -> fnPtr) mapping from
  /// OUTSIDE the taskpool -- ejit_clear_cache(), ejit_invalidate(), a
  /// compile-mode change -- since those bypass cachePublish() and
  /// setInstanceEnabled(). Both caches answer the same question and neither
  /// re-validates on read, so they are retired together or not at all.
  void retireDispatchCache();

  /// Open a resolve window: returns a token pinning the shared drain state, to
  /// be handed back to icacheFill(). Called at every taskpool entry point that
  /// can end in a fill, BEFORE the resolve and before any bucket read token.
  /// kEJitIcacheNoResolve when a drain is already in flight (its reach is
  /// unknown, so no fill from this resolve may be trusted).
  ///
  /// The token is a VALUE the caller keeps on its stack, not runtime state: on
  /// an RTOS a higher-priority task can preempt this core mid-resolve and run
  /// its own resolve, which would clobber any core-private snapshot.
  uint64_t icacheBeginResolve();

  /// Publish \p fnPtr into the cell for \p dims. Dropped unless \p token, from
  /// the icacheBeginResolve() that opened this resolve, shows no drain
  /// overlapped it.
  void icacheFill(uint32_t funcIndex, void *fnPtr, const EJitDimPair *dims,
                  uint32_t numDims, uint64_t token);

#ifdef EJIT_SRE_TASKPOOL_TESTING
  /// Test-only hook fired inside icacheFill() AFTER the cell store and BEFORE
  /// the post-store re-validation, so a single-threaded test can interleave a
  /// drain exactly where a preempting peer core would - the only way to reach
  /// the retract deterministically (the pre-store checks decline a token that
  /// is already stale when icacheFill is entered).
  using IcacheFillMidpointHook = void (*)(void *ctx);
  void setIcacheFillMidpointForTest(IcacheFillMidpointHook fn, void *ctx) {
    icacheFillMidpointHook_ = fn;
    icacheFillMidpointCtx_ = ctx;
  }
#endif

  /// Empty every cell of every registered icache slot, bracketed by
  /// icacheDrainsInFlight and closed by an icacheDrainSeq bump, so any resolve
  /// this overlaps drops its fill. This is THE cross-core
  /// invalidation: the cells are shared, so clearing them here clears them for
  /// every core, including one that is permanently hot and would never reach a
  /// runtime entry point of its own.
  ///
  /// Cost is O(cells registered) -- proportional to the table the build chose
  /// to allocate. Toggles are rare; paying the refill once per toggle is what
  /// buys a probe with no freshness check.
  ///
  /// Safe against a concurrent probe on any core: it reads the old pointer (and
  /// calls it once more; the code is never freed under the gate this cache
  /// requires) or the empty value (and misses -- for sentinel-form tables the
  /// empty value IS MissFn, so the miss executes the slow path directly).
  /// \p reason names the event that triggered it
  /// and appears in the EJIT_DIAG line, which is what makes a drain visible on
  /// an SRE board -- the probe never enters the runtime on a hit, so a sudden
  /// burst of misses is otherwise unexplained.
  void icacheDrainAll(const char *reason = "unspecified");

  /// Number of drains this shared state has performed. Diagnostic / test hook.
  uint32_t icacheDrainSeq() const;

  //--- consumer path (worker / test) -----------------------------------------
  bool pollOne();
  unsigned pollBudget(unsigned maxItems);

  //--- diagnostics ------------------------------------------------------------
  void getDiagnostics(EJitSharedDiagnostics &out) const;
  uint32_t sharedInitState() const;
  /// In-flight dedup count (used by the taskpool C ABI pending_count).
  uint32_t pendingCount() const;
  /// Owner-side explicit flush for deterministic tests. Production callers use
  /// requestCodeBatchFlushAndWait() so enable_ex runs on the owner worker.
  bool flushCodeBatch() { return flushPendingPublishes(); }
  size_t pendingPublishCount() const { return pendingPublishes_.size(); }
  size_t pendingBatchCompileCount() const {
    return pendingBatchCompiles_.size();
  }

  /// Ask the owner worker to seal every completed code range and publish its
  /// Pending cache entries, then wait for completion. Safe on any attached
  /// core.
  bool requestCodeBatchFlushAndWait();

  /// Ask the owner worker to print its owner-local may_const ranking. Any
  /// attached core may call this; concurrent callers coalesce. The call waits
  /// for the owner to complete the diagnostic and returns its success status.
  bool requestMayConstRanking();

  /// The worker loop body: poll until the shared state leaves Ready. Public so
  /// an injected task entry can forward to it; normally reached via
  /// WorkerEntry.
  void runWorkerLoop();

  /// One step of the worker state machine. Public so a deterministic test can
  /// drive the REAL machine across controlled state transitions (no thread).
  /// Waits (never exits) while the owner is still Initializing.
  EJitWorkerStep workerPollOnce();

  //--- worker observability (owner-local; the worker runs on this instance) ---
  /// Ready iterations the worker loop executed (proves it reached the consume
  /// phase, not merely that start was called).
  uint64_t workerConsumeLoops() const {
    return workerConsumeLoops_.loadRelaxed();
  }
  /// True if the worker loop ever had to wait on a not-yet-Ready owner.
  bool workerWaitedForReady() const {
    return workerWaitedForReady_.loadRelaxed() != 0;
  }
  /// Number of idle-hook calls (idle yields AND post-task throttle delays):
  /// proves the worker does not busy-spin while waiting, idle, or throttling.
  uint64_t workerIdleYields() const { return workerIdleYields_.loadRelaxed(); }

private:
  static void workerEntryThunk(void *ctx);
  /// Yield/delay the CPU (injected hook, or a reordering barrier). \p ticks=1
  /// is a single yield (idle/wait); \p ticks=MULT*DELAY_TICKS is the post-task
  /// throttle delay. Bumps workerIdleYields_ either way.
  void workerIdle(uint32_t ticks);
  /// Apply the configured post-task throttle through the same hook used by the
  /// normal worker loop. Batch helpers must call this between internal compile
  /// operations because they execute more than one operation per worker step.
  void workerThrottle();
  bool serviceMayConstRankingRequest();

  /// Result of a shared-cache lookup, including the cross-core fnPtr gate.
  struct SharedLookup {
    void *fnPtr = nullptr;
    /// Matched shared slot for post-validation diagnostics. A token-bearing
    /// hit keeps it stable; NO_RECLAIM marks it only after the outer seqlock
    /// validation has accepted the lookup.
    EJitSharedCacheSlot *slot = nullptr;
    uint32_t bucketIndex = 0;
    bool hasReadToken = false;
    bool readyButNotShareable = false;
    /// The instrumented Tier-1 has collected its configured number of root
    /// samples and Tier-2 is queued/compiling. Keep the Tier-1 allocation and
    /// counters alive for profile synthesis, but route calls back to AOT until
    /// Tier-2 publication replaces the slot.
    bool pgoSamplingComplete = false;
    /// EJIT_SRE_TASKPOOL_NO_RECLAIM only: a validated seqlock hit that holds NO
    /// read token. classifyHit() treats it as a CacheHit; bucketIndex is the
    /// out-of-range sentinel (kEJitSharedCacheBuckets) so the wrapper's
    /// releaseRead() is a safe no-op (its range check rejects it).
    bool noTokenHit = false;
    /// Set by peerPrepareSlot() when the pointer came from the out-of-line cold
    /// first-touch path (which self-revalidates), so the seqlock caller must
    /// not second-guess it with the bucket publishSeq check.
    bool coldPrepared = false;
    /// The matching hit crossed the Tier-2 threshold. The caller decides
    /// whether to enqueue immediately or attach bound descriptors first.
    bool tier2Arm = false;
  };

  // shared cache helpers (POD table in the shared blob)
  uint64_t hashIdentity(uint32_t funcIndex, const EJitDimPair *dims,
                        uint32_t numDims) const;
  /// True only when the shared cache currently publishes \p fnPtr as Tier-2
  /// for this exact identity and version snapshot. Used while online PGO is
  /// enabled to keep Tier-1 out of the wrapper's direct inline cache without
  /// duplicating hit accounting or triggering another Tier-2 request.
  bool isPublishedTier2(uint32_t funcIndex, void *fnPtr,
                        const EJitDimPair *dims, uint32_t numDims);
  SharedLookup cacheLookup(uint32_t funcIndex, const EJitDimPair *dims,
                           uint32_t numDims);
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  /// Load-only seqlock cache lookup (no per-hit read-token RMW). Returned hits
  /// carry no read token (SharedLookup::noTokenHit). See the .cpp for the
  /// never-free safety precondition.
  SharedLookup cacheLookupSeq(uint32_t funcIndex, const EJitDimPair *dims,
                              uint32_t numDims);
  /// Fixed-dimension load-only seqlock specializations (0-2 dims). Same
  /// never-free seqlock discipline as cacheLookupSeq() but with the identity
  /// hashing, slot identity comparison, and version comparison unrolled exactly
  /// like cacheLookup0D/1D/2D — so a NO_RECLAIM fixed-dimension caller reaches
  /// the shared slot resolution without the generic numDims loop, the
  /// slotIdentityMatches() call, or variable-length dims[] handling. Behavior
  /// is identical to cacheLookupSeq() with the matching numDims. 3D/4D keep
  /// using the generic cacheLookupSeq() (rare on the hot path).
  SharedLookup cacheLookupSeq0D(uint32_t funcIndex);
  SharedLookup cacheLookupSeq1D(uint32_t funcIndex, uint32_t dim0,
                                uint32_t inst0);
  SharedLookup cacheLookupSeq2D(uint32_t funcIndex, uint32_t dim0,
                                uint32_t inst0, uint32_t dim1, uint32_t inst1);
#endif
  /// Fixed-dimension specializations of cacheLookup() (0-4 dims). Identity
  /// hashing, slot identity comparison, and version comparison are all unrolled
  /// (no numDims loops, no dims[] indexing), so a cache hit reaches the shared
  /// slot resolution with the least per-hit work. Behavior is identical to
  /// cacheLookup() with the matching numDims. The cross-core fnPtr gate and the
  /// cold non-owner preparation are shared via resolveMatchedSlot() /
  /// peerPrepareSlot(), so these stay small on the hot path.
  SharedLookup cacheLookup0D(uint32_t funcIndex);
  SharedLookup cacheLookup1D(uint32_t funcIndex, uint32_t dim0, uint32_t inst0);
  SharedLookup cacheLookup2D(uint32_t funcIndex, uint32_t dim0, uint32_t inst0,
                             uint32_t dim1, uint32_t inst1);
  SharedLookup cacheLookup3D(uint32_t funcIndex, uint32_t dim0, uint32_t inst0,
                             uint32_t dim1, uint32_t inst1, uint32_t dim2,
                             uint32_t inst2);
  SharedLookup cacheLookup4D(uint32_t funcIndex, uint32_t dim0, uint32_t inst0,
                             uint32_t dim1, uint32_t inst1, uint32_t dim2,
                             uint32_t inst2, uint32_t dim3, uint32_t inst3);
  /// Resolve a cache slot whose identity + versions already matched, with the
  /// bucket read lock HELD on entry. Applies the cross-core fnPtr gate and
  /// returns the hit (with the read token held) for the owner core or a core
  /// that has already memoized execute permission; a core that may not read the
  /// pointer gets a clean readyButNotShareable fallback (lock released). The
  /// rare non-owner first-touch case is delegated to peerPrepareSlot(). Shared
  /// by cacheLookup() and all fixed-dimension specializations.
  SharedLookup resolveMatchedSlot(EJitSharedCacheBucket &bucket,
                                  uint32_t bucketIndex, uint32_t slotIndex);
  /// Submit Tier-2 from an identity/version-validated slot while its bucket
  /// read lock is held. This preserves the exact slot snapshot without
  /// enlarging the 16-byte CompileOrGetResult hot-path return value.
  void
  enqueueTier2FromSlot(const EJitSharedCacheSlot &slot,
                       const EJitBoundPtrDescriptor *boundPointers = nullptr,
                       uint32_t boundCount = 0);
  void
  enqueueTier2ForIdentity(uint32_t funcIndex, const EJitDimPair *dims,
                          uint32_t numDims,
                          const EJitBoundPtrDescriptor *boundPointers = nullptr,
                          uint32_t boundCount = 0);
  /// Cold non-owner first-touch execute-permission preparation for a matched
  /// slot, with the bucket read lock HELD on entry (this function releases it).
  /// Snapshots the slot, drops the lock for the per-core platform seal, then
  /// re-validates before handing back the prepared pointer. Kept out-of-line
  /// (noinline) so it never bloats the hit path in cacheLookup()/cacheLookupNd.
  SharedLookup peerPrepareSlot(EJitSharedCacheBucket &bucket,
                               uint32_t bucketIndex, uint32_t slotIndex);
  /// Convert a shared cache lookup outcome into a CompileOrGetResult with the
  /// fast-path terminal classification (CacheHit / readyButNotShareable /
  /// miss). Shared by tryCacheHit() and the fixed-dimension entries so the
  /// cache-hit counter is incremented exactly once and the semantics stay
  /// identical. Does NOT perform the Ready or instance-enabled checks (the
  /// callers do those first).
  CompileOrGetResult classifyHit(const SharedLookup &Hit,
                                 bool enqueueTier2 = true);
  EJitPublishStatus cachePublish(const EJitCompileRequest &req, void *fnPtr,
                                 const EJitCompiledCodeInfo *info,
                                 bool pgoClearExclusive = false);
  EJitPublishStatus cacheStagePending(const EJitCompileRequest &req,
                                      void *fnPtr);
  /// Reserve the request's complete identity in the shared cache before the
  /// owner defers compilation for layout sorting. Unlike cacheStagePending(),
  /// this never evicts Ready code: a collision simply keeps the coarse
  /// per-function in-flight claim until the batch is compiled.
  EJitPublishStatus cacheStageBatchRequest(const EJitCompileRequest &req);
  void cacheDropPending(const EJitCompileRequest &req, void *fnPtr);
  bool cacheHasPending(uint32_t funcIndex, const EJitDimPair *dims,
                       uint32_t numDims);

  /// Snapshot of one Ready cache slot taken under the bucket read lock, so the
  /// expensive per-core execute-permission preparation (split + enable_ex)
  /// happens with NO bucket lock held, then is re-validated against the live
  /// slot before the pointer is returned.
  struct PeerCodeRange {
    void *fn = nullptr;
    uint32_t slotIndex = 0;
    uint32_t bucket = 0;
    uint32_t funcIndex = 0;
    uint32_t numDims = 0;
    uint32_t generation = 0;
    EJitDimPair dims[4] = {};
    uint32_t versions[4] = {};
    uintptr_t codeStart = 0;
    uint64_t codeSize = 0;
    uintptr_t poolBase = 0;
    uint64_t poolSize = 0;
    /// Runtime-writable extents (e.g. __profc_) this core must enable_rw before
    /// it may execute the code. Snapshotted with the range so the per-core
    /// enable_rw runs with NO bucket lock held. writableCount 0 => none.
    uint32_t writableCount = 0;
    /// 1 => this pool is a fixed RX region: the peer must enable_rw the
    /// writable pages. 0 => dynamic RW pool: no enable_rw (ranges are
    /// diagnostic only).
    uint32_t requiresPeerEnableRw = 0;
    EJitSharedWritableRange writables[kEJitSharedMaxWritableRanges] = {};
  };
  /// Prepare execute permission for the current core over \p R's code range
  /// WITHOUT holding any bucket lock. 2M mode delegates to prepareCodeFn_; 4K
  /// mode ensures the pool is split for this core then seals every covered 4K
  /// page. Returns true only when the calling core can legally execute the
  /// code afterwards.
  bool prepareExecForCurrentCore(const PeerCodeRange &R, uint32_t self);
  /// Ensure the calling core has split \p poolBase once (4K mode). Coordinates
  /// concurrent first-touch via the shared per-pool readiness table.
  bool ensurePoolSplitForCurrentCore(uint32_t self, uintptr_t poolBase,
                                     uint64_t poolSize);
  /// Find (or open-address claim) the readiness entry for \p poolBase. Returns
  /// nullptr when the fixed table is full (clean fallback).
  EJitSharedPoolSplit *findOrClaimPoolSlot(uintptr_t poolBase);

  // switch/version helpers
  bool isInstanceEnabled(uint32_t dimType, uint32_t instanceId) const;
  uint32_t instanceVersion(uint32_t dimType, uint32_t instanceId) const;
  bool versionsCurrent(const EJitCompileRequest &req) const;

  // queue/dedup helpers
  bool queuePush(const EJitCompileRequest &req);
  bool queuePop(EJitCompileRequest &out);
  /// Claim the in-flight slot for \p funcIndex at generation \p gen: CAS
  /// 0->gen.
  EJitDedupResult dedupMark(uint32_t funcIndex, uint32_t gen);
  /// Release the in-flight slot ONLY if it still holds \p gen: CAS gen->0. A
  /// stale worker (older gen) therefore cannot clear a newer generation's bit.
  void dedupClear(uint32_t funcIndex, uint32_t gen);

  /// Compile one dequeued request through the two version checkpoints and the
  /// commit-gated publish. The Tier-2 aarch64 exclusive-monitor workaround
  /// (pgoClearExclusive) is derived here from the request's encoded tier
  /// (decodeReqTier), so a PGOUse (Tier-2) publish clears the monitor primed by
  /// the arming hit's hitCount RMW while a Tier-1 publish keeps the normal
  /// write lock. No caller-supplied flag: the worker owns this policy (spec
  /// §4.9).
  void runCompile(const EJitCompileRequest &req,
                  bool hasBatchRequestMarker = false);
  void compilePendingBatchRequests();
  bool flushPendingPublishes(bool compileBatchRequests = true);
  bool serviceCodeBatchRequest();
  bool serviceAutoTier2Publish();

  /// Release staged-PGO ownership if \p funcIndex still owns it. Tier-2
  /// completion and terminal worker failures call this; transient queue-full
  /// leaves ownership intact so the next hit can retry.
  void finishPgoFunction(uint32_t funcIndex, bool completed,
                         const char *reason = nullptr);

  /// Owner-only: snapshot the owner-core code-pool stats via the registered
  /// provider and storeRelaxed them into the shared mirror. Called after every
  /// successful compile (sync + async publish) so the mirror stays fresh for
  /// cross-core readers. No-op when no provider is registered.
  void publishCodePoolStats();

  EJitSharedTaskPoolState *state_ = nullptr;
  CompileCallback compileFn_ = nullptr;
  void *compileCtx_ = nullptr;
  PublishCallback publishFn_ = nullptr;
  void *publishCtx_ = nullptr;
  ReleaseCallback releaseFn_ = nullptr;
  void *releaseCtx_ = nullptr;
  PrepareCodeCallback prepareCodeFn_ = nullptr;
  void *prepareCodeCtx_ = nullptr;
  CodeRangeCallback codeRangeFn_ = nullptr;
  void *codeRangeCtx_ = nullptr;
  CodePoolStatsCallback codePoolStatsFn_ = nullptr;
  void *codePoolStatsCtx_ = nullptr;
  MayConstRankingCallback mayConstRankingFn_ = nullptr;
  void *mayConstRankingCtx_ = nullptr;
  CodeReadyCallback codeReadyFn_ = nullptr;
  CodeBatchFlushCallback codeBatchFlushFn_ = nullptr;
  void *codeBatchCtx_ = nullptr;
  SplitPoolCallback splitPoolFn_ = nullptr;
  void *splitPoolCtx_ = nullptr;
  SealPageCallback sealPageFn_ = nullptr;
  void *sealPageCtx_ = nullptr;
  EnableRwPageCallback enableRwPageFn_ = nullptr;
  void *enableRwPageCtx_ = nullptr;
  bool fourKSeal_ = false;
  WorkerStartFn workerStart_ = nullptr;
  WorkerStopFn workerStop_ = nullptr;
  void *workerCtx_ = nullptr;
  WorkerIdleFn workerIdle_ = nullptr;
  void *workerIdleCtx_ = nullptr;
#ifdef EJIT_SRE_TASKPOOL_TESTING
  TestHookFn pgoAdmissionTestHook_ = nullptr;
  void *pgoAdmissionTestHookCtx_ = nullptr;
#endif
  OwnerElectedFn ownerElected_ = nullptr;
  void *ownerElectedCtx_ = nullptr;
  OwnerReleasedFn ownerReleased_ = nullptr;
  void *ownerReleasedCtx_ = nullptr;
  uint64_t regFingerprint_ = 0;
  EJitCompileMode configuredMode_ = EJitCompileMode::Async;
  bool codeSharingEnabled_ = false;
  bool isOwner_ = false;
#ifdef EJIT_SRE_TASKPOOL_TESTING
  IcacheFillMidpointHook icacheFillMidpointHook_ = nullptr;
  void *icacheFillMidpointCtx_ = nullptr;
#endif
  struct PendingPublish {
    EJitCompileRequest req{};
    void *fn = nullptr;
  };
  struct PendingBatchCompile {
    EJitCompileRequest req{};
    bool hasSharedMarker = false;
  };
  std::vector<PendingBatchCompile> pendingBatchCompiles_;
  std::vector<PendingPublish> pendingPublishes_;
  /// Armed when Tier-2 links into the near RW/NX pool. Consumed only after the
  /// owner worker observes the shared compile queue empty.
  bool autoTier2PublishPending_ = false;
  // Inline-cache safety gate: true while the cache is safe to use (no releaser
  // wired - the production default). v2 does no HP-scan retire, so a wired
  // releaser (code may be freed) + the cache = UAF; the gate then auto-disables
  // the cache. See setReleaser().
  bool icacheReclamationSafe_ = true;
  /// Whether THIS facade currently contributes 1 to icacheReleasersWired.
  /// Without it, bind() and setReleaser() each add on their own and a facade
  /// that is bound twice, or wired then re-bound, counts itself more than once
  /// and the gate never re-opens.
  bool icacheReleaserCounted_ = false;
  /// Make the shared count agree with this facade's releaser, exactly once.
  ///
  /// LIMIT, and it is why production must keep to ONE facade per blob
  /// (EJitCompileDriver::sharedPool_): a (re)initialization zeroes the count
  /// while this flag stays set, so after a re-init this facade believes it is
  /// counted when it is not. Only init() re-publishes, and only for the owner's
  /// own releaseFn_ -- a peer facade that wired a releaser before the re-init
  /// is not restored, and its later unwire would decrement a count it no longer
  /// owns. The decrement saturates at zero so the worst case is re-opening the
  /// gate early, never wrapping it shut forever; closing it properly would need
  /// the same generation stamp the drain protocol uses.
  void syncIcacheReleaserCount() {
    const bool want = (state_ != nullptr) && (releaseFn_ != nullptr);
    if (want == icacheReleaserCounted_)
      return;
    if (want) {
      state_->icacheReleasersWired.fetchAdd(1);
      icacheReleaserCounted_ = true;
      return;
    }
    if (state_) {
      uint32_t cur = state_->icacheReleasersWired.loadAcquire();
      while (cur != 0 &&
             !state_->icacheReleasersWired.compareExchange(cur, cur - 1))
        ;
    }
    icacheReleaserCounted_ = false;
  }
  /// Publish this facade's local "needs per-core execute preparation" answer
  /// into the blob, so every other facade -- including ones never handed the
  /// seal mode -- gates the 0-dim shared scalar on it. Set-only: see
  /// icachePerCorePrepare for why the monotone direction is the safe one.
  void publishIcachePrepareMode() {
    // Only a wired prepareCodeFn_ counts. 4K-seal mode is deliberately NOT
    // per-core preparation here -- see icacheCrossCoreExecutable() for why, and
    // for the assumption to re-check if a 0-dim entry ever faults.
    if (state_ && prepareCodeFn_ != nullptr)
      state_->icachePerCorePrepare.storeRelease(1);
  }

  // Pre-init staging only. Once the shared blob is Ready, state_->pgoEnabled
  // and state_->tier2Threshold are the cross-core source of truth.
  EJitAtomicU8 pgoEnabled_{0};
  EJitAtomicU32 tier2Threshold_{0};
  EJitAtomicU32 pgoMaxConcurrentProfiles_{1};
  // PGO (§6): Tier-2 requests are NOT held in a facade-local bypass. A hit that
  // crosses the threshold (on ANY producer core / facade) submits a fully
  // value-initialized EJitCompileRequest through the shared MPSC queue, so the
  // single owner worker consumes it via the normal pollOne()/runCompile() path.
  // This is what lets a peer core's Tier-2 trigger actually be serviced.

  // Worker observability+ startup-wait bound (owner-local).
  EJitAtomicU64 workerConsumeLoops_{0};
  EJitAtomicU32 workerWaitedForReady_{0};
  EJitAtomicU64 workerIdleYields_{0};
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOL_H
