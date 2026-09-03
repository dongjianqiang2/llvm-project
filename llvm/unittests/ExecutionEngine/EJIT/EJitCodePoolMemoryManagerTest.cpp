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

#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/JITLink/JITLinkDylib.h"
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

void *testAlignedAlloc(size_t Alignment, size_t Size) {
  if (Alignment < alignof(void *) || (Alignment & (Alignment - 1)) != 0)
    return nullptr;
  void *Raw = std::malloc(Size + Alignment - 1 + sizeof(void *));
  if (!Raw)
    return nullptr;
  uintptr_t Start = reinterpret_cast<uintptr_t>(Raw) + sizeof(void *);
  uintptr_t Aligned = (Start + Alignment - 1) & ~(Alignment - 1);
  reinterpret_cast<void **>(Aligned)[-1] = Raw;
  return reinterpret_cast<void *>(Aligned);
}

void testAlignedFree(void *P) {
  if (P)
    std::free(reinterpret_cast<void **>(P)[-1]);
}

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

// A code graph with one DEFINED symbol of the given size at the block's start
// (offset 0). fnSize capture at finalize iterates G->defined_symbols(), so a
// graph with no defined symbol yields fnSize=0 (the "no symbol metadata"
// fallback); this helper builds the symbol-bearing variant used to assert the
// entry's real size is recovered.
std::unique_ptr<LinkGraph> makeCodeGraphWithDefinedSymbol(size_t Size,
                                                          uint64_t VAddr) {
  auto G = makeCodeGraph(Size, VAddr);
  // blocks() yields Block* (BlockSet iterators); dereference to a Block& for
  // addDefinedSymbol, which attaches a named defined symbol of |Size| at the
  // block's start (offset 0).
  Block &B = **G->blocks().begin();
  G->addDefinedSymbol(B, orc::ExecutorAddrDiff(0), "entry",
                      orc::ExecutorAddrDiff(Size), Linkage::Strong,
                      Scope::Default, true, false);
  return G;
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

TEST(EJitCodePoolMemMgr, RoutesTemporaryTier1ToFarPool) {
  MockSre M;
  auto NearOpts = poolOpts(/*PoolSize=*/256 * 1024);
  NearOpts.kind = EJitCodePoolKind::Near;
  auto FarOpts = poolOpts(/*PoolSize=*/256 * 1024);
  FarOpts.kind = EJitCodePoolKind::Far;
  EJitCodePoolManager Near(
      NearOpts, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
  EJitCodePoolManager Far(
      FarOpts, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *B) { return M.seal(B); });
  EJitCodePoolMemoryManager MM(Near, Far, /*PageSize=*/4096);

  JITLinkDylib Tier1("spec_t1_1");
  auto G1 = makeCodeGraph(64, 0x1000);
  auto IFA1 = cantFail(MM.allocate(&Tier1, *G1));
  void *Tier1Addr = firstBlockAddr(*G1);
  auto FA1 = cantFail(IFA1->finalize());

  EXPECT_FALSE(Near.contains(Tier1Addr));
  EXPECT_TRUE(Far.contains(Tier1Addr));
  EXPECT_EQ(Near.getStats().poolCount, 0u);
  EXPECT_EQ(Far.getStats().poolCount, 1u);
  EJitCompiledCodeInfo Tier1Info;
  ASSERT_TRUE(Far.findRange(Tier1Addr, Tier1Info));
  EXPECT_EQ(Tier1Info.poolKind, EJitCodePoolKind::Far);

  JITLinkDylib Tier2("spec_t2_1");
  auto G2 = makeCodeGraph(64, 0x2000);
  auto IFA2 = cantFail(MM.allocate(&Tier2, *G2));
  void *Tier2Addr = firstBlockAddr(*G2);
  auto FA2 = cantFail(IFA2->finalize());

  EXPECT_TRUE(Near.contains(Tier2Addr));
  EXPECT_FALSE(Far.contains(Tier2Addr));
  EXPECT_EQ(Near.getStats().poolCount, 1u);
  EJitCompiledCodeInfo Tier2Info;
  ASSERT_TRUE(Near.findRange(Tier2Addr, Tier2Info));
  EXPECT_EQ(Tier2Info.poolKind, EJitCodePoolKind::Near);

  cantFail(MM.deallocate(std::move(FA1)));
  cantFail(MM.deallocate(std::move(FA2)));
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
// seal (e.g. a second lookup of the same function) does not re-invoke
// enable_ex.
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
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kPoolSize - 1);
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
  uintptr_t FailSealRegion = 0;
  size_t FailSealRegionSize = 0;
  std::vector<uintptr_t> RwEnabledPages;
  size_t RwEnableCalls = 0;
  unsigned RwEnableRc = 0; // 0 = success; non-zero simulates enable_rw failure
  int FailRwOnCall = -1;   // 1-based enable_rw index to fail; -1 = never

  ~MockSre4K() {
    for (void *P : Origs)
      testAlignedFree(P);
  }
  void *rawAlloc(size_t Bytes) {
    void *Base = nullptr;
    // 2MiB-aligned over-allocation; return a deliberately misaligned pointer.
    Base = testAlignedAlloc(kTwoMiB, Bytes + kTwoMiB);
    if (!Base)
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
    const uintptr_t Page = reinterpret_cast<uintptr_t>(P);
    if (FailSealRegion != 0 &&
        (Page < FailSealRegion || Page - FailSealRegion >= FailSealRegionSize))
      return 0;
    if (FailSealRegion != 0)
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

// Backing buffer large enough for a multi-page content block (avoids the
// 64-byte CodeBytes overread for big graphs).
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

// A graph with one executable (__text, R+X) section and one pure read-only
// (__rodata, R) section — the shape a function holding a string constant or a
// const table links into. RodataAlign is the read-only block's alignment.
std::unique_ptr<LinkGraph> makeTextAndRoDataGraph(uint64_t TextVAddr,
                                                  uint64_t RoDataVAddr,
                                                  unsigned RodataAlign) {
  auto G = std::make_unique<LinkGraph>(
      "g", std::make_shared<orc::SymbolStringPool>(),
      Triple("x86_64-unknown-linux-gnu"), SubtargetFeatures(),
      getGenericEdgeKindName);
  auto &Text =
      G->createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  G->createContentBlock(Text, ArrayRef<char>(CodeBytes, 64),
                        orc::ExecutorAddr(TextVAddr), 16, 0);
  auto &RoData = G->createSection("__rodata", orc::MemProt::Read);
  // 37 bytes — the exact size of the .rodata.str1.1 section observed on the
  // board forcing a whole-graph page fallback.
  G->createContentBlock(RoData, ArrayRef<char>(CodeBytes, 37),
                        orc::ExecutorAddr(RoDataVAddr), RodataAlign, 0);
  return G;
}

// Returns the assigned address of the first block in the given section
// (after allocate() has applied the layout). Section identity, not prot: the
// read-only section is promoted to R+X during allocate, so a prot-based scan
// can no longer tell it apart from the code.
void *blockAddrInSection(LinkGraph &G, Section &S) {
  for (Block *B : G.blocks())
    if (&B->getSection() == &S)
      return B->getAddress().toPtr<void *>();
  return nullptr;
}

// The Tier-1 online-PGO shape: one executable (__text, R+X), one read-only
// data (__profd_, R), and one runtime-writable counter (__profc_, R+W) in
// the same graph. The far pool serves spec_t1_* dylibs in immediate 4K-seal
// mode; this is the layout the promotion widens to cover.
std::unique_ptr<LinkGraph> makeTextProfdProfcGraph(uint64_t TextVAddr,
                                                   uint64_t ProfdVAddr,
                                                   uint64_t ProfcVAddr) {
  auto G = std::make_unique<LinkGraph>(
      "g", std::make_shared<orc::SymbolStringPool>(),
      Triple("x86_64-unknown-linux-gnu"), SubtargetFeatures(),
      getGenericEdgeKindName);
  auto &Text =
      G->createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  G->createContentBlock(Text, ArrayRef<char>(CodeBytes, 64),
                        orc::ExecutorAddr(TextVAddr), 16, 0);
  auto &Profd = G->createSection("__profd_", orc::MemProt::Read);
  G->createContentBlock(Profd, ArrayRef<char>(CodeBytes, 37),
                        orc::ExecutorAddr(ProfdVAddr), 16, 0);
  auto &Profc =
      G->createSection("__profc_", orc::MemProt::Read | orc::MemProt::Write);
  G->createContentBlock(Profc, ArrayRef<char>(CodeBytes, 64),
                        orc::ExecutorAddr(ProfcVAddr), 16, 0);
  return G;
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
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
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

// The finalized executable range carries the allocation's RUNTIME-WRITABLE data
// extent (the __data / __profc_ segment) so a peer core can enable_rw exactly
// those pages before executing. The writable range must be page-disjoint from
// the code (no shared 4K page) — the guarantee that makes per-core enable_rw
// safe (never RWX).
TEST(EJitCodePoolMemMgr4K, FinalizedRangeCarriesWritableDataExtent) {
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
  auto FA = cantFail(IFA->finalize());

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(TextAddr, Info));
  ASSERT_EQ(Info.writableCount, 1u);
  EXPECT_EQ(Info.writableRanges[0].addr, reinterpret_cast<uintptr_t>(DataAddr));
  EXPECT_GT(Info.writableRanges[0].size, 0u);
  // Data-region pool (needsEnableRw=false): a peer does NOT need enable_rw.
  EXPECT_EQ(Info.requiresPeerEnableRw, 0u);

  // Page-disjoint from the executable extent.
  auto pageDown = [](uintptr_t A) {
    return A & ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  auto pageUp = [](uintptr_t A) {
    return (A + kFourKiB - 1) & ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  uintptr_t CodePS = pageDown(Info.codeStart);
  uintptr_t CodePE = pageUp(Info.codeStart + Info.codeSize);
  uintptr_t WPS = pageDown(Info.writableRanges[0].addr);
  uintptr_t WPE =
      pageUp(Info.writableRanges[0].addr + Info.writableRanges[0].size);
  EXPECT_TRUE(WPE <= CodePS || CodePE <= WPS); // no shared 4K page

  cantFail(MM.deallocate(std::move(FA)));
}

// The finalized executable range carries the entry function's real size
// (fnSize), recovered from the JITLink graph's defined symbols by matching
// the published fnPtr address. fnSize < codeSize (the allocation covers the
// whole executable extent incl. padding).
TEST(EJitCodePoolMemMgr4K, FinalizedRangeCarriesEntryFnSize) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  constexpr size_t FnSize = 64;
  auto G = makeCodeGraphWithDefinedSymbol(FnSize, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *EntryAddr = firstBlockAddr(*G);
  auto FA = cantFail(IFA->finalize());

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(EntryAddr, Info));
  EXPECT_EQ(Info.fnPtr, EntryAddr);
  EXPECT_EQ(Info.fnSize, static_cast<uint64_t>(FnSize));
  // codeSize covers the whole executable allocation (page-rounded): it is at
  // least the function size, so overhead = codeSize - fnSize >= 0.
  EXPECT_GE(Info.codeSize, Info.fnSize);

  cantFail(MM.deallocate(std::move(FA)));
}

// A pointer inside a symbol is enough to recover its executable allocation,
// but not an exact function body beginning at that pointer.
TEST(EJitCodePoolMemMgr4K, InteriorPointerHasUnknownFnSize) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeCodeGraphWithDefinedSymbol(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  auto *EntryAddr = static_cast<uint8_t *>(firstBlockAddr(*G));
  auto FA = cantFail(IFA->finalize());

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(EntryAddr + 1, Info));
  EXPECT_EQ(Info.fnPtr, EntryAddr + 1);
  EXPECT_EQ(Info.fnSize, 0u);
  EXPECT_GT(Info.codeSize, 0u);

  cantFail(MM.deallocate(std::move(FA)));
}

// A graph with no defined symbol records no symbol metadata: fnSize is 0
// (print_compiled then reports fn_size=0, overhead=codeSize). This guards the
// "no symbol metadata" fallback path so a symbolless graph never mis-reports
// a stale fnSize.
TEST(EJitCodePoolMemMgr4K, FinalizedRangeFnSizeZeroWithoutDefinedSymbol) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeCodeGraph(64, 0x1000); // no defined symbol
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = firstBlockAddr(*G);
  auto FA = cantFail(IFA->finalize());

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(CodeAddr, Info));
  EXPECT_EQ(Info.fnSize, 0u);
  EXPECT_GT(Info.codeSize, 0u);

  cantFail(MM.deallocate(std::move(FA)));
}

