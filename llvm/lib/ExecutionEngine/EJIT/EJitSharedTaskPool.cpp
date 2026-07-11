//===-- EJitSharedTaskPool.cpp - Cross-core shared single-worker facade ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Implements owner election and the producer/consumer paths over the POD
//  EJitSharedTaskPoolState. Uses ONLY EJitAtomic (acquire/release) — no
//  std::thread / std::mutex / std::condition_variable, no STL containers in the
//  shared data path. The switch/dedup/queue/commit-gate logic mirrors the
//  single-instance EJitTaskPool, re-expressed against shared POD storage; the
//  result cache is a fixed-capacity POD table (no std::unordered_map can live
//  in shared memory).
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitStats.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include <cstdlib>

using namespace llvm;
using namespace llvm::ejit;

#ifndef EJIT_SRE_TASKPOOL_WORKER_THROTTLE_ITEMS
#define EJIT_SRE_TASKPOOL_WORKER_THROTTLE_ITEMS 1u
#endif

#ifndef EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS
#define EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS 100u
#endif

namespace {

// Compiler reordering barrier used as a portable idle relax (no platform
// symbol, no arch-specific instruction in this layer).
inline void cpuRelax() { __asm__ __volatile__("" ::: "memory"); }

//===----------------------------------------------------------------------===//
// Inline per-bucket reader/writer lock over the two POD words (same protocol as
// EJitRwLock §3.2, but operating on shared-blob fields directly).
//
// EJIT_SRE_TASKPOOL_NO_RECLAIM changes the READER discipline to a load-only
// seqlock (no per-hit RMW on the shared readers line). This is memory-safe ONLY
// because in that build a published fnPtr is never physically freed (the code
// pool never reclaims; the taskpool releaser is never installed), so a hit that
// hands back a pointer without a read token can never dangle. The writer still
// takes writeFlag for writer/writer exclusion and additionally bumps a monotonic
// per-bucket publishSeq (odd while writing, even when done) that the reader uses
// to detect and discard a read that raced a publish. The default (token) build
// is unchanged: publishSeq is never touched.
//===----------------------------------------------------------------------===//
bool bucketTryRead(EJitSharedCacheBucket &b) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Load-only: never touch the shared readers line. The seqlock validity check
  // (bucketSeqBegin/bucketSeqStable) provides consistency; here we only refuse
  // to start reading while a writer is mid-publish.
  return b.writeFlag.loadAcquire() == 0;
#else
  if (b.writeFlag.loadAcquire() != 0)
    return false;
  b.readers.fetchAdd(1);
  if (b.writeFlag.loadAcquire() != 0) {
    b.readers.fetchSub(1);
    return false;
  }
  return true;
#endif
}
void bucketReadRelease(EJitSharedCacheBucket &b) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  (void)b; // no token was taken.
#else
  b.readers.fetchSub(1);
#endif
}
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
// Seqlock begin: snapshot the publish sequence. Returns false if a publish is in
// progress (odd), so the caller cleanly falls back rather than reading a slot
// mid-overwrite.
static inline bool bucketSeqBegin(EJitSharedCacheBucket &b, uint32_t &seq) {
  seq = b.publishSeq.loadAcquire();
  return (seq & 1u) == 0;
}
// Seqlock end: the read is consistent iff the sequence is unchanged (no publish
// to this bucket happened during the scan + fnPtr load). The compiler barrier
// keeps the guarded field loads from being reordered across this check.
static inline bool bucketSeqStable(EJitSharedCacheBucket &b, uint32_t seq0) {
  asm volatile("" ::: "memory");
  return b.publishSeq.loadAcquire() == seq0;
}
#endif
void bucketWrite(EJitSharedCacheBucket &b) {
  uint32_t expected = 0;
  while (!b.writeFlag.compareExchange(expected, 1))
    expected = 0;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Enter the odd (writing) phase before any slot field is written, so a
  // concurrent seqlock reader that began even will observe the change.
  b.publishSeq.fetchAdd(1);
#endif
  while (b.readers.loadAcquire() != 0)
    cpuRelax();
}
void bucketWriteRelease(EJitSharedCacheBucket &b) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Leave the even (stable) phase after all slot fields are published.
  b.publishSeq.fetchAdd(1);
#endif
  b.writeFlag.storeRelease(0);
}

constexpr uint32_t kReady = static_cast<uint32_t>(EJitSharedInitState::Ready);

/// Mixing constant shared by hashIdentity() and every unrolled fixed-dimension
/// lookup (cacheLookupNd / cacheLookupSeqNd). Kept here so the NO_RECLAIM
/// fixed-dimension seqlock specializations defined above the generic
/// cacheLookupNd block can also reference it.
constexpr uint64_t kHashMul = 0x9e3779b97f4a7c15ULL;

} // namespace

//===----------------------------------------------------------------------===//
// Switch controller helpers (§5.1) over shared arrays.
//===----------------------------------------------------------------------===//
bool EJitSharedTaskPool::isInstanceEnabled(uint32_t dimType,
                                           uint32_t instanceId) const {
  if (dimType >= kEJitSharedDimTypes || instanceId >= kEJitSharedInstances)
    return false;
  return state_->enabled[dimType][instanceId].loadRelaxed() != 0;
}

bool EJitSharedTaskPool::isInstanceActive(uint32_t dimType,
                                          uint32_t instanceId) const {
  // Public read counterpart of setInstanceEnabled. The compile gate
  // (compileCold) and ejit_is_active consult THIS shared bit, not an
  // owner-private copy, so producer (activate) and worker (compile) see the
  // same cross-core fact.
  return isInstanceEnabled(dimType, instanceId);
}

uint32_t EJitSharedTaskPool::instanceVersion(uint32_t dimType,
                                             uint32_t instanceId) const {
  if (dimType >= kEJitSharedDimTypes || instanceId >= kEJitSharedInstances)
    return 0;
  return state_->version[dimType][instanceId].loadAcquire();
}

void EJitSharedTaskPool::forEachCompiled(CompiledFuncCallback cb,
                                         void *ctx) const {
  if (!state_ || !cb)
    return;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b) {
    EJitSharedCacheBucket &B = state_->buckets[b];
    // Acquire the per-bucket read lock; retry briefly if a writer is
    // mid-publish, then skip the bucket if still contended (diagnostic — a
    // missing entry here just means it was being updated this instant).
    bool locked = false;
    for (int retry = 0; retry < 16; ++retry) {
      if (bucketTryRead(B)) {
        locked = true;
        break;
      }
      cpuRelax();
    }
    if (!locked)
      continue;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      const EJitSharedCacheSlot &slot = B.slots[s];
      if (static_cast<EJitSharedSlotState>(slot.state.loadAcquire()) !=
          EJitSharedSlotState::Ready)
        continue;
      void *fn = reinterpret_cast<void *>(slot.fnPtr.loadAcquire());
      cb(slot.funcIndex, slot.dims, slot.numDims, fn, ctx);
    }
    bucketReadRelease(B);
  }
}

bool EJitSharedTaskPool::setInstanceEnabled(uint32_t dimType,
                                            uint32_t instanceId, bool enabled) {
  if (!state_ || dimType >= kEJitSharedDimTypes ||
      instanceId >= kEJitSharedInstances) {
    EJIT_DIAG("shared setInstanceEnabled reject: state=%p dim=%u inst=%u "
              "(OOR dim<%u inst<%u)",
              (void *)state_, dimType, instanceId, kEJitSharedDimTypes,
              kEJitSharedInstances);
    return false;
  }
  uint8_t expected = enabled ? 0 : 1;
  uint8_t desired = enabled ? 1 : 0;
  if (state_->enabled[dimType][instanceId].compareExchange(expected, desired)) {
    state_->version[dimType][instanceId].fetchAdd(1);
    // Latch on the first successful enable of ANY instance: this brackets the
    // init→activate window during which instanceDisabled hits are tallied
    // separately (instanceDisabledPreActivate) for diagnosing the pre-activate
    // fallback storm. Once latched it stays 1 until the next (re)initialization.
    if (enabled)
      state_->anyInstanceActivated.storeRelease(1);
    return true;
  }
  return false;
}

bool EJitSharedTaskPool::versionsCurrent(const EJitCompileRequest &req) const {
  for (uint32_t i = 0; i < req.numDims; ++i)
    if (req.versions[i] !=
        instanceVersion(req.dims[i].dimType, req.dims[i].instanceId))
      return false;
  return true;
}

//===----------------------------------------------------------------------===//
// Dedup helpers (§3.5) over the shared flat slots. Each slot stores the OWNER
// GENERATION that claimed it (0 = free), so cross-generation clears are
// impossible (spec §11 generation-aware dedup).
//===----------------------------------------------------------------------===//
EJitDedupResult EJitSharedTaskPool::dedupMark(uint32_t funcIndex,
                                              uint32_t gen) {
  if (funcIndex >= kEJitSharedMaxFuncIndex)
    return EJitDedupResult::InvalidFuncIndex;
  uint32_t expected = 0;
  if (state_->inFlight[funcIndex].compareExchange(expected, gen))
    return EJitDedupResult::Claimed;
  return EJitDedupResult::AlreadyPending;
}

void EJitSharedTaskPool::dedupClear(uint32_t funcIndex, uint32_t gen) {
  if (funcIndex >= kEJitSharedMaxFuncIndex)
    return;
  // CAS gen->0: only clears the slot if it still holds OUR generation. A stale
  // worker whose generation was superseded (or whose slot was re-claimed by a
  // new generation after an owner re-init) fails this CAS and clears nothing.
  uint32_t expected = gen;
  state_->inFlight[funcIndex].compareExchange(expected, 0u);
}

