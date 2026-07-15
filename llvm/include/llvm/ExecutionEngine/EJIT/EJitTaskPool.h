//===-- EJitTaskPool.h - SRE taskpool compile scheduler -------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITTASKPOOL_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITTASKPOOL_H

#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitRwLock.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include "llvm/ExecutionEngine/EJIT/EJitWorker.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

#ifndef EJIT_SRE_TASKPOOL_BUCKETS
#define EJIT_SRE_TASKPOOL_BUCKETS 32u
#endif

// Flat dedup-table capacity (§3.5): funcIndex indexes inFlight_[] DIRECTLY. A
// funcIndex >= this capacity is rejected (InvalidFuncIndex), never folded or
// reduced — so two distinct functions can never alias the same in-flight bit.
// Must match the wrapper's kEJitMaxFuncIndex (EJitCommon.h) so the AOT-assigned
// dense funcIndex and the runtime table agree.
#ifndef EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX
#define EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX 4096u
#endif

namespace llvm {
namespace ejit {

enum class EJitCompileMode : uint32_t {
  Off = 0,
  Async = 1,
  Sync = 2, ///< Compile inline on the calling thread (no worker).
};

enum class EJitCompileOrGetStatus : uint32_t {
  CacheHit = 0,
  OffMode,
  InstanceDisabled,
  EnqueuedPending,
  AlreadyPending,
  QueueFullFallback,
  CompileFailed,
  InvalidParam,
};

struct EJitCacheLookupResult {
  void *fnPtr = nullptr;
  uint32_t bucketIndex = 0;
  bool hasReadToken = false;
  /// PGO: set when this hit brought hitCount to exactly the Tier-2 threshold
  /// (one-shot trigger signal, §6). False on miss / below threshold.
  bool tier2Arm = false;
};

//===----------------------------------------------------------------------===//
// EJitSwitchController (§5.1)
//
// Per-instance enable/version state in fixed 2-D arrays, indexed DIRECTLY by
// (dimType, instanceId). dimType is an explicit lifecycle slot in [0,
// MAX_DIM_TYPES) assigned once per lifecycle NAME by the process-global
// EJitLifecycleRegistry at registration time (the wrapper loads it back through
// a per-lifecycle global, so distinct lifecycles never share a slot and the
// same lifecycle is identical across modules); instanceId is in [0,
// MAX_INSTANCES). There is no runtime registry, no name hashing, and no probing
// in THIS array: out-of-range indices are rejected (disabled / version 0 /
// setEnabled false). enabled_ initializes to 1, version_ to 0; a successful
// enable/disable flip bumps version by exactly one (§5.1).
//===----------------------------------------------------------------------===//
class EJitSwitchController {
public:
  static constexpr uint32_t MAX_DIM_TYPES = 8;
  static constexpr uint32_t MAX_INSTANCES = 256;

  EJitSwitchController();

  bool isInstanceEnabled(uint32_t dimType, uint32_t instanceId) const;
  uint32_t getInstanceVersion(uint32_t dimType, uint32_t instanceId) const;
  bool setEnabled(uint32_t dimType, uint32_t instanceId, bool wantOn);

  EJitCompileMode getMode() const {
    return static_cast<EJitCompileMode>(mode_.loadAcquire());
  }
  void setMode(EJitCompileMode mode) {
    mode_.storeRelease(static_cast<uint32_t>(mode));
  }

private:
  EJitAtomicU8 enabled_[MAX_DIM_TYPES][MAX_INSTANCES];
  EJitAtomicU32 version_[MAX_DIM_TYPES][MAX_INSTANCES];
  EJitAtomicU32 mode_{static_cast<uint32_t>(EJitCompileMode::Async)};
};

//===----------------------------------------------------------------------===//
// EJitDedupTable (§3.5): flat O(1) in-flight bit, keyed by funcIndex ONLY.
//
// funcIndex indexes inFlight_[] DIRECTLY. A single funcIndex may have at most
// one in-flight compile, regardless of its dims/versions. Distinct funcIndexes
// are fully independent (separate array slots). A funcIndex outside
// [0, kCapacity) is rejected as InvalidFuncIndex — never folded, masked, or
// hashed into the table, so two distinct functions can never share a bit. The
// compile payload (dims/versions/fallback) lives entirely in the queue.
//===----------------------------------------------------------------------===//
enum class EJitDedupResult : uint32_t {
  Claimed = 0,      ///< Caller now owns the in-flight bit; must clear() later.
  AlreadyPending,   ///< Same funcIndex already in flight.
  InvalidFuncIndex, ///< funcIndex >= kCapacity: rejected, not folded.
};

class EJitDedupTable {
public:
  static constexpr uint32_t kCapacity = EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX;

