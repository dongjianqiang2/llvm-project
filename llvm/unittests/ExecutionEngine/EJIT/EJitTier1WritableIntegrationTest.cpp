//===-- EJitTier1WritableIntegrationTest.cpp - Tier-1 RW end-to-end -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  End-to-end integration test for cross-core runtime-writable (Online-PGO
//  Tier-1) preparation. It deliberately spans BOTH real components changed by
//  this fix in one test binary:
//
//    1. The real EJitCodePoolManager + EJitCodePoolMemoryManager JITLink path.
//    2. The real EJitSharedTaskPool cross-core cache + peer preparation.
//
//  It drives them with a JITLink LinkGraph whose layout mirrors a real Tier-1
//  instrumented function: an executable `__text` (R+X) segment plus a
//  runtime-writable `__llvm_prf_cnts` (R+W) segment holding the `__profc_`
//  counters that the instrumented body updates with an atomicrmw at runtime.
//
//  Scope note (why not real machine code): producing genuine Tier-1 machine
//  code (with the actual `atomicrmw` on `__profc_`) requires the full
//  Clang/ORC/PGO compile pipeline, which is not linkable into a lightweight
//  host unit target. JITLink itself operates on a linkable graph, not IR, so
//  the graph here carries the exact SEGMENT layout (R+X code disjoint from R+W
//  counters, page-aligned) that the code pool and the peer preparation actually
//  depend on. The load-bearing invariant a peer relies on — that every
//  `__profc_` address lands inside a published runtime-writable range that the
//  peer makes writable BEFORE executing — is asserted directly against the real
//  finalize/findRange output and the real peer preparation callbacks.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodePoolMemoryManager.h"
#include "llvm/ExecutionEngine/EJIT/EJitCodeRange.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedPlatform.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPoolState.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/Support/Error.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"

#include <cstdlib>
#include <memory>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::ejit;
using namespace llvm::jitlink;

namespace {

constexpr size_t kTwoMiB = static_cast<size_t>(2) * 1024 * 1024;
constexpr size_t kFourKiB = static_cast<size_t>(4) * 1024;

// Content bytes referenced by the graph blocks (must outlive the graph).
const char kCodeBytes[128] = {0};
const char kCounterBytes[16] = {0};

//===----------------------------------------------------------------------===//
// Owner-core code pool backend: a fixed RX 4K-seal pool. rawAlloc hands out a
// 2MiB-aligned slab; enable_rw/seal/split are logged so the owner path is real.
//===----------------------------------------------------------------------===//
struct OwnerPoolBackend {
  std::vector<void *> Origs;
  size_t SealCalls = 0;
  size_t RwEnableCalls = 0;