//===----------------------------------------------------------------------===//
// MPSC queue helpers (Vyukov, §3.3) over the shared ring.
//===----------------------------------------------------------------------===//
bool EJitSharedTaskPool::queuePush(const EJitCompileRequest &req) {
  constexpr uint32_t mask = kEJitSharedQueueSlots - 1;
  uint32_t pos = state_->enqueuePos.loadRelaxed();
  EJitSharedQueueCell *cell;
  for (;;) {
    cell = &state_->ring[pos & mask];
    uint32_t seq = cell->sequence.loadAcquire();
    int32_t dif = static_cast<int32_t>(seq) - static_cast<int32_t>(pos);
    if (dif == 0) {
      if (state_->enqueuePos.compareExchange(pos, pos + 1))
        break;
    } else if (dif < 0) {
      return false; // full
    } else {
      pos = state_->enqueuePos.loadRelaxed();
    }
  }
  cell->data = req;
  cell->sequence.storeRelease(pos + 1);
  return true;
}

bool EJitSharedTaskPool::queuePop(EJitCompileRequest &out) {
  constexpr uint32_t mask = kEJitSharedQueueSlots - 1;
  uint32_t pos = state_->dequeuePos.loadRelaxed();
  EJitSharedQueueCell *cell;
  for (;;) {
    cell = &state_->ring[pos & mask];
    uint32_t seq = cell->sequence.loadAcquire();
    int32_t dif = static_cast<int32_t>(seq) - static_cast<int32_t>(pos + 1);
    if (dif == 0) {
      if (state_->dequeuePos.compareExchange(pos, pos + 1))
        break;
    } else if (dif < 0) {
      return false; // empty
    } else {
      pos = state_->dequeuePos.loadRelaxed();
    }
  }
  out = cell->data;
  cell->sequence.storeRelease(pos + mask + 1);
  return true;
}

//===----------------------------------------------------------------------===//
// Shared POD result cache (§4.1, re-expressed without std::unordered_map).
//===----------------------------------------------------------------------===//
uint64_t EJitSharedTaskPool::hashIdentity(uint32_t funcIndex,
                                          const EJitDimPair *dims,
                                          uint32_t numDims) const {
  uint64_t key = static_cast<uint64_t>(funcIndex);
  for (uint32_t i = 0; i < numDims; ++i) {
    key ^= (static_cast<uint64_t>(dims[i].dimType) << 32) |
           static_cast<uint64_t>(dims[i].instanceId);
    key *= 0x9e3779b97f4a7c15ULL;
  }
  return key;
}

static bool slotIdentityMatches(const EJitSharedCacheSlot &s,
                                uint32_t funcIndex, const EJitDimPair *dims,
                                uint32_t numDims) {
  if (s.funcIndex != funcIndex || s.numDims != numDims)
    return false;
  for (uint32_t i = 0; i < numDims; ++i)
    if (s.dims[i].dimType != dims[i].dimType ||
        s.dims[i].instanceId != dims[i].instanceId)
      return false;
  return true;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup(uint32_t funcIndex, const EJitDimPair *dims,
                                uint32_t numDims) {
  SharedLookup R;
  if (numDims > 4)
    return R;
  uint64_t key = hashIdentity(funcIndex, dims, numDims);
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;

  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen) // stale across an owner re-init: ignore.
      continue;
    if (!slotIdentityMatches(Slot, funcIndex, dims, numDims))
      continue;
    bool versionsOk = true;
    for (uint32_t i = 0; i < numDims; ++i)
      if (Slot.versions[i] !=
          instanceVersion(dims[i].dimType, dims[i].instanceId)) {
        versionsOk = false;
        break;
      }
    if (!versionsOk)
      break; // identity matched but stale → miss, token off.
    // Identity + versions match: resolve the fnPtr gate (owner/memoized fast
    // return, clean reject, or cold peer preparation). Terminal — a bucket
    // holds at most one slot per identity.
    return resolveMatchedSlot(B, bucket, s);
  }

  bucketReadRelease(B);
  return R;
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
//===----------------------------------------------------------------------===//
// Load-only seqlock cache lookup (NO_RECLAIM build only). Same result as
// cacheLookup() but with ZERO per-hit RMW on the shared bucket line: the reader
// never touches bucket.readers. Consistency is a seqlock over bucket.publishSeq
// (snapshot before the scan, re-check after resolving). If a publish raced the
// read, retry a bounded number of times, then clean-miss and let the caller
// fall back. Memory-safe ONLY because a published fnPtr is never freed in this
// build, so a returned pointer that holds no read token can never dangle.
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq(uint32_t funcIndex, const EJitDimPair *dims,
                                   uint32_t numDims) {
  SharedLookup R;
  if (numDims > 4)
    return R;
  uint64_t key = hashIdentity(funcIndex, dims, numDims);
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) { // publish in progress -> retry
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue; // plain-load fast reject before the acquiring state load.
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (!slotIdentityMatches(Slot, funcIndex, dims, numDims))
        continue;
      bool versionsOk = true;
      for (uint32_t i = 0; i < numDims; ++i)
        if (Slot.versions[i] !=
            instanceVersion(dims[i].dimType, dims[i].instanceId)) {
          versionsOk = false;
          break;
        }
      if (!versionsOk)
        break; // identity matched but stale -> clean miss
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    // Validate that the scan + resolve observed a single stable publish epoch.
    // A cold peer preparation (coldPrepared) re-validates itself and may legally
    // span a publish, so it is exempt from the outer seq re-check.
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue; // publish raced the read -> retry
    }
    return hit; // validated hit (noTokenHit) or clean miss
  }
  return R; // repeated contention -> clean fallback to the slow path
}

//===----------------------------------------------------------------------===//
// Fixed-dimension load-only seqlock specializations (0-2 dims, NO_RECLAIM
// build only). Each mirrors the matching cacheLookup0D/1D/2D scan (unrolled
// identity hash, unrolled identity + version comparison, plain-identityHash
// fast reject before the acquiring state load) wrapped in the same bounded
// seqlock retry as cacheLookupSeq(). A NO_RECLAIM fixed-dimension caller thus
// avoids the generic numDims loop / slotIdentityMatches() call entirely.
// Behavior is identical to cacheLookupSeq() with the matching numDims; the
// cross-core fnPtr gate and cold peer preparation stay shared via
// resolveMatchedSlot().
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq0D(uint32_t funcIndex) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex); // hashIdentity(fi, _, 0)
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue; // plain-load fast reject before the acquiring state load.
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (Slot.funcIndex != funcIndex || Slot.numDims != 0)
        continue;
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue;
    }
    return hit;
  }
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq1D(uint32_t funcIndex, uint32_t dim0,
                                     uint32_t inst0) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue;
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (Slot.funcIndex != funcIndex || Slot.numDims != 1)
        continue;
      if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
        continue;
      if (Slot.versions[0] != instanceVersion(dim0, inst0))
        break; // identity matched but stale -> clean miss.
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue;
    }
    return hit;
  }
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookupSeq2D(uint32_t funcIndex, uint32_t dim0,
                                     uint32_t inst0, uint32_t dim1,
                                     uint32_t inst1) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  for (uint32_t attempt = 0; attempt < 4; ++attempt) {
    uint32_t seq0;
    if (!bucketSeqBegin(B, seq0)) {
      cpuRelax();
      continue;
    }
    uint32_t curGen = state_->generation.loadAcquire();
    SharedLookup hit;
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = B.slots[s];
      if (Slot.identityHash != key)
        continue;
      if (Slot.state.loadAcquire() !=
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        continue;
      if (Slot.generation != curGen)
        continue;
      if (Slot.funcIndex != funcIndex || Slot.numDims != 2)
        continue;
      if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
        continue;
      if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
        continue;
      if (Slot.versions[0] != instanceVersion(dim0, inst0))
        break;
      if (Slot.versions[1] != instanceVersion(dim1, inst1))
        break;
      hit = resolveMatchedSlot(B, bucket, s);
      break;
    }
    if (hit.fnPtr && !hit.coldPrepared && !bucketSeqStable(B, seq0)) {
      cpuRelax();
      continue;
    }
    return hit;
  }
  return R;
}
#endif // EJIT_SRE_TASKPOOL_NO_RECLAIM

//===----------------------------------------------------------------------===//
// Shared slot resolution: applied once a slot's identity + versions have
// matched (bucket read lock held). Dimension-independent, so cacheLookup() and
// every fixed-dimension cacheLookupNd() share it. The owner core and any core
// that already memoized execute permission return the hit here with the read
// token held; a core that may not legally read the cross-core pointer gets a
// clean readyButNotShareable fallback; the rare non-owner first touch is
// delegated to the out-of-line peerPrepareSlot().
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::resolveMatchedSlot(EJitSharedCacheBucket &B,
                                       uint32_t bucket, uint32_t slotIndex) {
  SharedLookup R;
  EJitSharedCacheSlot &Slot = B.slots[slotIndex];
  // fnPtr cross-core gate (§11 prerequisites): a non-owner core may read the
  // pointer only when code sharing is platform-validated (same VA, sealed,
  // I/D-cache coherent). Otherwise CLEAN-REJECT — never hand back a pointer
  // this core may not legally execute.
  uint32_t self = EJitCoreId::current();
  // ownerCoreId is immutable for the lifetime of a Ready pool: written exactly
  // once before initState is published Ready (release, "publish last"), and
  // every hit already observed initState==Ready via the acquire Ready-check at
  // entry - which makes that write visible here. It never changes during Ready,
  // so there is no concurrent write to order against; a relaxed re-load
  // suffices. The slot's state/fnPtr acquire loads still gate code publication
  // independently below.
  uint32_t owner = state_->ownerCoreId.loadRelaxed();
  // codeSharingEnabled's value is fixed at compile time by
  // EJIT_SRE_SHARED_CODE_POINTERS (the only setCodeSharingEnabled() calls are
  // flag-gated in EJitCompileDriver.cpp), so the per-hit runtime load is
  // redundant and replaced by the compile-time constant.
#if defined(EJIT_SRE_SHARED_CODE_POINTERS)
  // Sharing unconditionally enabled in this build: any core may read the
  // published pointer. The !mayReadPtr fallback below is dead-coded out.
  constexpr bool mayReadPtr = true;
#else
  // Sharing unconditionally disabled in this build: only the owner may read
  // its own published pointer. (Equivalent to the former
  // (codeSharingEnabled!=0) || (self==owner) with codeSharingEnabled==0.)
  bool mayReadPtr = (self == owner);
#endif
  if (!mayReadPtr) {
    bucketReadRelease(B);
    R.readyButNotShareable = true;
    return R;
  }
  void *fn = reinterpret_cast<void *>(Slot.fnPtr.loadAcquire());
  if (!fn) {
    bucketReadRelease(B);
    return R; // identity matched but no pointer yet → miss.
  }

  // The owner already has execute permission (it sealed the code before
  // publishing). Return directly while holding the read token.
  if (self == owner) {
    R.fnPtr = fn;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
    R.noTokenHit = true;
    R.bucketIndex = kEJitSharedCacheBuckets; // sentinel -> releaseRead no-op
#else
    R.bucketIndex = bucket;
    R.hasReadToken = true;
#endif
    return R;
  }

  // Non-owner: execute permission is a per-core property on the target. If this
  // core has already prepared THIS slot's code (memoized bit), return
  // immediately under the read lock — no platform call.
  const bool CanMemoize = self < kEJitSharedMaxMemoCores;
  const uint64_t CoreBit = CanMemoize ? (uint64_t{1} << self) : uint64_t{0};
  if (CanMemoize && (Slot.executableCoreMask.loadAcquire() & CoreBit) != 0) {
    R.fnPtr = fn;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
    R.noTokenHit = true;
    R.bucketIndex = kEJitSharedCacheBuckets; // sentinel -> releaseRead no-op
#else
    R.bucketIndex = bucket;
    R.hasReadToken = true;
#endif
    return R;
  }

  // Cold: this core has never prepared this code. Delegate to the out-of-line
  // helper so the hit path stays small.
  return peerPrepareSlot(B, bucket, slotIndex);
}

