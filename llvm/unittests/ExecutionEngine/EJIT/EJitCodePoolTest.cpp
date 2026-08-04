//===-- EJitCodePoolTest.cpp - Unit tests for the SRE code pool -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Host-runnable tests for EJitCodePoolManager. These never touch real SRE:
//  the raw allocator and the seal (enable_ex) primitive are injected mocks, so
//  the tests pass on any host regardless of whether EJIT_SRE_CODE_POOL is set.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/Support/Error.h"
#include "gtest/gtest.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// Mock SRE backend: tracks raw allocations (freed at teardown), records seal
/// calls and the bases that were sealed, and can be configured to fail.
struct MockSre {
  std::vector<void *> Raws;
  size_t AllocCalls = 0;
  bool FailNextAlloc = false;

  std::vector<void *> SealedBases;
  size_t SealCalls = 0;
  unsigned SealRc = 0; // return code handed back by the mock enable_ex

  ~MockSre() {
    for (void *P : Raws)
      std::free(P);
  }

  void *rawAlloc(size_t Bytes) {
    ++AllocCalls;
    if (FailNextAlloc) {
      FailNextAlloc = false;
      return nullptr;
    }
    void *P = std::malloc(Bytes);
    if (P)
      Raws.push_back(P);
    return P;
  }

  unsigned seal(void *Base) {
    ++SealCalls;
    if (SealRc == 0)
      SealedBases.push_back(Base);
    return SealRc;
  }
};

EJitCodePoolManager makeManager(MockSre &M,
                                EJitCodePoolManager::Options Opts) {
  return EJitCodePoolManager(
      Opts, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
}

EJitCodePoolManager::Options smallOpts(size_t PoolSize = 256) {
  EJitCodePoolManager::Options O;
  O.poolSize = PoolSize;
  O.poolAlign = PoolSize; // keep raw allocations tiny for logic tests
  O.minCodeAlign = 64;
  return O;
}

uintptr_t A(const void *P) { return reinterpret_cast<uintptr_t>(P); }

} // namespace

// 1. The usable base of a pool is 2MiB aligned.
TEST(EJitCodePool, BaseIs2MiBAligned) {
  constexpr size_t k2M = static_cast<size_t>(2) * 1024 * 1024;
  EJitCodePoolManager::Options O;
  O.poolSize = k2M;
  O.poolAlign = k2M;
  O.minCodeAlign = 64;
  MockSre M;
  auto Mgr = makeManager(M, O);

  void *P = cantFail(Mgr.allocateCode(128, 16));
  // First allocation sits at offset 0, so it equals the pool base.
  EXPECT_EQ(A(P) % k2M, 0u);
  EXPECT_TRUE(Mgr.contains(P));
}

// 2. Multiple small allocations bump contiguously inside one RW pool.
TEST(EJitCodePool, BumpAllocatesWithinOneRWPool) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts(/*PoolSize=*/4096));

  void *a = cantFail(Mgr.allocateCode(64, 64));
  void *b = cantFail(Mgr.allocateCode(64, 64));
  void *c = cantFail(Mgr.allocateCode(64, 64));

  EXPECT_LT(A(a), A(b));
  EXPECT_LT(A(b), A(c));
  EXPECT_EQ(A(b) - A(a), 64u); // 64-aligned, 64-byte blocks → exactly adjacent
  EXPECT_EQ(A(c) - A(b), 64u);

  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 1u);
  EXPECT_EQ(S.sealedCount, 0u);
  EXPECT_EQ(S.activeCount, 1u);
  EXPECT_EQ(M.SealCalls, 0u);
}

// 3. Sealing a pool marks it executable and invokes enable_ex exactly once.
TEST(EJitCodePool, SealMarksPoolExecutable) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts());

  void *a = cantFail(Mgr.allocateCode(64, 64));
  cantFail(Mgr.sealPoolContaining(a));

  auto S = Mgr.getStats();
  EXPECT_EQ(S.sealedCount, 1u);
  EXPECT_EQ(S.activeCount, 0u);
  EXPECT_EQ(S.sealInvocations, 1u);
  EXPECT_EQ(M.SealCalls, 1u);
  ASSERT_EQ(M.SealedBases.size(), 1u);
}