// A fixed RX code-segment pool (needsEnableRw=true) stamps requiresPeerEnableRw
// on the range: a peer MUST enable_rw the writable pages. The whole slab is
// enable_rw'd at allocate, then only the exec pages are sealed at finalize, so
// the writable data page stays RW on the owner.
TEST(EJitCodePoolMemMgr4K, FixedRxPoolMarksRequiresPeerEnableRw) {
  MockSre4K M;
  auto O = fourKMemMgrOpts();
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
  ASSERT_NE(TextAddr, nullptr);
  auto FA = cantFail(IFA->finalize());

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(TextAddr, Info));
  EXPECT_EQ(Info.writableCount, 1u);
  EXPECT_EQ(Info.requiresPeerEnableRw, 1u); // fixed RX pool -> peer enable_rw

  cantFail(MM.deallocate(std::move(FA)));
}

// A code-only allocation (no writable data, e.g. non-PGO or Tier-2) records a
// range with ZERO writable extents, so a peer seals only the code pages.
TEST(EJitCodePoolMemMgr4K, CodeOnlyAllocationHasNoWritableExtent) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeCodeGraph(64, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = firstBlockAddr(*G);
  auto FA = cantFail(IFA->finalize());

  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(CodeAddr, Info));
  EXPECT_EQ(Info.writableCount, 0u);

  cantFail(MM.deallocate(std::move(FA)));
}