//===----------------------------------------------------------------------===//
// Cold non-owner first-touch execute-permission preparation. Out-of-line
// (noinline) so it never inflates the cache-hit path. Bucket read lock held on
// entry; released here.
//===----------------------------------------------------------------------===//
__attribute__((noinline)) EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::peerPrepareSlot(EJitSharedCacheBucket &B, uint32_t bucket,
                                    uint32_t slotIndex) {
  SharedLookup R;
  EJitSharedCacheSlot &Slot = B.slots[slotIndex];
  uint32_t self = EJitCoreId::current();
  void *fn = reinterpret_cast<void *>(Slot.fnPtr.loadAcquire());

  // Snapshot the identity + the REAL code range, then DROP the bucket read
  // lock: the per-core platform split/enable_ex calls (potentially slow) must
  // never run while holding a cross-core bucket lock, or a concurrent owner
  // publish (which spins for readers==0) would stall.
  PeerCodeRange Snap;
  Snap.fn = fn;
  Snap.slotIndex = slotIndex;
  Snap.bucket = bucket;
  Snap.funcIndex = Slot.funcIndex;
  Snap.numDims = Slot.numDims;
  Snap.generation = Slot.generation;
  for (uint32_t i = 0; i < Snap.numDims && i < 4; ++i) {
    Snap.dims[i] = Slot.dims[i];
    Snap.versions[i] = Slot.versions[i];
  }
  Snap.codeStart = Slot.codeStart;
  Snap.codeSize = Slot.codeSize;
  Snap.poolBase = Slot.poolBase;
  Snap.poolSize = Slot.poolSize;
  bucketReadRelease(B);

  if (!prepareExecForCurrentCore(Snap, self)) {
    EJIT_STAT_INC(state_->counters.executePrepareFailed);
    R.readyButNotShareable = true;
    return R; // clean fallback: no token, no shared pointer handed back.
  }

  // Re-acquire the read lock and RE-VALIDATE the slot: a publish may have
  // overwritten it (new code/generation) while we prepared. Only a slot that is
  // still Ready, same generation + identity + fnPtr, with current versions, may
  // hand back the prepared pointer; otherwise clean fallback so a
  // replaced/stale pointer is never returned (spec §11 / §8).
  if (!bucketTryRead(B)) {
    R.readyButNotShareable = true;
    return R;
  }
  EJitSharedCacheSlot &S2 = B.slots[Snap.slotIndex];
  uint32_t curGen2 = state_->generation.loadAcquire();
  bool stillValid =
      S2.state.loadAcquire() ==
          static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
      S2.generation == curGen2 && S2.generation == Snap.generation &&
      slotIdentityMatches(S2, Snap.funcIndex, Snap.dims, Snap.numDims) &&
      reinterpret_cast<void *>(S2.fnPtr.loadAcquire()) == Snap.fn;
  if (stillValid)
    for (uint32_t i = 0; i < Snap.numDims; ++i)
      if (S2.versions[i] !=
          instanceVersion(Snap.dims[i].dimType, Snap.dims[i].instanceId)) {
        stillValid = false;
        break;
      }
  if (!stillValid) {
    bucketReadRelease(B);
    R.readyButNotShareable = true;
    return R;
  }
  const bool CanMemoize = self < kEJitSharedMaxMemoCores;
  const uint64_t CoreBit = CanMemoize ? (uint64_t{1} << self) : uint64_t{0};
  if (CanMemoize)
    S2.executableCoreMask.fetchOr(CoreBit);
  R.fnPtr = Snap.fn;
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // NO_RECLAIM: this cold path already fully re-validated identity/versions/fnPtr
  // above under a load-only re-read, so it hands back a validated pointer with no
  // read token. coldPrepared tells the seqlock caller not to re-check publishSeq
  // (this out-of-line path legally spanned platform calls and possible publishes,
  // but the re-validation guarantees the returned pointer is current).
  R.noTokenHit = true;
  R.coldPrepared = true;
  R.bucketIndex = kEJitSharedCacheBuckets; // sentinel -> releaseRead no-op
#else
  R.bucketIndex = Snap.bucket;
  R.hasReadToken = true;
#endif
  return R; // token held (re-acquired); caller releases after using fnPtr.
}

//===----------------------------------------------------------------------===//
// Fixed-dimension cacheLookup specializations (0-4 dims). Identity hashing,
// slot identity comparison, and version comparison are unrolled — no numDims
// loop, no dims[] indirection, no generic numDims>4 guard. The matched-slot
// fnPtr gate and cold peer preparation are shared via resolveMatchedSlot(), so
// each specialization stays small. Results are identical to cacheLookup() with
// the matching numDims. kHashMul mirrors the mixing constant in hashIdentity().
//===----------------------------------------------------------------------===//

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup0D(uint32_t funcIndex) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex); // hashIdentity(fi, _, 0)
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 0)
      continue;
    // 0D: no dims, no versions to compare.
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup1D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 1)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break; // identity matched but stale → miss.
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup2D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1,
                                  uint32_t inst1) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 2)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break;
    if (Slot.versions[1] != instanceVersion(dim1, inst1))
      break;
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup3D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim2) << 32) | static_cast<uint64_t>(inst2);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 3)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
      continue;
    if (Slot.dims[2].dimType != dim2 || Slot.dims[2].instanceId != inst2)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break;
    if (Slot.versions[1] != instanceVersion(dim1, inst1))
      break;
    if (Slot.versions[2] != instanceVersion(dim2, inst2))
      break;
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

EJitSharedTaskPool::SharedLookup
EJitSharedTaskPool::cacheLookup4D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2, uint32_t dim3,
                                  uint32_t inst3) {
  SharedLookup R;
  uint64_t key = static_cast<uint64_t>(funcIndex);
  key ^= (static_cast<uint64_t>(dim0) << 32) | static_cast<uint64_t>(inst0);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim1) << 32) | static_cast<uint64_t>(inst1);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim2) << 32) | static_cast<uint64_t>(inst2);
  key *= kHashMul;
  key ^= (static_cast<uint64_t>(dim3) << 32) | static_cast<uint64_t>(inst3);
  key *= kHashMul;
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];
  if (!bucketTryRead(B))
    return R;
  uint32_t curGen = state_->generation.loadAcquire();
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.identityHash != key)
      continue; // plain-load fast reject before the acquiring state load.
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen)
      continue;
    if (Slot.funcIndex != funcIndex || Slot.numDims != 4)
      continue;
    if (Slot.dims[0].dimType != dim0 || Slot.dims[0].instanceId != inst0)
      continue;
    if (Slot.dims[1].dimType != dim1 || Slot.dims[1].instanceId != inst1)
      continue;
    if (Slot.dims[2].dimType != dim2 || Slot.dims[2].instanceId != inst2)
      continue;
    if (Slot.dims[3].dimType != dim3 || Slot.dims[3].instanceId != inst3)
      continue;
    if (Slot.versions[0] != instanceVersion(dim0, inst0))
      break;
    if (Slot.versions[1] != instanceVersion(dim1, inst1))
      break;
    if (Slot.versions[2] != instanceVersion(dim2, inst2))
      break;
    if (Slot.versions[3] != instanceVersion(dim3, inst3))
      break;
    return resolveMatchedSlot(B, bucket, s);
  }
  bucketReadRelease(B);
  return R;
}

