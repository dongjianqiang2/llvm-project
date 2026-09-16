//===-- EJitVpCollectorTest.cpp - value-collector unit tests
//---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Unit tests for the per-core K-way heavy-hitter value collector and the
//  double-buffered generation release/acquire snapshot protocol
//  (EJIT_VALUE_PROFILE.md §3). The collector sources are compiled directly
//  here (with the VP macros) like EJITSharedTaskPoolTests, so this target links
//  only LLVMSupport and never pulls in OrcJIT/LLVMEJIT.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#include "gtest/gtest.h"
#include <cstring>
#include <optional>
#ifndef EJIT_FREESTANDING
#include <atomic>
#include <thread>
#include <vector>
#endif

using namespace llvm;
using namespace llvm::ejit;

namespace {

// The collector blob is a process-global; reset it field-by-field between
// tests (trivially default constructible, but not trivially copyable because
// EJitAtomic deletes its copy - so no memcpy).
void resetVpStateForTest() {
  gEJitVpState.magic.storeRelaxed(0);
  gEJitVpState.abiVersion.storeRelaxed(0);
  gEJitVpState.structSize.storeRelaxed(0);
  gEJitVpState.headerReserved = 0;
  gEJitVpState.armed.storeRelaxed(0);
  gEJitVpState.headerAlignPad = 0;
  gEJitVpState.lastIssuedSessionId.storeRelaxed(0);
  for (uint32_t i = 0; i < kEJitVpMaxSessions; ++i) {
    gEJitVpState.sessions[i].samplingSessionId.storeRelaxed(0);
    gEJitVpState.sessions[i].requestAttemptToken.storeRelaxed(0);
    gEJitVpState.sessions[i].gate.storeRelaxed(0);
    gEJitVpState.sessions[i].retireState.storeRelaxed(0);
    gEJitVpState.sessions[i].reserved = 0;
  }
  for (uint32_t i = 0; i < kEJitVpMaxProfdBindings; ++i) {
    gEJitVpState.profdBindings[i].sequence.storeRelaxed(0);
    gEJitVpState.profdBindings[i].profdAddr.storeRelaxed(0);
    gEJitVpState.profdBindings[i].samplingSessionId.storeRelaxed(0);
  }
  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    gEJitVpState.shards[c].generation.storeRelaxed(0);
    gEJitVpState.shards[c].writers[0].storeRelaxed(0);
    gEJitVpState.shards[c].writers[1].storeRelaxed(0);
    for (uint32_t h = 0; h < 2; ++h)
      for (uint32_t s = 0; s < kEJitVpSitesPerCore; ++s) {
        EJitVpSite &site = gEJitVpState.shards[c].payload[h].sites[s];
        site.siteKey.storeRelaxed(0);
        site.samplingSessionId.storeRelaxed(0);
        site.functionIdentity.storeRelaxed(0);
        site.kind.storeRelaxed(0);
        site.siteIndex.storeRelaxed(0);
        site.total.storeRelaxed(0);
        for (uint32_t i = 0; i < kEJitVpK; ++i) {
          site.cand[i].value.storeRelaxed(0);
          site.cand[i].count.storeRelaxed(0);
        }
      }
  }
}

class VpCollectorTest : public ::testing::Test {
protected:
  void SetUp() override {
    resetVpStateForTest();
    EJitCoreId::resetForTest();
  }
  void TearDown() override { EJitCoreId::resetForTest(); }
};

/// Reconstruct a fake __profd_ buffer (64-bit layout, InstrProfData.inc):
/// FuncHash@8, NumCounters@48, NumValueSites[3] (uint16) @52/54/56.
struct alignas(8) FakeProfd {
  uint64_t nameRef = 0;
  uint64_t funcHash = 0;
  uint64_t counterPtr = 0;
  uint64_t bitmapPtr = 0;
  uint64_t functionPtr = 0;
  uint64_t values = 0;
  uint32_t numCounters = 0;
  uint16_t numValueSites[3] = {0, 0, 0};
  uint32_t numBitmapBytes = 0;
};

} // namespace

