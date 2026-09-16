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
#include "llvm/ADT/SmallPtrSet.h"
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
class LoadInst;
namespace ejit {

struct SpecializationContext;

struct GVPeriodInfo {
  std::string periodName;
  bool isArray;
  size_t arraySize;
};

using GVPeriodMap = DenseMap<const GlobalVariable *, GVPeriodInfo>;
using MayConstOffsetMap =
    DenseMap<const GlobalVariable *, SmallVector<uint64_t, 4>>;

/// Arguments the pass may assume a value for while computing the address of a
/// may_const load. Used for both ejit_bound_ptr roots (where the value is the
/// offset of the borrowed object) and ejit_free_dim parameters (where it is the
/// witness the address is evaluated at).
using AssumedArgMap = DenseMap<const Argument *, uint64_t>;

/// PASS6: JIT-time specialization pass. Scans the module for load instructions
/// with !ejit.may_const metadata, reads the actual runtime values from process
/// memory via the PeriodArrayRegistry or a borrowed bound-pointer view, and
/// replaces the loads with LLVM constants.
class EJitStructFieldPass : public PassInfoMixin<EJitStructFieldPass> {
public:
  EJitStructFieldPass(PeriodArrayRegistry &reg,
                      ArrayRef<EJitBoundPointerView> boundPointers,
                      StringRef boundRootFunction = {})
      : registry_(reg),
        boundPointers_(boundPointers.begin(), boundPointers.end()),
        boundRootFunction_(boundRootFunction.str()) {}

  /// Compatibility constructor for direct pass users. The data pointer is
  /// borrowed for the duration of the pass and is never copied or freed.
  EJitStructFieldPass(PeriodArrayRegistry &reg, const uint8_t *rawPtr = nullptr,
                      uint32_t rawSize = 0, uint32_t boundArgIndex = 0,
                      StringRef boundRootFunction = {},
                      std::optional<uint8_t> boundPeriodInstance = std::nullopt)
      : registry_(reg), boundRootFunction_(boundRootFunction.str()) {
    if (rawPtr && rawSize)
      boundPointers_.push_back({rawPtr, rawSize, boundArgIndex,
                                boundPeriodInstance
                                    ? *boundPeriodInstance
                                    : std::numeric_limits<uint32_t>::max()});
  }

  /// Opt-in load-only policy for the version-sharing phase-1 experiment.
  /// Retain only identity values, not IR pointers. Call initFromModule after
  /// this method and after any inline/clone/cleanup that changes the module.
  /// This does not enable profile or physical-code sharing.
  void setPreservedDimensions(const SpecializationContext &Ctx);

  /// Rebuild the metadata and assumption maps from the current module.
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

private:
  PeriodArrayRegistry &registry_;
  SmallVector<EJitBoundPointerView, kEJitMaxBoundPointers> boundPointers_;
  std::string boundRootFunction_;

  bool preserveDimensions_ = false;
  bool preservedContextValid_ = false;
  SmallVector<std::pair<std::string, uint8_t>, 4> preservedDimensions_;
  AssumedArgMap preservedArgs_;
  AssumedArgMap preservedLoadArgs_;
  SmallPtrSet<const Function *, 16> preservedFunctions_;
  std::optional<uint8_t> preservedInstance(StringRef Period) const;
  void initPreservedDimensions(Module &M);
  Constant *tryReplacePreservedLoad(LoadInst *LI, const DataLayout &DL);

  struct BoundPointerState {
    EJitBoundPointerView view;
    AssumedArgMap boundArguments;
    SmallVector<std::pair<uint64_t, uint64_t>, 4> mayConstFields;
  };
  SmallVector<BoundPointerState, kEJitMaxBoundPointers> boundStates_;
  void initBoundArgumentPropagation(Module &M);

  /// ejit_free_dim parameters, mapped to the witness their addresses are
  /// evaluated at (always 0). Consulted ONLY when computing the byte offset of
  /// a may_const load: the argument itself is never replaced, so the stores and
  /// the address arithmetic the source performs keep the live parameter. See
  /// initFreeDimAssumptions().
  AssumedArgMap freeDimArgs_;
  void initFreeDimAssumptions(Module &M);

  // Cached metadata maps — built once per module, reused across functions.
  GVPeriodMap gvPeriodMap_;
  MayConstOffsetMap mayConstFieldMap_;
  bool mapsBuilt_ = false;
};

} // namespace ejit
} // namespace llvm

#endif