//===----------------------------------------------------------------------===//
// Per-core execute-permission preparation (no bucket lock held). 2M mode seals
// the whole 2MiB pool via prepareCodeFn_ (fnPtr-aligned); 4K mode splits the
// pool once per core, then seals exactly the 4KiB pages the code covers. All
// platform work goes through injected callbacks so this core never names a
// platform symbol (spec §7).
//===----------------------------------------------------------------------===//
EJitSharedPoolSplit *EJitSharedTaskPool::findOrClaimPoolSlot(uintptr_t base) {
  // Open addressing keyed by pool base. Every core probes the SAME slot
  // sequence for a given base, so concurrent first-touch converges on one entry
  // (never two entries for the same pool, which would let a core split twice).
  uint32_t start = static_cast<uint32_t>((base / kEJitSharedSplitGranule) %
                                         kEJitSharedPoolSlots);
  for (uint32_t probe = 0; probe < kEJitSharedPoolSlots; ++probe) {
    uint32_t i = (start + probe) % kEJitSharedPoolSlots;
    EJitSharedPoolSplit &P = state_->poolSplits[i];
    uintptr_t cur = P.poolBase.loadAcquire();
    if (cur == base)
      return &P;
    if (cur == 0) {
      uintptr_t expected = 0;
      if (P.poolBase.compareExchange(expected, base))
        return &P; // we claimed this empty entry for `base`.
      // Lost the race: `expected` now holds the winner's base.
      if (expected == base)
        return &P; // another core claimed the same base here.
      // Claimed by a different base — keep probing.
    }
    // Occupied by a different base — keep probing.
  }
  return nullptr; // table full: caller cleanly falls back.
}

bool EJitSharedTaskPool::ensurePoolSplitForCurrentCore(uint32_t self,
                                                       uintptr_t poolBase,
                                                       uint64_t poolSize) {
  EJIT_DIAG_VERBOSE("ensurePoolSplit: core=%u poolBase=0x%llx size=%llu", self,
            static_cast<unsigned long long>(poolBase),
            static_cast<unsigned long long>(poolSize));
  // Cores beyond the 64-bit memo width cannot record split state, so they must
  // re-run the (idempotent) split on every hit rather than risk skipping it.
  if (self >= kEJitSharedMaxMemoCores) {
    bool ok = splitPoolFn_ && splitPoolFn_(splitPoolCtx_, poolBase, poolSize);
    if (!ok)
      EJIT_DIAG("ensurePoolSplit FAIL: core=%u >= memoWidth, split callback "
                "missing/failed", self);
    return ok;
  }

  EJitSharedPoolSplit *P = findOrClaimPoolSlot(poolBase);
  if (!P) {
    EJIT_DIAG_VERBOSE("ensurePoolSplit fallback: core=%u poolBase=0x%llx split table full",
              self, static_cast<unsigned long long>(poolBase));
    return false; // table full -> clean fallback.
  }

  const uint64_t Bit = uint64_t{1} << self;
  if ((P->splitDoneMask.loadAcquire() & Bit) != 0)
    return true; // already split on this core.

  // Claim the per-core "preparing" bit. fetchOr returns the previous mask.
  const uint64_t Prev = P->splitPreparingMask.fetchOr(Bit);
  if ((Prev & Bit) != 0) {
    // Another context on THIS core is mid-split: wait (bounded) for it to
    // publish done; if it clears preparing without done (it failed) or the
    // bound elapses, cleanly fall back so we never proceed un-split.
    constexpr uint32_t kSplitSpinBudget = 1u << 16;
    for (uint32_t i = 0; i < kSplitSpinBudget; ++i) {
      if ((P->splitDoneMask.loadAcquire() & Bit) != 0)
        return true;
      if ((P->splitPreparingMask.loadAcquire() & Bit) == 0)
        break; // the other context finished without marking done -> it failed.
      cpuRelax();
    }
    bool done = (P->splitDoneMask.loadAcquire() & Bit) != 0;
    if (!done)
      EJIT_DIAG_VERBOSE("ensurePoolSplit fallback: core=%u poolBase=0x%llx peer split "
                "did not publish done", self,
                static_cast<unsigned long long>(poolBase));
    return done;
  }

  // We own the split for this (pool, core). Run it, publishing done only on
  // success; on failure roll back preparing so a later hit can retry.
  bool ok = splitPoolFn_ && splitPoolFn_(splitPoolCtx_, poolBase, poolSize);
  if (ok) {
    P->splitDoneMask.fetchOr(Bit);
    P->splitPreparingMask.fetchAnd(~Bit);
    return true;
  }
  EJIT_DIAG("ensurePoolSplit FAIL: core=%u poolBase=0x%llx split callback failed",
            self, static_cast<unsigned long long>(poolBase));
  P->splitPreparingMask.fetchAnd(~Bit);
  return false;
}

bool EJitSharedTaskPool::prepareExecForCurrentCore(const PeerCodeRange &R,
                                                   uint32_t self) {
  EJIT_DIAG_VERBOSE("prepareExec: core=%u fn=%p codeStart=0x%llx codeSize=%llu "
            "poolBase=0x%llx fourK=%u", self, R.fn,
            static_cast<unsigned long long>(R.codeStart),
            static_cast<unsigned long long>(R.codeSize),
            static_cast<unsigned long long>(R.poolBase),
            static_cast<unsigned>(fourKSeal_));
  if (!fourKSeal_) {
    // Legacy whole-2MiB-pool seal: align fnPtr to its pool base and enable_ex
    // that page. Range metadata is not required (spec §6 — 2M unchanged). When
    // no prepare callback is wired the platform/host needs no per-core
    // preparation (e.g. the single shared address space of the host
    // simulation), so the pointer is already executable on this core.
    if (!prepareCodeFn_)
      return true;
    bool ok = prepareCodeFn_(prepareCodeCtx_, R.fn);
    if (!ok)
      EJIT_DIAG("prepareExec FAIL: core=%u legacy prepareCode fn=%p", self, R.fn);
    return ok;
  }

  // 4K page seal needs the real executable extent. A slot with no recorded
  // range (or a malformed one) is a clean fallback, never a guessed seal.
  if (R.codeStart == 0 || R.codeSize == 0 || R.poolBase == 0 || R.poolSize == 0) {
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u fn=%p malformed range "
              "(codeStart=0x%llx codeSize=%llu poolBase=0x%llx poolSize=%llu)",
              self, R.fn, static_cast<unsigned long long>(R.codeStart),
              static_cast<unsigned long long>(R.codeSize),
              static_cast<unsigned long long>(R.poolBase),
              static_cast<unsigned long long>(R.poolSize));
    return false;
  }
  if (R.codeStart + R.codeSize < R.codeStart) { // code range overflow
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u code range overflow", self);
    return false;
  }
  if (R.poolBase + R.poolSize < R.poolBase) { // pool range overflow
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u pool range overflow", self);
    return false;
  }
  if (R.codeStart < R.poolBase ||
      R.codeStart + R.codeSize > R.poolBase + R.poolSize) {
    EJIT_DIAG_VERBOSE("prepareExec fallback: core=%u code not inside pool", self);
    return false; // code must lie wholly inside its pool.
  }

  // This core must have split the 2MiB pool exactly once before sealing pages.
  if (!ensurePoolSplitForCurrentCore(self, R.poolBase, R.poolSize)) {
    EJIT_DIAG("prepareExec FAIL: core=%u pool split not done", self);
    return false;
  }

  // Seal every 4KiB page the code overlaps: page-align start down, end up.
  const uintptr_t Page = static_cast<uintptr_t>(kEJitSharedSealPage);
  uintptr_t PageStart = R.codeStart & ~(Page - 1);
  uintptr_t PageEnd =
      (R.codeStart + static_cast<uintptr_t>(R.codeSize) + Page - 1) &
      ~(Page - 1);
  for (uintptr_t VA = PageStart; VA < PageEnd; VA += Page)
    if (!sealPageFn_ || !sealPageFn_(sealPageCtx_, VA)) {
      EJIT_DIAG("prepareExec FAIL: core=%u sealPage pageVA=0x%llx",
                self, static_cast<unsigned long long>(VA));
      return false; // any page failure -> no callable pointer is returned.
    }
  EJIT_DIAG_VERBOSE("prepareExec OK: core=%u fn=%p pages=[0x%llx,0x%llx)", self, R.fn,
            static_cast<unsigned long long>(PageStart),
            static_cast<unsigned long long>(PageEnd));
  return true;
}