// recordFinalizedRange must never truncate: an over-bound writable count is
// REJECTED (returns false) and the executable range is NOT recorded, so
// findRange fails and the owner never publishes a callable, under-prepared
// pointer. A malformed set (null array + non-zero count, or a writable range
// outside the pool) is rejected the same way.
TEST(EJitCodePoolMemMgr4K, RecordRejectsOverflowWritableRanges) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });

  // Carve a real pool so the recorded range resolves to a known pool.
  void *P = cantFail(Pool.allocateCode(4096, 64));
  EJitWritableRange W[kEJitMaxWritableRanges + 1];
  for (uint32_t i = 0; i < kEJitMaxWritableRanges + 1; ++i) {
    W[i].addr = reinterpret_cast<uintptr_t>(P) + 0x1000 * (i + 1);
    W[i].size = 16;
  }
  // Over-bound count -> rejected, nothing recorded.
  EXPECT_FALSE(
      Pool.recordFinalizedRange(P, 200, W, kEJitMaxWritableRanges + 1));
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Pool.findRange(P, Info)); // NOT recorded -> resolve fails

  // Non-zero count with a null array -> rejected.
  EXPECT_FALSE(Pool.recordFinalizedRange(P, 200, nullptr, 1));
  EXPECT_FALSE(Pool.findRange(P, Info));

  // A writable range outside the owning pool -> rejected.
  EJitWritableRange OutRange[1];
  OutRange[0].addr =
      reinterpret_cast<uintptr_t>(P) + kTwoMiB * 4; // far outside
  OutRange[0].size = 16;
  EXPECT_FALSE(Pool.recordFinalizedRange(P, 200, OutRange, 1));
  EXPECT_FALSE(Pool.findRange(P, Info));

  // A well-formed set at the bound records fine and resolves.
  EJitWritableRange OkRange[1];
  OkRange[0].addr = reinterpret_cast<uintptr_t>(P) + 0x800;
  OkRange[0].size = 16;
  EXPECT_TRUE(Pool.recordFinalizedRange(P, 200, OkRange, 1));
  ASSERT_TRUE(Pool.findRange(P, Info));
  EXPECT_EQ(Info.writableCount, 1u);
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
  void *Region = testAlignedAlloc(kTwoMiB, kRegion);
  ASSERT_NE(Region, nullptr);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, testAlignedFree);

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

