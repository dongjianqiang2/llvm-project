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
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace llvm::ejit;

#if defined(_WIN32)
// COFF retains the diagnostic dump routine from the directly compiled
// taskpool object even though these tests never call it. Keep this standalone
// test target independent of the full module-loader implementation.
namespace llvm::ejit {
const std::string &EJitModuleLoader::getFuncNameByFuncIdx(uint32_t) const {
  static const std::string Empty;
  return Empty;
}
} // namespace llvm::ejit
#endif

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

struct BoundPointerLog {
  uint32_t argIndex = 0;
  uint32_t size = 0;
  uint32_t value = 0;
  const void *rawPtr = nullptr;
  uint32_t boundCount = 0;
};
bool mockCompileBoundPointer(void *ctx, const EJitCompileRequest &req,
                             void **outFn) {
  auto *log = static_cast<BoundPointerLog *>(ctx);
  log->boundCount = req.boundCount;
  if (req.boundCount == 1) {
    const auto &Bound = req.boundPointers[0];
    log->argIndex = Bound.argIndex;
    log->size = Bound.size;
    log->rawPtr = Bound.rawPtr;
    if (Bound.size >= sizeof(log->value))
      std::memcpy(&log->value, Bound.rawPtr, sizeof(log->value));
  }
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

struct RankingLog {
  uint32_t calls = 0;
  uint32_t callbackCore = kEJitInvalidCoreId;
  bool succeed = true;
};
bool mockMayConstRanking(void *ctx) {
  auto *log = static_cast<RankingLog *>(ctx);
  ++log->calls;
  log->callbackCore = EJitCoreId::current();
  return log->succeed;
}

struct DriveOwnerCtx {
  EJitSharedTaskPool *owner = nullptr;
  uint32_t requesterCore = 1;
};
void driveOwnerOnRequesterIdle(void *ctx, uint32_t /*ticks*/) {
  auto *drive = static_cast<DriveOwnerCtx *>(ctx);
  EJitCoreId::setCurrentForTest(0);
  (void)drive->owner->workerPollOnce();
  EJitCoreId::setCurrentForTest(drive->requesterCore);
}

//===----------------------------------------------------------------------===//
// 4K page-seal mocks: a per-core split + per-page seal that log which core ran
// them and (optionally) fail or inject a concurrent slot/generation change at a
// chosen seal step (to exercise the re-validate-after-prepare protocol).
//===----------------------------------------------------------------------===//
struct FourKLog {
  std::vector<std::pair<uintptr_t, uint32_t>> splits;  // (poolBase, core)
  std::vector<std::pair<uintptr_t, uint32_t>> seals;   // (pageVA, core)
  std::vector<std::pair<uintptr_t, uint32_t>> rwPages; // (pageVA, core)
  bool splitOk = true;
  int failSealAtIndex = -1;           // fail the Nth (0-based) seal call
  bool rwOk = true;                   // enable_rw success/fail switch
  int failRwAtIndex = -1;             // fail the Nth (0-based) enable_rw call
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
// Per-core enable_rw of a JIT function's runtime-writable data pages (e.g. the
// Tier-1 __profc_ counters). Logs (pageVA, core) so a test can prove the peer
// made exactly the counter pages writable before executing.
bool mockEnableRwPage(void *ctx, uintptr_t pageVA) {
  auto *l = static_cast<FourKLog *>(ctx);
  int idx = static_cast<int>(l->rwPages.size());
  l->rwPages.push_back({pageVA, EJitCoreId::current()});
  if (l->failRwAtIndex == idx)
    return false;
  return l->rwOk;
}

// Owner-side resolver of a compiled pointer to a (test-controlled) executable
// range. Mutating the RangeCtx between compiles models distinct code extents.
struct RangeCtx {
  uintptr_t poolBase = 0x40000000ull;
  uint64_t poolSize = 0x200000ull; // 2 MiB
  uintptr_t codeStart = 0x40000000ull;
  uint64_t codeSize = 64;
  uint32_t poolId = 0;
  EJitCodePoolKind poolKind = EJitCodePoolKind::Near;
  bool provide = true;
  // Runtime-writable ranges (v9): 0 => none (non-PGO / Tier-2). When set,
  // models the Tier-1 __profc_ counter pages a peer core must enable_rw.
  uint32_t writableCount = 0;
  uintptr_t writableAddr[kEJitSharedMaxWritableRanges] = {};
  uint64_t writableSize[kEJitSharedMaxWritableRanges] = {};
  // 1 => fixed RX pool: a peer must enable_rw the writable pages. Defaults true
  // so the writable-range tests exercise the enable_rw path; a dynamic-pool
  // test sets it false.
  uint32_t requiresPeerEnableRw = 1;
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
  out->poolKind = r->poolKind;
  out->writableCount = r->writableCount;
  out->requiresPeerEnableRw = r->requiresPeerEnableRw;
  for (uint32_t i = 0; i < kEJitSharedMaxWritableRanges; ++i) {
    out->writableRanges[i].addr =
        (i < r->writableCount) ? r->writableAddr[i] : 0;
    out->writableRanges[i].size =
        (i < r->writableCount) ? r->writableSize[i] : 0;
  }
  return true;
}

struct BatchPublishCtx {
  std::vector<void *> linked;
  std::vector<void *> ready;
  std::vector<EJitCompileRequest> compileOrder;
  std::string timeline;
  unsigned flushCalls = 0;
  unsigned activeCompiles = 0;
  unsigned maxActiveCompiles = 0;
  bool failCompile = false;
  bool failFlush = false;
  bool tier1ReadyImmediately = false;
  bool recordTimeline = false;
};
bool mockBatchCompile(void *ctx, const EJitCompileRequest &req, void **outFn) {
  auto *B = static_cast<BatchPublishCtx *>(ctx);
  ++B->activeCompiles;
  B->maxActiveCompiles = std::max(B->maxActiveCompiles, B->activeCompiles);
  if (B->recordTimeline)
    B->timeline.push_back('C');
  B->compileOrder.push_back(req);
  if (B->failCompile) {
    --B->activeCompiles;
    *outFn = nullptr;
    return false;
  }
  uintptr_t Cell = req.numDims ? req.dims[0].instanceId : 0;
  *outFn = reinterpret_cast<void *>(0x500000ull + Cell * 64u +
                                    B->compileOrder.size() * 0x1000u);
  if (B->tier1ReadyImmediately &&
      decodeReqTier(req.funcIndex) == kEJitTierInstrumented)
    B->ready.push_back(*outFn);
  else
    B->linked.push_back(*outFn);
  --B->activeCompiles;
  return true;
}
bool mockBatchReady(void *ctx, const void *fnPtr) {
  auto *B = static_cast<BatchPublishCtx *>(ctx);
  return std::find(B->ready.begin(), B->ready.end(), fnPtr) != B->ready.end();
}
bool mockBatchFlush(void *ctx) {
  auto *B = static_cast<BatchPublishCtx *>(ctx);
  if (B->recordTimeline)
    B->timeline.push_back('F');
  ++B->flushCalls;
  if (B->failFlush)
    return false;
  B->ready.insert(B->ready.end(), B->linked.begin(), B->linked.end());
  B->linked.clear();
  return true;
}

struct BatchTimelineIdleCtx {
  BatchPublishCtx *batch = nullptr;
  EJitSharedTaskPoolState *state = nullptr;
};

void mockBatchTimelineIdle(void *ctx, uint32_t ticks) {
  auto *T = static_cast<BatchTimelineIdleCtx *>(ctx);
  if (ticks != 1u && T->batch->recordTimeline)
    T->batch->timeline.push_back('D');
  // Stop the real worker loop after the post-publish throttle has run.
  if (!T->batch->timeline.empty() && T->batch->timeline.back() == 'D' &&
      T->batch->timeline.find('F') != std::string::npos)
    T->state->initState.storeRelease(
        static_cast<uint32_t>(EJitSharedInitState::Stopping));
}

struct TwentyFunctionTimelineCtx {
  BatchPublishCtx *batch = nullptr;
  EJitSharedTaskPool *owner = nullptr;
  EJitSharedTaskPoolState *state = nullptr;
  uint32_t firstFunc = 0;
  uint32_t admitted = 0;
  uint32_t tier1Triggered = 0;
  unsigned observedFlushes = 0;
};

void twentyFunctionTimelineIdle(void *ctx, uint32_t ticks) {
  auto *T = static_cast<TwentyFunctionTimelineCtx *>(ctx);
  if (ticks == 1u)
    return;
  T->batch->timeline.push_back('D');

  uint32_t Tier1Compiled = 0;
  uint32_t Tier2Compiled = 0;
  for (const EJitCompileRequest &Req : T->batch->compileOrder) {
    if (decodeReqTier(Req.funcIndex) == kEJitTierInstrumented)
      ++Tier1Compiled;
    else if (decodeReqTier(Req.funcIndex) == kEJitTierPgoUse)
      ++Tier2Compiled;
  }

  // Once every Tier-1 in the current four-function wave is executable, model
  // one threshold-crossing business hit per function to queue Tier-2.
  if (Tier1Compiled == T->admitted && T->tier1Triggered < T->admitted) {
    while (T->tier1Triggered < T->admitted) {
      const uint32_t Func = T->firstFunc + T->tier1Triggered++;
      auto Hit = T->owner->tryCacheHit0D(Func);
      if (Hit.hasReadToken)
        T->owner->releaseRead(Hit.bucketIndex);
    }
  }

  if (T->batch->flushCalls == T->observedFlushes)
    return;
  T->observedFlushes = T->batch->flushCalls;
  if (T->admitted < 20u) {
    const uint32_t WaveEnd = std::min(T->admitted + 4u, 20u);
    while (T->admitted < WaveEnd) {
      const uint32_t Func = T->firstFunc + T->admitted++;
      (void)T->owner->compileOrGet(Func, nullptr, 0, codeFor(Func));
    }
    return;
  }
  if (Tier2Compiled == 20u)
    T->state->initState.storeRelease(
        static_cast<uint32_t>(EJitSharedInitState::Stopping));
}
bool mockBatchRange(void *ctx, const void *fnPtr, EJitCompiledCodeInfo *out) {
  if (!mockBatchReady(ctx, fnPtr))
    return false;
  uintptr_t Addr = reinterpret_cast<uintptr_t>(fnPtr);
  out->fnPtr = const_cast<void *>(fnPtr);
  out->codeStart = Addr;
  out->codeSize = 64;
  out->poolBase = Addr & ~static_cast<uintptr_t>(0x1fffff);
  out->poolSize = 0x200000;
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

struct PublishObserver {
  uint32_t calls = 0;
  bool lastPublished = false;
  uint32_t lastBoundCount = 0;
  const void *lastRawPtr = nullptr;

  static void notify(void *ctx, const EJitCompileRequest &req, bool published) {
    auto *self = static_cast<PublishObserver *>(ctx);
    ++self->calls;
    self->lastPublished = published;
    self->lastBoundCount = req.boundCount;
    self->lastRawPtr = req.boundCount ? req.boundPointers[0].rawPtr : nullptr;
  }
};

// Injectable worker hooks. The entry is never run on a real thread here; tests
// drive pollOne() manually, so these only prove "exactly one worker started".
struct WorkerHooks {
  int starts = 0;
  int stops = 0;
  bool failNext = false;
  uint32_t startCore = kEJitInvalidCoreId;
};
bool mockWorkerStart(void *ctx, EJitSharedTaskPool::WorkerEntryFn /*entry*/,
                     void * /*entryCtx*/, uint64_t *outTaskId) {
  auto *w = static_cast<WorkerHooks *>(ctx);
  if (w->failNext)
    return false;
  ++w->starts;
  w->startCore = EJitCoreId::current();
  *outTaskId = 0xABCDull;
  return true;
}
void mockWorkerStop(void *ctx) { ++static_cast<WorkerHooks *>(ctx)->stops; }

// Stands in for building/releasing the owner's ORC engine, and records what the
// blob looked like WHILE it ran -- the ordering against the worker start and
// the Ready publish is the contract, not just that it ran.
struct OwnerEngineLog {
  int built = 0;
  int released = 0;
  bool failBuild = false;
  uint32_t coreAtBuild = kEJitInvalidCoreId;
  uint32_t stateAtBuild = 0xFFFFFFFFu;
  uint64_t taskIdAtBuild = ~0ull;
  int workerStartsAtBuild = -1;
  uint32_t stateAtRelease = 0xFFFFFFFFu;
  int workerStopsAtRelease = -1;
  EJitSharedTaskPoolState *state = nullptr;
  const WorkerHooks *hooks = nullptr;
};
bool mockOwnerElected(void *ctx) {
  auto *l = static_cast<OwnerEngineLog *>(ctx);
  ++l->built;
  l->coreAtBuild = EJitCoreId::current();
  if (l->state) {
    l->stateAtBuild = l->state->initState.loadAcquire();
    l->taskIdAtBuild = l->state->workerTaskId.loadAcquire();
  }
  if (l->hooks)
    l->workerStartsAtBuild = l->hooks->starts;
  return !l->failBuild;
}
void mockOwnerReleased(void *ctx) {
  auto *l = static_cast<OwnerEngineLog *>(ctx);
  ++l->released;
  if (l->state)
    l->stateAtRelease = l->state->initState.loadAcquire();
  if (l->hooks)
    l->workerStopsAtRelease = l->hooks->stops;
}

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
    // Unregister every icache slot around EVERY test, not just the icache ones.
    // The registry is process-static and its bases are test-local arrays, so a
    // slot left registered by a test that returned early (a failed ASSERT)
    // would have icacheDrainAll() -- which any setInstanceEnabled reaches --
    // write through a dangling pointer in the next test.
    ejitIcacheClearAll();
    state_ = std::make_unique<EJitSharedTaskPoolState>();
  }
  void TearDown() override {
    ejitIcacheClearAll();
    EJitCoreId::resetForTest();
  }

  // Register a test-local stand-in for the wrapper's @__ejit_icache_fn_<name>
  // cell table. \p missFn non-null models a SENTINEL-form slot (NumDims <= 2,
  // timing off): the table is defined pre-filled with &MissFn and the runtime
  // must write that value back on drain / retract.
  void registerSlot(uint32_t funcIndex, void *base, uint32_t numDims,
                    const void *missFn = nullptr) {
    ASSERT_EQ(ejitIcacheRegisterSlot(funcIndex, base, numDims, missFn),
              EJitIcacheRegResult::Ok);
  }

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
    pool.setEnableRwPageCallback(&mockEnableRwPage, &fourK);
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

TEST_F(SharedTaskPoolTest, PeerRequestsMayConstRankingFromOwnerWorker) {
  EJitSharedTaskPool owner;
  RankingLog log;
  owner.setMayConstRankingCallback(&mockMayConstRanking, &log);
  bringUpOwner(owner);

  EJitSharedTaskPool peer;
  DriveOwnerCtx drive{&owner, 1};
  peer.bind(state_.get());
  peer.setMode(EJitCompileMode::Async);
  peer.setWorkerIdleHook(&driveOwnerOnRequesterIdle, &drive);
  EJitCoreId::setCurrentForTest(1);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);

  EXPECT_TRUE(peer.requestMayConstRanking());
  EXPECT_EQ(log.calls, 1u);
  EXPECT_EQ(log.callbackCore, 0u);
  EXPECT_EQ(state_->mayConstRankingRequest.loadAcquire(), 1u);
  EXPECT_EQ(state_->mayConstRankingComplete.loadAcquire(), 1u);
  EXPECT_EQ(state_->mayConstRankingResult.loadAcquire(), 1u);
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
// 3a/ Fixed worker core (build policy EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE).
//
// Compiled only in the opt-in fixed-core test config (-DEJIT_TEST_FIXED_WORKER_
// CORE=ON pins the designated core to 0, matching every bringUpOwner in this
// suite). The policy closes the open election: only the designated core may
// claim the blob; a non-designated core waits bounded (yielding) for it, never
// attempts the CAS, and never hangs.
//===----------------------------------------------------------------------===//
#ifdef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE

// The designated core wins and every other core attaches as a peer: same
// observable outcome as the open election, but the owner identity is pinned.
TEST_F(SharedTaskPoolTest, FixedWorkerCoreDesignatedCoreWinsAndPeerAttaches) {
  EJitSharedTaskPool owner, peer;
  for (auto *pool : {&owner, &peer}) {
    pool->bind(state_.get());
    pool->setCompiler(&mockCompile, nullptr);
    pool->setMode(EJitCompileMode::Async);
  }

  EJitCoreId::setCurrentForTest(0); // the designated core
  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  EXPECT_EQ(state_->ownerCoreId.loadAcquire(), 0u);
}

// A non-designated core that activates FIRST must not claim the blob: it waits
// bounded, the blob stays Uninitialized, no election attempt is ever counted,
// and the designated core can still claim it afterwards. No idle hook is
// injected here, so the wait budget burns cpuRelax iterations - fast enough
// for a unit test (production always injects the yield hook, where the same
// budget costs scheduler ticks instead).
TEST_F(SharedTaskPoolTest, FixedWorkerCorePeerNeverClaimsTheBlob) {
  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  peer.setCompiler(&mockCompile, nullptr);
  peer.setMode(EJitCompileMode::Async);

  EJitCoreId::setCurrentForTest(5); // not the designated core (0)
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::InitInProgress);
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
  EXPECT_EQ(state_->initAttempts.loadAcquire(), 0u)
      << "a non-designated core must not count as an election attempt";

  // The designated core can still claim the blob the peer left untouched.
  EJitSharedTaskPool owner;
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setMode(EJitCompileMode::Async);
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(state_->initAttempts.loadAcquire(), 1u);
}

// Bring-up order must not matter: a peer already WAITING when the designated
// core initializes attaches as soon as Ready is published. Simulated
// deterministically with the injectable idle hook - the peer's wait loop calls
// it, and on its first invocation the designated core's init() runs to
// completion (a second pool object on the same blob), modeling another core
// coming up concurrently.
TEST_F(SharedTaskPoolTest, FixedWorkerCorePeerWaitsThenAttaches) {
  EJitSharedTaskPool designated;
  designated.bind(state_.get());
  designated.setCompiler(&mockCompile, nullptr);
  designated.setMode(EJitCompileMode::Async);

  struct WaitCtx {
    EJitSharedTaskPool *designated;
    bool ran;
  } ctx{&designated, false};
  auto idleHook = [](void *c, uint32_t) {
    auto *w = static_cast<WaitCtx *>(c);
    if (w->ran)
      return; // one-shot: afterwards the loop re-observes Ready and exits
    w->ran = true;
    EJitCoreId::setCurrentForTest(0); // the designated core comes up now
    EXPECT_EQ(w->designated->init(),
              EJitSharedTaskPool::InitResult::BecameOwner);
    EJitCoreId::setCurrentForTest(1); // back to the waiting peer's core
  };

  EJitSharedTaskPool peer;
  peer.bind(state_.get());
  peer.setCompiler(&mockCompile, nullptr);
  peer.setMode(EJitCompileMode::Async);
  peer.setWorkerIdleHook(idleHook, &ctx);

  EJitCoreId::setCurrentForTest(1); // peer activates BEFORE the designated core
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  EXPECT_EQ(state_->ownerCoreId.loadAcquire(), 0u);
  EXPECT_EQ(state_->initAttempts.loadAcquire(), 1u);
}

// After a shutdown the blob is Uninitialized again: a peer still cannot win
// the re-election, only the designated core can.
TEST_F(SharedTaskPoolTest, FixedWorkerCoreOnlyDesignatedCoreReelects) {
  EJitSharedTaskPool owner, peer;
  for (auto *pool : {&owner, &peer}) {
    pool->bind(state_.get());
    pool->setCompiler(&mockCompile, nullptr);
    pool->setMode(EJitCompileMode::Async);
  }

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.ownerShutdown();
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
  // initAttempts is monotonic (a total, not a per-generation count): the first
  // owner's attempt is still on the books after the shutdown.
  ASSERT_EQ(state_->initAttempts.loadAcquire(), 1u);

  // The peer (core 1) may not re-win the freed blob: bounded wait, no attempt.
  EJitCoreId::setCurrentForTest(1);
  EXPECT_EQ(peer.init(), EJitSharedTaskPool::InitResult::InitInProgress);
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
  EXPECT_EQ(state_->initAttempts.loadAcquire(), 1u);

  // The designated core re-wins (its second attempt).
  EJitCoreId::setCurrentForTest(0);
  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(state_->initAttempts.loadAcquire(), 2u);
}

#endif // EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE

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

TEST_F(SharedTaskPoolTest, BoundPointerSharedAcrossCores) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  BoundPointerLog log;
  owner.setCompiler(&mockCompileBoundPointer, &log);

  EJitCoreId::setCurrentForTest(2);
  uint32_t callerValue = 0x12345678u;
  auto r = owner.compileOrGet(101, nullptr, 0, codeFor(101), &callerValue,
                              sizeof(callerValue), 3);
  ASSERT_EQ(r.status, EJitCompileOrGetStatus::EnqueuedPending);

  // The producer keeps the shared object alive until the worker callback
  // returns. The queue carries its address and size, not a payload copy.
  EJitCoreId::setCurrentForTest(0);
  ASSERT_TRUE(owner.pollOne());
  EXPECT_EQ(log.boundCount, 1u);
  EXPECT_EQ(log.rawPtr, static_cast<const void *>(&callerValue));
  EXPECT_EQ(log.argIndex, 3u);
  EXPECT_EQ(log.size, sizeof(callerValue));
  EXPECT_EQ(log.value, 0x12345678u);
}

