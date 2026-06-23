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
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"

using namespace llvm;
using namespace llvm::ejit;

namespace {

// Compiler reordering barrier used as a portable idle relax (no platform
// symbol, no arch-specific instruction in this layer).
inline void cpuRelax() { __asm__ __volatile__("" ::: "memory"); }

//===----------------------------------------------------------------------===//
// Inline per-bucket reader/writer lock over the two POD words (same protocol as
// EJitRwLock §3.2, but operating on shared-blob fields directly).
//===----------------------------------------------------------------------===//
bool bucketTryRead(EJitSharedCacheBucket &b) {
  if (b.writeFlag.loadAcquire() != 0)
    return false;
  b.readers.fetchAdd(1);
  if (b.writeFlag.loadAcquire() != 0) {
    b.readers.fetchSub(1);
    return false;
  }
  return true;
}
void bucketReadRelease(EJitSharedCacheBucket &b) { b.readers.fetchSub(1); }
void bucketWrite(EJitSharedCacheBucket &b) {
  uint32_t expected = 0;
  while (!b.writeFlag.compareExchange(expected, 1))
    expected = 0;
  while (b.readers.loadAcquire() != 0)
    cpuRelax();
}
void bucketWriteRelease(EJitSharedCacheBucket &b) {
  b.writeFlag.storeRelease(0);
}

constexpr uint32_t kReady = static_cast<uint32_t>(EJitSharedInitState::Ready);

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

uint32_t EJitSharedTaskPool::instanceVersion(uint32_t dimType,
                                             uint32_t instanceId) const {
  if (dimType >= kEJitSharedDimTypes || instanceId >= kEJitSharedInstances)
    return 0;
  return state_->version[dimType][instanceId].loadAcquire();
}

bool EJitSharedTaskPool::setInstanceEnabled(uint32_t dimType,
                                            uint32_t instanceId, bool enabled) {
  if (!state_ || dimType >= kEJitSharedDimTypes ||
      instanceId >= kEJitSharedInstances)
    return false;
  uint8_t expected = enabled ? 0 : 1;
  uint8_t desired = enabled ? 1 : 0;
  if (state_->enabled[dimType][instanceId].compareExchange(expected, desired)) {
    state_->version[dimType][instanceId].fetchAdd(1);
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
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.generation != curGen) // stale across an owner re-init: ignore.
      continue;
    if (Slot.identityHash != key)
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

    // fnPtr cross-core gate (§11 prerequisites): a non-owner core may read the
    // pointer only when code sharing is platform-validated (same VA, sealed,
    // I/D-cache coherent). Otherwise CLEAN-REJECT — never hand back a pointer
    // this core may not legally execute.
    uint32_t self = EJitCoreId::current();
    uint32_t owner = state_->ownerCoreId.loadAcquire();
    bool mayReadPtr =
        (state_->codeSharingEnabled.loadAcquire() != 0) || (self == owner);
    if (!mayReadPtr) {
      bucketReadRelease(B);
      R.readyButNotShareable = true;
      return R;
    }
    void *fn = reinterpret_cast<void *>(Slot.fnPtr.loadAcquire());
    if (!fn)
      break;

    // The owner already has execute permission (it sealed the code before
    // publishing). Return directly while holding the read token.
    if (self == owner) {
      R.fnPtr = fn;
      R.bucketIndex = bucket;
      R.hasReadToken = true;
      return R;
    }

    // Non-owner: execute permission is a per-core property on the target. If
    // this core has already prepared THIS slot's code (memoized bit), return
    // immediately under the read lock — no platform call.
    const bool CanMemoize = self < kEJitSharedMaxMemoCores;
    const uint64_t CoreBit = CanMemoize ? (uint64_t{1} << self) : uint64_t{0};
    if (CanMemoize && (Slot.executableCoreMask.loadAcquire() & CoreBit) != 0) {
      R.fnPtr = fn;
      R.bucketIndex = bucket;
      R.hasReadToken = true;
      return R;
    }

    // Not prepared yet. Snapshot the identity + the REAL code range, then DROP
    // the bucket read lock: the per-core platform split/enable_ex calls
    // (potentially slow) must never run while holding a cross-core bucket lock,
    // or a concurrent owner publish (which spins for readers==0) would stall.
    PeerCodeRange Snap;
    Snap.fn = fn;
    Snap.slotIndex = s;
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
      state_->counters.executePrepareFailed.fetchAdd(1);
      R.readyButNotShareable = true;
      return R; // clean fallback: no token, no shared pointer handed back.
    }

