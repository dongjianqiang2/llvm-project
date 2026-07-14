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
//     worker.
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
#include <atomic>
#include <cstdint>

namespace llvm {
namespace ejit {

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
  uint64_t instanceDisabledPreActivate; ///< instanceDisabled before first activate.
  uint64_t executePrepareFailed;
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
// Per-function inline cache (v2: sticky monomorphic).
//
// A single global slot per funcIndex holding a FROZEN specialization pointer:
// written once (first resolution, one-shot CAS) and read forever. No version /
// dims / generation re-validation, no refill, and no release_read on the hit
// path - the probe is a single acquire load + null check.
//
// Correctness rests on a hard precondition: every ejit_entry's specialization
// is invariant for process lifetime (period toggles do not change the baked
// code; each entry is monomorphic - always called with one dim identity). If
// that ever fails the cache silently runs a stale specialization; the safety
// gate below does NOT cover that, only UAF.
//
// Lifetime safety: JIT code is never physically freed in production (NO_RECLAIM
// + no releaseFn_ wired), so a cached pointer can never dangle. v2 does NO
// hazard-pointer retire and never will: if a releaser IS wired (code may be
// freed) the safety gate (icacheReclamationSafe_) auto-disables the cache
// (icacheTry always misses, icacheFill no-ops) to avoid UAF.
//
// The code-sharing gate is retained: a non-owner core may only read a cached
// pointer when EJIT_SRE_SHARED_CODE_POINTERS is platform-validated; otherwise it
// misses and falls back to ejit_taskpool_compile_or_get. Under sharing=OFF only
// the owner core uses the cache, so one global slot suffices.
//===----------------------------------------------------------------------===//
#ifndef EJIT_ICACHE_FUNC_SLOTS
#define EJIT_ICACHE_FUNC_SLOTS 64u
#endif

// Per-dim bound D of the multi-version inline cache (@__ejit_icache_fn_<name>
// is a [D]^numDims array). MUST be a power of 2 (the hit path indexes with
// shifts, no multiply). The CMake EJIT_ICACHE_DIM_SIZE var overrides this
// default; the AOT pass reads the same value via -mllvm -ejit-icache-dim-size
// so array layout and runtime linearization agree.
#ifndef EJIT_ICACHE_DIM_SIZE
#define EJIT_ICACHE_DIM_SIZE 16u
#endif
// Maximum number of ejit_dim params a cached ejit_entry may have. An entry
// with more is a compile error (the wrapper is not emitted). 4 matches the
// taskpool DimCount cap.
#ifndef EJIT_ICACHE_MAX_DIMS
#define EJIT_ICACHE_MAX_DIMS 4u
#endif

// Test/diagnostic: clear every icache slot. The slot-pointer table is
// process-static storage shared across pool instances, so tests clear it
// between cases to avoid stale cross-test leakage.
void ejitIcacheClearAll();

// Register a per-function icache slot: \p base is the address of the wrapper's
// @__ejit_icache_fn_<name> global (an EJitAtomicUPtr, or a [D]^numDims array of
// them for a multi-version entry), and \p numDims is its dimensionality. The
// runtime writes the frozen specialization pointer through the cell at
// [i0][i1]... (linearized from dims) on a successful resolve (icacheFill); the
// wrapper reads the cell directly. Called from ejit_register_icache_slot
// (name->funcIndex resolution) at ejit_auto_register / .ejit_period time.
// No-op for an out-of-range funcIndex or null base.
void ejitIcacheRegisterSlot(uint32_t funcIndex, void *base, uint32_t numDims);

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
    k = (k ^ (dims[i].dimType * 2654435761u) ^ dims[i].instanceId) * 2654435761u;
  return (k >> 26) & (kEJitL0Slots - 1);
}

class EJitSharedTaskPool {
public:
  /// Owner-private compile callback (reaches the owner's EJit/ORC). Returns
  /// true and *outFn on success.
  using CompileCallback = bool (*)(void *ctx, const EJitCompileRequest &req,
                                   void **outFn);
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
  /// pool exists (clean fallback). Optional: when unset, the shared mirror stays
  /// zero and readers fall back to their per-core (empty) view.
  using CodePoolStatsCallback = bool (*)(void *ctx, EJitCodePoolStatsOut *out);
  /// Per-core platform primitive: split a 2MiB-aligned [poolBase, poolBase +
  /// poolSize) window into 4KiB mappings in the CALLING core's translation
  /// context (split_2m_to_4k). Returns true on success. Used only in 4K
  /// page-seal mode.
  using SplitPoolCallback = bool (*)(void *ctx, uintptr_t poolBase,
                                     uint64_t poolSize);
  /// Per-core platform primitive: seal one 4KiB page at \p pageVA to RX in the
  /// CALLING core's translation context (enable_ex). Returns true on success.
  using SealPageCallback = bool (*)(void *ctx, uintptr_t pageVA);

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
  /// Idle/yield hook the worker calls whenever it has no work to do (waiting on
  /// the owner to publish Ready, or Ready with an empty queue). The production
  /// build injects a platform yield (EJitSreTask::yield: SRE_TaskDelay on
  /// freestanding, std::this_thread::yield on host) so a high-priority worker
  /// never busy-spins and starves the core trying to publish Ready. MUST NOT be
  /// called while holding a bucket lock / queue slot / dedup critical state.
  using WorkerIdleFn = void (*)(void *ctx);