TEST_F(VpCollectorTest, ArmedGate) {
  // Not armed: records drop even after initialization.
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejit_vp_record_scalar(0xABCDu, 0, 42);
  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  EXPECT_TRUE(snap.empty());

  // Armed: records land.
  ejitVpSetArmed(true);
  ejit_vp_record_scalar(0xABCDu, 0, 42);
  snap.clear();
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  EXPECT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].siteKey, ejitVpSiteKey(0xABCDu, kEJitVpScalar, 0));
  EXPECT_EQ(snap[0].total, 1u);
  EXPECT_EQ(snap[0].values[0], 42u);
  EXPECT_EQ(snap[0].counts[0], 1u);
}

TEST_F(VpCollectorTest, KWayHeavyHitter) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  // Value A dominates, B second, C appears once and must displace the
  // lowest-count candidate (B) - K-way eviction, never unbounded growth.
  for (int i = 0; i < 100; ++i)
    ejit_vp_record_scalar(0x1, 0, 0xAAA);
  for (int i = 0; i < 50; ++i)
    ejit_vp_record_scalar(0x1, 0, 0xBBB);
  ejit_vp_record_scalar(0x1, 0, 0xCCC);

  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].total, 151u);

  // Counts: A=100, C=1 (B was displaced). Order of candidates is not
  // guaranteed, so find by value.
  bool sawA = false, sawC = false;
  for (uint32_t i = 0; i < kEJitVpK; ++i) {
    if (snap[0].values[i] == 0xAAA) {
      sawA = true;
      EXPECT_EQ(snap[0].counts[i], 100u);
    }
    if (snap[0].values[i] == 0xCCC) {
      sawC = true;
      EXPECT_EQ(snap[0].counts[i], 1u);
    }
  }
  EXPECT_TRUE(sawA && sawC);
}

TEST_F(VpCollectorTest, PerCoreIsolationAndMerge) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  // Two simulated cores record the same site key with different values; the
  // snapshot must carry BOTH shards' samples (merge sums them later).
  EJitCoreId::setCurrentForTest(0);
  for (int i = 0; i < 10; ++i)
    ejit_vp_record_scalar(0x77u, 3, 11);
  EJitCoreId::setCurrentForTest(1);
  for (int i = 0; i < 10; ++i)
    ejit_vp_record_scalar(0x77u, 3, 22);
  EJitCoreId::resetForTest();

  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  const uint64_t key = ejitVpSiteKey(0x77u, kEJitVpScalar, 3);
  uint64_t total = 0;
  bool saw11 = false, saw22 = false;
  for (const EJitVpSiteSample &s : snap) {
    if (s.siteKey != key)
      continue;
    total += s.total;
    for (uint32_t i = 0; i < kEJitVpK; ++i) {
      if (s.values[i] == 11 && s.counts[i] == 10)
        saw11 = true;
      if (s.values[i] == 22 && s.counts[i] == 10)
        saw22 = true;
    }
  }
  EXPECT_EQ(total, 20u);
  EXPECT_TRUE(saw11);
  EXPECT_TRUE(saw22);
}

TEST_F(VpCollectorTest, DoubleBufferCoversDisjointWindows) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  ejit_vp_record_scalar(0x5u, 0, 1);
  ejit_vp_record_scalar(0x5u, 0, 1);

  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap)); // flips gen -> producers write half 1
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].total, 2u);

  // New window only: the retired half was cleared, the other half takes the
  // new records.
  ejit_vp_record_scalar(0x5u, 0, 7);
  snap.clear();
  ASSERT_TRUE(ejitVpTakeSnapshot(snap)); // reads half 1, flips gen again
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].total, 1u);
  EXPECT_EQ(snap[0].values[0], 7u);
}

