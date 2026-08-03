//===-- EJitCodePoolMemoryManagerTest.cpp - mem mgr over code pool --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Host-runnable tests that drive EJitCodePoolMemoryManager with a synthetic
//  JITLink LinkGraph and a mock SRE backend. These exercise the exact
//  allocate -> finalize -> seal path the engine uses at lookup, without needing
//  a native backend to actually execute code (the host has no matching JIT
//  target, so end-to-end execution is validated on the target instead).
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

#include <cstdlib>
#include <memory>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

namespace {

struct MockSre {
  std::vector<void *> Raws;
  size_t SealCalls = 0;
  unsigned SealRc = 0;
  std::vector<uintptr_t> RwEnabledPages;
  size_t RwEnableCalls = 0;
  unsigned RwEnableRc = 0;
  int FailRwOnCall = -1; // 1-based enable_rw index to fail; -1 = never

  ~MockSre() {
    for (void *P : Raws)
      std::free(P);
  }
  void *rawAlloc(size_t Bytes) {
    void *P = std::malloc(Bytes);
    if (P)
      Raws.push_back(P);
    return P;
  }
  unsigned seal(void *) {
    ++SealCalls;
    return SealRc;
  }
  unsigned enableRw(void *Va) {
    ++RwEnableCalls;
    if (RwEnableRc != 0)
      return RwEnableRc;
    if (FailRwOnCall > 0 && static_cast<int>(RwEnableCalls) == FailRwOnCall)
      return 1; // simulate a mid-range enable_rw failure
    RwEnabledPages.push_back(reinterpret_cast<uintptr_t>(Va));
    return 0;
  }
};

EJitCodePoolManager::Options poolOpts(size_t PoolSize) {
  EJitCodePoolManager::Options O;
  O.poolSize = PoolSize;
  O.poolAlign = PoolSize;
  O.minCodeAlign = 64;
  return O;
}

// 64 bytes of filler "code" referenced by the content block (must outlive G).
const char CodeBytes[64] = {0};

std::unique_ptr<LinkGraph> makeCodeGraph(size_t Size, uint64_t VAddr) {
  auto G = std::make_unique<LinkGraph>(
      "g", std::make_shared<orc::SymbolStringPool>(),
      Triple("x86_64-unknown-linux-gnu"), SubtargetFeatures(),
      getGenericEdgeKindName);
  auto &Sec =
      G->createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  G->createContentBlock(Sec, ArrayRef<char>(CodeBytes, Size),
                        orc::ExecutorAddr(VAddr), 16, 0);
  return G;
}

void *firstBlockAddr(LinkGraph &G) {
  Block *B = *G.blocks().begin();
  return B->getAddress().toPtr<void *>();
}

} // namespace

