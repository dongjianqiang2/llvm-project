// [AIMV] Public header for emitAIMVDiagnostic() and AIMVCostSnapshot.
// Included by LoopVectorize.cpp and LoopAccessAnalysis.cpp.
#ifndef LLVM_ANALYSIS_AIMVDIAGNOSTIC_H
#define LLVM_ANALYSIS_AIMVDIAGNOSTIC_H

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Module.h"

namespace llvm {

/// [AIMV] Pre-computed cost snapshot for a vectorization decision point.
/// Callers pre-compute costs from the CostModel and pass this struct
/// to emitAIMVDiagnostic() — the analysis layer never touches CostModel directly.
struct AIMVCostSnapshot {
  int ScalarCost = -1;
  int VectorCost = -1;
  int VF = 0;
  int IC = 0;

  static AIMVCostSnapshot unknown() { return {-1, -1, 0, 0}; }
};

/// [AIMV] Emit structured diagnostics into !aimv.diag Named Metadata.
/// Always writes when called; AIMVFeedbackPass controls consumption via
/// -mllvm -aimv-enable / -faimv. Overhead is negligible.
void emitAIMVDiagnostic(
    Module &M, Function &F, Loop &L,
    const LoopAccessInfo *LAI,
    const AIMVCostSnapshot &Cost,
    StringRef RemarkID, StringRef RemarkMsg,
    ScalarEvolution *SE = nullptr,
    int RtCheckCost = -1,
    int RtCheckCount = -1);

} // namespace llvm
#endif
