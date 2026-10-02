//===-- EJitSwitchCase.h - ejit_runtime_dim switch-case arms ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Switch-case mode (jit_design_doc/EJIT_SWITCH_CASE.md), built only with
// EJIT_SWITCH_CASE: clones the code that needs a projection of the
// ejit_runtime_dim parameter once per useful key, behind a switch on it, with
// the original code as the default arm. Eager path only (§5.2); anything else
// is declined and logged, leaving the entry unchanged.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITSWITCHCASE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITSWITCHCASE_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/PassManager.h"
#include <cstdint>
#include <string>

namespace llvm {
class Argument;
class Function;

namespace ejit {
class EJitStructFieldPass;

/// The arms one compile built; Tier-1 records it, Tier-2 replays it (§10).
/// Not behind EJIT_SWITCH_CASE, so SpecializationContext's layout is fixed.
struct EJitSwitchCaseDecision {
  bool valid = false;      ///< the entry has an ejit_runtime_dim
  uint64_t projection = 0; ///< 0 when no arms were built
  SmallVector<uint32_t, 8> keys;
};

/// Per-compile limits (§3).
struct EJitSwitchCaseLimits {
  unsigned maxArms;   ///< A_max; an entry's ejit_runtime_dim(n) replaces it
  unsigned maxRegion; ///< IR instructions in one arm's region
  unsigned maxCloned; ///< IR instructions added by cloning in one compile

  /// The EJIT_SWITCH_CASE_MAX_* values this library was built with.
  static EJitSwitchCaseLimits fromBuild();
};

/// The op of a projection descriptor, {op:8, width:8, pad:16, constant:32}.
enum class EJitProjectionOp : uint8_t { Identity = 0, URem = 1 };

/// What the step did to one entry, as logged.
struct EJitSwitchCaseResult {
  enum class Path { None, Eager };
  Path path = Path::None;
  /// Why no arms were built; empty when path == Eager.
  std::string declined;
  /// Packed descriptor; 0 until a projection is recognized.
  uint64_t projection = 0;
  /// M: the number of keys the projection can produce; 0 when unbounded.
  uint64_t domain = 0;
  /// Key-dependent may_const loads in the entry.
  unsigned sites = 0;
  SmallVector<uint32_t, 8> keptKeys;
  /// Instructions in one region, and added by all the clones.
  unsigned regionSize = 0;
  unsigned cloned = 0;
};

/// The ejit_runtime_dim parameter of \p F and its per-entry arm limit (0 for
/// the build default), or null when \p F does not carry a usable annotation.
Argument *getEJitRuntimeDim(const Function &F, unsigned *MaxArms = nullptr);

/// Add the arms to \p F, an ejit_entry that PASS6 has already run on.
/// \p Resolver must be configured like that run, so the keys selected are the
/// ones the next PASS6 run folds.
/// With \p Replay (PGO Tier-2), builds exactly Tier-1's arms instead of
/// selecting keys (§10).
EJitSwitchCaseResult runSwitchCase(Function &F, EJitStructFieldPass &Resolver,
                                   FunctionAnalysisManager &FAM,
                                   const EJitSwitchCaseLimits &Limits,
                                   const EJitSwitchCaseDecision *Replay = nullptr);

} // namespace ejit
} // namespace llvm

#endif