  ~OwnerPoolBackend() {
    for (void *P : Origs)
      std::free(P);
  }
  void *rawAlloc(size_t Bytes) {
    void *Base = nullptr;
    if (posix_memalign(&Base, kTwoMiB, Bytes + kTwoMiB) != 0)
      return nullptr;
    Origs.push_back(Base);
    return Base; // already 2MiB-aligned
  }
  unsigned split(void *, size_t) { return 0; }
  unsigned seal(void *) {
    ++SealCalls;
    return 0;
  }
  unsigned enableRw(void *) {
    ++RwEnableCalls;
    return 0;
  }
};

EJitCodePoolManager::Options fixedRxPoolOpts() {
  EJitCodePoolManager::Options O;
  O.poolSize = kTwoMiB;
  O.poolAlign = kTwoMiB;
  O.minCodeAlign = 64;
  O.fourKSeal = true;
  O.sealPageSize = kFourKiB;
  O.needsEnableRw = true; // fixed RX code-segment placement
  return O;
}

// Build a Tier-1-shaped graph: a `__text` (R+X) code block and a
// `__llvm_prf_cnts` (R+W) counter block (the `__profc_` data the instrumented
// body updates). Distinct AllocGroups => the memory manager lays them on
// separate, page-aligned segments (exec disjoint from writable).
std::unique_ptr<LinkGraph> makeTier1Graph(uint64_t CodeVAddr,
                                          uint64_t CounterVAddr) {
  auto G = std::make_unique<LinkGraph>(
      "tier1", std::make_shared<orc::SymbolStringPool>(),
      Triple("x86_64-unknown-linux-gnu"), SubtargetFeatures(),
      getGenericEdgeKindName);
  auto &Text =
      G->createSection("__text", orc::MemProt::Read | orc::MemProt::Exec);
  G->createContentBlock(Text, ArrayRef<char>(kCodeBytes, sizeof(kCodeBytes)),
                        orc::ExecutorAddr(CodeVAddr), 16, 0);
  auto &Cnts = G->createSection("__llvm_prf_cnts",
                                orc::MemProt::Read | orc::MemProt::Write);
  G->createContentBlock(Cnts,
                        ArrayRef<char>(kCounterBytes, sizeof(kCounterBytes)),
                        orc::ExecutorAddr(CounterVAddr), 8, 0);
  return G;
}

void *blockAddrByExec(LinkGraph &G, bool WantExec) {
  for (Block *B : G.blocks()) {
    bool IsExec = (B->getSection().getMemProt() & orc::MemProt::Exec) !=
                  orc::MemProt::None;
    if (IsExec == WantExec)
      return B->getAddress().toPtr<void *>();
  }
  return nullptr;
}

//===----------------------------------------------------------------------===//
// Peer-core preparation backend for the shared taskpool. A single ordered event
// log records enable_rw ('r') and enable_ex/seal ('e') calls so the test can
// prove the RW-before-EX ordering across the real peer path.
//===----------------------------------------------------------------------===//
struct PeerLog {
  std::vector<std::pair<char, uintptr_t>> events; // ('r'|'e', pageVA)
  std::vector<std::pair<uintptr_t, uint32_t>> splits;
};
bool peerSplit(void *ctx, uintptr_t poolBase, uint64_t) {
  static_cast<PeerLog *>(ctx)->splits.push_back(
      {poolBase, EJitCoreId::current()});
  return true;
}
bool peerEnableRw(void *ctx, uintptr_t pageVA) {
  static_cast<PeerLog *>(ctx)->events.push_back({'r', pageVA});
  return true;
}
bool peerSeal(void *ctx, uintptr_t pageVA) {
  static_cast<PeerLog *>(ctx)->events.push_back({'e', pageVA});
  return true;
}

// Code-range provider: hand the shared cache the REAL EJitCompiledCodeInfo the
// code pool produced for this function.
struct RangeProvider {
  EJitCompiledCodeInfo info{};
};
bool provideRange(void *ctx, const void *fnPtr, EJitCompiledCodeInfo *out) {
  *out = static_cast<RangeProvider *>(ctx)->info;
  out->fnPtr = const_cast<void *>(fnPtr);
  return true;
}

// Compiler that returns the real code entry pointer for the function.
struct FixedCompiler {
  void *fn = nullptr;
};
bool compileFixed(void *ctx, const EJitCompileRequest &, void **outFn) {
  *outFn = static_cast<FixedCompiler *>(ctx)->fn;
  return *outFn != nullptr;
}

} // namespace

