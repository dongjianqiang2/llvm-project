//===-- EJitFixedNearHotTaskPoolTest.cpp - fixed near-hot tests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/Support/Error.h"
#include "gtest/gtest.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

#if defined(_WIN32)
namespace llvm::ejit {
const std::string &EJitModuleLoader::getFuncNameByFuncIdx(uint32_t) const {
  static const std::string Empty;
  return Empty;
}
} // namespace llvm::ejit
#endif

namespace {

void *codeFor(uint32_t FuncIndex) {
  return reinterpret_cast<void *>(0x100000ull +
                                  static_cast<uintptr_t>(FuncIndex) * 64u);
}

EJitDimPair dim(uint32_t Type, uint32_t Instance) {
  return EJitDimPair{Type, Instance};
}

uint64_t mockNowCycles(void *Ctx) {
  return *static_cast<uint64_t *>(Ctx);
}

struct FixedNearEntry {
  void *fn = nullptr;
  uint32_t funcIndex = 0;
  uint32_t poolId = kEJitNearHotPublicPoolId;
  bool ready = false;
  uint32_t tier = kEJitTierBaseline;
};

struct FixedNearPublishCtx {
  std::vector<FixedNearEntry> entries;
  uint32_t flushCalls = 0;
  bool failCell0 = false;
  bool failCell1 = false;
  bool baselineReadyImmediately = false;
};

struct RealFixedNearPublishCtx {
  struct Entry {
    void *fn = nullptr;
    uint32_t tier = kEJitTierBaseline;
  };

  RealFixedNearPublishCtx() : storage(3 * PageSize) {
    uintptr_t Raw = reinterpret_cast<uintptr_t>(storage.data());
    base = (Raw + PageSize - 1) & ~(uintptr_t(PageSize) - 1);
    EJitCodePoolManager::Options Opts;
    Opts.kind = EJitCodePoolKind::Near;
    Opts.poolId = 0;
    Opts.poolSize = PageSize;
    Opts.poolAlign = PageSize;
    Opts.minCodeAlign = 16;
    Opts.fourKSeal = true;
    Opts.batchedPageSeal = true;
    Opts.sealPageSize = PageSize;
    Opts.fixedBase = base;
    Opts.fixedSize = PageSize;
    Opts.needsEnableRw = true;
    pool = std::make_unique<EJitCodePoolManager>(
        Opts, [](size_t) -> void * { return nullptr; },
        [this](void *) {
          timeline.push_back('X');
          return 0u;
        },
        [this](void *, size_t) {
          timeline.push_back('S');
          return 0u;
        },
        [this](void *) {
          timeline.push_back('W');
          return 0u;
        });
  }

  static constexpr size_t PageSize = 4096;
  std::vector<uint8_t> storage;
  uintptr_t base = 0;
  std::unique_ptr<EJitCodePoolManager> pool;
  std::vector<Entry> entries;
  std::vector<char> timeline;
  uint32_t flushCalls = 0;
};

uint32_t fixedNearPoolForRequest(const EJitCompileRequest &Req) {
  if (Req.numDims == 0)
    return kEJitNearHotPublicPoolId;
  return Req.dims[0].instanceId < kEJitNearHotCellPoolCount
             ? Req.dims[0].instanceId
             : kEJitNearHotPublicPoolId;
}

bool mockFixedNearCompile(void *Ctx, const EJitCompileRequest &Req,
                          void **OutFn) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  void *Fn = reinterpret_cast<void *>(
      0x600000ull + static_cast<uintptr_t>(C->entries.size()) * 0x100u);
  const uint32_t Tier = decodeReqTier(Req.funcIndex);
  C->entries.push_back({Fn, stripReqTier(Req.funcIndex),
                        fixedNearPoolForRequest(Req),
                        Tier == kEJitTierInstrumented ||
                            (Tier == kEJitTierBaseline &&
                             C->baselineReadyImmediately),
                        Tier});
  *OutFn = Fn;
  return true;
}

bool mockFixedNearReady(void *Ctx, const void *Fn) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  for (const FixedNearEntry &Entry : C->entries)
    if (Entry.fn == Fn)
      return Entry.ready;
  return false;
}

bool mockFixedNearRange(void *Ctx, const void *Fn, EJitCompiledCodeInfo *Out) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  for (const FixedNearEntry &Entry : C->entries) {
    if (Entry.fn != Fn)
      continue;
    Out->fnPtr = const_cast<void *>(Fn);
    Out->codeStart = reinterpret_cast<uintptr_t>(Fn);
    Out->codeSize = 64;
    Out->poolId = Entry.poolId;
    Out->poolKind = EJitCodePoolKind::Near;
    Out->poolBase =
        0x80000000ull + static_cast<uint64_t>(Entry.poolId) * 0x200000ull;
    Out->poolSize =
        Entry.poolId == kEJitNearHotPublicPoolId ? 0x400000ull : 0x200000ull;
    return true;
  }
  return false;
}

bool mockFixedNearLegacyFlush(void *) { return true; }

bool mockFixedNearFlushPool(void *Ctx, uint32_t PoolId) {
  auto *C = static_cast<FixedNearPublishCtx *>(Ctx);
  ++C->flushCalls;
  if (PoolId == 0 && C->failCell0)
    return false;
  if (PoolId == 1 && C->failCell1)
    return false;
  for (FixedNearEntry &Entry : C->entries)
    if (Entry.poolId == PoolId)
      Entry.ready = true;
  return true;
}

bool mockWrongFixedNearFlushPool(void *, uint32_t) { return true; }

