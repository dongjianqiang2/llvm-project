//===-- EJitBranchProfileTest.cpp -----------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ProfDataUtils.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <limits>

using namespace llvm;
using namespace llvm::ejit;

namespace {

Function *makeBranchFunction(Module &M, StringRef Name,
                             ArrayRef<uint32_t> Weights = {}) {
  LLVMContext &Ctx = M.getContext();
  auto *FT =
      FunctionType::get(Type::getVoidTy(Ctx), {Type::getInt1Ty(Ctx)}, false);
  Function *F = Function::Create(FT, Function::ExternalLinkage, Name, M);
  auto *Entry = BasicBlock::Create(Ctx, "entry", F);
  auto *Taken = BasicBlock::Create(Ctx, "taken", F);
  auto *Other = BasicBlock::Create(Ctx, "other", F);

  IRBuilder<> Builder(Entry);
  auto *Branch = Builder.CreateCondBr(F->getArg(0), Taken, Other);
  if (!Weights.empty())
    setBranchWeights(*Branch, Weights, false);
  Builder.SetInsertPoint(Taken);
  Builder.CreateRetVoid();
  Builder.SetInsertPoint(Other);
  Builder.CreateRetVoid();
  return F;
}

const EJitBranchProfileSummary &
findSummary(const std::vector<EJitBranchProfileSummary> &Summaries,
            StringRef Name) {
  auto It = std::find_if(
      Summaries.begin(), Summaries.end(),
      [Name](const auto &Summary) { return Summary.functionName == Name; });
  EXPECT_NE(It, Summaries.end());
  return *It;
}

TEST(EJitBranchProfile, ClassifiesBranchShapeWithoutChangingIR) {
  LLVMContext Ctx;
  Module M("branch-audit", Ctx);
  Function *Hot = makeBranchFunction(M, "hot", {99, 1});
  Hot->setEntryCount(123);
  makeBranchFunction(M, "balanced", {50, 50});
  makeBranchFunction(M, "zero_edge", {100, 0});
  makeBranchFunction(M, "missing");
  Function::Create(FunctionType::get(Type::getVoidTy(Ctx), false),
                   Function::ExternalLinkage, "declaration", M);

  auto Summaries = analyzeBranchProfiles(M, "hot");
  ASSERT_EQ(Summaries.size(), 4u);

  const auto &HotSummary = findSummary(Summaries, "hot");
  EXPECT_TRUE(HotSummary.isRoot);
  EXPECT_EQ(HotSummary.entryCount, 123u);
  EXPECT_EQ(HotSummary.branchSites, 1u);
  EXPECT_EQ(HotSummary.profiledSites, 1u);
  EXPECT_EQ(HotSummary.biasedSites95, 1u);
  EXPECT_EQ(HotSummary.balancedSites60, 0u);
  EXPECT_GT(HotSummary.instructionCount, 0u);

  const auto &Balanced = findSummary(Summaries, "balanced");
  EXPECT_EQ(Balanced.biasedSites95, 0u);
  EXPECT_EQ(Balanced.balancedSites60, 1u);

  const auto &ZeroEdge = findSummary(Summaries, "zero_edge");
  EXPECT_EQ(ZeroEdge.biasedSites95, 1u);
  EXPECT_EQ(ZeroEdge.zeroCountEdges, 1u);

  const auto &Missing = findSummary(Summaries, "missing");
  EXPECT_EQ(Missing.branchSites, 1u);
  EXPECT_EQ(Missing.profiledSites, 0u);
}

TEST(EJitBranchProfile, AggregatesMayConstSpecializations) {
  const EJitMayConstBenefitSample Samples[] = {
      {1, 100, 70, 40, 1000, 10, 800, 8, 64, 1000000},
      {2, 120, 90, 50, 2000, 20, 1500, 15, 80, 2000000},
      {3, 80, 75, 85, 3000, 31, 0, 0, 100, 1000000}};
  EJitMayConstBenefitSummary Summary = summarizeMayConstBenefits(Samples);
  EXPECT_EQ(Summary.versions, 3u);
  EXPECT_EQ(Summary.inputMayConstLoads, 300u);
  EXPECT_EQ(Summary.specializedMayConstLoads, 235u);
  EXPECT_EQ(Summary.finalMayConstLoads, 175u);
  EXPECT_EQ(Summary.totalRemoved, 125);
  EXPECT_EQ(Summary.directRemoved, 65);
  EXPECT_EQ(Summary.pipelineRemoved, 60);
  EXPECT_EQ(Summary.averageRemoved, 41);
  EXPECT_EQ(Summary.weightedRemovedPermille, 416);
  EXPECT_EQ(Summary.minimumRemoved, -5);
  EXPECT_EQ(Summary.maximumRemoved, 70);
  EXPECT_EQ(Summary.runtimeHits, 6000u);
  EXPECT_EQ(Summary.hitSites, 61u);
  EXPECT_EQ(Summary.averageActiveSitesPermille, 20333u);
  EXPECT_EQ(Summary.removedRuntimeHits, 2300u);
  EXPECT_EQ(Summary.removedHitSites, 23u);
  EXPECT_EQ(Summary.sampledEntries, 244u);
  EXPECT_EQ(Summary.sampleCycles, 4000000u);
  EXPECT_EQ(Summary.removedHitsPerEntryPermille, 9426u);
  EXPECT_EQ(Summary.benefitPerMillionCyclesMilli, 575000u);
}

TEST(EJitBranchProfile, BenefitScalingSaturatesWithoutOverflow) {
  EJitMayConstBenefitSample Sample;
  Sample.removedRuntimeHits = std::numeric_limits<uint64_t>::max();
  Sample.sampledEntries = 1;
  Sample.sampleCycles = 1;
  EJitMayConstBenefitSummary Summary = summarizeMayConstBenefits({Sample});
  EXPECT_EQ(Summary.removedHitsPerEntryPermille,
            std::numeric_limits<uint64_t>::max());
  EXPECT_EQ(Summary.benefitPerMillionCyclesMilli,
            std::numeric_limits<uint64_t>::max());
}

TEST(EJitBranchProfile, BenefitScalingRoundsToNearestFixedPoint) {
  EJitMayConstBenefitSample Sample;
  Sample.removedRuntimeHits = 2;
  Sample.sampledEntries = 3;
  EJitMayConstBenefitSummary Summary = summarizeMayConstBenefits({Sample});
  EXPECT_EQ(Summary.removedHitsPerEntryPermille, 667u);
}

TEST(EJitBranchProfile, SixVersionsReportExactFrozenEntryTotal) {
  std::vector<EJitMayConstBenefitSample> Samples(6);
  for (EJitMayConstBenefitSample &Sample : Samples)
    Sample.sampledEntries = 64;
  EXPECT_EQ(summarizeMayConstBenefits(Samples).sampledEntries, 384u);
}

TEST(EJitBranchProfile, ComputesBenefitDensityFromPerVersionCacheLines) {
  EJitMayConstBenefitSample First;
  First.removedRuntimeHits = 200;
  First.sampleCycles = 500;
  First.publishedHotCodeBytes = 65;
  First.publishedHotCodeSizeValid = true;
  EJitMayConstBenefitSample Second;
  Second.removedRuntimeHits = 100;
  Second.sampleCycles = 500;
  Second.publishedHotCodeBytes = 64;
  Second.publishedHotCodeSizeValid = true;

  EJitMayConstBenefitSummary Summary =
      summarizeMayConstBenefits({First, Second});
  EXPECT_EQ(Summary.publishedHotCodeBytes, 129u);
  EXPECT_EQ(Summary.publishedHotICacheLines, 3u);
  EXPECT_EQ(Summary.validPublishedHotCodeVersions, 2u);
  EXPECT_TRUE(Summary.entryBenefitDensityValid);
  EXPECT_EQ(Summary.benefitPerMillionCyclesMilli, 300000000u);
  EXPECT_EQ(Summary.entryBenefitDensityMilli, 100000000u);
}

TEST(EJitBranchProfile, ZeroCodeFootprintHasZeroBenefitDensity) {
  EJitMayConstBenefitSample Sample;
  Sample.removedRuntimeHits = 100;
  Sample.sampleCycles = 100;
  EJitMayConstBenefitSummary Summary = summarizeMayConstBenefits({Sample});
  EXPECT_EQ(Summary.publishedHotICacheLines, 0u);
  EXPECT_EQ(Summary.entryBenefitDensityMilli, 0u);
  EXPECT_FALSE(Summary.entryBenefitDensityValid);
}

TEST(EJitBranchProfile, AuditsCrossVersionPartialJitCandidates) {
  EJitMayConstBenefitSample First;
  First.publishedHotCodeBytes = 5 * 64;
  First.publishedHotCodeSizeValid = true;
  First.publishedHotLineFingerprints = {1, 2, 3, 9, 9};
  EJitMayConstBenefitSample Second;
  Second.publishedHotCodeBytes = 3 * 64;
  Second.publishedHotCodeSizeValid = true;
  Second.publishedHotLineFingerprints = {2, 3, 4};
  EJitMayConstBenefitSample Third;
  Third.publishedHotCodeBytes = 2 * 64;
  Third.publishedHotCodeSizeValid = true;
  Third.publishedHotLineFingerprints = {2, 5};

  EJitMayConstBenefitSummary Summary =
      summarizeMayConstBenefits({First, Second, Third});
  EXPECT_EQ(Summary.fingerprintedHotICacheLines, 10u);
  EXPECT_EQ(Summary.crossVersionMatchingICacheLines, 5u);
  EXPECT_EQ(Summary.partialJitCandidateICacheLines, 3u);
  EXPECT_EQ(Summary.partialJitCandidatePermille, 300u);
}

TEST(EJitBranchProfile, FingerprintsPublishedCodeLinesAndSkipsZeroPadding) {
  std::vector<uint8_t> Code(64 * 3 + 8, 0);
  std::fill(Code.begin(), Code.begin() + 64, 0x5a);
  std::fill(Code.begin() + 128, Code.begin() + 192, 0x5a);
  std::fill(Code.begin() + 192, Code.end(), 0x5a);

  std::vector<uint64_t> Fingerprints = fingerprintPublishedHotICacheLines(Code);
  ASSERT_EQ(Fingerprints.size(), 3u);
  EXPECT_EQ(Fingerprints[0], Fingerprints[1]);
  EXPECT_NE(Fingerprints[0], Fingerprints[2]);
}

TEST(EJitBranchProfile, PublishedFunctionBytesUseExactSymbolExtent) {
  std::vector<uint8_t> Allocation(256, 0x7a);
  ArrayRef<uint8_t> FunctionBytes;
  ASSERT_TRUE(
      getPublishedFunctionBytes(Allocation.data() + 64, 65,
                                reinterpret_cast<uintptr_t>(Allocation.data()),
                                Allocation.size(), FunctionBytes));
  EXPECT_EQ(FunctionBytes.data(), Allocation.data() + 64);
  EXPECT_EQ(FunctionBytes.size(), 65u);
  EXPECT_EQ(fingerprintPublishedHotICacheLines(FunctionBytes).size(), 2u);
}

TEST(EJitBranchProfile, PublishedFunctionBytesRejectUnknownOrOutOfRangeSize) {
  std::vector<uint8_t> Allocation(128, 0x5a);
  ArrayRef<uint8_t> FunctionBytes;
  EXPECT_FALSE(getPublishedFunctionBytes(
      Allocation.data(), 0, reinterpret_cast<uintptr_t>(Allocation.data()),
      Allocation.size(), FunctionBytes));
  EXPECT_TRUE(FunctionBytes.empty());
  EXPECT_FALSE(
      getPublishedFunctionBytes(Allocation.data() + 96, 64,
                                reinterpret_cast<uintptr_t>(Allocation.data()),
                                Allocation.size(), FunctionBytes));
  EXPECT_TRUE(FunctionBytes.empty());
  EXPECT_FALSE(getPublishedFunctionBytes(
      reinterpret_cast<const void *>(
          reinterpret_cast<uintptr_t>(Allocation.data()) - 1),
      1, reinterpret_cast<uintptr_t>(Allocation.data()), Allocation.size(),
      FunctionBytes));
}

TEST(EJitBranchProfile, UnknownFunctionSizeInvalidatesEntryDensity) {
  EJitMayConstBenefitSample Known;
  Known.removedRuntimeHits = 100;
  Known.sampleCycles = 100;
  Known.publishedHotCodeBytes = 64;
  Known.publishedHotCodeSizeValid = true;
  Known.publishedHotLineFingerprints = {0x1234};
  EJitMayConstBenefitSample Unknown = Known;
  Unknown.publishedHotCodeBytes = 0;
  Unknown.publishedHotCodeSizeValid = false;

  EJitMayConstBenefitSummary Summary =
      summarizeMayConstBenefits({Known, Unknown});
  EXPECT_EQ(Summary.validPublishedHotCodeVersions, 1u);
  EXPECT_FALSE(Summary.entryBenefitDensityValid);
  EXPECT_EQ(Summary.entryBenefitDensityMilli, 0u);
  EXPECT_EQ(Summary.fingerprintedHotICacheLines, 0u);
  EXPECT_EQ(Summary.crossVersionMatchingICacheLines, 0u);
  EXPECT_EQ(Summary.partialJitCandidateICacheLines, 0u);
  EXPECT_EQ(Summary.benefitPerMillionCyclesMilli, 1000000000u);
}

} // namespace
