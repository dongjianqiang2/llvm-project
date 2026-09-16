//===-- EJitObservedT1DispatchTest.cpp - observed T1 dispatch tests -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Production-path regressions for the experimental observed Tier-1 dispatch
// contract (shared-taskpool ABI v21, with the v22 leaf bucket observation lock)
// and its ProfileBundle mapping:
//
//   * only a real granted Tier-1 pointer return consumes one dispatch;
//   * rejected lookups (nonshareable pointer, null pointer, failed peer
//     preparation) and abandoned NO_RECLAIM seqlock retries consume none;
//   * the last allowed dispatch freezes count/quotaEnd exactly once, and later
//     calls fall back without rewriting the frozen boundary;
//   * a cold non-owner peer preparation carries the exact validated publish
//     identity, so the final real dispatch itself claims its Tier-2 request;
//   * a concurrent cancel + republish of the same slot address cannot absorb a
//     count or receive the predecessor's frozen timestamp (both builds);
//   * the admission commit uses a separate leaf bucket observation lock (ABI
//     v22) in NO_RECLAIM, so a granted dispatch never sets writeFlag or bumps
//     publishSeq and cannot invalidate a concurrent load-only lookup;
//   * a failed legacy (request-attempts OFF) cold-peer preparation queues no
//     Tier-2 for the zeroed default bucket0/slot0 coordinates (R2R-01);
//   * queue-full and delayed Tier-2 retries carry the frozen observation
//     unchanged across other compile work;
//   * the bundle mapping accepts metadata only for the exact attempt and
//     generation, keeps quotaEnd distinct from freezeCompletedAt, and reports
//     unavailable/approximate data honestly;
//   * the granted-T1 -> real queued Tier-2 request -> bundle join runs with the
//     production identity capture and bundle helper on real pool objects.
//
// The pool tests drive the real EJitSharedTaskPool with the same mock
// compiler/queue boundary as EJitSharedTaskPoolTest.cpp. The bundle tests call
// the production applyT1DispatchObservation() with real EJitCompileRequest and
// EJitProfileBundle objects; the ORC-engine part of the driver call site
// (EJitCompileDriver::compileCold) is compile-reviewed but not executed here
// (it needs the full engine), while the engine-independent join it performs is
// executed end-to-end by GrantedTier1ToQueuedTier2RequestReachesBundleJoin.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#include "gtest/gtest.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

using namespace llvm::ejit;

namespace {

// A deterministic, non-null "compiled code" address derived from funcIndex.
// The tests never execute it; they only compare/cache it.
void *codeFor(uint32_t funcIndex) {
  return reinterpret_cast<void *>(0x100000ull +
                                  static_cast<uintptr_t>(funcIndex) * 64u);
}

bool mockCompile(void * /*ctx*/, const EJitCompileRequest &req, void **outFn) {
  *outFn = codeFor(stripReqTier(req.funcIndex));
  return true;
}

/// Records every request the worker compiles, so a test can inspect the frozen
/// observation a Tier-2 request carries at execution time.
struct RecordedCompile {
  std::vector<EJitCompileRequest> requests;
  const EJitCompileRequest *find(uint32_t tier, uint32_t funcIndex) const {
    for (const EJitCompileRequest &R : requests)
      if (decodeReqTier(R.funcIndex) == tier &&
          stripReqTier(R.funcIndex) == funcIndex)
        return &R;
    return nullptr;
  }
};
bool mockRecordCompile(void *ctx, const EJitCompileRequest &req, void **outFn) {
  static_cast<RecordedCompile *>(ctx)->requests.push_back(req);
  *outFn = codeFor(stripReqTier(req.funcIndex));
  return true;
}

/// Injectable deterministic clock for the observed dispatch boundary. The
/// production clock is a thunk over the runtime trace clock; a test counts
/// calls so "frozen exactly once, never re-timestamped" is assertable.
struct TraceClockLog {
  uint32_t calls = 0;
  uint64_t next = 1000;
  uint64_t step = 10;
};
uint64_t mockTraceClock(void *ctx) {
  auto *Log = static_cast<TraceClockLog *>(ctx);
  ++Log->calls;
  const uint64_t Value = Log->next;
  Log->next += Log->step;
  return Value;
}

/// 4K-seal mocks (split + per-page seal) for the cold peer-preparation path.
struct PrepLog {
  std::vector<std::pair<uintptr_t, uint32_t>> splits;
  std::vector<std::pair<uintptr_t, uint32_t>> seals;
  bool splitOk = true;
  bool sealOk = true;
};
bool mockSplitPool(void *ctx, uintptr_t poolBase, uint64_t /*poolSize*/) {
  auto *Log = static_cast<PrepLog *>(ctx);
  Log->splits.push_back({poolBase, EJitCoreId::current()});
  return Log->splitOk;
}
bool mockSealPage(void *ctx, uintptr_t pageVA) {
  auto *Log = static_cast<PrepLog *>(ctx);
  Log->seals.push_back({pageVA, EJitCoreId::current()});
  return Log->sealOk;
}

struct PeerRangeCtx {
  uintptr_t poolBase = 0x40000000ull;
  uint64_t poolSize = 0x200000ull;
  uintptr_t codeStart = 0x40000100ull;
  uint64_t codeSize = 64;
  uint32_t poolId = 0;
  EJitCodePoolKind poolKind = EJitCodePoolKind::Near;
  // No runtime-writable ranges: the executable seal path is the one under test.
  uint32_t writableCount = 0;
  uint32_t requiresPeerEnableRw = 0;
};
bool mockCodeRange(void *ctx, const void *fnPtr, EJitCompiledCodeInfo *out) {
  auto *R = static_cast<PeerRangeCtx *>(ctx);
  out->fnPtr = const_cast<void *>(fnPtr);
  out->codeStart = R->codeStart;
  out->codeSize = R->codeSize;
  out->poolBase = R->poolBase;
  out->poolSize = R->poolSize;
  out->poolId = R->poolId;
  out->poolKind = R->poolKind;
  out->writableCount = R->writableCount;
  out->requiresPeerEnableRw = R->requiresPeerEnableRw;
  for (uint32_t I = 0; I < kEJitSharedMaxWritableRanges; ++I) {
    out->writableRanges[I].addr = 0;
    out->writableRanges[I].size = 0;
  }
  return true;
}

/// Force exactly one NO_RECLAIM seqlock retry: bump the matching bucket's
/// publishSeq by 2 (even -> even, so the next attempt can start) once, after a
/// matched resolve and before the outer stability check.
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
struct SeqRetryHookLog {
  EJitSharedTaskPoolState *state = nullptr;
  uint32_t bucket = 0;
  uint32_t fires = 0;
  uint32_t bumps = 0;
};
void mockSeqRetryHook(void *ctx, uint32_t bucketIndex) {
  auto *Log = static_cast<SeqRetryHookLog *>(ctx);
  ++Log->fires;
  if (Log->bumps == 0 && bucketIndex == Log->bucket) {
    Log->state->buckets[bucketIndex].publishSeq.fetchAdd(2);
    ++Log->bumps;
  }
}
#endif

/// Blocking dispatch clock that holds the FINAL admission inside its commit
/// window (after the count CAS, before/at the quotaEnd freeze) while a helper
/// core really cancels the attempt and republishes the same slot address. The
/// commit exclusion of both builds must keep the replacement out until the
/// commit completes, so the predecessor never stamps the replacement's slot and
/// the replacement never absorbs the predecessor's count.
struct ReplacementGate {
  std::atomic<uint32_t> clockCalls{0};
  std::atomic<bool> go{false};
  std::atomic<bool> helperStarted{false};
  std::atomic<bool> helperPublished{false};
  std::atomic<bool> publishedInsideCommit{false};
  std::atomic<uint64_t> newToken{0};
  uint64_t ts1 = 1000;
  uint64_t ts2 = 2000;
};

uint64_t replacementGateClock(void *ctx) {
  auto *G = static_cast<ReplacementGate *>(ctx);
  if (G->clockCalls.fetch_add(1) != 0)
    return G->ts2;
  G->go.store(true, std::memory_order_release);
  while (!G->helperStarted.load(std::memory_order_acquire))
    std::this_thread::yield();
  // The helper now cancels + republishes the slot. Give it a bounded window to
  // prove the exclusion: with the commit identity held, it cannot publish from
  // inside this window in either build.
  for (uint32_t I = 0;
       I < 100000 && !G->helperPublished.load(std::memory_order_acquire); ++I)
    std::this_thread::yield();
  G->publishedInsideCommit.store(
      G->helperPublished.load(std::memory_order_acquire),
      std::memory_order_release);
  return G->ts1;
}

/// Blocking dispatch clock that lets a peer lookup run while the final CAS has
/// already closed the count and the quotaEnd freeze is still pending. In the
/// token build the peer may claim the Tier-2 request with an unknown (0)
/// timestamp: honest, never a fabricated or previous-attempt time. The
/// NO_RECLAIM build holds the leaf observationLock across the freeze, so the
/// peer's closed-quota claim cannot complete inside the window; the finalizer's
/// own request carries the frozen value. The wait is deliberately bounded: the
/// gate must hold the window open for the peer to enter the claim path, not
/// wait for a claim that the exclusion itself is blocking.
struct FreezeGate {
  std::atomic<uint32_t> clockCalls{0};
  std::atomic<bool> go{false};
  std::atomic<bool> peerDone{false};
  std::atomic<uint32_t> peerStatus{0};
  void *peerFn = nullptr;
  uint64_t ts = 1000;
};

uint64_t freezeGateClock(void *ctx) {
  auto *G = static_cast<FreezeGate *>(ctx);
  if (G->clockCalls.fetch_add(1) != 0)
    return G->ts;
  G->go.store(true, std::memory_order_release);
  for (uint32_t I = 0;
       I < 200000 && !G->peerDone.load(std::memory_order_acquire); ++I)
    std::this_thread::yield();
  return G->ts;
}

class ObservedT1Test : public ::testing::Test {
protected:
  void SetUp() override {
    EJitCoreId::resetForTest();
    state_ = std::make_unique<EJitSharedTaskPoolState>();
  }
  void TearDown() override { EJitCoreId::resetForTest(); }

