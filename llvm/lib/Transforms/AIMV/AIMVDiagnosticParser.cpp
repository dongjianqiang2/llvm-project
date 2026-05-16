// [AIMV] Parse !aimv.diag Named Metadata into RawDiagnostic records
#include "llvm/Transforms/AIMV/AIMVFeedback.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

static std::optional<std::string> mdStr(const MDNode *N, unsigned I) {
  if (I >= N->getNumOperands()) return std::nullopt;
  if (auto *S = dyn_cast<MDString>(N->getOperand(I)))
    return S->getString().str();
  return std::nullopt;
}

static std::optional<int> mdInt32(const MDNode *N, unsigned I) {
  if (I >= N->getNumOperands()) return std::nullopt;
  auto *C = dyn_cast<ConstantAsMetadata>(N->getOperand(I));
  if (!C) return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(C->getValue());
  if (!CI) return std::nullopt;
  return (int)CI->getSExtValue();
}

std::vector<AIMVFeedbackPass::RawDiagnostic>
AIMVFeedbackPass::parseDiagnostics(Module &M) {
  std::vector<RawDiagnostic> result;
  NamedMDNode *NMD = M.getNamedMetadata("aimv.diag");
  if (!NMD) return result;

  for (const MDNode *Diag : NMD->operands()) {
    if (Diag->getNumOperands() < 9) continue;

    RawDiagnostic R;
    if (auto S = mdStr(Diag, 0)) R.PassName = *S;
    if (auto S = mdStr(Diag, 1)) R.RemarkID = *S;
    if (auto S = mdStr(Diag, 2)) R.FunctionName = *S;
    if (auto S = mdStr(Diag, 3)) R.SourceLocation = *S;
    if (auto S = mdStr(Diag, 4)) R.RemarkMsg = *S;

    if (auto *CostMD = dyn_cast<MDNode>(Diag->getOperand(5))) {
      if (auto V = mdInt32(CostMD, 0)) R.ScalarCost = *V;
      if (auto V = mdInt32(CostMD, 1)) R.VectorCost = *V;
      if (auto V = mdInt32(CostMD, 2)) R.VF = *V;
      if (auto V = mdInt32(CostMD, 3)) R.IC = *V;
    }

    if (auto *DepMD = dyn_cast<MDNode>(Diag->getOperand(6))) {
      for (unsigned I = 1; I < DepMD->getNumOperands(); ++I) {
        auto *Entry = dyn_cast<MDNode>(DepMD->getOperand(I));
        if (!Entry || Entry->getNumOperands() < 4) continue;
        RawDiagnostic::DepEntry D;
        if (auto S = mdStr(Entry, 0)) D.Type = *S;
        if (auto S = mdStr(Entry, 1)) D.Source = *S;
        if (auto S = mdStr(Entry, 2)) D.Sink = *S;
        if (auto S = mdStr(Entry, 3)) D.AliasResult = *S;
        R.Dependencies.push_back(D);
      }
    }

    if (auto *MemMD = dyn_cast<MDNode>(Diag->getOperand(7))) {
      if (auto V = mdInt32(MemMD, 0)) R.NumStores = *V;
      if (auto V = mdInt32(MemMD, 1)) R.NumLoads = *V;
      if (auto V = mdInt32(MemMD, 2)) R.NumPredStores = *V;
      if (auto V = mdInt32(MemMD, 3)) R.MaxAlignment = *V;
      if (auto S = mdStr(MemMD, 4)) R.Stride = *S;
      if (auto V = mdInt32(MemMD, 5)) R.MemCheckCount = *V;
      if (auto V = mdInt32(MemMD, 6)) R.MemCheckCost = *V;
    }

    if (auto *LoopMD = dyn_cast<MDNode>(Diag->getOperand(8))) {
      if (auto V = mdInt32(LoopMD, 1)) R.NumBlocks = *V;
      if (auto V = mdInt32(LoopMD, 2)) R.NumInsts = *V;
      if (auto V = mdInt32(LoopMD, 3)) R.TripCount = *V;
      if (auto V = mdInt32(LoopMD, 4)) R.NumBranches = *V;
      if (auto V = mdInt32(LoopMD, 5)) R.NumCalls = *V;
    }

    result.push_back(std::move(R));
  }
  return result;
}
