//===-- EJitPgoPolicy.h - adaptive PGO inline policy ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Adaptive policy for the stage-3 PGO inline (EJIT_ONLINE_PGO.md §12 阶段3,
// v0.7 "默认按 callee 形态自适应"). Decides, per ejit_entry closure at AOT
// time, whether PASS1 (preOptimizeBitcode) should drop buildModuleInlinerPipeline
// (non-pre-inlined bitcode -> JIT Tier-2 PGO inline decides) or keep it
// (pre-inlined, current behavior).
//
// The decision is driven by P0-6 (EJIT_ONLINE_PGO.md §11.11): non-pre-inlined
// bitcode is a Flash WIN only for medium+ callees at multiple callsites
// (callsite duplication would bloat the pre-inlined form; JIT PGO inline picks
// the hot callee). For small/foldable callees it is a Flash COST (+14~22%),
// because pre-inlining + GlobalDCE + inter-callee InstCombine folding beat the
// non-pre-inlined structural overhead. So the default is adaptive, not
// always-aggressive.
//
// This is a header-only pure function so it can be unit-tested without the AOT
// pass (LLVMEmbeddedJIT) and called by preOptimizeBitcode (stage 3b).
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITPGOPOLICY_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITPGOPOLICY_H

#include "llvm/ADT/ArrayRef.h"
#include <cstdint>

namespace llvm {
namespace ejit {

/// Shape of one internal callee in an ejit_entry closure, as seen at AOT
/// (preOptimizeBitcode) time.
struct CalleeShape {
  /// Callee body size in IR instructions (a proxy for inline cost).
  uint32_t instCount = 0;
  /// Number of callsites that reference this callee (from the entry function
  /// and other callees in the closure). >=2 means inlining would duplicate
  /// the body at multiple sites.
  uint32_t callSiteCount = 0;
};

/// Adaptive PGO-inline policy. Returns true => drop buildModuleInlinerPipeline
/// (non-pre-inlined bitcode, JIT Tier-2 PGO inline); false => keep it
/// (pre-inlined, current behavior).
///
/// Rule (P0-6): non-pre-inlined only when the closure has at least one
/// medium-sized callee referenced from multiple callsites - the one regime
/// where non-pre-inlined is a Flash win. Otherwise pre-inlined (small/foldable
/// callees, or single-callsite callees, favor pre-inlining).
///
/// Thresholds are calibrated on P0-6's synthetic modules (medium callee ~6-7
/// insts); they need re-calibration on real ejit_entry closures (§11.11
/// mitigation), but the rule structure is stable.
inline bool shouldUseNonPreInlinedBitcode(ArrayRef<CalleeShape> callees) {
  // A callee is "medium+" if its body is at least this many instructions.
  // P0-6: small (~1-3 insts) -> pre-inlined; medium (~6-7) -> non-pre wins.
  constexpr uint32_t kMediumInstThreshold = 6;
  constexpr uint32_t kMultiCallsiteThreshold = 2;
  for (const CalleeShape &c : callees)
    if (c.instCount >= kMediumInstThreshold &&
        c.callSiteCount >= kMultiCallsiteThreshold)
      return true;
  return false;
}

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITPGOPOLICY_H