// JIT code memory is allocated out of the pool (not mmap), and the resolved
// block address lands inside an owned pool.
TEST(EJitCodePoolMemMgr, CodeMemoryComesFromPool) {
  MockSre M;
  EJitCodePoolManager Pool(
      poolOpts(/*PoolSize=*/256 * 1024),
      [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
  EJitCodePoolMemoryManager MM(Pool, /*PageSize=*/4096);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = firstBlockAddr(*G);

  EXPECT_TRUE(Pool.contains(CodeAddr));
  auto S = Pool.getStats();
  EXPECT_EQ(S.poolCount, 1u);
  EXPECT_GT(S.usedBytes, 0u);

  auto FA = cantFail(IFA->finalize());
  cantFail(MM.deallocate(std::move(FA)));
}

// finalize() does NOT seal: the pool stays RW so JITLink can keep writing.
// Sealing happens out-of-band (the engine does it at lookup).
TEST(EJitCodePoolMemMgr, FinalizeKeepsPoolWritable) {
  MockSre M;
  EJitCodePoolManager Pool(
      poolOpts(256 * 1024), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = firstBlockAddr(*G);
  auto FA = cantFail(IFA->finalize());

  // The memory manager must not have flipped permissions.
  EXPECT_EQ(M.SealCalls, 0u);
  EXPECT_EQ(Pool.getStats().sealedCount, 0u);

  // The engine's lookup step seals the containing pool.
  cantFail(Pool.sealPoolContaining(CodeAddr));
  EXPECT_EQ(M.SealCalls, 1u);
  EXPECT_EQ(Pool.getStats().sealedCount, 1u);

  cantFail(MM.deallocate(std::move(FA)));
}

// Sealing the pool that backs a finalized function is idempotent: a repeated
// seal (e.g. a second lookup of the same function) does not re-invoke enable_ex.
TEST(EJitCodePoolMemMgr, RepeatedSealNoDuplicateEnableEx) {
  MockSre M;
  EJitCodePoolManager Pool(
      poolOpts(256 * 1024), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = firstBlockAddr(*G);
  auto FA = cantFail(IFA->finalize());

  cantFail(Pool.sealPoolContaining(CodeAddr)); // first lookup
  cantFail(Pool.sealPoolContaining(CodeAddr)); // second lookup, same pool
  EXPECT_EQ(M.SealCalls, 1u);
  EXPECT_EQ(Pool.getStats().sealInvocations, 1u);

  cantFail(MM.deallocate(std::move(FA)));
}

// Runtime scenario: two functions are compiled and "looked up" in turn. The
// first seals its pool; because a sealed pool is never reused, the second
// function is allocated from a brand-new pool. Mirrors the engine flow of
// compile -> lookup(seal) -> compile -> lookup(seal).
TEST(EJitCodePoolMemMgr, SecondFunctionUsesNewPoolAfterSeal) {
  constexpr size_t kPoolSize = 64 * 1024;
  MockSre M;
  EJitCodePoolManager Pool(
      poolOpts(kPoolSize), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  // Function 1: allocate, finalize, seal (first lookup).
  auto G1 = makeCodeGraph(64, 0x1000);
  auto IFA1 = cantFail(MM.allocate(nullptr, *G1));
  void *Addr1 = firstBlockAddr(*G1);
  auto FA1 = cantFail(IFA1->finalize());
  cantFail(Pool.sealPoolContaining(Addr1));
  EXPECT_EQ(M.SealCalls, 1u);
  EXPECT_EQ(Pool.getStats().poolCount, 1u);

  // Function 2: the active pool is now sealed, so this must land in a new pool
  // even though the first pool still had free space.
  auto G2 = makeCodeGraph(64, 0x2000);
  auto IFA2 = cantFail(MM.allocate(nullptr, *G2));
  void *Addr2 = firstBlockAddr(*G2);
  auto FA2 = cantFail(IFA2->finalize());
  cantFail(Pool.sealPoolContaining(Addr2));

  auto S = Pool.getStats();
  EXPECT_EQ(S.poolCount, 2u);
  EXPECT_EQ(S.sealedCount, 2u);
  EXPECT_EQ(M.SealCalls, 2u);

  // The two functions live in different 64KiB pools.
  auto poolOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) & ~static_cast<uintptr_t>(kPoolSize - 1);
  };
  EXPECT_NE(poolOf(Addr1), poolOf(Addr2));

  cantFail(MM.deallocate(std::move(FA1)));
  cantFail(MM.deallocate(std::move(FA2)));
}

//===----------------------------------------------------------------------===//
// 4K page-seal mode tests
//
// Drive the memory manager with the pool in 4K seal mode: split_2m_to_4k once
// per pool at creation, and enable_ex per covered 4KiB page at finalize (the
// allocate step must not seal anything). All mocks; no real platform symbols.
//===----------------------------------------------------------------------===//
namespace {

constexpr size_t kTwoMiB = static_cast<size_t>(2) * 1024 * 1024;
constexpr size_t kFourKiB = static_cast<size_t>(4) * 1024;

struct MockSre4K {
  std::vector<void *> Origs;
  std::vector<std::pair<uintptr_t, size_t>> Splits;
  std::vector<uintptr_t> SealedPages;
  unsigned SplitRc = 0;
  size_t SealCalls = 0;
  int FailSealOnCall = -1; // 1-based seal index to fail; -1 = never
  unsigned SealFailRc = 7;
  std::vector<uintptr_t> RwEnabledPages;
  size_t RwEnableCalls = 0;
  unsigned RwEnableRc = 0;     // 0 = success; non-zero simulates enable_rw failure
  int FailRwOnCall = -1;       // 1-based enable_rw index to fail; -1 = never

  ~MockSre4K() {
    for (void *P : Origs)
      std::free(P);
  }
  void *rawAlloc(size_t Bytes) {
    void *Base = nullptr;
    // 2MiB-aligned over-allocation; return a deliberately misaligned pointer.
    if (posix_memalign(&Base, kTwoMiB, Bytes + kTwoMiB) != 0)
      return nullptr;
    Origs.push_back(Base);
    return static_cast<char *>(Base) + kFourKiB;
  }
  unsigned split(void *Base, size_t Size) {
    Splits.push_back({reinterpret_cast<uintptr_t>(Base), Size});
    return SplitRc;
  }
  unsigned seal(void *P) {
    ++SealCalls;
    SealedPages.push_back(reinterpret_cast<uintptr_t>(P));
    if (FailSealOnCall > 0 && static_cast<int>(SealCalls) == FailSealOnCall)
      return SealFailRc;
    return 0;
  }
  unsigned enableRw(void *Va) {
    ++RwEnableCalls;
    if (RwEnableRc != 0)
      return RwEnableRc;
    if (FailRwOnCall > 0 && static_cast<int>(RwEnableCalls) == FailRwOnCall)
      return 1; // simulate a mid-range enable_rw failure
    RwEnabledPages.push_back(reinterpret_cast<uintptr_t>(Va));
    return 0;
  }
};

EJitCodePoolManager::Options fourKMemMgrOpts() {
  EJitCodePoolManager::Options O;
  O.poolSize = kTwoMiB;
  O.poolAlign = kTwoMiB;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kFourKiB;
  return O;
}

// Backing buffer large enough for a multi-page content block (avoids the 64-byte
// CodeBytes overread for big graphs).
const char BigCode[16 * 1024] = {0};

std::unique_ptr<LinkGraph> makeBackedCodeGraph(const char *Buf, size_t Size,
                                               uint64_t VAddr) {
  auto G = std::make_unique<LinkGraph>(
      "g", std::make_shared<orc::SymbolStringPool>(),
      Triple("x86_64-unknown-linux-gnu"), SubtargetFeatures(),
      getGenericEdgeKindName);
  auto &Sec =
      G->createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  G->createContentBlock(Sec, ArrayRef<char>(Buf, Size),
                        orc::ExecutorAddr(VAddr), 16, 0);
  return G;
}

// A graph with one executable (__text, R+X) section and one writable
// (__data, R+W) section. The two land in separate AllocGroups, so the memory
// manager lays them out on different (page-aligned) segments.
std::unique_ptr<LinkGraph> makeTextAndDataGraph(uint64_t TextVAddr,
                                                uint64_t DataVAddr) {
  auto G = std::make_unique<LinkGraph>(
      "g", std::make_shared<orc::SymbolStringPool>(),
      Triple("x86_64-unknown-linux-gnu"), SubtargetFeatures(),
      getGenericEdgeKindName);
  auto &Text =
      G->createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  G->createContentBlock(Text, ArrayRef<char>(CodeBytes, 64),
                        orc::ExecutorAddr(TextVAddr), 16, 0);
  auto &Data =
      G->createSection("__data", orc::MemProt::Read | orc::MemProt::Write);
  G->createContentBlock(Data, ArrayRef<char>(CodeBytes, 64),
                        orc::ExecutorAddr(DataVAddr), 16, 0);
  return G;
}

// Returns the assigned address of the first block whose owning section's
// executable bit matches WantExec (after allocate() has applied the layout).
void *blockAddrByExec(LinkGraph &G, bool WantExec) {
  for (Block *B : G.blocks()) {
    bool IsExec = (B->getSection().getMemProt() & orc::MemProt::Exec) !=
                  orc::MemProt::None;
    if (IsExec == WantExec)
      return B->getAddress().toPtr<void *>();
  }
  return nullptr;
}

} // namespace

// allocate() must not seal; finalize() seals exactly the covered 4K page(s);
// split runs once at pool creation.
TEST(EJitCodePoolMemMgr4K, FinalizeSealsCoveredPage) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeCodeGraph(64, 0x1000); // 64 bytes -> one 4K page slab
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = firstBlockAddr(*G);
  EXPECT_TRUE(Pool.contains(CodeAddr));
  EXPECT_EQ(M.SealCalls, 0u); // allocate must not enable_ex
  EXPECT_EQ(Pool.getStats().splitInvocations, 1u);

  auto FA = cantFail(IFA->finalize()); // seals here
  EXPECT_EQ(M.SealCalls, 1u);          // only the one covered page
  EXPECT_EQ(Pool.getStats().sealInvocations, 1u);

  cantFail(MM.deallocate(std::move(FA)));
}

// A multi-page function seals each covered 4K page at finalize.
TEST(EJitCodePoolMemMgr4K, FinalizeSealsAllPagesOfMultiPageCode) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  // 9000 bytes of content -> ceil(9000 / 4096) = 3 pages.
  auto G = makeBackedCodeGraph(BigCode, 9000, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  EXPECT_EQ(M.SealCalls, 0u);

  auto FA = cantFail(IFA->finalize());
  EXPECT_EQ(M.SealCalls, 3u);
  EXPECT_EQ(Pool.getStats().sealInvocations, 3u);

  cantFail(MM.deallocate(std::move(FA)));
}

// If enable_ex fails for any page, finalize returns an Error (no callable
// pointer is handed back).
TEST(EJitCodePoolMemMgr4K, FinalizeReturnsErrorWhenSealFails) {
  MockSre4K M;
  M.FailSealOnCall = 1; // fail the first page seal
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  auto FA = IFA->finalize();
  EXPECT_FALSE(static_cast<bool>(FA)); // finalize failed -> no FinalizedAlloc
  consumeError(FA.takeError());
}

// Two functions compiled in turn reuse the SAME 2MiB pool (split once) but land
// on different 4K pages, and neither lands on the other's sealed page.
TEST(EJitCodePoolMemMgr4K, SecondFunctionUsesFreshPageSamePool) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G1 = makeCodeGraph(64, 0x1000);
  auto IFA1 = cantFail(MM.allocate(nullptr, *G1));
  void *Addr1 = firstBlockAddr(*G1);
  auto FA1 = cantFail(IFA1->finalize());
  EXPECT_EQ(M.SealCalls, 1u);

  auto G2 = makeCodeGraph(64, 0x2000);
  auto IFA2 = cantFail(MM.allocate(nullptr, *G2));
  void *Addr2 = firstBlockAddr(*G2);
  auto FA2 = cantFail(IFA2->finalize());
  EXPECT_EQ(M.SealCalls, 2u);

  auto S = Pool.getStats();
  EXPECT_EQ(S.poolCount, 1u);        // same pool reused (memory efficient)
  EXPECT_EQ(S.splitInvocations, 1u); // split once for the one pool

  auto pageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) & ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  EXPECT_NE(pageOf(Addr1), pageOf(Addr2)); // different 4K pages

  cantFail(MM.deallocate(std::move(FA1)));
  cantFail(MM.deallocate(std::move(FA2)));
}

