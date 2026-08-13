//===-- EJitSharedTaskPoolTest.cpp - cross-core shared taskpool tests -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Deterministic, single-thread tests for the cross-core SHARED taskpool. Many
//  "cores" are simulated inside one process by switching EJitCoreId between
//  calls — no real thread is needed to exercise owner election, the shared MPSC
//  queue, cross-core dedup, generation/version invalidation, and the commit
//  gate. One optional test uses the host platform task to run a real worker.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm::ejit;

namespace {

// A deterministic, non-null "compiled code" address derived from funcIndex. The
// tests never execute it; they only compare/cache it.
void *codeFor(uint32_t funcIndex) {
  return reinterpret_cast<void *>(0x100000ull +
                                  static_cast<uintptr_t>(funcIndex) * 64u);
}

bool mockCompile(void * /*ctx*/, const EJitCompileRequest &req, void **outFn) {
  *outFn = codeFor(req.funcIndex);
  return true;
}

// Compiler that toggles an instance mid-compile to model a deactivate landing
// during compilation (ctx = the pool).
struct ToggleCtx {
  EJitSharedTaskPool *pool;
  uint32_t dimType;
  uint32_t instanceId;
};
bool mockCompileThenToggle(void *ctx, const EJitCompileRequest &req,
                           void **outFn) {
  auto *t = static_cast<ToggleCtx *>(ctx);
  *outFn = codeFor(req.funcIndex);
  t->pool->setInstanceEnabled(t->dimType, t->instanceId, false); // bump version
  return true;
}

// Records every pointer handed to the release callback.
struct ReleaseLog {
  std::vector<void *> freed;
};
void mockRelease(void *ctx, void *oldFn) {
  static_cast<ReleaseLog *>(ctx)->freed.push_back(oldFn);
}

struct PrepareLog {
  std::vector<uint32_t> cores;
  bool succeed = true;
};
bool mockPrepareCode(void *ctx, const void * /*fnPtr*/) {
  auto *log = static_cast<PrepareLog *>(ctx);
  log->cores.push_back(EJitCoreId::current());
  return log->succeed;
}

//===----------------------------------------------------------------------===//
// 4K page-seal mocks: a per-core split + per-page seal that log which core ran
// them and (optionally) fail or inject a concurrent slot/generation change at a
// chosen seal step (to exercise the re-validate-after-prepare protocol).
//===----------------------------------------------------------------------===//
struct FourKLog {
  std::vector<std::pair<uintptr_t, uint32_t>> splits; // (poolBase, core)
  std::vector<std::pair<uintptr_t, uint32_t>> seals;  // (pageVA, core)
  bool splitOk = true;
  int failSealAtIndex = -1;           // fail the Nth (0-based) seal call
  void (*raceHook)(void *) = nullptr; // run during a chosen seal (no lock held)
  void *raceCtx = nullptr;
  int raceAtSealIndex = -1;
};
bool mockSplitPool(void *ctx, uintptr_t poolBase, uint64_t /*poolSize*/) {
  auto *l = static_cast<FourKLog *>(ctx);
  l->splits.push_back({poolBase, EJitCoreId::current()});
  return l->splitOk;
}
bool mockSealPage(void *ctx, uintptr_t pageVA) {
  auto *l = static_cast<FourKLog *>(ctx);
  int idx = static_cast<int>(l->seals.size());
  l->seals.push_back({pageVA, EJitCoreId::current()});
  if (l->raceHook && idx == l->raceAtSealIndex)
    l->raceHook(l->raceCtx); // simulate a concurrent publish during prepare
  if (l->failSealAtIndex == idx)
    return false;
  return true;
}

// Owner-side resolver of a compiled pointer to a (test-controlled) executable
// range. Mutating the RangeCtx between compiles models distinct code extents.
struct RangeCtx {
  uintptr_t poolBase = 0x40000000ull;
  uint64_t poolSize = 0x200000ull; // 2 MiB
  uintptr_t codeStart = 0x40000000ull;
  uint64_t codeSize = 64;
  uint32_t poolId = 0;
  bool provide = true;
};
bool mockCodeRange(void *ctx, const void *fnPtr, EJitCompiledCodeInfo *out) {
  auto *r = static_cast<RangeCtx *>(ctx);
  if (!r->provide)
    return false;
  out->fnPtr = const_cast<void *>(fnPtr);
  out->codeStart = r->codeStart;
  out->codeSize = r->codeSize;
  out->poolBase = r->poolBase;
  out->poolSize = r->poolSize;
  out->poolId = r->poolId;
  return true;
}

// Compiler that returns a distinct, non-null pointer on every call (models a
// recompile landing at a new code address).
struct SeqCompiler {
  uint32_t n = 0;
};
bool mockCompileSeq(void *ctx, const EJitCompileRequest & /*req*/,
                    void **outFn) {
  auto *s = static_cast<SeqCompiler *>(ctx);
  *outFn = reinterpret_cast<void *>(0x200000ull +
                                    static_cast<uintptr_t>(++s->n) * 64u);
  return true;
}

// Injectable worker hooks. The entry is never run on a real thread here; tests
// drive pollOne() manually, so these only prove "exactly one worker started".
struct WorkerHooks {
  int starts = 0;
  int stops = 0;
  bool failNext = false;
};
bool mockWorkerStart(void *ctx, EJitSharedTaskPool::WorkerEntryFn /*entry*/,
                     void * /*entryCtx*/, uint64_t *outTaskId) {
  auto *w = static_cast<WorkerHooks *>(ctx);
  if (w->failNext)
    return false;
  ++w->starts;
  *outTaskId = 0xABCDull;
  return true;
}
void mockWorkerStop(void *ctx) { ++static_cast<WorkerHooks *>(ctx)->stops; }

// A worker start hook that ignores its ctx (returns success), so a test can
// share a non-WorkerHooks ctx between start and stop hooks.
bool startOkIgnoreCtx(void * /*ctx*/,
                      EJitSharedTaskPool::WorkerEntryFn /*entry*/,
                      void * /*entryCtx*/, uint64_t *outTaskId) {
  if (outTaskId)
    *outTaskId = 1;
  return true;
}

EJitDimPair dim(uint32_t t, uint32_t i) { return EJitDimPair{t, i}; }

class SharedTaskPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    EJitCoreId::resetForTest();
    state_ = std::make_unique<EJitSharedTaskPoolState>();
  }
  void TearDown() override { EJitCoreId::resetForTest(); }

  // Bring up a single owner on core 0 with the mock compiler and (by default)
  // no injected worker (the test drives pollOne()).
  void bringUpOwner(EJitSharedTaskPool &pool, bool codeSharing = false) {
    EJitCoreId::setCurrentForTest(0);
    pool.bind(state_.get());
    pool.setCompiler(&mockCompile, nullptr);
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeSharingEnabled(codeSharing);
    ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  }

  // Bring up a 4K-seal owner with code sharing ON, an injected range provider,
  // and mock per-core split + per-page seal callbacks.
  void bringUpOwner4K(EJitSharedTaskPool &pool, FourKLog &fourK,
                      RangeCtx &range) {
    EJitCoreId::setCurrentForTest(0);
    pool.bind(state_.get());
    pool.setCompiler(&mockCompile, nullptr);
    pool.setMode(EJitCompileMode::Async);
    pool.setCodeSharingEnabled(true);
    pool.setSealMode(true);
    pool.setCodeRangeProvider(&mockCodeRange, &range);
    pool.setSplitPoolCallback(&mockSplitPool, &fourK);
    pool.setSealPageCallback(&mockSealPage, &fourK);
    ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  }

  // Compile + publish one (funcIndex, no-dims) entry on the owner core.
  void publish(EJitSharedTaskPool &owner, uint32_t funcIndex) {
    EJitCoreId::setCurrentForTest(0);
    ASSERT_EQ(
        owner.compileOrGet(funcIndex, nullptr, 0, codeFor(funcIndex)).status,
        EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(owner.pollOne());
  }

  EJitSharedCacheSlot *findReadySlot(uint32_t funcIndex) {
    for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
      for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
        EJitSharedCacheSlot &Slot = state_->buckets[b].slots[s];
        if (Slot.state.loadAcquire() ==
                static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
            Slot.funcIndex == funcIndex)
          return &Slot;
      }
    return nullptr;
  }

  std::unique_ptr<EJitSharedTaskPoolState> state_;
};

//===----------------------------------------------------------------------===//
// 15/ABI: layout + static-assert-backed properties, header stamped on init.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, AbiLayoutAndHeader) {
  EXPECT_TRUE(std::is_standard_layout<EJitSharedTaskPoolState>::value);
  EXPECT_TRUE(std::is_trivially_destructible<EJitSharedTaskPoolState>::value);
  EXPECT_EQ(alignof(EJitSharedTaskPoolState), kEJitSharedCacheLine);
  EXPECT_EQ(offsetof(EJitSharedTaskPoolState, magic), 0u);

  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EXPECT_EQ(state_->magic, kEJitSharedAbiMagic);
  EXPECT_EQ(state_->abiVersion, kEJitSharedAbiVersion);
  EXPECT_EQ(state_->structSize, sizeof(EJitSharedTaskPoolState));
}

// A process-global instance of the shared blob must require no C++ dynamic
// initialization. Otherwise each image/core can emit and run a
// _GLOBAL__sub_I constructor that clears the shared section after another core
// has already published queue/cache/owner state.
TEST_F(SharedTaskPoolTest, SharedStateRequiresNoDynamicInitialization) {
  EXPECT_TRUE(
      std::is_trivially_default_constructible<EJitSharedTaskPoolState>::value)
      << "shared state must not emit .init_array initialization";
}

//===----------------------------------------------------------------------===//
// 1/ Owner election: exactly one owner across simulated cores.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, ExactlyOneOwnerAcrossCores) {
  EJitSharedTaskPool c0, c1, c2;
  for (auto *p : {&c0, &c1, &c2}) {
    p->bind(state_.get());
    p->setCompiler(&mockCompile, nullptr);
    p->setMode(EJitCompileMode::Async);
  }
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(c0.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(c1.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  EJitCoreId::setCurrentForTest(2);
  EXPECT_EQ(c2.init(), EJitSharedTaskPool::InitResult::AttachedReady);

  EXPECT_TRUE(c0.isOwner());
  EXPECT_FALSE(c1.isOwner());
  EXPECT_FALSE(c2.isOwner());
  EXPECT_EQ(state_->ownerCoreId.loadAcquire(), 0u);

  // Idempotency: the owner re-observing init() stays Ready, no re-election.
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(c0.init(), EJitSharedTaskPool::InitResult::AttachedReady);
}

//===----------------------------------------------------------------------===//
// 2/ Multiple cores → exactly one worker created (only the owner starts it).
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, OnlyOwnerStartsOneWorker) {
  WorkerHooks hooks;
  EJitSharedTaskPool c0, c1, c2;
  for (auto *p : {&c0, &c1, &c2}) {
    p->bind(state_.get());
    p->setCompiler(&mockCompile, nullptr);
    p->setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
    p->setMode(EJitCompileMode::Async);
  }
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(c0.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(c1.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  EJitCoreId::setCurrentForTest(2);
  EXPECT_EQ(c2.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  EXPECT_EQ(hooks.starts, 1); // single worker
}

//===----------------------------------------------------------------------===//
// 3/ Producers never observe a half-initialized blob; Initializing → pending.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, InitializingExposesNoHalfState) {
  EJitSharedTaskPool pool;
  pool.bind(state_.get());
  pool.setCompiler(&mockCompile, nullptr);
  pool.setMode(EJitCompileMode::Async);
  // Force the "another core is still initializing" state.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Initializing));

  // A producer before Ready cleanly falls back and touches no shared queue.
  EJitCoreId::setCurrentForTest(5);
  auto r = pool.compileOrGet(7, nullptr, 0, codeFor(7));
  EXPECT_EQ(r.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_EQ(r.fnPtr, codeFor(7));
  EJitSharedDiagnostics d;
  pool.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 0u);

  // init() against an Initializing peer returns pending (bounded, no deadlock).
  EXPECT_EQ(pool.init(), EJitSharedTaskPool::InitResult::InitInProgress);
}

//===----------------------------------------------------------------------===//
// 3b/ Compile mode is CROSS-CORE SHARED runtime state.
//
// Regression test for the shared compile-mode bug: setMode()/configuredMode_
// only seeds the mode at init time; a runtime mode switch must be published to
// the shared state_->mode (the field compileOrGet reads with an acquire load)
// so EVERY core — including a peer/other-core object, not just the owner —
// observes it. A mode flip is a pure control flag: it must NOT reset the queue,
// dedup, cache, owner election, or the single worker.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, SharedCompileModeIsCrossCoreRuntimeState) {
  const uint32_t kOff = static_cast<uint32_t>(EJitCompileMode::Off);
  const uint32_t kAsync = static_cast<uint32_t>(EJitCompileMode::Async);

  // Owner comes up with the configured mode = Off; the shared state must start
  // in exactly that configured mode.
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setMode(EJitCompileMode::Off);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(state_->mode.loadAcquire(), kOff);
  EXPECT_EQ(owner.getSharedMode(), EJitCompileMode::Off);

  // A second instance simulating another core attaches to the SAME blob.
  EJitSharedTaskPool peer;
  EJitCoreId::setCurrentForTest(1);
  peer.bind(state_.get());
  peer.setCompiler(&mockCompile, nullptr);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  EXPECT_EQ(peer.getSharedMode(), EJitCompileMode::Off);

  // In Off mode compileOrGet falls back cleanly (OffMode) on a cache miss.
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(owner.compileOrGet(10, nullptr, 0, codeFor(10)).status,
            EJitCompileOrGetStatus::OffMode);

  // Runtime switch to Async: the shared state_->mode must become Async with
  // release semantics (this is the field compileOrGet reads). Before the fix
  // this stayed Off and the producer kept taking OffMode forever.
  owner.setSharedMode(EJitCompileMode::Async);
  EXPECT_EQ(state_->mode.loadAcquire(), kAsync);
  EXPECT_EQ(owner.getSharedMode(), EJitCompileMode::Async);

  // compileOrGet no longer takes OffMode solely due to a stale shared mode: a
  // fresh request now enqueues async work.
  EXPECT_EQ(owner.compileOrGet(11, nullptr, 0, codeFor(11)).status,
            EJitCompileOrGetStatus::EnqueuedPending);

  // The switch is visible from the OTHER core's object, not only the owner.
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(peer.getSharedMode(), EJitCompileMode::Async);
  EXPECT_EQ(peer.compileOrGet(12, nullptr, 0, codeFor(12)).status,
            EJitCompileOrGetStatus::EnqueuedPending);

  // A mode flip is a pure control flag: the queued work is NOT reset. The two
  // distinct funcIndexes (11, 12) are still pending and the owner can drain it.
  EJitCoreId::setCurrentForTest(0);
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 2u);
  EXPECT_EQ(d.pendingCount, 2u);

  // Switch back to Sync/Off: shared state_->mode becomes Off and compileOrGet
  // returns OffMode/fallback again — visible from both the owner and the peer.
  owner.setSharedMode(EJitCompileMode::Off);
  EXPECT_EQ(state_->mode.loadAcquire(), kOff);
  EXPECT_EQ(owner.getSharedMode(), EJitCompileMode::Off);
  auto offOwner = owner.compileOrGet(13, nullptr, 0, codeFor(13));
  EXPECT_EQ(offOwner.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_EQ(offOwner.fnPtr, codeFor(13));

  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(peer.getSharedMode(), EJitCompileMode::Off);
  auto offPeer = peer.compileOrGet(14, nullptr, 0, codeFor(14));
  EXPECT_EQ(offPeer.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_EQ(offPeer.fnPtr, codeFor(14));

  // The earlier async work survived the mode flips (control flag, not reinit).
  EJitCoreId::setCurrentForTest(0);
  owner.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 2u);
}

//===----------------------------------------------------------------------===//
// 4 & 18/ Owner worker-start failure → Failed, no fake JIT success.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, OwnerFailurePropagatesFailed) {
  WorkerHooks hooks;
  hooks.failNext = true;
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
  owner.setMode(EJitCompileMode::Async);
  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::OwnerFailed);
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Failed));
  EXPECT_EQ(state_->lastInitError.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitError::WorkerStartFailed));

  // A peer observing Failed gets a clean fallback, never an infinite wait.
  EJitSharedTaskPool peer;
  EJitCoreId::setCurrentForTest(1);
  peer.bind(state_.get());
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::OwnerFailed);

  // No fake JIT success: compileOrGet returns the fallback, never a fnPtr.
  auto r = peer.compileOrGet(3, nullptr, 0, codeFor(3));
  EXPECT_EQ(r.fnPtr, codeFor(3));
  EXPECT_FALSE(r.hasReadToken);
}