bool realFixedNearCompile(void *Ctx, const EJitCompileRequest &Req,
                          void **OutFn) {
  auto *C = static_cast<RealFixedNearPublishCtx *>(Ctx);
  const uint32_t Tier = decodeReqTier(Req.funcIndex);
  if (Tier == kEJitTierInstrumented) {
    void *Fn = reinterpret_cast<void *>(
        0x700000u + static_cast<uintptr_t>(C->entries.size()) * 0x100u);
    C->entries.push_back({Fn, Tier});
    *OutFn = Fn;
    return true;
  }
  auto Alloc = C->pool->allocateCode(64, 16);
  if (!Alloc) {
    consumeError(Alloc.takeError());
    return false;
  }
  void *Fn = *Alloc;
  if (Error Err = C->pool->enableRwRange(Fn, 64)) {
    consumeError(std::move(Err));
    return false;
  }
  if (!C->pool->recordPendingRange(Fn, 64))
    return false;
  C->pool->notePendingAllocation();
  C->entries.push_back({Fn, Tier});
  *OutFn = Fn;
  return true;
}

bool realFixedNearReady(void *Ctx, const void *Fn) {
  auto *C = static_cast<RealFixedNearPublishCtx *>(Ctx);
  for (const auto &Entry : C->entries)
    if (Entry.fn == Fn)
      return Entry.tier == kEJitTierInstrumented ||
             C->pool->isRangeReady(Fn);
  return false;
}

bool realFixedNearRange(void *Ctx, const void *Fn,
                        EJitCompiledCodeInfo *Out) {
  auto *C = static_cast<RealFixedNearPublishCtx *>(Ctx);
  for (const auto &Entry : C->entries) {
    if (Entry.fn != Fn)
      continue;
    if (Entry.tier != kEJitTierInstrumented)
      return C->pool->findRange(Fn, *Out) ||
             C->pool->findPendingRange(Fn, *Out);
    Out->fnPtr = const_cast<void *>(Fn);
    Out->codeStart = reinterpret_cast<uintptr_t>(Fn);
    Out->codeSize = 64;
    Out->poolId = kEJitFarPoolId;
    Out->poolKind = EJitCodePoolKind::Far;
    Out->poolBase = Out->codeStart & ~uint64_t(0xfffu);
    Out->poolSize = RealFixedNearPublishCtx::PageSize;
    return true;
  }
  return false;
}

bool realFixedNearFlushPool(void *Ctx, uint32_t PoolId) {
  auto *C = static_cast<RealFixedNearPublishCtx *>(Ctx);
  ++C->flushCalls;
  C->timeline.push_back('F');
  if (PoolId != 0)
    return false;
  if (Error Err = C->pool->flushPendingRanges()) {
    consumeError(std::move(Err));
    return false;
  }
  return true;
}

class FixedNearTaskPoolTest : public ::testing::Test {
protected:
  void SetUp() override {
    EJitCoreId::resetForTest();
    ejitIcacheClearAll();
    State = std::make_unique<EJitSharedTaskPoolState>();
  }

  void TearDown() override {
    ejitIcacheClearAll();
    EJitCoreId::resetForTest();
  }

  EJitSharedCacheSlot *findReadySlot(uint32_t FuncIndex) {
    for (uint32_t Bucket = 0; Bucket < kEJitSharedCacheBuckets; ++Bucket)
      for (uint32_t SlotIndex = 0; SlotIndex < kEJitSharedCacheSlots;
           ++SlotIndex) {
        EJitSharedCacheSlot &Slot = State->buckets[Bucket].slots[SlotIndex];
        if (Slot.state.loadAcquire() ==
                static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
            Slot.funcIndex == FuncIndex)
          return &Slot;
      }
    return nullptr;
  }

  uint32_t linkedPendingCount() const {
    uint32_t Count = 0;
    for (uint32_t I = 0; I < kEJitSharedLinkedPendingSlots; ++I)
      Count += State->pgoLinkedPending[I].token != 0;
    return Count;
  }

  std::unique_ptr<EJitSharedTaskPoolState> State;
};