TEST_F(VpCollectorTest, BusyRetiredHalfIsRecoveredNextSnapshot) {
  ASSERT_TRUE(ejitVpEnsureInitialized());

  EJitVpShard &shard = gEJitVpState.shards[0];
  const uint64_t key = ejitVpSiteKey(0x51u, kEJitVpScalar, 0);
  EJitVpSite &site = shard.payload[0].sites[key & (kEJitVpSitesPerCore - 1u)];
  site.siteKey.storeRelaxed(key);
  site.samplingSessionId.storeRelaxed(1);
  site.functionIdentity.storeRelaxed(0x51u);
  site.kind.storeRelaxed(kEJitVpScalar);
  site.siteIndex.storeRelaxed(0);
  site.total.storeRelaxed(1);
  site.cand[0].value.storeRelaxed(77);
  site.cand[0].count.storeRelaxed(1);

  // Model a producer that remains registered beyond the bounded drain.
  shard.writers[0].storeRelaxed(1);
  std::vector<EJitVpSiteSample> snap;
  EXPECT_FALSE(ejitVpTakeSnapshot(snap));
  EXPECT_TRUE(snap.empty());

  // Recover the skipped half before reusing it once the producer leaves.
  shard.writers[0].storeRelease(0);
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].siteKey, key);
  EXPECT_EQ(snap[0].total, 1u);
  EXPECT_EQ(snap[0].values[0], 77u);
  EXPECT_EQ(snap[0].counts[0], 1u);
}

TEST_F(VpCollectorTest, ResetFunctionForgetsSites) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  ejit_vp_record_scalar(0x99u, 0, 5);
  ejit_vp_record_scalar(0x99u, 1, 6);

  const EJitVpKindSiteCount counts[] = {{kEJitVpScalar, 2}};
  ejitVpResetFunction(0x99u, ArrayRef<EJitVpKindSiteCount>(counts));

  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  EXPECT_TRUE(snap.empty());
}

TEST_F(VpCollectorTest, InstrumentTargetFlatIndexSplit) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);

  FakeProfd profd;
  profd.nameRef = 0xBEEFu;
  profd.funcHash = 0xF00Du;
  profd.numCounters = 1;
  profd.numValueSites[0] = 2; // 2 indirect-call sites
  profd.numValueSites[1] = 1; // 1 memop site
  profd.numValueSites[2] = 0;

  // Each site is verified in isolation (the direct-mapped table may displace
  // on slot collisions; the property under test is the flat-index split, not
  // slot placement). No gtest ASSERT inside the probe: it would break the
  // lambda's return type.
  auto probe = [&](uint32_t flat, uint64_t value) {
    resetVpStateForTest();
    if (!ejitVpEnsureInitialized())
      return std::optional<EJitVpSiteSample>();
    ejitVpSetArmed(true);
    __llvm_profile_instrument_target(value, &profd, flat);
    std::vector<EJitVpSiteSample> snap;
    if (!ejitVpTakeSnapshot(snap) || snap.size() != 1) {
      ADD_FAILURE() << "legacy official snapshot size=" << snap.size();
      return std::optional<EJitVpSiteSample>();
    }
    return std::optional<EJitVpSiteSample>(snap[0]);
  };

  std::optional<EJitVpSiteSample> s0 = probe(0, 0xA00D0);
  ASSERT_TRUE(s0.has_value());
  EXPECT_EQ(s0->siteKey, ejitVpSiteKey(0xBEEFu, kEJitVpIndirectCall, 0));
  EXPECT_EQ(s0->values[0], 0xA00D0u);

  std::optional<EJitVpSiteSample> s1 = probe(1, 0xA00D1);
  ASSERT_TRUE(s1.has_value());
  EXPECT_EQ(s1->siteKey, ejitVpSiteKey(0xBEEFu, kEJitVpIndirectCall, 1));
  EXPECT_EQ(s1->values[0], 0xA00D1u);

  std::optional<EJitVpSiteSample> sMem = probe(2, 64);
  ASSERT_TRUE(sMem.has_value());
  EXPECT_EQ(sMem->siteKey, ejitVpSiteKey(0xBEEFu, kEJitVpMemOpSize, 0));
  EXPECT_EQ(sMem->values[0], 64u);

  // Out of range (would be a vtable site): must be dropped.
  resetVpStateForTest();
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  __llvm_profile_instrument_target(0xBAD, &profd, 3);
  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  EXPECT_TRUE(snap.empty());
}