// finalize() must seal ONLY the executable segment's page(s); a writable
// (__data) segment that lands on its own page is never flipped to RX, and
// findRange resolves only the executable pointer (the data pointer is rejected
// because it was never recorded as code). This is the core guarantee that the
// per-core peer seal touches exactly the executable extent, not the whole slab.
TEST(EJitCodePoolMemMgr4K, SealsAndRecordsOnlyExecutableSegment) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeTextAndDataGraph(0x1000, 0x2000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *TextAddr = blockAddrByExec(*G, /*WantExec=*/true);
  void *DataAddr = blockAddrByExec(*G, /*WantExec=*/false);
  ASSERT_NE(TextAddr, nullptr);
  ASSERT_NE(DataAddr, nullptr);
  EXPECT_EQ(M.SealCalls, 0u); // allocate must not seal anything

  auto pageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  ASSERT_NE(pageOf(TextAddr), pageOf(DataAddr)); // distinct pages

  auto FA = cantFail(IFA->finalize());

  // Exactly one page sealed, and it is the executable page (never the data
  // one).
  EXPECT_EQ(M.SealCalls, 1u);
  ASSERT_EQ(M.SealedPages.size(), 1u);
  EXPECT_EQ(M.SealedPages[0], pageOf(TextAddr));
  for (uintptr_t Sealed : M.SealedPages)
    EXPECT_NE(Sealed, pageOf(DataAddr));

  // Only the executable pointer resolves to a recorded code range.
  EJitCompiledCodeInfo Info{};
  EXPECT_TRUE(Pool.findRange(TextAddr, Info));
  EXPECT_EQ(Info.fnPtr, TextAddr);
  EXPECT_FALSE(Pool.findRange(DataAddr, Info));

  cantFail(MM.deallocate(std::move(FA)));
}