// Once allocate() has made a fixed code-segment slab writable, abandoning the
// JITLink allocation must restore every slab page to RX. The bump allocation is
// intentionally not reclaimed, but no writable hole may remain in .text.ejit.
TEST(EJitCodePoolMemMgr4K, AbandonRestoresWholeSlabToRx) {
  constexpr size_t kRegion = kTwoMiB;
  void *Region = testAlignedAlloc(kTwoMiB, kRegion);
  ASSERT_NE(Region, nullptr);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, testAlignedFree);

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
  ASSERT_GT(M.RwEnabledPages.size(), 0u);
  size_t WritablePages = M.RwEnabledPages.size();

  bool CallbackCalled = false;
  bool AbandonFailed = false;
  IFA->abandon([&](Error Err) {
    CallbackCalled = true;
    if (Err) {
      AbandonFailed = true;
      consumeError(std::move(Err));
    }
  });
  EXPECT_TRUE(CallbackCalled);
  EXPECT_FALSE(AbandonFailed);
  EXPECT_EQ(M.SealCalls, WritablePages);
  EXPECT_EQ(M.SealedPages.size(), WritablePages);
}

TEST(EJitCodePoolMemMgrBatch, AbandonLeavesDeadSharedPageRwNxAndUnreused) {
  constexpr size_t kRegion = kTwoMiB;
  void *Region = testAlignedAlloc(kTwoMiB, kRegion);
  ASSERT_NE(Region, nullptr);
  std::unique_ptr<void, void (*)(void *)> Guard(Region, testAlignedFree);

  MockSre4K M;
  auto O = fourKMemMgrOpts();
  O.fixedBase = reinterpret_cast<uintptr_t>(Region);
  O.fixedSize = kRegion;
  O.needsEnableRw = true;
  O.minCodeAlign = 16;
  O.batchedPageSeal = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); },
      [&M](void *V) { return M.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G0 = makeCodeGraph(64, 0x3000);
  auto IFA0 = cantFail(MM.allocate(nullptr, *G0));
  void *Addr0 = firstBlockAddr(*G0);
  ASSERT_NE(Addr0, nullptr);
  ASSERT_GT(M.RwEnableCalls, 0u);

  bool AbandonFailed = false;
  IFA0->abandon([&](Error Err) {
    AbandonFailed = static_cast<bool>(Err);
    consumeError(std::move(Err));
  });
  EXPECT_FALSE(AbandonFailed);
  EXPECT_EQ(M.SealCalls, 0u)
      << "a shared batch page must stay RW/NX until the batch is complete";
  EXPECT_EQ(Pool.pendingRangeCount(), 0u);

  auto G1 = makeCodeGraph(64, 0x4000);
  auto IFA1 = cantFail(MM.allocate(nullptr, *G1));
  void *Addr1 = firstBlockAddr(*G1);
  ASSERT_NE(Addr1, nullptr);
  EXPECT_GT(reinterpret_cast<uintptr_t>(Addr1),
            reinterpret_cast<uintptr_t>(Addr0))
      << "the abandoned bump allocation must never be reused";
  IFA1->abandon([](Error Err) { consumeError(std::move(Err)); });
}

TEST(EJitCodePoolMemMgrBatch, PureCodeAllocationsSharePageUntilFlush) {
  MockSre4K M;
  auto O = fourKMemMgrOpts();
  O.minCodeAlign = 16;
  O.batchedPageSeal = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G0 = makeCodeGraph(64, 0x1000);
  auto IFA0 = cantFail(MM.allocate(nullptr, *G0));
  void *Addr0 = firstBlockAddr(*G0);
  auto FA0 = cantFail(IFA0->finalize());

  auto G1 = makeCodeGraph(64, 0x2000);
  auto IFA1 = cantFail(MM.allocate(nullptr, *G1));
  void *Addr1 = firstBlockAddr(*G1);
  auto FA1 = cantFail(IFA1->finalize());

  auto PageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  EXPECT_EQ(PageOf(Addr0), PageOf(Addr1));
  EXPECT_EQ(reinterpret_cast<uintptr_t>(Addr1) -
                reinterpret_cast<uintptr_t>(Addr0),
            64u);
  EXPECT_EQ(M.SealCalls, 0u);
  EXPECT_EQ(Pool.pendingRangeCount(), 2u);
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Pool.findRange(Addr0, Info));

  cantFail(Pool.flushPendingRanges());
  EXPECT_EQ(M.SealCalls, 1u);
  EXPECT_TRUE(Pool.findRange(Addr0, Info));
  EXPECT_TRUE(Pool.findRange(Addr1, Info));

  cantFail(MM.deallocate(std::move(FA0)));
  cantFail(MM.deallocate(std::move(FA1)));
}