  /// Owner core 0 with online PGO, the experimental request-attempt protocol,
  /// and a deterministic dispatch clock: the observed-dispatch contract.
  void bringUpObserved(EJitSharedTaskPool &pool, TraceClockLog &clock,
                       uint32_t threshold,
                       EJitSharedTaskPool::CompileCallback compile =
                           &mockCompile,
                       void *compileCtx = nullptr,
                       uint32_t maxConcurrentProfiles = 4) {
    EJitCoreId::setCurrentForTest(0);
    pool.bind(state_.get());
    pool.setCompiler(compile, compileCtx);
    pool.setMode(EJitCompileMode::Async);
    pool.setPgoEnabled(true, threshold, maxConcurrentProfiles);
    pool.setTraceClock(&mockTraceClock, &clock);
    ASSERT_TRUE(pool.setRequestAttemptsEnabled(true));
    ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  }

  /// Owner core 0 with cross-core code sharing + 4K-seal mocks, online PGO, the
  /// experimental request-attempt protocol and a deterministic dispatch clock:
  /// the observed contract on the cold non-owner (peer) preparation path.
  void bringUpColdPeer(EJitSharedTaskPool &pool, TraceClockLog &clock,
                       PrepLog &prep, PeerRangeCtx &range, uint32_t threshold,
                       EJitSharedTaskPool::CompileCallback compile =
                           &mockCompile,
                       void *compileCtx = nullptr) {
    EJitCoreId::setCurrentForTest(0);
    pool.bind(state_.get());
    pool.setCompiler(compile, compileCtx);
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeSharingEnabled(true);
    pool.setSealMode(true);
    pool.setCodeRangeProvider(&mockCodeRange, &range);
    pool.setSplitPoolCallback(&mockSplitPool, &prep);
    pool.setSealPageCallback(&mockSealPage, &prep);
    pool.setPgoEnabled(true, threshold, /*maxConcurrentProfiles=*/4);
    pool.setTraceClock(&mockTraceClock, &clock);
    ASSERT_TRUE(pool.setRequestAttemptsEnabled(true));
    ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  }

  /// Publish a Tier-1 for \p funcIndex through the production attempt path
  /// (enqueue with a real attempt token, worker compile, cachePublish) so the
  /// slot carries an observed-dispatch quota.
  void publishTier1(EJitSharedTaskPool &pool, uint32_t funcIndex) {
    uint64_t Token = 0;
    ASSERT_EQ(pool.compileOrGet(funcIndex, nullptr, 0, codeFor(funcIndex),
                                nullptr, 0, &Token)
                  .status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_NE(Token, 0u);
    ASSERT_TRUE(pool.pollOne());
  }

  EJitSharedCacheSlot *findReadySlot(uint32_t funcIndex) {
    for (uint32_t B = 0; B < kEJitSharedCacheBuckets; ++B)
      for (uint32_t S = 0; S < kEJitSharedCacheSlots; ++S) {
        EJitSharedCacheSlot &Slot = state_->buckets[B].slots[S];
        if (Slot.state.loadAcquire() ==
                static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
            Slot.funcIndex == funcIndex)
          return &Slot;
      }
    return nullptr;
  }

  static void release(EJitSharedTaskPool &pool,
                      const EJitSharedTaskPool::CompileOrGetResult &R) {
    if (R.hasReadToken)
      pool.releaseRead(R.bucketIndex);
  }

  std::unique_ptr<EJitSharedTaskPoolState> state_;
};

} // namespace

//===----------------------------------------------------------------------===//
// 1/ Real granted dispatches, the frozen boundary, and honest fallback.
//===----------------------------------------------------------------------===//

TEST_F(ObservedT1Test, LastAllowedDispatchCountsAndFreezesExactlyOnce) {
  TraceClockLog Clock;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/3);
  publishTier1(Owner, 90);