//===----------------------------------------------------------------------===//
// 5/ Multiple producer cores enqueue into ONE shared queue; owner drains.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, MultiProducerSharedQueue) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  // Three different cores each submit a distinct funcIndex.
  for (uint32_t core = 0; core < 3; ++core) {
    EJitCoreId::setCurrentForTest(core);
    auto r = owner.compileOrGet(100 + core, nullptr, 0, codeFor(100 + core));
    EXPECT_EQ(r.status, EJitCompileOrGetStatus::EnqueuedPending);
  }
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 3u);
  EXPECT_EQ(d.asyncEnqueues, 3u);

  // The single owner worker (driven here by pollBudget) compiles all three.
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(owner.pollBudget(8), 3u);
  owner.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount, 3u);
  EXPECT_EQ(d.asyncCompiles, 3u);
}

//===----------------------------------------------------------------------===//
// 6/ Cross-core dedup: same key submitted by two cores compiles once.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, CrossCoreSameKeyDedup) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  owner.setInstanceEnabled(0, 3, true);
  EJitDimPair d0[1] = {dim(0, 3)};

  EJitCoreId::setCurrentForTest(1);
  auto a = owner.compileOrGet(42, d0, 1, codeFor(42));
  EXPECT_EQ(a.status, EJitCompileOrGetStatus::EnqueuedPending);

  EJitCoreId::setCurrentForTest(2);
  auto b = owner.compileOrGet(42, d0, 1, codeFor(42));
  EXPECT_EQ(b.status, EJitCompileOrGetStatus::AlreadyPending);

  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.pendingCount, 1u);
  EXPECT_EQ(d.queueDepth, 1u);
}

//===----------------------------------------------------------------------===//
// 7/ Queue full → clean fallback AND dedup rolled back.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, QueueFullRollsBackDedup) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);
  // Fill the ring to capacity with distinct funcIndexes.
  for (uint32_t f = 0; f < kEJitSharedQueueSlots; ++f) {
    auto r = owner.compileOrGet(f, nullptr, 0, codeFor(f));
    ASSERT_EQ(r.status, EJitCompileOrGetStatus::EnqueuedPending);
  }
  // One more distinct funcIndex overflows: clean fallback, dedup rolled back.
  uint32_t overflow = kEJitSharedQueueSlots; // still < max func index
  auto r = owner.compileOrGet(overflow, nullptr, 0, codeFor(overflow));
  EXPECT_EQ(r.status, EJitCompileOrGetStatus::QueueFullFallback);

  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.pendingCount, kEJitSharedQueueSlots); // overflow NOT counted
  EXPECT_EQ(d.queueFull, 1u);
  EXPECT_EQ(state_->inFlight[overflow].loadAcquire(), 0u); // rolled back
}

//===----------------------------------------------------------------------===//
// 8/ Generation switch (owner re-init) drops stale queue + cache.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, GenerationSwitchDropsStaleState) {
  EJitSharedTaskPool owner;
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, new WorkerHooks());
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);
  // Publish one entry and queue one more (left un-polled).
  ASSERT_EQ(owner.compileOrGet(1, nullptr, 0, codeFor(1)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());
  ASSERT_EQ(owner.compileOrGet(2, nullptr, 0, codeFor(2)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount, 1u);
  EXPECT_EQ(d.queueDepth, 1u);
  uint32_t gen0 = d.generation;

  // Orderly shutdown + re-init bumps generation and resets shared storage.
  owner.ownerShutdown();
  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.getDiagnostics(d);
  EXPECT_GT(d.generation, gen0);
  EXPECT_EQ(d.cacheReadyCount, 0u); // stale cache dropped
  EXPECT_EQ(d.queueDepth, 0u);      // stale queue dropped
  // The previously published entry no longer hits.
  EJitCoreId::setCurrentForTest(0);
  auto miss = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_NE(miss.status, EJitCompileOrGetStatus::CacheHit);
}

//===----------------------------------------------------------------------===//
// 9/ Deactivate during compile blocks publish (commit gate).
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, DeactivateDuringCompileBlocksPublish) {
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setMode(EJitCompileMode::Async);
  ToggleCtx tctx{&owner, 0, 7};
  owner.setCompiler(&mockCompileThenToggle, &tctx);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.setInstanceEnabled(0, 7, true);

  EJitDimPair d0[1] = {dim(0, 7)};
  ASSERT_EQ(owner.compileOrGet(9, d0, 1, codeFor(9)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne()); // compiles, then toggle bumps version → reject
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount, 0u); // nothing published
  EXPECT_EQ(d.compileFailed, 1u);
}

//===----------------------------------------------------------------------===//
// 10 & 12/ Cache publish visibility + read-token release.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, PublishLookupAndReadTokenRelease) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};
  ASSERT_EQ(owner.compileOrGet(11, d0, 1, codeFor(11)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());

  auto hit = owner.compileOrGet(11, d0, 1, codeFor(11));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, codeFor(11));
  EXPECT_TRUE(hit.hasReadToken);
  // A held read token keeps readers > 0.
  EXPECT_GT(state_->buckets[hit.bucketIndex].readers.loadAcquire(), 0u);
  owner.releaseRead(hit.bucketIndex);
  EXPECT_EQ(state_->buckets[hit.bucketIndex].readers.loadAcquire(), 0u);
}

//===----------------------------------------------------------------------===//
// 11/ Cross-core fnPtr sharing gate.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, CrossCoreFnPtrSharingGate) {
  // codeSharing OFF: a non-owner cleanly rejects the pointer.
  {
    EJitSharedTaskPool owner;
    bringUpOwner(owner, /*codeSharing=*/false);
    EJitCoreId::setCurrentForTest(0);
    ASSERT_EQ(owner.compileOrGet(20, nullptr, 0, codeFor(20)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    EXPECT_TRUE(owner.pollOne());
    // Owner can read its own pointer.
    auto ownerHit = owner.compileOrGet(20, nullptr, 0, codeFor(20));
    EXPECT_EQ(ownerHit.status, EJitCompileOrGetStatus::CacheHit);
    if (ownerHit.hasReadToken)
      owner.releaseRead(ownerHit.bucketIndex);
    // Non-owner core may NOT: clean reject, no token, no recompile churn.
    EJitSharedTaskPool peer;
    peer.bind(state_.get());
    EJitCoreId::setCurrentForTest(9);
    auto peerHit = peer.compileOrGet(20, nullptr, 0, codeFor(20));
    EXPECT_FALSE(peerHit.hasReadToken);
    EXPECT_TRUE(peerHit.readyButNotShareable);
    EXPECT_EQ(peerHit.fnPtr, codeFor(20)); // fallback
  }
  // codeSharing ON: any core reads the SAME fnPtr.
  {
    state_ = std::make_unique<EJitSharedTaskPoolState>();
    PrepareLog prepare;
    EJitSharedTaskPool owner;
    owner.setPrepareCodeCallback(&mockPrepareCode, &prepare);
    bringUpOwner(owner, /*codeSharing=*/true);
    EJitCoreId::setCurrentForTest(0);
    ASSERT_EQ(owner.compileOrGet(21, nullptr, 0, codeFor(21)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    EXPECT_TRUE(owner.pollOne());
    EJitSharedTaskPool peer;
    peer.bind(state_.get());
    peer.setPrepareCodeCallback(&mockPrepareCode, &prepare);
    EJitCoreId::setCurrentForTest(9);
    auto peerHit = peer.compileOrGet(21, nullptr, 0, codeFor(21));
    ASSERT_EQ(peerHit.status, EJitCompileOrGetStatus::CacheHit);
    EXPECT_EQ(peerHit.fnPtr, codeFor(21)); // same pointer cross-core
    ASSERT_EQ(prepare.cores.size(), 1u);
    EXPECT_EQ(prepare.cores[0], 9u);
    peer.releaseRead(peerHit.bucketIndex);
  }
}

TEST_F(SharedTaskPoolTest, PeerPreparesExecutePermissionOncePerCore) {
  PrepareLog prepare;
  EJitSharedTaskPool owner;
  owner.setPrepareCodeCallback(&mockPrepareCode, &prepare);
  bringUpOwner(owner, /*codeSharing=*/true);

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(22, nullptr, 0, codeFor(22)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  // Owner code was prepared by its compile/lookup path before publication.
  auto ownerHit = owner.compileOrGet(22, nullptr, 0, codeFor(22));
  ASSERT_EQ(ownerHit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(ownerHit.bucketIndex);
  EXPECT_TRUE(prepare.cores.empty());

  EJitCoreId::setCurrentForTest(21);
  auto firstPeerHit = owner.compileOrGet(22, nullptr, 0, codeFor(22));
  ASSERT_EQ(firstPeerHit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(firstPeerHit.bucketIndex);
  ASSERT_EQ(prepare.cores.size(), 1u);
  EXPECT_EQ(prepare.cores[0], 21u);

  // The per-slot mask suppresses repeated enable_ex work on the same core.
  auto secondPeerHit = owner.compileOrGet(22, nullptr, 0, codeFor(22));
  ASSERT_EQ(secondPeerHit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(secondPeerHit.bucketIndex);
  EXPECT_EQ(prepare.cores.size(), 1u);

  // A different core has its own translation context and prepares separately.
  EJitCoreId::setCurrentForTest(22);
  auto otherPeerHit = owner.compileOrGet(22, nullptr, 0, codeFor(22));
  ASSERT_EQ(otherPeerHit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(otherPeerHit.bucketIndex);
  ASSERT_EQ(prepare.cores.size(), 2u);
  EXPECT_EQ(prepare.cores[1], 22u);
}

TEST_F(SharedTaskPoolTest, PeerPrepareFailureCleanlyFallsBack) {
  PrepareLog prepare;
  prepare.succeed = false;
  EJitSharedTaskPool owner;
  owner.setPrepareCodeCallback(&mockPrepareCode, &prepare);
  bringUpOwner(owner, /*codeSharing=*/true);

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(23, nullptr, 0, codeFor(23)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  void *fallback = reinterpret_cast<void *>(0xDEADBEEFull);
  EJitCoreId::setCurrentForTest(21);
  auto peerHit = owner.compileOrGet(23, nullptr, 0, fallback);
  EXPECT_EQ(peerHit.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_EQ(peerHit.fnPtr, fallback);
  EXPECT_FALSE(peerHit.hasReadToken);
  EXPECT_TRUE(peerHit.readyButNotShareable);
  EXPECT_EQ(state_->counters.executePrepareFailed.loadAcquire(), 1u);
}

//===----------------------------------------------------------------------===//
// 13/ FreeCode/publish: overwriting an identity releases the old pointer.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, PublishOverwriteReleasesOldCode) {
  ReleaseLog log;
  SeqCompiler seq;
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompileSeq, &seq);
  owner.setReleaser(&mockRelease, &log);
  owner.setCodeSharingEnabled(true);
  owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.setInstanceEnabled(0, 1, true);

  EJitDimPair d0[1] = {dim(0, 1)};
  // First publish for (func=30, (0,1)).
  ASSERT_EQ(owner.compileOrGet(30, d0, 1, codeFor(30)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());
  auto first = owner.compileOrGet(30, d0, 1, codeFor(30));
  ASSERT_EQ(first.status, EJitCompileOrGetStatus::CacheHit);
  void *firstPtr = first.fnPtr;
  owner.releaseRead(first.bucketIndex);
  // Toggle off then on: version advances by two but identity is unchanged, so a
  // re-compile (new address) overwrites the SAME slot and releases the old.
  owner.setInstanceEnabled(0, 1, false);
  owner.setInstanceEnabled(0, 1, true);
  ASSERT_EQ(owner.compileOrGet(30, d0, 1, codeFor(30)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());
  ASSERT_EQ(log.freed.size(), 1u);
  EXPECT_EQ(log.freed[0], firstPtr); // old address freed on recompile
}

//===----------------------------------------------------------------------===//
// 14/ Big-endian field semantics: values round-trip by field, never byte-swap.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, BigEndianFieldSemantics) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  owner.setInstanceEnabled(0x03u, 0x0005u, true);
  owner.setInstanceEnabled(0x07u, 0x00FFu, true);
  EJitCoreId::setCurrentForTest(0);
  const uint32_t func = 0x0ABCu; // distinct bytes, still < max func index
  EJitDimPair d0[2] = {dim(0x03u, 0x0005u), dim(0x07u, 0x00FFu)};
  ASSERT_EQ(owner.compileOrGet(func, d0, 2, codeFor(func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());

  // Inspect the published slot directly: each field equals exactly what we put
  // in (correct on aarch64_be precisely because access is by-field, by-value).
  uint64_t key = 0; // recompute the same identity hash the pool uses
  key = static_cast<uint64_t>(func);
  for (uint32_t i = 0; i < 2; ++i) {
    key ^= (static_cast<uint64_t>(d0[i].dimType) << 32) |
           static_cast<uint64_t>(d0[i].instanceId);
    key *= 0x9e3779b97f4a7c15ULL;
  }
  uint32_t bucket = static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
  const EJitSharedCacheBucket &B = state_->buckets[bucket];
  bool found = false;
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    const EJitSharedCacheSlot &Slot = B.slots[s];
    if (Slot.state.loadAcquire() !=
        static_cast<uint32_t>(EJitSharedSlotState::Ready))
      continue;
    if (Slot.funcIndex != func)
      continue;
    found = true;
    EXPECT_EQ(Slot.numDims, 2u);
    EXPECT_EQ(Slot.dims[0].dimType, 0x03u);
    EXPECT_EQ(Slot.dims[0].instanceId, 0x0005u);
    EXPECT_EQ(Slot.dims[1].dimType, 0x07u);
    EXPECT_EQ(Slot.dims[1].instanceId, 0x00FFu);
    EXPECT_EQ(Slot.identityHash, key);
    EXPECT_EQ(reinterpret_cast<void *>(Slot.fnPtr.loadAcquire()),
              codeFor(func));
    break;
  }
  EXPECT_TRUE(found);
}

//===----------------------------------------------------------------------===//
// ABI mismatch on a Ready blob is refused.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, AbiMismatchRefused) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  state_->magic = kEJitSharedAbiMagic + 1; // corrupt the header
  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AbiMismatch);
}

//===----------------------------------------------------------------------===//
// Instance-disabled producers fall back and never enqueue.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, DisabledInstanceFallsBackNoEnqueue) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);
  EJitDimPair d0[1] = {dim(2, 5)};
  EXPECT_TRUE(owner.setInstanceEnabled(2, 5, true));
  EXPECT_TRUE(owner.setInstanceEnabled(2, 5, false));
  auto r = owner.compileOrGet(50, d0, 1, codeFor(50));
  EXPECT_EQ(r.status, EJitCompileOrGetStatus::InstanceDisabled);
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 0u);
  EXPECT_EQ(d.instanceDisabled, 1u);
}

//===----------------------------------------------------------------------===//
// Round-2 review fixes (spec §11).
//===----------------------------------------------------------------------===//

// An idle-hook "script" that drives the REAL runWorkerLoop deterministically
// with no thread: it counts the worker's yields, and on a controlled schedule
// publishes Ready + enqueues a request, then (after the request is consumed)
// transitions to Stopping so the loop exits. This proves the SAME worker yields
// while Initializing, survives to Ready, consumes, yields on the empty queue,
// and exits on Stopping — without any spin-budget early exit.
struct IdleScript {
  EJitSharedTaskPool *pool;
  EJitSharedTaskPoolState *st;
  void *fallback;
  int idleCalls = 0;
  int initializingYields = 0;
  bool readyPublished = false;
  bool stopped = false;
};
void scriptedIdle(void *ctx) {
  auto *s = static_cast<IdleScript *>(ctx);
  ++s->idleCalls;
  uint32_t st = s->st->initState.loadAcquire();
  if (st == static_cast<uint32_t>(EJitSharedInitState::Initializing)) {
    ++s->initializingYields;
    // After a few yields proving the worker did NOT exit, the "owner" publishes
    // Ready and enqueues one request.
    if (s->initializingYields == 3) {
      s->st->initState.storeRelease(
          static_cast<uint32_t>(EJitSharedInitState::Ready));
      s->readyPublished = true;
      s->pool->compileOrGet(7, nullptr, 0, s->fallback); // enqueue (now Ready)
    }
    return;
  }
  // Ready + empty queue (the request was already consumed): stop the loop.
  if (s->readyPublished && !s->stopped) {
    s->st->initState.storeRelease(
        static_cast<uint32_t>(EJitSharedInitState::Stopping));
    s->stopped = true;
  }
}

// A compiler that flips the shared state to Stopping once it has compiled a
// target number of requests, so a REAL runWorkerLoop drains then exits.
struct StopAfterCtx {
  EJitSharedTaskPoolState *st;
  int remaining;
};
bool compileThenStopAfter(void *ctx, const EJitCompileRequest &req,
                          void **outFn) {
  auto *c = static_cast<StopAfterCtx *>(ctx);
  *outFn = codeFor(req.funcIndex);
  if (--c->remaining == 0)
    c->st->initState.storeRelease(
        static_cast<uint32_t>(EJitSharedInitState::Stopping));
  return true;
}

// A compiler that bumps the shared generation mid-compile (models an owner
// re-init landing during compilation).
struct GenBumpCtx {
  EJitSharedTaskPoolState *st;
};
bool compileThenBumpGeneration(void *ctx, const EJitCompileRequest &req,
                               void **outFn) {
  auto *c = static_cast<GenBumpCtx *>(ctx);
  *outFn = codeFor(req.funcIndex);
  c->st->generation.fetchAdd(1);
  return true;
}

// A worker stopper that records the init state it observed when called, to
// prove ownerShutdown signals Stopping BEFORE the join.
struct StopObserver {
  EJitSharedTaskPoolState *st;
  uint32_t stateAtStop = 0xFFFFFFFFu;
  int calls = 0;
};
void observingStop(void *ctx) {
  auto *o = static_cast<StopObserver *>(ctx);
  o->stateAtStop = o->st->initState.loadAcquire();
  ++o->calls;
}

// 七.1 — the REAL worker state machine: it WAITS on Initializing (never exits),
// reaches Ready and consumes, and exits on a terminal state. Driven
// step-by-step (workerPollOnce) so it is fully deterministic with no thread.
TEST_F(SharedTaskPoolTest, WorkerStartsWhileInitializingAndWaitsForReady) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool); // state Ready, owner core 0
  EJitCoreId::setCurrentForTest(0);

  // Simulate the SRE task being scheduled before the owner published Ready.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Initializing));
  EXPECT_EQ(pool.workerPollOnce(), EJitWorkerStep::WaitForReady); // NO exit
  EXPECT_EQ(pool.workerPollOnce(), EJitWorkerStep::WaitForReady);
  EXPECT_TRUE(pool.workerWaitedForReady());

  // Owner publishes Ready; the worker now reaches the consume phase.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Ready));
  ASSERT_EQ(pool.compileOrGet(1, nullptr, 0, codeFor(1)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(pool.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_GT(pool.workerConsumeLoops(), 0u);
  EXPECT_EQ(pool.workerPollOnce(), EJitWorkerStep::Idle); // Ready, queue empty

  // Terminal states exit the loop.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Stopping));
  EXPECT_EQ(pool.workerPollOnce(), EJitWorkerStep::Exit);
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Failed));
  EXPECT_EQ(pool.workerPollOnce(), EJitWorkerStep::Exit);
}

// 七.1 (real entry) — the REAL runWorkerLoop, started while the owner is still
// Initializing, YIELDS (does not busy-spin, does not exit early), and the SAME
// worker survives to Ready, consumes the enqueued request, yields on the empty
// queue, and exits on Stopping. Driven by an injected idle hook (no thread, no
// spin budget, no deadlock).
TEST_F(SharedTaskPoolTest, RealWorkerEntrySurvivesInitializingAndConsumes) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool); // runs initSharedStorage (valid ring), state Ready
  EJitCoreId::setCurrentForTest(0);
  IdleScript script{&pool, state_.get(), codeFor(7)};
  pool.setWorkerIdleHook(&scriptedIdle, &script);
  // Simulate the SRE task being scheduled BEFORE the owner published Ready.
  state_->initState.storeRelease(
      static_cast<uint32_t>(EJitSharedInitState::Initializing));
  pool.runWorkerLoop(); // REAL entry; the idle script drives the transitions.

  EXPECT_GE(script.initializingYields,
            3); // yielded (not exited) on Initializing
  EXPECT_GT(pool.workerIdleYields(), 0u); // worker yielded, never busy-spun
  EXPECT_TRUE(pool.workerWaitedForReady());
  EXPECT_GT(pool.workerConsumeLoops(),
            0u); // SAME worker reached Ready+consumed
  EJitSharedDiagnostics d;
  pool.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount,
            1u); // the enqueued request was actually compiled
}

// 七.1 (real entry) — the actual runWorkerLoop reaches Ready, consumes queued
// work, and exits via a controlled Stopping transition (no thread, no
// deadlock).
TEST_F(SharedTaskPoolTest, RealWorkerEntryConsumesThenStops) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  StopAfterCtx sa{state_.get(), 3};
  pool.setCompiler(&compileThenStopAfter, &sa);
  for (uint32_t f = 1; f <= 3; ++f)
    ASSERT_EQ(pool.compileOrGet(f, nullptr, 0, codeFor(f)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
  pool.runWorkerLoop(); // REAL entry: consumes 3 then sees Stopping → exits.
  EXPECT_GE(pool.workerConsumeLoops(), 3u);
  EJitSharedDiagnostics d;
  pool.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount, 3u);
}

// The real worker must not drain a long queue at full speed. After consumed
// work it yields through the injected platform hook, giving heartbeat/business
// tasks scheduler time even when more compile requests remain queued.
TEST_F(SharedTaskPoolTest, RealWorkerThrottlesBetweenConsumedRequests) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  StopAfterCtx sa{state_.get(), 3};
  pool.setCompiler(&compileThenStopAfter, &sa);
  for (uint32_t f = 1; f <= 3; ++f)
    ASSERT_EQ(pool.compileOrGet(f, nullptr, 0, codeFor(f)).status,
              EJitCompileOrGetStatus::EnqueuedPending);

  uint64_t before = pool.workerIdleYields();
  pool.runWorkerLoop();
  EXPECT_GE(pool.workerConsumeLoops(), 3u);
  EXPECT_GT(pool.workerIdleYields(), before);
}

// 七.2 — worker start failure publishes Failed and records the reason.
TEST_F(SharedTaskPoolTest, WorkerStartFailurePublishesFailed) {
  WorkerHooks hooks;
  hooks.failNext = true;
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
  owner.setMode(EJitCompileMode::Async);
  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::OwnerFailed);
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Failed));
  EXPECT_EQ(state_->lastInitError.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitError::WorkerStartFailed));
  EXPECT_EQ(hooks.starts, 0);
}