// 4. A sealed pool is never reused; the next allocation creates a new pool.
TEST(EJitCodePool, SealedPoolNotReusedNewPoolCreated) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts());

  void *a = cantFail(Mgr.allocateCode(64, 64));
  cantFail(Mgr.sealPoolContaining(a));
  void *b = cantFail(Mgr.allocateCode(64, 64));

  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 2u);
  EXPECT_EQ(S.sealedCount, 1u);
  EXPECT_EQ(S.activeCount, 1u);
  // b must not fall inside the first (sealed) pool's range.
  EXPECT_GE(A(b) > A(a) ? A(b) - A(a) : A(a) - A(b), 64u);
}

// 5. Repeated seal of the same pool does not re-invoke enable_ex (idempotent).
TEST(EJitCodePool, RepeatedSealNoDuplicateEnableEx) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts());

  void *a = cantFail(Mgr.allocateCode(64, 64));
  void *b = cantFail(Mgr.allocateCode(64, 64)); // same pool as a

  cantFail(Mgr.sealPoolContaining(a));
  EXPECT_EQ(M.SealCalls, 1u);
  cantFail(Mgr.sealPoolContaining(b)); // already sealed → no-op success
  EXPECT_EQ(M.SealCalls, 1u);
  EXPECT_EQ(Mgr.getStats().sealInvocations, 1u);
}

// 6. enable_ex failure surfaces as an Error and the pool stays writable.
TEST(EJitCodePool, EnableExFailureReturnsError) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts());

  void *a = cantFail(Mgr.allocateCode(64, 64));
  M.SealRc = 7; // make enable_ex fail

  Error Err = Mgr.sealPoolContaining(a);
  EXPECT_TRUE(static_cast<bool>(Err));
  consumeError(std::move(Err));

  auto S = Mgr.getStats();
  EXPECT_EQ(S.sealedCount, 0u); // still RW, not sealed
  EXPECT_EQ(S.sealInvocations, 0u);
}

// 6b. A seal failure during full-pool rollover propagates out of allocateCode.
TEST(EJitCodePool, RolloverSealFailurePropagates) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts(/*PoolSize=*/256));

  // Fill the 256-byte pool with four 64-byte blocks (offsets 0/64/128/192).
  for (int i = 0; i < 4; ++i)
    (void)cantFail(Mgr.allocateCode(64, 64));

  M.SealRc = 9; // the rollover seal of the full pool will fail
  auto E = Mgr.allocateCode(64, 64);
  EXPECT_FALSE(static_cast<bool>(E));
  consumeError(E.takeError());
}

// 7. A request larger than the pool size is rejected cleanly (no allocation).
TEST(EJitCodePool, OversizeRequestCleanReject) {
  MockSre M;
  auto Opts = smallOpts(/*PoolSize=*/4096);
  auto Mgr = makeManager(M, Opts);

  auto E = Mgr.allocateCode(Opts.poolSize + 1, 16);
  EXPECT_FALSE(static_cast<bool>(E));
  consumeError(E.takeError());

  EXPECT_EQ(Mgr.getStats().poolCount, 0u);
  EXPECT_EQ(M.AllocCalls, 0u); // never even tried to allocate a pool
}

// 8. Statistics (pool count, sealed count, used / wasted bytes) are correct.
TEST(EJitCodePool, StatsAreAccurate) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts(/*PoolSize=*/4096));

  (void)cantFail(Mgr.allocateCode(100, 64)); // off 0,   used 100
  void *b = cantFail(Mgr.allocateCode(200, 64)); // off 128, used 328

  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 1u);
  EXPECT_EQ(S.reservedBytes, 4096u);
  EXPECT_EQ(S.usedBytes, 328u);
  EXPECT_EQ(S.sealedCount, 0u);
  EXPECT_EQ(S.wastedBytes, 0u); // active pool tail is not counted as wasted

  cantFail(Mgr.sealPoolContaining(b));
  S = Mgr.getStats();
  EXPECT_EQ(S.sealedCount, 1u);
  EXPECT_EQ(S.wastedBytes, 4096u - 328u); // sealed tail is wasted
}

// Strategy case 3: a full active pool is sealed automatically before a new
// pool is created on the next allocation.
TEST(EJitCodePool, FullPoolSealedOnRollover) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts(/*PoolSize=*/256));

  for (int i = 0; i < 4; ++i)
    (void)cantFail(Mgr.allocateCode(64, 64)); // fills the pool exactly

  EXPECT_EQ(M.SealCalls, 0u);
  void *e = cantFail(Mgr.allocateCode(64, 64)); // triggers seal + new pool
  EXPECT_TRUE(Mgr.contains(e));

  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 2u);
  EXPECT_EQ(S.sealedCount, 1u);
  EXPECT_EQ(M.SealCalls, 1u);
}