TEST_F(SharedTaskPoolTest, BoundPointerDescriptorsAreFixedAndValidated) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  BoundPointerLog log;
  owner.setCompiler(&mockCompileBoundPointer, &log);

  uint8_t Data[kEJitMaxBoundPointers][2048] = {};
  EJitBoundPtrDescriptor Bounds[kEJitMaxBoundPointers] = {};
  for (uint32_t I = 0; I < kEJitMaxBoundPointers; ++I)
    Bounds[I] = {Data[I], sizeof(Data[I]), I};

  EJitCoreId::setCurrentForTest(3);
  auto accepted = owner.compileOrGet(102, nullptr, 0, codeFor(102), Bounds,
                                     kEJitMaxBoundPointers);
  ASSERT_EQ(accepted.status, EJitCompileOrGetStatus::EnqueuedPending);
  EJitCoreId::setCurrentForTest(0);
  ASSERT_TRUE(owner.pollOne());
  EXPECT_EQ(log.boundCount, kEJitMaxBoundPointers);

  EJitBoundPtrDescriptor Invalid{nullptr, sizeof(Data[0]), 0};
  EXPECT_EQ(
      owner.compileOrGet(103, nullptr, 0, codeFor(103), &Invalid, 1).status,
      EJitCompileOrGetStatus::InvalidParam);
  Invalid = {Data[0], 0, 0};
  EXPECT_EQ(
      owner.compileOrGet(104, nullptr, 0, codeFor(104), &Invalid, 1).status,
      EJitCompileOrGetStatus::InvalidParam);
  Invalid = {reinterpret_cast<const void *>(
                 std::numeric_limits<uintptr_t>::max() - 3u),
             8, 0};
  EXPECT_EQ(
      owner.compileOrGet(105, nullptr, 0, codeFor(105), &Invalid, 1).status,
      EJitCompileOrGetStatus::InvalidParam);
}

TEST_F(SharedTaskPoolTest, PgoBoundPointerIsReadOnlyDuringCompile) {
  EJitSharedTaskPool owner;
  BatchPublishCtx Batch;
  PublishObserver Observer;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockBatchCompile, &Batch);
  owner.setPublishCallback(&PublishObserver::notify, &Observer);
  owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  owner.setPgoEnabled(true, 1);

  uint8_t Data[2048] = {};
  Data[0] = 0x5a;
  EJitBoundPtrDescriptor Bound{Data, sizeof(Data), 1};
  ASSERT_EQ(owner.compileOrGet(106, nullptr, 0, codeFor(106), &Bound, 1).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());
  ASSERT_EQ(Batch.compileOrder.size(), 1u);
  EXPECT_EQ(Batch.compileOrder[0].boundCount, 1u);
  EXPECT_EQ(Batch.compileOrder[0].boundPointers[0].rawPtr, Data);
  ASSERT_EQ(owner.pendingPublishCount(), 1u);
  ASSERT_TRUE(owner.flushCodeBatch());
  EXPECT_EQ(Observer.lastBoundCount, 0u);
  EXPECT_EQ(Observer.lastRawPtr, nullptr);

  auto hit = owner.compileOrGet(106, nullptr, 0, codeFor(106), &Bound, 1);
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  if (hit.hasReadToken)
    owner.releaseRead(hit.bucketIndex);
  ASSERT_EQ(owner.pendingCount(), 1u);

  ASSERT_TRUE(owner.pollOne());
  ASSERT_EQ(Batch.compileOrder.size(), 2u);
  EXPECT_EQ(Batch.compileOrder[1].boundCount, 1u);
  EXPECT_EQ(Batch.compileOrder[1].boundPointers[0].rawPtr, Data);
  Data[0] = 0xa5;
  ASSERT_TRUE(owner.flushCodeBatch());
  EXPECT_EQ(Observer.lastBoundCount, 0u);
  EXPECT_EQ(Observer.lastRawPtr, nullptr);
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

