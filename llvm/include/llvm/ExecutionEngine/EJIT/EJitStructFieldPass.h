//===-- EJitStructFieldPass.h - JIT Constant Substitution Pass ------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSTRUCTFIELDPASS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSTRUCTFIELDPASS_H

#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/IR/PassManager.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
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
/// memory via the PeriodArrayRegistry, and replaces the loads with LLVM
/// constants.
class EJitStructFieldPass : public PassInfoMixin<EJitStructFieldPass> {
public:
  /// \p verify selects the diagnostic mode described in EJitVerify.h: keep
  /// each may_const load and check it at run time instead of substituting it.
  EJitStructFieldPass(PeriodArrayRegistry &reg,
                      const uint8_t *boundData = nullptr,
                      uint32_t boundSize = 0, uint32_t boundArgIndex = 0,
                      bool verify = false)
      : registry_(reg), boundData_(boundData), boundSize_(boundSize),
        boundArgIndex_(boundArgIndex), verify_(verify) {}

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

private:
  PeriodArrayRegistry &registry_;
  const uint8_t *boundData_ = nullptr;
  uint32_t boundSize_ = 0;
  uint32_t boundArgIndex_ = 0;
  /// Unread without EJIT_VERIFY_SUBSTITUTION; kept in the interface so callers
  /// need no #ifdef.
  [[maybe_unused]] bool verify_ = false;

  // Cached metadata maps — built once per module, reused across functions.
  GVPeriodMap gvPeriodMap_;
  MayConstOffsetMap mayConstFieldMap_;
  bool mapsBuilt_ = false;
};

} // namespace ejit
} // namespace llvm

#endif