// Larger alignment requests are honored.
TEST(EJitCodePool, RespectsLargerAlignment) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts(/*PoolSize=*/4096));

  (void)cantFail(Mgr.allocateCode(8, 64));   // off 0
  void *b = cantFail(Mgr.allocateCode(8, 256)); // must be 256-aligned
  EXPECT_EQ(A(b) % 256u, 0u);
}

// sealAllWritablePools seals every still-writable pool.
TEST(EJitCodePool, SealAllWritablePools) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts());

  void *a = cantFail(Mgr.allocateCode(64, 64));
  cantFail(Mgr.sealPoolContaining(a)); // pool 1 sealed
  (void)cantFail(Mgr.allocateCode(64, 64)); // pool 2 active

  cantFail(Mgr.sealAllWritablePools());
  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 2u);
  EXPECT_EQ(S.sealedCount, 2u);
  EXPECT_EQ(S.activeCount, 0u);
  EXPECT_EQ(M.SealCalls, 2u); // pool 1 not re-sealed
}

// An address not owned by any pool cannot be sealed.
TEST(EJitCodePool, SealUnknownAddressFails) {
  MockSre M;
  auto Mgr = makeManager(M, smallOpts());
  int OnStack = 0;
  Error Err = Mgr.sealPoolContaining(&OnStack);
  EXPECT_TRUE(static_cast<bool>(Err));
  consumeError(std::move(Err));
}

//===----------------------------------------------------------------------===//
// 4K page-seal mode tests
//
// These exercise the SRE-platform 4K execute-permission interface: the pool is
// still 2MiB-aligned (split into 4K mappings via split_2m_to_4k at creation),
// but only the 4KiB pages a finalized allocation covers are sealed (enable_ex
// per page). All injected mocks; no real platform symbols.
//===----------------------------------------------------------------------===//
namespace {

constexpr size_t kTwoMiB = static_cast<size_t>(2) * 1024 * 1024;
constexpr size_t kFourKiB = static_cast<size_t>(4) * 1024;

/// Mock SRE backend for 4K mode: deliberately returns a non-2MiB-aligned raw
/// base, records split_2m_to_4k(base,size) calls and per-page enable_ex calls,
/// and can be made to fail split or a chosen seal call.
struct MockSre4K {
  std::vector<void *> Origs; // posix_memalign bases (freed at teardown)
  uintptr_t LastRawReturned = 0;
  size_t LastBytesRequested = 0;
  size_t AllocCalls = 0;

  std::vector<std::pair<uintptr_t, size_t>> Splits;
  unsigned SplitRc = 0;

  std::vector<uintptr_t> SealedPages;
  size_t SealCalls = 0;
  int FailSealOnCall = -1; // 1-based seal call index to fail; -1 = never
  unsigned SealFailRc = 7;

  std::vector<uintptr_t> RwEnabledPages;
  size_t RwEnableCalls = 0;
  unsigned RwEnableRc = 0; // 0 = success; non-zero simulates enable_rw failure

  ~MockSre4K() {
    for (void *P : Origs)
      std::free(P);
  }

  void *rawAlloc(size_t Bytes) {
    ++AllocCalls;
    LastBytesRequested = Bytes;
    void *Base = nullptr;
    // Over-allocate, 2MiB-aligned, then hand back a deliberately misaligned
    // pointer (offset 4KiB) so the manager must round the base up to 2MiB.
    if (posix_memalign(&Base, kTwoMiB, Bytes + kTwoMiB) != 0)
      return nullptr;
    Origs.push_back(Base);
    void *Raw = static_cast<char *>(Base) + kFourKiB;
    LastRawReturned = reinterpret_cast<uintptr_t>(Raw);
    return Raw;
  }

  unsigned split(void *Base, size_t Size) {
    Splits.push_back({reinterpret_cast<uintptr_t>(Base), Size});
    return SplitRc;
  }

  unsigned seal(void *PageVA) {
    ++SealCalls;
    if (FailSealOnCall > 0 && static_cast<int>(SealCalls) == FailSealOnCall)
      return SealFailRc;
    SealedPages.push_back(reinterpret_cast<uintptr_t>(PageVA));
    return 0;
  }