// Code-segment placement (needsEnableRw): allocate must enable_rw the slab's
// pages (RX -> RW) BEFORE memset/JITLink writes, so the RX code-segment pages
// are writable. enable_rw happens at allocate; enable_ex (seal) only later at
// finalize/lookup - confirming the RW-then-RX toggle order.
TEST(EJitCodePoolMemMgr, AllocateEnablesRwBeforeWrite) {
  MockSre M;
  auto O = poolOpts(256 * 1024);
  O.needsEnableRw = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); }, nullptr, // no split (legacy mode)
      [&M](void *V) { return M.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));

  // allocate enable_rw'd the slab's pages before the write; seal not yet.
  EXPECT_GT(M.RwEnableCalls, 0u);
  EXPECT_EQ(M.SealCalls, 0u);

  auto FA = cantFail(IFA->finalize());
  // finalize in legacy mode does not seal either (seal is at lookup).
  EXPECT_EQ(M.SealCalls, 0u);
  cantFail(MM.deallocate(std::move(FA)));
}

// enable_rw failure (every page fails): allocate must return an Error and not
// reach memset. No rollback (0 pages were successfully RW'd).
TEST(EJitCodePoolMemMgr, AllocateFailsWhenEnableRwFails) {
  MockSre M;
  M.RwEnableRc = 7; // every enable_rw call fails
  auto O = poolOpts(256 * 1024);
  O.needsEnableRw = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); }, nullptr,
      [&M](void *V) { return M.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  auto G = makeCodeGraph(64, 0x1000);
  auto Res = MM.allocate(nullptr, *G);
  ASSERT_FALSE((bool)Res) << "allocate must fail when enable_rw fails";
  EXPECT_GT(M.RwEnableCalls, 0u); // enable_rw was attempted
  EXPECT_EQ(M.SealCalls, 0u);     // 0 pages RW'd -> no rollback seal
  consumeError(Res.takeError());
}