// A pure read-only section (string constant, const table, jump table) must
// NOT cost a page-exclusive segment in batched-seal mode: allocate folds it
// into the executable segment, so the graph keeps the compact 16B layout —
// the 37 bytes of .rodata share the code's page instead of owning a 4KiB one
// — and at flush they seal RX together with the code (tightening W^X over
// the page-based layout, which left them on a slab-wide RW page).
TEST(EJitCodePoolMemMgrBatch, ReadOnlySectionFoldsIntoExecSegment) {
  MockSre4K M;
  auto O = fourKMemMgrOpts();
  O.minCodeAlign = 16;
  O.batchedPageSeal = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeTextAndRoDataGraph(0x1000, 0x2000, /*RodataAlign=*/16);
  Section *Text = G->findSectionByName("__text");
  Section *RoData = G->findSectionByName("__rodata");
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(RoData, nullptr);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  // Resolve by section identity, not block-iteration order: G.blocks() backs a
  // DenseSet so its order is non-deterministic, and after promotion both
  // sections are R+X so a prot-based scan cannot tell them apart either.
  void *TextAddr = blockAddrInSection(*G, *Text);
  void *RoAddr = blockAddrInSection(*G, *RoData);
  ASSERT_NE(TextAddr, nullptr);
  ASSERT_NE(RoAddr, nullptr);

  // Folded into the executable segment: the section is promoted to R+X and
  // the 37 bytes share the code's page.
  EXPECT_EQ(RoData->getMemProt(),
            orc::MemProt::Read | orc::MemProt::Exec);
  auto PageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  EXPECT_EQ(PageOf(TextAddr), PageOf(RoAddr))
      << "read-only content must not occupy a page of its own";

  // The compact stride survives the folded section: the next graph packs
  // onto the same page (total 64+37 bytes, 16B-padded).
  auto G2 = makeCodeGraph(64, 0x3000);
  auto IFA2 = cantFail(MM.allocate(nullptr, *G2));
  void *Addr2 = firstBlockAddr(*G2);
  EXPECT_EQ(PageOf(Addr2), PageOf(TextAddr));

  // Both allocations record pending ranges and seal as ONE page at flush.
  EXPECT_EQ(M.SealCalls, 0u);
  auto FA = cantFail(IFA->finalize());
  auto FA2 = cantFail(IFA2->finalize());
  EXPECT_EQ(Pool.pendingRangeCount(), 2u);
  cantFail(Pool.flushPendingRanges());
  EXPECT_EQ(M.SealCalls, 1u);

  EJitCompiledCodeInfo Info{};
  EXPECT_TRUE(Pool.findRange(TextAddr, Info));

  cantFail(MM.deallocate(std::move(FA)));
  cantFail(MM.deallocate(std::move(FA2)));
}

// A read-only block aligned BEYOND the code alignment (e.g. alignas(32))
// keeps the page-granular fallback: the promotion still folds it into the
// executable segment, but the merged segment's alignment then exceeds the
// compact threshold, so the layout is page-per-segment again — the safety
// valve that keeps over-aligned content out of 16B packing.
TEST(EJitCodePoolMemMgrBatch, HighAlignedReadOnlySectionKeepsPageLayout) {
  MockSre4K M;
  auto O = fourKMemMgrOpts();
  O.minCodeAlign = 16;
  O.batchedPageSeal = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeTextAndRoDataGraph(0x1000, 0x2000, /*RodataAlign=*/32);
  Section *Text = G->findSectionByName("__text");
  Section *RoData = G->findSectionByName("__rodata");
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(RoData, nullptr);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *TextAddr = blockAddrInSection(*G, *Text);
  void *RoAddr = blockAddrInSection(*G, *RoData);
  ASSERT_NE(TextAddr, nullptr);
  ASSERT_NE(RoAddr, nullptr);

  // Folded (promoted, same page as the code)…
  EXPECT_EQ(RoData->getMemProt(), orc::MemProt::Read | orc::MemProt::Exec);
  auto PageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  EXPECT_EQ(PageOf(TextAddr), PageOf(RoAddr));

  // …but the layout fell back to page granularity: a following allocation
  // cannot share this page (a compact stride would land it ~112 bytes in).
  auto G2 = makeCodeGraph(64, 0x3000);
  auto IFA2 = cantFail(MM.allocate(nullptr, *G2));
  void *Addr2 = firstBlockAddr(*G2);
  EXPECT_NE(PageOf(Addr2), PageOf(TextAddr));

  auto FA = cantFail(IFA->finalize());
  auto FA2 = cantFail(IFA2->finalize());
  cantFail(MM.deallocate(std::move(FA)));
  cantFail(MM.deallocate(std::move(FA2)));
}