EJitPublishStatus
EJitSharedTaskPool::cachePublish(const EJitCompileRequest &req, void *fnPtr,
                                 const EJitCompiledCodeInfo *info) {
  if (!fnPtr || req.numDims > 4)
    return EJitPublishStatus::InvalidParam;
  // PGO: tier rides in req.funcIndex's top 2 bits (EJitSreQueue.h). Strip it
  // for identity (slots store the stripped funcIndex so Tier-1/Tier-2 of the
  // same (funcIndex, dims) match); decode for the future trigger (part3).
  uint32_t tier = decodeReqTier(req.funcIndex);
  uint32_t fidx = stripReqTier(req.funcIndex);
  uint64_t key = hashIdentity(fidx, req.dims, req.numDims);
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  EJitSharedCacheBucket &B = state_->buckets[bucket];

  bucketWrite(B); // spins until readers drain to 0: no use-after-free on free.

  // Commit gate (§5.3/§5.4): re-verify the version snapshot under the lock.
  for (uint32_t i = 0; i < req.numDims; ++i)
    if (req.versions[i] !=
        instanceVersion(req.dims[i].dimType, req.dims[i].instanceId)) {
      bucketWriteRelease(B);
      return EJitPublishStatus::VersionMismatch;
    }

  uint32_t curGen = state_->generation.loadAcquire();
  // Generation gate (spec §11): use the REQUEST's generation, never silently
  // substitute the current one. A request whose generation has been superseded
  // (owner re-init between enqueue and publish) is rejected here.
  if (req.generation != curGen) {
    bucketWriteRelease(B);
    return EJitPublishStatus::VersionMismatch;
  }
  EJitSharedCacheSlot *target = nullptr;
  EJitSharedCacheSlot *firstEmpty = nullptr;
  EJitSharedCacheSlot *evict = nullptr;
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    EJitSharedCacheSlot &Slot = B.slots[s];
    uint32_t st = Slot.state.loadAcquire();
    if (st != static_cast<uint32_t>(EJitSharedSlotState::Empty) &&
        Slot.generation == req.generation &&
        slotIdentityMatches(Slot, fidx, req.dims, req.numDims)) {
      target = &Slot; // overwrite same identity in place
      break;
    }
    if (!firstEmpty && st == static_cast<uint32_t>(EJitSharedSlotState::Empty))
      firstEmpty = &Slot;
    if (!evict && st != static_cast<uint32_t>(EJitSharedSlotState::Empty))
      evict = &Slot; // first occupied: deterministic eviction victim
  }
  if (!target)
    target = firstEmpty ? firstEmpty : evict; // bucket full → evict slot 0-ish.
  if (!target) {
    bucketWriteRelease(B);
    return EJitPublishStatus::Failed;
  }

  void *oldFn = reinterpret_cast<void *>(target->fnPtr.loadAcquire());

  target->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Publishing));
  target->funcIndex = fidx;
  target->numDims = req.numDims;
  target->generation = req.generation; // the request's generation, not curGen
  target->identityHash = key;
  for (uint32_t i = 0; i < req.numDims; ++i) {
    target->dims[i] = req.dims[i];
    target->versions[i] = req.versions[i];
  }
  // Copy the real executable range (v5) so a peer core can later seal exactly
  // the 4KiB pages this code covers. Written BEFORE the fnPtr/state release
  // below, so an acquiring reader that observes state==Ready also sees a
  // consistent range. A null/empty info clears the range (0 codeSize), which a
  // peer treats as "no range -> clean fallback".
  if (info && info->codeSize != 0) {
    target->codeStart = info->codeStart;
    target->codeSize = info->codeSize;
    target->poolBase = info->poolBase;
    target->poolSize = info->poolSize;
    target->poolId = info->poolId;
  } else {
    target->codeStart = 0;
    target->codeSize = 0;
    target->poolBase = 0;
    target->poolSize = 0;
    target->poolId = 0;
  }
  target->rangeReserved = 0;
  target->fnPtr.storeRelease(reinterpret_cast<uintptr_t>(fnPtr));
  // PGO: reset stale hitCount + counter addrs on every publish (overwrite of a
  // Tier-1 slot, or reuse of an evicted slot). Under the write lock + before
  // state=Ready, so storeRelaxed is published by the Ready release (§7.1).
  target->hitCount.storeRelaxed(0);
  target->profcAddr.storeRelaxed(0);
  target->profdAddr.storeRelaxed(0);
  const uint32_t OwnerCore = state_->ownerCoreId.loadAcquire();
  target->executableCoreMask.storeRelease(
      OwnerCore < 64 ? (uint64_t{1} << OwnerCore) : 0);
  target->state.storeRelease(static_cast<uint32_t>(EJitSharedSlotState::Ready));
  bucketWriteRelease(B);

  // Release the slot's PREVIOUS code OUTSIDE the bucket lock (the callback may
  // re-enter the code pool / ORC / allocator / platform). This covers both a
  // same-identity recompile (new address replaces old) and the eviction of a
  // different identity when the bucket was full. Readers already drained to 0
  // under the write lock and the slot now points at the new code, so the old
  // pointer is unreachable and safe to free.
  if (releaseFn_ && oldFn && oldFn != fnPtr)
    releaseFn_(releaseCtx_, oldFn);
  return EJitPublishStatus::Published;
}

void EJitSharedTaskPool::releaseRead(uint32_t bucketIndex) {
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // A load-only seqlock hit takes no read token and deliberately returns the
  // one-past-the-end bucket as a sentinel. Wrappers still call releaseRead()
  // uniformly, so this is an expected no-op rather than a rejected release.
  if (bucketIndex == kEJitSharedCacheBuckets)
    return;
#endif
  if (!state_ || bucketIndex >= kEJitSharedCacheBuckets) {
    EJIT_DIAG("shared releaseRead reject: state=%p bucket=%u (max=%u)",
              (void *)state_, bucketIndex, kEJitSharedCacheBuckets);
    return;
  }
  bucketReadRelease(state_->buckets[bucketIndex]);
}

//===----------------------------------------------------------------------===//
// Owner election + init (§11).
//===----------------------------------------------------------------------===//
namespace {
// Field-by-field init so it is correct on raw, uninitialized shared memory
// (never relies on a C++ constructor having run on the blob).
//
// enabled defaults to 0 (inactive): a period instance must be explicitly
// activated via ejit_activate before the JIT will compile it. This matches
// the non-shared EJitRuntimeState::isActive default (no entry => inactive).
// setInstanceEnabled(true) flips 0->1 and bumps version on first activate.
void initSharedStorage(EJitSharedTaskPoolState *st, uint32_t mode) {
  bool canFreeDumpPayload =
      st->magic == kEJitSharedAbiMagic &&
      st->abiVersion == kEJitSharedAbiVersion &&
      st->structSize == sizeof(EJitSharedTaskPoolState);
  for (uint32_t d = 0; d < kEJitSharedDimTypes; ++d)
    for (uint32_t i = 0; i < kEJitSharedInstances; ++i) {
      st->enabled[d][i].storeRelaxed(0);
      st->version[d][i].storeRelaxed(0);
    }
  st->mode.storeRelaxed(mode);
  st->anyInstanceActivated.storeRelaxed(0);
  for (uint32_t i = 0; i < kEJitSharedMaxFuncIndex; ++i)
    st->inFlight[i].storeRelaxed(0);
  for (uint32_t i = 0; i < kEJitSharedQueueSlots; ++i) {
    st->ring[i].sequence.storeRelaxed(i); // Vyukov initial sequence = index
  }
  st->enqueuePos.storeRelaxed(0);
  st->dequeuePos.storeRelaxed(0);
  st->counters.cacheHits.storeRelaxed(0);
  st->counters.asyncCompiles.storeRelaxed(0);
  st->counters.asyncEnqueues.storeRelaxed(0);
  st->counters.alreadyPending.storeRelaxed(0);
  st->counters.queueFull.storeRelaxed(0);
  st->counters.compileFailed.storeRelaxed(0);
  st->counters.publishFailed.storeRelaxed(0);
  st->counters.instanceDisabled.storeRelaxed(0);
  st->counters.instanceDisabledPreActivate.storeRelaxed(0);
  st->counters.executePrepareFailed.storeRelaxed(0);
  // Code-pool stats mirror: zero until the owner publishes the first snapshot
  // after a successful compile (the pools are owner-private and empty at init).
  st->codePoolStats.poolCount.storeRelaxed(0);
  st->codePoolStats.sealedCount.storeRelaxed(0);
  st->codePoolStats.activeCount.storeRelaxed(0);
  st->codePoolStats.usedBytes.storeRelaxed(0);
  st->codePoolStats.reservedBytes.storeRelaxed(0);
  st->codePoolStats.wastedBytes.storeRelaxed(0);
  st->codePoolStats.sealInvocations.storeRelaxed(0);
  st->codePoolStats.splitInvocations.storeRelaxed(0);
  st->codePoolStats.finalizedRangeCount.storeRelaxed(0);
  // Per-core, per-pool 4K split readiness (ABI v5). MUST be cleared on every
  // (re)initialization: a stale splitDone bit from an earlier generation would
  // otherwise make a peer skip split_2m_to_4k for a pool the new generation
  // rebuilt, so it would seal pages that were never split. Cleared, the first
  // post-reinit hit re-runs the split on each core.
  for (uint32_t i = 0; i < kEJitSharedPoolSlots; ++i) {
    st->poolSplits[i].poolBase.storeRelaxed(0);
    st->poolSplits[i].splitDoneMask.storeRelaxed(0);
    st->poolSplits[i].splitPreparingMask.storeRelaxed(0);
  }
  st->dump.lock.storeRelaxed(0);
  st->dump.filterEnabled.storeRelaxed(0);
  st->dump.filterLen = 0;
  st->dump.nextSlot = 0;
  st->dump.reserved0 = 0;
  for (uint32_t i = 0; i < kEJitSharedDumpNameBytes; ++i)
    st->dump.filterName[i] = 0;
  for (uint32_t s = 0; s < kEJitSharedDumpSlotCount; ++s) {
    EJitSharedDumpSlot &Slot = st->dump.slots[s];
    Slot.valid.storeRelaxed(0);
    Slot.truncated.storeRelaxed(0);
    Slot.nameLen = 0;
    Slot.irSize = 0;
    Slot.asmSize = 0;
    Slot.keyHi = 0;
    Slot.keyLo = 0;
    Slot.reserved0 = 0;
    if (canFreeDumpPayload && Slot.irPtr)
      std::free(reinterpret_cast<void *>(Slot.irPtr));
    if (canFreeDumpPayload && Slot.asmPtr)
      std::free(reinterpret_cast<void *>(Slot.asmPtr));
    Slot.irPtr = 0;
    Slot.asmPtr = 0;
    for (uint32_t i = 0; i < kEJitSharedDumpNameBytes; ++i)
      Slot.name[i] = 0;
  }
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b) {
    st->buckets[b].writeFlag.storeRelaxed(0);
    st->buckets[b].readers.storeRelaxed(0);
    st->buckets[b].publishSeq.storeRelaxed(0); // even => no publish in flight
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = st->buckets[b].slots[s];
      Slot.state.storeRelaxed(
          static_cast<uint32_t>(EJitSharedSlotState::Empty));
      Slot.funcIndex = 0;
      Slot.numDims = 0;
      Slot.generation = 0;
      for (uint32_t i = 0; i < 4; ++i) {
        Slot.dims[i] = EJitDimPair{0, 0};
        Slot.versions[i] = 0;
      }
      Slot.identityHash = 0;
      Slot.fnPtr.storeRelaxed(0);
      Slot.executableCoreMask.storeRelaxed(0);
      // Executable-range metadata (ABI v5): cleared so a stale range from an
      // earlier generation can never be read back after a re-init.
      Slot.codeStart = 0;
      Slot.codeSize = 0;
      Slot.poolBase = 0;
      Slot.poolSize = 0;
      Slot.poolId = 0;
      Slot.rangeReserved = 0;
    }
  }
}
} // namespace