// 七.3 — the host test build must NOT select the platform core-id path, and the
// (settable) core id actually participates in owner election (not defaulted 0).
TEST_F(SharedTaskPoolTest, PlatformCoreIdBuildSelection) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_PLATFORM
  FAIL() << "host unit-test build must not define "
            "EJIT_SRE_SHARED_TASKPOOL_PLATFORM";
#else
  EJitSharedTaskPool pool;
  pool.bind(state_.get());
  pool.setCompiler(&mockCompile, nullptr);
  pool.setMode(EJitCompileMode::Async);
  EJitCoreId::setCurrentForTest(7); // a non-zero core wins election
  ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(state_->ownerCoreId.loadAcquire(), 7u); // core id participated
#endif
}

// 七.4 — the configured worker stack-size macro is present and valid (the value
// the freestanding SRE adapter passes to SRE_TaskCreate). The authoritative
// "reaches TaskCreate" check is the freestanding compile in the build phase.
TEST_F(SharedTaskPoolTest, ConfiguredWorkerStackReachesTaskCreate) {
#ifdef EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE
  EXPECT_GT(
      static_cast<unsigned long long>(EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE),
      0ull);
  EXPECT_EQ(EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE % 16u, 0u);
  EXPECT_LE(
      static_cast<unsigned long long>(EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE),
      0xFFFFFFFFull);
#else
  FAIL() << "EJIT_SRE_TASKPOOL_WORKER_STACK_SIZE must be defined by the build";
#endif
}

// 七.5 — code sharing OFF: a non-owner core hits a Ready entry but gets NO
// fnPtr and does NOT re-enqueue (no recompile churn).
TEST_F(SharedTaskPoolTest, CodeSharingOffRejectsPeerWithoutReenqueue) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/false);
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(60, nullptr, 0, codeFor(60)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  EJitCoreId::setCurrentForTest(9);
  EJitSharedDiagnostics before;
  peer.getDiagnostics(before);
  auto r = peer.compileOrGet(60, nullptr, 0, codeFor(60));
  EXPECT_FALSE(r.hasReadToken);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_EQ(r.fnPtr, codeFor(60)); // fallback
  EJitSharedDiagnostics after;
  peer.getDiagnostics(after);
  EXPECT_EQ(after.queueDepth, before.queueDepth); // NOT re-enqueued
  EXPECT_EQ(after.pendingCount, before.pendingCount);
}

// 七.6 — code sharing ON: a non-owner core gets the SAME fnPtr + read token,
// and the owner can read its own pointer too.
TEST_F(SharedTaskPoolTest, CodeSharingOnReturnsSamePointerToPeer) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(61, nullptr, 0, codeFor(61)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());

  auto ownerHit = owner.compileOrGet(61, nullptr, 0, codeFor(61));
  EXPECT_EQ(ownerHit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(ownerHit.fnPtr, codeFor(61));
  if (ownerHit.hasReadToken)
    owner.releaseRead(ownerHit.bucketIndex);

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  EJitCoreId::setCurrentForTest(9);
  auto peerHit = peer.compileOrGet(61, nullptr, 0, codeFor(61));
  ASSERT_EQ(peerHit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(peerHit.fnPtr, codeFor(61)); // same pointer cross-core
  EXPECT_TRUE(peerHit.hasReadToken);
  peer.releaseRead(peerHit.bucketIndex);
}

// 七.7 — a request whose generation has been superseded is dropped at the
// worker (no compile, no publish) and its OWN-generation dedup slot is
// released.
TEST_F(SharedTaskPoolTest, StaleQueuedGenerationDropped) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  uint32_t g = state_->generation.loadAcquire();
  EXPECT_EQ(state_->inFlight[5].loadAcquire(), g); // dedup slot holds gen g
  // Bump the generation as an owner re-init would (without resetting the
  // queue).
  state_->generation.storeRelease(g + 1);
  EXPECT_TRUE(owner.pollOne()); // worker pops the stale request → drops it
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount, 0u); // nothing published
  EXPECT_EQ(d.compileFailed, 1u);
  EXPECT_EQ(state_->inFlight[5].loadAcquire(), 0u); // gen-g slot released
}

// 七.8 — a generation change DURING compilation drops the result (released, not
// published).
TEST_F(SharedTaskPoolTest, GenerationChangesDuringCompileDropsResult) {
  ReleaseLog log;
  GenBumpCtx gctx{state_.get()};
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&compileThenBumpGeneration, &gctx);
  owner.setReleaser(&mockRelease, &log);
  owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  ASSERT_EQ(owner.compileOrGet(6, nullptr, 0, codeFor(6)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne()); // compiles, bumps gen → checkpoint 2 rejects
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.cacheReadyCount, 0u);
  EXPECT_EQ(d.compileFailed, 1u);
  ASSERT_EQ(log.freed.size(), 1u);
  EXPECT_EQ(log.freed[0], codeFor(6)); // stale result released
}

// 七.9 — a stale (older-generation) worker clearing its dedup slot must NOT
// clear a newer generation's in-flight slot for the same funcIndex.
TEST_F(SharedTaskPoolTest, StaleWorkerCannotClearNewGenerationDedup) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);
  // Gen g1 producer enqueues funcIndex 7 (dedup slot := g1).
  ASSERT_EQ(owner.compileOrGet(7, nullptr, 0, codeFor(7)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  uint32_t g1 = state_->generation.loadAcquire();
  // Simulate an owner re-init: dedup slot reset + generation bumped to g2.
  state_->inFlight[7].storeRelease(0);
  state_->generation.storeRelease(g1 + 1);
  // New gen-g2 producer re-claims funcIndex 7 (dedup slot := g2).
  ASSERT_EQ(owner.compileOrGet(7, nullptr, 0, codeFor(7)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(state_->inFlight[7].loadAcquire(), g1 + 1);
  // The stale gen-g1 request is first in the queue; the worker pops + drops it,
  // calling dedupClear(7, g1) = CAS(g1->0) which FAILS against the g2 value.
  EXPECT_TRUE(owner.pollOne());
  EXPECT_EQ(state_->inFlight[7].loadAcquire(),
            g1 + 1); // new-gen slot preserved
}

// 七.10 — destroying a non-owner peer must NOT stop the owner's worker.
TEST_F(SharedTaskPoolTest, PeerDestructionDoesNotStopOwnerWorker) {
  WorkerHooks hooks;
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
  owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(hooks.starts, 1);
  {
    EJitSharedTaskPool peer;
    peer.bind(state_.get());
    EJitCoreId::setCurrentForTest(1);
    ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
    EXPECT_FALSE(peer.isOwner());
    peer.ownerShutdown(); // non-owner: must be a no-op
  }
  EXPECT_EQ(hooks.stops, 0); // owner worker NOT stopped by the peer
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Ready));
  EXPECT_TRUE(owner.isOwner());
  owner.ownerShutdown();
  EXPECT_EQ(hooks.stops, 1);
}

// 七.11 — owner shutdown signals Stopping and JOINS the worker BEFORE returning
// the shared state to Uninitialized (so private ORC/driver teardown is safe).
TEST_F(SharedTaskPoolTest,
       OwnerShutdownStopsWorkerBeforePrivateContextDestruction) {
  StopObserver obs{state_.get()};
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setWorkerHooks(&startOkIgnoreCtx, &observingStop, &obs);
  owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.ownerShutdown();
  EXPECT_EQ(obs.calls, 1);
  // The worker stop (join) observed Stopping: the worker was told to stop
  // BEFORE the join, never after the state was already torn down.
  EXPECT_EQ(obs.stateAtStop,
            static_cast<uint32_t>(EJitSharedInitState::Stopping));
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
  EXPECT_FALSE(owner.isOwner());
}

//===----------------------------------------------------------------------===//
// Round-3 review fixes (spec §11): registration fingerprint consistency.
//===----------------------------------------------------------------------===//

// 三 — a peer whose registration fingerprint matches the owner's attaches.
TEST_F(SharedTaskPoolTest, RegistrationFingerprintMatchAttaches) {
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setMode(EJitCompileMode::Async);
  owner.setRegistrationFingerprint(0xA1B2C3D4E5F60718ull);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(state_->registrationFingerprint.loadAcquire(),
            0xA1B2C3D4E5F60718ull);

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  peer.setRegistrationFingerprint(0xA1B2C3D4E5F60718ull); // same mapping
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
}

// 三 — a peer whose registration fingerprint differs is cleanly rejected and
// must NOT submit requests against a mismatched mapping.
TEST_F(SharedTaskPoolTest, RegistrationFingerprintMismatchRejected) {
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setMode(EJitCompileMode::Async);
  owner.setRegistrationFingerprint(0x1111111111111111ull);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  peer.setRegistrationFingerprint(0x2222222222222222ull); // divergent mapping
  EJitCoreId::setCurrentForTest(2);
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::FingerprintMismatch);
  EXPECT_FALSE(peer.isOwner());
}

//===----------------------------------------------------------------------===//
// 4K per-core shared code execute-permission preparation.
//===----------------------------------------------------------------------===//

// 1/ Single-page function: the peer splits its pool ONCE and seals ONE page.
TEST_F(SharedTaskPoolTest, FourKPeerSplitsOnceSealsSinglePage) {
  FourKLog fourK;
  RangeCtx range; // codeStart 0x40000000, size 64 -> one 4K page
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, codeFor(1));
  EXPECT_TRUE(hit.hasReadToken);
  owner.releaseRead(hit.bucketIndex);

  ASSERT_EQ(fourK.splits.size(), 1u);
  EXPECT_EQ(fourK.splits[0].first, range.poolBase);
  EXPECT_EQ(fourK.splits[0].second, 3u);
  ASSERT_EQ(fourK.seals.size(), 1u);
  EXPECT_EQ(fourK.seals[0].first, 0x40000000ull);
  EXPECT_EQ(fourK.seals[0].second, 3u);
}

// 2/3 Range spanning two pages with an unaligned start/end: seal BOTH pages.
TEST_F(SharedTaskPoolTest, FourKPeerSealsEveryCoveredPageUnaligned) {
  FourKLog fourK;
  RangeCtx range;
  range.codeStart = 0x40000F00ull; // unaligned, near the end of page 0
  range.codeSize = 0x200;          // crosses into page 1
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 2);

  EJitCoreId::setCurrentForTest(5);
  auto hit = owner.compileOrGet(2, nullptr, 0, codeFor(2));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);

  ASSERT_EQ(fourK.seals.size(), 2u);
  EXPECT_EQ(fourK.seals[0].first, 0x40000000ull); // page-aligned down
  EXPECT_EQ(fourK.seals[1].first, 0x40001000ull); // page-aligned up
}

