//===-- EJitValueProfile.h - scalar/loop-bound value profiling passes -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  Two small passes implementing the third value-site kind
//  (EJIT_VALUE_PROFILE.md §7):
//
//  * discovery + instrumentation (runValueProfileOnFunction, Tier-1): for each
//    loop whose single exiting block ends in a direct integer comparison
//    against a loop-invariant, non-constant bound, insert one
//    ejit_vp_record_scalar(funcHash, siteIdx, value) call. Direct calls only:
//    the CFG (and therefore the PGO hash and the official value-site
//    numbering) is untouched.
//
//  * guarded specialization (EJitScalarValueSpecPass, Tier-2): for each
//    !ejit.vp-annotated site whose (funcHash, siteIdx) is in the merged side
//    table AND passes the dominance thresholds, version the loop:
//
//        preheader:
//          %guard = icmp eq iN %bound, V
//          br i1 %guard, label %hot.preheader, label %cold.preheader
//
//    The hot loop is the ORIGINAL loop with every in-loop use of %bound
//    replaced by the constant V plus an llvm.assume; the cold loop is an
//    untouched clone - the generic fallback. EJIT has no deoptimization, so
//    the fallback is never removed. Conservative shape restrictions (single
//    exiting block, single exit block, dedicated preheader, innermost loop)
//    keep the versioning provably semantics-preserving; sites that do not
//    qualify are skipped (metadata stripped) and reported at DEBUG level.
//
//  Both passes are compiled only under EJIT_SRE_PGO_VALUE_PROFILE and are
//  inert otherwise.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITVALUEPROFILE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITVALUEPROFILE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/IR/PassManager.h"

namespace llvm {

template <typename T> class function_ref;

namespace ejit {

/// Metadata attached to a loop-exit branch by the Annotate mode and consumed
/// by EJitScalarValueSpecPass: !ejit.vp = !{ i32 siteIdx, i64 funcHash }.
/// Survives inlining (it rides the instruction), so callee sites that get
/// inlined into callers keep their identity. Stripped by the specialization
/// pass once consumed.
constexpr const char *MD_EJIT_VP_SITE = "ejit.vp";

enum class EJitValueProfileMode : uint8_t {
  /// Tier-1: discover sites and insert ejit_vp_record_scalar calls.
  Instrument,
  /// Tier-2: discover sites and attach !ejit.vp metadata only (no CFG change,
  /// so the PGO hash and the official value-site numbering are untouched).
  Annotate,
};

/// Discover scalar/loop-bound value sites in \p F and instrument or annotate
/// them according to \p Mode. \p OnFunctionSites is invoked once per function
/// with (function name, site count) - the Tier-1 pipeline uses it to capture
/// the per-function site inventory for the merge. Site indices are assigned in
/// a deterministic order over the same (post-critical-edge-split) CFG, so
/// Tier-1 records and the Tier-2 transform agree.
void runValueProfileOnFunction(
    Function &F, FunctionAnalysisManager &FAM, EJitValueProfileMode Mode,
    function_ref<void(StringRef, uint32_t)> OnFunctionSites = nullptr,
    uint64_t SamplingSessionId = 0);

/// Tier-2 guarded scalar/loop-bound specialization (EJIT_VALUE_PROFILE.md
/// §7.2). Consumes \p sites (the merged side table) and the !ejit.vp metadata
/// left by the Annotate mode. Sites whose top-1 fails the min-samples /
/// dominance thresholds, or whose loop shape is not provably safe, are left
/// untouched on the generic path. Invalidate all analyses on change.
class EJitScalarValueSpecPass
    : public PassInfoMixin<EJitScalarValueSpecPass> {
public:
  explicit EJitScalarValueSpecPass(ArrayRef<PgoScalarSite> sites);

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM);

  static bool isRequired() { return false; }

private:
  SmallVector<PgoScalarSite, 8> sites_;
};

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITVALUEPROFILE_H