  unsigned enableRw(void *PageVA) {
    ++RwEnableCalls;
    if (RwEnableRc != 0)
      return RwEnableRc;
    RwEnabledPages.push_back(reinterpret_cast<uintptr_t>(PageVA));
    return 0;
  }
};

EJitCodePoolManager::Options fourKOpts() {
  EJitCodePoolManager::Options O;
  O.poolSize = kTwoMiB;
  O.poolAlign = kTwoMiB;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kFourKiB;
  return O;
}

EJitCodePoolManager makeManager4K(MockSre4K &M,
                                  EJitCodePoolManager::Options Opts) {
  return EJitCodePoolManager(
      Opts, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); },
      [&M](void *V) { return M.enableRw(V); });
}

} // namespace

// A deliberately non-2MiB-aligned raw base is rounded up to a 2MiB-aligned pool
// base, and the usable window stays inside the raw allocation.
TEST(EJitCodePool4K, AlignsMisalignedRawBaseTo2MiB) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(128, 64));
  // First allocation in 4K mode starts at offset 0, i.e. the pool base.
  EXPECT_EQ(A(P) % kTwoMiB, 0u);          // aligned base is 2MiB aligned
  EXPECT_TRUE(Mgr.contains(P));
  EXPECT_NE(A(P), M.LastRawReturned);     // raw base was misaligned, base != raw
  // Usable window [base, base+poolSize) must fit within [raw, raw+requested).
  EXPECT_LE(A(P) + kTwoMiB, M.LastRawReturned + M.LastBytesRequested);
  // The manager requests poolSize + 2MiB of alignment slack.
  EXPECT_EQ(M.LastBytesRequested, kTwoMiB + kTwoMiB);
}

// split_2m_to_4k is called exactly once per pool, with the aligned base and the
// usable pool size; a second allocation in the same pool does not re-split.
TEST(EJitCodePool4K, SplitCalledOncePerPool) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(128, 64));
  ASSERT_EQ(M.Splits.size(), 1u);
  EXPECT_EQ(M.Splits[0].first, A(P));      // split base == aligned pool base
  EXPECT_EQ(M.Splits[0].second, kTwoMiB);  // split size == usable pool size
  EXPECT_EQ(M.Splits[0].first % kTwoMiB, 0u);
  EXPECT_EQ(Mgr.getStats().splitInvocations, 1u);

  (void)cantFail(Mgr.allocateCode(128, 64)); // same pool, no new split
  EXPECT_EQ(M.Splits.size(), 1u);
  EXPECT_EQ(Mgr.getStats().splitInvocations, 1u);
}

// A small function seals only the single 4K page it covers, not the whole pool.
TEST(EJitCodePool4K, SmallCodeSealsOnlyCoveredPage) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(100, 64));
  cantFail(Mgr.sealCodeRange(P, 100));

  EXPECT_EQ(M.SealCalls, 1u); // one page, NOT 512 (the whole 2MiB pool)
  ASSERT_EQ(M.SealedPages.size(), 1u);
  EXPECT_EQ(M.SealedPages[0], A(P)); // page base == 4K-aligned code start
  EXPECT_EQ(Mgr.getStats().sealInvocations, 1u);
}

// A code range spanning multiple 4K pages loops enable_ex over each page.
TEST(EJitCodePool4K, MultiPageCodeSealsEachPage) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  size_t Sz = 2 * kFourKiB + 200; // covers 3 pages
  void *P = cantFail(Mgr.allocateCode(Sz, 64));
  cantFail(Mgr.sealCodeRange(P, Sz));

  EXPECT_EQ(M.SealCalls, 3u);
  ASSERT_EQ(M.SealedPages.size(), 3u);
  EXPECT_EQ(M.SealedPages[0], A(P));
  EXPECT_EQ(M.SealedPages[1], A(P) + kFourKiB);
  EXPECT_EQ(M.SealedPages[2], A(P) + 2 * kFourKiB);
}

// If any page's enable_ex fails, sealCodeRange returns an Error.
TEST(EJitCodePool4K, EnableExFailureOnAPageReturnsError) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  size_t Sz = 2 * kFourKiB + 200; // 3 pages
  void *P = cantFail(Mgr.allocateCode(Sz, 64));
  M.FailSealOnCall = 2; // fail the 2nd page

  Error Err = Mgr.sealCodeRange(P, Sz);
  EXPECT_TRUE(static_cast<bool>(Err));
  consumeError(std::move(Err));
}