// 4/ Repeated hit on the SAME core does NOT re-split or re-seal (memoized).
TEST_F(SharedTaskPoolTest, FourKSameCoreRepeatHitNoRework) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto h1 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h1.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h1.bucketIndex);
  auto h2 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h2.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h2.bucketIndex);

  EXPECT_EQ(fourK.splits.size(), 1u); // not repeated
  EXPECT_EQ(fourK.seals.size(), 1u);  // not repeated
}

// 5/ A second function in the SAME pool: no re-split, but seals its OWN pages.
TEST_F(SharedTaskPoolTest, FourKSecondFuncSamePoolNoResplit) {
  FourKLog fourK;
  RangeCtx range;
  range.codeStart = 0x40000000ull; // func 1 -> page 0
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);
  range.codeStart = 0x40010000ull; // func 2 -> a different page, SAME pool
  publish(owner, 2);

  EJitCoreId::setCurrentForTest(3);
  auto h1 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h1.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h1.bucketIndex);
  auto h2 = owner.compileOrGet(2, nullptr, 0, codeFor(2));
  ASSERT_EQ(h2.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h2.bucketIndex);

  EXPECT_EQ(fourK.splits.size(), 1u); // split once for the shared pool
  ASSERT_EQ(fourK.seals.size(), 2u);  // but each func sealed its own page
  EXPECT_EQ(fourK.seals[0].first, 0x40000000ull);
  EXPECT_EQ(fourK.seals[1].first, 0x40010000ull);
}

// 6/ Two peer cores each split + seal in their own translation context.
TEST_F(SharedTaskPoolTest, FourKTwoPeerCoresEachPrepare) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto h3 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h3.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h3.bucketIndex);
  EJitCoreId::setCurrentForTest(4);
  auto h4 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h4.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h4.bucketIndex);

  ASSERT_EQ(fourK.splits.size(), 2u);
  EXPECT_EQ(fourK.splits[0].second, 3u);
  EXPECT_EQ(fourK.splits[1].second, 4u);
  ASSERT_EQ(fourK.seals.size(), 2u);
  EXPECT_EQ(fourK.seals[0].second, 3u);
  EXPECT_EQ(fourK.seals[1].second, 4u);
}

// 7/ The owner never runs peer preparation (it sealed the code itself).
TEST_F(SharedTaskPoolTest, FourKOwnerDoesNotPeerPrepare) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(0); // owner
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);
  EXPECT_TRUE(fourK.splits.empty());
  EXPECT_TRUE(fourK.seals.empty());
}

// 8/ Split failure: clean fallback, NO ready bit, NO seal; a later retry can
// succeed once the platform recovers.
TEST_F(SharedTaskPoolTest, FourKSplitFailureFallsBackAndRetries) {
  FourKLog fourK;
  fourK.splitOk = false;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto miss = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(miss.readyButNotShareable);
  EXPECT_FALSE(miss.hasReadToken);
  EXPECT_TRUE(fourK.seals.empty()); // never sealed after a failed split
  EXPECT_EQ(state_->counters.executePrepareFailed.loadAcquire(), 1u);
  // The per-core split-done bit must NOT be set, so a retry re-attempts.
  fourK.splitOk = true;
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);
  EXPECT_EQ(fourK.splits.size(), 2u); // re-attempted
  EXPECT_EQ(fourK.seals.size(), 1u);
}

// 9/ A mid-range seal failure: clean fallback, the slot is NOT marked ready for
// this core (the next hit re-prepares).
TEST_F(SharedTaskPoolTest, FourKMidSealFailureFallsBack) {
  FourKLog fourK;
  fourK.failSealAtIndex = 1; // first page seals, second fails
  RangeCtx range;
  range.codeStart = 0x40000F00ull;
  range.codeSize = 0x200; // two pages
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto miss = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(miss.readyButNotShareable);
  EXPECT_EQ(fourK.seals.size(), 2u); // attempted both, second failed
  EXPECT_EQ(state_->counters.executePrepareFailed.loadAcquire(), 1u);
  // Not memoized: a retry (now succeeding) re-seals.
  fourK.failSealAtIndex = -1;
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);
}

// 10/ Missing / zero-size / overflowing range: clean fallback, no platform
// call.
TEST_F(SharedTaskPoolTest, FourKMissingRangeFallsBack) {
  FourKLog fourK;
  RangeCtx range;
  range.provide = false; // owner publishes NO range (codeSize stays 0)
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);
  // Slot carries no range metadata.
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->codeSize, 0ull);

  EJitCoreId::setCurrentForTest(3);
  auto miss = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(miss.readyButNotShareable);
  EXPECT_TRUE(fourK.splits.empty());
  EXPECT_TRUE(fourK.seals.empty());
}

// 10b/ A range whose code lies outside its pool is rejected (no seal).
TEST_F(SharedTaskPoolTest, FourKOutOfPoolRangeRejected) {
  FourKLog fourK;
  RangeCtx range;
  range.codeStart = 0x40000000ull;
  range.codeSize = 0x300000ull; // 3 MiB > 2 MiB pool: code escapes the pool
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto miss = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_EQ(miss.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(miss.readyButNotShareable);
  EXPECT_TRUE(fourK.seals.empty());
}

// 11/ Pool-readiness table full: a peer hitting an untracked pool cleanly falls
// back rather than overflow the fixed table.
TEST_F(SharedTaskPoolTest, FourKPoolTableFullFallsBack) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  // Publish kEJitSharedPoolSlots + 1 functions, each in a DISTINCT 2MiB pool.
  const uint32_t N = kEJitSharedPoolSlots + 1;
  for (uint32_t f = 0; f < N; ++f) {
    range.poolBase = 0x40000000ull + static_cast<uint64_t>(f) * 0x200000ull;
    range.codeStart = range.poolBase;
    range.codeSize = 64;
    publish(owner, f);
  }
  // A single peer core touches every pool; the table holds only
  // kEJitSharedPoolSlots, so exactly one distinct pool cannot be tracked and
  // that hit cleanly falls back.
  EJitCoreId::setCurrentForTest(3);
  uint32_t fallbacks = 0, hits = 0;
  for (uint32_t f = 0; f < N; ++f) {
    auto r = owner.compileOrGet(f, nullptr, 0, codeFor(f));
    if (r.status == EJitCompileOrGetStatus::CacheHit) {
      ++hits;
      owner.releaseRead(r.bucketIndex);
    } else {
      EXPECT_EQ(r.status, EJitCompileOrGetStatus::OffMode);
      EXPECT_TRUE(r.readyButNotShareable);
      ++fallbacks;
    }
  }
  EXPECT_EQ(hits, kEJitSharedPoolSlots);
  EXPECT_EQ(fallbacks, 1u);
}

// 12/ Core id beyond the 64-bit memo width: no UB, and (documented) the split +
// seal re-run on every hit since the per-core bit cannot be recorded.
TEST_F(SharedTaskPoolTest, FourKOutOfRangeCoreRePreparesEachHit) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(100); // >= kEJitSharedMaxMemoCores
  auto h1 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(h1.fnPtr, codeFor(1));
  owner.releaseRead(h1.bucketIndex);
  auto h2 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h2.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h2.bucketIndex);
  EXPECT_EQ(fourK.splits.size(), 2u); // re-prepared (no memoization)
  EXPECT_EQ(fourK.seals.size(), 2u);
}

// 13/ Slot replaced (fnPtr changed) DURING preparation: the prepared pointer is
// re-validated and the now-stale pointer is NOT returned.
TEST_F(SharedTaskPoolTest, FourKSlotReplacedDuringPrepareNotReturned) {
  static EJitSharedCacheSlot *gSlot = nullptr;
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);
  gSlot = findReadySlot(1);
  ASSERT_NE(gSlot, nullptr);
  // During the (only) seal call — after the bucket read lock was released —
  // overwrite the slot's fnPtr to model a concurrent same-generation republish.
  fourK.raceAtSealIndex = 0;
  fourK.raceCtx = nullptr;
  fourK.raceHook = [](void *) {
    gSlot->fnPtr.storeRelease(
        reinterpret_cast<uintptr_t>(reinterpret_cast<void *>(0xBADC0DEull)));
  };

  EJitCoreId::setCurrentForTest(3);
  auto r = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_EQ(r.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_FALSE(r.hasReadToken);
}

// 14/ Generation changed DURING preparation: the prepared pointer is discarded.
TEST_F(SharedTaskPoolTest, FourKGenerationChangeDuringPrepareNotReturned) {
  static EJitSharedTaskPoolState *gState = nullptr;
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);
  gState = state_.get();
  fourK.raceAtSealIndex = 0;
  fourK.raceHook = [](void *) {
    gState->generation.storeRelease(gState->generation.loadAcquire() + 1);
  };

  EJitCoreId::setCurrentForTest(3);
  auto r = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  EXPECT_EQ(r.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(r.readyButNotShareable);
}

// Shared ABI layout: the slot carries the executable range as fixed-width,
// naturally-aligned scalars (read back by value — endian-safe), the pool-split
// table is POD, dump slots use dynamic payload pointers, and each bucket
// carries the NO_RECLAIM seqlock publishSeq word.
TEST_F(SharedTaskPoolTest, FourKAbiVersionAndRangeFieldSemantics) {
  EXPECT_EQ(kEJitSharedAbiVersion, 10u);
  EXPECT_TRUE(std::is_standard_layout<EJitSharedPoolSplit>::value);
  EXPECT_TRUE(std::is_trivially_destructible<EJitSharedPoolSplit>::value);
  EXPECT_TRUE(
      std::is_trivially_default_constructible<EJitSharedPoolSplit>::value);

  FourKLog fourK;
  RangeCtx range;
  range.codeStart = 0x40012340ull;
  range.codeSize = 0x456;
  range.poolBase = 0x40000000ull;
  range.poolSize = 0x200000ull;
  range.poolId = 7;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  EXPECT_EQ(state_->abiVersion, kEJitSharedAbiVersion);
  publish(owner, 1);

  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->codeStart, 0x40012340ull);
  EXPECT_EQ(slot->codeSize, 0x456ull);
  EXPECT_EQ(slot->poolBase, 0x40000000ull);
  EXPECT_EQ(slot->poolSize, 0x200000ull);
  EXPECT_EQ(slot->poolId, 7u);
  EXPECT_EQ(slot->rangeReserved, 0u);
  // PGO fields: zero on a Baseline publish (PGO off).
  EXPECT_EQ(slot->hitCount.loadRelaxed(), 0u);
}

TEST_F(SharedTaskPoolTest, DumpDynamicPayloadsClearedOnInit) {
  state_->magic = kEJitSharedAbiMagic;
  state_->abiVersion = kEJitSharedAbiVersion;
  state_->structSize = sizeof(EJitSharedTaskPoolState);

  char *IR = static_cast<char *>(std::malloc(8));
  char *ASM = static_cast<char *>(std::malloc(8));
  ASSERT_NE(IR, nullptr);
  ASSERT_NE(ASM, nullptr);
  state_->dump.slots[0].valid.storeRelaxed(1);
  state_->dump.slots[0].truncated.storeRelaxed(7);
  state_->dump.slots[0].nameLen = 4;
  state_->dump.slots[0].irPtr = reinterpret_cast<uintptr_t>(IR);
  state_->dump.slots[0].asmPtr = reinterpret_cast<uintptr_t>(ASM);
  state_->dump.slots[0].irSize = 7;
  state_->dump.slots[0].asmSize = 7;
  state_->dump.slots[0].keyHi = 0x12;
  state_->dump.slots[0].keyLo = 0x34;
  state_->dump.slots[0].name[0] = 'f';
  state_->dump.nextSlot = 1;

  EJitSharedTaskPool owner;
  bringUpOwner(owner);

  EXPECT_EQ(state_->dump.slots[0].valid.loadRelaxed(), 0u);
  EXPECT_EQ(state_->dump.slots[0].truncated.loadRelaxed(), 0u);
  EXPECT_EQ(state_->dump.slots[0].nameLen, 0u);
  EXPECT_EQ(state_->dump.slots[0].irPtr, 0u);
  EXPECT_EQ(state_->dump.slots[0].asmPtr, 0u);
  EXPECT_EQ(state_->dump.slots[0].irSize, 0u);
  EXPECT_EQ(state_->dump.slots[0].asmSize, 0u);
  EXPECT_EQ(state_->dump.slots[0].keyHi, 0u);
  EXPECT_EQ(state_->dump.slots[0].keyLo, 0u);
  EXPECT_EQ(state_->dump.slots[0].name[0], 0);
  EXPECT_EQ(state_->dump.nextSlot, 0u);
}

// 18/ Code-sharing OFF in 4K mode: a non-owner cleanly rejects and triggers NO
// platform split/seal call at all.
TEST_F(SharedTaskPoolTest, FourKCodeSharingOffMakesNoPlatformCall) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(false); // capability OFF
  owner.setSealMode(true);
  owner.setCodeRangeProvider(&mockCodeRange, &range);
  owner.setSplitPoolCallback(&mockSplitPool, &fourK);
  owner.setSealPageCallback(&mockSealPage, &fourK);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_EQ(r.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_EQ(r.fnPtr, fallback);
  EXPECT_TRUE(fourK.splits.empty());
  EXPECT_TRUE(fourK.seals.empty());
}

// 19/ Re-init scrubs EVERY ABI-v5 field. A re-initialization runs over the same
// shared blob, so initSharedStorage must clear the per-pool split table AND the
// per-slot executable-range metadata (plus fnPtr / executableCoreMask / state).
// Pre-stain the raw blob with garbage and prove the owner election zeroes it.
TEST_F(SharedTaskPoolTest, FourKReinitScrubsStaleV5Fields) {
  // Stain the per-pool split table.
  for (uint32_t i = 0; i < kEJitSharedPoolSlots; ++i) {
    state_->poolSplits[i].poolBase.storeRelaxed(0xDEADBEEFull);
    state_->poolSplits[i].splitDoneMask.storeRelaxed(~uint64_t(0));
    state_->poolSplits[i].splitPreparingMask.storeRelaxed(~uint64_t(0));
  }
  // Stain every cache slot's range + sharing fields and mark it (falsely)
  // Ready.
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = state_->buckets[b].slots[s];
      Slot.state.storeRelaxed(
          static_cast<uint32_t>(EJitSharedSlotState::Ready));
      Slot.fnPtr.storeRelaxed(0xBADC0DEull);
      Slot.executableCoreMask.storeRelaxed(~uint64_t(0));
      Slot.codeStart = 0x1111;
      Slot.codeSize = 0x2222;
      Slot.poolBase = 0x3333;
      Slot.poolSize = 0x4444;
      Slot.poolId = 0x5555;
      Slot.rangeReserved = 0x6666;
    }

  // init-state was left Uninitialized, so owner election runs
  // initSharedStorage.
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);

  for (uint32_t i = 0; i < kEJitSharedPoolSlots; ++i) {
    EXPECT_EQ(state_->poolSplits[i].poolBase.loadRelaxed(), 0u);
    EXPECT_EQ(state_->poolSplits[i].splitDoneMask.loadRelaxed(), 0u);
    EXPECT_EQ(state_->poolSplits[i].splitPreparingMask.loadRelaxed(), 0u);
  }
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = state_->buckets[b].slots[s];
      EXPECT_EQ(Slot.state.loadAcquire(),
                static_cast<uint32_t>(EJitSharedSlotState::Empty));
      EXPECT_EQ(Slot.fnPtr.loadRelaxed(), 0u);
      EXPECT_EQ(Slot.executableCoreMask.loadRelaxed(), 0u);
      EXPECT_EQ(Slot.codeStart, 0u);
      EXPECT_EQ(Slot.codeSize, 0u);
      EXPECT_EQ(Slot.poolBase, 0u);
      EXPECT_EQ(Slot.poolSize, 0u);
      EXPECT_EQ(Slot.poolId, 0u);
      EXPECT_EQ(Slot.rangeReserved, 0u);
    }
}

// 20/ A per-core split-done bit recorded under one generation must NOT survive
// a re-init: after the owner tears down and re-initializes (new generation),
// the same peer core re-runs split_2m_to_4k for the rebuilt pool (a stale done
// bit would otherwise let it seal pages that were never split in this
// generation).
TEST_F(SharedTaskPoolTest, FourKReinitForcesPeerToReSplit) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  // Peer core 3 prepares once: records a split + a per-core done bit.
  EJitCoreId::setCurrentForTest(3);
  auto h = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h.bucketIndex);
  ASSERT_EQ(fourK.splits.size(), 1u);

  // Tear down + re-init on the owner core: the whole blob (and split table) is
  // rebuilt under a fresh generation.
  EJitCoreId::setCurrentForTest(0);
  owner.ownerShutdown();
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  for (uint32_t i = 0; i < kEJitSharedPoolSlots; ++i) {
    EXPECT_EQ(state_->poolSplits[i].poolBase.loadRelaxed(), 0u);
    EXPECT_EQ(state_->poolSplits[i].splitDoneMask.loadRelaxed(), 0u);
  }
  publish(owner, 1);

  // The same peer core must split AGAIN for the rebuilt pool.
  EJitCoreId::setCurrentForTest(3);
  auto h2 = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(h2.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(h2.bucketIndex);
  EXPECT_EQ(fourK.splits.size(), 2u); // re-split after re-init
  EXPECT_EQ(fourK.splits[1].second, 3u);
}