  /// CAS 0->1 for funcIndex's slot. InvalidFuncIndex when out of range.
  EJitDedupResult tryMarkPending(uint32_t funcIndex);
  /// storeRelease(0) for funcIndex's slot (no-op when out of range).
  void clear(uint32_t funcIndex);
  /// Best-effort count of in-flight slots.
  uint32_t pendingCount() const;

private:
  EJitAtomicU32 inFlight_[kCapacity]{};
};

class EJitTaskQueue {
public:
  enum class EnqueueResult : uint32_t {
    Enqueued = 0,
    AlreadyPending,
    QueueFull,
    InvalidFuncIndex,
  };

  explicit EJitTaskQueue(uint32_t queueCapacity) : queue_(queueCapacity) {}

  EnqueueResult tryEnqueue(const EJitCompileRequest &req);
  bool tryDequeue(EJitCompileRequest &out) { return queue_.pop(out); }
  void release(uint32_t funcIndex) { dedup_.clear(funcIndex); }
  uint32_t inFlightCount() const { return dedup_.pendingCount(); }
  uint32_t queueDepth() const { return queue_.approximateSize(); }

private:
  EJitQueue queue_;
  EJitDedupTable dedup_;
};

struct EJitCacheEntry {
  uint32_t funcIndex = 0;
  uint32_t numDims = 0;
  EJitDimPair dims[4]{};
  uint32_t versions[4]{};
  uintptr_t fnPtr = 0;
  /// PGO hotspot counter (incremented on cache hit; Tier-2 trigger, §6).
  /// Zeroed when Tier-2 publishes over a Tier-1 entry (§7.1). Atomic: the
  /// non-shared cache is read under the bucket read lock by concurrent
  /// business threads, so the hit-path increment is an atomic RMW.
  EJitAtomicU64 hitCount{};
  /// Tier-1 captured counter/data global addresses (§5.2). 0 for Baseline and
  /// after Tier-2 publish (Tier-2 carries no counters). Plain: written under
  /// the bucket write lock (publish) and not read concurrently.
  uintptr_t profcAddr = 0;
  uintptr_t profdAddr = 0;
  /// PGO: compile tier of the published code. 0=Baseline/empty, 1=Tier-1,
  /// 2=Tier-2 (PGOUse).  Suppresses repeat Tier-2 triggers on already-Tier-2
  /// slots (§4).  Written under the bucket write lock (publish).
  uint8_t tier = 0;
};

//===----------------------------------------------------------------------===//
// EJitTaskPoolCache (§4.1): 32 buckets, each an unordered_map keyed by the
// golden-ratio hash of (funcIndex, dims). A 64-bit hash CAN collide, so each
// key maps to a *vector* of entries carrying the full identity (funcIndex +
// dims); lookup/publish match the full identity, so distinct requests that
// hash-collide are stored side by side and never falsely hit each other. Old
// code is released only through a real release callback — the cache never
// fabricates a physical free.
//===----------------------------------------------------------------------===//

/// Outcome of EJitTaskPoolCache::publish() — the commit gate (§5.3/§5.4).
enum class EJitPublishStatus : uint32_t {
  Published = 0,   ///< Entry written/overwritten with the request's versions.
  VersionMismatch, ///< A version changed before/at publish: nothing written.
  InvalidParam,    ///< Null fn or numDims > 4.
  Failed,          ///< No slot available (should not happen with chaining).
};

class EJitTaskPoolCache {
public:
  static constexpr uint32_t kBuckets = EJIT_SRE_TASKPOOL_BUCKETS;

  /// Called under the bucket write lock (readers already drained to 0) to
  /// release physical code memory for an overwritten/retired function pointer.
  /// Optional: when unset the cache performs a purely logical drop (the cache
  /// never fabricates a physical free).
  using ReleaseCallback = void (*)(void *ctx, void *oldFn);

  explicit EJitTaskPoolCache(EJitSwitchController &Switch) : switch_(Switch) {}

  void setReleaser(ReleaseCallback fn, void *ctx) {
    releaseFn_ = fn;
    releaseCtx_ = ctx;
  }
  /// PGO: set the Tier-2 trigger threshold (hitCount at which a hit arms a
  /// Tier-2 recompile). 0 = disabled (no arming). §6.
  void setTier2Threshold(uint32_t threshold) { tier2Threshold_ = threshold; }

  EJitCacheLookupResult lookup(uint32_t funcIndex, const EJitDimPair *dims,
                               uint32_t numDims);