// Full chain: real code-pool finalize -> findRange (with __profc_ membership)
// -> shared publish -> peer lookup makes the counter pages writable (enable_rw)
// BEFORE sealing the code pages (enable_ex), returns the fnPtr and memoizes the
// core; a second peer hit performs no permission callback.
TEST(EJitTier1WritableIntegration, PeerEnablesRwForProfcThenExecutes) {
  // --- Owner: finalize a Tier-1-shaped allocation through the real code pool.
  OwnerPoolBackend Be;
  EJitCodePoolManager Pool(
      fixedRxPoolOpts(), [&Be](size_t N) { return Be.rawAlloc(N); },
      [&Be](void *V) { return Be.seal(V); },
      [&Be](void *B, size_t S) { return Be.split(B, S); },
      [&Be](void *V) { return Be.enableRw(V); });
  EJitCodePoolMemoryManager MM(Pool, kFourKiB);

  auto G = makeTier1Graph(0x1000, 0x2000);
  auto IFA = cantFail(MM.allocate(nullptr, *G));
  void *CodeAddr = blockAddrByExec(*G, /*WantExec=*/true);
  void *ProfcAddr = blockAddrByExec(*G, /*WantExec=*/false);
  ASSERT_NE(CodeAddr, nullptr);
  ASSERT_NE(ProfcAddr, nullptr);
  // The owner made the whole slab writable before writing (enable_rw), so the
  // counter pages are RW on the owner.
  EXPECT_GT(Be.RwEnableCalls, 0u);
  auto FA = cantFail(IFA->finalize());

  // --- findRange returns the real writable range, and __profc_ lies inside it.
  EJitCompiledCodeInfo Info{};
  ASSERT_TRUE(Pool.findRange(CodeAddr, Info));
  ASSERT_GE(Info.writableCount, 1u);
  EXPECT_EQ(Info.requiresPeerEnableRw, 1u); // fixed RX pool
  uintptr_t Profc = reinterpret_cast<uintptr_t>(ProfcAddr);
  bool ProfcInRange = false;
  for (uint32_t i = 0; i < Info.writableCount; ++i) {
    uintptr_t A = Info.writableRanges[i].addr;
    uint64_t S = Info.writableRanges[i].size;
    if (Profc >= A && Profc < A + S)
      ProfcInRange = true;
  }
  EXPECT_TRUE(ProfcInRange) << "__profc_ address must fall in a writable range";

  // The counter page must be page-disjoint from the code page (no RWX).
  auto pageDown = [](uintptr_t A) {
    return A & ~static_cast<uintptr_t>(kFourKiB - 1);
  };
  EXPECT_NE(pageDown(reinterpret_cast<uintptr_t>(CodeAddr)), pageDown(Profc));

  // --- Shared taskpool: publish the real info, then a peer prepares it.
  auto state = std::make_unique<EJitSharedTaskPoolState>();
  EJitCoreId::resetForTest();
  EJitCoreId::setCurrentForTest(0);

  RangeProvider RP;
  RP.info = Info;
  FixedCompiler FC;
  FC.fn = CodeAddr; // real entry pointer
  PeerLog PL;

  EJitSharedTaskPool owner;
  owner.bind(state.get());
  owner.setCompiler(&compileFixed, &FC);
  owner.setMode(EJitCompileMode::Async);
  owner.setCodeSharingEnabled(true);
  owner.setSealMode(true);
  owner.setCodeRangeProvider(&provideRange, &RP);
  owner.setSplitPoolCallback(&peerSplit, &PL);
  owner.setSealPageCallback(&peerSeal, &PL);
  owner.setEnableRwPageCallback(&peerEnableRw, &PL);
  ASSERT_EQ(owner.init(), EJitSharedTaskPool::InitResult::BecameOwner);

  // Owner compile + publish (async: enqueue then poll).
  ASSERT_EQ(owner.compileOrGet(1, nullptr, 0, CodeAddr).status,
            EJitCompileOrGetStatus::EnqueuedPending);
  ASSERT_TRUE(owner.pollOne());

  // --- Peer core first touch.
  EJitCoreId::setCurrentForTest(3);
  auto hit = owner.compileOrGet(1, nullptr, 0, CodeAddr);
  ASSERT_EQ(hit.status, EJitCompileOrGetStatus::CacheHit);
  EXPECT_EQ(hit.fnPtr, CodeAddr);
  owner.releaseRead(hit.bucketIndex);

  // enable_rw happened (>=1), enable_ex happened (>=1), and EVERY enable_rw
  // event precedes EVERY enable_ex event (RW prepared before the code seal).
  size_t rwCount = 0, exCount = 0, lastRw = 0, firstEx = PL.events.size();
  for (size_t i = 0; i < PL.events.size(); ++i) {
    if (PL.events[i].first == 'r') {
      ++rwCount;
      lastRw = i;
    } else {
      ++exCount;
      if (i < firstEx)
        firstEx = i;
    }
  }
  ASSERT_GT(rwCount, 0u) << "peer must enable_rw the __profc_ page(s)";
  ASSERT_GT(exCount, 0u) << "peer must enable_ex the code page(s)";
  EXPECT_LT(lastRw, firstEx) << "all enable_rw must precede all enable_ex";

  // The enable_rw event covered the __profc_ page.
  bool rwCoveredProfc = false;
  for (auto &E : PL.events)
    if (E.first == 'r' && E.second == pageDown(Profc))
      rwCoveredProfc = true;
  EXPECT_TRUE(rwCoveredProfc);

  // Peer prepared bit is set.
  bool foundReady = false;
  for (uint32_t b = 0; b < kEJitSharedCacheBuckets && !foundReady; ++b)
    for (uint32_t s = 0; s < kEJitSharedCacheSlots; ++s) {
      EJitSharedCacheSlot &Slot = state->buckets[b].slots[s];
      if (Slot.state.loadAcquire() ==
              static_cast<uint32_t>(EJitSharedSlotState::Ready) &&
          Slot.funcIndex == 1) {
        EXPECT_NE(Slot.executableCoreMask.loadRelaxed() & (uint64_t{1} << 3),
                  0u);
        foundReady = true;
        break;
      }
    }
  EXPECT_TRUE(foundReady);

  // --- Second peer hit on the same core: NO further permission callback.
  size_t eventsAfterFirst = PL.events.size();
  auto hit2 = owner.compileOrGet(1, nullptr, 0, CodeAddr);
  ASSERT_EQ(hit2.status, EJitCompileOrGetStatus::CacheHit);
  owner.releaseRead(hit2.bucketIndex);
  EXPECT_EQ(PL.events.size(), eventsAfterFirst) << "memoized: no new callbacks";

  cantFail(MM.deallocate(std::move(FA)));
  EJitCoreId::resetForTest();
}