// A subsequent allocation lands on a fresh 4K page, never inside a sealed page,
// and stays in the same (still partially-writable) pool.
TEST(EJitCodePool4K, NextAllocationSkipsSealedPage) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *A1 = cantFail(Mgr.allocateCode(100, 64)); // page 0
  cantFail(Mgr.sealCodeRange(A1, 100));           // seal page 0
  void *A2 = cantFail(Mgr.allocateCode(100, 64)); // must be a later page

  EXPECT_TRUE(Mgr.contains(A2));
  EXPECT_GE(A(A2), A(A1) + kFourKiB);
  EXPECT_FALSE(A(A2) >= A(A1) && A(A2) < A(A1) + kFourKiB); // not in sealed page
  EXPECT_EQ(A(A2) % kFourKiB, 0u);                          // fresh page start
  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 1u);          // same pool reused
  EXPECT_EQ(S.splitInvocations, 1u);
}

// split_2m_to_4k failure makes pool creation (hence allocateCode) fail cleanly,
// registering no pool.
TEST(EJitCodePool4K, SplitFailureFailsPoolCreation) {
  MockSre4K M;
  M.SplitRc = 5; // split_2m_to_4k fails
  auto Mgr = makeManager4K(M, fourKOpts());

  auto E = Mgr.allocateCode(128, 64);
  EXPECT_FALSE(static_cast<bool>(E));
  consumeError(E.takeError());
  EXPECT_EQ(Mgr.getStats().poolCount, 0u);
  EXPECT_EQ(Mgr.getStats().splitInvocations, 0u);
}

// Rolling over to a new pool splits the new pool exactly once, and no whole-pool
// seal happens (sealing is per-page at finalize, not on rollover).
TEST(EJitCodePool4K, RolloverCreatesNewPoolAndSplitsAgain) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  (void)cantFail(Mgr.allocateCode(kTwoMiB, 64)); // fills pool 1 exactly
  void *P2 = cantFail(Mgr.allocateCode(64, 64)); // forces pool 2

  EXPECT_TRUE(Mgr.contains(P2));
  auto S = Mgr.getStats();
  EXPECT_EQ(S.poolCount, 2u);
  EXPECT_EQ(S.splitInvocations, 2u); // one split per pool
  EXPECT_EQ(M.SealCalls, 0u);        // no whole-pool seal on rollover
  EXPECT_EQ(S.sealedCount, 0u);
}

// In 4K mode the whole-pool seal entry point sealPoolContaining is unsupported
// (a bare pointer has no size); it must return an Error and never enable_ex.
TEST(EJitCodePool4K, SealPoolContainingReturnsErrorIn4KMode) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(100, 64));
  Error Err = Mgr.sealPoolContaining(P);
  EXPECT_TRUE(static_cast<bool>(Err));
  consumeError(std::move(Err));

  // No enable_ex was invoked and the pool was not marked sealed.
  EXPECT_EQ(M.SealCalls, 0u);
  EXPECT_EQ(Mgr.getStats().sealInvocations, 0u);
  EXPECT_EQ(Mgr.getStats().sealedCount, 0u);
}

// In 4K mode sealAllWritablePools (whole-pool sealing) is unsupported and must
// return an Error rather than silently sealing or no-op succeeding.
TEST(EJitCodePool4K, SealAllWritablePoolsReturnsErrorIn4KMode) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  (void)cantFail(Mgr.allocateCode(100, 64));
  Error Err = Mgr.sealAllWritablePools();
  EXPECT_TRUE(static_cast<bool>(Err));
  consumeError(std::move(Err));

  EXPECT_EQ(M.SealCalls, 0u);
  EXPECT_EQ(Mgr.getStats().sealedCount, 0u);
}

//===----------------------------------------------------------------------===//
// findRange: recover the real executable extent + owning pool for a pointer.
// This is the cross-core 4K-seal range source — it must come from recorded
// finalized allocations, never a guess.
//===----------------------------------------------------------------------===//

// A pointer anywhere inside a recorded finalized allocation resolves to its
// real [codeStart, codeSize) and the owning pool's base/size/id.
TEST(EJitCodePoolRange, FindRangeResolvesRecordedAllocation) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(200, 64));
  Mgr.recordFinalizedRange(P, 200);

  EJitCompiledCodeInfo Info{};
  void *Mid = reinterpret_cast<void *>(A(P) + 64); // interior pointer
  ASSERT_TRUE(Mgr.findRange(Mid, Info));
  EXPECT_EQ(Info.codeStart, A(P));
  EXPECT_EQ(Info.codeSize, 200u);
  EXPECT_EQ(Info.poolBase % kTwoMiB, 0u);
  EXPECT_EQ(Info.poolSize, kTwoMiB);
  EXPECT_GE(A(P), Info.poolBase);
  EXPECT_LE(A(P) + 200, Info.poolBase + Info.poolSize);
  EXPECT_EQ(Info.fnPtr, Mid);
}