TEST_F(SharedTaskPoolTest, BatchPendingDedupsFullKeyAndAllowsNextCell) {
  BatchPublishCtx Batch;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setCodeSharingEnabled(true);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 0, true);
  Owner.setInstanceEnabled(0, 1, true);

  EJitDimPair Cell0[1] = {dim(0, 0)};
  EJitDimPair Cell1[1] = {dim(0, 1)};
  EXPECT_EQ(Owner.compileOrGet(42, Cell0, 1, codeFor(42)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(Owner.pendingBatchCompileCount(), 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_TRUE(Batch.compileOrder.empty());
  EXPECT_EQ(Owner.pendingCount(), 0u);

  EXPECT_EQ(Owner.compileOrGet(42, Cell0, 1, codeFor(42)).status,
            EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(Owner.compileOrGet(42, Cell1, 1, codeFor(42)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(Owner.pendingBatchCompileCount(), 2u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(Batch.flushCalls, 0u);

#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
  for (unsigned I = 0; I != 64; ++I)
    EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Batch.flushCalls, 0u);

  std::atomic<int> PublishResult{-1};
  std::thread Caller([&] {
    PublishResult.store(Owner.requestCodeBatchFlushAndWait() ? 1 : 0,
                        std::memory_order_release);
  });
  while (state_->codeBatchRequestState.loadAcquire() !=
         static_cast<uint32_t>(EJitCodeBatchRequestState::Requested))
    std::this_thread::yield();
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  Caller.join();
  EXPECT_EQ(PublishResult.load(std::memory_order_acquire), 1);
#else
  ASSERT_TRUE(Owner.flushCodeBatch());
#endif
  EXPECT_EQ(Batch.flushCalls, 1u);
  ASSERT_EQ(Batch.compileOrder.size(), 2u);
  EXPECT_EQ(Batch.compileOrder[0].dims[0].instanceId, 0u);
  EXPECT_EQ(Batch.compileOrder[1].dims[0].instanceId, 1u);
  EXPECT_EQ(Owner.pendingBatchCompileCount(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  auto Hit0 = Owner.compileOrGet(42, Cell0, 1, codeFor(42));
  auto Hit1 = Owner.compileOrGet(42, Cell1, 1, codeFor(42));
  EXPECT_EQ(Hit0.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(Hit1.status, EJitCompileOrGetStatus::CacheHit);
  Owner.releaseRead(Hit0.bucketIndex);
  Owner.releaseRead(Hit1.bucketIndex);
}

TEST_F(SharedTaskPoolTest, PartialBatchCallbacksDisableStaging) {
  BatchPublishCtx Batch;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, nullptr, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(43, nullptr, 0, codeFor(43)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(Batch.flushCalls, 0u);
  auto Hit = Owner.compileOrGet(43, nullptr, 0, codeFor(43));
  EXPECT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  Owner.releaseRead(Hit.bucketIndex);
}

#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
TEST_F(SharedTaskPoolTest, ExplicitBatchPublishDrainsPreexistingQueueFirst) {
  BatchPublishCtx Batch;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(48, nullptr, 0, codeFor(48)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.compileOrGet(47, nullptr, 0, codeFor(47)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingBatchCompileCount(), 1u);

  std::atomic<int> PublishResult{-1};
  std::thread Caller([&] {
    PublishResult.store(Owner.requestCodeBatchFlushAndWait() ? 1 : 0,
                        std::memory_order_release);
  });
  while (state_->codeBatchRequestState.loadAcquire() !=
         static_cast<uint32_t>(EJitCodeBatchRequestState::Requested))
    std::this_thread::yield();
  const uint64_t IdleBeforePublish = Owner.workerIdleYields();
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  Caller.join();

  EXPECT_EQ(PublishResult.load(std::memory_order_acquire), 1);
  ASSERT_EQ(Batch.compileOrder.size(), 2u);
  EXPECT_EQ(stripReqTier(Batch.compileOrder[0].funcIndex), 47u);
  EXPECT_EQ(stripReqTier(Batch.compileOrder[1].funcIndex), 48u);
#if EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT != 0u &&                            \
    EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS != 0u
  // One queue drain plus two sorted ORC compiles happen inside this publish
  // step. Every operation retains the configured scheduling gap.
  EXPECT_EQ(Owner.workerIdleYields(), IdleBeforePublish + 3u);
#endif
  EJitSharedDiagnostics D{};
  Owner.getDiagnostics(D);
  EXPECT_EQ(D.queueDepth, 0u);
}
#endif

TEST_F(SharedTaskPoolTest, BatchPgoTier1PublishesImmediatelyFromFarPool) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 8);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(44, nullptr, 0, codeFor(44)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Batch.compileOrder.size(), 1u);
  EXPECT_EQ(decodeReqTier(Batch.compileOrder[0].funcIndex),
            kEJitTierInstrumented);
  EXPECT_EQ(Owner.pendingBatchCompileCount(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(Batch.flushCalls, 0u);
  auto Hit = Owner.compileOrGet(44, nullptr, 0, codeFor(44));
  EXPECT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  Owner.releaseRead(Hit.bucketIndex);
}

TEST_F(SharedTaskPoolTest, BatchPgoTier2AutoPublishesWhenQueueDrains) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(46, nullptr, 0, codeFor(46)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Batch.compileOrder.size(), 1u);
  EXPECT_EQ(decodeReqTier(Batch.compileOrder[0].funcIndex),
            kEJitTierInstrumented);
  EXPECT_EQ(Batch.flushCalls, 0u);

  auto Tier1 = Owner.tryCacheHit0D(46);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_EQ(Owner.pendingCount(), 1u);

  // Tier-2 compiles and links immediately, but remains owner-private and RW/NX
  // until the worker observes the compile queue empty. Tier-1 stays allocated
  // for counter synthesis, but dispatch falls back to AOT once sampling is
  // complete instead of continuing atomic instrumentation while waiting.
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(Batch.flushCalls, 0u);
  ASSERT_EQ(Batch.compileOrder.size(), 2u);
  EXPECT_EQ(decodeReqTier(Batch.compileOrder[1].funcIndex), kEJitTierPgoUse);
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Owner.pendingCount(), 1u);

  auto StillTier1 = Owner.tryCacheHit0D(46);
  EXPECT_EQ(StillTier1.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(StillTier1.fnPtr, nullptr);
  EXPECT_TRUE(StillTier1.fastPathTerminal);

#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
#else
  ASSERT_TRUE(Owner.flushCodeBatch());
#endif
  EXPECT_EQ(Batch.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(Owner.pendingCount(), 0u);

  EJitSharedDiagnostics D{};
  Owner.getDiagnostics(D);
  EXPECT_EQ(D.tier1Compiles, 1u);
  EXPECT_EQ(D.tier2Compiles, 1u);
  auto Tier2 = Owner.tryCacheHit0D(46);
  EXPECT_EQ(Tier2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(Tier2.fnPtr, Tier1.fnPtr);
  if (Tier2.hasReadToken)
    Owner.releaseRead(Tier2.bucketIndex);
}

#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
TEST_F(SharedTaskPoolTest, BatchPgoAutoPublishWaitsForQueuedTier1) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, /*maxConcurrentProfiles=*/2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(52, nullptr, 0, codeFor(52)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto FirstTier1 = Owner.tryCacheHit0D(52);
  ASSERT_EQ(FirstTier1.status, EJitCompileOrGetStatus::CacheHit);
  if (FirstTier1.hasReadToken)
    Owner.releaseRead(FirstTier1.bucketIndex);

  // Queue order is Tier-2(func 52), then Tier-1(func 53). Auto-publish must not
  // run between them: Tier-1 uses the far immediate pool and must start
  // profiling before the near-pool batch is sealed.
  ASSERT_EQ(Owner.compileOrGet(53, nullptr, 0, codeFor(53)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Batch.flushCalls, 0u);
  ASSERT_EQ(Batch.compileOrder.size(), 2u);
  EXPECT_EQ(decodeReqTier(Batch.compileOrder[1].funcIndex), kEJitTierPgoUse);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Batch.flushCalls, 0u);
  ASSERT_EQ(Batch.compileOrder.size(), 3u);
  EXPECT_EQ(stripReqTier(Batch.compileOrder[2].funcIndex), 53u);
  EXPECT_EQ(decodeReqTier(Batch.compileOrder[2].funcIndex),
            kEJitTierInstrumented);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Batch.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(SharedTaskPoolTest, BatchPgoFourTier2CompilesRetainWorkerThrottle) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, /*maxConcurrentProfiles=*/4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  // Model the board workload: four admitted functions publish Tier-1 first,
  // then their threshold hits queue four Tier-2 compilations together.
  for (uint32_t Func = 60; Func != 64; ++Func) {
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
  }
  for (uint32_t Func = 60; Func != 64; ++Func) {
    auto Hit = Owner.tryCacheHit0D(Func);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    if (Hit.hasReadToken)
      Owner.releaseRead(Hit.bucketIndex);
  }
  ASSERT_EQ(Owner.pendingCount(), 4u);

  Batch.timeline.clear();
  Batch.activeCompiles = 0;
  Batch.maxActiveCompiles = 0;
  Batch.recordTimeline = true;
  BatchTimelineIdleCtx Idle{&Batch, state_.get()};
  Owner.setWorkerIdleHook(&mockBatchTimelineIdle, &Idle);
  Owner.runWorkerLoop();

  // C=compile, D=worker throttle, F=enable_ex/cache publication. There must be
  // a scheduling gap between every Tier-2 compile and after publication.
  EXPECT_EQ(Batch.timeline, "CDCDCDCDFD");
  EXPECT_EQ(Batch.maxActiveCompiles, 1u);
  EXPECT_EQ(Batch.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(SharedTaskPoolTest, ExplicitPublishFourTier2CompilesRetainThrottle) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, /*maxConcurrentProfiles=*/4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  for (uint32_t Func = 64; Func != 68; ++Func) {
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
  }
  for (uint32_t Func = 64; Func != 68; ++Func) {
    auto Hit = Owner.tryCacheHit0D(Func);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    if (Hit.hasReadToken)
      Owner.releaseRead(Hit.bucketIndex);
  }
  ASSERT_EQ(Owner.pendingCount(), 4u);

  Batch.timeline.clear();
  Batch.activeCompiles = 0;
  Batch.maxActiveCompiles = 0;
  Batch.recordTimeline = true;
  BatchTimelineIdleCtx Idle{&Batch, state_.get()};
  Owner.setWorkerIdleHook(&mockBatchTimelineIdle, &Idle);

  std::atomic<int> PublishResult{-1};
  std::thread Caller([&] {
    PublishResult.store(Owner.requestCodeBatchFlushAndWait() ? 1 : 0,
                        std::memory_order_release);
  });
  while (state_->codeBatchRequestState.loadAcquire() !=
         static_cast<uint32_t>(EJitCodeBatchRequestState::Requested))
    std::this_thread::yield();
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  Caller.join();

  EXPECT_EQ(PublishResult.load(std::memory_order_acquire), 1);
  EXPECT_EQ(Batch.timeline, "CDCDCDCDF");
  EXPECT_EQ(Batch.maxActiveCompiles, 1u);
  EXPECT_EQ(Batch.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(SharedTaskPoolTest, BatchPgoTwentyFunctionsRunInFiveThrottledWaves) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  Batch.recordTimeline = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, /*maxConcurrentProfiles=*/4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t FirstFunc = 100;
  TwentyFunctionTimelineCtx Timeline{&Batch, &Owner, state_.get(), FirstFunc,
                                     /*admitted=*/4};
  Owner.setWorkerIdleHook(&twentyFunctionTimelineIdle, &Timeline);
  for (uint32_t Func = FirstFunc; Func != FirstFunc + 4u; ++Func)
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);

  Owner.runWorkerLoop();

  std::string Expected;
  for (unsigned Wave = 0; Wave != 5; ++Wave) {
    for (unsigned Compile = 0; Compile != 8; ++Compile)
      Expected += "CD";
    Expected += "FD";
  }
  EXPECT_EQ(Batch.timeline, Expected);
  EXPECT_EQ(Batch.timeline.find("CC"), std::string::npos);
  EXPECT_EQ(Batch.compileOrder.size(), 40u);
  EXPECT_EQ(Batch.maxActiveCompiles, 1u);
  EXPECT_EQ(Batch.flushCalls, 5u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(SharedTaskPoolTest, BatchPgoAutoPublishFailureWaitsForExplicitRetry) {
  BatchPublishCtx Batch;
  Batch.tier1ReadyImmediately = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(54, nullptr, 0, codeFor(54)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Tier1 = Owner.tryCacheHit0D(54);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);

  Batch.failFlush = true;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Batch.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  // A failed automatic attempt is not retried in a busy loop.
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Batch.flushCalls, 1u);

  Batch.failFlush = false;
  EXPECT_TRUE(Owner.flushCodeBatch());
  EXPECT_EQ(Batch.flushCalls, 2u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}
#endif

TEST_F(SharedTaskPoolTest, BatchCompileLayoutSortsFirstThenSecondDimension) {
  BatchPublishCtx Batch;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeRangeProvider(&mockBatchRange, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setCodeSharingEnabled(true);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  for (uint32_t Cell = 0; Cell != 3; ++Cell)
    Owner.setInstanceEnabled(0, Cell, true);
  for (uint32_t Trp = 0; Trp != 4; ++Trp)
    Owner.setInstanceEnabled(1, Trp, true);

  const EJitDimPair Cell2Trp1[2] = {dim(0, 2), dim(1, 1)};
  const EJitDimPair Cell0Trp3[2] = {dim(0, 0), dim(1, 3)};
  const EJitDimPair Cell0Trp1[2] = {dim(0, 0), dim(1, 1)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  struct Request {
    uint32_t Func;
    const EJitDimPair *Dims;
    uint32_t NumDims;
  };
  const Request Requests[] = {{12, Cell2Trp1, 2},
                              {11, Cell0Trp3, 2},
                              {13, Cell0Trp1, 2},
                              {10, Cell1, 1},
                              {9, Cell0Trp1, 2}};
  for (const Request &R : Requests)
    ASSERT_EQ(
        Owner.compileOrGet(R.Func, R.Dims, R.NumDims, codeFor(R.Func)).status,
        EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.pollBudget(5), 5u);
  EXPECT_TRUE(Batch.compileOrder.empty());

  ASSERT_TRUE(Owner.flushCodeBatch());
  ASSERT_EQ(Batch.compileOrder.size(), 5u);
  const uint32_t ExpectedFunc[] = {9, 13, 11, 10, 12};
  const uint32_t ExpectedCell[] = {0, 0, 0, 1, 2};
  const uint32_t ExpectedTrp[] = {1, 1, 3, UINT32_MAX, 1};
  for (size_t I = 0; I != Batch.compileOrder.size(); ++I) {
    const EJitCompileRequest &Compiled = Batch.compileOrder[I];
    EXPECT_EQ(stripReqTier(Compiled.funcIndex), ExpectedFunc[I]);
    EXPECT_EQ(Compiled.dims[0].instanceId, ExpectedCell[I]);
    EXPECT_EQ(Compiled.numDims > 1 ? Compiled.dims[1].instanceId : UINT32_MAX,
              ExpectedTrp[I]);
  }
}

TEST_F(SharedTaskPoolTest, BatchCompileFailureDropsRequestMarkerForRetry) {
  BatchPublishCtx Batch;
  Batch.failCompile = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(state_.get());
  Owner.setCompiler(&mockBatchCompile, &Batch);
  Owner.setCodeBatchCallbacks(&mockBatchReady, &mockBatchFlush, &Batch);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 3, true);

  const EJitDimPair Cell3[1] = {dim(0, 3)};
  ASSERT_EQ(Owner.compileOrGet(45, Cell3, 1, codeFor(45)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_TRUE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingBatchCompileCount(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(Owner.compileOrGet(45, Cell3, 1, codeFor(45)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
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
  PublishObserver observer;
  owner.setCompiler(&mockCompileThenToggle, &tctx);
  owner.setPublishCallback(&PublishObserver::notify, &observer);
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
  EXPECT_EQ(observer.calls, 1u);
  EXPECT_FALSE(observer.lastPublished);
}

TEST_F(SharedTaskPoolTest, PublishCallbackRunsAfterCommit) {
  EJitSharedTaskPool owner;
  PublishObserver observer;
  bringUpOwner(owner);
  owner.setPublishCallback(&PublishObserver::notify, &observer);
  publish(owner, 10);
  EXPECT_EQ(observer.calls, 1u);
  EXPECT_TRUE(observer.lastPublished);
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
// forEachCompiled walk stats: visited Ready slots and skipped (write-lock
// contended) buckets are reported, so a diagnostic dump is never silently
// incomplete.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, ForEachCompiledReportsVisitedAndSkipped) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  EJitCoreId::setCurrentForTest(0);

  // Two published entries: one 0-dim, one 1-dim.
  publish(owner, 30);
  owner.setInstanceEnabled(1, 4, true);
  EJitDimPair d0[1] = {dim(1, 4)};
  ASSERT_EQ(owner.compileOrGet(31, d0, 1, codeFor(31)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_TRUE(owner.pollOne());

  auto isReady = [&](uint32_t b, uint32_t s) {
    return state_->buckets[b].slots[s].state.loadAcquire() ==
           static_cast<uint32_t>(EJitSharedSlotState::Ready);
  };
  uint32_t readyBefore = 0;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s)
      readyBefore += isReady(b, s);
  ASSERT_EQ(readyBefore, 2u);

  struct Count {
    uint32_t visited = 0;
  };
  auto cb = [](const EJitSharedCacheSlot &slot, void *ctx) {
    ++static_cast<Count *>(ctx)->visited;
    (void)slot;
  };
  Count count;
  auto st = owner.forEachCompiled(cb, &count);
  EXPECT_EQ(count.visited, 2u);
  EXPECT_EQ(st.visitedSlots, 2u);
  EXPECT_EQ(st.skippedBuckets, 0u);

  // Hold bucket 0's writer flag: the walk's read-lock retries all fail, the
  // bucket is skipped, and the callback only sees the other buckets.
  state_->buckets[0].writeFlag.storeRelaxed(1);
  uint32_t readyInB0 = 0;
  for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s)
    readyInB0 += isReady(0, s);

  Count count2;
  auto st2 = owner.forEachCompiled(cb, &count2);
  EXPECT_EQ(st2.skippedBuckets, 1u);
  EXPECT_EQ(st2.visitedSlots, 2u - readyInB0);
  EXPECT_EQ(count2.visited, st2.visitedSlots);

  state_->buckets[0].writeFlag.storeRelaxed(0);

  // Every walk paired its bucketTryRead with bucketReadRelease: no read
  // tokens are left held on any bucket (0 in both lock modes: the token
  // build decrements back to 0, NO_RECLAIM never increments).
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    EXPECT_EQ(state_->buckets[b].readers.loadAcquire(), 0u) << "bucket " << b;
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
  // Record the ticks arg the idle/delay hook was called with, so tests can
  // assert the throttle path passes MULT*DELAY_TICKS (not 1) and the wait/idle
  // path passes 1.
  uint32_t maxTicks = 0;
  bool sawWaitTicks = false;
};
void scriptedIdle(void *ctx, uint32_t ticks) {
  auto *s = static_cast<IdleScript *>(ctx);
  ++s->idleCalls;
  if (ticks > s->maxTicks)
    s->maxTicks = ticks;
  if (ticks == 1u)
    s->sawWaitTicks = true;
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
  // The throttle path (after the consume) must pass MULT*DELAY_TICKS in ONE
  // call (not 1, not a per-tick loop); the wait/idle path must pass 1.
  EXPECT_EQ(
      script.maxTicks,
      static_cast<uint32_t>(EJIT_SRE_TASKPOOL_WORKER_THROTTLE_MULT *
                            EJIT_SRE_TASKPOOL_WORKER_THROTTLE_DELAY_TICKS));
  EXPECT_TRUE(script.sawWaitTicks);
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
#elif defined(EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE)
  // Fixed worker core build: the election is closed to the designated core
  // (0 in the test config), so a non-zero core id must NOT win - it waits and
  // the blob stays Uninitialized.
  EJitSharedTaskPool pool;
  pool.bind(state_.get());
  pool.setCompiler(&mockCompile, nullptr);
  pool.setMode(EJitCompileMode::Async);
  EJitCoreId::setCurrentForTest(7);
  EXPECT_EQ(pool.init(), EJitSharedTaskPool::InitResult::InitInProgress);
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Uninitialized));
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

//===----------------------------------------------------------------------===//
// Cross-core runtime-writable (Online-PGO Tier-1) peer preparation (v9).
//
// A JIT function whose body writes runtime data (e.g. the Tier-1 __profc_
// counters) lives in the fixed RX .text.ejit code segment. A non-owner core
// must make those data pages writable (enable_rw) in its own translation
// context BEFORE executing, or the first counter atomicrmw faults. These tests
// drive that path with the enable_rw mock.
//===----------------------------------------------------------------------===//

// A peer's first touch of a Tier-1 entry enable_rw's the counter page(s) AND
// enable_ex's the code page(s), on that peer core, before the pointer is
// returned. enable_rw happens before the seal (RW data prepared first).
TEST_F(SharedTaskPoolTest, FourKPeerFirstTouchEnablesRwThenSeals) {
  FourKLog fourK;
  RangeCtx range; // code at 0x40000000 size 64 (page 0)
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull; // __profc_ page 1 (disjoint from code)
  range.writableSize[0] = 32;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, codeFor(1));
  owner.releaseRead(hit.bucketIndex);

  // enable_rw ran on core 3 for the counter page, and the code page was sealed.
  ASSERT_EQ(fourK.rwPages.size(), 1u);
  EXPECT_EQ(fourK.rwPages[0].first, 0x40001000ull);
  EXPECT_EQ(fourK.rwPages[0].second, 3u);
  ASSERT_EQ(fourK.seals.size(), 1u);
  EXPECT_EQ(fourK.seals[0].first, 0x40000000ull);
  EXPECT_EQ(fourK.seals[0].second, 3u);
  // The peer's executable+writable ready bit is set (memoized).
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_NE(slot->executableCoreMask.loadRelaxed() & (uint64_t{1} << 3), 0u);
}

// A writable range spanning an unaligned start/end enable_rw's every covered
// 4K page, and none of them is a code page.
TEST_F(SharedTaskPoolTest, FourKPeerEnablesRwEveryCounterPage) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40002F00ull; // unaligned, near end of page 2
  range.writableSize[0] = 0x200;         // crosses into page 3
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(4);
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);

  ASSERT_EQ(fourK.rwPages.size(), 2u);
  EXPECT_EQ(fourK.rwPages[0].first, 0x40002000ull);
  EXPECT_EQ(fourK.rwPages[1].first, 0x40003000ull);
}

// A repeated hit on the SAME peer core (after the first-touch prepared it) does
// NO enable_rw / enable_ex callback: the per-core ready bit is memoized, so the
// cache-hit path adds no permission traffic.
TEST_F(SharedTaskPoolTest, FourKRepeatedPeerHitNoRwCallback) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull;
  range.writableSize[0] = 16;
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

  EXPECT_EQ(fourK.rwPages.size(), 1u); // not repeated on the second hit
  EXPECT_EQ(fourK.seals.size(), 1u);
}

// enable_rw failure on a peer: NO fnPtr is handed back, NO code page is sealed
// (enable_rw runs first), and the peer's ready bit stays clear so a later hit
// re-attempts rather than executing un-prepared code.
TEST_F(SharedTaskPoolTest, FourKPeerEnableRwFailureNoFnPtrNoMask) {
  FourKLog fourK;
  fourK.failRwAtIndex = 0; // fail the first enable_rw
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull;
  range.writableSize[0] = 16;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_EQ(r.fnPtr, fallback);        // clean fallback, never the shared ptr
  EXPECT_EQ(fourK.rwPages.size(), 1u); // attempted once
  EXPECT_TRUE(fourK.seals.empty());    // never reached the code seal
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->executableCoreMask.loadRelaxed() & (uint64_t{1} << 3), 0u);
}

// A non-PGO / Tier-2 entry carries NO writable ranges: the peer seals code
// pages but makes no enable_rw call (nothing to prepare).
TEST_F(SharedTaskPoolTest, FourKPeerNoWritableDataSkipsRw) {
  FourKLog fourK;
  RangeCtx range; // writableCount defaults to 0
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);

  EXPECT_TRUE(
      fourK.rwPages.empty());        // no runtime-writable data -> no enable_rw
  EXPECT_EQ(fourK.seals.size(), 1u); // code still sealed
}

// W^X guard: a writable range that shares a 4K page with the executable extent
// is refused (never enable_rw'd), so a peer never makes a code page writable.
TEST_F(SharedTaskPoolTest, FourKPeerRejectsWritableOverlappingCode) {
  FourKLog fourK;
  RangeCtx range; // code at 0x40000000 size 64
  range.writableCount = 1;
  range.writableAddr[0] = 0x40000040ull; // SAME page as the code -> illegal
  range.writableSize[0] = 16;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_EQ(r.fnPtr, fallback);
  EXPECT_TRUE(fourK.rwPages.empty()); // refused before any enable_rw
  EXPECT_TRUE(fourK.seals.empty());   // and before any seal
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->executableCoreMask.loadRelaxed() & (uint64_t{1} << 3), 0u);
}

// A writable range that lies outside the code's pool is rejected (a peer only
// splits/prepares within the pool it knows).
TEST_F(SharedTaskPoolTest, FourKPeerRejectsWritableOutOfPool) {
  FourKLog fourK;
  RangeCtx range; // pool [0x40000000, 0x40200000)
  range.writableCount = 1;
  range.writableAddr[0] = 0x50000000ull; // outside the pool
  range.writableSize[0] = 16;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_TRUE(fourK.rwPages.empty());
  EXPECT_TRUE(fourK.seals.empty());
}

// Two peer cores each independently enable_rw the counter page in their OWN
// translation context (execute permission and write permission are per-core).
TEST_F(SharedTaskPoolTest, FourKTwoPeerCoresEachEnableRw) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull;
  range.writableSize[0] = 16;
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

  ASSERT_EQ(fourK.rwPages.size(), 2u);
  EXPECT_EQ(fourK.rwPages[0].second, 3u);
  EXPECT_EQ(fourK.rwPages[1].second, 4u);
}

// An over-bound writable count on a slot (should never occur: the owner rejects
// an over-bound allocation before publish) is a clean fallback, never a
// truncated/partial enable_rw.
TEST_F(SharedTaskPoolTest, FourKPeerRejectsOverflowWritableCount) {
  FourKLog fourK;
  RangeCtx range;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  // Corrupt the slot to claim more writable ranges than the fixed bound.
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  slot->writableCount = kEJitSharedMaxWritableRanges + 1;

  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_TRUE(fourK.rwPages.empty()); // never partially prepared
  EXPECT_TRUE(fourK.seals.empty());
}

// The owner core itself already made its counter pages RW at compile time, so a
// hit on the owner core performs NO peer enable_rw / seal callback.
TEST_F(SharedTaskPoolTest, FourKOwnerHitDoesNotEnableRw) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull;
  range.writableSize[0] = 16;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(0); // owner core
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit.bucketIndex);

  EXPECT_TRUE(fourK.rwPages.empty());
  EXPECT_TRUE(fourK.seals.empty());
}

// A DYNAMIC RW pool (requiresPeerEnableRw=0) that still carries writable
// metadata: a peer seals the code pages but makes NO enable_rw call, and
// succeeds even though the writable ranges are present (they are diagnostic
// only — the pool memory is already RW). This is the dynamic-pool + 4K + shared
// pointers configuration that must NOT be forced to fall back.
TEST_F(SharedTaskPoolTest, FourKDynamicPoolWritableRangesNoEnableRw) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull;
  range.writableSize[0] = 32;
  range.requiresPeerEnableRw = 0; // dynamic pool: already RW
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  auto hit = owner.compileOrGet(1, nullptr, 0, codeFor(1));
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, codeFor(1));
  owner.releaseRead(hit.bucketIndex);

  EXPECT_TRUE(fourK.rwPages.empty()); // dynamic pool -> no enable_rw
  ASSERT_EQ(fourK.seals.size(), 1u);  // code still sealed per-core
  EXPECT_EQ(fourK.seals[0].second, 3u);
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->requiresPeerEnableRw, 0u);
  EXPECT_NE(slot->executableCoreMask.loadRelaxed() & (uint64_t{1} << 3), 0u);
}

// A fixed RX pool (requiresPeerEnableRw=1) with writable ranges but NO
// enable_rw callback wired: clean fallback (no fnPtr, no seal, no core bit).
TEST_F(SharedTaskPoolTest, FourKFixedPoolMissingEnableRwCallbackFallsBack) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40001000ull;
  range.writableSize[0] = 16;
  range.requiresPeerEnableRw = 1;
  // Bring up WITHOUT an enable_rw callback (mimic a build/config that forgot
  // it).
  EJitCoreId::setCurrentForTest(0);
  EJitSharedTaskPool owner;
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  owner.setSealMode(true);
  owner.setCodeRangeProvider(&mockCodeRange, &range);
  owner.setSplitPoolCallback(&mockSplitPool, &fourK);
  owner.setSealPageCallback(&mockSealPage, &fourK);
  // NOTE: setEnableRwPageCallback intentionally NOT called.
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  publish(owner, 1);

  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_TRUE(r.readyButNotShareable);
  EXPECT_EQ(r.fnPtr, fallback);
  EXPECT_TRUE(fourK.rwPages.empty());
  EXPECT_TRUE(fourK.seals.empty()); // enable_rw needed first -> never sealed
  EJitSharedCacheSlot *slot = findReadySlot(1);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->executableCoreMask.loadRelaxed() & (uint64_t{1} << 3), 0u);
}

// An over-bound writableCount from the code-range provider must make the owner
// REJECT the publish entirely: no slot goes Ready, no fnPtr is shared, so a
// peer (and even the owner) cleanly misses instead of running under-prepared
// code. This is the end-to-end guard for the "never degrade to writableCount=0
// and publish anyway" rule.
TEST_F(SharedTaskPoolTest, FourKOverflowWritableRejectsPublish) {
  FourKLog fourK;
  RangeCtx range;
  range.writableCount = kEJitSharedMaxWritableRanges + 1; // over-bound
  range.requiresPeerEnableRw = 1;
  EJitSharedTaskPool owner;
  bringUpOwner4K(owner, fourK, range);

  // Owner compile + attempted publish: the publish is rejected, so no Ready
  // slot.
  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.compileOrGet(1, nullptr, 0, codeFor(1)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  owner.pollOne(); // runs the compile + rejected publish

  EXPECT_EQ(findReadySlot(1), nullptr); // nothing published

  // A peer lookup finds no Ready slot -> clean miss/fallback, no fnPtr shared.
  EJitCoreId::setCurrentForTest(3);
  void *fallback = reinterpret_cast<void *>(0xFEEDull);
  auto r = owner.compileOrGet(1, nullptr, 0, fallback);
  EXPECT_NE(r.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_TRUE(fourK.rwPages.empty());
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
// table is POD, dump state contains metadata only, and each bucket carries the
// NO_RECLAIM seqlock publishSeq word.
TEST_F(SharedTaskPoolTest, FourKAbiVersionAndRangeFieldSemantics) {
  // v8 added icacheDrainSeq / icacheDrainsInFlight; v9 added the icache gates
  // that must be shared rather than per-facade (icachePerCorePrepare,
  // icacheReleasersWired), the icacheArmed drain-skip flag, and the shared
  // one-shot diagnostic mask. Bump this deliberately: the blob is mapped at one
  // address by every core, so a layout change that slips through unversioned is
  // a silent cross-core corruption.
  // v10-v13 add PGO controls, writable ranges, staged admission, and audit
  // requests; v14 introduced bound-pointer transport, and v15 adds
  // per-version post-publish reuse tracking; v16 adds near/far placement.
  // v17 adds explicit batch publish state.
  EXPECT_EQ(kEJitSharedAbiVersion, 18u);
  EXPECT_TRUE(std::is_standard_layout<EJitSharedPoolSplit>::value);
  EXPECT_TRUE(std::is_trivially_destructible<EJitSharedPoolSplit>::value);
  EXPECT_TRUE(
      std::is_trivially_default_constructible<EJitSharedPoolSplit>::value);
  EXPECT_TRUE(std::is_standard_layout<EJitSharedWritableRange>::value);
  EXPECT_TRUE(
      std::is_trivially_default_constructible<EJitSharedWritableRange>::value);

  FourKLog fourK;
  RangeCtx range;
  range.codeStart = 0x40012340ull;
  range.codeSize = 0x456;
  range.poolBase = 0x40000000ull;
  range.poolSize = 0x200000ull;
  range.poolId = 7;
  range.writableCount = 1;
  range.writableAddr[0] = 0x40080000ull;
  range.writableSize[0] = 0x40;
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
  EXPECT_EQ(slot->poolKind, static_cast<uint32_t>(EJitCodePoolKind::Near));
  // Runtime-writable ranges (v9): published verbatim from the code-range info.
  EXPECT_EQ(slot->writableCount, 1u);
  EXPECT_EQ(slot->requiresPeerEnableRw, 1u); // RangeCtx default: fixed RX pool
  EXPECT_EQ(slot->writableRanges[0].addr, 0x40080000ull);
  EXPECT_EQ(slot->writableRanges[0].size, 0x40ull);
  // PGO fields: zero on a Baseline publish (PGO off).
  EXPECT_EQ(slot->hitCount.loadRelaxed(), 0u);
}

TEST_F(SharedTaskPoolTest, DumpStateHoldsOnlySmallMetadata) {
  EXPECT_TRUE(std::is_standard_layout<EJitSharedDumpState>::value);
  EXPECT_TRUE(std::is_trivially_destructible<EJitSharedDumpState>::value);
  EXPECT_LE(sizeof(EJitSharedDumpState), 2u * kEJitSharedDumpNameBytes + 256u);
}

TEST_F(SharedTaskPoolTest, DumpMetadataClearedOnInit) {
  state_->dump.filterEnabled.storeRelaxed(1);
  state_->dump.hasDump.storeRelaxed(1);
  state_->dump.status.storeRelaxed(4);
  state_->dump.filterLen = 4;
  state_->dump.resultNameLen = 4;
  state_->dump.irSize = 123;
  state_->dump.asmSize = 456;
  state_->dump.keyHi = 0x12;
  state_->dump.keyLo = 0x34;
  state_->dump.workerCore = 7;
  state_->dump.filterName[0] = 'f';
  state_->dump.resultName[0] = 'g';
  EJitSharedTaskPool owner;
  bringUpOwner(owner);

  EXPECT_EQ(state_->dump.filterEnabled.loadRelaxed(), 0u);
  EXPECT_EQ(state_->dump.hasDump.loadRelaxed(), 0u);
  EXPECT_EQ(state_->dump.status.loadRelaxed(), 0u);
  EXPECT_EQ(state_->dump.filterLen, 0u);
  EXPECT_EQ(state_->dump.resultNameLen, 0u);
  EXPECT_EQ(state_->dump.irSize, 0u);
  EXPECT_EQ(state_->dump.asmSize, 0u);
  EXPECT_EQ(state_->dump.keyHi, 0u);
  EXPECT_EQ(state_->dump.keyLo, 0u);
  EXPECT_EQ(state_->dump.workerCore, kEJitInvalidCoreId);
  EXPECT_EQ(state_->dump.filterName[0], 0);
  EXPECT_EQ(state_->dump.resultName[0], 0);
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
      Slot.poolKind = static_cast<uint32_t>(EJitCodePoolKind::Far);
      Slot.writableCount = 0x7777;
      Slot.requiresPeerEnableRw = 0x8888;
      for (uint32_t i = 0; i < kEJitSharedMaxWritableRanges; ++i) {
        Slot.writableRanges[i].addr = 0x9999 + i;
        Slot.writableRanges[i].size = 0xAAAA + i;
      }
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
      EXPECT_EQ(Slot.poolKind,
                static_cast<uint32_t>(EJitCodePoolKind::Unknown));
      EXPECT_EQ(Slot.writableCount, 0u);
      EXPECT_EQ(Slot.requiresPeerEnableRw, 0u);
      for (uint32_t i = 0; i < kEJitSharedMaxWritableRanges; ++i) {
        EXPECT_EQ(Slot.writableRanges[i].addr, 0u);
        EXPECT_EQ(Slot.writableRanges[i].size, 0u);
      }
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
  printf("  %-28s avg=%.1fns p50=%.1f p90=%.1f p99=%.1f max=%.1f (n=%zu "
         "batches)\n",
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
  const uint32_t kIters = 2000;   // per batch
  const uint32_t kBatches = 2000; // distribution samples
  printf("HotHit micro-bench: cntfrq=%llu Hz (%.3f ns/tick), %u iters x %u "
         "batches\n",
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

  // (B) Lookup only (read-token acquired but released untimed): isolates
  // get_fn.
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
// function share one bucket cache line. The read-token RMW (fetchAdd/fetchSub
// on bucket.readers) then bounces that line between cores; a load-only seqlock
// read does not. This is the scenario the 6us regression comes from.
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
// Per-function inline cache: SHARED PARTITIONED CELL TABLE.
//
// The probe carries no freshness check, so everything that keeps a cell honest
// happens in the runtime and is pinned here: the fill, the cross-core drain on
// a period toggle, the guard that stops a resolve from putting back a cell the
// drain just cleared, and the registration/range gates. Assertions are on
// behaviour (boolean + pointer), never on stats.
//
// A test-local array stands in for the wrapper's @__ejit_icache_fn_<name>
// table. In production that array is in the inter-core shared section, so the
// ONE array these tests register models the ONE table every core sees -- which
// is exactly what makes "core A's drain clears core B's cell" testable in a
// single-process simulation.
//===----------------------------------------------------------------------===//
TEST_F(SharedTaskPoolTest, InlineCacheFillAndServe) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool); // core 0 owner, Ready, code sharing off (owner-only).
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);
  void *fn = codeFor(kFunc);
  void *out = nullptr;

  // Cold icache misses (empty cell).
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);

  // Fill on a resolve -> subsequent probe hits, returning the cached fnPtr.
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);

  // Plain store, no one-shot CAS: cores write disjoint identities, so a later
  // resolve of the SAME identity carries the same pointer and the overwrite is
  // semantically a no-op. Verify the served pointer is unchanged.
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);

  // Out-of-range funcIndex misses without touching memory.
  EXPECT_FALSE(pool.icacheTry(EJIT_ICACHE_FUNC_SLOTS, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);
}

// The funcIndex space is dense up to EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX, so the
// slot table must cover the WHOLE range: slots registered at a high (but
// in-range) funcIndex must fill and hit exactly like a low one. Regression
// guard for the FUNC_SLOTS < MAX_FUNC_INDEX desync that silently dropped
// icacheFill for 85% of the functions in the field (slots were 64, funcIndex
// space was 4096).
TEST_F(SharedTaskPoolTest, InlineCacheHighFuncIndex) {
  ejitIcacheClearAll();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);

  // Top of the table: funcIndex = EJIT_ICACHE_FUNC_SLOTS - 1.
  constexpr uint32_t kTop = EJIT_ICACHE_FUNC_SLOTS - 1;
  uintptr_t slotTop = 0;
  registerSlot(kTop, &slotTop, 0);
  void *fnTop = codeFor(kTop);
  void *out = nullptr;

  EXPECT_FALSE(pool.icacheTry(kTop, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);
  pool.icacheFill(kTop, fnTop, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kTop, nullptr, 0, &out));
  EXPECT_EQ(out, fnTop);

  // Mid-table index that the old 64-slot table would have dropped.
  constexpr uint32_t kMid = 2000;
  uintptr_t slotMid = 0;
  registerSlot(kMid, &slotMid, 0);
  void *fnMid = codeFor(kMid);
  pool.icacheFill(kMid, fnMid, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kMid, nullptr, 0, &out));
  EXPECT_EQ(out, fnMid);

  // icacheFill at funcIndex == EJIT_ICACHE_FUNC_SLOTS stays a no-op and must
  // not disturb the neighbouring top slot.
  pool.icacheFill(EJIT_ICACHE_FUNC_SLOTS, fnMid, nullptr, 0,
                  pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kTop, nullptr, 0, &out));
  EXPECT_EQ(out, fnTop);

  ejitIcacheClearAll();
}

// The core claim of the shared-table design: a period toggle EMPTIES the cell,
// so the very next probe misses and re-resolves. No epoch is published, no core
// has to notice anything -- the cell itself is gone.
TEST_F(SharedTaskPoolTest, InlineCacheDrainsOnPeriodToggle) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);
  void *fn = codeFor(kFunc);
  void *out = nullptr;

  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  ASSERT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));

  // Activate, then deactivate: either end of the mutation window drains.
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  EXPECT_EQ(slot, 0u) << "activate must empty the cell";

  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  ASSERT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, false));
  EXPECT_EQ(slot, 0u) << "deactivate must empty the cell";
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);

  // And the cache is not poisoned: a fresh resolve refills and serves again.
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);
}