  /// Commit gate: takes the worker's per-instance version snapshot. Acquires
  /// the bucket write lock (draining readers to 0), then re-verifies every
  /// dim's snapshot still equals the current SwitchController version. On any
  /// mismatch it writes nothing (returns VersionMismatch) so a result compiled
  /// for a stale instance state is never stamped current. On success the entry
  /// stores exactly the request's versions.
  EJitPublishStatus publish(uint32_t funcIndex, const EJitDimPair *dims,
                            uint32_t numDims, const uint32_t *versions,
                            void *fnPtr);

  /// Retire a never-published function pointer (e.g. a rejected stale compile)
  /// through the release callback, if one is installed. No-op otherwise.
  void retireCode(void *fnPtr);

  /// PGO test/diagnostic: the hitCount of the entry matching (funcIndex, dims),
  /// or 0 if no match. Strips tier bits from funcIndex (publish stores stripped).
  uint64_t hitCountOf(uint32_t funcIndex, const EJitDimPair *dims,
                      uint32_t numDims);

  void releaseRead(uint32_t bucketIndex);
  uint32_t readyCount() const;
  /// Acquire every bucket's write lock (draining readers to 0), release each
  /// live entry's code through the release callback exactly once (logical drop
  /// when no callback), then clear. Must be called only after the worker stops.
  void shutdown();

#ifdef EJIT_SRE_TASKPOOL_TESTING
  /// Test-only hook fired inside publish() after the write lock is held and
  /// BEFORE the under-lock version recheck, to deterministically inject a
  /// toggle into the commit window.
  using PrePublishHook = void (*)(void *ctx);
  void setPrePublishHookForTest(PrePublishHook fn, void *ctx) {
    prePublishHook_ = fn;
    prePublishCtx_ = ctx;
  }
#endif

private:
  friend class EJitTaskPool;

  struct Bucket {
    EJitRwLock lock;
    std::unordered_map<uint64_t, std::vector<EJitCacheEntry>> entries;
  };

  uint64_t hashKey(uint32_t funcIndex, const EJitDimPair *dims,
                   uint32_t numDims) const;
  static bool identityMatches(const EJitCacheEntry &e, uint32_t funcIndex,
                              const EJitDimPair *dims, uint32_t numDims);

  EJitSwitchController &switch_;
  Bucket buckets_[kBuckets];
  ReleaseCallback releaseFn_ = nullptr;
  void *releaseCtx_ = nullptr;
  uint32_t tier2Threshold_ = 0; // PGO: 0 = Tier-2 trigger disabled (§6)
#ifdef EJIT_SRE_TASKPOOL_TESTING
  PrePublishHook prePublishHook_ = nullptr;
  void *prePublishCtx_ = nullptr;
#endif
};

/// Monotonic taskpool statistics. The increments are gated by EJIT_STATS_ENABLE
/// (see EJitStats.h): with stats off the EJIT_STAT_INC call sites compile to
/// nothing (zero per-call atomic cost) and these fields stay zero; the fields
/// remain so getStats() still works (reporting zeros). Default OFF for
/// production.
struct EJitTaskPoolCounters {
  EJitAtomicU64 cacheHits;
  EJitAtomicU64 asyncCompiles;
  EJitAtomicU64 asyncEnqueues;
  EJitAtomicU64 alreadyPending;
  EJitAtomicU64 queueFull;
  EJitAtomicU64 compileFailed;
  EJitAtomicU64 publishFailed;
  EJitAtomicU64 instanceDisabled;
  /// PGO (EJIT_ONLINE_PGO.md §10): Tier-1/Tier-2 compile counts + profile
  /// synthesis failures. Zero when PGO is off.
  EJitAtomicU64 tier1Compiles;
  EJitAtomicU64 tier2Compiles;
  EJitAtomicU64 profileMergeFails;
};

struct EJitTaskPoolStatsSnapshot {
  uint64_t cacheHits;
  uint64_t asyncCompiles;
  uint64_t asyncEnqueues;
  uint64_t alreadyPending;
  uint64_t queueFull;
  uint64_t compileFailed;
  uint64_t publishFailed;
  uint64_t instanceDisabled;
  uint32_t readyEntries;
  uint32_t pendingEntries;
  uint32_t queueApproxSize;
};

class EJitTaskPool {
public:
  using CompileCallback = bool (*)(void *ctx, const EJitCompileRequest &req,
                                   void **outFn);

  struct CompileOrGetResult {
    EJitCompileOrGetStatus status = EJitCompileOrGetStatus::CompileFailed;
    void *fnPtr = nullptr;
    uint32_t bucketIndex = 0;
    bool hasReadToken = false;
    /// Set by tryCacheHit() when the request was fully resolved on the fast
    /// cache-hit path (a cache hit or a disabled instance). When true the
    /// caller returns this result directly and MUST NOT enter the
    /// compileOrGet() slow path; when false the request is a true miss that
    /// still needs compileOrGet(). Internal control signal only — it does not
    /// affect status mapping or the C ABI.
    bool fastPathTerminal = false;
    /// PGO: set by tryCacheHit on a hit that crossed the Tier-2 threshold
    /// (mirrors EJitCacheLookupResult::tier2Arm). compileOrGet enqueues a
    /// Tier-2 recompile when this is set + PGO on + Async (§6).
    bool tier2Arm = false;
  };