// Partial enable_rw failure (2nd page fails on a multi-page slab): allocate
// must return an Error AND roll back the page that was successfully made
// writable (seal it back to RX), so the code segment is not left with a
// permanently-RW page (W^X).
TEST(EJitCodePoolMemMgr, AllocateRollsBackOnPartialEnableRwFailure) {
  MockSre M;
  M.FailRwOnCall = 2; // page 0 succeeds, page 1 fails
  auto O = poolOpts(256 * 1024);
  O.needsEnableRw = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); }, nullptr,
      [&M](void *V) { return M.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  auto G = makeCodeGraph(8192, 0x1000); // spans >= 2 pages
  auto Res = MM.allocate(nullptr, *G);
  ASSERT_FALSE((bool)Res) << "allocate must fail on partial enable_rw failure";
  EXPECT_EQ(M.RwEnabledPages.size(), 1u); // only page 0 was RW'd
  EXPECT_EQ(M.SealCalls, 1u);             // rollback sealed page 0 back to RX
  consumeError(Res.takeError());
}

// needsEnableRw=false (data-region placement): allocate must NOT call
// enable_rw (the region is already RW). Regression guard for the no-op path.
TEST(EJitCodePoolMemMgr, NoEnableRwWhenNotNeeded) {
  MockSre M;
  EJitCodePoolManager Pool(
      poolOpts(256 * 1024), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); }, nullptr,
      [&M](void *V) { return M.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, 4096);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  EXPECT_EQ(M.RwEnableCalls, 0u); // data-region: no enable_rw

  auto FA = cantFail(IFA->finalize());
  cantFail(MM.deallocate(std::move(FA)));
}