// What a per-core table cannot do. Two cores fill DISJOINT partitions of the
// same shared table (the deployment premise: each core drives its own
// ejit_period_lc instance indices). A toggle issued on a THIRD core zeroes both
// partitions, so neither of the other cores keeps calling a stale
// specialization -- and neither of them had to call into the runtime to find
// out.
TEST_F(SharedTaskPoolTest, InlineCacheDrainReachesEveryCorePartition) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  constexpr uint32_t D = EJIT_ICACHE_DIM_SIZE;
  uintptr_t cells[D] = {};
  registerSlot(kFunc, &cells[0], 1);

  // Core 1 owns instance 1, core 2 owns instance 2. Disjoint cells. The
  // assertions read the cells directly, which is what the AOT probe does -- the
  // probe has no cross-core gate, unlike icacheTry (see its comment).
  EJitDimPair id1[1] = {{0, 1}};
  EJitDimPair id2[1] = {{0, 2}};
  void *fn1 = codeFor(kFunc);
  void *fn2 = codeFor(kFunc + 1);

  EJitCoreId::setCurrentForTest(1);
  pool.icacheFill(kFunc, fn1, id1, 1, pool.icacheBeginResolve());
  EJitCoreId::setCurrentForTest(2);
  pool.icacheFill(kFunc, fn2, id2, 1, pool.icacheBeginResolve());

  // Each core wrote its own partition, and only its own.
  EXPECT_EQ(cells[1], reinterpret_cast<uintptr_t>(fn1));
  EXPECT_EQ(cells[2], reinterpret_cast<uintptr_t>(fn2));
  for (uint32_t i = 0; i < D; ++i)
    if (i != 1 && i != 2)
      ASSERT_EQ(cells[i], 0u) << "cell " << i << " was written by no core";

  // A toggle on core 3 -- which never filled anything and owns neither
  // partition -- clears both. This is what a per-core table cannot do: core 1
  // and core 2 need not call into the runtime, or notice anything at all.
  EJitCoreId::setCurrentForTest(3);
  ASSERT_TRUE(pool.setInstanceEnabled(0, 7, true));
  EXPECT_EQ(cells[1], 0u);
  EXPECT_EQ(cells[2], 0u);
}

// The drain runs on EVERY setInstanceEnabled call, not only the one that moves
// the shared bit. N cores each bracket their own period-value writes over one
// shared bit, so a caller that LOST the CAS may still have rewritten its own
// copy -- and its peers' cells are just as stale. version[] moves only on the
// real transition, because runCompile DISCARDS an in-flight compile when it
// changes.
TEST_F(SharedTaskPoolTest, InlineCacheDrainsEvenWhenTheEnableBitDoesNotMove) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);

  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true)); // first: flips the bit
  const uint32_t versionAfterFirst = state_->version[0][5].loadAcquire();
  const uint32_t drainsAfterFirst = pool.icacheDrainSeq();

  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  ASSERT_NE(slot, 0u);

  // Second core activating the same instance: loses the CAS, changes no shared
  // state -- and still drains, because it may have rewritten its own period
  // values.
  EXPECT_FALSE(pool.setInstanceEnabled(0, 5, true));
  EXPECT_EQ(slot, 0u) << "a lost CAS must still drain";
  EXPECT_GT(pool.icacheDrainSeq(), drainsAfterFirst);
  EXPECT_EQ(state_->version[0][5].loadAcquire(), versionAfterFirst)
      << "version[] must not move without a real transition";
}

// A resolve that started BEFORE a drain must not put its result back
// afterwards: the pointer may be specialized for the period values the toggle
// just replaced, and nothing later would clear it again.
TEST_F(SharedTaskPoolTest, InlineCacheDropsAFillThatRacedADrain) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);

  // Enter the taskpool (token taken), then a peer toggles a period, then the
  // resolve completes and tries to fill with the now-stale token.
  const uint64_t staleTok = pool.icacheBeginResolve();
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, staleTok);
  EXPECT_EQ(slot, 0u)
      << "a fill from a resolve older than the drain is dropped";

  void *out = nullptr;
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));

  // The NEXT resolve takes a fresh token and fills normally.
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, codeFor(kFunc));

  // The drain brackets itself: it moves the sequence, and leaves no in-flight
  // count behind for the next resolve to trip over.
  const uint64_t before = pool.icacheBeginResolve();
  ASSERT_TRUE(pool.setInstanceEnabled(0, 6, true));
  const uint64_t after = pool.icacheBeginResolve();
  EXPECT_NE(after, kEJitIcacheNoResolve) << "drain left in-flight raised";
  EXPECT_NE(after, before);
}

// A fill with no usable token is dropped rather than accepted on the strength
// of a zero word. icacheBeginResolve() hands back kEJitIcacheNoResolve whenever
// it cannot vouch for the window (notably: a drain already in flight, whose
// reach over the table is unknown).
TEST_F(SharedTaskPoolTest, InlineCacheFillRequiresAValidResolveToken) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);

  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, kEJitIcacheNoResolve);
  EXPECT_EQ(slot, 0u);

  const uint64_t tok = pool.icacheBeginResolve();
  EXPECT_NE(tok, kEJitIcacheNoResolve);
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, tok);
  EXPECT_NE(slot, 0u);
}

// ejit_clear_cache() / ejit_invalidate() / a compile-mode change retire cached
// (identity -> fnPtr) mappings from outside the taskpool. The inline cache
// answers the same question with no epoch of its own, so it is emptied too.
TEST_F(SharedTaskPoolTest, InlineCacheDrainsOnRetireDispatchCache) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  ASSERT_NE(slot, 0u);

  pool.retireDispatchCache();
  EXPECT_EQ(slot, 0u);
}

// Tearing down the generation retires the code the cells point at. Draining
// here is what retires them on every core at once.
TEST_F(SharedTaskPoolTest, InlineCacheDrainsOnOwnerShutdown) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  ASSERT_NE(slot, 0u);

  pool.ownerShutdown();
  EXPECT_EQ(slot, 0u);
}

// numDims sizes the [D]^numDims array the drain walks, so an over-cap value
// would make the drain write far past the end of the wrapper's global. Reject
// the registration instead: the cell stays null and the taskpool serves every
// call.
TEST_F(SharedTaskPoolTest, InlineCacheRegistrationRejectsAnOverCapShape) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
  EXPECT_EQ(ejitIcacheRegisterSlot(3, &cells[0], EJIT_ICACHE_MAX_DIMS + 1),
            EJitIcacheRegResult::Invalid);
  EXPECT_EQ(ejitIcacheRegisterSlot(3, nullptr, 1),
            EJitIcacheRegResult::Invalid);
  // Capacity, NOT a defect: callers must degrade rather than fail init.
  EXPECT_EQ(ejitIcacheRegisterSlot(EJIT_ICACHE_FUNC_SLOTS, &cells[0], 1),
            EJitIcacheRegResult::CapacityMiss);

  // Nothing was registered, so nothing can be served and -- the point of the
  // cap -- a drain has no over-sized array to walk.
  void *out = nullptr;
  EXPECT_FALSE(pool.icacheTry(3, nullptr, EJIT_ICACHE_MAX_DIMS + 1, &out));
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));

  // A shape at the cap is accepted.
  EXPECT_EQ(ejitIcacheRegisterSlot(3, &cells[0], 1), EJitIcacheRegResult::Ok);
}