TEST_F(VpCollectorTest, ExactSessionsSnapshotIndependently) {
  ASSERT_TRUE(ejitVpBeginSession(101));
  ASSERT_TRUE(ejitVpBeginSession(202));
  ejit_vp_record_scalar_session(101, 0x77u, 0, 11);
  ejit_vp_record_scalar_session(202, 0x77u, 0, 22);

  std::vector<EJitVpSiteSample> first;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(101, first));
  ASSERT_EQ(first.size(), 1u);
  EXPECT_EQ(first[0].samplingSessionId, 101u);
  EXPECT_EQ(first[0].values[0], 11u);

  std::vector<EJitVpSiteSample> second;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(202, second));
  ASSERT_EQ(second.size(), 1u);
  EXPECT_EQ(second[0].samplingSessionId, 202u);
  EXPECT_EQ(second[0].values[0], 22u);
}

TEST_F(VpCollectorTest, EndedSessionDropsLateWritesWithoutStoppingPeer) {
  ASSERT_TRUE(ejitVpBeginSession(301));
  ASSERT_TRUE(ejitVpBeginSession(302));
  ejit_vp_record_scalar_session(301, 0x88u, 0, 1);
  ejitVpEndSession(301);
  ejit_vp_record_scalar_session(301, 0x88u, 0, 99);
  ejit_vp_record_scalar_session(302, 0x88u, 0, 7);

  std::vector<EJitVpSiteSample> old;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(301, old));
  ASSERT_EQ(old.size(), 1u);
  EXPECT_EQ(old[0].total, 1u);
  EXPECT_EQ(old[0].values[0], 1u);

  std::vector<EJitVpSiteSample> peer;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(302, peer));
  ASSERT_EQ(peer.size(), 1u);
  EXPECT_EQ(peer[0].values[0], 7u);
}

TEST_F(VpCollectorTest, ProfileDataBindingRoutesOfficialKindsToSession) {
  FakeProfd profd;
  profd.nameRef = 0xBEEFu;
  profd.numValueSites[0] = 1;
  profd.numValueSites[1] = 1;
  ASSERT_TRUE(ejitVpBeginSession(401));
  ASSERT_TRUE(ejitVpBindProfileData(401, reinterpret_cast<uintptr_t>(&profd)));
  __llvm_profile_instrument_memop(0xCAFEu, &profd, 0);
  __llvm_profile_instrument_memop(64, &profd, 1);
  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(401, snap));
  ASSERT_EQ(snap.size(), 2u);
  for (const EJitVpSiteSample &sample : snap)
    EXPECT_EQ(sample.samplingSessionId, 401u);
}

TEST_F(VpCollectorTest, SessionCapacityFailsWithoutReusingOldIdentity) {
  for (uint64_t i = 1; i <= kEJitVpMaxSessions; ++i) {
    ASSERT_TRUE(ejitVpBeginSession(500 + i));
    ejitVpEndSession(500 + i);
  }
  EXPECT_FALSE(ejitVpBeginSession(9999));
  // A late writer for the first retired identity cannot be redirected to 9999.
  ejit_vp_record_scalar_session(501, 1, 0, 42);
  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(501, snap));
  EXPECT_TRUE(snap.empty());
}

TEST_F(VpCollectorTest, SequentialSessionsReuseRetiredSlots) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  for (uint64_t I = 0; I < 120; ++I) {
    const uint64_t SessionId = ejitVpCreateSession();
    ASSERT_NE(SessionId, 0u) << "sequential session " << I;
    ejit_vp_record_scalar_session(SessionId, 0x123456u, 0, I + 1);
    ejitVpEndSession(SessionId);
    std::vector<EJitVpSiteSample> Samples;
    ASSERT_TRUE(ejitVpTakeSessionSnapshot(SessionId, Samples));
    ASSERT_EQ(Samples.size(), 1u);
    EXPECT_EQ(Samples[0].samplingSessionId, SessionId);
  }
}