//===----------------------------------------------------------------------===//
// tryCacheHit(): the flattened fast cache-hit path the C API drives before the
// compileOrGet slow path. It must preserve every compileOrGet hot-path
// semantic (ordering, counters, read tokens) while NEVER enqueuing/deduping.
//===----------------------------------------------------------------------===//

// 1/ A cache hit is served entirely on the fast path: fnPtr + bucket + a held
//    read token, fastPathTerminal set, and NO enqueue/dedup side effects.
TEST_F(SharedTaskPoolTest, TryCacheHitServesHitWithoutEnqueue) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(0);
  EJitSharedDiagnostics before;
  owner.getDiagnostics(before);

  auto fast = owner.tryCacheHit(1, nullptr, 0);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fast.fnPtr, codeFor(1));
  EXPECT_TRUE(fast.hasReadToken);
  EXPECT_FALSE(fast.readyButNotShareable);
  // A held read token keeps readers > 0 (same ownership contract as
  // compileOrGet — the caller must release through releaseRead).
  EXPECT_GT(state_->buckets[fast.bucketIndex].readers.loadAcquire(), 0u);

  EJitSharedDiagnostics after;
  owner.getDiagnostics(after);
  EXPECT_EQ(after.cacheHits, before.cacheHits + 1);     // counted exactly once
  EXPECT_EQ(after.asyncEnqueues, before.asyncEnqueues); // no enqueue
  EXPECT_EQ(after.queueDepth, 0u);                      // no dedup slot
  EXPECT_EQ(after.pendingCount, 0u);

  owner.releaseRead(fast.bucketIndex);
  EXPECT_EQ(state_->buckets[fast.bucketIndex].readers.loadAcquire(), 0u);
}

// 2/ A true miss is NOT terminal on the fast path (no enqueue), and the slow
//    path (compileOrGet) still enqueues/compiles exactly as before.
TEST_F(SharedTaskPoolTest, TryCacheHitMissFallsThroughToCompileOrGet) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);

  auto fast = owner.tryCacheHit(5, nullptr, 0);
  EXPECT_FALSE(fast.fastPathTerminal); // must fall through
  EXPECT_NE(fast.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_FALSE(fast.hasReadToken);

  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.asyncEnqueues, 0u); // fast path never enqueues
  EXPECT_EQ(d.queueDepth, 0u);
  EXPECT_EQ(d.pendingCount, 0u);

  // The slow path still enqueues the async request unchanged.
  auto slow = owner.compileOrGet(5, nullptr, 0, codeFor(5));
  EXPECT_EQ(slow.status, EJitCompileOrGetStatus::EnqueuedPending);
  owner.getDiagnostics(d);
  EXPECT_EQ(d.asyncEnqueues, 1u);
  EXPECT_EQ(d.queueDepth, 1u);
  EXPECT_EQ(d.pendingCount, 1u);
}

// 3/ A disabled instance returns InstanceDisabled on the fast path and NEVER
//    hands back cached code, even though a Ready entry exists (deactivate must
//    not serve stale code). No enqueue.
TEST_F(SharedTaskPoolTest, TryCacheHitDisabledInstanceReturnsDisabled) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  owner.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(11, d0, 1, codeFor(11)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  // Sanity: while enabled the entry is a real hit.
  auto sane = owner.tryCacheHit(11, d0, 1);
  ASSERT_EQ(sane.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(sane.bucketIndex);

  // Deactivate the instance, then the fast path must reject before the cache.
  owner.setInstanceEnabled(1, 4, false);
  EJitSharedDiagnostics before;
  owner.getDiagnostics(before);

  auto fast = owner.tryCacheHit(11, d0, 1);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::InstanceDisabled);
  EXPECT_EQ(fast.fnPtr, nullptr); // no stale cached code
  EXPECT_FALSE(fast.hasReadToken);

  EJitSharedDiagnostics after;
  owner.getDiagnostics(after);
  EXPECT_EQ(after.instanceDisabled, before.instanceDisabled + 1);
  EXPECT_EQ(after.queueDepth, before.queueDepth); // no enqueue
  EXPECT_EQ(after.asyncEnqueues, before.asyncEnqueues);
}

// 4/ readyButNotShareable: a peer core that may not read the cross-core pointer
//    gets a clean OffMode fallback with readyButNotShareable set, no read
//    token, and NO enqueue/dedup.
TEST_F(SharedTaskPoolTest, TryCacheHitReadyButNotShareableCleanFallback) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/false);
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(20, nullptr, 0, codeFor(20)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  EJitCoreId::setCurrentForTest(9);
  EJitSharedDiagnostics before;
  peer.getDiagnostics(before);

  auto fast = peer.tryCacheHit(20, nullptr, 0);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(fast.readyButNotShareable);
  EXPECT_FALSE(fast.hasReadToken);
  EXPECT_EQ(fast.fnPtr, nullptr);

  EJitSharedDiagnostics after;
  peer.getDiagnostics(after);
  EXPECT_EQ(after.queueDepth, before.queueDepth); // no enqueue/dedup
  EXPECT_EQ(after.asyncEnqueues, before.asyncEnqueues);
  EXPECT_EQ(after.pendingCount, before.pendingCount);
}

// 5/ Mode Off still returns an existing cache hit on the fast path — the cache
//    lookup is ordered ahead of the Off check, matching compileOrGet.
TEST_F(SharedTaskPoolTest, TryCacheHitServesHitEvenWhenModeOff) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true); // comes up Async
  publish(owner, 30);

  // Flip the cross-core mode to Off after the entry is Ready.
  owner.setSharedMode(EJitCompileMode::Off);
  EXPECT_EQ(owner.getSharedMode(), EJitCompileMode::Off);

  EJitCoreId::setCurrentForTest(0);
  auto fast = owner.tryCacheHit(30, nullptr, 0);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fast.fnPtr, codeFor(30));
  EXPECT_TRUE(fast.hasReadToken);
  owner.releaseRead(fast.bucketIndex);
}

//===----------------------------------------------------------------------===//
// Fixed-dimension fast cache-hit entries (tryCacheHit0D..4D): the unrolled
// front-halves the C ABI ejit_taskpool_compile_or_get_Nd entries drive. Each
// must match the generic tryCacheHit() semantics for the matching numDims.
//===----------------------------------------------------------------------===//