// A pointer with NO recorded finalized range is a clean miss (no guessed extent
// is fabricated from the pool alone).
TEST(EJitCodePoolRange, FindRangeMissForUnrecordedPointer) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(200, 64)); // allocated but NOT recorded
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Mgr.findRange(P, Info));
}

// A non-pool address (e.g. a stack pointer) is never mishandled.
TEST(EJitCodePoolRange, FindRangeRejectsNonPoolAddress) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(200, 64));
  Mgr.recordFinalizedRange(P, 200);

  int OnStack = 0;
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Mgr.findRange(&OnStack, Info));
}

// A zero-size finalized record is ignored (the pointer then misses).
TEST(EJitCodePoolRange, RecordZeroSizeRangeIgnored) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(200, 64));
  Mgr.recordFinalizedRange(P, 0); // ignored
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Mgr.findRange(P, Info));
}

// Distinct pools yield distinct poolBase and poolId.
TEST(EJitCodePoolRange, FindRangeReportsDistinctPoolIds) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P0 = cantFail(Mgr.allocateCode(kTwoMiB, 64)); // fills pool 0
  Mgr.recordFinalizedRange(P0, 100);
  void *P1 = cantFail(Mgr.allocateCode(100, 64)); // forces a new pool
  Mgr.recordFinalizedRange(P1, 100);

  EJitCompiledCodeInfo I0{}, I1{};
  ASSERT_TRUE(Mgr.findRange(P0, I0));
  ASSERT_TRUE(Mgr.findRange(P1, I1));
  EXPECT_NE(I0.poolBase, I1.poolBase);
  EXPECT_NE(I0.poolId, I1.poolId);
}

// Recording the IDENTICAL [start, size) extent more than once is idempotent:
// the range table does not grow and findRange still resolves the pointer. This
// guards against a retried finalize creating duplicate, equally-valid matches.
TEST(EJitCodePoolRange, RecordDuplicateRangeIsIdempotent) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(200, 64));
  Mgr.recordFinalizedRange(P, 200);
  Mgr.recordFinalizedRange(P, 200); // exact duplicate
  Mgr.recordFinalizedRange(P, 200); // and again

  EXPECT_EQ(Mgr.getStats().finalizedRangeCount, 1u);

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Mgr.findRange(P, Info));
  EXPECT_EQ(Info.codeStart, A(P));
  EXPECT_EQ(Info.codeSize, 200u);
}

// Two distinct, non-overlapping executable extents in the SAME pool are both
// recorded and each pointer resolves to its own range.
TEST(EJitCodePoolRange, TwoDistinctRangesResolveIndependently) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P0 = cantFail(Mgr.allocateCode(128, 64));
  void *P1 = cantFail(Mgr.allocateCode(256, 64));
  Mgr.recordFinalizedRange(P0, 128);
  Mgr.recordFinalizedRange(P1, 256);

  EXPECT_EQ(Mgr.getStats().finalizedRangeCount, 2u);

  EJitCompiledCodeInfo I0{}, I1{};
  ASSERT_TRUE(Mgr.findRange(P0, I0));
  ASSERT_TRUE(Mgr.findRange(P1, I1));
  EXPECT_EQ(I0.codeStart, A(P0));
  EXPECT_EQ(I0.codeSize, 128u);
  EXPECT_EQ(I1.codeStart, A(P1));
  EXPECT_EQ(I1.codeSize, 256u);
  // Same pool, so identical pool identity.
  EXPECT_EQ(I0.poolBase, I1.poolBase);
  EXPECT_EQ(I0.poolId, I1.poolId);
}

// findRange is deterministic when ranges overlap (an invariant violation that
// could only arise from address reuse): the first recorded range that contains
// the pointer wins, in append order. No UB, no ambiguity.
TEST(EJitCodePoolRange, OverlappingRangesResolveDeterministically) {
  MockSre4K M;
  auto Mgr = makeManager4K(M, fourKOpts());

  void *P = cantFail(Mgr.allocateCode(400, 64));
  // Two overlapping extents covering the same interior pointer, recorded in a
  // fixed order. (In practice pool addresses are never reused while live, so
  // this cannot occur; the test pins the deterministic resolution anyway.)
  Mgr.recordFinalizedRange(P, 400);                                  // first
  Mgr.recordFinalizedRange(reinterpret_cast<void *>(A(P) + 64), 64); // second

  EXPECT_EQ(Mgr.getStats().finalizedRangeCount, 2u);

  EJitCompiledCodeInfo Info{};
  void *Mid = reinterpret_cast<void *>(A(P) + 100);
  ASSERT_TRUE(Mgr.findRange(Mid, Info));
  // First (append order) containing range wins.
  EXPECT_EQ(Info.codeStart, A(P));
  EXPECT_EQ(Info.codeSize, 400u);
}