TEST_F(VpCollectorTest, FourConcurrentSessionsCanRunInRepeatedWaves) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  for (uint64_t Wave = 0; Wave < 30; ++Wave) {
    uint64_t Sessions[4] = {};
    for (uint32_t I = 0; I < 4; ++I) {
      Sessions[I] = ejitVpCreateSession();
      ASSERT_NE(Sessions[I], 0u);
      EJitCoreId::setCurrentForTest(I);
      ejit_vp_record_scalar_session(Sessions[I], 0xABC000u + Sessions[I], 0,
                                    Sessions[I]);
    }
    EJitCoreId::resetForTest();
    for (uint64_t SessionId : Sessions) {
      ejitVpEndSession(SessionId);
      std::vector<EJitVpSiteSample> Samples;
      ASSERT_TRUE(ejitVpTakeSessionSnapshot(SessionId, Samples));
      ASSERT_EQ(Samples.size(), 1u);
      EXPECT_EQ(Samples[0].samplingSessionId, SessionId);
    }
  }
}

TEST_F(VpCollectorTest, RetiredSessionCannotReopenOrAcceptLateWrites) {
  ASSERT_TRUE(ejitVpBeginSession(101));
  ejit_vp_record_scalar_session(101, 0x123456u, 0, 11);
  ejitVpEndSession(101);
  std::vector<EJitVpSiteSample> First;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(101, First));
  ASSERT_FALSE(First.empty());

  EXPECT_FALSE(ejitVpBeginSession(101));
  ejit_vp_record_scalar_session(101, 0x123456u, 0, 999);
  std::vector<EJitVpSiteSample> Late;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(101, Late));
  EXPECT_TRUE(Late.empty());
}

TEST_F(VpCollectorTest, LegacyRecordsStayOutsideProductionSessionOne) {
  ASSERT_TRUE(ejitVpBeginSession(1));
  ejitVpSetArmed(true);
  ejit_vp_record_scalar(0x123456u, 0, 999);
  std::vector<EJitVpSiteSample> Production;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(1, Production));
  EXPECT_TRUE(Production.empty());

  std::vector<EJitVpSiteSample> Legacy;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(kEJitVpLegacySessionId, Legacy));
  ASSERT_EQ(Legacy.size(), 1u);
  EXPECT_EQ(Legacy[0].samplingSessionId, kEJitVpLegacySessionId);
}

TEST_F(VpCollectorTest, RetiringSessionReleasesProfileBindings) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  FakeProfd Profds[kEJitVpMaxProfdBindings + 1] = {};
  const uint64_t FirstSession = ejitVpCreateSession();
  ASSERT_NE(FirstSession, 0u);
  for (uint32_t I = 0; I < kEJitVpMaxProfdBindings; ++I)
    ASSERT_TRUE(ejitVpBindProfileData(FirstSession,
                                      reinterpret_cast<uintptr_t>(&Profds[I])));
  EXPECT_FALSE(ejitVpBindProfileData(
      FirstSession,
      reinterpret_cast<uintptr_t>(&Profds[kEJitVpMaxProfdBindings])));
  ejitVpEndSession(FirstSession);
  std::vector<EJitVpSiteSample> Discarded;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(FirstSession, Discarded));

  const uint64_t NextSession = ejitVpCreateSession();
  ASSERT_NE(NextSession, 0u);
  EXPECT_TRUE(ejitVpBindProfileData(
      NextSession,
      reinterpret_cast<uintptr_t>(&Profds[kEJitVpMaxProfdBindings])));
}

TEST_F(VpCollectorTest, FailedCancellationRetirementIsServicedOnPressure) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  gEJitVpState.shards[1].writers[0].storeRelaxed(1);
  for (unsigned I = 0; I < kEJitVpMaxSessions; ++I) {
    const uint64_t SessionId = ejitVpCreateSession();
    ASSERT_NE(SessionId, 0u);
    ejitVpEndSession(SessionId);
    std::vector<EJitVpSiteSample> Discarded;
    EXPECT_FALSE(ejitVpTakeSessionSnapshot(SessionId, Discarded));
  }
  gEJitVpState.shards[1].writers[0].storeRelease(0);
  EXPECT_NE(ejitVpCreateSession(), 0u);
}

