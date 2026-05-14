// [BiSheng] AIMVDiagnostic.h — shared internal header for emitAIMVDiagnostic()
//
// Declared here (non-static), for inclusion by LoopVectorize.cpp and
// LoopAccessAnalysis.cpp. Implementation lives in LoopVectorize.cpp.
//
// Zero cross-component symbol dependencies:
//   - Function only depends on LLVM Core/Analysis public APIs
//     (Module, Function, Loop, LoopAccessInfo, etc.)
//   - Does NOT reference any LLVMAIMV component symbols
//   - AIMVFeedbackPass includes this header for parseDiagnostics(),
//     does NOT call emitAIMVDiagnostic()
#ifndef LLVM_TRANSFORMS_VECTORIZE_AIMVDIAGNOSTIC_H
#define LLVM_TRANSFORMS_VECTORIZE_AIMVDIAGNOSTIC_H

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Module.h"

namespace llvm {
class LoopVectorizationCostModel;
}

namespace llvm {

/// [BiSheng] Emit structured diagnostics into !aimv.diag Named Metadata.
///
/// Called from LoopVectorize (rejection/success points) and LoopAccessAnalysis
/// (UnsafeDep). Writes parallel to ORE remarks for downstream AIMVFeedbackPass.
///
/// Activation: only writes when getLLVMRemarkStreamer() is non-null
///   (-fsave-optimization-record or -Rpass-missed=loop-vectorize).
///   -aimv-enable / -aimv-output are consumed by AIMVFeedbackPass itself
///   and do NOT affect this function. No cross-component cl::opt shared
///   variables are used, avoiding dynamic-library linking and init-order issues.
///
/// @param M           Module
/// @param F           Function being compiled
/// @param L           Loop under analysis
/// @param LAI         LoopAccessInfo (dependency analysis result)
/// @param CM          Cost model (nullptr = cost stage not reached)
/// @param VF          Selected vectorization factor
/// @param IC          Selected interleave count
/// @param RemarkID    Rejection/success identifier (e.g. "CantReorderMemOps")
/// @param RemarkMsg   Diagnostic message text
/// @param SE          ScalarEvolution (nullptr = trip count not available)
/// @param RtCheckCost Runtime check total cost from GeneratedRTChecks::getCost()
///                    (-1 = not available)
/// @param RtCheckCount Runtime pointer check count from
///                     RuntimePointerChecking::getNumberOfChecks()
///                     (-1 = not available)
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