// ---- Fixed-region mode (EJIT_FIXED_CODE_POOL) ----
// When Options::fixedSize > 0, pools are carved from the pre-reserved
// [fixedBase, fixedBase+fixedSize) region instead of calling RawAllocFn. These
// tests use tiny geometry (poolAlign=poolSize=256) so they run fast and never
// touch real 2MiB pages; the carve math is identical at any scale.

// Fixed-region mode carves poolAlign-aligned pools from the region WITHOUT ever
// calling the raw allocator (dynamic SRE_MemDbgAlloc is the fallback only when
// fixedSize == 0).
TEST(EJitCodePoolFixed, CarvesAlignedPoolsWithoutAlloc) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  constexpr size_t kRegion = 1024; // 4 pools
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;

  MockSre M;
  auto Mgr = makeManager(M, O);

  uintptr_t RegionBase = reinterpret_cast<uintptr_t>(Region);
  uintptr_t Prev = 0;
  for (int i = 0; i < 4; ++i) {
    void *P = cantFail(Mgr.allocateCode(kPool, 64));
    uintptr_t AP = A(P);
    EXPECT_GE(AP, RegionBase);
    EXPECT_LT(AP, RegionBase + kRegion);
    EXPECT_EQ(AP % kAlign, 0u);
    EXPECT_NE(AP, Prev);
    Prev = AP;
  }
  // The raw allocator is never consulted in fixed-region mode.
  EXPECT_EQ(M.AllocCalls, 0u);
}

// Exhausting the fixed region is a clean Error (no crash, no silent fallback
// to dynamic allocation), and pools already carved remain valid.
TEST(EJitCodePoolFixed, ExhaustsCleanly) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  constexpr size_t kRegion = 512; // 2 pools
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;

  MockSre M;
  auto Mgr = makeManager(M, O);

  void *P0 = cantFail(Mgr.allocateCode(kPool, 64));
  void *P1 = cantFail(Mgr.allocateCode(kPool, 64));
  EXPECT_NE(A(P0), A(P1));
  Expected<void *> E = Mgr.allocateCode(kPool, 64);
  ASSERT_FALSE(E) << "fixed region must exhaust cleanly, not fall back";
  consumeError(E.takeError());
}

// 4K page-seal mode still splits the fixed-region pool at creation and seals
// the covering pages at finalize - the fixed region is RW until sealed, exactly
// like a dynamically allocated pool.
TEST(EJitCodePoolFixed, StillSplitsAndSealsIn4KMode) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  constexpr size_t kPage = 256;
  constexpr size_t kRegion = 512; // 2 pools
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kPage;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;

  MockSre4K M;
  auto Mgr = makeManager4K(M, O);

  void *P = cantFail(Mgr.allocateCode(128, 64));
  // Pool creation in 4K mode still split the fixed-region pool.
  ASSERT_EQ(M.Splits.size(), 1u);
  EXPECT_EQ(M.Splits[0].first, reinterpret_cast<uintptr_t>(Region));
  EXPECT_EQ(M.Splits[0].second, kPool);
  // Raw allocator never consulted.
  EXPECT_EQ(M.AllocCalls, 0u);

  // Sealing the carved code range flips the covering page. P is page-aligned
  // (EffAlign >= sealPageSize in 4K mode), so exactly one page is sealed.
  cantFail(Mgr.sealCodeRange(P, 128));
  EXPECT_EQ(M.SealCalls, 1u);
  ASSERT_EQ(M.SealedPages.size(), 1u);
  EXPECT_EQ(M.SealedPages[0], A(P));
}

