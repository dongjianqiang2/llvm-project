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
  gEJitVpState.losslessMode.storeRelaxed(0);
  gEJitVpState.losslessFailed.storeRelaxed(0);
  gEJitVpState.armed.storeRelaxed(0);
  for (uint32_t c = 0; c < kEJitVpMaxCores; ++c) {
    gEJitVpState.shards[c].generation.storeRelaxed(0);
    gEJitVpState.shards[c].writers[0].storeRelaxed(0);
    gEJitVpState.shards[c].writers[1].storeRelaxed(0);
    for (uint32_t h = 0; h < 2; ++h) {
      gEJitVpState.shards[c].payload[h].updateBusy.storeRelaxed(0);
      for (uint32_t s = 0; s < kEJitVpSitesPerCore; ++s) {
        EJitVpSite &site = gEJitVpState.shards[c].payload[h].sites[s];
        site.siteKey.storeRelaxed(0);
        site.total.storeRelaxed(0);
        for (uint32_t i = 0; i < kEJitVpK; ++i) {
          site.cand[i].value.storeRelaxed(0);
          site.cand[i].count.storeRelaxed(0);
        }
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

TEST_F(VpCollectorTest, StrictSitesPreserveCollidingKeys) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetLosslessMode(true);
  ejitVpSetArmed(true);
  EJitCoreId::setCurrentForTest(0);
  for (uint32_t i = 1; i <= kEJitVpSitesPerCore; ++i)
    ejit_vp_record_scalar(i, 0, i + 100);
  EXPECT_FALSE(ejitVpLosslessFailed());
  std::vector<EJitVpSiteSample> samples;
  ASSERT_TRUE(ejitVpTakeSnapshot(samples));
  EXPECT_EQ(samples.size(), kEJitVpSitesPerCore);
  for (const auto &sample : samples)
    EXPECT_EQ(sample.total, 1u);
}

TEST_F(VpCollectorTest, StrictFullTableFailsRatherThanDisplacingSites) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetLosslessMode(true);
  ejitVpSetArmed(true);
  EJitCoreId::setCurrentForTest(0);
  for (uint32_t i = 1; i <= kEJitVpSitesPerCore + 1; ++i)
    ejit_vp_record_scalar(i, 0, i + 100);
  EXPECT_TRUE(ejitVpLosslessFailed());
}

TEST_F(VpCollectorTest, StrictPreemptedWriterFailsWithoutSpinning) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetLosslessMode(true);
  ejitVpSetArmed(true);
  EJitCoreId::setCurrentForTest(0);
  auto &shard = gEJitVpState.shards[0];
  shard.payload[shard.generation.loadAcquire() & 1].updateBusy.storeRelaxed(1);
  ejit_vp_record_scalar(1, 0, 123);
  EXPECT_TRUE(ejitVpLosslessFailed());
  EXPECT_EQ(shard.writers[0].loadAcquire(), 0u);
  shard.payload[0].updateBusy.storeRelaxed(0);
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
  EJitVpSite &site =
      shard.payload[0].sites[key & (kEJitVpSitesPerCore - 1u)];
  site.siteKey.storeRelaxed(key);
  site.total.storeRelaxed(1);
  site.cand[0].value.storeRelaxed(77);
  site.cand[0].count.storeRelaxed(1);

  // Model a producer that remains registered beyond the bounded drain.
  shard.writers[0].storeRelaxed(1);
  std::vector<EJitVpSiteSample> snap;
  ASSERT_TRUE(ejitVpTakeSnapshot(snap));
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
    if (!ejitVpTakeSnapshot(snap) || snap.size() != 1)
      return std::optional<EJitVpSiteSample>();
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
  EXPECT_LE(kEJitVpPerCoreBytes, kEJitVpCacheLine + 2u * kEJitVpSitesPerCore *
                                                        8u *
                                                        (2u + 2u * kEJitVpK));
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

TEST_F(VpCollectorTest,
       GenerationResetRequiresDrainedWritersAndClearsBothHalves) {
  ASSERT_TRUE(ejitVpEnsureInitialized());
  ejitVpSetArmed(true);
  ejit_vp_record_scalar(123, 0, 7);
  gEJitVpState.shards[0].writers[0].storeRelaxed(1);
  EXPECT_FALSE(ejitVpResetDrained());
  gEJitVpState.shards[0].writers[0].storeRelaxed(0);
  EXPECT_TRUE(ejitVpResetDrained());
  std::vector<EJitVpSiteSample> samples;
  ASSERT_TRUE(ejitVpTakeSnapshot(samples));
  EXPECT_TRUE(samples.empty());
}
