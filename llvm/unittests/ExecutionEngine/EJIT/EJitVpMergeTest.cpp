//===-- EJitVpMergeTest.cpp - value-profile merge unit tests --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Tests for the Tier-2 value-profile aggregation and profile synthesis
//  (EJIT_VALUE_PROFILE.md §5): address->MD5 verification, flat-site probing,
//  top-N/confidence reporting, and edge+value data round-tripping through the
//  official InstrProfWriter/IndexedInstrProfReader in ONE legal profile.
//  Compiled only under EJIT_SRE_PGO_VALUE_PROFILE (the APIs are macro-gated);
//  the translation unit is intentionally empty otherwise.
//
//===----------------------------------------------------------------------===//

#ifdef EJIT_SRE_PGO_VALUE_PROFILE

#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#include "llvm/ProfileData/InstrProf.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/MemoryBuffer.h"
#include "gtest/gtest.h"
#include <cstdint>

using namespace llvm;
using namespace llvm::ejit;

namespace {

/// Reconstruct a __profd_ buffer (64-bit layout, InstrProfData.inc):
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

EJitVpSiteSample
sampleOf(uint64_t siteKey, uint64_t total,
         std::initializer_list<std::pair<uint64_t, uint64_t>> valueCounts) {
  EJitVpSiteSample S;
  S.siteKey = siteKey;
  S.total = total;
  uint32_t i = 0;
  for (auto &VC : valueCounts) {
    if (i >= kEJitVpK)
      break; // test inputs never exceed K; keep the helper total
    S.values[i] = VC.first;
    S.counts[i] = VC.second;
    ++i;
  }
  return S;
}

} // namespace

TEST(EJitVpMerge, AggregationMapsVerifiedTargetsAndDropsUnknown) {
  constexpr uint64_t Hash = 0xABCDEF01u;
  constexpr uintptr_t KnownAddr = 0x4000u;
  constexpr uint64_t KnownMd5 = 0x1234567890u;

  PgoValueTarget targets[] = {{KnownAddr, KnownMd5}};
  PgoValueFunction funcs[] = {{Hash, /*pgoNameHash=*/Hash, /*numIcSites=*/1,
                               /*numMemSites=*/1,
                               /*numScalarSites=*/1}};
  EJitVpSiteSample samples[] = {
      // IC site 0: known target dominates, one unverified address must be
      // DROPPED (never written raw into the profile as if it were a hash).
      sampleOf(ejitVpSiteKey(Hash, kEJitVpIndirectCall, 0), 100,
               {{KnownAddr, 90}, {0xDEADBEEFu, 10}}),
      // Memop site 0: sizes pass through by value.
      sampleOf(ejitVpSiteKey(Hash, kEJitVpMemOpSize, 0), 50, {{64, 50}}),
      // Scalar site 0: top-1 = 100, total = 1000.
      sampleOf(ejitVpSiteKey(Hash, kEJitVpScalar, 0), 1000,
               {{100, 990}, {7, 10}}),
  };

  SmallVector<PgoValueSite, 4> icMem;
  SmallVector<PgoScalarSite, 4> scalars;
  ASSERT_TRUE(aggregateValueSamples(samples, funcs, targets, icMem, scalars));

  // IC site: exactly one value (the verified md5), unknown address dropped.
  ASSERT_EQ(icMem.size(), 2u);
  const PgoValueSite *ic = nullptr;
  const PgoValueSite *mem = nullptr;
  for (const PgoValueSite &S : icMem) {
    if (S.valueKind == IPVK_IndirectCallTarget)
      ic = &S;
    else if (S.valueKind == IPVK_MemOPSize)
      mem = &S;
  }
  ASSERT_TRUE(ic);
  ASSERT_EQ(ic->values.size(), 1u);
  EXPECT_EQ(ic->values[0].Value, KnownMd5);
  EXPECT_EQ(ic->values[0].Count, 90u);
  EXPECT_EQ(ic->siteIndex, 0u);

  // Memop site: raw size preserved.
  ASSERT_TRUE(mem);
  ASSERT_EQ(mem->values.size(), 1u);
  EXPECT_EQ(mem->values[0].Value, 64u);
  EXPECT_EQ(mem->values[0].Count, 50u);

  // Scalar site: measured distribution only (thresholds are caller policy).
  ASSERT_EQ(scalars.size(), 1u);
  EXPECT_EQ(scalars[0].funcHash, Hash);
  EXPECT_EQ(scalars[0].siteIndex, 0u);
  EXPECT_EQ(scalars[0].topValue, 100u);
  EXPECT_EQ(scalars[0].topCount, 990u);
  EXPECT_EQ(scalars[0].total, 1000u);
}