// 0D/1D/2D/3D/4D cache hit: fnPtr + bucket + read token, no enqueue/dedup.
TEST_F(SharedTaskPoolTest, FixedDimEntriesServeCacheHit) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(0, 1, true);
  owner.setInstanceEnabled(1, 2, true);
  owner.setInstanceEnabled(2, 3, true);
  owner.setInstanceEnabled(3, 4, true);

  auto publishDims = [&](uint32_t fi, const EJitDimPair *d, uint32_t n) {
    ASSERT_EQ(owner.compileOrGet(fi, d, n, codeFor(fi)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(owner.pollOne());
  };

  // 0D
  publish(owner, 1);
  auto h0 = owner.tryCacheHit0D(1);
  EXPECT_TRUE(h0.fastPathTerminal);
  EXPECT_EQ(h0.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(h0.fnPtr, codeFor(1));
  EXPECT_TRUE(h0.hasReadToken);
  EXPECT_GT(state_->buckets[h0.bucketIndex].readers.loadAcquire(), 0u);
  owner.releaseRead(h0.bucketIndex);

  // 1D
  EJitDimPair d1[1] = {dim(0, 1)};
  publishDims(2, d1, 1);
  auto h1 = owner.tryCacheHit1D(2, 0, 1);
  ASSERT_EQ(h1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(h1.fnPtr, codeFor(2));
  EXPECT_TRUE(h1.hasReadToken);
  owner.releaseRead(h1.bucketIndex);

  // 2D
  EJitDimPair d2[2] = {dim(0, 1), dim(1, 2)};
  publishDims(3, d2, 2);
  auto h2 = owner.tryCacheHit2D(3, 0, 1, 1, 2);
  ASSERT_EQ(h2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(h2.fnPtr, codeFor(3));
  owner.releaseRead(h2.bucketIndex);

  // 3D
  EJitDimPair d3[3] = {dim(0, 1), dim(1, 2), dim(2, 3)};
  publishDims(4, d3, 3);
  auto h3 = owner.tryCacheHit3D(4, 0, 1, 1, 2, 2, 3);
  ASSERT_EQ(h3.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(h3.fnPtr, codeFor(4));
  owner.releaseRead(h3.bucketIndex);

  // 4D
  EJitDimPair d4[4] = {dim(0, 1), dim(1, 2), dim(2, 3), dim(3, 4)};
  publishDims(5, d4, 4);
  auto h4 = owner.tryCacheHit4D(5, 0, 1, 1, 2, 2, 3, 3, 4);
  ASSERT_EQ(h4.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(h4.fnPtr, codeFor(5));
  owner.releaseRead(h4.bucketIndex);

  // Cache hits do not enqueue/dedup.
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 0u);
  EXPECT_EQ(d.pendingCount, 0u);
}

// A fixed-dim entry matches the generic path exactly for the same identity.
TEST_F(SharedTaskPoolTest, FixedDim1DMatchesGeneric) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(2, 5, true);
  EJitDimPair d1[1] = {dim(2, 5)};
  ASSERT_EQ(owner.compileOrGet(7, d1, 1, codeFor(7)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  auto generic = owner.tryCacheHit(7, d1, 1);
  ASSERT_EQ(generic.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(generic.bucketIndex);
  auto fixed = owner.tryCacheHit1D(7, 2, 5);
  ASSERT_EQ(fixed.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fixed.fnPtr, generic.fnPtr);
  EXPECT_EQ(fixed.bucketIndex, generic.bucketIndex);
  owner.releaseRead(fixed.bucketIndex);
}

// Disabled instance: fixed entry returns InstanceDisabled, no stale code, no
// enqueue — including when only a later dim is disabled.
TEST_F(SharedTaskPoolTest, FixedDimDisabledInstanceReturnsDisabled) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(0, 1, true);
  owner.setInstanceEnabled(1, 2, true);
  EJitDimPair d2[2] = {dim(0, 1), dim(1, 2)};
  ASSERT_EQ(owner.compileOrGet(9, d2, 2, codeFor(9)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  // Disable the SECOND dim only.
  owner.setInstanceEnabled(1, 2, false);
  EJitSharedDiagnostics before;
  owner.getDiagnostics(before);
  auto fast = owner.tryCacheHit2D(9, 0, 1, 1, 2);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::InstanceDisabled);
  EXPECT_EQ(fast.fnPtr, nullptr);
  EXPECT_FALSE(fast.hasReadToken);
  EJitSharedDiagnostics after;
  owner.getDiagnostics(after);
  EXPECT_EQ(after.instanceDisabled, before.instanceDisabled + 1);
  EXPECT_EQ(after.queueDepth, before.queueDepth);
}

// Miss: fixed entry is not terminal and does not enqueue; the slow path still
// enqueues.
TEST_F(SharedTaskPoolTest, FixedDimMissFallsThrough) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(0, 1, true);

  auto fast = owner.tryCacheHit1D(15, 0, 1);
  EXPECT_FALSE(fast.fastPathTerminal);
  EXPECT_NE(fast.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_FALSE(fast.hasReadToken);
  EJitSharedDiagnostics d;
  owner.getDiagnostics(d);
  EXPECT_EQ(d.asyncEnqueues, 0u);
  EXPECT_EQ(d.queueDepth, 0u);

  EJitDimPair d1[1] = {dim(0, 1)};
  EXPECT_EQ(owner.compileOrGet(15, d1, 1, codeFor(15)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  owner.getDiagnostics(d);
  EXPECT_EQ(d.queueDepth, 1u);
}

// Mode Off still serves an existing cache hit via a fixed entry.
TEST_F(SharedTaskPoolTest, FixedDimServesHitEvenWhenModeOff) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  publish(owner, 40);
  owner.setSharedMode(EJitCompileMode::Off);
  EJitCoreId::setCurrentForTest(0);
  auto fast = owner.tryCacheHit0D(40);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fast.fnPtr, codeFor(40));
  EXPECT_TRUE(fast.hasReadToken);
  owner.releaseRead(fast.bucketIndex);
}

// readyButNotShareable: a peer core that may not read the pointer gets a clean
// OffMode fallback with no read token and no enqueue via a fixed entry.
TEST_F(SharedTaskPoolTest, FixedDimReadyButNotShareableCleanFallback) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/false);
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(50, nullptr, 0, codeFor(50)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  EJitCoreId::setCurrentForTest(9);
  auto fast = peer.tryCacheHit0D(50);
  EXPECT_TRUE(fast.fastPathTerminal);
  EXPECT_EQ(fast.status, EJitCompileOrGetStatus::OffMode);
  EXPECT_TRUE(fast.readyButNotShareable);
  EXPECT_FALSE(fast.hasReadToken);
  EXPECT_EQ(fast.fnPtr, nullptr);
}

// The specialized cacheLookupNd behind each fixed entry must agree with the
// generic tryCacheHit()/cacheLookup() for the same identity: same fnPtr, same
// bucket, same CacheHit status across 0D..4D.
TEST_F(SharedTaskPoolTest, FixedDimCacheLookupMatchesGenericAllDims) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(0, 1, true);
  owner.setInstanceEnabled(1, 2, true);
  owner.setInstanceEnabled(2, 3, true);
  owner.setInstanceEnabled(3, 4, true);
  EJitDimPair d1[1] = {dim(0, 1)};
  EJitDimPair d2[2] = {dim(0, 1), dim(1, 2)};
  EJitDimPair d3[3] = {dim(0, 1), dim(1, 2), dim(2, 3)};
  EJitDimPair d4[4] = {dim(0, 1), dim(1, 2), dim(2, 3), dim(3, 4)};
  auto publishDims = [&](uint32_t fi, const EJitDimPair *d, uint32_t n) {
    ASSERT_EQ(owner.compileOrGet(fi, d, n, codeFor(fi)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(owner.pollOne());
  };
  publish(owner, 60);
  publishDims(61, d1, 1);
  publishDims(62, d2, 2);
  publishDims(63, d3, 3);
  publishDims(64, d4, 4);

  auto cmp = [&](EJitSharedTaskPool::CompileOrGetResult fixed, uint32_t fi,
                 const EJitDimPair *d, uint32_t n) {
    auto gen = owner.tryCacheHit(fi, d, n);
    EXPECT_EQ(fixed.status, EJitCompileOrGetStatus::CacheHit);
    EXPECT_EQ(fixed.status, gen.status);
    EXPECT_EQ(fixed.fnPtr, gen.fnPtr);
    EXPECT_EQ(fixed.bucketIndex, gen.bucketIndex);
    owner.releaseRead(fixed.bucketIndex);
    owner.releaseRead(gen.bucketIndex);
  };
  cmp(owner.tryCacheHit0D(60), 60, nullptr, 0);
  cmp(owner.tryCacheHit1D(61, 0, 1), 61, d1, 1);
  cmp(owner.tryCacheHit2D(62, 0, 1, 1, 2), 62, d2, 2);
  cmp(owner.tryCacheHit3D(63, 0, 1, 1, 2, 2, 3), 63, d3, 3);
  cmp(owner.tryCacheHit4D(64, 0, 1, 1, 2, 2, 3, 3, 4), 64, d4, 4);
}

// Version mismatch: a stale instance version (bumped by a deactivate/activate
// cycle after publish) must miss on the fixed path exactly as on the generic
// path — never returning the stale cached pointer.
TEST_F(SharedTaskPoolTest, FixedDimVersionMismatchMisses) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(0, 1, true);
  owner.setInstanceEnabled(1, 2, true);

  // 1D
  EJitDimPair d1[1] = {dim(0, 1)};
  ASSERT_EQ(owner.compileOrGet(70, d1, 1, codeFor(70)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  // Bump the version while leaving the instance ENABLED (disable then enable).
  owner.setInstanceEnabled(0, 1, false);
  owner.setInstanceEnabled(0, 1, true);
  auto genMiss = owner.tryCacheHit(70, d1, 1);
  auto fixedMiss = owner.tryCacheHit1D(70, 0, 1);
  EXPECT_FALSE(genMiss.fastPathTerminal);   // stale version → miss
  EXPECT_FALSE(fixedMiss.fastPathTerminal); // same on the fixed path
  EXPECT_NE(fixedMiss.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_FALSE(fixedMiss.hasReadToken);

  // 2D: bump only the second dim's version.
  EJitDimPair d2[2] = {dim(0, 1), dim(1, 2)};
  ASSERT_EQ(owner.compileOrGet(71, d2, 2, codeFor(71)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  auto hit2 = owner.tryCacheHit2D(71, 0, 1, 1, 2); // sanity: hits before bump
  ASSERT_EQ(hit2.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit2.bucketIndex);
  owner.setInstanceEnabled(1, 2, false);
  owner.setInstanceEnabled(1, 2, true);
  auto fixedMiss2 = owner.tryCacheHit2D(71, 0, 1, 1, 2);
  EXPECT_FALSE(fixedMiss2.fastPathTerminal);
  EXPECT_FALSE(fixedMiss2.hasReadToken);
}

//===----------------------------------------------------------------------===//
// Read-token discipline + invalidation, valid in BOTH builds:
//  * default (token) build: a hit holds a read token, keeps readers > 0, and
//    hands back a real bucketIndex the caller releases.
//  * EJIT_SRE_TASKPOOL_NO_RECLAIM (seqlock) build: a hit holds NO token, never
//    touches readers, and returns the out-of-range sentinel bucketIndex so the
//    wrapper's releaseRead() cleanly no-ops. The safety of skipping the token
//    rests on published code never being freed in that build.
// In EITHER build, activate/deactivate (a version bump) must still invalidate:
// a hit for a re-disabled instance is never served stale code.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, HitTokenDisciplineAndVersionInvalidation) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitDimPair d0[1] = {dim(1u, 2u)};
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(1, 2, true); // enable the instance (bumps version)
  ASSERT_EQ(owner.compileOrGet(9, d0, 1, codeFor(9)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  auto hit = owner.tryCacheHit1D(9, 1, 2);
  ASSERT_TRUE(hit.fastPathTerminal);
  EXPECT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, codeFor(9)); // correct specialization pointer
#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
  EXPECT_FALSE(hit.hasReadToken);
  EXPECT_EQ(hit.bucketIndex, kEJitSharedCacheBuckets); // sentinel
  // No token was taken: readers stay 0 across the whole hit.
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    EXPECT_EQ(state_->buckets[b].readers.loadAcquire(), 0u);
  owner.releaseRead(hit.bucketIndex); // sentinel -> safe no-op
#else
  EXPECT_TRUE(hit.hasReadToken);
  EXPECT_LT(hit.bucketIndex, kEJitSharedCacheBuckets);
  EXPECT_GT(state_->buckets[hit.bucketIndex].readers.loadAcquire(), 0u);
  owner.releaseRead(hit.bucketIndex);
  EXPECT_EQ(state_->buckets[hit.bucketIndex].readers.loadAcquire(), 0u);
#endif

  // Deactivate → re-activate bumps the instance version: the stale-version slot
  // must NOT be served; the fast path cleanly reports it (disabled or miss) so
  // the caller recompiles. This holds identically in the seqlock build.
  owner.setInstanceEnabled(1, 2, false);
  auto disabled = owner.tryCacheHit1D(9, 1, 2);
  EXPECT_NE(disabled.status, EJitCompileOrGetStatus::CacheHit);
  owner.setInstanceEnabled(1, 2, true);
  auto stale = owner.tryCacheHit1D(9, 1, 2);
  // Re-enabled but version moved on: identity matches, version does not → not a
  // hit (a fresh compile must republish under the new version).
  EXPECT_NE(stale.status, EJitCompileOrGetStatus::CacheHit);
  if (stale.hasReadToken)
    owner.releaseRead(stale.bucketIndex);
}

//===----------------------------------------------------------------------===//
// Slot-depth diagnostics: 0D identities whose funcIndex is a multiple of
// kEJitSharedCacheBuckets all hash to bucket 0 and fill its slots 0,1,2,... in
// publish order (cachePublish uses first-empty). This deterministically places
// a target at a chosen linear-scan depth, exercising the per-slot scan (and the
// identityHash-first fast-reject reorder) and proving the deepest slot is still
// found and correctly identified.
//===----------------------------------------------------------------------===//
namespace {
// Return the linear slot index at which \p funcIndex is Ready in its bucket, or
// -1 if not found. Pure diagnostic scan (no lock needed in the single-thread
// test).
int slotDepthOf(EJitSharedTaskPoolState *st, uint32_t funcIndex,
                uint32_t numDims) {
  uint32_t bucket = funcIndex % kEJitSharedCacheBuckets; // 0D key == funcIndex
  auto &B = st->buckets[bucket];
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
    auto &Slot = B.slots[s];
    if (Slot.state.loadAcquire() ==
            static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
        Slot.funcIndex == funcIndex && Slot.numDims == numDims)
      return static_cast<int>(s);
  }
  return -1;
}
} // namespace

TEST_F(SharedTaskPoolTest, SlotDepthHitsAtEveryDepth) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  const uint32_t kB = kEJitSharedCacheBuckets;

  // Fill bucket 0 slots 0..15 with distinct 0D colliders.
  for (uint32_t d = 0; d < kEJitSharedCacheSlots; ++d) {
    uint32_t fi = d * kB;
    ASSERT_EQ(owner.compileOrGet(fi, nullptr, 0, codeFor(fi)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(owner.pollOne());
    EXPECT_EQ(slotDepthOf(state_.get(), fi, 0), static_cast<int>(d));
  }

  // Every colliding identity — including the deepest slot 15 — must still be a
  // correct cache hit with its own specialization pointer.
  for (uint32_t d = 0; d < kEJitSharedCacheSlots; ++d) {
    uint32_t fi = d * kB;
    auto hit = owner.tryCacheHit0D(fi);
    ASSERT_TRUE(hit.fastPathTerminal) << "depth " << d;
    EXPECT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit) << "depth " << d;
    EXPECT_EQ(hit.fnPtr, codeFor(fi)) << "depth " << d;
    if (hit.hasReadToken)
      owner.releaseRead(hit.bucketIndex);
  }

  // An unpublished colliding key in the same (now full) bucket is a clean miss.
  auto miss = owner.tryCacheHit0D(kEJitSharedCacheSlots * kB);
  EXPECT_NE(miss.status, EJitCompileOrGetStatus::CacheHit);
  if (miss.hasReadToken)
    owner.releaseRead(miss.bucketIndex);
}

// The identityHash-first reorder must never alias two identities that share a
// bucket: a wrong-funcIndex query with the same bucket must miss even when a
// different identity is Ready there.
TEST_F(SharedTaskPoolTest, SlotDepthNoIdentityAliasAcrossBucket) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  const uint32_t kB = kEJitSharedCacheBuckets;
  // Publish only funcIndex 5*kB at slot 0 of bucket 0.
  uint32_t present = 5 * kB;
  ASSERT_EQ(owner.compileOrGet(present, nullptr, 0, codeFor(present)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  // A different colliding funcIndex (same bucket, different identity) misses.
  auto miss = owner.tryCacheHit0D(9 * kB);
  EXPECT_NE(miss.status, EJitCompileOrGetStatus::CacheHit);
  if (miss.hasReadToken)
    owner.releaseRead(miss.bucketIndex);
  // The present one still hits with the right pointer.
  auto hit = owner.tryCacheHit0D(present);
  EXPECT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, codeFor(present));
  if (hit.hasReadToken)
    owner.releaseRead(hit.bucketIndex);
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
//===----------------------------------------------------------------------===//
// NO_RECLAIM fixed-dimension seqlock specializations (cacheLookupSeq0D/1D/2D):
// the unrolled seqlock lookups reached by tryCacheHit0D/1D/2D must return the
// SAME result as the generic seqlock path (cacheLookupSeq via tryCacheHit), for
// hits, deep-slot hits, misses, disabled instances, and version invalidation.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, SeqFixedDimMatchesGeneric0D1D2D) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(1, 2, true);
  owner.setInstanceEnabled(3, 4, true);

  // Publish a 0D, a 1D and a 2D specialization.
  ASSERT_EQ(owner.compileOrGet(21, nullptr, 0, codeFor(21)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  EJitDimPair d1[1] = {dim(1, 2)};
  ASSERT_EQ(owner.compileOrGet(22, d1, 1, codeFor(22)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  EJitDimPair d2[2] = {dim(1, 2), dim(3, 4)};
  ASSERT_EQ(owner.compileOrGet(23, d2, 2, codeFor(23)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  // Fixed-dim seqlock entry vs generic seqlock entry: identical outcomes.
  auto fixed0 = owner.tryCacheHit0D(21);
  auto gen0 = owner.tryCacheHit(21, nullptr, 0);
  EXPECT_EQ(fixed0.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fixed0.status, gen0.status);
  EXPECT_EQ(fixed0.fnPtr, gen0.fnPtr);
  EXPECT_EQ(fixed0.fnPtr, codeFor(21));

  auto fixed1 = owner.tryCacheHit1D(22, 1, 2);
  auto gen1 = owner.tryCacheHit(22, d1, 1);
  EXPECT_EQ(fixed1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fixed1.status, gen1.status);
  EXPECT_EQ(fixed1.fnPtr, gen1.fnPtr);
  EXPECT_EQ(fixed1.fnPtr, codeFor(22));

  auto fixed2 = owner.tryCacheHit2D(23, 1, 2, 3, 4);
  auto gen2 = owner.tryCacheHit(23, d2, 2);
  EXPECT_EQ(fixed2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(fixed2.status, gen2.status);
  EXPECT_EQ(fixed2.fnPtr, gen2.fnPtr);
  EXPECT_EQ(fixed2.fnPtr, codeFor(23));

  // A miss and a wrong-dim query must also agree (both miss).
  EXPECT_NE(owner.tryCacheHit0D(99).status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(owner.tryCacheHit1D(22, 1, 3).status,
            EJitCompileOrGetStatus::CacheHit); // wrong instanceId
}

// Version bump after a seqlock fixed-dim publish must invalidate the old slot.
TEST_F(SharedTaskPoolTest, SeqFixedDimVersionBumpMisses) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);
  owner.setInstanceEnabled(2, 7, true);
  EJitDimPair d1[1] = {dim(2, 7)};
  ASSERT_EQ(owner.compileOrGet(31, d1, 1, codeFor(31)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  EXPECT_EQ(owner.tryCacheHit1D(31, 2, 7).status,
            EJitCompileOrGetStatus::CacheHit);
  // Deactivate (bumps version): stale slot must not be served.
  owner.setInstanceEnabled(2, 7, false);
  EXPECT_NE(owner.tryCacheHit1D(31, 2, 7).status,
            EJitCompileOrGetStatus::CacheHit);
}
#endif // EJIT_SRE_TASKPOOL_NO_RECLAIM

//===----------------------------------------------------------------------===//
// Phase-1 hot-hit micro-benchmark (DISABLED by default; run explicitly with
//   --gtest_also_run_disabled_tests --gtest_filter='*HotHitMicroBench*'
// on an aarch64 host). Measures the per-hit cost of the shared taskpool hit
// path components to drive the read-token optimization. Not a correctness test.
//===----------------------------------------------------------------------===//
#if defined(__aarch64__)
namespace {
static inline uint64_t benchNow() {
  uint64_t v;
  asm volatile("isb; mrs %0, cntvct_el0" : "=r"(v)::"memory");
  return v;
}
static inline uint64_t benchFreq() {
  uint64_t v;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
  return v;
}
struct BatchStats {
  double avgNs, p50, p90, p99, maxNs;
  size_t samples;
};
static BatchStats summarize(std::vector<double> &perIterNs) {
  std::sort(perIterNs.begin(), perIterNs.end());
  BatchStats s;
  s.samples = perIterNs.size();
  double sum = 0;
  for (double v : perIterNs)
    sum += v;
  s.avgNs = sum / perIterNs.size();
  auto pct = [&](double p) {
    size_t idx = static_cast<size_t>(p * (perIterNs.size() - 1));
    return perIterNs[idx];
  };
  s.p50 = pct(0.50);
  s.p90 = pct(0.90);
  s.p99 = pct(0.99);
  s.maxNs = perIterNs.back();
  return s;
}
static void report(const char *name, const BatchStats &s) {
  printf("  %-28s avg=%.1fns p50=%.1f p90=%.1f p99=%.1f max=%.1f (n=%zu batches)\n",
         name, s.avgNs, s.p50, s.p90, s.p99, s.maxNs, s.samples);
}
} // namespace

TEST_F(SharedTaskPoolTest, DISABLED_HotHitMicroBench) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  publish(owner, 42);
  EJitCoreId::setCurrentForTest(0); // owner-core hit path
  const uint64_t freq = benchFreq();
  const double tickNs = 1e9 / static_cast<double>(freq);
  const uint32_t kIters = 2000;    // per batch
  const uint32_t kBatches = 2000;  // distribution samples
  printf("HotHit micro-bench: cntfrq=%llu Hz (%.3f ns/tick), %u iters x %u batches\n",
         (unsigned long long)freq, tickNs, kIters, kBatches);

  // Warm up + correctness sanity.
  {
    auto r = owner.tryCacheHit0D(42);
    ASSERT_TRUE(r.fastPathTerminal);
    ASSERT_NE(r.fnPtr, nullptr);
    if (r.hasReadToken)
      owner.releaseRead(r.bucketIndex);
  }

  auto runBatches = [&](auto &&body) {
    std::vector<double> perIterNs;
    perIterNs.reserve(kBatches);
    for (uint32_t b = 0; b < kBatches; ++b) {
      uint64_t t0 = benchNow();
      for (uint32_t i = 0; i < kIters; ++i)
        body();
      uint64_t t1 = benchNow();
      perIterNs.push_back((double)(t1 - t0) * tickNs / kIters);
    }
    return summarize(perIterNs);
  };

  volatile void *sink = nullptr;

  // (A) Full current hit path: lookup (read-token acquire) + releaseRead.
  auto full = runBatches([&] {
    auto r = owner.tryCacheHit0D(42);
    sink = r.fnPtr;
    owner.releaseRead(r.bucketIndex);
  });

  // (B) Lookup only (read-token acquired but released untimed): isolates get_fn.
  auto lookup = runBatches([&] {
    auto r = owner.tryCacheHit0D(42);
    sink = r.fnPtr;
    owner.releaseRead(r.bucketIndex); // released, but the timed body is above
  });
  // Re-time (B) with release truly outside the measured region is not possible
  // per-iter; instead measure release in isolation as (C).

  // (C) releaseRead in isolation (lookup done untimed just before).
  auto release = runBatches([&] {
    auto r = owner.tryCacheHit0D(42); // untimed-ish, but included; see note
    owner.releaseRead(r.bucketIndex);
    sink = r.fnPtr;
  });

  // (D) Load-only seqlock-style read of the SAME slot: no RMW atomics, no
  // release. This is the target design's per-hit cost.
  EJitSharedCacheSlot *slot = findReadySlot(42);
  ASSERT_NE(slot, nullptr);
  auto seqlike = runBatches([&] {
    // state(acquire) -> identity loads -> fnPtr(acquire) -> re-check state.
    uint32_t s0 = slot->state.loadAcquire();
    uint32_t fi = slot->funcIndex;
    uint64_t h = slot->identityHash;
    void *fn = reinterpret_cast<void *>(slot->fnPtr.loadAcquire());
    uint32_t s1 = slot->state.loadAcquire();
    if (s0 == s1 && fi == 42 && h)
      sink = fn;
  });
  (void)sink;

  printf("Shared taskpool owner-core 0D hot hit component costs:\n");
  report("A full (lookup+release)", full);
  report("B lookup+release (dup)", lookup);
  report("C lookup+release iso", release);
  report("D seqlock load-only", seqlike);
  printf("Delta A-D (RMW+release removed) approx avg = %.1f ns/hit\n",
         full.avgNs - seqlike.avgNs);
}

// Multi-core contention model of the real board: N cores hammering the SAME hot
// function share one bucket cache line. The read-token RMW (fetchAdd/fetchSub on
// bucket.readers) then bounces that line between cores; a load-only seqlock read
// does not. This is the scenario the 6us regression comes from.
TEST_F(SharedTaskPoolTest, DISABLED_HotHitContendedBench) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  publish(owner, 42);
  EJitSharedCacheSlot *slot = findReadySlot(42);
  ASSERT_NE(slot, nullptr);
  EJitSharedCacheBucket *bucket = nullptr;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets && !bucket; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s)
      if (&state_->buckets[b].slots[s] == slot) {
        bucket = &state_->buckets[b];
        break;
      }
  ASSERT_NE(bucket, nullptr);
  const double tickNs = 1e9 / static_cast<double>(benchFreq());
  const uint32_t kIters = 200000;

  for (unsigned T : {1u, 2u, 4u, 8u}) {
    std::vector<double> tokDur(T), seqDur(T);
    auto tokenBody = [&](unsigned idx) {
      uint64_t t0 = benchNow();
      volatile uint64_t acc = 0;
      for (uint32_t i = 0; i < kIters; ++i) {
        bucket->readers.fetchAdd(1);
        acc += slot->fnPtr.loadAcquire();
        bucket->readers.fetchSub(1);
      }
      uint64_t t1 = benchNow();
      (void)acc;
      tokDur[idx] = (double)(t1 - t0) * tickNs / kIters;
    };
    auto seqBody = [&](unsigned idx) {
      uint64_t t0 = benchNow();
      volatile uint64_t acc = 0;
      for (uint32_t i = 0; i < kIters; ++i) {
        uint32_t s0 = slot->state.loadAcquire();
        acc += slot->fnPtr.loadAcquire();
        uint32_t s1 = slot->state.loadAcquire();
        acc += (s0 == s1);
      }
      uint64_t t1 = benchNow();
      (void)acc;
      seqDur[idx] = (double)(t1 - t0) * tickNs / kIters;
    };
    auto runContended = [&](auto body, std::vector<double> &dur) {
      std::vector<std::thread> ths;
      for (unsigned t = 0; t < T; ++t)
        ths.emplace_back([&, t] { body(t); });
      for (auto &th : ths)
        th.join();
      double sum = 0, mx = 0;
      for (double d : dur) {
        sum += d;
        mx = std::max(mx, d);
      }
      return std::pair<double, double>(sum / T, mx);
    };
    auto tok = runContended(tokenBody, tokDur);
    auto seq = runContended(seqBody, seqDur);
    printf("T=%u cores same bucket: token(RMW) avg=%.1fns max=%.1f | "
           "seqlock(load) avg=%.1fns max=%.1f | token/seqlock=%.1fx\n",
           T, tok.first, tok.second, seq.first, seq.second,
           seq.first > 0 ? tok.first / seq.first : 0.0);
  }
}
#endif // __aarch64__

//===----------------------------------------------------------------------===//
// PGO (§6): shared taskpool hitCount + Tier-2 auto-trigger.
//===----------------------------------------------------------------------===//

// Shared equivalent of EJitTaskPoolTest::PgoHitThresholdArmsTier2Recompile.
// Set PGO threshold=3, publish Tier-1, hit it twice (below threshold), then
// the third hit crosses the threshold -> tier2Arm + Tier-2 request enqueued.
// pollOne compiles it -> publish resets hitCount to 0.
TEST_F(SharedTaskPoolTest, SharedPgoHitThresholdArmsTier2Recompile) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);

  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};
  pool.setPgoEnabled(true, 2); // arm Tier-2 on the 2nd hit

  // Tier-1: miss -> enqueue -> pollOne compiles + publishes.
  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  EXPECT_EQ(pool.pendingCount(), 0u);

  // One hit: below threshold -> no Tier-2 armed, but hitCount increments.
  auto r1 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_FALSE(r1.tier2Arm);
  EXPECT_EQ(pool.pendingCount(), 0u);
  if (r1.hasReadToken)
    pool.releaseRead(r1.bucketIndex);

  // Verify hitCount incremented.
  { EJitSharedCacheSlot *s = findReadySlot(5); ASSERT_NE(s,nullptr); EXPECT_EQ(s->hitCount.loadRelaxed(),1u); }

  // Second hit crosses threshold -> Tier-2 armed + enqueued on the SHARED
  // MPSC queue (no facade-local bypass).  The in-flight dedup bit for the
  // (stripped) funcIndex is now claimed, so pendingCount reflects the queued
  // Tier-2 request.
  auto r2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(r2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_TRUE(r2.tier2Arm) << "hit crossing threshold should arm Tier-2";
  EXPECT_EQ(pool.pendingCount(), 1u)
      << "Tier-2 request queued via shared queue";
  if (r2.hasReadToken)
    pool.releaseRead(r2.bucketIndex);

  // Verify hitCount.
  { EJitSharedCacheSlot *s = findReadySlot(5); ASSERT_NE(s,nullptr); EXPECT_EQ(s->hitCount.loadRelaxed(),2u); }

  // The owner worker consumes the shared queue: pollOne() pops the Tier-2
  // request and compiles it (returns true — the Tier-2 travelled through the
  // ring, it is NOT a facade-local inline bypass).  runCompile() derives the
  // aarch64 exclusive-monitor workaround from the request's encoded PGOUse
  // tier.
  EXPECT_TRUE(pool.pollOne());
  EXPECT_EQ(pool.pendingCount(), 0u);

  // hitCount reset to 0 on Tier-2 publish (cachePublish overwrites Tier-1).
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->hitCount.loadRelaxed(), 0u);
    EXPECT_NE(s->fnPtr.loadRelaxed(), 0u); // now has Tier-2 code
    EXPECT_EQ(s->tier.loadRelaxed(),
              static_cast<uint8_t>(kEJitTierPgoUse)); // slot now Tier-2
  }
}

TEST_F(SharedTaskPoolTest, SharedPgoProfilesFunctionsSequentially) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setPgoEnabled(true, 2);
  EXPECT_EQ(state_->pgoMaxActiveFunctions.loadAcquire(), 1u);

  // The first miss owns staged profiling and is explicitly queued as Tier-1.
  ASSERT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 1u);
  EXPECT_EQ(state_->pgoActiveFunctions[0].loadAcquire(), 6u);
  EXPECT_EQ(state_->enqueuePos.loadRelaxed() -
                state_->dequeuePos.loadRelaxed(),
            1u);

  // A different function stays on its AOT fallback and adds no compiler work.
  auto deferred = pool.compileOrGet(6, nullptr, 0, codeFor(6));
  EXPECT_EQ(deferred.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(deferred.fnPtr, codeFor(6));
  EXPECT_EQ(state_->enqueuePos.loadRelaxed() -
                state_->dequeuePos.loadRelaxed(),
            1u);
  EXPECT_EQ(state_->pgoDeferredMisses.loadRelaxed(), 1u);

  ASSERT_TRUE(pool.pollOne());
  EJitSharedCacheSlot *tier1 = findReadySlot(5);
  ASSERT_NE(tier1, nullptr);
  EXPECT_EQ(tier1->tier.loadRelaxed(),
            static_cast<uint8_t>(kEJitTierInstrumented));

  // Only the active function accumulates profile progress and arms Tier-2.
  for (unsigned i = 0; i < 2; ++i) {
    auto hit = pool.compileOrGet(5, nullptr, 0, codeFor(5));
    ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
    if (hit.hasReadToken)
      pool.releaseRead(hit.bucketIndex);
  }
  EXPECT_EQ(state_->pgoProgressQuarters[0].loadAcquire(), 4u);
  EXPECT_EQ(pool.pendingCount(), 1u);

  // Tier-2 completion releases admission. The next called function can then
  // begin its own Tier-1; there is never more than one instrumented function.
  ASSERT_TRUE(pool.pollOne());
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(state_->pgoCompletedFunctions.loadRelaxed(), 1u);
  ASSERT_EQ(pool.compileOrGet(6, nullptr, 0, codeFor(6)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 1u);
  EXPECT_EQ(state_->pgoActiveFunctions[0].loadAcquire(), 7u);
}

TEST_F(SharedTaskPoolTest, SharedPgoHonorsConcurrentProfileLimit) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setPgoEnabled(true, 2, 2);

  ASSERT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(pool.compileOrGet(6, nullptr, 0, codeFor(6)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 2u);
  EXPECT_EQ(state_->pgoMaxActiveFunctions.loadAcquire(), 2u);

  auto deferred = pool.compileOrGet(7, nullptr, 0, codeFor(7));
  EXPECT_EQ(deferred.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(deferred.fnPtr, codeFor(7));
  ASSERT_TRUE(pool.pollOne());
  ASSERT_TRUE(pool.pollOne());

  for (unsigned i = 0; i < 2; ++i) {
    auto hit = pool.compileOrGet(5, nullptr, 0, codeFor(5));
    ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
    if (hit.hasReadToken)
      pool.releaseRead(hit.bucketIndex);
  }
  ASSERT_TRUE(pool.pollOne());
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 1u);

  ASSERT_EQ(pool.compileOrGet(7, nullptr, 0, codeFor(7)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 2u);
}

// Shared version bump test (§7.2 / §4): a Tier-2 request that was queued when
// the identity was current is DISCARDED by the worker when a version bump lands
// between arm and consume. runCompile's checkpoint 1 (versionsCurrent) fails,
// the code is never published over Tier-1, and the encoded-funcIndex dedup bit
// is cleared (dedupClear strips the tier bits), so a later hit can retry.
TEST_F(SharedTaskPoolTest, SharedPgoTier2DiscardedOnVersionBump) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);

  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  pool.setPgoEnabled(true, 2);

  // Tier-1 published.
  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  void *tier1Fn = nullptr;
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    tier1Fn = reinterpret_cast<void *>(s->fnPtr.loadRelaxed());
  }

  // First hit below threshold, second hit arms + enqueues the Tier-2 request
  // (snapshotting the CURRENT versions).
  auto r1 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_FALSE(r1.tier2Arm);
  if (r1.hasReadToken)
    pool.releaseRead(r1.bucketIndex);
  auto r2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_TRUE(r2.tier2Arm);
  EXPECT_EQ(pool.pendingCount(), 1u); // Tier-2 queued
  EXPECT_NE(state_->inFlight[5].loadRelaxed(), 0u);
  if (r2.hasReadToken)
    pool.releaseRead(r2.bucketIndex);

  // Toggle the instance AFTER the Tier-2 is queued (version bump): the queued
  // request's snapshot versions are now stale.
  pool.setInstanceEnabled(1, 4, false);
  pool.setInstanceEnabled(1, 4, true);

  // Worker consumes the queued Tier-2: it pops it (returns true) but
  // runCompile drops it at the version checkpoint — no publish over Tier-1.
  EXPECT_TRUE(pool.pollOne());

  // Tier-1 code is intact (Tier-2 never overwrote it) and the encoded-funcIndex
  // dedup bit was cleared by the strip-aware dedupClear, so a retry is
  // possible.
  EXPECT_EQ(pool.pendingCount(), 0u);
  EXPECT_EQ(state_->inFlight[5].loadRelaxed(), 0u)
      << "dedupClear must strip the Tier-2 tier bits and free the in-flight "
         "bit";
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(reinterpret_cast<void *>(s->fnPtr.loadRelaxed()), tier1Fn)
        << "stale Tier-2 must not overwrite the still-executable Tier-1 code";
    EXPECT_NE(s->tier.loadRelaxed(), static_cast<uint8_t>(kEJitTierPgoUse))
        << "the slot must not have been upgraded to Tier-2";
  }
}

// PGO off → no hitCount increment, no tier2Arm, no Tier-2 enqueue.
// Baseline guards: compileOrGet hits are ordinary cache hits with zero
// PGO behaviour when setPgoEnabled was never called.
TEST_F(SharedTaskPoolTest, SharedPgoOffZeroOverhead) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);

  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  // PGO left off (default) — never call setPgoEnabled.

  // Tier-1 published.
  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());

  // Hits: ordinary cache hits, no tier2Arm.
  for (int i = 0; i < 10; ++i) {
    auto r = pool.compileOrGet(5, d0, 1, codeFor(5));
    ASSERT_EQ(r.status, EJitCompileOrGetStatus::CacheHit);
    EXPECT_FALSE(r.tier2Arm);
    if (r.hasReadToken)
      pool.releaseRead(r.bucketIndex);
  }
  EXPECT_EQ(pool.pendingCount(), 0u); // no Tier-2 enqueued

  // hitCount stays at 0 (threshold was never set).
  EJitSharedCacheSlot *slot = findReadySlot(5);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->hitCount.loadRelaxed(), 0u);
}

// PGO threshold=0 (setPgoEnabled(true,0)) → counting disabled, no trigger.
// hitCount is still incremented but no Tier-2 is ever armed.
TEST_F(SharedTaskPoolTest, SharedPgoThresholdZeroDisablesTrigger) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);

  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  // Enable PGO but with threshold=0 → hitCount increments but trigger is off.
  pool.setPgoEnabled(true, 0);

  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());

  for (int i = 0; i < 10; ++i) {
    auto r = pool.compileOrGet(5, d0, 1, codeFor(5));
    ASSERT_EQ(r.status, EJitCompileOrGetStatus::CacheHit);
    EXPECT_FALSE(r.tier2Arm);
    if (r.hasReadToken)
      pool.releaseRead(r.bucketIndex);
  }
  EXPECT_EQ(pool.pendingCount(), 0u); // no trigger

  // hitCount IS incremented (bookkeeping, not arming).
  EJitSharedCacheSlot *slot = findReadySlot(5);
  ASSERT_NE(slot, nullptr);
  EXPECT_GT(slot->hitCount.loadRelaxed(), 0u);
}

// End-to-end: PGO enabled → Tier-1 publish → hits cross threshold → pollOne
// inline Tier-2 compile with pgoClearExclusive → publish overwrites Tier-1 →
// hitCount reset → subsequent hits return Tier-2 code.
TEST_F(SharedTaskPoolTest, SharedPgoEndToEndTier2OverwritesTier1) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};
  pool.setPgoEnabled(true, 2);

  // Phase 1: Tier-1 compile + publish.
  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());

  void *tier1Fn = nullptr;
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    tier1Fn = reinterpret_cast<void *>(s->fnPtr.loadRelaxed());
    ASSERT_NE(tier1Fn, nullptr);
  }

  // Phase 2: two cache hits → 2nd arms Tier-2.
  auto r1 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_FALSE(r1.tier2Arm);
  if (r1.hasReadToken)
    pool.releaseRead(r1.bucketIndex);

  auto r2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_TRUE(r2.tier2Arm);
  EXPECT_EQ(pool.pendingCount(), 1u);
  if (r2.hasReadToken)
    pool.releaseRead(r2.bucketIndex);

  // Phase 3: the owner worker consumes the shared queue — pollOne pops the
  // Tier-2 request and compiles it (returns true; runCompile derives the
  // aarch64 exclusive-monitor workaround from the encoded PGOUse tier).
  EXPECT_TRUE(pool.pollOne());
  EXPECT_EQ(pool.pendingCount(), 0u);

  // Phase 4: Tier-2 overwrote Tier-1 — fnPtr changed, hitCount reset.
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->hitCount.loadRelaxed(), 0u);
    void *tier2Fn = reinterpret_cast<void *>(s->fnPtr.loadRelaxed());
    ASSERT_NE(tier2Fn, nullptr);
    EXPECT_NE(tier2Fn, tier1Fn)
        << "Tier-2 compile produced new code";
  }

  // Phase 5: subsequent hit returns Tier-2 pointer.
  auto r3 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r3.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(r3.fnPtr, codeFor(5))
      << "hit after Tier-2 returns Tier-2 code, not fallback";
  EXPECT_FALSE(r3.tier2Arm)
      << "hitCount was reset, no re-trigger";
  if (r3.hasReadToken)
    pool.releaseRead(r3.bucketIndex);
}