  EJitSharedCacheSlot *Slot = findReadySlot(90);
  ASSERT_NE(Slot, nullptr);
  EXPECT_EQ(Slot->t1DispatchLimit.loadRelaxed(), 3u);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 0u);
  EXPECT_EQ(Clock.calls, 0u);

  // The first limit-1 dispatches are real grants below the limit: they count
  // and return the Tier-1 pointer, but they freeze nothing and claim no T2.
  for (uint32_t I = 1; I < 3; ++I) {
    auto Hit = Owner.tryCacheHit0D(90);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit) << "dispatch " << I;
    ASSERT_NE(Hit.fnPtr, nullptr);
    release(Owner, Hit);
    EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), I);
  }
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 0u);
  EXPECT_EQ(Clock.calls, 0u);
  EXPECT_EQ(Owner.pendingCount(), 0u);

  // The last allowed dispatch is still granted; it freezes the boundary
  // exactly once and claims the Tier-2 request.
  auto Final = Owner.tryCacheHit0D(90);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  ASSERT_NE(Final.fnPtr, nullptr);
  release(Owner, Final);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 3u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Clock.calls, 1u);
  EXPECT_EQ(Owner.pendingCount(), 1u);
  // Legacy hotness/stats keep their identity-hit semantics (capped at the
  // configured threshold exactly like the legacy path).
  EXPECT_EQ(Slot->hitCount.loadRelaxed(), 3u);

  // Later calls fall back to AOT: no pointer, no count, no re-timestamp. The
  // frozen boundary can never be rewritten.
  const uint64_t Frozen = Slot->t1QuotaEnd.loadRelaxed();
  for (uint32_t I = 0; I < 3; ++I) {
    auto Late = Owner.tryCacheHit0D(90);
    EXPECT_EQ(Late.status, EJitCompileOrGetStatus::AlreadyPending);
    EXPECT_EQ(Late.fnPtr, nullptr);
  }
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 3u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), Frozen);
  EXPECT_EQ(Clock.calls, 1u);
}

TEST_F(ObservedT1Test, RejectedPathsConsumeNoQuota) {
  TraceClockLog Clock;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/3);
  publishTier1(Owner, 91);

  EJitSharedCacheSlot *Slot = findReadySlot(91);
  ASSERT_NE(Slot, nullptr);

  // (a) A peer core may read the pointer only when cross-core code sharing is
  //     platform-validated for this build. When it is not, the lookup must be
  //     a clean non-shareable fallback that consumes no quota.
#ifndef EJIT_SRE_SHARED_CODE_POINTERS
  EJitCoreId::setCurrentForTest(5);
  auto Peer = Owner.tryCacheHit0D(91);
  EXPECT_TRUE(Peer.readyButNotShareable);
  EXPECT_EQ(Peer.fnPtr, nullptr);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 0u);
#endif

  // (b) An identity match whose published pointer is not there yet is a miss,
  //     not a dispatch.
  EJitCoreId::setCurrentForTest(0);
  Slot->fnPtr.storeRelease(0);
  auto NullPtrHit = Owner.tryCacheHit0D(91);
  EXPECT_EQ(NullPtrHit.fnPtr, nullptr);
  EXPECT_FALSE(NullPtrHit.fastPathTerminal);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  Slot->fnPtr.storeRelease(reinterpret_cast<uintptr_t>(codeFor(91)));

  // The quota is intact: a real owner lookup still grants and counts.
  auto Real = Owner.tryCacheHit0D(91);
  ASSERT_EQ(Real.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, Real);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);
  EXPECT_EQ(Clock.calls, 0u);
}

TEST_F(ObservedT1Test, PeerPreparationFailureConsumesNoQuotaButSuccessCounts) {
  TraceClockLog Clock;
  PrepLog Prep;
  PeerRangeCtx Range;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockCompile, nullptr);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setCodeSharingEnabled(true);
  Owner.setSealMode(true);
  Owner.setCodeRangeProvider(&mockCodeRange, &Range);
  Owner.setSplitPoolCallback(&mockSplitPool, &Prep);
  Owner.setSealPageCallback(&mockSealPage, &Prep);
  Owner.setPgoEnabled(true, /*threshold=*/2, /*maxConcurrentProfiles=*/4);
  Owner.setTraceClock(&mockTraceClock, &Clock);
  ASSERT_TRUE(Owner.setRequestAttemptsEnabled(true));
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  publishTier1(Owner, 92);

  EJitSharedCacheSlot *Slot = findReadySlot(92);
  ASSERT_NE(Slot, nullptr);

  // (a) A failed cold peer preparation must not consume a real dispatch.
  Prep.splitOk = false;
  EJitCoreId::setCurrentForTest(3);
  auto Failed = Owner.tryCacheHit0D(92);
  EXPECT_TRUE(Failed.readyButNotShareable);
  EXPECT_EQ(Failed.fnPtr, nullptr);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  EXPECT_EQ(Clock.calls, 0u);

  // (b) A successful cold peer preparation hands back a real Tier-1 pointer:
  //     exactly one dispatch is granted and counted.
  Prep.splitOk = true;
  auto Prepared = Owner.tryCacheHit0D(92);
  ASSERT_EQ(Prepared.status, EJitCompileOrGetStatus::CacheHit);
  ASSERT_NE(Prepared.fnPtr, nullptr);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);
  release(Owner, Prepared);

  // The memoized second peer hit is a real grant too, and the quota is
  // untouched by the earlier failed preparation.
  auto Memoized = Owner.tryCacheHit0D(92);
  ASSERT_EQ(Memoized.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, Memoized);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 2u);
}

//===----------------------------------------------------------------------===//
// 1b/ Cold non-owner peer preparation: the final real dispatch must itself
//     arrange its Tier-2 request (R2-PR230-01). Runs in the token build and in
//     the EJIT_SRE_TASKPOOL_NO_RECLAIM build.
//===----------------------------------------------------------------------===//