// Cross-core executability gate. A shared cell publishes the pointer to every
// core the instant it is written, so it may only be filled where a resolved
// fnPtr is callable on every core with no per-core work. Wiring per-core
// execute preparation (a prepareCode callback, or 4K-seal mode) makes that
// false, and the fill must decline rather than hand a peer an address it has
// not sealed.
// The fill does NOT depend on cross-core executability, and must not: every
// build the AOT probe is allowed in (EJIT_SRE_SHARED_CODE_POINTERS) wires
// either fourKSeal_ or prepareCodeFn_, so gating on icacheCrossCoreExecutable()
// here would leave the inline cache inert everywhere it is supposed to run.
//
// What makes a cell safe to jump to under per-core preparation is the
// deployment contract that cores drive disjoint dim identities: a core only
// reads cells it filled itself, after resolving through the taskpool, which is
// where it prepared the code. See the header.
TEST_F(SharedTaskPoolTest, InlineCacheFillsWhenCoresPrepareIndividually) {
  constexpr uint32_t kFunc = 3;
  constexpr uint32_t kInst = 2;
  void *fn = codeFor(kFunc);
  // A DIMENSIONED entry: the identity is what partitions the table, so this is
  // the shape the disjointness argument actually covers. (The 0-dim shape has
  // one shared scalar and is gated separately -- see the test below.)
  const EJitDimPair d[1] = {{0, kInst}};

  {
    EJitSharedTaskPool pool;
    bringUpOwner(pool);
    uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
    registerSlot(kFunc, cells, 1);
    ASSERT_TRUE(pool.icacheCrossCoreExecutable());
    pool.icacheFill(kFunc, fn, d, 1, pool.icacheBeginResolve());
    ASSERT_NE(cells[kInst], 0u)
        << "baseline: no per-core preparation, fill allowed";

    // Legacy 2M path: a wired prepareCode callback means each core makes the
    // pointer executable itself. The cache is still filled -- the core that
    // fills has already prepared, and disjointness keeps peers off this cell.
    PrepareLog prepare;
    pool.setPrepareCodeCallback(&mockPrepareCode, &prepare);
    EXPECT_FALSE(pool.icacheCrossCoreExecutable())
        << "a wired prepareCodeFn_ IS per-core work, so the platform state is "
           "still reported -- it is just not a fill gate for a dimensioned "
           "entry";
    cells[kInst] = 0;
    pool.icacheFill(kFunc, fn, d, 1, pool.icacheBeginResolve());
    EXPECT_NE(cells[kInst], 0u) << "prepareCode wired: fill must still happen";

    pool.setPrepareCodeCallback(nullptr, nullptr);
    cells[kInst] = 0;
    pool.icacheFill(kFunc, fn, d, 1, pool.icacheBeginResolve());
    EXPECT_NE(cells[kInst], 0u) << "callback unwired: fill unchanged";
    pool.ownerShutdown(); // release the blob so the 4K pool below can own it
  }

  // 4K-seal mode: the mode the production board runs. This is the case that was
  // silently disabling the whole feature.
  {
    ejitIcacheClearAll();
    FourKLog fourK;
    RangeCtx range;
    EJitSharedTaskPool sealPool;
    bringUpOwner4K(sealPool, fourK, range);
    uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
    registerSlot(kFunc, cells, 1);
    // 4K seal is no longer counted as per-core preparation: the seal acts on an
    // address space every core translates through, so it does not make a
    // resolved pointer core-local.
    EXPECT_TRUE(sealPool.icacheCrossCoreExecutable());
    sealPool.icacheFill(kFunc, fn, d, 1, sealPool.icacheBeginResolve());
    EXPECT_NE(cells[kInst], 0u)
        << "4K-seal mode: fill must happen, or the inline cache "
           "is dead on every real target";
  }
}

// A 0-dim entry has ONE cell and no identity to partition it by, so the
// disjointness contract cannot cover it: core B necessarily reads the cell core
// A wrote. Under per-core execute preparation that means B branches to a page
// it never sealed, on its very first call, without entering the taskpool. The
// shared scalar is therefore allowed ONLY where a resolved pointer is callable
// everywhere the instant it exists.
TEST_F(SharedTaskPoolTest,
       InlineCacheDeclinesScalarFillWhenCoresPrepareIndividually) {
  constexpr uint32_t kFunc = 6;
  void *fn = codeFor(kFunc);

  // Legacy whole-2MiB path: a wired prepareCodeFn_ is real per-core work, so a
  // pointer one core resolved is NOT callable on a peer until that peer
  // prepares. The 0-dim shape has one cell and no identity to partition it by,
  // so core B necessarily reads what core A wrote -- decline.
  {
    ejitIcacheClearAll();
    EJitSharedTaskPool coreA;
    bringUpOwner(coreA);
    PrepareLog prepare;
    coreA.setPrepareCodeCallback(&mockPrepareCode, &prepare);
    uintptr_t scalar = 0;
    registerSlot(kFunc, &scalar, 0);
    ASSERT_FALSE(coreA.icacheCrossCoreExecutable());

    coreA.icacheFill(kFunc, fn, nullptr, 0, coreA.icacheBeginResolve());
    EXPECT_EQ(scalar, 0u)
        << "0-dim cell is shared by every core: filling it under per-core "
           "preparation publishes code core B has not prepared";

    // Core B binds the same blob and the same (still empty) table. Its probe
    // must miss, so the call goes to the taskpool, which prepares for B. The
    // gate reaches B through the SHARED icachePerCorePrepare, not through B's
    // own (unset) callback.
    EJitCoreId::setCurrentForTest(1);
    EJitSharedTaskPool coreB;
    coreB.bind(coreA.state());
    EXPECT_FALSE(coreB.icacheCrossCoreExecutable());
    void *out = reinterpret_cast<void *>(0x1234);
    EXPECT_FALSE(coreB.icacheTry(kFunc, nullptr, 0, &out));
    EXPECT_EQ(out, nullptr);
    coreB.icacheFill(kFunc, fn, nullptr, 0, coreB.icacheBeginResolve());
    EXPECT_EQ(scalar, 0u) << "a peer must not arm the shared scalar either";
    EJitCoreId::setCurrentForTest(0);
    coreA.ownerShutdown();
  }

  // 4K-seal mode: NOT per-core preparation. The seal acts on an address space
  // every core translates through, so a resolved pointer is callable everywhere
  // and the shared scalar is sound. Counting the seal here left every 0-dim
  // entry on the full taskpool path on the only platform the cache ships on.
  {
    ejitIcacheClearAll();
    FourKLog fourK;
    RangeCtx range;
    EJitSharedTaskPool sealPool;
    bringUpOwner4K(sealPool, fourK, range);
    uintptr_t scalar = 0;
    registerSlot(kFunc, &scalar, 0);
    ASSERT_TRUE(sealPool.icacheCrossCoreExecutable());
    sealPool.icacheFill(kFunc, fn, nullptr, 0, sealPool.icacheBeginResolve());
    EXPECT_NE(scalar, 0u)
        << "4K seal must not block the 0-dim fill, or f_0-shaped entries never "
           "reach the inline cache on the production board";
    sealPool.ownerShutdown();
  }

  // No preparation of any kind: unchanged, the shared scalar fills.
  {
    ejitIcacheClearAll();
    EJitSharedTaskPool pool;
    bringUpOwner(pool);
    uintptr_t scalar = 0;
    registerSlot(kFunc, &scalar, 0);
    ASSERT_TRUE(pool.icacheCrossCoreExecutable());
    pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
    EXPECT_NE(scalar, 0u)
        << "no per-core preparation: the shared scalar is safe and must fill";
  }
}

// Safety gate: wiring a releaser means code may be freed, and the cache does no
// HP-scan retire, so it auto-disables (icacheTry always misses, icacheFill
// no-op) to avoid UAF. Unwiring the releaser re-enables it.
TEST_F(SharedTaskPoolTest, InlineCacheAutoDisablesWhenReclamationWired) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0);
  void *fn = codeFor(kFunc);
  void *out = nullptr;

  // Before a releaser: fill -> hit.
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);

  // Wire a releaser: icache auto-disables (miss + no-op fill).
  ReleaseLog rel;
  pool.setReleaser(&mockRelease, &rel);
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);
  pool.icacheFill(kFunc, fn, nullptr, 0,
                  pool.icacheBeginResolve()); // no-op: the gate blocks the fill
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);

  // Unwire the releaser: the gate re-opens; fill -> hit works again. Wiring the
  // releaser drained the table (retireDispatchCache), so this models a FRESH
  // resolve -- in production every fill is preceded by an entry-point arm.
  pool.setReleaser(nullptr, nullptr);
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);
}

// A drain walks every cell of every registered slot. At bring-up nothing is
// cached yet, so that walk writes 0 over 0 -- and there is one per
// ejit_activate per core, which on a 22-core image with five slots at dims 0..4
// is ~12M pointless stores to shared memory. icacheArmed lets a drain skip the
// walk entirely until something is actually cached.
//
// What must NOT happen is a skip that strands a live cell, so this pins both
// halves: the skip when cold, and the walk once armed.
TEST_F(SharedTaskPoolTest, DrainSkipsTheWalkUntilSomethingIsCached) {
  ejitIcacheClearAll();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  auto *st = pool.state();
  constexpr uint32_t kFunc = 3;
  constexpr uint32_t kInst = 2;
  uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
  registerSlot(kFunc, cells, 1);
  const EJitDimPair d[1] = {{0, kInst}};

  // Cold: nothing armed, so a toggle must not walk. Poison a cell behind the
  // runtime's back -- a drain that walked would clear it.
  ASSERT_EQ(st->icacheArmed.loadAcquire(), 0u);
  cells[7] = 0xDEADBEEF;
  ASSERT_TRUE(pool.setInstanceEnabled(0, 1, true));
  EXPECT_EQ(cells[7], 0xDEADBEEFu)
      << "a cold drain must skip the walk, not zero the table";
  // The sequence still moves: a resolve in flight must still see a drain.
  EXPECT_EQ(st->icacheArmed.loadAcquire(), 0u);
  cells[7] = 0;

  // A fill arms the table.
  const uint32_t seqBefore = pool.icacheDrainSeq();
  pool.icacheFill(kFunc, codeFor(kFunc), d, 1, pool.icacheBeginResolve());
  ASSERT_NE(cells[kInst], 0u);
  EXPECT_EQ(st->icacheArmed.loadAcquire(), 1u)
      << "icacheFill must arm before it publishes";
  EXPECT_GT(pool.icacheDrainSeq(), seqBefore - 1u);

  // Armed: the next drain walks and empties the cell, then disarms.
  ASSERT_TRUE(pool.setInstanceEnabled(0, 2, true));
  EXPECT_EQ(cells[kInst], 0u) << "an armed drain must still clear the table";
  EXPECT_EQ(st->icacheArmed.loadAcquire(), 0u)
      << "a completed walk must disarm, or every later drain walks for nothing";

  // And it re-arms on the next fill, so the optimization is not one-shot.
  pool.icacheFill(kFunc, codeFor(kFunc), d, 1, pool.icacheBeginResolve());
  EXPECT_EQ(st->icacheArmed.loadAcquire(), 1u);
  ASSERT_TRUE(pool.setInstanceEnabled(0, 3, true));
  EXPECT_EQ(cells[kInst], 0u);

  ejitIcacheClearAll();
}

// Wiring a releaser is ONE invalidation event and must cost one drain.
// retireDispatchCache() already empties the cell table, so an extra explicit
// icacheDrainAll() next to it walked every slot twice and moved icacheDrainSeq
// by two -- which also invalidates twice as many in-flight resolve tokens as
// the event actually justifies.
TEST_F(SharedTaskPoolTest, WiringAReleaserDrainsExactlyOnce) {
  ejitIcacheClearAll();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
  registerSlot(kFunc, cells, 1);
  const EJitDimPair d[1] = {{0, 1}};
  pool.icacheFill(kFunc, codeFor(kFunc), d, 1, pool.icacheBeginResolve());
  ASSERT_NE(cells[1], 0u);

  const uint32_t before = pool.icacheDrainSeq();
  ReleaseLog rel;
  pool.setReleaser(&mockRelease, &rel);
  EXPECT_EQ(cells[1], 0u) << "the table must still be drained";
  EXPECT_EQ(pool.icacheDrainSeq(), before + 1u)
      << "one event, one drain: retireDispatchCache already drains";

  // Unwiring is not an invalidation event and must not drain at all.
  const uint32_t afterWire = pool.icacheDrainSeq();
  pool.setReleaser(nullptr, nullptr);
  EXPECT_EQ(pool.icacheDrainSeq(), afterWire);

  ejitIcacheClearAll();
}

// bind() and setReleaser() both reconcile this facade's contribution to the
// shared releaser count, so a facade that is bound more than once -- or wired
// and then re-bound -- must still count exactly one. Double counting leaves the
// count stuck above zero and the cache disabled with no way back.
TEST_F(SharedTaskPoolTest, ReleaserCountIsIdempotentAcrossRebind) {
  ejitIcacheClearAll();
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  constexpr uint32_t kFunc = 3;
  uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
  registerSlot(kFunc, cells, 1);
  const EJitDimPair d[1] = {{0, 1}};

  ReleaseLog rel;
  EJitSharedTaskPool peer;
  peer.setReleaser(&mockRelease, &rel); // wired BEFORE binding
  peer.bind(owner.state());
  peer.bind(owner.state()); // re-bind: must not count twice
  peer.bind(owner.state());

  owner.icacheFill(kFunc, codeFor(kFunc), d, 1, owner.icacheBeginResolve());
  EXPECT_EQ(cells[1], 0u) << "peer's releaser must disable the shared table";

  // One unwire must therefore be enough to re-open it.
  peer.setReleaser(nullptr, nullptr);
  owner.icacheFill(kFunc, codeFor(kFunc), d, 1, owner.icacheBeginResolve());
  EXPECT_NE(cells[1], 0u)
      << "count was incremented more than once for a single facade";

  ejitIcacheClearAll();
}

// The reclamation gate must live in the BLOB, not in the facade that wired the
// releaser. One table backs every core and the AOT probe consults no gate at
// all, so if the disable state were core-private a peer facade -- whose own
// flag is still clear -- could refill the cells the owner just drained, and the
// owner would then reclaim code another core is still calling.
//
// Single-pool tests cannot see this: it takes two facades over one blob.
TEST_F(SharedTaskPoolTest, InlineCacheReclamationGateIsSharedAcrossFacades) {
  ejitIcacheClearAll();
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  constexpr uint32_t kFunc = 5;
  constexpr uint32_t kInst = 1;
  // A dimensioned entry, so the 0-dim platform gate is not what is under test.
  uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
  registerSlot(kFunc, cells, 1);
  const EJitDimPair d[1] = {{0, kInst}};
  void *fn = codeFor(kFunc);

  // A second facade over the SAME blob, as a peer core would have.
  EJitSharedTaskPool peer;
  peer.bind(owner.state());

  // Baseline: no releaser anywhere, so the peer may fill.
  peer.icacheFill(kFunc, fn, d, 1, peer.icacheBeginResolve());
  ASSERT_NE(cells[kInst], 0u) << "baseline: no releaser, peer fill allowed";

  // The OWNER wires a releaser. Two things must follow, and neither of them is
  // visible to the peer's own icacheReclamationSafe_, which is still true.
  ReleaseLog rel;
  owner.setReleaser(&mockRelease, &rel);
  EXPECT_EQ(cells[kInst], 0u)
      << "wiring a releaser must drain the shared table, not just block fills";

  peer.icacheFill(kFunc, fn, d, 1, peer.icacheBeginResolve());
  EXPECT_EQ(cells[kInst], 0u)
      << "a peer facade must not re-arm a table the owner disabled";

  // Unwiring the last releaser re-opens the gate for every facade.
  owner.setReleaser(nullptr, nullptr);
  peer.icacheFill(kFunc, fn, d, 1, peer.icacheBeginResolve());
  EXPECT_NE(cells[kInst], 0u)
      << "gate must re-open once the last releaser is gone";

  // And the count is what re-opens it, not the last writer: with the PEER
  // holding a releaser, the owner unwiring its own must leave the cache shut.
  peer.setReleaser(&mockRelease, &rel);
  owner.setReleaser(&mockRelease, &rel);
  owner.setReleaser(nullptr, nullptr);
  cells[kInst] = 0;
  owner.icacheFill(kFunc, fn, d, 1, owner.icacheBeginResolve());
  EXPECT_EQ(cells[kInst], 0u)
      << "one facade unwiring must not re-arm while another still holds one";
  peer.setReleaser(nullptr, nullptr);
  owner.icacheFill(kFunc, fn, d, 1, owner.icacheBeginResolve());
  EXPECT_NE(cells[kInst], 0u) << "last releaser gone: gate re-opens";

  ejitIcacheClearAll();
}

// setInstanceEnabled() does not require Ready, so a peer drain can still be
// walking cells when the owner shuts down and a new owner claims the blob. The
// exact interleaving that used to corrupt the protocol:
//
//   peer:      icacheDrainsInFlight.fetchAdd(1), starts walking
//   old owner: publishes Uninitialized
//   new owner: claims the blob and clears the counter
//   peer:      finishes and fetchSub(1)  ->  0 - 1  ==  UINT32_MAX
//
// After that icacheBeginResolve() refuses every token forever -- it declines
// while any drain is in flight -- and no later drain can repair the count,
// since each one is +1 then -1. The inline cache is then permanently dead with
// no diagnostic.
//
// Two things prevent it. The new owner clears the counter only AFTER publishing
// the new generation, and a drain retires its increment only if the generation
// is still the one it announced under.
TEST_F(SharedTaskPoolTest, ReinitCannotUnderflowAStragglerDrainCounter) {
  ejitIcacheClearAll();
  EJitSharedTaskPool owner;
  bringUpOwner(owner);
  auto *st = owner.state();
  const uint32_t staleGen = st->generation.loadAcquire();

  // A peer drain announces itself and begins walking. This is exactly the first
  // half of icacheDrainAll().
  st->icacheDrainsInFlight.fetchAdd(1);
  ASSERT_EQ(st->icacheDrainsInFlight.loadAcquire(), 1u);
  // While it is in flight nothing may resolve -- the drain's reach is unknown.
  EXPECT_EQ(owner.icacheBeginResolve(), kEJitIcacheNoResolve);

  // The owner hands the blob over and a new owner claims it, mid-walk.
  owner.ownerShutdown();
  EJitSharedTaskPool nextOwner;
  bringUpOwner(nextOwner);
  ASSERT_EQ(nextOwner.state(), st) << "same blob, new generation";
  const uint32_t freshGen = st->generation.loadAcquire();
  ASSERT_NE(freshGen, staleGen);
  EXPECT_EQ(st->icacheDrainsInFlight.loadAcquire(), 0u)
      << "re-init must discard the straggler's increment";

  // Now the straggler finishes, through the real retire path, still stamped
  // with the generation it started under.
  ejitIcacheRetireDrain(st, staleGen);
  EXPECT_EQ(st->icacheDrainsInFlight.loadAcquire(), 0u)
      << "a straggler from a dead generation must not decrement";

  // The observable consequence: resolves still get tokens and fills still land.
  const uint64_t token = nextOwner.icacheBeginResolve();
  ASSERT_NE(token, kEJitIcacheNoResolve)
      << "the counter underflowed: every resolve is refused from here on";
  constexpr uint32_t kFunc = 4;
  constexpr uint32_t kInst = 3;
  uintptr_t cells[EJIT_ICACHE_DIM_SIZE] = {};
  registerSlot(kFunc, cells, 1);
  const EJitDimPair d[1] = {{0, kInst}};
  nextOwner.icacheFill(kFunc, codeFor(kFunc), d, 1,
                       nextOwner.icacheBeginResolve());
  EXPECT_NE(cells[kInst], 0u) << "the cache must still work after the re-init";

  // A retire stamped with the CURRENT generation still decrements, so the
  // generation check has not simply disabled the accounting.
  st->icacheDrainsInFlight.fetchAdd(1);
  ejitIcacheRetireDrain(st, freshGen);
  EXPECT_EQ(st->icacheDrainsInFlight.loadAcquire(), 0u);

  // And it saturates: an unmatched retire cannot drive the count negative.
  ejitIcacheRetireDrain(st, freshGen);
  EXPECT_EQ(st->icacheDrainsInFlight.loadAcquire(), 0u)
      << "retire must saturate at zero, never wrap";

  ejitIcacheClearAll();
}