  enum class InitResult : uint32_t {
    BecameOwner =
        0,         ///< Won election; built state; worker started (if injected).
    AttachedReady, ///< Bound to an already-Ready shared state.
    OwnerFailed,   ///< State is Failed/Stopping: clean fallback, no wait.
    InitInProgress, ///< Another core still Initializing; bounded retry hit.
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
    /// PGO (§6): set when this hit crosses the Tier-2 threshold — compileOrGet()
    /// enqueues a one-shot Tier-2 (PGOUse) recompile. (Shared equivalent of
    /// the non-shared CompileOrGetResult::tier2Arm.)
    bool tier2Arm = false;
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
  void bind(EJitSharedTaskPoolState *state) { state_ = state; }
  EJitSharedTaskPoolState *state() const { return state_; }

  /// Callback type for forEachCompiled: receives the funcIndex, its dim
  /// identity (dimType:instanceId pairs, numDims entries), the compiled
  /// function pointer, and the caller-provided context.
  using CompiledFuncCallback = void (*)(uint32_t funcIndex,
                                        const EJitDimPair *dims,
                                        uint32_t numDims, void *fnPtr,
                                        void *ctx);

  /// Invoke \p cb once for every successfully compiled (Ready) cache entry.
  /// For diagnostics (e.g. ejit_taskpool_print_compiled). Best-effort: a slot
  /// mid-publish is skipped, and this is not a snapshot — concurrent publishes
  /// may add entries during iteration.
  void forEachCompiled(CompiledFuncCallback cb, void *ctx) const;

  //--- owner-only configuration (applied if this core wins election) ----------
  void setCompiler(CompileCallback fn, void *ctx) {
    compileFn_ = fn;
    compileCtx_ = ctx;
  }
  void setReleaser(ReleaseCallback fn, void *ctx) {
    releaseFn_ = fn;
    releaseCtx_ = ctx;
    // v2 inline cache never reclaims (no HP-scan retire, ever). A wired
    // releaser means code may be freed while a cached fnPtr still pins it ->
    // UAF. Auto-disable the cache while a releaser is wired. Production wires
    // no releaser, so the gate stays open and the cache is unconditionally safe.
    icacheReclamationSafe_ = (fn == nullptr);
    if (fn) {
      // The gate is evaluated at fill, so blocking new fills is not enough:
      // entries armed earlier would keep serving code the releaser may free.
      gEJitL0State = nullptr;
      retireDispatchCache();
    }
  }
  void setPrepareCodeCallback(PrepareCodeCallback fn, void *ctx) {
    prepareCodeFn_ = fn;
    prepareCodeCtx_ = ctx;
  }
  /// Owner: provide the finalized code-range resolver (see CodeRangeCallback).
  void setCodeRangeProvider(CodeRangeCallback fn, void *ctx) {
    codeRangeFn_ = fn;
    codeRangeCtx_ = ctx;
  }
  /// Owner: provide the code-pool stats snapshotter (see CodePoolStatsCallback).
  void setCodePoolStatsProvider(CodePoolStatsCallback fn, void *ctx) {
    codePoolStatsFn_ = fn;
    codePoolStatsCtx_ = ctx;
  }
  /// Read the shared code-pool stats mirror (last owner-published snapshot).
  /// Returns false if the shared state is not bound. Every core sees the same
  /// values. Use this for ejit_get/print_code_pool_stats in shared builds.
  bool readCodePoolStats(EJitCodePoolStatsOut *out) const;
  /// Select the execute-permission seal granularity for non-owner preparation:
  /// true = 4KiB page seal (split the pool once per core, then enable_ex every
  /// page the code covers), false = legacy whole-2MiB-pool seal. Must match the
  /// owner's code pool. Default false.
  void setSealMode(bool fourKSeal) { fourKSeal_ = fourKSeal; }
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
  void setWorkerHooks(WorkerStartFn start, WorkerStopFn stop, void *ctx) {
    workerStart_ = start;
    workerStop_ = stop;
    workerCtx_ = ctx;
  }
  /// Inject the worker idle/yield hook (see WorkerIdleFn). When unset the loop
  /// falls back to a compiler reordering barrier only (used by step tests).
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
  void setPgoEnabled(bool enable, uint32_t threshold) {
    pgoEnabled_ = enable;
    tier2Threshold_.storeRelaxed(enable ? threshold : 0u);
  }