// Production config (EJIT_FIXED_CODE_POOL preset): 4K page-seal + fixed
// code-segment region (needsEnableRw). allocate enable_rw's the slab pages
// (RX->RW) before memset - covering BOTH the executable __text page and the
// writable __data page (the whole slab is writable during the write). finalize
// then seals ONLY the executable page (RW->RX), leaving the __data page
// unsealed (RW) - the per-page W^X toggle order and granularity the
// code-segment placement relies on. The raw allocator is never consulted
// (the fixed region supplies the memory).
TEST(EJitCodePoolMemMgr4K, FixedCodeSegmentEnablesRwThenSealsExecOnly) {
  // A 2MiB-aligned 2MiB region stands in for the linker-script .text.ejit block
  // [__ejit_code_start, __ejit_code_end). Exactly one pool is carved from it.
  constexpr size_t kRegion = kTwoMiB;
  void *Region = nullptr;
  ASSERT_EQ(posix_memalign(&Region, kTwoMiB, kRegion), 0);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, std::free);

  MockSre4K M;
  auto O = fourKMemMgrOpts();
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;
  O.needsEnableRw = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); },
      [&M](void *V) { return M.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeTextAndDataGraph(0x1000, 0x2000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *TextAddr = blockAddrByExec(*G, /*WantExec=*/true);
  void *DataAddr = blockAddrByExec(*G, /*WantExec=*/false);
  ASSERT_NE(TextAddr, nullptr);
  ASSERT_NE(DataAddr, nullptr);

  auto pageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  ASSERT_NE(pageOf(TextAddr), pageOf(DataAddr)); // distinct pages

  // allocate enable_rw'd the slab's pages (RX->RW) before the write - both the
  // executable and the data page, since the whole slab is written. Seal has
  // not run yet, and the raw allocator was never consulted (fixed region).
  EXPECT_GT(M.RwEnableCalls, 0u);
  EXPECT_EQ(M.SealCalls, 0u);
  EXPECT_TRUE(M.Origs.empty());
  bool TextRw = false, DataRw = false;
  for (uintptr_t P : M.RwEnabledPages) {
    if (P == pageOf(TextAddr))
      TextRw = true;
    if (P == pageOf(DataAddr))
      DataRw = true;
  }
  EXPECT_TRUE(TextRw); // exec page RW'd (sealed RX at finalize)
  EXPECT_TRUE(DataRw); // data page RW'd (stays RW - the W^X tradeoff)

  auto FA = cantFail(IFA->finalize());

  // finalize seals ONLY the executable page (RW->RX); the data page is NOT
  // sealed (left RW for data/GOT writes) - the per-page W^X guarantee.
  EXPECT_EQ(M.SealCalls, 1u);
  ASSERT_EQ(M.SealedPages.size(), 1u);
  EXPECT_EQ(M.SealedPages[0], pageOf(TextAddr));
  for (uintptr_t Sealed : M.SealedPages)
    EXPECT_NE(Sealed, pageOf(DataAddr));

  cantFail(MM.deallocate(std::move(FA)));
}