// Multi-version: a 2-dim icache holds one fnPtr per dim identity, so different
// (i,j) combos serve different specializations. Distinct identities fill
// distinct cells; a shape mismatch misses; and a drain clears every identity,
// not just the one the draining core happened to use.
TEST_F(SharedTaskPoolTest, InlineCacheMultiVersion) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  // A 2-dim [D][D] table (D = EJIT_ICACHE_DIM_SIZE), zero-initialized. The
  // runtime reaches cells through a base pointer + linearized index.
  constexpr uint32_t D = EJIT_ICACHE_DIM_SIZE;
  uintptr_t slots[D][D] = {};
  registerSlot(kFunc, &slots[0][0], 2);

  void *fn00 = codeFor(kFunc);
  void *fn01 = codeFor(kFunc + 1);
  void *fn10 = codeFor(kFunc + 2);
  EJitDimPair id00[2] = {{0, 0}, {0, 0}};
  EJitDimPair id01[2] = {{0, 0}, {0, 1}};
  EJitDimPair id10[2] = {{0, 1}, {0, 0}};
  void *out = nullptr;

  // Cold: every identity misses (cells start empty).
  EXPECT_FALSE(pool.icacheTry(kFunc, id00, 2, &out));
  EXPECT_FALSE(pool.icacheTry(kFunc, id01, 2, &out));

  // Fill each identity with its own fnPtr.
  pool.icacheFill(kFunc, fn00, id00, 2, pool.icacheBeginResolve());
  pool.icacheFill(kFunc, fn01, id01, 2, pool.icacheBeginResolve());
  pool.icacheFill(kFunc, fn10, id10, 2, pool.icacheBeginResolve());

  // Each identity serves its OWN fnPtr (multi-version: no cross-contamination).
  EXPECT_TRUE(pool.icacheTry(kFunc, id00, 2, &out));
  EXPECT_EQ(out, fn00);
  EXPECT_TRUE(pool.icacheTry(kFunc, id01, 2, &out));
  EXPECT_EQ(out, fn01);
  EXPECT_TRUE(pool.icacheTry(kFunc, id10, 2, &out));
  EXPECT_EQ(out, fn10);

  // Re-fill of (0,0) with the same pointer leaves the served pointer alone.
  pool.icacheFill(kFunc, fn00, id00, 2, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, id00, 2, &out));
  EXPECT_EQ(out, fn00);

  // Shape mismatch (caller numDims != registered 2): miss, no OOB access.
  EXPECT_FALSE(pool.icacheTry(kFunc, id00, 1, &out));
  EXPECT_FALSE(pool.icacheTry(kFunc, id00, 0, &out));

  // A drain walks the whole [D][D] table: every identity is cleared, wherever
  // in the array it sits.
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  EXPECT_FALSE(pool.icacheTry(kFunc, id00, 2, &out));
  EXPECT_FALSE(pool.icacheTry(kFunc, id01, 2, &out));
  EXPECT_FALSE(pool.icacheTry(kFunc, id10, 2, &out));
  for (uint32_t i = 0; i < D; ++i)
    for (uint32_t j = 0; j < D; ++j)
      ASSERT_EQ(slots[i][j], 0u) << "cell [" << i << "][" << j << "] survived";
}

//===----------------------------------------------------------------------===//
// Sentinel-form slots: the branchless wrapper.
//
// For NumDims <= 2 (no -ejit-wrapper-timing) the AOT DEFINES the cell table
// pre-filled with &MissFn and the wrapper is load + musttail BLR with no null
// guard, so a cell holding 0 is a crash, not a miss. The runtime's side of
// that contract: whatever path empties a cell writes the slot's registered
// missFn back, and icacheTry reports the sentinel as a miss. Guarded slots
// (3D/4D, timing - registered with a null missFn) keep the historical 0.
//===----------------------------------------------------------------------===//

namespace {
// A stand-in for the AOT-generated <name>_miss: the runtime only stores and
// compares the address, so any stable non-null function address models it.
// C++ has no implicit function-pointer -> void* conversion (and the cast is
// not a constant expression), so both forms come from these helpers.
void testMissFn() {}
const void *testSentinelPtr() {
  return reinterpret_cast<const void *>(&testMissFn);
}
uintptr_t testSentinel() { return reinterpret_cast<uintptr_t>(&testMissFn); }
} // namespace

// The full sentinel lifecycle: definition-time sentinel -> miss; fill -> hit;
// period toggle -> SENTINEL back (never 0); refill -> hit again.
TEST_F(SharedTaskPoolTest, InlineCacheSentinelDrainsToMissFn) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  // The cell as the image loader places it: DEFINED as &MissFn. 0D here; the
  // multi-dim walk is pinned below.
  uintptr_t slot = testSentinel();
  registerSlot(kFunc, &slot, 0, testSentinelPtr());
  void *fn = codeFor(kFunc);
  void *out = nullptr;

  // The sentinel IS a miss: icacheTry reports it with *outFn cleared.
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, nullptr);

  // A resolve fills over the sentinel and serves.
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);

  // A period toggle empties the cell back to the SENTINEL. This is the
  // load-bearing assertion of the branchless wrapper: the probe BLRs the cell
  // unconditionally, so 0 here would be a jump to null.
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  EXPECT_EQ(slot, testSentinel())
      << "drain must empty a sentinel slot to &MissFn";
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));

  // Not poisoned: a fresh resolve refills and serves again.
  pool.icacheFill(kFunc, fn, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, fn);
}

// A stale-token fill DECLINES before storing, so it must leave the sentinel
// the drain just wrote in place -- it must not zero the cell either.
TEST_F(SharedTaskPoolTest, InlineCacheSentinelDeclinesLeaveTheDrainValue) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = testSentinel();
  registerSlot(kFunc, &slot, 0, testSentinelPtr());

  // Arm the table with a live fill, so the toggle's drain below actually
  // walks (an unarmed drain skips the walk -- there is nothing to empty).
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  ASSERT_NE(slot, testSentinel());

  const uint64_t staleTok = pool.icacheBeginResolve();
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true)); // drain -> sentinel
  EXPECT_EQ(slot, testSentinel());
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, staleTok); // declines
  EXPECT_EQ(slot, testSentinel())
      << "a declined fill must leave the drain's sentinel untouched";
  void *out = nullptr;
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
}

// The retract: a drain that begins after the fill's pre-store checks pass but
// before its re-validation. Reached deterministically through the test-only
// midpoint hook (the production interleave is a preempting peer core). The
// retracted cell must hold the SENTINEL, not 0 -- same contract as the drain.
TEST_F(SharedTaskPoolTest, InlineCacheSentinelFillRetractWritesMissFn) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = testSentinel();
  registerSlot(kFunc, &slot, 0, testSentinelPtr());

  // Fire a period toggle from inside the fill, between the store and the
  // re-validation -- exactly where a preempting peer's drain lands.
  pool.setIcacheFillMidpointForTest(
      [](void *ctx) {
        static_cast<EJitSharedTaskPool *>(ctx)->setInstanceEnabled(0, 5, true);
      },
      &pool);
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  pool.setIcacheFillMidpointForTest(nullptr, nullptr);

  EXPECT_EQ(slot, testSentinel())
      << "a retracted fill must reset the cell to &MissFn, not 0";
  void *out = nullptr;
  EXPECT_FALSE(pool.icacheTry(kFunc, nullptr, 0, &out));
}

// The same retract with a GUARDED slot (null missFn): the empty value stays 0.
// One seam, both forms - the retract picks its value from the registration.
TEST_F(SharedTaskPoolTest, InlineCacheGuardedFillRetractStillWritesZero) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0); // guarded form: no missFn

  pool.setIcacheFillMidpointForTest(
      [](void *ctx) {
        static_cast<EJitSharedTaskPool *>(ctx)->setInstanceEnabled(0, 5, true);
      },
      &pool);
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  pool.setIcacheFillMidpointForTest(nullptr, nullptr);

  EXPECT_EQ(slot, 0u) << "a guarded slot's retract keeps the historical 0";
}

// A multi-dim (2D) sentinel table: the drain walks every cell writing the
// sentinel, wherever the filled identities sit.
TEST_F(SharedTaskPoolTest, InlineCacheSentinelDrainsEveryCellOfAMultiDimTable) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  constexpr uint32_t D = EJIT_ICACHE_DIM_SIZE;
  // The [D][D] table as defined: every cell &MissFn.
  uintptr_t slots[D][D];
  for (uint32_t i = 0; i < D; ++i)
    for (uint32_t j = 0; j < D; ++j)
      slots[i][j] = testSentinel();
  registerSlot(kFunc, &slots[0][0], 2, testSentinelPtr());

  EJitDimPair id01[2] = {{0, 0}, {0, 1}};
  EJitDimPair id10[2] = {{0, 1}, {0, 0}};
  void *out = nullptr;

  EXPECT_FALSE(pool.icacheTry(kFunc, id01, 2, &out));
  pool.icacheFill(kFunc, codeFor(kFunc), id01, 2, pool.icacheBeginResolve());
  pool.icacheFill(kFunc, codeFor(kFunc + 1), id10, 2,
                  pool.icacheBeginResolve());
  EXPECT_EQ(slots[0][1], reinterpret_cast<uintptr_t>(codeFor(kFunc)));
  EXPECT_EQ(slots[1][0], reinterpret_cast<uintptr_t>(codeFor(kFunc + 1)));

  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  for (uint32_t i = 0; i < D; ++i)
    for (uint32_t j = 0; j < D; ++j)
      ASSERT_EQ(slots[i][j], testSentinel())
          << "cell [" << i << "][" << j << "] must drain to &MissFn";
  EXPECT_FALSE(pool.icacheTry(kFunc, id01, 2, &out));
  EXPECT_FALSE(pool.icacheTry(kFunc, id10, 2, &out));
}

// One drain, two forms: a sentinel slot and a guarded slot registered side by
// side empty to their own values. The registration is the ONLY thing that
// decides the empty value - a guarded slot next to a sentinel one must not
// inherit the sentinel.
TEST_F(SharedTaskPoolTest, InlineCacheGuardedSlotStillDrainsToZero) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kSentinelFunc = 3;
  constexpr uint32_t kGuardedFunc = 4;
  uintptr_t sentinelSlot = testSentinel();
  uintptr_t guardedSlot = 0;
  registerSlot(kSentinelFunc, &sentinelSlot, 0, testSentinelPtr());
  registerSlot(kGuardedFunc, &guardedSlot, 0); // guarded: null missFn

  pool.icacheFill(kSentinelFunc, codeFor(kSentinelFunc), nullptr, 0,
                  pool.icacheBeginResolve());
  pool.icacheFill(kGuardedFunc, codeFor(kGuardedFunc), nullptr, 0,
                  pool.icacheBeginResolve());
  ASSERT_EQ(sentinelSlot, reinterpret_cast<uintptr_t>(codeFor(kSentinelFunc)));
  ASSERT_EQ(guardedSlot, reinterpret_cast<uintptr_t>(codeFor(kGuardedFunc)));

  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  EXPECT_EQ(sentinelSlot, testSentinel());
  EXPECT_EQ(guardedSlot, 0u);
  void *out = nullptr;
  EXPECT_FALSE(pool.icacheTry(kSentinelFunc, nullptr, 0, &out));
  EXPECT_FALSE(pool.icacheTry(kGuardedFunc, nullptr, 0, &out));
}

// ejitIcacheClearAll() retires the registration wholesale: re-registering the
// same funcIndex as a guarded slot must not inherit the stale missFn (a drain
// would then write &MissFn into a wrapper that null-checks 0 - harmless to
// execution, but the sentinel-vs-0 contract would be lying).
TEST_F(SharedTaskPoolTest, InlineCacheClearAllDropsTheSentinelRegistration) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  constexpr uint32_t kFunc = 3;
  uintptr_t slot = 0;
  registerSlot(kFunc, &slot, 0, testSentinelPtr());
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  ASSERT_NE(slot, 0u);

  ejitIcacheClearAll();
  slot = 0;
  registerSlot(kFunc, &slot, 0); // re-register guarded
  pool.icacheFill(kFunc, codeFor(kFunc), nullptr, 0, pool.icacheBeginResolve());
  ASSERT_NE(slot, 0u);
  ASSERT_TRUE(pool.setInstanceEnabled(0, 5, true));
  EXPECT_EQ(slot, 0u) << "clearAll must drop the missFn, not leak it";
}

//===----------------------------------------------------------------------===//
// Per-core L0 dispatch cache
//
// Nothing on the hit path re-checks what makes it safe, so each invariant is
// pinned here. Getting one wrong yields a stale or dangling code pointer, which
// no value assertion elsewhere would catch.
//===----------------------------------------------------------------------===//

namespace {
/// Process-globals standing in for core-private storage: no test may inherit
/// them from the one before it.
void resetL0ForTest() {
  for (uint32_t i = 0; i < kEJitL0Slots; ++i)
    gEJitL0[i] = EJitL0Entry{};
  gEJitL0State = nullptr;
}
} // namespace

TEST_F(SharedTaskPoolTest, L0ServesARepeatDispatch) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  const uint32_t kFunc = 3;
  const EJitDimPair dims[1] = {{0, 1}};

  void *out = nullptr;
  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out)) << "cold table must miss";

  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  EXPECT_TRUE(pool.l0Try(kFunc, dims, 1, &out));
  EXPECT_EQ(out, codeFor(kFunc));

  // A different identity must miss, not return the wrong pointer.
  const EJitDimPair other[1] = {{0, 2}};
  EXPECT_FALSE(pool.l0Try(kFunc, other, 1, &out));
}

TEST_F(SharedTaskPoolTest, L0RetiredByPublishAndByVersionChange) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  const uint32_t kFunc = 4;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(pool.l0Try(kFunc, dims, 1, &out));

  publish(pool, 9);
  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out))
      << "publish must retire the L0";

  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(pool.l0Try(kFunc, dims, 1, &out));

  // L0 entries carry no version of their own.
  pool.setInstanceEnabled(0, 1, true);
  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out))
      << "version change must retire the L0";
}

TEST_F(SharedTaskPoolTest, L0RetiredByExplicitFlush) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  const uint32_t kFunc = 5;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(pool.l0Try(kFunc, dims, 1, &out));

  // ejit_clear_cache() / ejit_invalidate() / a compile-mode change reach the
  // L0 only through this hook.
  pool.retireDispatchCache();
  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out));
}

TEST_F(SharedTaskPoolTest, L0EntryIsNotServedToAnotherCore) {
  resetL0ForTest();
  EJitSharedTaskPool owner;
  bringUpOwner(owner, /*codeSharing=*/true);
  const uint32_t kFunc = 6;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  EJitCoreId::setCurrentForTest(0);
  owner.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(owner.l0Try(kFunc, dims, 1, &out));

  // Cannot arise on hardware (core-private table), but can where cores are
  // simulated: a peer taking the owner's entry would skip peerPrepareSlot().
  EJitCoreId::setCurrentForTest(1);
  EXPECT_FALSE(owner.l0Try(kFunc, dims, 1, &out))
      << "a peer must not be served the owner's entry";

  EJitCoreId::setCurrentForTest(0);
  EXPECT_TRUE(owner.l0Try(kFunc, dims, 1, &out)) << "owner still hits";
}

TEST_F(SharedTaskPoolTest, L0DoesNotSurviveANewSharedBlob) {
  resetL0ForTest();
  const uint32_t kFunc = 7;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(pool.l0Try(kFunc, dims, 1, &out));
  ASSERT_NE(gEJitL0State, nullptr);

  // The table outlives the shared blob. A fresh blob starts from a low
  // dispatchEpoch, so the epoch alone cannot separate the two instances -- the
  // entry must be rejected because it was armed against another state.
  auto other = std::make_unique<EJitSharedTaskPoolState>();
  EJitSharedTaskPool fresh;
  EJitCoreId::setCurrentForTest(0);
  fresh.bind(other.get());
  fresh.setCompiler(&mockCompile, nullptr);
  fresh.setMode(EJitCompileMode::Async);
  ASSERT_EQ(fresh.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_FALSE(fresh.l0Try(kFunc, dims, 1, &out))
      << "an entry armed against a previous blob must not validate";
}

TEST_F(SharedTaskPoolTest, L0DoesNotSurviveAReInitialization) {
  resetL0ForTest();
  const uint32_t kFunc = 11;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(pool.l0Try(kFunc, dims, 1, &out));

  // Same blob, torn down and stood back up: entries still point into a code
  // pool that has been reset.
  EJitCoreId::setCurrentForTest(0);
  pool.ownerShutdown();
  ASSERT_EQ(pool.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out))
      << "an entry from the previous pool instance must not validate";

  // ...and the cache re-arms normally afterwards.
  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  EXPECT_TRUE(pool.l0Try(kFunc, dims, 1, &out));
}

TEST_F(SharedTaskPoolTest, L0RefusesToArmWhileAReleaserIsWired) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  const uint32_t kFunc = 8;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  // With a releaser wired, code can be freed under a caller, and an L0 hit
  // carries no read token. Same gate the inline cache uses.
  pool.setReleaser([](void *, void *) {}, nullptr);
  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out));
}

// The gate is evaluated at fill, so wiring a releaser AFTER entries are armed
// must retire them.
TEST_F(SharedTaskPoolTest, L0EntriesAreRetiredWhenAReleaserIsWiredLater) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  const uint32_t kFunc = 12;
  const EJitDimPair dims[1] = {{0, 1}};
  void *out = nullptr;

  pool.l0Fill(kFunc, codeFor(kFunc), dims, 1);
  ASSERT_TRUE(pool.l0Try(kFunc, dims, 1, &out));

  pool.setReleaser([](void *, void *) {}, nullptr);
  EXPECT_FALSE(pool.l0Try(kFunc, dims, 1, &out))
      << "an armed entry must not survive a releaser being wired";
}

TEST_F(SharedTaskPoolTest, ReleaseReadIgnoresTheNoBucketSentinel) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  // The wrapper calls releaseRead() unconditionally, so kEJitNoBucket must be
  // a no-op rather than a spurious decrement of a real bucket.
  pool.releaseRead(kEJitNoBucket);
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets; ++b)
    EXPECT_EQ(state_->buckets[b].readers.loadRelaxed(), 0u)
        << "bucket " << b << " reader count disturbed";
}