// Immediate 4K-seal mode (batchedPageSeal = false, fourKSeal = true): the
// promotion must still fire, because it is gated on usesPageSeal(), not on
// batched. Here the win is intra-allocation, not cross-allocation: a pure
// read-only section would otherwise land in its own segment and own its own
// 4KiB page (BasicLayout groups by {MemProt, MemLifetime}, so R-- and R+X
// stay separate segments, each page-padded). Promoted to R+X, rodata merges
// into the code segment, lays out contiguous with it, and the single 4K page
// is sealed RX at finalize. This is the aarch64_be far-pool / Tier-1 shape
// (online-PGO __profc_ counters are R+W and stay out of the gate).
TEST(EJitCodePoolMemMgr4K, ReadOnlySectionFoldsIntoExecSegmentImmediate) {
  MockSre4K M;
  EJitCodePoolManager Pool(
      fourKMemMgrOpts(), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeTextAndRoDataGraph(0x1000, 0x2000, /*RodataAlign=*/16);
  Section *Text = G->findSectionByName("__text");
  Section *RoData = G->findSectionByName("__rodata");
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(RoData, nullptr);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *TextAddr = blockAddrInSection(*G, *Text);
  void *RoAddr = blockAddrInSection(*G, *RoData);
  ASSERT_NE(TextAddr, nullptr);
  ASSERT_NE(RoAddr, nullptr);

  // Promoted, and the 37 bytes share the code's page (no page of its own).
  EXPECT_EQ(RoData->getMemProt(), orc::MemProt::Read | orc::MemProt::Exec);
  auto PageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  EXPECT_EQ(PageOf(TextAddr), PageOf(RoAddr))
      << "read-only content must not occupy a page of its own in immediate "
         "mode either";
  // Contiguous within the segment: rodata lands right after the 64-byte text
  // block (16-byte aligned), so the two do not straddle a page boundary.
  EXPECT_EQ(static_cast<uintptr_t>(reinterpret_cast<char *>(RoAddr) -
                                   reinterpret_cast<char *>(TextAddr)),
            static_cast<uintptr_t>(64))
      << "text and rodata must be contiguous, not page-strided";

  // Immediate mode seals the exec page at finalize (not deferred to flush).
  EXPECT_EQ(M.SealCalls, 0u);
  auto FA = cantFail(IFA->finalize());
  EXPECT_EQ(M.SealCalls, 1u);
  ASSERT_EQ(M.SealedPages.size(), 1u);
  EXPECT_EQ(M.SealedPages[0], PageOf(TextAddr));
  cantFail(MM.deallocate(std::move(FA)));
}

// The Tier-1 online-PGO shape routed through selectPool's spec_t1_ branch:
// text (R+X) + __profd_ (R, read-only) + __profc_ (R+W, runtime counter) in
// one graph, allocated on the FAR pool in immediate 4K-seal mode. This is the
// real surface the widened gate (usesPageSeal) exposes, and it asserts the
// invariants the far-pool path must keep:
//  - __profd_ is promoted to R+X and folds into the code page (it is
//    read-only; a peer reads it from the RX page, same as code);
//  - __profc_ (R+W) is NOT promoted (Write excluded) and keeps its own RW
//    page, never sharing a 4K page with the executable extent (no RWX);
//  - the recorded writable range covers ONLY __profc_ (never the exec
//    extent), so a peer's enable_rw touches only the counter page.
TEST(EJitCodePoolMemMgr4K, FarPoolTier1ProfdFoldsProfcStaysWritable) {
  MockSre4K NearM, FarM;
  auto NearOpts = fourKMemMgrOpts(); // fourKSeal=true, batched=false, near
  NearOpts.kind = EJitCodePoolKind::Near;
  auto FarOpts = fourKMemMgrOpts();
  FarOpts.kind = EJitCodePoolKind::Far;
  EJitCodePoolManager Near(
      NearOpts, [&NearM](size_t N) { return NearM.rawAlloc(N); },
      [&NearM](void *V) { return NearM.seal(V); },
      [&NearM](void *B, size_t S) { return NearM.split(B, S); });
  EJitCodePoolManager Far(
      FarOpts, [&FarM](size_t N) { return FarM.rawAlloc(N); },
      [&FarM](void *V) { return FarM.seal(V); },
      [&FarM](void *B, size_t S) { return FarM.split(B, S); });
  EJitCodePoolMemoryManager MM(Near, Far, kFourKiB);

  JITLinkDylib Tier1("spec_t1_1");
  auto G = makeTextProfdProfcGraph(0x1000, 0x2000, 0x3000);
  Section *Text = G->findSectionByName("__text");
  Section *Profd = G->findSectionByName("__profd_");
  Section *Profc = G->findSectionByName("__profc_");
  ASSERT_NE(Text, nullptr);
  ASSERT_NE(Profd, nullptr);
  ASSERT_NE(Profc, nullptr);
  auto IFA = cantFail(MM.allocate(&Tier1, *G));
  void *TextAddr = blockAddrInSection(*G, *Text);
  void *ProfdAddr = blockAddrInSection(*G, *Profd);
  void *ProfcAddr = blockAddrInSection(*G, *Profc);
  ASSERT_NE(TextAddr, nullptr);
  ASSERT_NE(ProfdAddr, nullptr);
  ASSERT_NE(ProfcAddr, nullptr);

  auto PageOf = [](void *P) {
    return reinterpret_cast<uintptr_t>(P) &
           ~static_cast<uintptr_t>(kFourKiB - 1);
  };

  // Routed to the far pool (selectPool's spec_t1_ branch).
  EXPECT_TRUE(Far.contains(TextAddr));
  EXPECT_FALSE(Near.contains(TextAddr));

  // __profd_ (read-only) promoted to R+X and folded onto the code page.
  EXPECT_EQ(Profd->getMemProt(), orc::MemProt::Read | orc::MemProt::Exec);
  EXPECT_EQ(PageOf(TextAddr), PageOf(ProfdAddr))
      << "__profd_ must fold into the code page, not own one";

  // __profc_ (R+W) NOT promoted and never shares a page with the exec extent.
  EXPECT_EQ(Profc->getMemProt(),
            orc::MemProt::Read | orc::MemProt::Write);
  EXPECT_NE(PageOf(ProfcAddr), PageOf(TextAddr))
      << "__profc_ must keep its own RW page (no RWX)";

  auto FA = cantFail(IFA->finalize());
  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Far.findRange(TextAddr, Info));
  EXPECT_EQ(Info.poolKind, EJitCodePoolKind::Far);
  // Exactly one writable range, and it covers only __profc_ (never the exec
  // extent that contains text + __profd_).
  ASSERT_EQ(Info.writableCount, 1u);
  EXPECT_EQ(Info.writableRanges[0].addr, PageOf(ProfcAddr));
  EXPECT_EQ(PageOf(reinterpret_cast<void *>(Info.writableRanges[0].addr)),
            PageOf(ProfcAddr));
  EXPECT_NE(PageOf(reinterpret_cast<void *>(Info.writableRanges[0].addr)),
            PageOf(TextAddr));
  cantFail(MM.deallocate(std::move(FA)));
}