TEST_F(ObservedT1Test, ColdPeerFinalDispatchClaimsTier2AtThresholdOne) {
  TraceClockLog Clock;
  PrepLog Prep;
  PeerRangeCtx Range;
  RecordedCompile Rec;
  EJitSharedTaskPool Owner;
  bringUpColdPeer(Owner, Clock, Prep, Range, /*threshold=*/1,
                  &mockRecordCompile, &Rec);
  publishTier1(Owner, 97);

  EJitSharedCacheSlot *Slot = findReadySlot(97);
  ASSERT_NE(Slot, nullptr);
  ASSERT_EQ(Slot->t1DispatchLimit.loadRelaxed(), 1u);
  const uint64_t Tier1Token = Slot->attemptToken;
  ASSERT_NE(Tier1Token, 0u);

  // A peer core that has never prepared this code takes the ONLY allowed
  // dispatch through the real cold path. Its commit must carry the validated
  // coordinates, so the final real entry itself claims the Tier-2 request.
  EJitCoreId::setCurrentForTest(3);
  auto Final = Owner.tryCacheHit0D(97);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  ASSERT_NE(Final.fnPtr, nullptr);
  release(Owner, Final);

  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Clock.calls, 1u);
  EXPECT_EQ(Owner.pendingCount(), 1u); // R2-PR230-01 regression

  // Later calls fall back and cannot rewrite the frozen boundary. The closed
  // quota re-arms the retry, but the outstanding claim is already this exact
  // attempt's request, so nothing new is queued.
  EJitCoreId::setCurrentForTest(0);
  auto Late = Owner.tryCacheHit0D(97);
  EXPECT_EQ(Late.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(Late.fnPtr, nullptr);
  EXPECT_EQ(Clock.calls, 1u);
  EXPECT_EQ(Owner.pendingCount(), 1u);

  // The queued request is this exact attempt's Tier-2 request with the frozen
  // observation; no later business call was needed to arrange it.
  ASSERT_TRUE(Owner.pollOne());
  const EJitCompileRequest *T2 = Rec.find(kEJitTierPgoUse, 97);
  ASSERT_NE(T2, nullptr);
  EXPECT_EQ(T2->attemptToken, Tier1Token);
  EXPECT_EQ(T2->t1DispatchCount, 1u);
  EXPECT_EQ(T2->t1DispatchLimit, 1u);
  EXPECT_EQ(T2->t1QuotaEnd, 1000u);
}

TEST_F(ObservedT1Test, ColdPeerFinalDispatchClaimsLargerQuota) {
  TraceClockLog Clock;
  PrepLog Prep;
  PeerRangeCtx Range;
  RecordedCompile Rec;
  EJitSharedTaskPool Owner;
  bringUpColdPeer(Owner, Clock, Prep, Range, /*threshold=*/3,
                  &mockRecordCompile, &Rec);
  publishTier1(Owner, 98);

  EJitSharedCacheSlot *Slot = findReadySlot(98);
  ASSERT_NE(Slot, nullptr);
  ASSERT_EQ(Slot->t1DispatchLimit.loadRelaxed(), 3u);
  const uint64_t Tier1Token = Slot->attemptToken;

  // Two granted owner dispatches leave the quota one below its limit.
  for (uint32_t I = 1; I < 3; ++I) {
    auto Hit = Owner.tryCacheHit0D(98);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit) << "dispatch " << I;
    release(Owner, Hit);
    EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), I);
  }
  EXPECT_EQ(Owner.pendingCount(), 0u);
  EXPECT_EQ(Clock.calls, 0u);

  // The cold peer's first preparation closes the larger quota: the final
  // dispatch is a real grant AND the Tier-2 claim.
  EJitCoreId::setCurrentForTest(2);
  auto Final = Owner.tryCacheHit0D(98);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  ASSERT_NE(Final.fnPtr, nullptr);
  release(Owner, Final);

  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 3u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Clock.calls, 1u);
  EXPECT_EQ(Owner.pendingCount(), 1u);

  ASSERT_TRUE(Owner.pollOne());
  const EJitCompileRequest *T2 = Rec.find(kEJitTierPgoUse, 98);
  ASSERT_NE(T2, nullptr);
  EXPECT_EQ(T2->attemptToken, Tier1Token);
  EXPECT_EQ(T2->t1DispatchCount, 3u);
  EXPECT_EQ(T2->t1QuotaEnd, 1000u);
}

TEST_F(ObservedT1Test, ColdPeerFailedPreparationConsumesNoQuotaNorTier2) {
  TraceClockLog Clock;
  PrepLog Prep;
  PeerRangeCtx Range;
  EJitSharedTaskPool Owner;
  bringUpColdPeer(Owner, Clock, Prep, Range, /*threshold=*/1);
  publishTier1(Owner, 99);

  EJitSharedCacheSlot *Slot = findReadySlot(99);
  ASSERT_NE(Slot, nullptr);

  // A failed cold preparation is a clean non-shareable fallback: no pointer,
  // no counted dispatch, no Tier-2 claim, no clock sample.
  Prep.splitOk = false;
  EJitCoreId::setCurrentForTest(1);
  auto Failed = Owner.tryCacheHit0D(99);
  EXPECT_TRUE(Failed.readyButNotShareable);
  EXPECT_EQ(Failed.fnPtr, nullptr);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 0u);
  EXPECT_EQ(Clock.calls, 0u);
  EXPECT_EQ(Owner.pendingCount(), 0u);

  // The quota is untouched: the retried cold preparation is the only allowed
  // dispatch and still arranges its own Tier-2 request.
  Prep.splitOk = true;
  auto Final = Owner.tryCacheHit0D(99);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  ASSERT_NE(Final.fnPtr, nullptr);
  release(Owner, Final);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Owner.pendingCount(), 1u);
}