  /// True when the shared PGO auto-trigger is armed.
  bool isPgoEnabled() const { return pgoEnabled_; }

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
    if (state_ &&
        state_->initState.loadAcquire() ==
            static_cast<uint32_t>(EJitSharedInitState::Ready))
      state_->mode.storeRelease(static_cast<uint32_t>(mode));
  }
  /// The current cross-core compile mode: the shared state's mode (acquire
  /// load) once the blob is Ready, otherwise the staged configuredMode_.
  EJitCompileMode getSharedMode() const {
    if (state_ &&
        state_->initState.loadAcquire() ==
            static_cast<uint32_t>(EJitSharedInitState::Ready))
      return static_cast<EJitCompileMode>(state_->mode.loadAcquire());
    return configuredMode_;
  }

  /// Run owner election + bind. Idempotent: re-observes the same outcome.
  InitResult init();
  bool isOwner() const { return isOwner_; }

  /// Owner-only orderly shutdown: stop+join the worker, then return the state
  /// to Uninitialized so a later init() can re-elect. No-op for a non-owner.
  void ownerShutdown();

  //--- producer path ----------------------------------------------------------
  CompileOrGetResult compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                  uint32_t numDims, void *fallback);
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
  CompileOrGetResult tryCacheHit(uint32_t funcIndex, const EJitDimPair *dims,
                                 uint32_t numDims);
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
  bool setInstanceEnabled(uint32_t dimType, uint32_t instanceId, bool enabled);
  /// Query the shared activation bit for a lifecycle instance — the read
  /// counterpart of setInstanceEnabled, and the single cross-core source of
  /// truth the compile gate (compileCold) and ejit_is_active consult. Returns
  /// false for an out-of-range dimType/instanceId (never reads out of bounds).
  bool isInstanceActive(uint32_t dimType, uint32_t instanceId) const;

  //--- per-function inline cache (multi-version direct-indexed) --------------
  // NOTE: the production hit path does NOT use icacheTry. With -ejit-inline-cache
  // the ejit_entry wrapper reads its per-function @__ejit_icache_fn_<name> slot
  // directly - a GEP into the [D]^numDims array by the ejit_dim arg values, one
  // acquire load + null-check + indirect call, NO ejit_icache_try call, NO
  // per-call guards. icacheTry is retained for unit tests / diagnostics: on a
  // hit it sets *outFn to the frozen specialization for the given dims (call
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
    const bool match = fn != nullptr && e.epoch == state_->dispatchEpoch.loadRelaxed() &&
                       e.core == EJitCoreId::current() &&
                       e.funcIndex == funcIndex && e.numDims == numDims &&
                       dimsEqual(e.dims, dims, numDims);

    std::atomic_signal_fence(std::memory_order_acquire);
    if (e.seq != s0 || !match)
      return false;

    EJIT_STAT_INC(state_->counters.cacheHits);
    *outFn = fn;
    return true;
  }

  void l0Fill(uint32_t funcIndex, void *fnPtr, const EJitDimPair *dims,
              uint32_t numDims);

  /// Retire every core's L0. Must be called from every path that can
  /// invalidate an (identity -> fnPtr) mapping from OUTSIDE the taskpool --
  /// ejit_clear_cache(), ejit_invalidate(), a compile-mode change -- since
  /// those bypass cachePublish() and setInstanceEnabled().
  void retireDispatchCache();

  void icacheFill(uint32_t funcIndex, void *fnPtr, const EJitDimPair *dims,
                  uint32_t numDims);

  //--- consumer path (worker / test) -----------------------------------------
  bool pollOne();
  unsigned pollBudget(unsigned maxItems);

  //--- diagnostics ------------------------------------------------------------
  void getDiagnostics(EJitSharedDiagnostics &out) const;
  uint32_t sharedInitState() const;
  /// In-flight dedup count (used by the taskpool C ABI pending_count).
  uint32_t pendingCount() const;

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
  /// Number of times the worker yielded (idle hook calls): proves it does not
  /// busy-spin while waiting or idle.
  uint64_t workerIdleYields() const { return workerIdleYields_.loadRelaxed(); }