// In batched-seal mode, fnSize capture flows through the pending path
// (recordPendingRange → flushPendingRanges → findRange), not the immediate
// recordFinalizedRange path. A pending range is NOT resolvable by findRange
// until flush; fnSize must survive the pending→finalized copy so it is still
// recoverable after flush.
TEST(EJitCodePoolMemMgrBatch, FnSizeSurvivesPendingFlush) {
  MockSre4K M;
  auto O = fourKMemMgrOpts();
  O.minCodeAlign = 16;
  O.batchedPageSeal = true;
  EJitCodePoolManager Pool(
      O, [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); },
      [&M](void *B, size_t S) { return M.split(B, S); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  constexpr size_t FnSize = 64;
  auto G = makeCodeGraphWithDefinedSymbol(FnSize, 0x1000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *EntryAddr = firstBlockAddr(*G);
  auto FA = cantFail(IFA->finalize());

  // Pending: not resolvable yet, so fnSize is not yet recoverable.
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Pool.findRange(EntryAddr, Info));

  cantFail(Pool.flushPendingRanges());
  ASSERT_TRUE(Pool.findRange(EntryAddr, Info));
  EXPECT_EQ(Info.fnPtr, EntryAddr);
  EXPECT_EQ(Info.fnSize, static_cast<uint64_t>(FnSize));

  cantFail(MM.deallocate(std::move(FA)));
}

// The fixed near-hot layout uses the JITDylib identity selected before
// JITLink allocation. Verify that cell/public allocations remain isolated and
// that a failed pool commit does not prevent another pool from committing.
TEST(EJitCodePoolMemMgrBatch, FixedNearPoolsSelectAndCommitIndependently) {
  constexpr uint32_t CellPool0 = 0;
  constexpr uint32_t CellPool1 = 1;
  constexpr uint32_t PublicPool = 16;
  constexpr size_t CellBytes = kTwoMiB;
  constexpr size_t PublicBytes = 4 * kTwoMiB;

  MockSre4K M;
  std::vector<std::unique_ptr<void, void (*)(void *)>> Regions;
  std::vector<std::unique_ptr<EJitCodePoolManager>> Pools;
  std::vector<EJitCodePoolManager *> NearPools;
  for (uint32_t I = 0; I <= PublicPool; ++I) {
    const size_t Bytes = I == PublicPool ? PublicBytes : CellBytes;
    void *Region = testAlignedAlloc(kTwoMiB, Bytes);
    ASSERT_NE(Region, nullptr);
    Regions.emplace_back(Region, testAlignedFree);

    auto O = fourKMemMgrOpts();
    O.kind = EJitCodePoolKind::Near;
    O.poolId = I;
    O.poolSize = Bytes;
    O.fixedBase = reinterpret_cast<uintptr_t>(Region);
    O.fixedSize = Bytes;
    O.needsEnableRw = true;
    O.batchedPageSeal = true;
    Pools.push_back(std::make_unique<EJitCodePoolManager>(
        O, [&M](size_t N) { return M.rawAlloc(N); },
        [&M](void *V) { return M.seal(V); },
        [&M](void *B, size_t S) { return M.split(B, S); },
        [&M](void *V) { return M.enableRw(V); }));
    NearPools.push_back(Pools.back().get());
  }

  auto Far = std::make_unique<EJitCodePoolManager>(
      poolOpts(CellBytes), [&M](size_t N) { return M.rawAlloc(N); },
      [&M](void *V) { return M.seal(V); });
  EJitCodePoolMemoryManager MM(
      NearPools, *Far, kFourKiB,
      [&](const JITLinkDylib *JD) -> EJitCodePoolManager * {
        if (!JD)
          return nullptr;
        if (JD->getName() == "spec_cell0")
          return NearPools[CellPool0];
        if (JD->getName() == "spec_cell1")
          return NearPools[CellPool1];
        if (JD->getName() == "spec_public")
          return NearPools[PublicPool];
        return nullptr;
      });

  JITLinkDylib Cell0("spec_cell0");
  JITLinkDylib Cell1("spec_cell1");
  JITLinkDylib Public("spec_public");
  JITLinkDylib Unknown("spec_cell00");

  auto G0 = makeCodeGraph(64, 0x1000);
  auto IFA0 = cantFail(MM.allocate(&Cell0, *G0));
  void *Addr0 = firstBlockAddr(*G0);
  auto FA0 = cantFail(IFA0->finalize());
  auto G0b = makeCodeGraph(64, 0x1100);
  auto IFA0b = cantFail(MM.allocate(&Cell0, *G0b));
  void *Addr0b = firstBlockAddr(*G0b);
  auto FA0b = cantFail(IFA0b->finalize());

  auto G1 = makeCodeGraph(64, 0x2000);
  auto IFA1 = cantFail(MM.allocate(&Cell1, *G1));
  void *Addr1 = firstBlockAddr(*G1);
  auto FA1 = cantFail(IFA1->finalize());
  auto GP = makeCodeGraph(64, 0x3000);
  auto IFAP = cantFail(MM.allocate(&Public, *GP));
  void *AddrP = firstBlockAddr(*GP);
  auto FAP = cantFail(IFAP->finalize());

  EXPECT_TRUE(Pools[CellPool0]->contains(Addr0));
  EXPECT_TRUE(Pools[CellPool0]->contains(Addr0b));
  EXPECT_TRUE(Pools[CellPool1]->contains(Addr1));
  EXPECT_TRUE(Pools[PublicPool]->contains(AddrP));
  EXPECT_GT(reinterpret_cast<uintptr_t>(Addr0b),
            reinterpret_cast<uintptr_t>(Addr0));
  EJitCompiledCodeInfo Info{};
  EXPECT_FALSE(Pools[CellPool0]->findRange(Addr0, Info));
  ASSERT_TRUE(Pools[CellPool0]->findPendingRange(Addr0, Info));
  EXPECT_EQ(Info.poolId, CellPool0);
  EXPECT_EQ(Info.poolKind, EJitCodePoolKind::Near);
  EXPECT_FALSE(Pools[CellPool1]->findRange(Addr1, Info));
  EXPECT_FALSE(Pools[PublicPool]->findRange(AddrP, Info));

  // Exercise the real JITLink memory-manager path with an interleaved batch,
  // rather than relying only on the four sentinel allocations above. The
  // fixed selector remains semantic (cell0/cell1/public), and every result is
  // still pending until its owning pool is flushed.
  std::vector<void *> Interleaved;
  for (size_t I = 0; I < 20; ++I) {
    const uint32_t PoolId = I % 3 == 0   ? CellPool0
                            : I % 3 == 1 ? CellPool1
                                         : PublicPool;
    const std::string Name = PoolId == CellPool0   ? "spec_cell0"
                             : PoolId == CellPool1 ? "spec_cell1"
                                                   : "spec_public";
    JITLinkDylib JD(Name);
    auto G = makeCodeGraph(64, 0x5000 + I * 0x100);
    auto IFA = cantFail(MM.allocate(&JD, *G));
    void *Addr = firstBlockAddr(*G);
    Interleaved.push_back(Addr);
    auto FA = cantFail(IFA->finalize());
    cantFail(MM.deallocate(std::move(FA)));
    ASSERT_TRUE(Pools[PoolId]->contains(Addr));
    EXPECT_FALSE(Pools[PoolId]->findRange(Addr, Info));
    ASSERT_TRUE(Pools[PoolId]->findPendingRange(Addr, Info));
    EXPECT_EQ(Info.poolId, PoolId);
  }
  EXPECT_EQ(Interleaved.size(), 20u);

  // A malformed/unknown JITDylib identity must fail allocation rather than
  // silently falling back to the first near pool.
  auto BadGraph = makeCodeGraph(64, 0x4000);
  auto BadAlloc = MM.allocate(&Unknown, *BadGraph);
  ASSERT_FALSE(static_cast<bool>(BadAlloc));
  consumeError(BadAlloc.takeError());

  // Commit cell0 and public independently. Inject a seal failure only for
  // cell1; its pending range remains retryable and cannot block cell0/public.
  M.FailSealRegion = reinterpret_cast<uintptr_t>(Regions[CellPool1].get());
  M.FailSealRegionSize = CellBytes;
  cantFail(Pools[CellPool0]->flushPendingRanges());
  cantFail(Pools[PublicPool]->flushPendingRanges());
  EXPECT_TRUE(Pools[CellPool0]->findRange(Addr0, Info));
  EXPECT_TRUE(Pools[PublicPool]->findRange(AddrP, Info));
  EXPECT_FALSE(Pools[CellPool1]->findRange(Addr1, Info));
  EXPECT_GT(Pools[CellPool1]->pendingRangeCount(), 0u);
  auto Failed = Pools[CellPool1]->flushPendingRanges();
  ASSERT_TRUE(static_cast<bool>(Failed));
  consumeError(std::move(Failed));

  M.FailSealRegion = 0;
  cantFail(Pools[CellPool1]->flushPendingRanges());
  EXPECT_TRUE(Pools[CellPool1]->findRange(Addr1, Info));
  EXPECT_EQ(Pools[CellPool1]->pendingRangeCount(), 0u);

  cantFail(MM.deallocate(std::move(FA0)));
  cantFail(MM.deallocate(std::move(FA0b)));
  cantFail(MM.deallocate(std::move(FA1)));
  cantFail(MM.deallocate(std::move(FAP)));
}