TEST_F(ObservedT1Test, QueueFullAndDelayedRetryKeepFrozenObservation) {
  TraceClockLog Clock;
  RecordedCompile Rec;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/2, &mockRecordCompile, &Rec);
  publishTier1(Owner, 93);

  EJitSharedCacheSlot *Slot = findReadySlot(93);
  ASSERT_NE(Slot, nullptr);

  auto First = Owner.tryCacheHit0D(93);
  ASSERT_EQ(First.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, First);

  // The final allowed dispatch runs while the ring looks full: the Tier-2
  // enqueue is lost, but the observation is frozen and nothing rewrites it.
  const uint32_t EnqueuePos = state_->enqueuePos.loadRelaxed();
  const uint32_t RingIndex = EnqueuePos & (kEJitSharedQueueSlots - 1);
  state_->ring[RingIndex].sequence.storeRelaxed(UINT32_MAX);
  auto Final = Owner.tryCacheHit0D(93);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, Final);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 2u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Clock.calls, 1u);
  EXPECT_EQ(Owner.pendingCount(), 0u);
  EXPECT_EQ(state_->enqueuePos.loadRelaxed(), EnqueuePos); // push rolled back

  // Restore the ring and put OTHER compile work between the frozen admission
  // and the Tier-2 execution. A later closed-quota lookup retries the claim.
  state_->ring[RingIndex].sequence.storeRelaxed(EnqueuePos);
  uint64_t OtherToken = 0;
  ASSERT_EQ(Owner.compileOrGet(94, nullptr, 0, codeFor(94), nullptr, 0,
                               &OtherToken)
                .status,
            EJitCompileOrGetStatus::EnqueuedPending);
  auto Late = Owner.tryCacheHit0D(93);
  EXPECT_EQ(Late.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(Late.fnPtr, nullptr);
  // Two claims are outstanding: the unrelated Tier-1 above and the retried
  // Tier-2 for 93.
  EXPECT_EQ(Owner.pendingCount(), 2u);

  ASSERT_TRUE(Owner.pollOne()); // unrelated Tier-1 first
  ASSERT_TRUE(Owner.pollOne()); // then the delayed Tier-2 request

  const EJitCompileRequest *T2 =
      Rec.find(kEJitTierPgoUse, /*funcIndex=*/93);
  ASSERT_NE(T2, nullptr);
  // The retried request still carries the ORIGINAL frozen observation.
  EXPECT_EQ(T2->t1DispatchCount, 2u);
  EXPECT_EQ(T2->t1DispatchLimit, 2u);
  EXPECT_EQ(T2->t1QuotaEnd, 1000u);
  // The boundary was never re-timestamped by the retry or the intervening work.
  EXPECT_EQ(Clock.calls, 1u);
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
TEST_F(ObservedT1Test, AbandonedSeqlockRetryConsumesNoDispatch) {
  TraceClockLog Clock;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/2);
  publishTier1(Owner, 95);

  EJitSharedCacheSlot *Slot = findReadySlot(95);
  ASSERT_NE(Slot, nullptr);
  ASSERT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);

  // Force exactly one abandoned seqlock read: the resolve happens, the outer
  // publishSeq check fails, the lookup is discarded and retried.
  SeqRetryHookLog Hook;
  Hook.state = state_.get();
  // 0-dim identity hash is the bare funcIndex (see cacheLookupSeq0D).
  Hook.bucket = 95u % kEJitSharedCacheBuckets;
  Owner.setSeqlockRetryHookForTest(&mockSeqRetryHook, &Hook);

  auto Hit = Owner.tryCacheHit0D(95);
  ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  ASSERT_NE(Hit.fnPtr, nullptr);
  EXPECT_EQ(Hook.bumps, 1u);
  EXPECT_EQ(Hook.fires, 2u); // one abandoned attempt + the accepted retry

  // Exactly one real dispatch was granted; the abandoned read consumed none.
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);
  EXPECT_EQ(Clock.calls, 0u);
  // Legacy hotness still counts every identity hit, including the abandoned
  // one: the two contracts are deliberately distinct.
  EXPECT_EQ(Slot->hitCount.loadRelaxed(), 2u);
}

TEST_F(ObservedT1Test, ObservationAdmissionKeepsTheSeqlockStable) {
  TraceClockLog Clock;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/3);
  publishTier1(Owner, 104);

  EJitSharedCacheSlot *Slot = findReadySlot(104);
  ASSERT_NE(Slot, nullptr);
  EJitSharedCacheBucket &Bucket =
      state_->buckets[104u % kEJitSharedCacheBuckets];
  const uint32_t SeqAfterPublish = Bucket.publishSeq.loadRelaxed();
  EXPECT_EQ(Bucket.writeFlag.loadRelaxed(), 0u);

  // Below-limit grants run through the admission commit only. It must not set
  // writeFlag or bump publishSeq: observation activity never invalidates the
  // load-only seqlock reader (R1R-1). At e245 every grant bumped the sequence
  // by two and made concurrent readers clean-miss.
  for (uint32_t I = 1; I < 3; ++I) {
    auto Hit = Owner.tryCacheHit0D(104);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit) << "dispatch " << I;
    ASSERT_NE(Hit.fnPtr, nullptr);
    release(Owner, Hit);
    EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), I);
    EXPECT_EQ(Bucket.writeFlag.loadRelaxed(), 0u);
    EXPECT_EQ(Bucket.publishSeq.loadRelaxed(), SeqAfterPublish);
  }

  // The final grant freezes the boundary and really claims Tier-2; that claim is
  // the only step allowed to take the bucket writer lock (exactly one
  // acquire/release pair => +2), not the admission commit itself.
  auto Final = Owner.tryCacheHit0D(104);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, Final);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 3u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Owner.pendingCount(), 1u);
  EXPECT_EQ(Bucket.publishSeq.loadRelaxed(), SeqAfterPublish + 2u);
}
#endif // EJIT_SRE_TASKPOOL_NO_RECLAIM

TEST_F(ObservedT1Test, ConcurrentReplacementCannotInterleaveTheCommit) {
  TraceClockLog Clock;
  RecordedCompile Rec;
  ReplacementGate Gate;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/2, &mockRecordCompile, &Rec);
  Owner.setTraceClock(&replacementGateClock, &Gate);
  publishTier1(Owner, 100);

  EJitSharedCacheSlot *Slot = findReadySlot(100);
  ASSERT_NE(Slot, nullptr);
  const uint64_t Token1 = Slot->attemptToken;
  ASSERT_NE(Token1, 0u);

  auto First = Owner.tryCacheHit0D(100);
  ASSERT_EQ(First.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, First);
  ASSERT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);

  // Helper core: cancel the published attempt and publish a REAL replacement
  // into the same slot address while the finalizer sits in its commit window.
  std::thread Helper([&] {
    EJitCoreId::setCurrentForTest(4);
    while (!Gate.go.load(std::memory_order_acquire))
      std::this_thread::yield();
    Gate.helperStarted.store(true, std::memory_order_release);
    (void)Owner.cancelRequestAttempt(Token1);
    uint64_t NewToken = 0;
    (void)Owner.compileOrGet(100, nullptr, 0, codeFor(100), nullptr, 0,
                             &NewToken);
    Gate.newToken.store(NewToken, std::memory_order_release);
    // Bounded drain, NOT a single pollOne(): the finalizer may already have
    // queued its own (now cancelled) G1 Tier-2 request ahead of this
    // replacement Tier-1 in the FIFO ring. One poll can then pop and drop the
    // stale request without publishing G2 (R2R-02). Poll until this helper's
    // own attempt really owns the published slot, with an explicit bound; if
    // the bound is ever exhausted, the main-thread assertions below still
    // report the real failure instead of hiding it behind a sleep.
    constexpr uint32_t kMaxPolls = 64;
    for (uint32_t I = 0; I < kMaxPolls; ++I) {
      if (Slot->attemptToken == NewToken)
        break;
      if (!Owner.pollOne())
        std::this_thread::yield();
    }
    Gate.helperPublished.store(true, std::memory_order_release);
  });

  // Final admission: the clock hook fires inside the commit identity exclusion.
  auto Final = Owner.tryCacheHit0D(100);
  EXPECT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  // Release any read token BEFORE joining: the replacement's publish waits for
  // it in the token build (that is exactly the exclusion under test).
  release(Owner, Final);
  Helper.join();

  EXPECT_TRUE(Gate.helperPublished.load());
  // The replacement could not publish from inside the commit window: neither
  // build upgrades/leaks the exclusion between its identity check, CAS and
  // freeze.
  EXPECT_FALSE(Gate.publishedInsideCommit.load());

  const uint64_t Token2 = Gate.newToken.load();
  EXPECT_NE(Token2, 0u);
  EXPECT_NE(Token2, Token1);
  EXPECT_EQ(Slot->attemptToken, Token2);
  // The replacement's slot was reset by its own publish: it never carries the
  // predecessor's count or timestamp (R1-1 / R2-PR230-02).
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 0u);
  EXPECT_NE(Slot->t1QuotaEnd.loadRelaxed(), Gate.ts1);
  EXPECT_EQ(Gate.clockCalls.load(), 1u); // exactly one freeze, on G1

  // The replacement's own boundary is its own timestamp, carried by its own
  // Tier-2 request.
  auto G2First = Owner.tryCacheHit0D(100);
  ASSERT_EQ(G2First.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, G2First);
  auto G2Final = Owner.tryCacheHit0D(100);
  ASSERT_EQ(G2Final.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, G2Final);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 2u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), Gate.ts2);
  EXPECT_EQ(Gate.clockCalls.load(), 2u);
  EXPECT_EQ(Owner.pendingCount(), 1u);
  ASSERT_TRUE(Owner.pollOne());
  const EJitCompileRequest *T2 = Rec.find(kEJitTierPgoUse, 100);
  ASSERT_NE(T2, nullptr);
  EXPECT_EQ(T2->attemptToken, Token2);
  EXPECT_EQ(T2->t1DispatchCount, 2u);
  EXPECT_EQ(T2->t1QuotaEnd, Gate.ts2);
  EXPECT_NE(T2->t1QuotaEnd, Gate.ts1);
}