TEST_F(FixedNearTaskPoolTest, WaitsForQuiescenceAndPublishesByPool) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 0, true);
  Owner.setInstanceEnabled(0, 1, true);

  const EJitDimPair Cell0[1] = {dim(0, 0)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  EXPECT_EQ(Owner.compileOrGet(201, Cell0, 1, codeFor(201)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(202, Cell1, 1, codeFor(202)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(203, nullptr, 0, codeFor(203)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(3), 3u);
  ASSERT_EQ(Owner.pendingPublishCount(), 3u);
  ASSERT_EQ(Ctx.flushCalls, 0u);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 3u);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, 3u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  ASSERT_EQ(Ctx.entries.size(), 3u);
  EXPECT_EQ(Ctx.entries[0].poolId, 0u);
  EXPECT_EQ(Ctx.entries[1].poolId, 1u);
  EXPECT_EQ(Ctx.entries[2].poolId, kEJitNearHotPublicPoolId);
  for (const FixedNearEntry &Entry : Ctx.entries)
    EXPECT_TRUE(Entry.ready);

  auto H0 = Owner.compileOrGet(201, Cell0, 1, codeFor(201));
  auto H1 = Owner.compileOrGet(202, Cell1, 1, codeFor(202));
  auto HP = Owner.compileOrGet(203, nullptr, 0, codeFor(203));
  EXPECT_EQ(H0.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(H1.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(HP.status, EJitCompileOrGetStatus::CacheHit);
  if (H0.hasReadToken)
    Owner.releaseRead(H0.bucketIndex);
  if (H1.hasReadToken)
    Owner.releaseRead(H1.bucketIndex);
  if (HP.hasReadToken)
    Owner.releaseRead(HP.bucketIndex);
}

TEST_F(FixedNearTaskPoolTest, Accumulates150PgoVersionsUntilQuietWindow) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  uint64_t Now = 1;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setNowTicksSource(&mockNowCycles, &Now);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t FirstFunc = 400;
  constexpr uint32_t VersionCount = 150;
  for (uint32_t Func = FirstFunc; Func != FirstFunc + VersionCount; ++Func) {
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
    auto Tier1 = Owner.tryCacheHit0D(Func);
    ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
    if (Tier1.hasReadToken)
      Owner.releaseRead(Tier1.bucketIndex);
    ASSERT_TRUE(Owner.pollOne());
  }

  EXPECT_EQ(Owner.pendingPublishCount(), VersionCount);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  Now += EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(FixedNearTaskPoolTest,
       SameFunctionSixteenCellsReleaseVpGateAfterEachTier2Link) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  uint64_t Now = 1;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setNowTicksSource(&mockNowCycles, &Now);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t Func = 560;
  for (uint32_t Cell = 0; Cell != kEJitNearHotCellPoolCount; ++Cell)
    Owner.setInstanceEnabled(0, Cell, true);

  bool NextCellAlreadyQueued = false;
  for (uint32_t Cell = 0; Cell != kEJitNearHotCellPoolCount; ++Cell) {
    const EJitDimPair Dims[1] = {dim(0, Cell)};
    if (!NextCellAlreadyQueued)
      ASSERT_EQ(Owner.compileOrGet(Func, Dims, 1, codeFor(Func)).status,
                EJitCompileOrGetStatus::EnqueuedPending);
    NextCellAlreadyQueued = false;
    ASSERT_TRUE(Owner.pollOne());

    if (Cell + 1 != kEJitNearHotCellPoolCount) {
      const EJitDimPair NextDims[1] = {dim(0, Cell + 1)};
      EXPECT_EQ(Owner.compileOrGet(Func, NextDims, 1, codeFor(Func)).status,
                EJitCompileOrGetStatus::PgoAdmissionDeferred);
    }

    auto Tier1 = Owner.tryCacheHit1D(Func, 0, Cell);
    ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
    if (Tier1.hasReadToken)
      Owner.releaseRead(Tier1.bucketIndex);
    ASSERT_TRUE(Owner.pollOne());
    ASSERT_EQ(Owner.pendingPublishCount(), Cell + 1u);

    if (Cell + 1 != kEJitNearHotCellPoolCount) {
      const EJitDimPair NextDims[1] = {dim(0, Cell + 1)};
      ASSERT_EQ(Owner.compileOrGet(Func, NextDims, 1, codeFor(Func)).status,
                EJitCompileOrGetStatus::EnqueuedPending);
      NextCellAlreadyQueued = true;
    }
  }

  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), 0u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  Now += EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  // One quiet-window batch commits each of the 16 independent cell pools.
  EXPECT_EQ(Ctx.flushCalls, kEJitNearHotCellPoolCount);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(),
            kEJitNearHotCellPoolCount);
  for (uint32_t Cell = 0; Cell != kEJitNearHotCellPoolCount; ++Cell) {
    auto Tier2 = Owner.tryCacheHit1D(Func, 0, Cell);
    EXPECT_EQ(Tier2.status, EJitCompileOrGetStatus::CacheHit);
    EXPECT_NE(Tier2.fnPtr, codeFor(Func));
    if (Tier2.hasReadToken)
      Owner.releaseRead(Tier2.bucketIndex);
  }
}

TEST_F(FixedNearTaskPoolTest,
       FourFunctionsRetainAllSixteenCellVersionsUntilOneQuietBatch) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  uint64_t Now = 1;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setNowTicksSource(&mockNowCycles, &Now);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t FirstFunc = 600;
  constexpr uint32_t FuncCount = 4;
  for (uint32_t Cell = 0; Cell != kEJitNearHotCellPoolCount; ++Cell)
    Owner.setInstanceEnabled(0, Cell, true);
  for (uint32_t Cell = 0; Cell != kEJitNearHotCellPoolCount; ++Cell) {
    for (uint32_t Func = FirstFunc; Func != FirstFunc + FuncCount; ++Func) {
      const EJitDimPair Dims[1] = {dim(0, Cell)};
      ASSERT_EQ(Owner.compileOrGet(Func, Dims, 1, codeFor(Func)).status,
                EJitCompileOrGetStatus::EnqueuedPending);
    }
    EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), FuncCount);
    ASSERT_EQ(Owner.pollBudget(FuncCount), FuncCount);
    for (uint32_t Func = FirstFunc; Func != FirstFunc + FuncCount; ++Func) {
      auto Tier1 = Owner.tryCacheHit1D(Func, 0, Cell);
      ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
      if (Tier1.hasReadToken)
        Owner.releaseRead(Tier1.bucketIndex);
    }
    ASSERT_EQ(Owner.pollBudget(FuncCount), FuncCount);
    EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 0u);
  }

  constexpr uint32_t VersionCount =
      FuncCount * kEJitNearHotCellPoolCount;
  EXPECT_EQ(Owner.pendingPublishCount(), VersionCount);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  Now += EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, kEJitNearHotCellPoolCount);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), VersionCount);

  for (uint32_t Func = FirstFunc; Func != FirstFunc + FuncCount; ++Func)
    for (uint32_t Cell = 0; Cell != kEJitNearHotCellPoolCount; ++Cell) {
      auto Tier2 = Owner.tryCacheHit1D(Func, 0, Cell);
      EXPECT_EQ(Tier2.status, EJitCompileOrGetStatus::CacheHit);
      if (Tier2.hasReadToken)
        Owner.releaseRead(Tier2.bucketIndex);
  }
}