private:
  static void workerEntryThunk(void *ctx);
  /// Yield the CPU between work items (injected hook, or a reordering barrier).
  void workerIdle();

  /// Result of a shared-cache lookup, including the cross-core fnPtr gate.
  struct SharedLookup {
    void *fnPtr = nullptr;
    uint32_t bucketIndex = 0;
    bool hasReadToken = false;
    bool readyButNotShareable = false;
    /// EJIT_SRE_TASKPOOL_NO_RECLAIM only: a validated seqlock hit that holds NO
    /// read token. classifyHit() treats it as a CacheHit; bucketIndex is the
    /// out-of-range sentinel (kEJitSharedCacheBuckets) so the wrapper's
    /// releaseRead() is a safe no-op (its range check rejects it).
    bool noTokenHit = false;
    /// Set by peerPrepareSlot() when the pointer came from the out-of-line cold
    /// first-touch path (which self-revalidates), so the seqlock caller must not
    /// second-guess it with the bucket publishSeq check.
    bool coldPrepared = false;
    /// PGO (§6): set when this hit crosses the Tier-2 threshold, arming
    /// a one-shot lazy enqueue of a PGOUse recompile. compileOrGet()
    /// consumes it on the fast-hit path. (Shared equivalent of
    /// EJitCacheLookupResult::tier2Arm.)
    bool tier2Arm = false;
  };

  // shared cache helpers (POD table in the shared blob)
  uint64_t hashIdentity(uint32_t funcIndex, const EJitDimPair *dims,
                        uint32_t numDims) const;
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
  CompileOrGetResult classifyHit(const SharedLookup &Hit);
  EJitPublishStatus cachePublish(const EJitCompileRequest &req, void *fnPtr,
                                 const EJitCompiledCodeInfo *info,
                                 bool pgoClearExclusive = false);

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

  void runCompile(const EJitCompileRequest &req, bool pgoClearExclusive = false);

  /// Owner-only: snapshot the owner-core code-pool stats via the registered
  /// provider and storeRelaxed them into the shared mirror. Called after every
  /// successful compile (sync + async publish) so the mirror stays fresh for
  /// cross-core readers. No-op when no provider is registered.
  void publishCodePoolStats();

  EJitSharedTaskPoolState *state_ = nullptr;
  CompileCallback compileFn_ = nullptr;
  void *compileCtx_ = nullptr;
  ReleaseCallback releaseFn_ = nullptr;
  void *releaseCtx_ = nullptr;
  PrepareCodeCallback prepareCodeFn_ = nullptr;
  void *prepareCodeCtx_ = nullptr;
  CodeRangeCallback codeRangeFn_ = nullptr;
  void *codeRangeCtx_ = nullptr;
  CodePoolStatsCallback codePoolStatsFn_ = nullptr;
  void *codePoolStatsCtx_ = nullptr;
  SplitPoolCallback splitPoolFn_ = nullptr;
  void *splitPoolCtx_ = nullptr;
  SealPageCallback sealPageFn_ = nullptr;
  void *sealPageCtx_ = nullptr;
  bool fourKSeal_ = false;
  WorkerStartFn workerStart_ = nullptr;
  WorkerStopFn workerStop_ = nullptr;
  void *workerCtx_ = nullptr;
  WorkerIdleFn workerIdle_ = nullptr;
  void *workerIdleCtx_ = nullptr;
  uint64_t regFingerprint_ = 0;
  EJitCompileMode configuredMode_ = EJitCompileMode::Async;
  bool codeSharingEnabled_ = false;
  bool isOwner_ = false;
  // Inline-cache safety gate: true while the cache is safe to use (no releaser
  // wired - the production default). v2 does no HP-scan retire, so a wired
  // releaser (code may be freed) + the cache = UAF; the gate then auto-disables
  // the cache. See setReleaser().
  bool icacheReclamationSafe_ = true;

  bool pgoEnabled_ = false; // PGO (§6): gates the Tier-2 trigger
  EJitAtomicU32 tier2Threshold_{0}; // PGO: threshold for Tier-2 arming
  bool tier2Pending_{false}; // PGO: set by compileOrGet, cleared by pollOne
  EJitCompileRequest tier2PendingReq_{}; // PGO: pending Tier-2 request data

  // Worker observability + startup-wait bound (owner-local).
  EJitAtomicU64 workerConsumeLoops_{0};
  EJitAtomicU32 workerWaitedForReady_{0};
  EJitAtomicU64 workerIdleYields_{0};
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITSHAREDTASKPOOL_H
