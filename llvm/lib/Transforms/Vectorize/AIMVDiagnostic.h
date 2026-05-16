// [AIMV] Shared internal header for emitAIMVDiagnostic()
// Included by LoopVectorize.cpp and LoopAccessAnalysis.cpp.
// Implementation lives in LoopVectorize.cpp.
#ifndef LLVM_TRANSFORMS_VECTORIZE_AIMVDIAGNOSTIC_H
#define LLVM_TRANSFORMS_VECTORIZE_AIMVDIAGNOSTIC_H

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Module.h"

namespace llvm {
class LoopVectorizationCostModel;

/// [AIMV] Emit structured diagnostics into !aimv.diag Named Metadata.
/// Activation: unconditional — always writes when called. AIMVFeedbackPass
/// controls whether the data is consumed (via -mllvm -aimv-enable / -faimv).
/// Write overhead is negligible (a few MDNodes per loop).
void emitAIMVDiagnostic(
    Module &M, Function &F, Loop &L,
    const LoopAccessInfo *LAI,
    LoopVectorizationCostModel *CM,
    ElementCount VF, unsigned IC,
    StringRef RemarkID, StringRef RemarkMsg,
    ScalarEvolution *SE = nullptr,
    int RtCheckCost = -1,
    int RtCheckCount = -1);

} // namespace llvm
#endif