    // Re-acquire the read lock and RE-VALIDATE the slot: a publish may have
    // overwritten it (new code/generation) while we prepared. Only a slot that
    // is still Ready, same generation + identity + fnPtr, with current
    // versions, may hand back the prepared pointer; otherwise clean fallback so
    // a replaced/stale pointer is never returned (spec §11 / §8).
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
    if (CanMemoize)
      S2.executableCoreMask.fetchOr(CoreBit);
    R.fnPtr = Snap.fn;
    R.bucketIndex = Snap.bucket;
    R.hasReadToken = true;
    return R; // token held (re-acquired); caller releases after using fnPtr.
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
  // Cores beyond the 64-bit memo width cannot record split state, so they must
  // re-run the (idempotent) split on every hit rather than risk skipping it.
  if (self >= kEJitSharedMaxMemoCores)
    return splitPoolFn_ && splitPoolFn_(splitPoolCtx_, poolBase, poolSize);

  EJitSharedPoolSplit *P = findOrClaimPoolSlot(poolBase);
  if (!P)
    return false; // table full -> clean fallback.

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
    return (P->splitDoneMask.loadAcquire() & Bit) != 0;
  }

  // We own the split for this (pool, core). Run it, publishing done only on
  // success; on failure roll back preparing so a later hit can retry.
  bool ok = splitPoolFn_ && splitPoolFn_(splitPoolCtx_, poolBase, poolSize);
  if (ok) {
    P->splitDoneMask.fetchOr(Bit);
    P->splitPreparingMask.fetchAnd(~Bit);
    return true;
  }
  P->splitPreparingMask.fetchAnd(~Bit);
  return false;
}

bool EJitSharedTaskPool::prepareExecForCurrentCore(const PeerCodeRange &R,
                                                   uint32_t self) {
  if (!fourKSeal_) {
    // Legacy whole-2MiB-pool seal: align fnPtr to its pool base and enable_ex
    // that page. Range metadata is not required (spec §6 — 2M unchanged). When
    // no prepare callback is wired the platform/host needs no per-core
    // preparation (e.g. the single shared address space of the host
    // simulation), so the pointer is already executable on this core.
    if (!prepareCodeFn_)
      return true;
    return prepareCodeFn_(prepareCodeCtx_, R.fn);
  }

  // 4K page seal needs the real executable extent. A slot with no recorded
  // range (or a malformed one) is a clean fallback, never a guessed seal.
  if (R.codeStart == 0 || R.codeSize == 0 || R.poolBase == 0 || R.poolSize == 0)
    return false;
  if (R.codeStart + R.codeSize < R.codeStart) // code range overflow
    return false;
  if (R.poolBase + R.poolSize < R.poolBase) // pool range overflow
    return false;
  if (R.codeStart < R.poolBase ||
      R.codeStart + R.codeSize > R.poolBase + R.poolSize)
    return false; // code must lie wholly inside its pool.

  // This core must have split the 2MiB pool exactly once before sealing pages.
  if (!ensurePoolSplitForCurrentCore(self, R.poolBase, R.poolSize))
    return false;

  // Seal every 4KiB page the code overlaps: page-align start down, end up.
  const uintptr_t Page = static_cast<uintptr_t>(kEJitSharedSealPage);
  uintptr_t PageStart = R.codeStart & ~(Page - 1);
  uintptr_t PageEnd =
      (R.codeStart + static_cast<uintptr_t>(R.codeSize) + Page - 1) &
      ~(Page - 1);
  for (uintptr_t VA = PageStart; VA < PageEnd; VA += Page)
    if (!sealPageFn_ || !sealPageFn_(sealPageCtx_, VA))
      return false; // any page failure -> no callable pointer is returned.
  return true;
}