TEST_F(ObservedT1Test, ClosedQuotaDuringFreezeKeepsHonestObservation) {
  TraceClockLog Clock;
  RecordedCompile Rec;
  FreezeGate Gate;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/2, &mockRecordCompile, &Rec);
  Owner.setTraceClock(&freezeGateClock, &Gate);
  publishTier1(Owner, 101);

  EJitSharedCacheSlot *Slot = findReadySlot(101);
  ASSERT_NE(Slot, nullptr);

  auto First = Owner.tryCacheHit0D(101);
  ASSERT_EQ(First.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, First);
  ASSERT_EQ(Slot->t1DispatchCount.loadRelaxed(), 1u);

  // Peer core: a closed-quota lookup lands after the final CAS while the
  // freeze is still pending (the clock hook holds the commit there).
  std::thread Peer([&] {
    EJitCoreId::setCurrentForTest(5);
    while (!Gate.go.load(std::memory_order_acquire))
      std::this_thread::yield();
    auto PeerHit = Owner.tryCacheHit0D(101);
    Gate.peerStatus.store(static_cast<uint32_t>(PeerHit.status),
                          std::memory_order_relaxed);
    Gate.peerFn = PeerHit.fnPtr;
    Gate.peerDone.store(true, std::memory_order_release);
  });

  auto Final = Owner.tryCacheHit0D(101);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, Final);
  Peer.join();

  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 2u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 1000u);
  EXPECT_EQ(Gate.clockCalls.load(), 1u);
  EXPECT_EQ(Owner.pendingCount(), 1u);
  // A closed quota grants no Tier-1 pointer to the racing lookup in any build.
  EXPECT_EQ(Gate.peerFn, nullptr);

  ASSERT_TRUE(Owner.pollOne());
  const EJitCompileRequest *T2 = Rec.find(kEJitTierPgoUse, 101);
  ASSERT_NE(T2, nullptr);
  // The count boundary is exact in both builds; the timestamp is either the
  // frozen one or an honest unknown (0) - never a queue/compile time and never
  // a value from another attempt.
  EXPECT_EQ(T2->t1DispatchCount, 2u);
  EXPECT_EQ(T2->t1DispatchLimit, 2u);
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  // The freeze runs under the leaf observationLock, which the peer's
  // closed-quota claim also takes, so the finalizer's own request (with the
  // frozen timestamp) is the one queued.
  EXPECT_EQ(T2->t1QuotaEnd, 1000u);
#else
  // Token build: the peer may win the Tier-2 claim between the CAS and the
  // freeze store. That window is reported as unknown, never as a precise or
  // stale time.
  EXPECT_TRUE(T2->t1QuotaEnd == 0u || T2->t1QuotaEnd == 1000u);
#endif

  // The bundle mapping keeps the honest quality in both builds: the boundary is
  // known and frozen, and an unknown timestamp stays 0 (never a precise time).
  EJitProfileBundle Bundle;
  Bundle.freezeCompletedAt = 4242;
  applyT1DispatchObservation(
      Bundle, T2,
      Tier1ProfileAttemptIdentity{T2->attemptToken, T2->generation});
  EXPECT_EQ(Bundle.actualDispatchCount, 2u);
  EXPECT_EQ(Bundle.dispatchLimit, 2u);
  EXPECT_EQ(Bundle.dispatchQuality,
            T1DispatchObservationQuality::FrozenAtQuota);
  EXPECT_EQ(Bundle.quotaEnd, T2->t1QuotaEnd);
  EXPECT_EQ(Bundle.freezeCompletedAt, 4242u);
}

TEST_F(ObservedT1Test, LegacyModeKeepsLegacySemanticsAndReportsNoObservation) {
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockCompile, nullptr);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, /*threshold=*/2, /*maxConcurrentProfiles=*/4);
  // Deliberately NOT enabling the request-attempt protocol: legacy mode.
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(96, nullptr, 0, codeFor(96)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  EJitSharedCacheSlot *Slot = findReadySlot(96);
  ASSERT_NE(Slot, nullptr);
  EXPECT_EQ(Slot->t1DispatchLimit.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);

  // Legacy behavior is unchanged: the second identity hit crosses the
  // threshold, arms Tier-2, and the third falls back to AOT.
  for (uint32_t I = 0; I < 2; ++I) {
    auto Hit = Owner.tryCacheHit0D(96);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit) << "hit " << I;
    release(Owner, Hit);
  }
  EXPECT_EQ(Slot->hitCount.loadRelaxed(), 2u);
  EXPECT_EQ(Owner.pendingCount(), 1u);
  auto Late = Owner.tryCacheHit0D(96);
  EXPECT_EQ(Late.status, EJitCompileOrGetStatus::AlreadyPending);

  // No observation exists, so the bundle must say so instead of substituting
  // the configured threshold or a compile-time timestamp.
  EXPECT_EQ(Slot->t1DispatchCount.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1QuotaEnd.loadRelaxed(), 0u);
  EXPECT_EQ(Slot->t1DispatchLimit.loadRelaxed(), 0u);
}