TEST(EJitVpMerge, AggregationRejectsOversizedInventory) {
  PgoValueFunction funcs[] = {{1, 1, /*numIcSites=*/1u << 20, 0, 0}};
  SmallVector<PgoValueSite, 1> icMem;
  SmallVector<PgoScalarSite, 1> scalars;
  EXPECT_FALSE(aggregateValueSamples({}, funcs, {}, icMem, scalars));
}

TEST(EJitVpMerge, SameCfgHashDoesNotAliasDifferentFunctions) {
  constexpr uint64_t SharedCfgHash = 0x515151u;
  constexpr uint64_t NameA = 0xAAAAu;
  constexpr uint64_t NameB = 0xBBBBu;
  PgoValueFunction funcs[] = {
      {SharedCfgHash, NameA, 0, 1, 0},
      {SharedCfgHash, NameB, 0, 1, 0},
  };
  EJitVpSiteSample samples[] = {
      sampleOf(ejitVpSiteKey(NameA, kEJitVpMemOpSize, 0), 100, {{16, 100}}),
      sampleOf(ejitVpSiteKey(NameB, kEJitVpMemOpSize, 0), 100, {{64, 100}}),
  };

  SmallVector<PgoValueSite, 2> sites;
  SmallVector<PgoScalarSite, 1> scalars;
  ASSERT_TRUE(aggregateValueSamples(samples, funcs, {}, sites, scalars));
  ASSERT_EQ(sites.size(), 2u);
  for (const PgoValueSite &site : sites) {
    ASSERT_EQ(site.values.size(), 1u);
    if (site.pgoNameHash == NameA)
      EXPECT_EQ(site.values[0].Value, 16u);
    else if (site.pgoNameHash == NameB)
      EXPECT_EQ(site.values[0].Value, 64u);
    else
      FAIL() << "unexpected function identity";
  }
}

TEST(EJitVpMerge, SynthesisCarriesEdgeAndValueInOneProfile) {
  FakeProfd pd;
  pd.nameRef = 0xBADC0DEu;
  pd.funcHash = 0xC0FFEEu;
  pd.numCounters = 2;
  pd.numValueSites[0] = 1; // one indirect-call site
  pd.numValueSites[1] = 1; // one memop site
  uint64_t counters[2] = {10, 20};

  PgoCounterRef refs[] = {{"vp_test_fn", reinterpret_cast<uintptr_t>(counters),
                           reinterpret_cast<uintptr_t>(&pd)}};
  PgoValueSite sites[] = {
      {0xBADC0DEu, IPVK_IndirectCallTarget, 0, {{0x77u, 90}}},
      {0xBADC0DEu, IPVK_MemOPSize, 0, {{64, 50}}},
  };

  std::string buf = synthesizeProfileBuffer(refs, sites);
  ASSERT_FALSE(buf.empty());

  // Read the synthesized profile back with the OFFICIAL reader: edge counters
  // and value data must land in one legal indexed profile.
  std::unique_ptr<MemoryBuffer> Buf =
      MemoryBuffer::getMemBuffer(buf, "ejit-vp-test.prof");
  auto ReaderOrErr = IndexedInstrProfReader::create(std::move(Buf));
  ASSERT_TRUE(static_cast<bool>(ReaderOrErr));
  std::unique_ptr<IndexedInstrProfReader> Reader = std::move(*ReaderOrErr);

  auto RecOrErr = Reader->getInstrProfRecord("vp_test_fn", 0xC0FFEEu);
  ASSERT_TRUE(static_cast<bool>(RecOrErr));
  NamedInstrProfRecord Rec = std::move(*RecOrErr);

  ASSERT_EQ(Rec.Counts.size(), 2u);
  EXPECT_EQ(Rec.Counts[0], 10u);
  EXPECT_EQ(Rec.Counts[1], 20u);

  EXPECT_EQ(Rec.getNumValueSites(IPVK_IndirectCallTarget), 1u);
  ArrayRef<InstrProfValueData> ic =
      Rec.getValueArrayForSite(IPVK_IndirectCallTarget, 0);
  ASSERT_EQ(ic.size(), 1u);
  EXPECT_EQ(ic[0].Value, 0x77u);
  EXPECT_EQ(ic[0].Count, 90u);

  EXPECT_EQ(Rec.getNumValueSites(IPVK_MemOPSize), 1u);
  ArrayRef<InstrProfValueData> mem =
      Rec.getValueArrayForSite(IPVK_MemOPSize, 0);
  ASSERT_EQ(mem.size(), 1u);
  EXPECT_EQ(mem[0].Value, 64u);
  EXPECT_EQ(mem[0].Count, 50u);
}