  explicit EJitTaskPool(
      uint32_t queueCapacity = EJIT_SRE_TASKPOOL_QUEUE_CAPACITY,
      bool autoStartWorker = true);
  ~EJitTaskPool();

  void setCompiler(CompileCallback fn, void *ctx) {
    compileFn_ = fn;
    compileCtx_ = ctx;
  }

  /// Install the optional physical-code release callback used when publish
  /// overwrites an existing cache entry (see EJitTaskPoolCache::setReleaser).
  void setReleaser(EJitTaskPoolCache::ReleaseCallback fn, void *ctx) {
    cache_.setReleaser(fn, ctx);
  }
  /// PGO opt-in + Tier-2 trigger threshold (§6). When enabled, a cache hit
  /// that brings hitCount to exactly the threshold arms a lazy Tier-2
  /// recompile (Async only). disable => threshold 0 (no arming).
  void setPgoEnabled(bool enable, uint32_t threshold) {
    pgoEnabled_ = enable;
    cache_.setTier2Threshold(enable ? threshold : 0);
  }

  EJitSwitchController &switchController() { return switch_; }
  EJitTaskPoolCache &cache() { return cache_; }

  CompileOrGetResult compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                  uint32_t numDims, void *fallback);

  /// Flattened fast cache-hit path (spec §5.2 steps 0-1). Performs ONLY the
  /// terminal front half of compileOrGet(): the instance-enabled check and the
  /// cache lookup, then classifies the outcome:
  ///   * CacheHit         — returns fnPtr + bucketIndex + a held read token
  ///                        (caller releases via releaseRead), cacheHits++.
  ///   * InstanceDisabled — a disabled dim, instanceDisabled++.
  /// Both set fastPathTerminal = true; the caller returns the result directly
  /// and never enters the slow path. A true miss (enabled, no cached code)
  /// returns fastPathTerminal = false and the caller must fall through to
  /// compileOrGet(). compileOrGet() itself calls this so the
  /// ordering/counters/semantics stay identical.
  CompileOrGetResult tryCacheHit(uint32_t funcIndex, const EJitDimPair *dims,
                                 uint32_t numDims);

  /// Fixed-dimension fast cache-hit entries (0-4 dims). Same terminal
  /// semantics as tryCacheHit() but the instance-enabled check is unrolled and
  /// the dim identity is built directly on the stack (no numDims loop), so the
  /// C ABI fixed-dimension entries (ejit_taskpool_compile_or_get_Nd) reach the
  /// shared cache lookup with the least overhead. 4 is the maximum dimension
  /// count; higher-dimension callers keep using tryCacheHit().
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

  bool pollOne();
  unsigned pollBudget(unsigned maxItems);

  bool startWorker();
  void stopWorker();
  bool isWorkerRunning() const;
  uint64_t workerProcessedCount() const { return worker_.processedCount(); }
  uint64_t workerSpinCount() const { return worker_.spinCount(); }

  void releaseRead(uint32_t bucketIndex) { cache_.releaseRead(bucketIndex); }
  uint32_t pendingCount() const { return queue_.inFlightCount(); }

  void getStats(EJitTaskPoolStatsSnapshot &out) const;

private:
  bool versionsMatch(const EJitCompileRequest &req) const;
  void runCompile(const EJitCompileRequest &req);
  /// Convert a cache lookup outcome into a CompileOrGetResult with the
  /// fast-path terminal classification (CacheHit / miss). Shared by
  /// tryCacheHit() and the fixed-dimension entries so the cache-hit counter is
  /// incremented exactly once and the semantics stay identical. Does NOT
  /// perform the instance-enabled check (the callers do that first).
  CompileOrGetResult classifyHit(const EJitCacheLookupResult &Hit);

  EJitSwitchController switch_;
  EJitTaskQueue queue_;
  EJitTaskPoolCache cache_;
  bool pgoEnabled_ = false; // PGO (§6): gates the Tier-2 trigger
  CompileCallback compileFn_ = nullptr;
  void *compileCtx_ = nullptr;
  // Value-initialized: EJitAtomic now has a trivial default ctor (so the shared
  // state blob needs no dynamic init / .init_array), so this heap-resident
  // counters block zeroes itself explicitly rather than via the atomic ctor.
  EJitTaskPoolCounters counters_{};
  EJitWorker worker_;
  bool autoStartWorker_ = true;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITTASKPOOL_H