EJitSharedTaskPool::InitResult EJitSharedTaskPool::init() {
  if (!state_) {
    EJIT_DIAG("shared taskpool init FAILED: no shared state bound");
    return InitResult::NoState;
  }
  EJIT_DIAG("shared taskpool init: state=%p fingerprint=0x%llx",
            static_cast<void *>(state_),
            static_cast<unsigned long long>(regFingerprint_));

  // Bounded retry so an in-progress peer never deadlocks us.
  constexpr uint32_t kMaxSpins = 1u << 20;
  for (uint32_t spin = 0; spin < kMaxSpins; ++spin) {
    uint32_t st = state_->initState.loadAcquire();
    switch (static_cast<EJitSharedInitState>(st)) {
    case EJitSharedInitState::Uninitialized: {
      state_->initAttempts.fetchAdd(1);
      uint32_t expected =
          static_cast<uint32_t>(EJitSharedInitState::Uninitialized);
      if (!state_->initState.compareExchange(
              expected,
              static_cast<uint32_t>(EJitSharedInitState::Initializing)))
        break; // lost the race; re-observe.

      // We are the owner. Build the whole blob, then publish Ready LAST.
      uint32_t self = EJitCoreId::current();
      uint32_t nextGen = state_->generation.loadRelaxed() + 1;
      initSharedStorage(state_, static_cast<uint32_t>(configuredMode_));
      state_->generation.storeRelease(nextGen);
      state_->ownerCoreId.storeRelease(self);
      state_->codeSharingEnabled.storeRelease(codeSharingEnabled_ ? 1u : 0u);
      state_->lastInitError.storeRelease(0);
      state_->workerTaskId.storeRelease(0);
      // Publish the owner's funcIndex/dimType registration digest so peers can
      // reject a divergent mapping before submitting any request (spec §11).
      state_->registrationFingerprint.storeRelease(regFingerprint_);
      state_->magic = kEJitSharedAbiMagic;
      state_->abiVersion = kEJitSharedAbiVersion;
      state_->structSize =
          static_cast<uint32_t>(sizeof(EJitSharedTaskPoolState));

      // Start the ONE worker (if a starter was injected). A failure here is a
      // clean init failure: record it, publish Failed, and DO NOT pretend JIT
      // is up.
      bool workerOk = true;
      if (workerStart_) {
        uint64_t taskId = 0;
        workerOk = workerStart_(
            workerCtx_, &EJitSharedTaskPool::workerEntryThunk, this, &taskId);
        if (workerOk)
          state_->workerTaskId.storeRelease(taskId);
      }
      if (!workerOk) {
        state_->lastInitError.storeRelease(
            static_cast<uint32_t>(EJitSharedInitError::WorkerStartFailed));
        state_->initState.storeRelease(
            static_cast<uint32_t>(EJitSharedInitState::Failed));
        EJIT_DIAG("shared taskpool owner=%u worker start FAILED", self);
        return InitResult::OwnerFailed;
      }
      isOwner_ = true;
      state_->initState.storeRelease(
          static_cast<uint32_t>(EJitSharedInitState::Ready)); // publish last
      EJIT_DIAG("shared taskpool owner=%u gen=%u ready", self, nextGen);
      return InitResult::BecameOwner;
    }
    case EJitSharedInitState::Initializing:
      // A peer racing the owner yields (not busy-spins) so a high-priority peer
      // never starves the owner core trying to finish init + publish Ready.
      if (workerIdle_)
        workerIdle_(workerIdleCtx_);
      else
        cpuRelax();
      break;
    case EJitSharedInitState::Ready:
      if (state_->magic != kEJitSharedAbiMagic ||
          state_->abiVersion != kEJitSharedAbiVersion ||
          state_->structSize != sizeof(EJitSharedTaskPoolState)) {
        EJIT_DIAG("shared taskpool attach REJECTED: ABI mismatch "
                  "(magic=0x%x ver=%u size=%u exp_size=%zu)",
                  state_->magic, state_->abiVersion, state_->structSize,
                  sizeof(EJitSharedTaskPoolState));
        return InitResult::AbiMismatch;
      }
      // Registration consistency: a peer whose funcIndex/dimType mapping digest
      // differs from the owner's must NOT submit requests against mismatched
      // indices. Clean-fail instead (the owner itself re-observes its own
      // fingerprint, so this never rejects the owner).
      if (state_->registrationFingerprint.loadAcquire() != regFingerprint_) {
        EJIT_DIAG("shared taskpool attach REJECTED: registration fingerprint "
                  "mismatch (owner=%llu self=%llu)",
                  static_cast<unsigned long long>(
                      state_->registrationFingerprint.loadAcquire()),
                  static_cast<unsigned long long>(regFingerprint_));
        return InitResult::FingerprintMismatch;
      }
      EJIT_DIAG("shared taskpool attached ready (owner=%u)",
                state_->ownerCoreId.loadAcquire());
      return InitResult::AttachedReady;
    case EJitSharedInitState::Failed:
      EJIT_DIAG("shared taskpool init FAILED: owner reported Failed (err=%u)",
                state_->lastInitError.loadAcquire());
      return InitResult::OwnerFailed;
    case EJitSharedInitState::Stopping:
      EJIT_DIAG("shared taskpool init FAILED: owner is Stopping");
      return InitResult::OwnerFailed;
    }
  }
  EJIT_DIAG("shared taskpool init FAILED: peer still initializing after spins");
  return InitResult::InitInProgress; // peer still initializing; pending, no
                                     // hang.
}

void EJitSharedTaskPool::ownerShutdown() {
  if (!state_ || !isOwner_)
    return;
  EJIT_DIAG("shared taskpool owner shutdown begin");
  // Signal the worker loop to exit, then join it BEFORE returning state to
  // Uninitialized so no worker can touch owner-private state afterwards.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Stopping));
  if (workerStop_)
    workerStop_(workerCtx_); // soft-stop + JOIN (no use-after-free).
  state_->ownerCoreId.storeRelease(kEJitInvalidCoreId);
  state_->workerTaskId.storeRelease(0);
  state_->generation.storeRelease(state_->generation.loadRelaxed() + 1);
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
  isOwner_ = false;
  EJIT_DIAG("shared taskpool owner shutdown complete");
}

//===----------------------------------------------------------------------===//
// Producer path (§5.2).
//===----------------------------------------------------------------------===//
__attribute__((always_inline)) EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::classifyHit(const SharedLookup &Hit) {
  CompileOrGetResult R;
  if (Hit.hasReadToken && Hit.fnPtr) {
    EJIT_STAT_INC(state_->counters.cacheHits);
    R.status = EJitCompileOrGetStatus::CacheHit;
    R.fnPtr = Hit.fnPtr;
    R.bucketIndex = Hit.bucketIndex;
    R.hasReadToken = true;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // Seqlock hit: same terminal CacheHit, but no read token was taken and the
  // bucketIndex is the out-of-range sentinel, so the wrapper's releaseRead()
  // cleanly no-ops. Safe because published code is never freed in this build.
  if (Hit.noTokenHit && Hit.fnPtr) {
    EJIT_STAT_INC(state_->counters.cacheHits);
    R.status = EJitCompileOrGetStatus::CacheHit;
    R.fnPtr = Hit.fnPtr;
    R.bucketIndex = Hit.bucketIndex; // == kEJitSharedCacheBuckets (sentinel)
    R.hasReadToken = false;
    R.fastPathTerminal = true;
    return R;
  }
#endif
  if (Hit.readyButNotShareable) {
    // The work is already done but this core may not read the cross-core
    // pointer; fall back cleanly WITHOUT re-enqueuing (avoids recompile churn).
    R.status = EJitCompileOrGetStatus::OffMode;
    R.readyButNotShareable = true;
    R.fastPathTerminal = true;
    return R;
  }
  // True miss (Ready, enabled, no shareable cached code): the caller must fall
  // through to the compileOrGet() slow path (Off / Sync / Async).
  R.fastPathTerminal = false;
  return R;
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit(uint32_t funcIndex, const EJitDimPair *dims,
                                uint32_t numDims) {
  CompileOrGetResult R;
  // Parameter check already done by the C API layer.

  // Ready check (§5.2 step 0): a not-yet-Ready pool is a clean fallback and
  // never reads the queue/cache.
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    EJIT_DIAG_VERBOSE("shared taskpool fallback func=%u: not Ready", funcIndex);
    R.status = EJitCompileOrGetStatus::OffMode; // not Ready → clean fallback.
    R.fastPathTerminal = true;
    return R;
  }

  // Instance-enabled check (§5.2 step 0): a disabled dim falls back and never
  // reaches the cache, so a deactivated instance is never served stale code.
  for (uint32_t i = 0; i < numDims; ++i)
    if (!isInstanceEnabled(dims[i].dimType, dims[i].instanceId)) {
      EJIT_STAT_INC_INSTANCE_DISABLED(state_);
      EJIT_DIAG_VERBOSE("shared taskpool disabled func=%u dim[%u]=(%u,%u)",
                        funcIndex, i, dims[i].dimType, dims[i].instanceId);
      R.status = EJitCompileOrGetStatus::InstanceDisabled;
      R.fastPathTerminal = true;
      return R;
    }

  // Cache lookup (§5.2 step 1) — runs BEFORE the Off check, so an already
  // compiled entry is still served even when the pool is globally Off.
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq(funcIndex, dims, numDims));
#else
  return classifyHit(cacheLookup(funcIndex, dims, numDims));
#endif
}

//===----------------------------------------------------------------------===//
// Fixed-dimension fast cache-hit entries (§5.2 steps 0-1, unrolled). Each
// shares the Ready gate + classifyHit() with tryCacheHit() but the
// instance-enabled check is unrolled and the dim identity is built directly on
// the stack, so the C ABI fixed-dimension entries reach the shared
// cacheLookup() without a numDims loop or variable-length array setup.
// Semantics are identical to tryCacheHit() with the matching numDims.
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit0D(uint32_t funcIndex) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq0D(funcIndex));
#else
  return classifyHit(cacheLookup0D(funcIndex));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit1D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq1D(funcIndex, dim0, inst0));
