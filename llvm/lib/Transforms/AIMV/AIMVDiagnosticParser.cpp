// [AIMV] AIMVDiagnosticParser — parse !aimv.diag Named Metadata into
// RawDiagnostic records. See aimv_design_doc/LLVM_DESIGN.md §1.2-§3.2.
#include "llvm/Transforms/AIMV/AIMVFeedback.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

static std::optional<std::string> mdStringOperand(const MDNode *N, unsigned I) {
  if (I >= N->getNumOperands())
    return std::nullopt;
  auto *S = dyn_cast<MDString>(N->getOperand(I));
  if (!S)
    return std::nullopt;
  return S->getString().str();
}

static std::optional<int> int32Operand(const MDNode *N, unsigned I) {
  if (I >= N->getNumOperands())
    return std::nullopt;
  auto *C = dyn_cast<ConstantAsMetadata>(N->getOperand(I));
  if (!C)
    return std::nullopt;
  auto *CI = dyn_cast<ConstantInt>(C->getValue());
  if (!CI)
    return std::nullopt;
  return (int)CI->getSExtValue();
}

std::vector<AIMVFeedbackPass::RawDiagnostic>
AIMVFeedbackPass::parseDiagnostics(Module &M) {
  std::vector<RawDiagnostic> result;

  NamedMDNode *NMD = M.getNamedMetadata("aimv.diag");
  if (!NMD)
    return result;

  for (const MDNode *Diag : NMD->operands()) {
    if (Diag->getNumOperands() < 9)
      continue; // skip malformed entries

    RawDiagnostic R;
    if (auto S = mdStringOperand(Diag, 0)) R.PassName = *S;
    if (auto S = mdStringOperand(Diag, 1)) R.RemarkID = *S;
    if (auto S = mdStringOperand(Diag, 2)) R.FunctionName = *S;
    if (auto S = mdStringOperand(Diag, 3)) R.SourceLocation = *S;
    if (auto S = mdStringOperand(Diag, 4)) R.RemarkMsg = *S;

    // cost_data (operand [5])
    if (auto *CostMD = dyn_cast<MDNode>(Diag->getOperand(5))) {
      if (auto V = int32Operand(CostMD, 0)) R.ScalarCost = *V;
      if (auto V = int32Operand(CostMD, 1)) R.VectorCost = *V;
      if (auto V = int32Operand(CostMD, 2)) R.VF = *V;
      if (auto V = int32Operand(CostMD, 3)) R.IC = *V;
    }

    // dep_data (operand [6])
    if (auto *DepMD = dyn_cast<MDNode>(Diag->getOperand(6))) {
      if (DepMD->getNumOperands() > 0) {
        // First operand is dep_count; remaining are per-dependency MDNodes
        for (unsigned I = 1; I < DepMD->getNumOperands(); ++I) {
          auto *Entry = dyn_cast<MDNode>(DepMD->getOperand(I));
          if (!Entry || Entry->getNumOperands() < 4)
            continue;
          RawDiagnostic::DepEntry D;
          if (auto S = mdStringOperand(Entry, 0)) D.Type = *S;
          if (auto S = mdStringOperand(Entry, 1)) D.Source = *S;
          if (auto S = mdStringOperand(Entry, 2)) D.Sink = *S;
          if (auto S = mdStringOperand(Entry, 3)) D.AliasResult = *S;
          R.Dependencies.push_back(std::move(D));
        }
      }
    }

    // memory_data (operand [7])
    if (auto *MemMD = dyn_cast<MDNode>(Diag->getOperand(7))) {
      if (auto V = int32Operand(MemMD, 0)) R.NumStores = *V;
      if (auto V = int32Operand(MemMD, 1)) R.NumLoads = *V;
      if (auto V = int32Operand(MemMD, 2)) R.NumPredStores = *V;
      if (auto V = int32Operand(MemMD, 3)) R.MaxAlignment = *V;
      if (auto S = mdStringOperand(MemMD, 4)) R.Stride = *S;
      if (auto V = int32Operand(MemMD, 5)) R.MemCheckCount = *V;
      if (auto V = int32Operand(MemMD, 6)) R.MemCheckCost = *V;
    }

    // loop_info (operand [8])
    if (auto *LoopMD = dyn_cast<MDNode>(Diag->getOperand(8))) {
      if (auto V = int32Operand(LoopMD, 1)) R.NumBlocks = *V;
      if (auto V = int32Operand(LoopMD, 2)) R.NumInsts = *V;
      if (auto V = int32Operand(LoopMD, 3)) R.TripCount = *V;
      if (auto V = int32Operand(LoopMD, 4)) R.NumBranches = *V;
      if (auto V = int32Operand(LoopMD, 5)) R.NumCalls = *V;
    }

    result.push_back(std::move(R));
  }

  return result;
}