TEST_F(ObservedT1Test, LegacyFailedPeerPreparationQueuesNoWrongTier2) {
#ifndef EJIT_SRE_SHARED_CODE_POINTERS
  // The defect lives on the cold peer-preparation path. Without compiled-in
  // cross-core code sharing the gate rejects before peerPrepareSlot() runs, so
  // the zeroed-coordinate arm cannot be built and the scenario is unreachable.
  GTEST_SKIP() << "cold peer preparation requires shared code pointers";
#endif
  PrepLog Prep;
  PeerRangeCtx Range;
  RecordedCompile Rec;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockRecordCompile, &Rec);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setCodeSharingEnabled(true);
  Owner.setSealMode(true);
  Owner.setCodeRangeProvider(&mockCodeRange, &Range);
  Owner.setSplitPoolCallback(&mockSplitPool, &Prep);
  Owner.setSealPageCallback(&mockSealPage, &Prep);
  Owner.setPgoEnabled(true, /*threshold=*/2, /*maxConcurrentProfiles=*/4);
  // Deliberately legacy: the request-attempt protocol stays OFF, so published
  // slots carry no attempt token and the observed contract is inactive.
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  auto Publish = [&](uint32_t Func) {
    uint64_t Token = 0;
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func), nullptr, 0,
                                 &Token)
                  .status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
  };
  // 0-dim hash == funcIndex: 32 is the unrelated Ready entry in bucket 0/slot 0
  // and 5 is the hot identity in bucket 5.
  Publish(32);
  Publish(5);
  ASSERT_EQ(state_->buckets[0].slots[0].funcIndex, 32u);
  ASSERT_EQ(state_->buckets[0].slots[0].state.loadAcquire(),
            static_cast<uint32_t>(EJitSharedSlotState::Ready));

  // Cold peer first touch: the second hit crosses the legacy threshold, but the
  // platform preparation FAILS. The clean AOT fallback must not arm the default
  // bucket0/slot0 coordinates and claim the unrelated func 32 (R2R-01).
  Prep.splitOk = false;
  EJitCoreId::setCurrentForTest(1);
  for (uint32_t I = 0; I < 2; ++I) {
    auto Failed = Owner.tryCacheHit0D(5);
    EXPECT_TRUE(Failed.readyButNotShareable) << "hit " << I;
    EXPECT_EQ(Failed.fnPtr, nullptr);
  }
  EXPECT_EQ(Owner.pendingCount(), 0u);
  EXPECT_EQ(Rec.find(kEJitTierPgoUse, 32), nullptr);
  EXPECT_EQ(Rec.find(kEJitTierPgoUse, 5), nullptr);

  // The saturated legacy lookup re-arms on the next identity hit, this time
  // from the slot itself (not through the failed preparation), so the correct
  // Tier-2 for func 5 is still queued and never the unrelated func 32.
  auto Saturated = Owner.tryCacheHit0D(5);
  EXPECT_EQ(Saturated.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(Saturated.fnPtr, nullptr);
  EXPECT_EQ(Owner.pendingCount(), 1u);
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_NE(Rec.find(kEJitTierPgoUse, 5), nullptr);
  EXPECT_EQ(Rec.find(kEJitTierPgoUse, 32), nullptr);
}

//===----------------------------------------------------------------------===//
// 2/ Integration gate: a granted real Tier-1 dispatch -> the Tier-2 request the
//    real worker actually dequeues -> the driver's ProfileBundle join, using
//    the production identity capture and the production bundle helper. The
//    ORC-engine part of EJitCompileDriver::compileCold cannot run here; the
//    engine-independent join it performs is the pair exercised below.
//===----------------------------------------------------------------------===//

TEST_F(ObservedT1Test, GrantedTier1ToQueuedTier2RequestReachesBundleJoin) {
  TraceClockLog Clock;
  RecordedCompile Rec;
  EJitSharedTaskPool Owner;
  bringUpObserved(Owner, Clock, /*threshold=*/2, &mockRecordCompile, &Rec);
  publishTier1(Owner, 97);

  // The exact request object the driver receives for the Instrumented tier.
  const EJitCompileRequest *T1 = Rec.find(kEJitTierInstrumented, 97);
  ASSERT_NE(T1, nullptr);
  ASSERT_NE(T1->attemptToken, 0u);

  EJitSharedCacheSlot *Slot = findReadySlot(97);
  ASSERT_NE(Slot, nullptr);
  const uint64_t PublishedToken = Slot->attemptToken;
  EXPECT_EQ(PublishedToken, T1->attemptToken);
  EXPECT_EQ(Slot->generation, T1->generation);

  // Real granted dispatches; the final one reaches the limit and queries the
  // real queue through the production path.
  auto First = Owner.tryCacheHit0D(97);
  ASSERT_EQ(First.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, First);
  auto Final = Owner.tryCacheHit0D(97);
  ASSERT_EQ(Final.status, EJitCompileOrGetStatus::CacheHit);
  release(Owner, Final);
  ASSERT_EQ(Owner.pendingCount(), 1u);

  // The real worker pops and compiles that queued Tier-2 request.
  ASSERT_TRUE(Owner.pollOne());
  const EJitCompileRequest *T2 = Rec.find(kEJitTierPgoUse, 97);
  ASSERT_NE(T2, nullptr);
  EXPECT_EQ(T2->attemptToken, PublishedToken);
  EXPECT_EQ(T2->generation, T1->generation);
  EXPECT_EQ(T2->t1DispatchCount, 2u);
  EXPECT_EQ(T2->t1DispatchLimit, 2u);
  EXPECT_EQ(T2->t1QuotaEnd, 1000u);

  // Driver join: capture the representative Tier-1 identity from the real
  // Tier-1 request (production helper called by compileCold) and apply the
  // frozen observation of the real Tier-2 request (production bundle helper).
  const Tier1ProfileAttemptIdentity Attempt =
      captureTier1ProfileAttemptIdentity(T1);
  EXPECT_EQ(Attempt.representativeAttemptToken, T1->attemptToken);
  EXPECT_EQ(Attempt.generation, T1->generation);

  EJitProfileBundle Bundle;
  Bundle.freezeCompletedAt = 4242;
  applyT1DispatchObservation(Bundle, T2, Attempt);
  EXPECT_EQ(Bundle.actualDispatchCount, 2u);
  EXPECT_EQ(Bundle.dispatchLimit, 2u);
  EXPECT_EQ(Bundle.quotaEnd, 1000u);
  EXPECT_EQ(Bundle.dispatchQuality,
            T1DispatchObservationQuality::FrozenAtQuota);
  EXPECT_EQ(Bundle.freezeCompletedAt, 4242u);
  EXPECT_NE(Bundle.quotaEnd, Bundle.freezeCompletedAt);

  // A replacement attempt identity rejects the same physical request: the
  // observation can never cross sessions at the join.
  EJitProfileBundle Replaced;
  applyT1DispatchObservation(
      Replaced, T2,
      Tier1ProfileAttemptIdentity{T1->attemptToken + 1, T1->generation});
  EXPECT_EQ(Replaced.actualDispatchCount, 0u);
  EXPECT_EQ(Replaced.quotaEnd, 0u);
  EXPECT_EQ(Replaced.dispatchLimit, 0u);
  EXPECT_EQ(Replaced.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);

  // A null Tier-1 request (legacy/sync compile) captures no identity, so the
  // same request reports Unavailable instead of attaching to a session.
  const Tier1ProfileAttemptIdentity NoIdentity =
      captureTier1ProfileAttemptIdentity(nullptr);
  EXPECT_EQ(NoIdentity.representativeAttemptToken, 0u);
  EJitProfileBundle NoSession;
  applyT1DispatchObservation(NoSession, T2, NoIdentity);
  EXPECT_EQ(NoSession.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);
}