#else
  return classifyHit(cacheLookup1D(funcIndex, dim0, inst0));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit2D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1,
                                  uint32_t inst1) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0) || !isInstanceEnabled(dim1, inst1)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  return classifyHit(cacheLookupSeq2D(funcIndex, dim0, inst0, dim1, inst1));
#else
  return classifyHit(cacheLookup2D(funcIndex, dim0, inst0, dim1, inst1));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit3D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0) || !isInstanceEnabled(dim1, inst1) ||
      !isInstanceEnabled(dim2, inst2)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  const EJitDimPair d3[3] = {{dim0, inst0}, {dim1, inst1}, {dim2, inst2}};
  return classifyHit(cacheLookupSeq(funcIndex, d3, 3));
#else
  return classifyHit(
      cacheLookup3D(funcIndex, dim0, inst0, dim1, inst1, dim2, inst2));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::tryCacheHit4D(uint32_t funcIndex, uint32_t dim0,
                                  uint32_t inst0, uint32_t dim1, uint32_t inst1,
                                  uint32_t dim2, uint32_t inst2, uint32_t dim3,
                                  uint32_t inst3) {
  CompileOrGetResult R;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode;
    R.fastPathTerminal = true;
    return R;
  }
  if (!isInstanceEnabled(dim0, inst0) || !isInstanceEnabled(dim1, inst1) ||
      !isInstanceEnabled(dim2, inst2) || !isInstanceEnabled(dim3, inst3)) {
    EJIT_STAT_INC_INSTANCE_DISABLED(state_);
    R.status = EJitCompileOrGetStatus::InstanceDisabled;
    R.fastPathTerminal = true;
    return R;
  }
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  const EJitDimPair d4[4] = {
      {dim0, inst0}, {dim1, inst1}, {dim2, inst2}, {dim3, inst3}};
  return classifyHit(cacheLookupSeq(funcIndex, d4, 4));
#else
  return classifyHit(cacheLookup4D(funcIndex, dim0, inst0, dim1, inst1, dim2,
                                   inst2, dim3, inst3));
#endif
}

EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                 uint32_t numDims, void *fallback) {
  EJIT_DIAG_VERBOSE("shared taskpool request func=%u dims=%u fallback=%p",
                    funcIndex, numDims, fallback);
  // Parameter check already done by the C API layer.

  // Fast cache-hit path (§5.2 steps 0-1). On any terminal outcome (hit,
  // disabled instance, not-Ready, or ready-but-not-shareable) return directly.
  CompileOrGetResult R = tryCacheHit(funcIndex, dims, numDims);
  if (R.fastPathTerminal) {
    // Non-hit terminals surface the caller's fallback pointer (a hit already
    // carries the cached fnPtr + read token).
    if (R.status != EJitCompileOrGetStatus::CacheHit)
      R.fnPtr = fallback;
    return R;
  }
  // True miss: continue the slow path with the caller's fallback.
  R.fnPtr = fallback;
  // Off mode (§5.2 step 2).
  if (state_->mode.loadAcquire() ==
      static_cast<uint32_t>(EJitCompileMode::Off)) {
    EJIT_DIAG_VERBOSE("shared taskpool fallback func=%u: mode off", funcIndex);
    R.status = EJitCompileOrGetStatus::OffMode;
    return R;
  }
  // Sync mode: compile inline on the calling thread (no queue, no worker). Only
  // the owner core has the compile callback; non-owner cores fall back cleanly.
  if (state_->mode.loadAcquire() ==
      static_cast<uint32_t>(EJitCompileMode::Sync)) {
    if (!isOwner_ || !compileFn_) {
      EJIT_DIAG_VERBOSE("shared taskpool sync fallback func=%u: not owner (owner=%u fn=%p)",
                        funcIndex, static_cast<unsigned>(isOwner_),
                        reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(compileFn_)));
      R.status = EJitCompileOrGetStatus::OffMode;
      return R;
    }
    // Build request with current instance versions, then compile inline.
    EJitCompileRequest ReqLocal{};
    ReqLocal.funcIndex = funcIndex;
    ReqLocal.numDims = numDims;
    for (uint32_t i = 0; i < numDims; ++i) {
      ReqLocal.dims[i] = dims[i];
      ReqLocal.versions[i] =
          instanceVersion(dims[i].dimType, dims[i].instanceId);
    }
    void *fn = nullptr;
    bool ok = compileFn_(compileCtx_, ReqLocal, &fn);
    if (!ok || !fn) {
      EJIT_STAT_INC(state_->counters.compileFailed);
      EJIT_DIAG("shared taskpool sync compile failed func=%u ok=%u", funcIndex,
                static_cast<unsigned>(ok));
      R.status = EJitCompileOrGetStatus::CompileFailed;
      return R;
    }
    if (!versionsCurrent(ReqLocal)) {
      if (releaseFn_) releaseFn_(releaseCtx_, fn);
      EJIT_STAT_INC(state_->counters.compileFailed);
      EJIT_DIAG("shared taskpool sync compile drop func=%u: version changed",
                funcIndex);
      R.status = EJitCompileOrGetStatus::CompileFailed;
      return R;
    }
    EJitCompiledCodeInfo info;
    if (codeRangeFn_) codeRangeFn_(codeRangeCtx_, fn, &info);
    EJitPublishStatus PS =
        cachePublish(ReqLocal, fn, info.codeSize ? &info : nullptr);
    if (PS == EJitPublishStatus::Published) {
      EJIT_STAT_INC(state_->counters.asyncCompiles);
      publishCodePoolStats();
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
      SharedLookup Hit2 = cacheLookupSeq(funcIndex, dims, numDims);
      if ((Hit2.hasReadToken || Hit2.noTokenHit) && Hit2.fnPtr) {
        R.status = EJitCompileOrGetStatus::CacheHit;
        R.fnPtr = Hit2.fnPtr;
        R.bucketIndex = Hit2.bucketIndex; // sentinel -> releaseRead no-op
        R.hasReadToken = Hit2.hasReadToken;
        EJIT_DIAG_VERBOSE("shared taskpool sync compiled func=%u fn=%p", funcIndex,
                          Hit2.fnPtr);
        return R;
      }
#else
      SharedLookup Hit2 = cacheLookup(funcIndex, dims, numDims);
      if (Hit2.hasReadToken && Hit2.fnPtr) {
        R.status = EJitCompileOrGetStatus::CacheHit;
        R.fnPtr = Hit2.fnPtr;
        R.bucketIndex = Hit2.bucketIndex;
        R.hasReadToken = true;
        EJIT_DIAG_VERBOSE("shared taskpool sync compiled func=%u fn=%p", funcIndex,
                          Hit2.fnPtr);
        return R;
      }
#endif
    } else {
      if (releaseFn_) releaseFn_(releaseCtx_, fn);
    }
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared taskpool sync compile failed func=%u publish=%u", funcIndex,
              static_cast<unsigned>(PS));
    R.status = EJitCompileOrGetStatus::CompileFailed;
    return R;
  }
  // Dedup + enqueue (§5.2 step 3) — Async path.
  uint32_t gen = state_->generation.loadAcquire();
  EJitCompileRequest Req{};
  Req.funcIndex = funcIndex;
  Req.numDims = numDims;
  Req.fallbackPtr = reinterpret_cast<uintptr_t>(fallback);
  Req.generation = gen;
  for (uint32_t i = 0; i < numDims; ++i) {
    Req.dims[i] = dims[i];
    Req.versions[i] = instanceVersion(dims[i].dimType, dims[i].instanceId);
  }
  switch (dedupMark(funcIndex, gen)) {
  case EJitDedupResult::AlreadyPending:
    EJIT_STAT_INC(state_->counters.alreadyPending);
    EJIT_DIAG_VERBOSE("shared taskpool coalesced func=%u: already pending", funcIndex);
    R.status = EJitCompileOrGetStatus::AlreadyPending;
    return R;
  case EJitDedupResult::InvalidFuncIndex:
    EJIT_DIAG("shared taskpool reject func=%u: out of range", funcIndex);
    R.status = EJitCompileOrGetStatus::InvalidParam;
    return R;
  case EJitDedupResult::Claimed:
    break;
  }
  if (!queuePush(Req)) {
    dedupClear(funcIndex, gen); // queue full → roll back the in-flight slot.
    EJIT_STAT_INC(state_->counters.queueFull);
    EJIT_DIAG("shared taskpool fallback func=%u: queue full", funcIndex);
    R.status = EJitCompileOrGetStatus::QueueFullFallback;
    return R;
  }
  EJIT_STAT_INC(state_->counters.asyncEnqueues);
  EJIT_DIAG_VERBOSE("shared taskpool enqueued func=%u gen=%u", funcIndex, gen);
  R.status = EJitCompileOrGetStatus::EnqueuedPending;
  return R;
}

//===----------------------------------------------------------------------===//
// Consumer path (§5.3) — runs on the single owner worker (or a test driver).
//===----------------------------------------------------------------------===//
void EJitSharedTaskPool::publishCodePoolStats() {
  if (!state_ || !codePoolStatsFn_)
    return;
  EJitCodePoolStatsOut s{};
  if (!codePoolStatsFn_(codePoolStatsCtx_, &s))
    return;
  state_->codePoolStats.poolCount.storeRelaxed(s.poolCount);
  state_->codePoolStats.sealedCount.storeRelaxed(s.sealedCount);
  state_->codePoolStats.activeCount.storeRelaxed(s.activeCount);
  state_->codePoolStats.usedBytes.storeRelaxed(s.usedBytes);
  state_->codePoolStats.reservedBytes.storeRelaxed(s.reservedBytes);
  state_->codePoolStats.wastedBytes.storeRelaxed(s.wastedBytes);
  state_->codePoolStats.sealInvocations.storeRelaxed(s.sealInvocations);
  state_->codePoolStats.splitInvocations.storeRelaxed(s.splitInvocations);
  state_->codePoolStats.finalizedRangeCount.storeRelaxed(s.finalizedRangeCount);
}

