//===-- EJitStructFieldPass.h - JIT Constant Substitution Pass ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSTRUCTFIELDPASS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSTRUCTFIELDPASS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitBoundPtr.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/PassManager.h"
#include <limits>
#include <optional>
#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#include <vector>
#endif

namespace llvm {
namespace ejit {

struct GVPeriodInfo {
  std::string periodName;
  bool isArray;
  size_t arraySize;
};

using GVPeriodMap = DenseMap<const GlobalVariable *, GVPeriodInfo>;
using MayConstOffsetMap =
    DenseMap<const GlobalVariable *, SmallVector<uint64_t, 4>>;

/// PASS6: JIT-time specialization pass. Scans the module for load instructions
/// with !ejit.may_const metadata, reads the actual runtime values from process
/// memory via the PeriodArrayRegistry or a borrowed bound-pointer view, and
/// replaces the loads with LLVM constants.
class EJitStructFieldPass : public PassInfoMixin<EJitStructFieldPass> {
public:
  /// Specialization counters from the most recent run() call, feeding the
  /// INFO-level per-function compile summary.
  struct RunStats {
    size_t MayConstLoads = 0;    // may_const loads seen
    size_t MayConstReplaced = 0; // may_const loads replaced with constants
    size_t PtrBaseReplaced = 0;  // pointer-form period roots replaced
  };

  /// \p verify selects the diagnostic mode described in EJitVerify.h: keep
  /// each may_const load and check it at run time instead of substituting it.
  /// \p FinalRound marks the last StructFieldPass invocation of a compile.
  /// The optimizer runs this pass several times and a load that fails an
  /// early round is often replaced by a later one, so per-load replace
  /// failures log at INFO only in the final round (where failure is final)
  /// and at VERBOSE in earlier rounds.
  EJitStructFieldPass(PeriodArrayRegistry &reg,
                      ArrayRef<EJitBoundPointerView> boundPointers,
                      StringRef boundRootFunction = {}, bool verify = false,
                      bool FinalRound = false)
      : registry_(reg),
        boundPointers_(boundPointers.begin(), boundPointers.end()),
        boundRootFunction_(boundRootFunction.str()), verify_(verify),
        finalRound_(FinalRound) {}

  /// Compatibility constructor for direct pass users. The data pointer is
  /// borrowed for the duration of the pass and is never copied or freed.
  EJitStructFieldPass(PeriodArrayRegistry &reg, const uint8_t *rawPtr = nullptr,
                      uint32_t rawSize = 0, uint32_t boundArgIndex = 0,
                      StringRef boundRootFunction = {},
                      std::optional<uint8_t> boundPeriodInstance = std::nullopt,
                      bool verify = false, bool FinalRound = false)
      : registry_(reg), boundRootFunction_(boundRootFunction.str()),
        verify_(verify), finalRound_(FinalRound) {
    if (rawPtr && rawSize)
      boundPointers_.push_back({rawPtr, rawSize, boundArgIndex,
                                boundPeriodInstance
                                    ? *boundPeriodInstance
                                    : std::numeric_limits<uint32_t>::max()});
  }

  /// Preserve the pre-multi-pointer verifier constructor signature.
  EJitStructFieldPass(PeriodArrayRegistry &reg, const uint8_t *rawPtr,
                      uint32_t rawSize, uint32_t boundArgIndex, bool verify)
      : EJitStructFieldPass(reg, rawPtr, rawSize, boundArgIndex, StringRef(),
                            std::nullopt, verify) {}

  /// Keep string literals from selecting the legacy bool overload above.
  EJitStructFieldPass(PeriodArrayRegistry &reg, const uint8_t *rawPtr,
                      uint32_t rawSize, uint32_t boundArgIndex,
                      const char *boundRootFunction,
                      std::optional<uint8_t> boundPeriodInstance = std::nullopt,
                      bool verify = false)
      : EJitStructFieldPass(reg, rawPtr, rawSize, boundArgIndex,
                            StringRef(boundRootFunction), boundPeriodInstance,
                            verify) {}

  /// Pre-build GV metadata maps from the Module (call once before run()).
  void initFromModule(Module &M);

#ifdef EJIT_SRE_PGO_BRANCH_AUDIT
  /// Identify loads using the same metadata and field-offset fallback as the
  /// replacement pass. The returned sites are read-only audit data.
  std::vector<EJitMayConstLoadSite>
  collectMayConstLoadSites(const Module &M) const;

  /// Add one monotonic i64 counter immediately before every may_const load.
  /// The later specialization may remove the load, but the counter remains at
  /// the original control-flow site in the temporary Tier-1 code.
  std::vector<EJitMayConstLoadSite> instrumentMayConstLoadSites(Module &M);

  /// Remove the temporary counter increments and backing global after profile
  /// matching, before final code optimization/publication.
  static void removeMayConstLoadInstrumentation(Module &M);

  static constexpr const char *MayConstCounterName = "__ejit_mayconst_hits";
  static constexpr const char *MayConstAuditSiteMD =
      "ejit.mayconst.audit.site";
#endif

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  const RunStats &lastStats() const { return lastStats_; }

private:
  PeriodArrayRegistry &registry_;
  SmallVector<EJitBoundPointerView, kEJitMaxBoundPointers> boundPointers_;
  std::string boundRootFunction_;
  /// Unread without EJIT_VERIFY_SUBSTITUTION; kept in the interface so callers
  /// need no #ifdef.
  [[maybe_unused]] bool verify_ = false;
  bool finalRound_ = false;
  RunStats lastStats_;

  struct BoundPointerState {
    EJitBoundPointerView view;
    DenseMap<const Argument *, uint64_t> boundArguments;
    SmallVector<std::pair<uint64_t, uint64_t>, 4> mayConstFields;
  };
  SmallVector<BoundPointerState, kEJitMaxBoundPointers> boundStates_;
  void initBoundArgumentPropagation(Module &M);

  // Cached metadata maps — built once per module, reused across functions.
  GVPeriodMap gvPeriodMap_;
  MayConstOffsetMap mayConstFieldMap_;
  bool mapsBuilt_ = false;
};

} // namespace ejit
} // namespace llvm

#endif