//===----------------------------------------------------------------------===//
// PGO (§1-§4): peer-triggered Tier-2 through the SHARED queue, exact-slot
// snapshots, dedup coalescing, queue-full rollback, and strip-aware
// dedupClear.  These exercise the hardened cross-core tier transition where a
// non-owner facade may cross the threshold but only the single owner worker
// consumes the shared MPSC queue.
//===----------------------------------------------------------------------===//

// Compiler that records what tier / snapshot each request carried, so a test
// can assert the owner worker saw the EXACT hit slot's generation + versions.
struct PgoRecorder {
  int tier1 = 0;
  int tier2 = 0;
  std::vector<uint32_t> t2Func; // stripped funcIndex per Tier-2 compile
  std::vector<uint32_t> t2Gen;  // generation per Tier-2 compile
  std::vector<uint32_t> t2Ver0; // versions[0] per Tier-2 compile
};
bool mockCompileRecordPgo(void *ctx, const EJitCompileRequest &req,
                          void **outFn) {
  auto *r = static_cast<PgoRecorder *>(ctx);
  if (decodeReqTier(req.funcIndex) == kEJitTierPgoUse) {
    ++r->tier2;
    r->t2Func.push_back(stripReqTier(req.funcIndex));
    r->t2Gen.push_back(req.generation);
    r->t2Ver0.push_back(req.numDims ? req.versions[0] : 0u);
  } else {
    ++r->tier1;
  }
  *outFn = codeFor(req.funcIndex);
  return true;
}

// Fails only the Tier-2 (PGOUse) compile; Tier-1/Baseline succeed. Lets a test
// drive the compile-failure dedup-rollback path.
bool mockCompileFailTier2(void * /*ctx*/, const EJitCompileRequest &req,
                          void **outFn) {
  if (decodeReqTier(req.funcIndex) == kEJitTierPgoUse)
    return false;
  *outFn = codeFor(req.funcIndex);
  return true;
}