TEST_F(FixedNearTaskPoolTest,
       LinkedTier2RegistrySurvivesTier1SameBucketEviction) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t Target = 580;
  ASSERT_EQ(Owner.compileOrGet(Target, nullptr, 0, codeFor(Target)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Tier1 = Owner.tryCacheHit0D(Target);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  ASSERT_EQ(linkedPendingCount(), 1u);
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(State->pgoVpFunctionGates[Target].loadAcquire(), 0u);

  // Fill all 16 ways with baseline identities that hash to the target bucket.
  // The last insertion deterministically evicts the target's Tier-1 slot.
  Owner.setPgoEnabled(false, 0);
  Ctx.baselineReadyImmediately = true;
  for (uint32_t I = 1; I <= kEJitSharedCacheSlots; ++I) {
    const uint32_t Collider = Target + I * kEJitSharedCacheBuckets;
    ASSERT_LT(Collider, kEJitSharedMaxFuncIndex);
    ASSERT_EQ(Owner.compileOrGet(Collider, nullptr, 0, codeFor(Collider)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
  }
  ASSERT_EQ(findReadySlot(Target), nullptr);

  Owner.setPgoEnabled(true, 1, 2);
  const size_t CompilesBeforeRetry = Ctx.entries.size();
  auto Retry = Owner.compileOrGet(Target, nullptr, 0, codeFor(Target));
  EXPECT_EQ(Retry.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(Retry.fnPtr, codeFor(Target));
  EXPECT_EQ(Ctx.entries.size(), CompilesBeforeRetry);
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(linkedPendingCount(), 1u);

  ASSERT_TRUE(Owner.flushCodeBatch());
  EXPECT_EQ(linkedPendingCount(), 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), 1u);
  auto Tier2 = Owner.tryCacheHit0D(Target);
  EXPECT_EQ(Tier2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(Tier2.fnPtr, codeFor(Target));
  if (Tier2.hasReadToken)
    Owner.releaseRead(Tier2.bucketIndex);

  uint32_t TargetTier1Compiles = 0;
  uint32_t TargetTier2Compiles = 0;
  for (const FixedNearEntry &Entry : Ctx.entries) {
    if (Entry.funcIndex != Target)
      continue;
    TargetTier1Compiles += Entry.tier == kEJitTierInstrumented;
    TargetTier2Compiles += Entry.tier == kEJitTierPgoUse;
  }
  EXPECT_EQ(TargetTier1Compiles, 1u);
  EXPECT_EQ(TargetTier2Compiles, 1u);
}

struct LinkedPendingRaceCtx {
  EJitSharedTaskPool *pool = nullptr;
  bool actionSucceeded = false;
};

void linkTier2AfterRegistryMiss(void *Opaque) {
  auto &Race = *static_cast<LinkedPendingRaceCtx *>(Opaque);
  Race.pool->setPgoLinkedPendingMissTestHook(nullptr, nullptr);
  Race.actionSucceeded = Race.pool->pollOne();
}

void publishTier2AfterCacheMiss(void *Opaque) {
  auto &Race = *static_cast<LinkedPendingRaceCtx *>(Opaque);
  Race.pool->setPgoCacheMissTestHook(nullptr, nullptr);
  Race.actionSucceeded = Race.pool->flushCodeBatch();
}

struct DispatchEpochChurnCtx {
  EJitSharedTaskPoolState *state = nullptr;
  uint32_t calls = 0;
};

void advanceDispatchEpochAfterRegistryMiss(void *Opaque) {
  auto &Churn = *static_cast<DispatchEpochChurnCtx *>(Opaque);
  ++Churn.calls;
  Churn.state->dispatchEpoch.fetchAdd(1);
}

TEST_F(FixedNearTaskPoolTest,
       RegistryMissCannotRaceTier2LinkAndDedupRelease) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t Func = 583;
  ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Tier1 = Owner.tryCacheHit0D(Func);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_EQ(Owner.pendingCount(), 1u);
  EJitSharedCacheSlot *Tier1Slot = findReadySlot(Func);
  ASSERT_NE(Tier1Slot, nullptr);
  Tier1Slot->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Empty));

  LinkedPendingRaceCtx Race{&Owner};
  Owner.setPgoLinkedPendingMissTestHook(&linkTier2AfterRegistryMiss, &Race);
  const size_t CompilesBeforeRetry = Ctx.entries.size();
  auto Retry = Owner.compileOrGet(Func, nullptr, 0, codeFor(Func));
  EXPECT_TRUE(Race.actionSucceeded);
  EXPECT_EQ(Retry.status, EJitCompileOrGetStatus::AlreadyPending);
  EXPECT_EQ(Ctx.entries.size(), CompilesBeforeRetry + 1u);
  EXPECT_EQ(Owner.pendingCount(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(linkedPendingCount(), 1u);
  ASSERT_TRUE(Owner.flushCodeBatch());
}

TEST_F(FixedNearTaskPoolTest,
       CacheMissRetriesAfterTier2PublishAndRegistryClear) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t Func = 584;
  ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Tier1 = Owner.tryCacheHit0D(Func);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(linkedPendingCount(), 1u);
  EJitSharedCacheSlot *Tier1Slot = findReadySlot(Func);
  ASSERT_NE(Tier1Slot, nullptr);
  Tier1Slot->state.storeRelease(
      static_cast<uint32_t>(EJitSharedSlotState::Empty));

  LinkedPendingRaceCtx Race{&Owner};
  Owner.setPgoCacheMissTestHook(&publishTier2AfterCacheMiss, &Race);
  const size_t CompilesBeforeRetry = Ctx.entries.size();
  auto Tier2 = Owner.compileOrGet(Func, nullptr, 0, codeFor(Func));
  EXPECT_TRUE(Race.actionSucceeded);
  EXPECT_EQ(Tier2.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_NE(Tier2.fnPtr, codeFor(Func));
  EXPECT_EQ(Ctx.entries.size(), CompilesBeforeRetry);
  EXPECT_EQ(Owner.pendingCount(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(linkedPendingCount(), 0u);
  if (Tier2.hasReadToken)
    Owner.releaseRead(Tier2.bucketIndex);
}

TEST_F(FixedNearTaskPoolTest,
       PersistentPublishContentionFallsBackWithoutEnqueue) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  DispatchEpochChurnCtx Churn{State.get()};
  Owner.setPgoLinkedPendingMissTestHook(
      &advanceDispatchEpochAfterRegistryMiss, &Churn);
  auto Result = Owner.compileOrGet(585, nullptr, 0, codeFor(585));
  EXPECT_EQ(Result.status, EJitCompileOrGetStatus::PgoAdmissionDeferred);
  EXPECT_EQ(Result.fnPtr, codeFor(585));
  EXPECT_EQ(Churn.calls, 3u);
  EXPECT_EQ(Owner.pendingCount(), 0u);
  EXPECT_EQ(State->inFlight[585].loadAcquire(), 0u);
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 0u);
}

TEST_F(FixedNearTaskPoolTest,
       LinkedTier2RegistryFullRetainsOriginalPgoOwnership) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t Func = 581;
  ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Tier1 = Owner.tryCacheHit0D(Func);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);

  for (uint32_t I = 0; I < kEJitSharedLinkedPendingSlots; ++I)
    State->pgoLinkedPending[I].token = UINT64_C(0x10000) + I;
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(linkedPendingCount(), kEJitSharedLinkedPendingSlots);
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 1u);
  EXPECT_NE(State->pgoVpFunctionGates[Func].loadAcquire(), 0u);
  EXPECT_NE(State->inFlight[Func].loadAcquire(), 0u);

  // The synthetic entries only force the registration failure. Publication of
  // the unregistered request must settle through its retained original token.
  for (uint32_t I = 0; I < kEJitSharedLinkedPendingSlots; ++I)
    State->pgoLinkedPending[I].token = 0;
  ASSERT_TRUE(Owner.flushCodeBatch());
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(State->pgoVpFunctionGates[Func].loadAcquire(), 0u);
  EXPECT_EQ(State->inFlight[Func].loadAcquire(), 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), 1u);
}