// A non-poolAlign-aligned fixedBase is aligned UP by the manager. The factory
// (makeSreCodePoolManager) does this alignment itself; this test pins that the
// manager stays robust to a misaligned base from any direct Options caller
// (defense in depth): the first pool lands at alignUp(fixedBase, poolAlign),
// inside the region, and the raw allocator is still never consulted.
TEST(EJitCodePoolFixed, AlignsMisalignedBase) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  // 256-aligned buffer; start the region at +100 so it is NOT 256-aligned, with
  // enough headroom that the align-up still leaves room for a pool.
  void *Buf = nullptr;
  ASSERT_EQ(posix_memalign(&Buf, kAlign, kAlign + 1024), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Buf, std::free);
  uintptr_t MisalignedBase = reinterpret_cast<uintptr_t>(Buf) + 100;

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fixedBase = MisalignedBase;
  O.fixedSize = 1024;

  MockSre M;
  auto Mgr = makeManager(M, O);

  uintptr_t ExpectedFirst =
      (MisalignedBase + (kAlign - 1)) & ~(static_cast<uintptr_t>(kAlign) - 1);
  void *P = cantFail(Mgr.allocateCode(kPool, 64));
  EXPECT_EQ(A(P), ExpectedFirst);
  EXPECT_GE(A(P), MisalignedBase);
  EXPECT_LT(A(P), MisalignedBase + 1024);
  EXPECT_EQ(A(P) % kAlign, 0u);
  EXPECT_EQ(M.AllocCalls, 0u);
}

// ---- Code-segment placement (enable_rw) ----

// enableRwRange makes the slab's 4K pages writable (RX -> RW) when
// needsEnableRw is set (code-segment placement). Page-aligned like
// sealCodeRange; one enable_rw per covered page.
TEST(EJitCodePoolFixed, EnableRwRangeFlipsCoveredPages) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 1024; // >= the 384-byte request
  constexpr size_t kPage = 256;
  constexpr size_t kRegion = 2048; // 2 pools
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kPage;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;
  O.needsEnableRw = true;

  MockSre4K M;
  auto Mgr = makeManager4K(M, O);

  // A 384-byte request spans 2 pages ([P, P+384) -> [P, P+512)).
  void *P = cantFail(Mgr.allocateCode(384, 64));
  cantFail(Mgr.enableRwRange(P, 384));
  EXPECT_EQ(M.RwEnableCalls, 2u);
  ASSERT_EQ(M.RwEnabledPages.size(), 2u);
  EXPECT_EQ(M.RwEnabledPages[0], A(P));
  EXPECT_EQ(M.RwEnabledPages[1], A(P) + kPage);
}

// enableRwRange is a clean no-op when needsEnableRw is false (data-region
// placement is already RW) - enable_rw is never called.
TEST(EJitCodePoolFixed, EnableRwRangeNoOpWhenNotNeeded) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  constexpr size_t kPage = 256;
  constexpr size_t kRegion = 512;
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kPage;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;
  O.needsEnableRw = false; // data-region placement: no enable_rw

  MockSre4K M;
  auto Mgr = makeManager4K(M, O);

  void *P = cantFail(Mgr.allocateCode(128, 64));
  cantFail(Mgr.enableRwRange(P, 128));
  EXPECT_EQ(M.RwEnableCalls, 0u);
  EXPECT_TRUE(M.RwEnabledPages.empty());
}

// enableRwRange propagates an enable_rw failure as an Error (the slab must not
// be written if a page cannot be made writable).
TEST(EJitCodePoolFixed, EnableRwRangeFailsOnRc) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  constexpr size_t kPage = 256;
  constexpr size_t kRegion = 512;
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kPage;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;
  O.needsEnableRw = true;

  MockSre4K M;
  M.RwEnableRc = 5; // simulate enable_rw failure
  auto Mgr = makeManager4K(M, O);

  void *P = cantFail(Mgr.allocateCode(128, 64));
  Error E = Mgr.enableRwRange(P, 128);
  EXPECT_TRUE(bool(E)) << "enable_rw failure must propagate as an Error";
  consumeError(std::move(E));
}

// A range whose first byte is owned but whose end crosses the pool boundary
// must be rejected before any platform permission callback runs.
TEST(EJitCodePoolFixed, PermissionRangeCannotCrossPoolBoundary) {
  constexpr size_t kAlign = 256;
  constexpr size_t kPool = 256;
  constexpr size_t kRegion = 512;
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kAlign, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  EJitCodePoolManager::Options O;
  O.poolSize = kPool;
  O.poolAlign = kAlign;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kAlign;
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;
  O.needsEnableRw = true;

  MockSre4K M;
  auto Mgr = makeManager4K(M, O);
  void *P = cantFail(Mgr.allocateCode(128, 64));

  Error RwErr = Mgr.enableRwRange(P, kPool + 1);
  EXPECT_TRUE(bool(RwErr));
  consumeError(std::move(RwErr));
  EXPECT_EQ(M.RwEnableCalls, 0u);

  Error SealErr = Mgr.sealCodeRange(P, kPool + 1);
  EXPECT_TRUE(bool(SealErr));
  consumeError(std::move(SealErr));
  EXPECT_EQ(M.SealCalls, 0u);
}