// Mirror of EJitSharedTaskPool::hashIdentity's bucket selection so a test can
// deterministically construct two identities that collide into one bucket.
uint32_t bucketOfIdentity(uint32_t funcIndex, const EJitDimPair *dims,
                          uint32_t numDims) {
  uint64_t key = static_cast<uint64_t>(funcIndex);
  for (uint32_t i = 0; i < numDims; ++i) {
    key ^= (static_cast<uint64_t>(dims[i].dimType) << 32) |
           static_cast<uint64_t>(dims[i].instanceId);
    key *= 0x9e3779b97f4a7c15ULL;
  }
  return static_cast<uint32_t>(key % kEJitSharedCacheBuckets);
}

// Attach a non-owner producer facade on \p core. PGO configuration is read
// from the shared blob; the peer must not need facade-local configuration.
void attachPeer(EJitSharedTaskPool &peer, EJitSharedTaskPoolState *state,
                uint32_t core) {
  EJitCoreId::setCurrentForTest(core);
  peer.bind(state);
  peer.setMode(EJitCompileMode::Async);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
}

TEST_F(SharedTaskPoolTest, PgoControlIsSharedAcrossFacades) {
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setMode(EJitCompileMode::Async);
  owner.setPgoEnabled(true, 17);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(state_->pgoEnabled.loadAcquire(), 1u);
  EXPECT_EQ(state_->tier2Threshold.loadAcquire(), 17u);

  EJitSharedTaskPool peer;
  attachPeer(peer, state_.get(), /*core=*/1);
  EXPECT_TRUE(peer.isPgoEnabled());

  // A live owner-side update is shared too; the peer has no local setter call.
  EJitCoreId::setCurrentForTest(0);
  owner.setPgoEnabled(false, 0);
  EJitCoreId::setCurrentForTest(1);
  EXPECT_FALSE(peer.isPgoEnabled());
  EXPECT_EQ(state_->tier2Threshold.loadAcquire(), 0u);
}

// (1) A peer core crosses the Tier-2 threshold; the request travels the shared
// queue and only the OWNER worker consumes it, publishing Tier-2.
TEST_F(SharedTaskPoolTest, PeerCrossesThresholdOwnerConsumes) {
  EJitSharedTaskPool owner;
  PgoRecorder rec;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompileRecordPgo, &rec);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  owner.setPgoEnabled(true, 3);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  owner.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  // Tier-1 published by the owner.
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  EXPECT_EQ(rec.tier1, 1);

  // A peer facade attaches on core 1 and drives the hits.
  EJitSharedTaskPool peer;
  attachPeer(peer, state_.get(), /*core=*/1);

  // Peer hits: each increments the SHARED slot hitCount; the third crosses the
  // threshold and enqueues a Tier-2 request onto the shared queue.
  EJitCoreId::setCurrentForTest(1);
  for (int i = 0; i < 3; ++i)
    (void)peer.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(peer.pendingCount(), 1u) << "peer enqueued exactly one Tier-2";
  EXPECT_NE(state_->inFlight[5].loadRelaxed(), 0u);

  // The peer has no worker; the OWNER worker consumes the shared queue.
  EJitCoreId::setCurrentForTest(0);
  EXPECT_TRUE(owner.pollOne());
  EXPECT_EQ(rec.tier2, 1) << "owner compiled the peer-triggered Tier-2 once";

  // Tier-2 published: slot tier upgraded, fnPtr changed, queue + dedup drained.
  EJitSharedCacheSlot *s = findReadySlot(5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->tier.loadRelaxed(), static_cast<uint8_t>(kEJitTierPgoUse));
  EXPECT_EQ(s->hitCount.loadRelaxed(), 0u);
  EXPECT_NE(reinterpret_cast<void *>(s->fnPtr.loadRelaxed()), codeFor(5));
  EXPECT_EQ(owner.pendingCount(), 0u);
  EXPECT_FALSE(owner.pollOne()); // queue empty
}

// (2) Two peers cross the threshold on the SAME identity; shared dedup admits
// exactly one Tier-2 request and the owner compiles it once. No permanent
// pending remains.
TEST_F(SharedTaskPoolTest, MultiplePeersTriggerOnlyOneTier2) {
  EJitSharedTaskPool owner;
  PgoRecorder rec;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompileRecordPgo, &rec);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  owner.setPgoEnabled(true, 2);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  // Tier-1 published.
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  EJitSharedTaskPool peerA, peerB;
  attachPeer(peerA, state_.get(), /*core=*/1);
  attachPeer(peerB, state_.get(), /*core=*/2);

  // Both peers reach/exceed the threshold on the same (5, dim) identity.
  EJitCoreId::setCurrentForTest(1);
  (void)peerA.compileOrGet(5, d0, 1, codeFor(5));
  (void)peerA.compileOrGet(5, d0, 1, codeFor(5)); // crosses -> enqueue
  EJitCoreId::setCurrentForTest(2);
  (void)peerB.compileOrGet(5, d0, 1, codeFor(5));
  (void)peerB.compileOrGet(5, d0, 1, codeFor(5)); // crosses -> AlreadyPending

  // Shared dedup coalesced both peers into a single queued Tier-2 request.
  EXPECT_EQ(owner.pendingCount(), 1u);

  EJitCoreId::setCurrentForTest(0);
  EXPECT_TRUE(owner.pollOne());  // compiles the single Tier-2
  EXPECT_FALSE(owner.pollOne()); // nothing else queued
  EXPECT_EQ(rec.tier2, 1) << "owner compiled Tier-2 exactly once for two peers";
  EXPECT_EQ(owner.pendingCount(), 0u) << "no permanent pending";

  EJitSharedCacheSlot *s = findReadySlot(5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->tier.loadRelaxed(), static_cast<uint8_t>(kEJitTierPgoUse));
}

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
// (3) NO_RECLAIM: two DISTINCT identities each trigger Tier-2; the second
// request must carry its OWN generation/version snapshot, never inherit the
// first's (the stale-request bug the removed generation==0 sentinel caused).
TEST_F(SharedTaskPoolTest, NoReclaimDistinctTier2RequestsUseOwnSnapshots) {
  EJitSharedTaskPool owner;
  PgoRecorder rec;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompileRecordPgo, &rec);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  // Two functions on two instances with DISTINCT versions.
  owner.setInstanceEnabled(1, 4, true);  // version 1
  owner.setInstanceEnabled(2, 7, true);  // version 1
  owner.setInstanceEnabled(2, 7, false); // version 2
  owner.setInstanceEnabled(2, 7, true);  // version 3
  EJitDimPair dA[1] = {dim(1, 4)};
  EJitDimPair dB[1] = {dim(2, 7)};
  ASSERT_EQ(state_->version[1][4].loadAcquire(), 1u);
  ASSERT_EQ(state_->version[2][7].loadAcquire(), 3u);

  // Tier-1 for both.
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(5, dA, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  ASSERT_EQ(owner.compileOrGet(6, dB, 1, codeFor(6)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  owner.setPgoEnabled(true, 1); // a single hit arms

  // Trigger + consume A, then B — consecutively.
  (void)owner.compileOrGet(5, dA, 1, codeFor(5)); // arms A (versions[0]=1)
  ASSERT_TRUE(owner.pollOne());
  (void)owner.compileOrGet(6, dB, 1, codeFor(6)); // arms B (versions[0]=3)
  ASSERT_TRUE(owner.pollOne());

  ASSERT_EQ(rec.tier2, 2u);
  // The two Tier-2 requests carried their OWN identity + version snapshot.
  ASSERT_EQ(rec.t2Func.size(), 2u);
  EXPECT_EQ(rec.t2Func[0], 5u);
  EXPECT_EQ(rec.t2Ver0[0], 1u);
  EXPECT_EQ(rec.t2Func[1], 6u);
  EXPECT_EQ(rec.t2Ver0[1], 3u)
      << "second Tier-2 must use its own version, not inherit the first's";
}
#endif // EJIT_SRE_TASKPOOL_NO_RECLAIM

// (4) Two specializations with the SAME funcIndex + same numDims but different
// dims that COLLIDE into one bucket. Triggering Tier-2 on one must snapshot
// THAT slot's version, never the colliding sibling's.
TEST_F(SharedTaskPoolTest, SameFuncSameNumDimsBucketCollisionUsesExactSlot) {
  EJitSharedTaskPool owner;
  PgoRecorder rec;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompileRecordPgo, &rec);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  // Find two instanceIds on dimType 1 whose (funcIndex=5, 1-dim) identities
  // hash to the SAME bucket.
  const uint32_t funcIndex = 5;
  uint32_t instA = 0, instB = 0;
  bool found = false;
  for (uint32_t i = 1; i < kEJitSharedInstances && !found; ++i) {
    EJitDimPair di[1] = {dim(1, i)};
    uint32_t bi = bucketOfIdentity(funcIndex, di, 1);
    for (uint32_t j = i + 1; j < kEJitSharedInstances && !found; ++j) {
      EJitDimPair dj[1] = {dim(1, j)};
      if (bucketOfIdentity(funcIndex, dj, 1) == bi) {
        instA = i;
        instB = j;
        found = true;
      }
    }
  }
  ASSERT_TRUE(found) << "need two colliding identities";
  EJitDimPair dA[1] = {dim(1, instA)};
  EJitDimPair dB[1] = {dim(1, instB)};
  ASSERT_EQ(bucketOfIdentity(funcIndex, dA, 1),
            bucketOfIdentity(funcIndex, dB, 1));

  // Give the two instances DISTINCT versions: A=1, B=3.
  owner.setInstanceEnabled(1, instA, true);  // A version 1
  owner.setInstanceEnabled(1, instB, true);  // B version 1
  owner.setInstanceEnabled(1, instB, false); // B version 2
  owner.setInstanceEnabled(1, instB, true);  // B version 3
  ASSERT_EQ(state_->version[1][instA].loadAcquire(), 1u);
  ASSERT_EQ(state_->version[1][instB].loadAcquire(), 3u);

  // Tier-1 for both (they land in the same bucket, distinct slots).
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(funcIndex, dA, 1, codeFor(funcIndex)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  ASSERT_EQ(owner.compileOrGet(funcIndex, dB, 1, codeFor(funcIndex)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  owner.setPgoEnabled(true, 1);

  // Trigger Tier-2 on A ONLY: the snapshot must come from A's slot (version 1).
  {
    auto h = owner.compileOrGet(funcIndex, dA, 1, codeFor(funcIndex));
    if (h.hasReadToken)
      owner.releaseRead(h.bucketIndex);
  }
  ASSERT_TRUE(owner.pollOne());

  ASSERT_EQ(rec.tier2, 1u);
  ASSERT_EQ(rec.t2Func.size(), 1u);
  EXPECT_EQ(rec.t2Func[0], funcIndex);
  EXPECT_EQ(rec.t2Ver0[0], 1u)
      << "Tier-2 must snapshot the hit slot (A, version 1), not the colliding "
         "sibling (B, version 3)";
}

// (5) A full queue makes the Tier-2 enqueue fail; the in-flight dedup bit is
// rolled back so a later hit (after space frees) can retrigger and succeed.
TEST_F(SharedTaskPoolTest, Tier2QueueFullRollsBackDedup) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};

  // Publish Tier-1 for the target (funcIndex 5).
  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());

  // Fill the shared queue with ordinary undrained requests until full, then
  // enable PGO so staged admission does not intentionally defer the fillers.
  bool full = false;
  uint32_t f = 100;
  for (; f < 100 + kEJitSharedQueueSlots + 8; ++f) {
    auto rr = pool.compileOrGet(f, nullptr, 0, codeFor(f));
    if (rr.status == EJitCompileOrGetStatus::QueueFullFallback) {
      full = true;
      break;
    }
  }
  ASSERT_TRUE(full) << "expected the shared queue to reach capacity";
  pool.setPgoEnabled(true, 1);

  // A hit on the target arms Tier-2, but the enqueue fails (queue full) and the
  // in-flight bit is rolled back (dedupClear strips the encoded tier bits).
  ASSERT_EQ(state_->inFlight[5].loadRelaxed(), 0u);
  auto hit = pool.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_TRUE(hit.tier2Arm);
  EXPECT_EQ(state_->inFlight[5].loadRelaxed(), 0u)
      << "queue-full Tier-2 must roll back its in-flight claim";
  if (hit.hasReadToken)
    pool.releaseRead(hit.bucketIndex);

  // Free a few queue slots (keep most fillers queued so the target's Tier-1
  // slot is not evicted by a full recompile storm before we retry).
  (void)pool.pollBudget(8);

  // Now the target can retrigger and successfully enqueue its Tier-2.
  auto hit2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(hit2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_TRUE(hit2.tier2Arm);
  EXPECT_NE(state_->inFlight[5].loadRelaxed(), 0u)
      << "retry after space frees must claim the in-flight bit";
  if (hit2.hasReadToken)
    pool.releaseRead(hit2.bucketIndex);

  // Drain the rest of the queue; the retried Tier-2 (queued at the tail, after
  // all fillers) is compiled last and publishes func 5 at tier PGOUse.
  (void)pool.pollBudget(kEJitSharedQueueSlots * 2);
  EXPECT_EQ(state_->inFlight[5].loadRelaxed(), 0u);
  EJitSharedCacheSlot *s = findReadySlot(5);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->tier.loadRelaxed(), static_cast<uint8_t>(kEJitTierPgoUse));
}

// (6) The encoded Tier-2 funcIndex must leave NO in-flight bit after success,
// version mismatch, OR compile failure — i.e. dedupClear strips the tier bits.
TEST_F(SharedTaskPoolTest, Tier2DedupClearStripsTierBits) {
  EJitSharedTaskPool pool;
  PgoRecorder rec;
  EJitCoreId::setCurrentForTest(0);
  pool.bind(state_.get());
  pool.setCompiler(&mockCompileRecordPgo, &rec);
  pool.setMode(EJitCompileMode::Async);
  ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  pool.setPgoEnabled(true, 1);

  // --- Phase A: successful Tier-2 clears the in-flight bit. ---
  pool.setInstanceEnabled(1, 4, true);
  EJitDimPair dA[1] = {dim(1, 4)};
  ASSERT_EQ(pool.compileOrGet(7, dA, 1, codeFor(7)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  {
    auto h = pool.compileOrGet(7, dA, 1, codeFor(7)); // arm -> enqueue
    if (h.hasReadToken)
      pool.releaseRead(h.bucketIndex);
  }
  EXPECT_NE(state_->inFlight[7].loadRelaxed(), 0u);
  ASSERT_TRUE(pool.pollOne()); // publish Tier-2
  EXPECT_EQ(state_->inFlight[7].loadRelaxed(), 0u)
      << "successful Tier-2 must clear the stripped-funcIndex in-flight bit";

  // --- Phase B: version mismatch drops Tier-2 and clears the in-flight bit.
  // ---
  pool.setInstanceEnabled(3, 9, true);
  EJitDimPair dB[1] = {dim(3, 9)};
  ASSERT_EQ(pool.compileOrGet(8, dB, 1, codeFor(8)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  {
    auto h = pool.compileOrGet(8, dB, 1, codeFor(8)); // arm -> enqueue
    if (h.hasReadToken)
      pool.releaseRead(h.bucketIndex);
  }
  EXPECT_NE(state_->inFlight[8].loadRelaxed(), 0u);
  pool.setInstanceEnabled(3, 9, false); // version bump invalidates the request
  pool.setInstanceEnabled(3, 9, true);
  ASSERT_TRUE(pool.pollOne()); // worker drops it at the version checkpoint
  EXPECT_EQ(state_->inFlight[8].loadRelaxed(), 0u)
      << "version-mismatch Tier-2 must still clear the in-flight bit";

  // --- Phase C: compile failure clears the in-flight bit. ---
  pool.setCompiler(&mockCompileFailTier2, nullptr); // fail only PGOUse
  pool.setInstanceEnabled(5, 2, true);
  EJitDimPair dC[1] = {dim(5, 2)};
  ASSERT_EQ(pool.compileOrGet(9, dC, 1, codeFor(9)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne()); // Tier-1 succeeds (mockCompileFailTier2)
  {
    auto h = pool.compileOrGet(9, dC, 1, codeFor(9)); // arm -> enqueue
    if (h.hasReadToken)
      pool.releaseRead(h.bucketIndex);
  }
  EXPECT_NE(state_->inFlight[9].loadRelaxed(), 0u);
  ASSERT_TRUE(pool.pollOne()); // Tier-2 compile fails
  EXPECT_EQ(state_->inFlight[9].loadRelaxed(), 0u)
      << "failed Tier-2 compile must clear the in-flight bit";
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 1u)
      << "valid Tier-1 remains instrumented, so staged admission must stay "
         "with this function for a later Tier-2 retry";
}

} // namespace