// A fill interrupted between its payload stores must not leave an old identity
// paired with the new fnPtr. Core-private storage excludes other CORES, not
// preemption on this one, so the seqlock is what makes the entry atomic.
TEST_F(SharedTaskPoolTest, L0PartiallyWrittenEntryIsNotServed) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  const EJitDimPair a[1] = {{0, 1}};
  void *out = nullptr;

  pool.l0Fill(20, codeFor(20), a, 1);
  ASSERT_TRUE(pool.l0Try(20, a, 1, &out));
  ASSERT_EQ(out, codeFor(20));

  // The interleaving a preempted fill leaves: odd sequence, fnPtr replaced,
  // identity not yet updated.
  EJitL0Entry &e = gEJitL0[ejitL0Index(20, a, 1)];
  e.seq = e.seq + 1u;
  e.fn = codeFor(21);

  EXPECT_FALSE(pool.l0Try(20, a, 1, &out))
      << "a half-written entry must not be served";

  e.seq = e.seq + 1u; // writer completes
  EXPECT_TRUE(pool.l0Try(20, a, 1, &out));
}

// Two identities landing in the same slot must evict each other, never
// cross-serve. The colliding pair is SEARCHED FOR rather than hard-coded, so
// the test keeps its meaning if the hash changes.
TEST_F(SharedTaskPoolTest, L0CollidingIdentitiesNeverCrossServe) {
  resetL0ForTest();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  void *out = nullptr;

  const EJitDimPair a[1] = {{0, 1}};
  const uint32_t slot = ejitL0Index(30, a, 1);
  EJitDimPair b[1] = {{0, 0}};
  bool found = false;
  for (uint32_t inst = 2; inst < kEJitSharedInstances && !found; ++inst) {
    b[0].instanceId = inst;
    found = ejitL0Index(30, b, 1) == slot;
  }
  ASSERT_TRUE(found) << "no colliding identity found in the instance space";

  pool.l0Fill(30, codeFor(30), a, 1);
  pool.l0Fill(30, codeFor(31), b, 1); // evicts a from the shared slot

  EXPECT_FALSE(pool.l0Try(30, a, 1, &out))
      << "the evicted identity must miss, not receive the evictor's pointer";
  ASSERT_TRUE(pool.l0Try(30, b, 1, &out));
  EXPECT_EQ(out, codeFor(31));

  // The pair the old packed key aliased via dimType*31 + instanceId.
  resetL0ForTest();
  const EJitDimPair c[1] = {{0, 31}};
  const EJitDimPair d[1] = {{1, 0}};
  pool.l0Fill(30, codeFor(32), c, 1);
  pool.l0Fill(30, codeFor(33), d, 1);
  if (pool.l0Try(30, c, 1, &out))
    EXPECT_EQ(out, codeFor(32)) << "(0,31) served (1,0)'s specialization";
  if (pool.l0Try(30, d, 1, &out))
    EXPECT_EQ(out, codeFor(33)) << "(1,0) served (0,31)'s specialization";

  // Same funcIndex, differing arity must not alias.
  resetL0ForTest();
  const EJitDimPair two[2] = {{0, 31}, {0, 0}};
  pool.l0Fill(30, codeFor(34), c, 1);
  pool.l0Fill(30, codeFor(35), two, 2);
  if (pool.l0Try(30, c, 1, &out))
    EXPECT_EQ(out, codeFor(34)) << "1D identity served a 2D entry";
  if (pool.l0Try(30, two, 2, &out))
    EXPECT_EQ(out, codeFor(35));

  // Different funcIndex, identical dims must not alias.
  resetL0ForTest();
  pool.l0Fill(30, codeFor(36), c, 1);
  pool.l0Fill(31, codeFor(37), c, 1);
  if (pool.l0Try(30, c, 1, &out))
    EXPECT_NE(out, codeFor(37)) << "funcIndex 30 served funcIndex 31's entry";
}

// PGO (§6): shared taskpool hitCount + Tier-2 auto-trigger.
//===----------------------------------------------------------------------===//

TEST_F(SharedTaskPoolTest, SharedPgoFastHitEnqueuesTier2) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setPgoEnabled(true, 1);

  ASSERT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  ASSERT_EQ(pool.pendingCount(), 0u);

  auto hit = pool.tryCacheHit0D(5);
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(pool.pendingCount(), 1u)
      << "the flattened C-API fast hit must enqueue Tier-2 itself";
  if (hit.hasReadToken)
    pool.releaseRead(hit.bucketIndex);
}

TEST_F(SharedTaskPoolTest, SharedPgoDisablesL0Fill) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setPgoEnabled(true, 2);

  void *out = nullptr;
  pool.l0Fill(5, codeFor(5), nullptr, 0);
  EXPECT_FALSE(pool.l0Try(5, nullptr, 0, &out));
}

