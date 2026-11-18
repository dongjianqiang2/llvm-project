//===-- EJitBranchProfile.cpp - Online-PGO branch audit -------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ProfDataUtils.h"

#include <algorithm>
#include <limits>

using namespace llvm;
using namespace llvm::ejit;

namespace {

uint64_t ceilPercent(uint64_t Total, uint32_t Percent) {
  return (Total / 100) * Percent + ((Total % 100) * Percent + 99) / 100;
}

uint64_t floorPercent(uint64_t Total, uint32_t Percent) {
  return (Total / 100) * Percent + ((Total % 100) * Percent) / 100;
}

uint64_t saturatingAdd(uint64_t L, uint64_t R) {
  const uint64_t Max = std::numeric_limits<uint64_t>::max();
  return R > Max - L ? Max : L + R;
}

uint64_t scaledRatio(uint64_t Numerator, uint64_t Denominator,
                     uint64_t Scale) {
  if (Denominator == 0)
    return 0;
  APInt Wide(129, Numerator);
  Wide *= APInt(129, Scale);
  Wide += APInt(129, Denominator / 2);
  Wide = Wide.udiv(APInt(129, Denominator));
  if (Wide.getActiveBits() > 64)
    return std::numeric_limits<uint64_t>::max();
  return Wide.getZExtValue();
}

} // namespace

EJitMayConstBenefitSummary llvm::ejit::summarizeMayConstBenefits(
    ArrayRef<EJitMayConstBenefitSample> Samples) {
  EJitMayConstBenefitSummary Summary;
  Summary.versions = Samples.size();
  if (Samples.empty())
    return Summary;

  bool First = true;
  for (const EJitMayConstBenefitSample &Sample : Samples) {
    Summary.inputMayConstLoads =
        saturatingAdd(Summary.inputMayConstLoads, Sample.inputMayConstLoads);
    Summary.finalMayConstLoads =
        saturatingAdd(Summary.finalMayConstLoads, Sample.finalMayConstLoads);
    Summary.runtimeHits = saturatingAdd(Summary.runtimeHits, Sample.runtimeHits);
    Summary.hitSites = saturatingAdd(Summary.hitSites, Sample.hitSites);
    Summary.removedRuntimeHits =
        saturatingAdd(Summary.removedRuntimeHits, Sample.removedRuntimeHits);
    Summary.removedHitSites =
        saturatingAdd(Summary.removedHitSites, Sample.removedHitSites);
    Summary.sampledEntries =
        saturatingAdd(Summary.sampledEntries, Sample.sampledEntries);
    Summary.sampleCycles =
        saturatingAdd(Summary.sampleCycles, Sample.sampleCycles);
    const int64_t Removed = static_cast<int64_t>(Sample.inputMayConstLoads) -
                            static_cast<int64_t>(Sample.finalMayConstLoads);
    if (First) {
      Summary.minimumRemoved = Removed;
      Summary.maximumRemoved = Removed;
      First = false;
    } else {
      Summary.minimumRemoved = std::min(Summary.minimumRemoved, Removed);
      Summary.maximumRemoved = std::max(Summary.maximumRemoved, Removed);
    }
  }

  Summary.totalRemoved = static_cast<int64_t>(Summary.inputMayConstLoads) -
                         static_cast<int64_t>(Summary.finalMayConstLoads);
  Summary.averageRemoved =
      Summary.totalRemoved / static_cast<int64_t>(Summary.versions);
  Summary.averageActiveSitesPermille =
      scaledRatio(Summary.hitSites, Summary.versions, 1000);
  Summary.removedHitsPerEntryPermille =
      scaledRatio(Summary.removedRuntimeHits, Summary.sampledEntries, 1000);
  Summary.benefitPerMillionCyclesMilli = scaledRatio(
      Summary.removedRuntimeHits, Summary.sampleCycles, 1000000000);
  if (Summary.inputMayConstLoads != 0)
    Summary.weightedRemovedPermille =
        Summary.totalRemoved * 1000 /
        static_cast<int64_t>(Summary.inputMayConstLoads);
  return Summary;
}

std::vector<EJitBranchProfileSummary>
llvm::ejit::analyzeBranchProfiles(const Module &M,
                                  const std::string &rootName) {
  std::vector<EJitBranchProfileSummary> Result;

  for (const Function &F : M) {
    if (F.isDeclaration() || F.empty() ||
        F.getName().starts_with("__llvm_profile"))
      continue;

    EJitBranchProfileSummary Summary;
    Summary.functionName = F.getName().str();
    Summary.isRoot = F.getName() == rootName;
    if (auto Count = F.getEntryCount())
      Summary.entryCount = Count->getCount();

    for (const BasicBlock &BB : F) {
      Summary.instructionCount += BB.size();
      const Instruction *Term = BB.getTerminator();
      if (!Term || Term->getNumSuccessors() < 2)
        continue;

      ++Summary.branchSites;
      SmallVector<uint32_t, 8> Weights;
      if (!extractBranchWeights(*Term, Weights) ||
          Weights.size() != Term->getNumSuccessors())
        continue;

      uint64_t Total = 0;
      uint64_t Max = 0;
      uint32_t ZeroEdges = 0;
      for (uint32_t Weight : Weights) {
        Total += Weight;
        Max = std::max(Max, static_cast<uint64_t>(Weight));
        ZeroEdges += Weight == 0;
      }
      if (Total == 0)
        continue;

      ++Summary.profiledSites;
      Summary.zeroCountEdges += ZeroEdges;
      Summary.biasedSites95 += Max >= ceilPercent(Total, 95);
      Summary.balancedSites60 += Max <= floorPercent(Total, 60);
    }

    Result.push_back(std::move(Summary));
  }

  return Result;
}