//===----------------------------------------------------------------------===//
// 3/ Bundle mapping: exact attempt/generation, honest quality, distinct times.
//===----------------------------------------------------------------------===//

namespace {

EJitCompileRequest observedRequest(uint64_t token, uint32_t generation,
                                   uint64_t count, uint64_t limit,
                                   uint64_t quotaEnd) {
  EJitCompileRequest Req{};
  Req.attemptToken = token;
  Req.generation = generation;
  Req.t1DispatchCount = count;
  Req.t1DispatchLimit = limit;
  Req.t1QuotaEnd = quotaEnd;
  return Req;
}

} // namespace

TEST(ObservedT1Bundle, FrozenBoundaryIsDistinctFromFreezeCompletion) {
  EJitCompileRequest Req = observedRequest(0xABC, 7, 64, 64, 123456);
  EJitProfileBundle Bundle;
  Bundle.freezeCompletedAt = 999999; // set by the driver at snapshot finish
  applyT1DispatchObservation(Bundle, &Req, /*expectedAttemptToken=*/0xABC,
                             /*expectedGeneration=*/7);
  EXPECT_EQ(Bundle.actualDispatchCount, 64u);
  EXPECT_EQ(Bundle.dispatchLimit, 64u);
  EXPECT_EQ(Bundle.quotaEnd, 123456u);
  EXPECT_EQ(Bundle.dispatchQuality,
            T1DispatchObservationQuality::FrozenAtQuota);
  // quotaEnd and freezeCompletedAt are deliberately different instants: the
  // Tier-2 queue wait and the compilation are not part of the dispatch window.
  EXPECT_EQ(Bundle.freezeCompletedAt, 999999u);
}

TEST(ObservedT1Bundle, UnknownClockAndOpenQuotaAreReportedHonestly) {
  // count == limit with no clock configured: the boundary is known, the
  // timestamp is not. It must stay unknown rather than be fabricated.
  EJitCompileRequest NoClock = observedRequest(0x11, 3, 64, 64, 0);
  EJitProfileBundle Frozen;
  applyT1DispatchObservation(Frozen, &NoClock, 0x11, 3);
  EXPECT_EQ(Frozen.actualDispatchCount, 64u);
  EXPECT_EQ(Frozen.quotaEnd, 0u);
  EXPECT_EQ(Frozen.dispatchQuality,
            T1DispatchObservationQuality::FrozenAtQuota);

  // count < limit: a real partial observation whose window was still open. A
  // stale quotaEnd carried by such a request must never leak into the bundle.
  EJitCompileRequest Open = observedRequest(0x12, 3, 40, 64, 5555);
  EJitProfileBundle Partial;
  applyT1DispatchObservation(Partial, &Open, 0x12, 3);
  EXPECT_EQ(Partial.actualDispatchCount, 40u);
  EXPECT_EQ(Partial.dispatchLimit, 64u);
  EXPECT_EQ(Partial.quotaEnd, 0u);
  EXPECT_EQ(Partial.dispatchQuality,
            T1DispatchObservationQuality::PartialOpenQuota);
}

TEST(ObservedT1Bundle, StaleAttemptOrGenerationCannotReachReplacementSession) {
  // Same logical key, replacement attempt: the old request must not attach its
  // observation to the new session, and vice versa.
  EJitCompileRequest Old = observedRequest(0x100, 5, 64, 64, 777);
  EJitCompileRequest New = observedRequest(0x200, 5, 64, 64, 888);

  EJitProfileBundle Replacement;
  applyT1DispatchObservation(Replacement, &Old, /*expected=*/0x200, 5);
  EXPECT_EQ(Replacement.actualDispatchCount, 0u);
  EXPECT_EQ(Replacement.quotaEnd, 0u);
  EXPECT_EQ(Replacement.dispatchLimit, 0u);
  EXPECT_EQ(Replacement.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);

  EJitProfileBundle Accepted;
  applyT1DispatchObservation(Accepted, &New, /*expected=*/0x200, 5);
  EXPECT_EQ(Accepted.actualDispatchCount, 64u);
  EXPECT_EQ(Accepted.quotaEnd, 888u);

  // A generation change (owner re-init / republished slot) is the same class.
  EJitProfileBundle StaleGeneration;
  applyT1DispatchObservation(StaleGeneration, &New, 0x200, /*expectedGen=*/6);
  EXPECT_EQ(StaleGeneration.actualDispatchCount, 0u);
  EXPECT_EQ(StaleGeneration.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);
}

TEST(ObservedT1Bundle, MalformedAndLegacyRequestsReportUnavailable) {
  // Legacy/tokenless mode: no observation exists.
  EJitProfileBundle Legacy;
  EJitCompileRequest NoObservation{};
  applyT1DispatchObservation(Legacy, &NoObservation, 0, 0);
  EXPECT_EQ(Legacy.actualDispatchCount, 0u);
  EXPECT_EQ(Legacy.quotaEnd, 0u);
  EXPECT_EQ(Legacy.dispatchLimit, 0u);
  EXPECT_EQ(Legacy.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);

  // A null request (non-shared / early path) is the same.
  EJitProfileBundle NoRequest;
  applyT1DispatchObservation(NoRequest, nullptr, 0x300, 9);
  EXPECT_EQ(NoRequest.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);

  // count > limit is malformed and must be rejected, never reported.
  EJitCompileRequest Overshoot = observedRequest(0x300, 9, 65, 64, 42);
  EJitProfileBundle Malformed;
  applyT1DispatchObservation(Malformed, &Overshoot, 0x300, 9);
  EXPECT_EQ(Malformed.actualDispatchCount, 0u);
  EXPECT_EQ(Malformed.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);

  // limit == 0 means the slot carried no observed quota at all.
  EJitCompileRequest NoLimit = observedRequest(0x300, 9, 0, 0, 42);
  EJitProfileBundle NoQuota;
  applyT1DispatchObservation(NoQuota, &NoLimit, 0x300, 9);
  EXPECT_EQ(NoQuota.dispatchQuality,
            T1DispatchObservationQuality::Unavailable);
}