TEST_F(SharedTaskPoolTest, SharedPgoInlineCacheWaitsForTier2) {
  ejitIcacheClearAll();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  constexpr uint32_t kFunc = 5;
  uintptr_t cell = 0;
  ejitIcacheRegisterSlot(kFunc, &cell, 0);
  pool.setPgoEnabled(true, 1);

  // Publish Tier-1. Its first hit crosses the threshold, but the wrapper cell
  // must remain empty so subsequent calls can continue observing the shared
  // PGO state until Tier-2 lands.
  ASSERT_EQ(pool.compileOrGet(kFunc, nullptr, 0, codeFor(kFunc)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  auto tier1 = pool.tryCacheHit0D(kFunc);
  ASSERT_EQ(tier1.status, EJitCompileOrGetStatus::CacheHit);
  pool.icacheFill(kFunc, tier1.fnPtr, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_EQ(cell, 0u);
  EXPECT_EQ(pool.pendingCount(), 1u);
  if (tier1.hasReadToken)
    pool.releaseRead(tier1.bucketIndex);

  // Tier-2 replaces the slot. The next normal resolve returns the Tier-2
  // pointer, which is now eligible for the calling core's private cell.
  ASSERT_TRUE(pool.pollOne());
  auto tier2 = pool.tryCacheHit0D(kFunc);
  ASSERT_EQ(tier2.status, EJitCompileOrGetStatus::CacheHit);
  pool.icacheFill(kFunc, tier2.fnPtr, nullptr, 0, pool.icacheBeginResolve());
  EXPECT_EQ(cell, reinterpret_cast<uintptr_t>(tier2.fnPtr));
  EXPECT_NE(tier2.fnPtr, tier1.fnPtr);
  if (tier2.hasReadToken)
    pool.releaseRead(tier2.bucketIndex);

  void *out = nullptr;
  EXPECT_TRUE(pool.icacheTry(kFunc, nullptr, 0, &out));
  EXPECT_EQ(out, tier2.fnPtr);
  ejitIcacheClearAll();
}

TEST_F(SharedTaskPoolTest, SharedPgoInlineCacheRequiresExactTier2Identity) {
  ejitIcacheClearAll();
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  constexpr uint32_t kFunc = 5;
  constexpr uint32_t D = EJIT_ICACHE_DIM_SIZE;
  uintptr_t cells[D] = {};
  ejitIcacheRegisterSlot(kFunc, cells, 1);
  pool.setPgoEnabled(true, 1);
  pool.setInstanceEnabled(1, 0, true);
  pool.setInstanceEnabled(1, 1, true);
  EJitDimPair d0[1] = {dim(1, 0)};
  EJitDimPair d1[1] = {dim(1, 1)};

  ASSERT_EQ(pool.compileOrGet(kFunc, d0, 1, codeFor(kFunc)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne());
  auto hit = pool.tryCacheHit1D(kFunc, 1, 0);
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  if (hit.hasReadToken)
    pool.releaseRead(hit.bucketIndex);
  ASSERT_TRUE(pool.pollOne()); // publish Tier-2 for d0 only
  auto tier2 = pool.tryCacheHit1D(kFunc, 1, 0);
  ASSERT_EQ(tier2.status, EJitCompileOrGetStatus::CacheHit);

  pool.icacheFill(kFunc, tier2.fnPtr, d1, 1, pool.icacheBeginResolve());
  EXPECT_EQ(cells[1], 0u) << "a Tier-2 pointer must not fill another identity";
  pool.icacheFill(kFunc, tier2.fnPtr, d0, 1, pool.icacheBeginResolve());
  EXPECT_EQ(cells[0], reinterpret_cast<uintptr_t>(tier2.fnPtr));
  if (tier2.hasReadToken)
    pool.releaseRead(tier2.bucketIndex);
  ejitIcacheClearAll();
}

// Shared equivalent of EJitTaskPoolTest::PgoHitThresholdArmsTier2Recompile.
// Set PGO threshold=3, publish Tier-1, hit it twice (below threshold), then
// the third hit crosses the threshold -> Tier-2 request enqueued.
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
  EXPECT_EQ(pool.pendingCount(), 0u);
  if (r1.hasReadToken)
    pool.releaseRead(r1.bucketIndex);

  // Verify hitCount incremented.
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->hitCount.loadRelaxed(), 1u);
  }

  // Second hit crosses threshold -> Tier-2 armed + enqueued on the SHARED
  // MPSC queue (no facade-local bypass).  The in-flight dedup bit for the
  // (stripped) funcIndex is now claimed, so pendingCount reflects the queued
  // Tier-2 request.
  auto r2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(r2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(pool.pendingCount(), 1u)
      << "Tier-2 request queued via shared queue";
  if (r2.hasReadToken)
    pool.releaseRead(r2.bucketIndex);

  // Verify hitCount.
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->hitCount.loadRelaxed(), 2u);
  }

  // Sampling is complete, but Tier-2 has not compiled yet. Further calls must
  // use AOT instead of continuing to execute the atomic-instrumented Tier-1.
  // The counter stays capped at the configured sample count.
  auto waiting = pool.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(waiting.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(waiting.fnPtr, codeFor(5));
  EXPECT_EQ(pool.pendingCount(), 1u);
  {
    EJitSharedCacheSlot *s = findReadySlot(5);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->hitCount.loadRelaxed(), 2u);
    EXPECT_EQ(s->tier.loadRelaxed(),
              static_cast<uint8_t>(kEJitTierInstrumented));
  }

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

  auto final = pool.compileOrGet(5, d0, 1, codeFor(5));
  EXPECT_EQ(final.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(final.fnPtr, codeFor(5));
  if (final.hasReadToken)
    pool.releaseRead(final.bucketIndex);
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
  EXPECT_EQ(state_->enqueuePos.loadRelaxed() - state_->dequeuePos.loadRelaxed(),
            1u);

  // A different function stays on its AOT fallback and adds no compiler work.
  auto deferred = pool.compileOrGet(6, nullptr, 0, codeFor(6));
  EXPECT_EQ(deferred.status, EJitCompileOrGetStatus::PgoAdmissionDeferred);
  EXPECT_EQ(deferred.fnPtr, codeFor(6));
  EXPECT_EQ(state_->enqueuePos.loadRelaxed() - state_->dequeuePos.loadRelaxed(),
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

TEST_F(SharedTaskPoolTest, SharedPgoProfilesOneVersionPerFunctionAtATime) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setPgoEnabled(true, 2, /*maxConcurrentProfiles=*/2);
  pool.setInstanceEnabled(1, 4, true);
  pool.setInstanceEnabled(1, 5, true);
  EJitDimPair d0[1] = {dim(1, 4)};
  EJitDimPair d1[1] = {dim(1, 5)};

  ASSERT_EQ(pool.compileOrGet(5, d0, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  auto deferred = pool.compileOrGet(5, d1, 1, codeFor(5));
  EXPECT_EQ(deferred.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(deferred.fnPtr, codeFor(5));
  EXPECT_EQ(pool.pendingCount(), 1u);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 1u);
  EXPECT_EQ(state_->pgoDeferredMisses.loadRelaxed(), 0u);

  ASSERT_TRUE(pool.pollOne()); // Publish d0 Tier-1.
  deferred = pool.compileOrGet(5, d1, 1, codeFor(5));
  EXPECT_EQ(deferred.status, EJitCompileOrGetStatus::PgoAdmissionDeferred);
  EXPECT_EQ(pool.pendingCount(), 0u);
  EXPECT_EQ(state_->pgoDeferredMisses.loadRelaxed(), 1u);

  for (unsigned i = 0; i < 2; ++i) {
    auto hit = pool.compileOrGet(5, d0, 1, codeFor(5));
    ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
    if (hit.hasReadToken)
      pool.releaseRead(hit.bucketIndex);
  }
  ASSERT_EQ(pool.pendingCount(), 1u);
  ASSERT_TRUE(pool.pollOne()); // Publish d0 Tier-2 and release admission.
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 0u);

  EXPECT_EQ(pool.compileOrGet(5, d1, 1, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 1u);
}

TEST_F(SharedTaskPoolTest, SharedPgoReleasesAdmissionForPreexistingWork) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);

  ASSERT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  pool.setPgoEnabled(true, 2);
  uint32_t admissionHooks = 0;
  pool.setPgoAdmissionTestHook(
      [](void *ctx) { ++*static_cast<uint32_t *>(ctx); }, &admissionHooks);

  EXPECT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(admissionHooks, 0u)
      << "an already-pending request must not transiently start PGO";
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(state_->pgoActiveFunctions[0].loadAcquire(), 0u);

  EXPECT_EQ(pool.compileOrGet(6, nullptr, 0, codeFor(6)).status,
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
  EXPECT_EQ(deferred.status, EJitCompileOrGetStatus::PgoAdmissionDeferred);
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

TEST_F(SharedTaskPoolTest,
       DesignatedOwnerWorkerServesConcurrentPgoProducerCores) {
  constexpr uint32_t kOwnerCore = 8;
  constexpr uint32_t kProducerCount = 3;
  constexpr uint32_t kProducerCores[kProducerCount] = {18, 19, 20};
  constexpr uint32_t kFuncIndices[kProducerCount] = {5, 6, 7};

  WorkerHooks hooks;
  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(kOwnerCore);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  owner.setPgoEnabled(true, /*threshold=*/2,
                      /*maxConcurrentProfiles=*/2);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  ASSERT_EQ(state_->ownerCoreId.loadAcquire(), kOwnerCore);
  ASSERT_EQ(hooks.starts, 1);
  ASSERT_EQ(hooks.startCore, kOwnerCore);

  EJitSharedTaskPool peers[kProducerCount];
  for (uint32_t i = 0; i < kProducerCount; ++i) {
    peers[i].setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
    EJitCoreId::setCurrentForTest(kProducerCores[i]);
    peers[i].bind(state_.get());
    peers[i].setMode(EJitCompileMode::Async);
    ASSERT_EQ(peers[i].init(), EJitSharedTaskPool::InitResult::AttachedReady);
  }
  ASSERT_EQ(hooks.starts, 1) << "peer cores must not start workers";

  EJitSharedTaskPool::CompileOrGetResult first[kProducerCount];
  std::atomic<uint32_t> ready{0};
  std::atomic<bool> go{false};
  std::vector<std::thread> producers;
  for (uint32_t i = 0; i < kProducerCount; ++i) {
    producers.emplace_back([&, i] {
      EJitCoreId::setCurrentForTest(kProducerCores[i]);
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      first[i] = peers[i].compileOrGet(kFuncIndices[i], nullptr, 0,
                                       codeFor(kFuncIndices[i]));
    });
  }
  while (ready.load(std::memory_order_acquire) != kProducerCount)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto &producer : producers)
    producer.join();

  uint32_t admitted = 0;
  uint32_t deferred = kProducerCount;
  for (uint32_t i = 0; i < kProducerCount; ++i) {
    if (first[i].status == EJitCompileOrGetStatus::EnqueuedPending) {
      ++admitted;
    } else {
      EXPECT_EQ(first[i].status, EJitCompileOrGetStatus::PgoAdmissionDeferred);
      EXPECT_EQ(first[i].fnPtr, codeFor(kFuncIndices[i]));
      deferred = i;
    }
  }
  ASSERT_EQ(admitted, 2u);
  ASSERT_LT(deferred, kProducerCount);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 2u);
  EXPECT_EQ(owner.pendingCount(), 2u);

  // Deterministically step the already-started core-8 worker. Producer-side
  // admission above used real concurrent host threads; manual polling keeps
  // compile completion ordering stable for the assertions below.
  EJitCoreId::setCurrentForTest(kOwnerCore);
  ASSERT_TRUE(owner.pollOne());
  ASSERT_TRUE(owner.pollOne());
  EXPECT_FALSE(owner.pollOne());
  for (uint32_t i = 0; i < kProducerCount; ++i) {
    if (i == deferred)
      continue;
    EJitSharedCacheSlot *slot = findReadySlot(kFuncIndices[i]);
    ASSERT_NE(slot, nullptr);
    EXPECT_EQ(slot->tier.loadRelaxed(),
              static_cast<uint8_t>(kEJitTierInstrumented));
  }

  // The same producer cores now run together. The admitted functions collect
  // two hits each and enqueue Tier-2; the deferred function remains on AOT.
  ready.store(0, std::memory_order_relaxed);
  go.store(false, std::memory_order_relaxed);
  producers.clear();
  EJitSharedTaskPool::CompileOrGetResult profile[kProducerCount][2];
  for (uint32_t i = 0; i < kProducerCount; ++i) {
    producers.emplace_back([&, i] {
      EJitCoreId::setCurrentForTest(kProducerCores[i]);
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      unsigned calls = i == deferred ? 1u : 2u;
      for (unsigned hit = 0; hit < calls; ++hit) {
        profile[i][hit] = peers[i].compileOrGet(kFuncIndices[i], nullptr, 0,
                                                codeFor(kFuncIndices[i]));
        if (profile[i][hit].hasReadToken)
          peers[i].releaseRead(profile[i][hit].bucketIndex);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != kProducerCount)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto &producer : producers)
    producer.join();

  EXPECT_EQ(profile[deferred][0].status,
            EJitCompileOrGetStatus::PgoAdmissionDeferred);
  EXPECT_EQ(profile[deferred][0].fnPtr, codeFor(kFuncIndices[deferred]));
  EXPECT_EQ(owner.pendingCount(), 2u);
  uint32_t profilesAtThreshold = 0;
  for (uint32_t i = 0; i < kEJitSharedMaxConcurrentProfiles; ++i) {
    if (state_->pgoActiveFunctions[i].loadAcquire() == 0)
      continue;
    ++profilesAtThreshold;
    EXPECT_EQ(state_->pgoProgressQuarters[i].loadAcquire(), 4u);
  }
  EXPECT_EQ(profilesAtThreshold, 2u);
  EJitCoreId::setCurrentForTest(kOwnerCore);
  ASSERT_TRUE(owner.pollOne());
  ASSERT_TRUE(owner.pollOne());
  EXPECT_EQ(state_->pgoCompletedFunctions.loadRelaxed(), 2u);
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 0u);

  // Once the two profiles finish, the function that stayed on AOT can claim a
  // slot. The designated owner remains the only worker throughout.
  EJitCoreId::setCurrentForTest(kProducerCores[deferred]);
  EXPECT_EQ(peers[deferred]
                .compileOrGet(kFuncIndices[deferred], nullptr, 0,
                              codeFor(kFuncIndices[deferred]))
                .status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EJitCoreId::setCurrentForTest(kOwnerCore);
  ASSERT_TRUE(owner.pollOne());
  EJitSharedCacheSlot *last = findReadySlot(kFuncIndices[deferred]);
  ASSERT_NE(last, nullptr);
  EXPECT_EQ(last->tier.loadRelaxed(),
            static_cast<uint8_t>(kEJitTierInstrumented));
  EXPECT_EQ(hooks.starts, 1);
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
  if (r1.hasReadToken)
    pool.releaseRead(r1.bucketIndex);
  auto r2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r2.status, EJitCompileOrGetStatus::CacheHit);
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

// PGO off → no hitCount increment and no Tier-2 enqueue. Stats builds still
// set the independent one-shot postPublishSeen diagnostic on the first reuse.
TEST_F(SharedTaskPoolTest, SharedPgoOffTracksPostPublishReuse) {
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
  EJitSharedCacheSlot *slot = findReadySlot(5);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->postPublishSeen.loadRelaxed(), 0u);

  // Hits remain ordinary cache hits and do not enqueue Tier-2.
  for (int i = 0; i < 10; ++i) {
    auto r = pool.compileOrGet(5, d0, 1, codeFor(5));
    ASSERT_EQ(r.status, EJitCompileOrGetStatus::CacheHit);
    if (r.hasReadToken)
      pool.releaseRead(r.bucketIndex);
  }
  EXPECT_EQ(pool.pendingCount(), 0u); // no Tier-2 enqueued

  // hitCount stays at 0 (threshold was never set).
  EXPECT_EQ(slot->hitCount.loadRelaxed(), 0u);
  EXPECT_EQ(slot->postPublishSeen.loadRelaxed(), 1u);
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
  if (r1.hasReadToken)
    pool.releaseRead(r1.bucketIndex);

  auto r2 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r2.status, EJitCompileOrGetStatus::CacheHit);
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
    EXPECT_NE(tier2Fn, tier1Fn) << "Tier-2 compile produced new code";
  }

  // Phase 5: subsequent hit returns Tier-2 pointer.
  auto r3 = pool.compileOrGet(5, d0, 1, codeFor(5));
  ASSERT_EQ(r3.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(r3.fnPtr, codeFor(5))
      << "hit after Tier-2 returns Tier-2 code, not fallback";
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

struct CapturedFailingCompile {
  uint32_t funcIndex = 0;
  uint32_t calls = 0;
};

bool mockCompileCaptureAndFail(void *ctx, const EJitCompileRequest &req,
                               void ** /*outFn*/) {
  auto *capture = static_cast<CapturedFailingCompile *>(ctx);
  capture->funcIndex = req.funcIndex;
  ++capture->calls;
  return false;
}

void disablePgoHook(void *ctx) {
  static_cast<EJitSharedTaskPool *>(ctx)->setPgoEnabled(false, 0);
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

#ifdef EJIT_SRE_TASKPOOL_NO_RECLAIM
TEST_F(SharedTaskPoolTest, PgoTier2PublishWaitsForExistingWriter) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  pool.setPgoEnabled(true, 1);

  ASSERT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(pool.pollOne()); // Publish Tier-1.
  auto hit = pool.tryCacheHit0D(5);
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  if (hit.hasReadToken)
    pool.releaseRead(hit.bucketIndex);
  ASSERT_EQ(pool.pendingCount(), 1u); // Tier-2 is queued.

  uint32_t bucket = bucketOfIdentity(5, nullptr, 0);
  state_->buckets[bucket].writeFlag.storeRelease(1);

  std::atomic<bool> ready{false};
  std::atomic<bool> start{false};
  std::atomic<bool> done{false};
  bool consumed = false;
  std::thread publisher([&] {
    ready.store(true, std::memory_order_release);
    while (!start.load(std::memory_order_acquire))
      std::this_thread::yield();
    consumed = pool.pollOne();
    done.store(true, std::memory_order_release);
  });

  while (!ready.load(std::memory_order_acquire))
    std::this_thread::yield();
  start.store(true, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_FALSE(done.load(std::memory_order_acquire))
      << "Tier-2 publish entered while another writer held writeFlag";

  state_->buckets[bucket].writeFlag.storeRelease(0);
  publisher.join();
  EXPECT_TRUE(consumed);
  EXPECT_EQ(pool.pendingCount(), 0u);
  EJitSharedCacheSlot *slot = findReadySlot(5);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->tier.loadRelaxed(), static_cast<uint8_t>(kEJitTierPgoUse));
}
#endif

TEST_F(SharedTaskPoolTest, PgoAdmissionSnapshotSelectsInstrumentedTier) {
  EJitSharedTaskPool pool;
  bringUpOwner(pool);
  EJitCoreId::setCurrentForTest(0);
  CapturedFailingCompile capture;
  pool.setCompiler(&mockCompileCaptureAndFail, &capture);
  pool.setPgoEnabled(true, 2);
  pool.setPgoAdmissionTestHook(&disablePgoHook, &pool);

  ASSERT_EQ(pool.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  pool.setPgoAdmissionTestHook(nullptr, nullptr);

  ASSERT_TRUE(pool.pollOne());
  ASSERT_EQ(capture.calls, 1u);
  EXPECT_EQ(stripReqTier(capture.funcIndex), 5u);
  EXPECT_EQ(decodeReqTier(capture.funcIndex), kEJitTierInstrumented)
      << "admission and queued tier must use the same PGO snapshot";
  EXPECT_EQ(state_->pgoActiveFunctionCount.loadAcquire(), 0u)
      << "Tier-1 failure must roll back the admission taken for this request";
  EXPECT_EQ(state_->pgoActiveFunctions[0].loadAcquire(), 0u);
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
  for (int i = 0; i < 3; ++i) {
    auto Hit = peer.compileOrGet(5, d0, 1, codeFor(5));
    if (Hit.hasReadToken)
      peer.releaseRead(Hit.bucketIndex);
  }
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
  auto hitAndRelease = [&](EJitSharedTaskPool &Pool) {
    auto Hit = Pool.compileOrGet(5, d0, 1, codeFor(5));
    if (Hit.hasReadToken)
      Pool.releaseRead(Hit.bucketIndex);
  };
  EJitCoreId::setCurrentForTest(1);
  hitAndRelease(peerA);
  hitAndRelease(peerA); // crosses -> enqueue
  EJitCoreId::setCurrentForTest(2);
  hitAndRelease(peerB);
  hitAndRelease(peerB); // crosses -> AlreadyPending

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

TEST_F(SharedTaskPoolTest, ConcurrentPeersCapTier1AtConfiguredSampleCount) {
  constexpr uint32_t kThreads = 8;
  constexpr uint32_t kCallsPerThread = 32;
  constexpr uint32_t kThreshold = 64;

  EJitSharedTaskPool owner;
  PgoRecorder rec;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompileRecordPgo, &rec);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  owner.setPgoEnabled(true, kThreshold);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  ASSERT_EQ(owner.compileOrGet(5, nullptr, 0, codeFor(5)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  std::atomic<uint32_t> ready{0};
  std::atomic<bool> go{false};
  std::atomic<uint32_t> tier1Calls{0};
  std::atomic<uint32_t> aotCalls{0};
  std::atomic<uint32_t> unexpected{0};
  std::vector<std::thread> peers;
  for (uint32_t thread = 0; thread < kThreads; ++thread) {
    peers.emplace_back([&, thread] {
      EJitCoreId::setCurrentForTest(thread + 1);
      ready.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      for (uint32_t call = 0; call < kCallsPerThread; ++call) {
        auto result = owner.compileOrGet(5, nullptr, 0, codeFor(5));
        if (result.status == EJitCompileOrGetStatus::CacheHit) {
          tier1Calls.fetch_add(1, std::memory_order_relaxed);
          if (result.hasReadToken)
            owner.releaseRead(result.bucketIndex);
        } else if (result.status == EJitCompileOrGetStatus::AlreadyPending &&
                   result.fnPtr == codeFor(5)) {
          aotCalls.fetch_add(1, std::memory_order_relaxed);
        } else
          unexpected.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  while (ready.load(std::memory_order_acquire) != kThreads)
    std::this_thread::yield();
  go.store(true, std::memory_order_release);
  for (auto &peer : peers)
    peer.join();

  EXPECT_EQ(tier1Calls.load(), kThreshold);
  EXPECT_EQ(aotCalls.load(), kThreads * kCallsPerThread - kThreshold);
  EXPECT_EQ(unexpected.load(), 0u);
  EJitSharedCacheSlot *slot = findReadySlot(5);
  ASSERT_NE(slot, nullptr);
  EXPECT_EQ(slot->hitCount.loadRelaxed(), kThreshold);
  EXPECT_EQ(slot->tier.loadRelaxed(),
            static_cast<uint8_t>(kEJitTierInstrumented));
  EXPECT_EQ(owner.pendingCount(), 1u);

  EJitCoreId::setCurrentForTest(0);
  ASSERT_TRUE(owner.pollOne());
  EXPECT_EQ(rec.tier2, 1);
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
  auto HitA = owner.compileOrGet(5, dA, 1, codeFor(5));
  if (HitA.hasReadToken)
    owner.releaseRead(HitA.bucketIndex);
  ASSERT_TRUE(owner.pollOne());
  auto HitB = owner.compileOrGet(6, dB, 1, codeFor(6));
  if (HitB.hasReadToken)
    owner.releaseRead(HitB.bucketIndex);
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

//===----------------------------------------------------------------------===//
// Owner-only engine lifecycle. Only the elected owner builds an ORC engine, and
// it releases it when it gives ownership up, so a handoff never leaves two.
//===----------------------------------------------------------------------===//

// Winner-only, and BEFORE the worker exists and before Ready is published: the
// worker can compile the instant it starts and a peer can enqueue the instant
// it sees Ready, so an engine built after either would be too late.
//
// Excluded from the fixed-worker-core test config (EJIT_TEST_FIXED_WORKER_CORE
// pins the worker to core 0): this test needs a NON-designated core (3) to win
// the election, which the fixed-core policy forbids by design.
#ifndef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE
TEST_F(SharedTaskPoolTest, OwnerSetupRunsOnTheWinnerBeforeWorkerAndReady) {
  WorkerHooks hooks;
  OwnerEngineLog winner, loser;
  winner.state = state_.get();
  winner.hooks = &hooks;
  loser.state = state_.get();

  EJitSharedTaskPool c0, c1;
  for (auto *pool : {&c0, &c1}) {
    pool->bind(state_.get());
    pool->setCompiler(&mockCompile, nullptr);
    pool->setMode(EJitCompileMode::Async);
  }
  c0.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
  c0.setOwnerElectedCallback(&mockOwnerElected, &winner);
  c1.setOwnerElectedCallback(&mockOwnerElected, &loser);

  EJitCoreId::setCurrentForTest(3);
  ASSERT_EQ(c0.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(4);
  ASSERT_EQ(c1.init(), EJitSharedTaskPool::InitResult::AttachedReady);

  EXPECT_EQ(winner.built, 1);
  EXPECT_EQ(winner.coreAtBuild, 3u);
  EXPECT_EQ(loser.built, 0) << "a peer built an LLJIT it can never use";

  EXPECT_EQ(winner.stateAtBuild,
            static_cast<uint32_t>(EJitSharedInitState::Initializing));
  EXPECT_EQ(winner.workerStartsAtBuild, 0);
  EXPECT_EQ(winner.taskIdAtBuild, 0ull);
  EXPECT_EQ(hooks.starts, 1);
  EXPECT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Ready));
}
#endif // !EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE

// A failed engine build is a clean init failure: OwnerSetupFailed, no worker.
TEST_F(SharedTaskPoolTest, OwnerSetupFailureStartsNoWorker) {
  WorkerHooks hooks;
  OwnerEngineLog log;
  log.failBuild = true;

  EJitSharedTaskPool owner;
  EJitCoreId::setCurrentForTest(0);
  owner.bind(state_.get());
  owner.setCompiler(&mockCompile, nullptr);
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);
  owner.setOwnerElectedCallback(&mockOwnerElected, &log);
  owner.setMode(EJitCompileMode::Async);

  EXPECT_EQ(owner.init(), EJitSharedTaskPool::InitResult::OwnerFailed);
  EXPECT_EQ(log.built, 1);
  EXPECT_EQ(hooks.starts, 0) << "a worker with no compiler behind it";
  EXPECT_EQ(state_->lastInitError.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitError::OwnerSetupFailed));
  EXPECT_FALSE(owner.isOwner());
  EXPECT_FALSE(owner.asyncServiceAvailable());
}

// The handoff: the old owner releases its engine and the new one builds its
// own, so the system never holds two. Release lands after the worker join and
// before the blob is up for election again.
//
// Excluded from the fixed-worker-core test config: the handoff here is to a
// DIFFERENT core (the peer at core 1), which the fixed-core policy forbids -
// under it only the designated core (0) may re-win, which the fixed-core tests
// below cover directly.
#ifndef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE
TEST_F(SharedTaskPoolTest, ReElectionReleasesTheOldEngineAndBuildsTheNew) {
  WorkerHooks ownerHooks, peerHooks;
  OwnerEngineLog ownerLog, peerLog;
  ownerLog.state = peerLog.state = state_.get();
  ownerLog.hooks = &ownerHooks;
  peerLog.hooks = &peerHooks;

  EJitSharedTaskPool owner, peer;
  for (auto *pool : {&owner, &peer}) {
    pool->bind(state_.get());
    pool->setCompiler(&mockCompile, nullptr);
    pool->setMode(EJitCompileMode::Async);
  }
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &ownerHooks);
  owner.setOwnerElectedCallback(&mockOwnerElected, &ownerLog);
  owner.setOwnerReleasedCallback(&mockOwnerReleased, &ownerLog);
  peer.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &peerHooks);
  peer.setOwnerElectedCallback(&mockOwnerElected, &peerLog);
  peer.setOwnerReleasedCallback(&mockOwnerReleased, &peerLog);

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(1);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);
  ASSERT_EQ(ownerLog.built, 1);
  ASSERT_EQ(peerLog.built, 0) << "the peer built an engine while attaching";

  EJitCoreId::setCurrentForTest(0);
  owner.ownerShutdown();
  EXPECT_EQ(ownerLog.released, 1) << "the former owner kept its engine";
  EXPECT_EQ(ownerLog.workerStopsAtRelease, 1)
      << "released before the worker was joined";
  EXPECT_EQ(ownerLog.stateAtRelease,
            static_cast<uint32_t>(EJitSharedInitState::Stopping))
      << "released after the blob was already up for re-election";

  EJitCoreId::setCurrentForTest(1);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EXPECT_EQ(peerLog.built, 1) << "the new owner must build its own engine";
  EXPECT_EQ(ownerLog.built, 1) << "the old owner was rebuilt";
  EXPECT_EQ(ownerLog.released, 1);
  EXPECT_TRUE(peer.asyncServiceAvailable());
}
#endif // !EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE

// A peer with no engine of its own can still flip the shared mode in both
// directions, because the elected owner compiles for every core.
TEST_F(SharedTaskPoolTest, PeerSwitchesSharedModeBothWays) {
  WorkerHooks hooks;
  EJitSharedTaskPool owner, peer;
  for (auto *pool : {&owner, &peer}) {
    pool->bind(state_.get());
    pool->setCompiler(&mockCompile, nullptr);
    pool->setMode(EJitCompileMode::Async);
  }
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(1);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);

  uint32_t gen = 0;
  ASSERT_TRUE(peer.asyncServiceAvailable(&gen));
  peer.setSharedMode(EJitCompileMode::Sync);
  EXPECT_EQ(owner.getSharedMode(), EJitCompileMode::Sync)
      << "the owner never saw the peer's flip";

  ASSERT_TRUE(peer.asyncServiceAvailable(&gen));
  EXPECT_TRUE(peer.publishSharedMode(EJitCompileMode::Async, gen))
      << "Sync -> Async is unreachable for a peer";
  EXPECT_EQ(owner.getSharedMode(), EJitCompileMode::Async);
}

// The shutdown race: a peer that validated the pool, then had the owner shut
// down underneath it, must NOT commit Async against a stopped worker.
TEST_F(SharedTaskPoolTest, ShutdownDuringModeSwitchRejectsTheAsyncPublish) {
  WorkerHooks hooks;
  EJitSharedTaskPool owner, peer;
  for (auto *pool : {&owner, &peer}) {
    pool->bind(state_.get());
    pool->setCompiler(&mockCompile, nullptr);
    pool->setMode(EJitCompileMode::Sync);
  }
  owner.setWorkerHooks(&mockWorkerStart, &mockWorkerStop, &hooks);

  EJitCoreId::setCurrentForTest(0);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  EJitCoreId::setCurrentForTest(1);
  ASSERT_EQ(peer.init(), EJitSharedTaskPool::InitResult::AttachedReady);

  // The peer checks and is told yes.
  uint32_t gen = 0;
  EJitCoreId::setCurrentForTest(1);
  ASSERT_TRUE(peer.asyncServiceAvailable(&gen));

  // The owner shuts down before the peer gets to publish.
  EJitCoreId::setCurrentForTest(0);
  owner.ownerShutdown();
  ASSERT_EQ(hooks.stops, 1);

  EJitCoreId::setCurrentForTest(1);
  EXPECT_FALSE(peer.publishSharedMode(EJitCompileMode::Async, gen))
      << "published Async over a stopped worker";
  EXPECT_FALSE(peer.asyncServiceAvailable())
      << "still claims the pool can service async";
  // And nothing can be enqueued anyway: every producer path gates on Ready.
  const EJitDimPair d[1] = {{0, 0}};
  EXPECT_EQ(peer.compileOrGet(1, d, 1, codeFor(1)).status,
            EJitCompileOrGetStatus::OffMode);
}

// A Ready blob whose worker never started is not serviceable: a request
// enqueued there would sit pending forever.
TEST_F(SharedTaskPoolTest, AsyncServiceUnavailableWithoutAWorker) {
  EJitSharedTaskPool owner;
  bringUpOwner(owner); // no worker hooks injected: tests drive pollOne()
  ASSERT_EQ(state_->initState.loadAcquire(),
            static_cast<uint32_t>(EJitSharedInitState::Ready));
  EXPECT_FALSE(owner.asyncServiceAvailable());
}

} // namespace