TEST_F(VpCollectorTest, PreservedFreezeIsNotScavengedUnderPressure) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  uint64_t Preserved = ejitVpCreateSession();
  ASSERT_NE(Preserved, 0u);
  ejitVpEndSession(Preserved, true);
  for (unsigned I = 1; I < kEJitVpMaxSessions; ++I)
    ASSERT_NE(ejitVpCreateSession(), 0u);
  EXPECT_EQ(ejitVpCreateSession(), 0u);
  std::vector<EJitVpSiteSample> Samples;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(Preserved, Samples));
  EXPECT_NE(ejitVpCreateSession(), 0u);
}

TEST_F(VpCollectorTest, RetryAccumulatorPreservesPartiallyDrainedSamples) {
  const uint64_t SessionId = ejitVpCreateSession();
  ASSERT_NE(SessionId, 0u);
  EJitCoreId::setCurrentForTest(0);
  ejit_vp_record_scalar_session(SessionId, 0x123456u, 0, 11);
  EJitCoreId::setCurrentForTest(1);
  ejit_vp_record_scalar_session(SessionId, 0x123456u, 0, 22);
  EJitCoreId::resetForTest();

  gEJitVpState.shards[1].writers[0].storeRelaxed(1);
  ejitVpEndSession(SessionId, true);
  std::vector<EJitVpSiteSample> Pending;
  EXPECT_FALSE(ejitVpTakeSessionSnapshot(SessionId, Pending));
  ASSERT_FALSE(Pending.empty());

  // CompileDriver retains this exact-session accumulator across a bounded T2
  // retry instead of destroying already-drained samples.
  gEJitVpState.shards[1].writers[0].storeRelease(0);
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(SessionId, Pending));
  uint64_t Total = 0;
  for (const EJitVpSiteSample &Sample : Pending)
    Total += Sample.total;
  EXPECT_EQ(Total, 2u);
}

TEST_F(VpCollectorTest, ExactAttemptCancellationClosesOnlyItsSession) {
  const uint64_t First = ejitVpCreateSession(1001);
  const uint64_t Second = ejitVpCreateSession(1002);
  ASSERT_NE(First, 0u);
  ASSERT_NE(Second, 0u);
  ejitVpCancelAttempt(1001);
  ejit_vp_record_scalar_session(First, 0xAAu, 0, 1);
  ejit_vp_record_scalar_session(Second, 0xBBu, 0, 2);

  std::vector<EJitVpSiteSample> FirstSamples;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(First, FirstSamples));
  EXPECT_TRUE(FirstSamples.empty());
  std::vector<EJitVpSiteSample> SecondSamples;
  ASSERT_TRUE(ejitVpTakeSessionSnapshot(Second, SecondSamples));
  ASSERT_EQ(SecondSamples.size(), 1u);
  EXPECT_EQ(SecondSamples[0].values[0], 2u);
}

TEST_F(VpCollectorTest, LateFreezeCannotUndoTerminalCancellation) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  gEJitVpState.shards[1].writers[0].storeRelaxed(1);
  for (uint64_t I = 0; I < kEJitVpMaxSessions; ++I) {
    const uint64_t SessionId = ejitVpCreateSession(9000 + I);
    ASSERT_NE(SessionId, 0u);
    ejitVpCancelAttempt(9000 + I);
    // Models an already-running T2 reaching its preserve request after cancel.
    ejitVpEndSession(SessionId, true);
    std::vector<EJitVpSiteSample> Pending;
    EXPECT_FALSE(ejitVpTakeSessionSnapshot(SessionId, Pending));
  }
  gEJitVpState.shards[1].writers[0].storeRelease(0);
  EXPECT_NE(ejitVpCreateSession(), 0u);
}

TEST_F(VpCollectorTest, PhysicalProducerCoreIdsFitDefaultShardRange) {
  ASSERT_GT(kEJitVpMaxCores, 20u);
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  EJitCoreId::setCurrentForTest(20);
  ejit_vp_record_scalar(0x2020u, 0, 100);

  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
  ASSERT_EQ(snap.size(), 1u);
  EXPECT_EQ(snap[0].siteKey, ejitVpSiteKey(0x2020u, kEJitVpScalar, 0));
  EXPECT_EQ(snap[0].values[0], 100u);
  EJitCoreId::resetForTest();
}