TEST_F(FixedNearTaskPoolTest,
       LinkedTier2VersionMismatchClearsExactRegistryEntry) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t Func = 582;
  const EJitDimPair Cell0[1] = {dim(0, 0)};
  Owner.setInstanceEnabled(0, 0, true);
  ASSERT_EQ(Owner.compileOrGet(Func, Cell0, 1, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Tier1 = Owner.tryCacheHit1D(Func, 0, 0);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(linkedPendingCount(), 1u);

  Owner.setInstanceEnabled(0, 0, false);
  Owner.setInstanceEnabled(0, 0, true);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(linkedPendingCount(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), 0u);
  EXPECT_EQ(Owner.compileOrGet(Func, Cell0, 1, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
}

TEST_F(FixedNearTaskPoolTest, PgoActivityRestartsQuietWindow) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  uint64_t Now = 1;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setNowTicksSource(&mockNowCycles, &Now);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 1);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  auto CompileTier2 = [&](uint32_t Func) {
    EXPECT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    EXPECT_TRUE(Owner.pollOne());
    auto Tier1 = Owner.tryCacheHit0D(Func);
    EXPECT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
    if (Tier1.hasReadToken)
      Owner.releaseRead(Tier1.bucketIndex);
    EXPECT_TRUE(Owner.pollOne());
  };

  CompileTier2(600);
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  Now += EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES - 1;
  CompileTier2(601);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  ++Now;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  Now += EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES - 1;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(FixedNearTaskPoolTest, CapacityAndExplicitPublishRemainImmediate) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  uint64_t Now = 1;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setNowTicksSource(&mockNowCycles, &Now);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 4);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  constexpr uint32_t FirstFunc = 800;
  constexpr uint32_t VersionCount =
      EJIT_CODE_POOL_FIXED_NEAR_HOT_PENDING_LIMIT + 1;
  for (uint32_t Func = FirstFunc; Func != FirstFunc + VersionCount; ++Func) {
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
    auto Tier1 = Owner.tryCacheHit0D(Func);
    ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
    if (Tier1.hasReadToken)
      Owner.releaseRead(Tier1.bucketIndex);
    ASSERT_TRUE(Owner.pollOne());
  }

  EXPECT_EQ(Ctx.flushCalls, 1u);
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_TRUE(Owner.flushCodeBatch());
  EXPECT_EQ(Ctx.flushCalls, 2u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(FixedNearTaskPoolTest, ColdProfileTimeoutStartsFinalQuietWindow) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  uint64_t Now = 1;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setNowTicksSource(&mockNowCycles, &Now);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  for (uint32_t Func : {1100u, 1101u}) {
    ASSERT_EQ(Owner.compileOrGet(Func, nullptr, 0, codeFor(Func)).status,
              EJitCompileOrGetStatus::EnqueuedPending);
    ASSERT_TRUE(Owner.pollOne());
  }
  auto Tier1 = Owner.tryCacheHit0D(1100);
  ASSERT_EQ(Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Tier1.hasReadToken)
    Owner.releaseRead(Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Ctx.flushCalls, 0u);

  Now += EJIT_SRE_PGO_COLD_PROFILE_TIMEOUT_TICKS + 1;
  for (unsigned Step = 0;
       Step != 8 && State->pgoActiveFunctionCount.loadAcquire() != 0; ++Step)
    (void)Owner.workerPollOnce();
  ASSERT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  Now += EJIT_SRE_PGO_PUBLISH_QUIET_CYCLES;
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

struct PublishBarrierRaceCtx {
  EJitSharedTaskPool *pool = nullptr;
  EJitCompileOrGetStatus status = EJitCompileOrGetStatus::InvalidParam;
};

struct AbortRecorder {
  uint32_t calls = 0;
  uint64_t token = 0;
};

void recordAbort(void *Opaque, const EJitCompileRequest &Req) {
  auto &Record = *static_cast<AbortRecorder *>(Opaque);
  ++Record.calls;
  Record.token = Req.requestToken;
}

TEST_F(FixedNearTaskPoolTest,
       OldCellTerminalPublishFailureDoesNotClearNewCellPgoToken) {
  FixedNearPublishCtx Ctx;
  AbortRecorder Abort;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setAbortPgoProfileCallback(&recordAbort, &Abort);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 0, true);
  Owner.setInstanceEnabled(0, 1, true);

  constexpr uint32_t Func = 580;
  const EJitDimPair Cell0[1] = {dim(0, 0)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  ASSERT_EQ(Owner.compileOrGet(Func, Cell0, 1, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  const uint64_t Cell0Token = State->pgoVpFunctionGates[Func].loadAcquire();
  ASSERT_NE(Cell0Token, 0u);
  ASSERT_TRUE(Owner.pollOne());
  auto Cell0Tier1 = Owner.tryCacheHit1D(Func, 0, 0);
  ASSERT_EQ(Cell0Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Cell0Tier1.hasReadToken)
    Owner.releaseRead(Cell0Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  ASSERT_EQ(linkedPendingCount(), 1u);
  EXPECT_EQ(State->pgoVpFunctionGates[Func].loadAcquire(), 0u);

  ASSERT_EQ(Owner.compileOrGet(Func, Cell1, 1, codeFor(Func)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  const uint64_t Cell1Token = State->pgoVpFunctionGates[Func].loadAcquire();
  ASSERT_NE(Cell1Token, 0u);
  ASSERT_NE(Cell1Token, Cell0Token);
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 1u);

  Ctx.failCell0 = true;
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(linkedPendingCount(), 1u);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(linkedPendingCount(), 1u);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(linkedPendingCount(), 0u);
  EXPECT_EQ(Abort.calls, 1u);
  EXPECT_EQ(Abort.token, Cell0Token);
  EXPECT_EQ(State->pgoVpFunctionGates[Func].loadAcquire(), Cell1Token);
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 1u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), 0u);

  auto Cell1Tier1 = Owner.tryCacheHit1D(Func, 0, 1);
  ASSERT_EQ(Cell1Tier1.status, EJitCompileOrGetStatus::CacheHit);
  if (Cell1Tier1.hasReadToken)
    Owner.releaseRead(Cell1Tier1.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(State->pgoActiveFunctionCount.loadAcquire(), 0u);
  EXPECT_EQ(linkedPendingCount(), 1u);
  Ctx.failCell0 = false;
  EXPECT_TRUE(Owner.flushCodeBatch());
  EXPECT_EQ(linkedPendingCount(), 0u);
  EXPECT_EQ(State->pgoCompletedFunctions.loadRelaxed(), 1u);
  auto Cell1Tier2 = Owner.tryCacheHit1D(Func, 0, 1);
  EXPECT_EQ(Cell1Tier2.status, EJitCompileOrGetStatus::CacheHit);
  if (Cell1Tier2.hasReadToken)
    Owner.releaseRead(Cell1Tier2.bucketIndex);
}

void produceAtPublishBarrier(void *Opaque) {
  auto &Race = *static_cast<PublishBarrierRaceCtx *>(Opaque);
  Race.status = Race.pool->compileOrGet(299, nullptr, 0, codeFor(299)).status;
}

TEST_F(FixedNearTaskPoolTest, PublishBarrierClosesProducerQueueGap) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  ASSERT_EQ(Owner.compileOrGet(298, nullptr, 0, codeFor(298)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());

  PublishBarrierRaceCtx Race{&Owner};
  Owner.setAutoPublishBarrierTestHook(&produceAtPublishBarrier, &Race);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  Owner.setAutoPublishBarrierTestHook(nullptr, nullptr);
  EXPECT_EQ(Race.status, EJitCompileOrGetStatus::QueueFullFallback);
  EXPECT_EQ(Ctx.flushCalls, 1u);
}

TEST_F(FixedNearTaskPoolTest,
       TwoTier2RequestsAutoFlushThroughRealCodePoolStateMachine) {
  RealFixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&realFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&realFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&realFixedNearReady,
                              &mockFixedNearLegacyFlush, &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&realFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(211, nullptr, 0, codeFor(211)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.compileOrGet(212, nullptr, 0, codeFor(212)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(2), 2u);
  for (uint32_t Func : {211u, 212u}) {
    auto Hit = Owner.tryCacheHit0D(Func);
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    if (Hit.hasReadToken)
      Owner.releaseRead(Hit.bucketIndex);
  }
  ASSERT_EQ(Owner.pollBudget(2), 2u);
  ASSERT_EQ(Owner.pendingPublishCount(), 2u);
  ASSERT_EQ(Ctx.flushCalls, 0u);

  EJitCompiledCodeInfo Pending{};
  ASSERT_GE(Ctx.entries.size(), 4u);
  void *Tier2A = Ctx.entries[2].fn;
  void *Tier2B = Ctx.entries[3].fn;
  EXPECT_TRUE(Ctx.pool->findPendingRange(Tier2A, Pending));
  EXPECT_TRUE(Ctx.pool->findPendingRange(Tier2B, Pending));
  EXPECT_FALSE(Ctx.pool->isRangeReady(Tier2A));
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Ctx.flushCalls, 0u);

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_EQ(Ctx.flushCalls, 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_TRUE(Ctx.pool->isRangeReady(Tier2A));
  EXPECT_TRUE(Ctx.pool->isRangeReady(Tier2B));

  const std::vector<char> Expected = {'S', 'W', 'W', 'F', 'X'};
  EXPECT_EQ(Ctx.timeline, Expected);
  for (uint32_t Func : {211u, 212u}) {
    auto Hit = Owner.tryCacheHit0D(Func);
    EXPECT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    if (Hit.hasReadToken)
      Owner.releaseRead(Hit.bucketIndex);
  }
}

TEST_F(FixedNearTaskPoolTest, ShutdownAbortsLinkedTier2DriverLifecycle) {
  FixedNearPublishCtx Ctx;
  AbortRecorder Abort;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setAbortPgoProfileCallback(&recordAbort, &Abort);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(220, nullptr, 0, codeFor(220)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Hit = Owner.tryCacheHit0D(220);
  ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  if (Hit.hasReadToken)
    Owner.releaseRead(Hit.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  ASSERT_EQ(linkedPendingCount(), 1u);
  EJitSharedCacheSlot *Tier1Slot = findReadySlot(220);
  ASSERT_NE(Tier1Slot, nullptr);
  const uint64_t Token = Tier1Slot->pgoToken.loadAcquire();
  ASSERT_NE(Token, 0u);
  EXPECT_EQ(State->inFlight[220].loadAcquire(), 0u);

  Owner.ownerShutdown();
  EXPECT_EQ(Abort.calls, 1u);
  EXPECT_EQ(Abort.token, Token);
  EXPECT_EQ(State->inFlight[220].loadAcquire(), 0u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(linkedPendingCount(), 0u);
}

TEST_F(FixedNearTaskPoolTest, FailureDoesNotBlockOtherPools) {
  FixedNearPublishCtx Ctx;
  Ctx.failCell1 = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  Owner.setInstanceEnabled(0, 0, true);
  Owner.setInstanceEnabled(0, 1, true);

  const EJitDimPair Cell0[1] = {dim(0, 0)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  EXPECT_EQ(Owner.compileOrGet(301, Cell0, 1, codeFor(301)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(302, Cell1, 1, codeFor(302)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  EXPECT_EQ(Owner.compileOrGet(303, nullptr, 0, codeFor(303)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(3), 3u);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);

  ASSERT_EQ(Ctx.entries.size(), 3u);
  EXPECT_TRUE(Ctx.entries[0].ready);
  EXPECT_FALSE(Ctx.entries[1].ready);
  EXPECT_TRUE(Ctx.entries[2].ready);
  EXPECT_NE(findReadySlot(301), nullptr);
  EXPECT_EQ(findReadySlot(302), nullptr);
  EXPECT_NE(findReadySlot(303), nullptr);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
}

TEST_F(FixedNearTaskPoolTest,
       Tier2PermanentPoolFailureIsBoundedAndDoesNotBlockNewHealthyPool) {
  FixedNearPublishCtx Ctx;
  Ctx.failCell1 = true;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1, 2);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);
  for (uint32_t Cell = 0; Cell != 3; ++Cell)
    Owner.setInstanceEnabled(0, Cell, true);

  const EJitDimPair Cell0[1] = {dim(0, 0)};
  const EJitDimPair Cell1[1] = {dim(0, 1)};
  const EJitDimPair Cell2[1] = {dim(0, 2)};
  ASSERT_EQ(Owner.compileOrGet(321, Cell0, 1, codeFor(321)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.compileOrGet(322, Cell1, 1, codeFor(322)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(2), 2u);
  for (auto Pair : {std::make_pair(321u, Cell0),
                    std::make_pair(322u, Cell1)}) {
    auto Hit = Owner.compileOrGet(Pair.first, Pair.second, 1,
                                  codeFor(Pair.first));
    ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
    if (Hit.hasReadToken)
      Owner.releaseRead(Hit.bucketIndex);
  }
  ASSERT_EQ(Owner.pollBudget(2), 2u);
  ASSERT_EQ(Owner.pendingPublishCount(), 2u);
  EXPECT_NE(Owner.compileOrGet(322, Cell1, 1, codeFor(322)).status,
            EJitCompileOrGetStatus::CacheHit)
      << "linked Tier-2 must remain AOT-only before pool commit";

  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_NE(findReadySlot(321), nullptr);
  ASSERT_NE(findReadySlot(322), nullptr);
  EXPECT_EQ(findReadySlot(322)->tier.loadRelaxed(),
            static_cast<uint8_t>(kEJitTierInstrumented));
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);

  ASSERT_EQ(Owner.compileOrGet(323, Cell2, 1, codeFor(323)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Hit = Owner.compileOrGet(323, Cell2, 1, codeFor(323));
  ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  if (Hit.hasReadToken)
    Owner.releaseRead(Hit.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Idle);
  EXPECT_EQ(Owner.workerPollOnce(), EJitWorkerStep::Consumed);
  EXPECT_NE(findReadySlot(323), nullptr)
      << "a delayed failed pool must not hold a newly linked healthy pool";

  for (unsigned I = 0; I != 80 && Owner.pendingPublishCount() != 0; ++I)
    (void)Owner.workerPollOnce();
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(findReadySlot(322), nullptr);
  EXPECT_EQ(Owner.compileOrGet(322, Cell1, 1, codeFor(322)).status,
            EJitCompileOrGetStatus::PgoAdmissionDeferred)
      << "permanent failure reaches terminal AOT suppression";
}

TEST_F(FixedNearTaskPoolTest, MissingPoolFlushFallsBackCleanly) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(304, nullptr, 0, codeFor(304)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(1), 1u);
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(findReadySlot(304), nullptr);
}

TEST_F(FixedNearTaskPoolTest, MissingPoolFlushContextFallsBackWithoutCall) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  // The production callback dereferences its context to reach the owner ORC
  // engine. A missing context must be a clean pending/AOT fallback, not an
  // indirect call through x0 == 0 on the worker.
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, nullptr);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(305, nullptr, 0, codeFor(305)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(1), 1u);
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(findReadySlot(305), nullptr);
}

TEST_F(FixedNearTaskPoolTest, CorruptPoolFlushFunctionFallsBackWithoutCall) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(306, nullptr, 0, codeFor(306)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(1), 1u);
  Owner.corruptCodeBatchPoolCallbackForTest(&mockWrongFixedNearFlushPool,
                                            &Ctx);

  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(findReadySlot(306), nullptr);
}

TEST_F(FixedNearTaskPoolTest, CorruptPoolFlushContextFallsBackWithoutCall) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(307, nullptr, 0, codeFor(307)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(1), 1u);
  Owner.corruptCodeBatchPoolCallbackForTest(
      &mockFixedNearFlushPool, reinterpret_cast<void *>(uintptr_t{1}));

  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(findReadySlot(307), nullptr);
}

TEST_F(FixedNearTaskPoolTest, CorruptPoolFlushLayoutFallsBackWithoutCall) {
  FixedNearPublishCtx Ctx;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setMode(EJitCompileMode::Async);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  EXPECT_EQ(Owner.compileOrGet(308, nullptr, 0, codeFor(308)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_EQ(Owner.pollBudget(1), 1u);
  Owner.corruptCodeBatchPoolLayoutTagForTest(0xFFFFFFFFFFFFFFFFULL);

  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_EQ(Ctx.flushCalls, 0u);
  EXPECT_EQ(findReadySlot(308), nullptr);
}

TEST_F(FixedNearTaskPoolTest,
       ThirdInvalidCallbackFailureReturnsFalseAndAbortsTier2) {
  FixedNearPublishCtx Ctx;
  AbortRecorder Abort;
  EJitSharedTaskPool Owner;
  EJitCoreId::setCurrentForTest(0);
  Owner.bind(State.get());
  Owner.setCompiler(&mockFixedNearCompile, &Ctx);
  Owner.setCodeRangeProvider(&mockFixedNearRange, &Ctx);
  Owner.setCodeBatchCallbacks(&mockFixedNearReady, &mockFixedNearLegacyFlush,
                              &Ctx);
  Owner.setCodeBatchPoolFlushCallback(&mockFixedNearFlushPool, &Ctx);
  Owner.setAbortPgoProfileCallback(&recordAbort, &Abort);
  Owner.setMode(EJitCompileMode::Async);
  Owner.setPgoEnabled(true, 1);
  ASSERT_EQ(Owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  ASSERT_EQ(Owner.compileOrGet(309, nullptr, 0, codeFor(309)).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(Owner.pollOne());
  auto Hit = Owner.tryCacheHit0D(309);
  ASSERT_EQ(Hit.status, EJitCompileOrGetStatus::CacheHit);
  if (Hit.hasReadToken)
    Owner.releaseRead(Hit.bucketIndex);
  ASSERT_TRUE(Owner.pollOne());
  ASSERT_EQ(Owner.pendingPublishCount(), 1u);
  EJitSharedCacheSlot *Tier1Slot = findReadySlot(309);
  ASSERT_NE(Tier1Slot, nullptr);
  const uint64_t Token = Tier1Slot->pgoToken.loadAcquire();
  ASSERT_NE(Token, 0u);
  EXPECT_EQ(State->inFlight[309].loadAcquire(), 0u);

  Owner.corruptCodeBatchPoolLayoutTagForTest(0xFFFFFFFFFFFFFFFFULL);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 1u);
  EXPECT_FALSE(Owner.flushCodeBatch());
  EXPECT_EQ(Owner.pendingPublishCount(), 0u);
  EXPECT_EQ(Abort.calls, 1u);
  EXPECT_EQ(Abort.token, Token);
  EXPECT_EQ(State->inFlight[309].loadAcquire(), 0u);
  EXPECT_EQ(Owner.compileOrGet(309, nullptr, 0, codeFor(309)).status,
            EJitCompileOrGetStatus::PgoAdmissionDeferred);
}

} // namespace