bool EJitSharedTaskPool::readCodePoolStats(EJitCodePoolStatsOut *out) const {
  if (!state_ || !out)
    return false;
  out->poolCount = state_->codePoolStats.poolCount.loadRelaxed();
  out->sealedCount = state_->codePoolStats.sealedCount.loadRelaxed();
  out->activeCount = state_->codePoolStats.activeCount.loadRelaxed();
  out->usedBytes = state_->codePoolStats.usedBytes.loadRelaxed();
  out->reservedBytes = state_->codePoolStats.reservedBytes.loadRelaxed();
  out->wastedBytes = state_->codePoolStats.wastedBytes.loadRelaxed();
  out->sealInvocations = state_->codePoolStats.sealInvocations.loadRelaxed();
  out->splitInvocations = state_->codePoolStats.splitInvocations.loadRelaxed();
  out->finalizedRangeCount =
      state_->codePoolStats.finalizedRangeCount.loadRelaxed();
  return true;
}

void EJitSharedTaskPool::runCompile(const EJitCompileRequest &req) {
  EJIT_DIAG_VERBOSE("shared worker compile begin func=%u dims=%u gen=%u", req.funcIndex,
            req.numDims, req.generation);
  // Checkpoint 0 (spec §11): generation guard. A request enqueued under an
  // earlier generation (owner re-init in between) is dropped before compiling.
  // dedupClear is generation-aware, so this never clears a NEW generation's
  // in-flight slot for the same funcIndex.
  if (req.generation != state_->generation.loadAcquire()) {
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker compile drop func=%u: generation changed", req.funcIndex);
    return;
  }
  // Checkpoint 1: invalidated before compile started.
  if (!versionsCurrent(req)) {
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker compile drop func=%u: version changed before compile",
              req.funcIndex);
    return;
  }
  void *fn = nullptr;
  bool ok = compileFn_ && compileFn_(compileCtx_, req, &fn);
  if (!ok || !fn) {
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker compile failed func=%u ok=%u fn=%p", req.funcIndex,
              static_cast<unsigned>(ok), fn);
    return;
  }
  // Resolve the real executable range for the freshly compiled pointer (from
  // the owner's code-pool finalize metadata) so it can be published into the
  // cache slot for cross-core 4K sealing. Optional: if no provider is wired or
  // the pointer is not pool-backed, the slot carries no range and a 4K peer
  // cleanly falls back.
  EJitCompiledCodeInfo info{};
  if (codeRangeFn_)
    (void)codeRangeFn_(codeRangeCtx_, fn, &info);
  // Checkpoint 2: a generation bump OR a toggle during compilation invalidates
  // the result.
  if (req.generation != state_->generation.loadAcquire() ||
      !versionsCurrent(req)) {
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker compile drop func=%u: version/gen changed after compile",
              req.funcIndex);
    return;
  }
  EJitPublishStatus PS = cachePublish(req, fn, &info);
  switch (PS) {
  case EJitPublishStatus::Published:
    EJIT_STAT_INC(state_->counters.asyncCompiles);
    publishCodePoolStats();
    dedupClear(req.funcIndex, req.generation);
    EJIT_DIAG_VERBOSE("shared worker publish ok func=%u fn=%p", req.funcIndex, fn);
    return;
  case EJitPublishStatus::VersionMismatch:
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.compileFailed);
    EJIT_DIAG("shared worker publish drop func=%u: version mismatch", req.funcIndex);
    return;
  case EJitPublishStatus::InvalidParam:
  case EJitPublishStatus::Failed:
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    dedupClear(req.funcIndex, req.generation);
    EJIT_STAT_INC(state_->counters.publishFailed);
    EJIT_DIAG("shared worker publish failed func=%u status=%u", req.funcIndex,
              static_cast<unsigned>(PS));
    return;
  }
}

bool EJitSharedTaskPool::pollOne() {
  if (!state_)
    return false;
  EJitCompileRequest Req{};
  if (!queuePop(Req))
    return false;
  runCompile(Req);
  return true;
}

unsigned EJitSharedTaskPool::pollBudget(unsigned maxItems) {
  unsigned n = 0;
  while (n < maxItems && pollOne())
    ++n;
  return n;
}

EJitWorkerStep EJitSharedTaskPool::workerPollOnce() {
  if (!state_)
    return EJitWorkerStep::Exit;
  uint32_t st = state_->initState.loadAcquire();
  switch (static_cast<EJitSharedInitState>(st)) {
  case EJitSharedInitState::Ready:
    workerConsumeLoops_.fetchAdd(1);
    return pollOne() ? EJitWorkerStep::Consumed : EJitWorkerStep::Idle;
  case EJitSharedInitState::Initializing:
    // The owner is still arming the pool. The SRE task may have been scheduled
    // before the owner published Ready; WAIT for Ready/Failed — never exit
    // early and never read the half-armed queue/cache (spec §11).
    workerWaitedForReady_.storeRelease(1);
    return EJitWorkerStep::WaitForReady;
  case EJitSharedInitState::Uninitialized:
  case EJitSharedInitState::Failed:
  case EJitSharedInitState::Stopping:
  default:
    EJIT_DIAG_DEBUG("shared worker exit: state=%u", st);
    return EJitWorkerStep::Exit;
  }
}

void EJitSharedTaskPool::runWorkerLoop() {
  EJIT_DIAG_VERBOSE("shared worker loop enter");
  // Loop until a terminal state. The worker is a PRODUCTION-lifetime task: it
  // never exits just because the owner is slightly slow to publish Ready (no
  // spin budget). On every non-consuming iteration — waiting through
  // Initializing, or Ready with an empty queue — it YIELDS the CPU via the
  // injected idle hook (platform EJitSreTask::yield), so a high-priority worker
  // cannot starve the core that must publish Ready or enqueue work. The idle
  // hook runs OUTSIDE any bucket lock / queue slot / dedup critical state
  // (pollOne returns before we idle).
  uint32_t consumedSinceThrottle = 0;
  for (;;) {
    EJitWorkerStep s = workerPollOnce();
    if (s == EJitWorkerStep::Exit)
      break;
    if (s == EJitWorkerStep::WaitForReady || s == EJitWorkerStep::Idle)
      workerIdle();
    else if (EJIT_SRE_TASKPOOL_WORKER_THROTTLE_ITEMS != 0u &&
             EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS != 0u &&
             ++consumedSinceThrottle >=
                 EJIT_SRE_TASKPOOL_WORKER_THROTTLE_ITEMS) {
      consumedSinceThrottle = 0;
      for (uint32_t i = 0; i < EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS; ++i)
        workerIdle();
    }
  }
  EJIT_DIAG_VERBOSE("shared worker loop leave");
}

void EJitSharedTaskPool::workerIdle() {
  workerIdleYields_.fetchAdd(1);
  if (workerIdle_)
    workerIdle_(workerIdleCtx_); // platform yield (SRE_TaskDelay / std::yield)
  else
    cpuRelax(); // step/unit tests with no injected hook
}

void EJitSharedTaskPool::workerEntryThunk(void *ctx) {
  static_cast<EJitSharedTaskPool *>(ctx)->runWorkerLoop();
}

//===----------------------------------------------------------------------===//
// Diagnostics (§11 observability).
//===----------------------------------------------------------------------===//
uint32_t EJitSharedTaskPool::sharedInitState() const {
  return state_ ? state_->initState.loadAcquire()
                : static_cast<uint32_t>(EJitSharedInitState::Uninitialized);
}

uint32_t EJitSharedTaskPool::pendingCount() const {
  if (!state_)
    return 0;
  uint32_t pending = 0;
  for (uint32_t i = 0; i < kEJitSharedMaxFuncIndex; ++i)
    if (state_->inFlight[i].loadRelaxed() != 0)
      ++pending;
  return pending;
}

void EJitSharedTaskPool::getDiagnostics(EJitSharedDiagnostics &out) const {
  out = EJitSharedDiagnostics{};
  if (!state_)
    return;
  out.initState = state_->initState.loadAcquire();
  out.ownerCoreId = state_->ownerCoreId.loadAcquire();
  out.generation = state_->generation.loadAcquire();
  out.lastInitError = state_->lastInitError.loadAcquire();
  out.initAttempts = state_->initAttempts.loadAcquire();
  out.codeSharingEnabled = state_->codeSharingEnabled.loadAcquire();
  out.workerTaskId = state_->workerTaskId.loadAcquire();
  out.registrationFingerprint = state_->registrationFingerprint.loadAcquire();
  out.queueDepth =
      state_->enqueuePos.loadRelaxed() - state_->dequeuePos.loadRelaxed();
  uint32_t pending = 0;
  for (uint32_t i = 0; i < kEJitSharedMaxFuncIndex; ++i)
    if (state_->inFlight[i].loadRelaxed() != 0)
      ++pending;
  out.pendingCount = pending;
  uint32_t ready = 0;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s)
      if (state_->buckets[b].slots[s].state.loadAcquire() ==
          static_cast<uint32_t>(EJitSharedSlotState::Ready))
        ++ready;
  out.cacheReadyCount = ready;
  out.cacheHits = state_->counters.cacheHits.loadRelaxed();
  out.asyncEnqueues = state_->counters.asyncEnqueues.loadRelaxed();
  out.asyncCompiles = state_->counters.asyncCompiles.loadRelaxed();
  out.alreadyPending = state_->counters.alreadyPending.loadRelaxed();
  out.queueFull = state_->counters.queueFull.loadRelaxed();
  out.compileFailed = state_->counters.compileFailed.loadRelaxed();
  out.publishFailed = state_->counters.publishFailed.loadRelaxed();
  out.instanceDisabled = state_->counters.instanceDisabled.loadRelaxed();
  out.instanceDisabledPreActivate =
      state_->counters.instanceDisabledPreActivate.loadRelaxed();
  out.executePrepareFailed =
      state_->counters.executePrepareFailed.loadRelaxed();
}
