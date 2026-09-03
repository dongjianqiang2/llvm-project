//===-- EJitBranchProfile.cpp - Online-PGO branch audit -------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ProfDataUtils.h"
#include "llvm/Support/xxhash.h"

#include <algorithm>
#include <limits>

using namespace llvm;
using namespace llvm::ejit;

namespace {

constexpr uint64_t EJitICacheLineBytes = 64;

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

uint64_t scaledRatio(uint64_t Numerator, uint64_t Denominator, uint64_t Scale) {
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

uint64_t scaledRatioWithProductDenominator(uint64_t Numerator,
                                           uint64_t DenominatorL,
                                           uint64_t DenominatorR,
                                           uint64_t Scale) {
  if (DenominatorL == 0 || DenominatorR == 0)
    return 0;
  APInt Denominator(192, DenominatorL);
  Denominator *= APInt(192, DenominatorR);
  APInt Wide(192, Numerator);
  Wide *= APInt(192, Scale);
  Wide += Denominator.lshr(1);
  Wide = Wide.udiv(Denominator);
  if (Wide.getActiveBits() > 64)
    return std::numeric_limits<uint64_t>::max();
  return Wide.getZExtValue();
}

} // namespace

std::vector<uint64_t>
llvm::ejit::fingerprintPublishedHotICacheLines(ArrayRef<uint8_t> CodeBytes) {
  std::vector<uint64_t> Fingerprints;
  Fingerprints.reserve(CodeBytes.size() / EJitICacheLineBytes +
                       (CodeBytes.size() % EJitICacheLineBytes != 0));
  for (size_t Offset = 0; Offset < CodeBytes.size();
       Offset += EJitICacheLineBytes) {
    const size_t LineSize =
        std::min<size_t>(EJitICacheLineBytes, CodeBytes.size() - Offset);
    ArrayRef<uint8_t> Line = CodeBytes.slice(Offset, LineSize);
    if (std::all_of(Line.begin(), Line.end(),
                    [](uint8_t Byte) { return Byte == 0; }))
      continue;
    uint64_t Fingerprint = xxHash64(Line);
    // Keep a short tail distinct from an equal prefix of a full cache line.
    Fingerprint ^= static_cast<uint64_t>(LineSize) * 0x9e3779b97f4a7c15ULL;
    Fingerprints.push_back(Fingerprint);
  }
  return Fingerprints;
}

bool llvm::ejit::getPublishedFunctionBytes(const void *FnPtr, uint64_t FnSize,
                                           uintptr_t AllocationStart,
                                           uint64_t AllocationSize,
                                           ArrayRef<uint8_t> &Out) {
  Out = {};
  const uintptr_t FnStart = reinterpret_cast<uintptr_t>(FnPtr);
  if (FnStart == 0 || FnSize == 0 || AllocationStart == 0 ||
      AllocationSize == 0 || FnSize > std::numeric_limits<size_t>::max() ||
      FnStart < AllocationStart)
    return false;
  const uint64_t Offset = FnStart - AllocationStart;
  if (Offset > AllocationSize || FnSize > AllocationSize - Offset)
    return false;
  Out = ArrayRef<uint8_t>(static_cast<const uint8_t *>(FnPtr),
                          static_cast<size_t>(FnSize));
  return true;
}

EJitMayConstBenefitSummary llvm::ejit::summarizeMayConstBenefits(
    ArrayRef<EJitMayConstBenefitSample> Samples) {
  EJitMayConstBenefitSummary Summary;
  Summary.versions = Samples.size();
  if (Samples.empty())
    return Summary;

  bool First = true;
  struct FingerprintCounts {
    uint64_t occurrences = 0;
    uint64_t versions = 0;
  };
  DenseMap<uint64_t, FingerprintCounts> Fingerprints;
  for (const EJitMayConstBenefitSample &Sample : Samples) {
    Summary.inputMayConstLoads =
        saturatingAdd(Summary.inputMayConstLoads, Sample.inputMayConstLoads);
    Summary.specializedMayConstLoads = saturatingAdd(
        Summary.specializedMayConstLoads, Sample.specializedMayConstLoads);
    Summary.finalMayConstLoads =
        saturatingAdd(Summary.finalMayConstLoads, Sample.finalMayConstLoads);
    Summary.runtimeHits =
        saturatingAdd(Summary.runtimeHits, Sample.runtimeHits);
    Summary.hitSites = saturatingAdd(Summary.hitSites, Sample.hitSites);
    Summary.removedRuntimeHits =
        saturatingAdd(Summary.removedRuntimeHits, Sample.removedRuntimeHits);
    Summary.removedHitSites =
        saturatingAdd(Summary.removedHitSites, Sample.removedHitSites);
    Summary.sampledEntries =
        saturatingAdd(Summary.sampledEntries, Sample.sampledEntries);
    Summary.sampleCycles =
        saturatingAdd(Summary.sampleCycles, Sample.sampleCycles);
    Summary.publishedHotCodeBytes = saturatingAdd(Summary.publishedHotCodeBytes,
                                                  Sample.publishedHotCodeBytes);
    Summary.validPublishedHotCodeVersions =
        saturatingAdd(Summary.validPublishedHotCodeVersions,
                      Sample.publishedHotCodeSizeValid ? 1 : 0);
    const uint64_t VersionICacheLines =
        Sample.publishedHotCodeBytes / EJitICacheLineBytes +
        (Sample.publishedHotCodeBytes % EJitICacheLineBytes != 0);
    Summary.publishedHotICacheLines =
        saturatingAdd(Summary.publishedHotICacheLines, VersionICacheLines);
    DenseSet<uint64_t> SeenInVersion;
    for (uint64_t Fingerprint : Sample.publishedHotLineFingerprints) {
      Summary.fingerprintedHotICacheLines =
          saturatingAdd(Summary.fingerprintedHotICacheLines, 1);
      FingerprintCounts &Counts = Fingerprints[Fingerprint];
      Counts.occurrences = saturatingAdd(Counts.occurrences, 1);
      if (SeenInVersion.insert(Fingerprint).second)
        Counts.versions = saturatingAdd(Counts.versions, 1);
    }
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
  Summary.directRemoved =
      static_cast<int64_t>(Summary.inputMayConstLoads) -
      static_cast<int64_t>(Summary.specializedMayConstLoads);
  Summary.pipelineRemoved =
      static_cast<int64_t>(Summary.specializedMayConstLoads) -
      static_cast<int64_t>(Summary.finalMayConstLoads);
  Summary.averageRemoved =
      Summary.totalRemoved / static_cast<int64_t>(Summary.versions);
  Summary.averageActiveSitesPermille =
      scaledRatio(Summary.hitSites, Summary.versions, 1000);
  Summary.removedHitsPerEntryPermille =
      scaledRatio(Summary.removedRuntimeHits, Summary.sampledEntries, 1000);
  Summary.benefitPerMillionCyclesMilli =
      scaledRatio(Summary.removedRuntimeHits, Summary.sampleCycles, 1000000000);
  Summary.entryBenefitDensityValid =
      Summary.validPublishedHotCodeVersions == Summary.versions;
  if (Summary.entryBenefitDensityValid) {
    Summary.entryBenefitDensityMilli = scaledRatioWithProductDenominator(
        Summary.removedRuntimeHits, Summary.sampleCycles,
        Summary.publishedHotICacheLines, 1000000000);
    for (const auto &Entry : Fingerprints) {
      const FingerprintCounts &Counts = Entry.second;
      if (Counts.versions < 2)
        continue;
      Summary.crossVersionMatchingICacheLines = saturatingAdd(
          Summary.crossVersionMatchingICacheLines, Counts.occurrences);
      Summary.partialJitCandidateICacheLines = saturatingAdd(
          Summary.partialJitCandidateICacheLines, Counts.occurrences - 1);
    }
    Summary.partialJitCandidatePermille =
        scaledRatio(Summary.partialJitCandidateICacheLines,
                    Summary.publishedHotICacheLines, 1000);
  } else {
    // A partial version set cannot support an honest cross-version code
    // comparison. Keep size_valid_versions as the explanation, but suppress
    // all aggregate fingerprint claims alongside density.
    Summary.fingerprintedHotICacheLines = 0;
  }
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