EJitPublishStatus
EJitSharedTaskPool::cachePublish(const EJitCompileRequest &req, void *fnPtr,
                                 const EJitCompiledCodeInfo *info) {
  if (!fnPtr || req.numDims > 4)
    return EJitPublishStatus::InvalidParam;
  uint64_t key = hashIdentity(req.funcIndex, req.dims, req.numDims);
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
        slotIdentityMatches(Slot, req.funcIndex, req.dims, req.numDims)) {
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
  target->funcIndex = req.funcIndex;
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
  if (!state_ || bucketIndex >= kEJitSharedCacheBuckets)
    return;
  bucketReadRelease(state_->buckets[bucketIndex]);
}

//===----------------------------------------------------------------------===//
// Owner election + init (§11).
//===----------------------------------------------------------------------===//
namespace {
// Field-by-field init so it is correct on raw, uninitialized shared memory
// (never relies on a C++ constructor having run on the blob).
void initSharedStorage(EJitSharedTaskPoolState *st, uint32_t mode) {
  for (uint32_t d = 0; d < kEJitSharedDimTypes; ++d)
    for (uint32_t i = 0; i < kEJitSharedInstances; ++i) {
      st->enabled[d][i].storeRelaxed(1);
      st->version[d][i].storeRelaxed(0);
    }
  st->mode.storeRelaxed(mode);
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
  st->counters.executePrepareFailed.storeRelaxed(0);
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
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b) {
    st->buckets[b].writeFlag.storeRelaxed(0);
    st->buckets[b].readers.storeRelaxed(0);
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
  if (!state_)
    return InitResult::NoState;

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
          state_->structSize != sizeof(EJitSharedTaskPoolState))
        return InitResult::AbiMismatch;
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
      return InitResult::AttachedReady;
    case EJitSharedInitState::Failed:
      return InitResult::OwnerFailed;
    case EJitSharedInitState::Stopping:
      return InitResult::OwnerFailed;
    }
  }
  return InitResult::InitInProgress; // peer still initializing; pending, no
                                     // hang.
}

void EJitSharedTaskPool::ownerShutdown() {
  if (!state_ || !isOwner_)
    return;
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
}

//===----------------------------------------------------------------------===//
// Producer path (§5.2).
//===----------------------------------------------------------------------===//
EJitSharedTaskPool::CompileOrGetResult
EJitSharedTaskPool::compileOrGet(uint32_t funcIndex, const EJitDimPair *dims,
                                 uint32_t numDims, void *fallback) {
  CompileOrGetResult R;
  R.fnPtr = fallback;
  if (!state_ || state_->initState.loadAcquire() != kReady) {
    R.status = EJitCompileOrGetStatus::OffMode; // not Ready → clean fallback.
    return R;
  }
  if ((numDims > 0 && !dims) || numDims > 4) {
    R.status = EJitCompileOrGetStatus::InvalidParam;
    return R;
  }
  // Instance-enabled check (§5.2 step 0).
  for (uint32_t i = 0; i < numDims; ++i)
    if (!isInstanceEnabled(dims[i].dimType, dims[i].instanceId)) {
      state_->counters.instanceDisabled.fetchAdd(1);
      R.status = EJitCompileOrGetStatus::InstanceDisabled;
      return R;
    }
  // Cache lookup (§5.2 step 1).
  EJitSharedTaskPool::SharedLookup Hit = cacheLookup(funcIndex, dims, numDims);
  if (Hit.hasReadToken && Hit.fnPtr) {
    state_->counters.cacheHits.fetchAdd(1);
    R.status = EJitCompileOrGetStatus::CacheHit;
    R.fnPtr = Hit.fnPtr;
    R.bucketIndex = Hit.bucketIndex;
    R.hasReadToken = true;
    return R;
  }
  if (Hit.readyButNotShareable) {
    // The work is already done but this core may not read the cross-core
    // pointer; fall back cleanly WITHOUT re-enqueuing (avoids recompile churn).
    R.status = EJitCompileOrGetStatus::OffMode;
    R.readyButNotShareable = true;
    return R;
  }
  // Off mode (§5.2 step 2).
  if (state_->mode.loadAcquire() ==
      static_cast<uint32_t>(EJitCompileMode::Off)) {
    R.status = EJitCompileOrGetStatus::OffMode;
    return R;
  }
  // Dedup + enqueue (§5.2 step 3).
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
    state_->counters.alreadyPending.fetchAdd(1);
    R.status = EJitCompileOrGetStatus::AlreadyPending;
    return R;
  case EJitDedupResult::InvalidFuncIndex:
    R.status = EJitCompileOrGetStatus::InvalidParam;
    return R;
  case EJitDedupResult::Claimed:
    break;
  }
  if (!queuePush(Req)) {
    dedupClear(funcIndex, gen); // queue full → roll back the in-flight slot.
    state_->counters.queueFull.fetchAdd(1);
    R.status = EJitCompileOrGetStatus::QueueFullFallback;
    return R;
  }
  state_->counters.asyncEnqueues.fetchAdd(1);
  R.status = EJitCompileOrGetStatus::EnqueuedPending;
  return R;
}