TEST_F(VpCollectorTest, AbiMismatchRefused) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  // Corrupt the struct size as a foreign build would present it.
  gEJitVpState.structSize.storeRelaxed(0x1234u);
  std::vector<EJitVpSiteSample> snap;
  EXPECT_FALSE(ejitVpTakeSnapshot(snap));
  // Records must drop at the ABI gate even when armed.
  ejitVpSetArmed(true);
  ejit_vp_record_scalar(1, 0, 2);
  // (no crash is the observable behavior; the blob is corrupted on purpose)
}

TEST_F(VpCollectorTest, MemoryBoundDocumented) {
  // The computable bound from the header must cover the actual layout.
  EXPECT_GE(kEJitVpPerCoreBytes, sizeof(EJitVpShard));
  EXPECT_EQ(kEJitVpPerCoreBytes,
            kEJitVpCacheLine + 2u * kEJitVpSitesPerCore * sizeof(EJitVpSite));
  EXPECT_EQ(kEJitVpTotalBytes, kEJitVpMaxCores * kEJitVpPerCoreBytes);
}

#ifndef EJIT_FREESTANDING
// Real-thread stress: eight producer threads hammer their own shards (one
// simulated core each) while the collector takes repeated snapshots from a
// ninth core. The snapshot protocol must survive the concurrency: totals are
// positive and bounded, and every observed candidate value is one the
// producers actually recorded (no torn/garbage values).
TEST_F(VpCollectorTest, ConcurrentProducersSnapshotStability) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  constexpr uint32_t kThreads = 8;
  std::atomic<bool> done{false};
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (uint32_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([t, &done]() {
      EJitCoreId::setCurrentForTest(t);
      while (!done.load(std::memory_order_relaxed)) {
        ejit_vp_record_scalar(0x42u, 0, 100 + (t % 3));
        ejit_vp_record_scalar(0x42u, 1, 7 + (t % 3));
      }
    });
  }
  EJitCoreId::setCurrentForTest(kThreads); // collector "core"
  const uint64_t key0 = ejitVpSiteKey(0x42u, kEJitVpScalar, 0);
  const uint64_t key1 = ejitVpSiteKey(0x42u, kEJitVpScalar, 1);
  // Producer threads start cold (the scheduler may not have run them when the
  // first snapshots retire empty halves), so keep snapshotting until at least
  // one round observes data, then verify every observed value is intact.
  bool sawData = false;
  for (int round = 0; round < 50 && !sawData; ++round) {
    std::vector<EJitVpSiteSample> snap;
    ASSERT_TRUE(ejitVpTakeSnapshot(snap));
    uint64_t total = 0;
    for (const EJitVpSiteSample &s : snap) {
      if (s.siteKey != key0 && s.siteKey != key1)
        continue;
      total += s.total;
      for (uint32_t i = 0; i < kEJitVpK; ++i)
        if (s.counts[i] != 0) {
          if (s.siteKey == key0) {
            EXPECT_GE(s.values[i], 100u);
            EXPECT_LE(s.values[i], 102u);
          } else {
            EXPECT_GE(s.values[i], 7u);
            EXPECT_LE(s.values[i], 9u);
          }
        }
    }
    if (total > 0)
      sawData = true;
  }
  EXPECT_TRUE(sawData);
  // One final round after data was observed: values must still be intact.
  {
    std::vector<EJitVpSiteSample> snap;
    ASSERT_TRUE(ejitVpTakeSnapshot(snap));
    for (const EJitVpSiteSample &s : snap)
      if (s.siteKey == key0 || s.siteKey == key1)
        for (uint32_t i = 0; i < kEJitVpK; ++i)
          if (s.counts[i] != 0) {
            if (s.siteKey == key0) {
              EXPECT_GE(s.values[i], 100u);
              EXPECT_LE(s.values[i], 102u);
            } else {
              EXPECT_GE(s.values[i], 7u);
              EXPECT_LE(s.values[i], 9u);
            }
          }
  }
  done.store(true);
  for (std::thread &th : threads)
    th.join();
  EJitCoreId::resetForTest();
}
#endif // !EJIT_FREESTANDING