TEST(EJitVpMerge, InventoryReadsNumValueSites) {
  FakeProfd pd;
  pd.nameRef = 0x1234u;  // NameRef: the IR-PGO-name hash (scalar site keys)
  pd.funcHash = 0xABCDu; // FuncHash: the CFG hash (IC/memop site keys)
  pd.numCounters = 1;
  pd.numValueSites[0] = 3;
  pd.numValueSites[1] = 5;
  uint64_t counter = 0;
  PgoCounterRef refs[] = {{"fn", reinterpret_cast<uintptr_t>(&counter),
                           reinterpret_cast<uintptr_t>(&pd)}};
  SmallVector<PgoValueFunction, 1> funcs;
  ASSERT_TRUE(readValueSiteInventory(refs, funcs));
  ASSERT_EQ(funcs.size(), 1u);
  EXPECT_EQ(funcs[0].funcHash, 0xABCDu);
  EXPECT_EQ(funcs[0].pgoNameHash, 0x1234u);
  EXPECT_EQ(funcs[0].numIcSites, 3u);
  EXPECT_EQ(funcs[0].numMemSites, 5u);
  EXPECT_EQ(funcs[0].numScalarSites, 0u); // patched by the driver from capture
}

TEST(EJitVpMerge, ProfileSchemaCapturesExactIdentityAndRejectsDrift) {
  FakeProfd pd;
  pd.nameRef = 0x1234u;
  pd.funcHash = 0xABCDu;
  pd.numCounters = 2;
  pd.numValueSites[0] = 3;
  pd.numValueSites[1] = 5;
  uint64_t counters[2] = {1, 2};
  PgoCounterRef refs[] = {{"exact_name", reinterpret_cast<uintptr_t>(counters),
                           reinterpret_cast<uintptr_t>(&pd)}};
  PgoValueFunction funcs[] = {{0xABCDu, 0x1234u, 3, 5, 7}};
  std::vector<PgoFunctionSchema> schema;
  ASSERT_TRUE(readProfileSchema(refs, funcs, schema));
  ASSERT_EQ(schema.size(), 1u);
  EXPECT_EQ(schema[0].pgoName, "exact_name");
  EXPECT_EQ(schema[0].funcHash, 0xABCDu);
  EXPECT_EQ(schema[0].pgoNameHash, 0x1234u);
  EXPECT_EQ(schema[0].numCounters, 2u);
  EXPECT_EQ(schema[0].numIcSites, 3u);
  EXPECT_EQ(schema[0].numMemSites, 5u);
  EXPECT_EQ(schema[0].numScalarSites, 7u);

  funcs[0].numMemSites = 4;
  EXPECT_FALSE(readProfileSchema(refs, funcs, schema));
  EXPECT_TRUE(schema.empty());
}

#endif // EJIT_SRE_PGO_VALUE_PROFILE