//===----------------------------------------------------------------------===//
// Consumer path (§5.3) — runs on the single owner worker (or a test driver).
//===----------------------------------------------------------------------===//
void EJitSharedTaskPool::runCompile(const EJitCompileRequest &req) {
  // Checkpoint 0 (spec §11): generation guard. A request enqueued under an
  // earlier generation (owner re-init in between) is dropped before compiling.
  // dedupClear is generation-aware, so this never clears a NEW generation's
  // in-flight slot for the same funcIndex.
  if (req.generation != state_->generation.loadAcquire()) {
    dedupClear(req.funcIndex, req.generation);
    state_->counters.compileFailed.fetchAdd(1);
    return;
  }
  // Checkpoint 1: invalidated before compile started.
  if (!versionsCurrent(req)) {
    dedupClear(req.funcIndex, req.generation);
    state_->counters.compileFailed.fetchAdd(1);
    return;
  }
  void *fn = nullptr;
  bool ok = compileFn_ && compileFn_(compileCtx_, req, &fn);
  if (!ok || !fn) {
    dedupClear(req.funcIndex, req.generation);
    state_->counters.compileFailed.fetchAdd(1);
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
    state_->counters.compileFailed.fetchAdd(1);
    return;
  }
  EJitPublishStatus PS = cachePublish(req, fn, &info);
  switch (PS) {
  case EJitPublishStatus::Published:
    state_->counters.asyncCompiles.fetchAdd(1);
    dedupClear(req.funcIndex, req.generation);
    return;
  case EJitPublishStatus::VersionMismatch:
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    dedupClear(req.funcIndex, req.generation);
    state_->counters.compileFailed.fetchAdd(1);
    return;
  case EJitPublishStatus::InvalidParam:
  case EJitPublishStatus::Failed:
    if (releaseFn_)
      releaseFn_(releaseCtx_, fn);
    dedupClear(req.funcIndex, req.generation);
    state_->counters.publishFailed.fetchAdd(1);
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
    return EJitWorkerStep::Exit;
  }
}

void EJitSharedTaskPool::runWorkerLoop() {
  // Loop until a terminal state. The worker is a PRODUCTION-lifetime task: it
  // never exits just because the owner is slightly slow to publish Ready (no
  // spin budget). On every non-consuming iteration — waiting through
  // Initializing, or Ready with an empty queue — it YIELDS the CPU via the
  // injected idle hook (platform EJitSreTask::yield), so a high-priority worker
  // cannot starve the core that must publish Ready or enqueue work. The idle
  // hook runs OUTSIDE any bucket lock / queue slot / dedup critical state
  // (pollOne returns before we idle).
  for (;;) {
    EJitWorkerStep s = workerPollOnce();
    if (s == EJitWorkerStep::Exit)
      return;
    if (s == EJitWorkerStep::WaitForReady || s == EJitWorkerStep::Idle)
      workerIdle();
    // Consumed: loop immediately, more work is likely queued.
  }
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
  out.executePrepareFailed =
      state_->counters.executePrepareFailed.loadRelaxed();
}
