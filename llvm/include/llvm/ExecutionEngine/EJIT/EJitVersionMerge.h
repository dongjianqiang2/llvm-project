//===-- EJitVersionMerge.h - Final member body assembly ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITVERSIONMERGE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITVERSIONMERGE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <string>

namespace llvm {
class Function;
class Module;

namespace ejit {

// Count full dimension tuples, not values on each switch level. This covers
// the product's 6x6 grid with room for an 8x8 grid; allocation stays demand-led.
constexpr unsigned kEJitFinalMergeMaxArms = 64;
constexpr unsigned kEJitFinalMergeMaxMemberInstructions = 8000;
constexpr unsigned kEJitFinalMergeMaxInstructions = 64000;
constexpr uint64_t kEJitFinalMergeMaxMemberBytes = 512 * 1024;
constexpr uint64_t kEJitFinalMergeMaxArtifactBytes =
    kEJitFinalMergeMaxArms * kEJitFinalMergeMaxMemberBytes;

struct EJitFinalMergeArm {
  Function *body = nullptr;
  SmallVector<uint32_t, 2> dimensions;
};

struct EJitFinalMergeLimits {
  unsigned maxArms = kEJitFinalMergeMaxArms;
  unsigned maxInstructions = kEJitFinalMergeMaxInstructions;
};

/// Assemble already-specialized, already-profiled bodies in one module into
/// one new function. Full-width ordinary dimension arguments select each arm;
/// other tuples call the supplied original AOT declaration with unchanged args.
/// On success the member entry calls are inlined, not retained as trampolines.
/// Input bodies remain owned by the caller and are not mutated or erased.
///
/// This is an IR assembly primitive, NOT runtime admission or publication.
/// The caller must supply one source/binding cohort, protect every member's
/// lifecycle, import private objects without conflating identity, and publish
/// the emitted function through current-version gates. No configuration is
/// read here, and no T1 counters or profiles are combined.
Expected<Function *> assembleFinalVersionSwitch(
    StringRef Name, Function &AotFallback, ArrayRef<unsigned> DimensionArgs,
    ArrayRef<EJitFinalMergeArm> Arms,
    const EJitFinalMergeLimits &Limits = EJitFinalMergeLimits());

/// Bounded owned post-PGO artifact. No borrowed configuration or profile data.
Error validateFinalVersionModule(const Module &M, StringRef Entry);
Expected<SmallVector<unsigned, 2>> finalVersionDimensionArgs(
    const Function &F, ArrayRef<std::string> Periods);
struct EJitFinalModuleArm {
  StringRef bitcode;
  SmallVector<uint32_t, 2> dimensions;
};
Expected<std::string> assembleFinalVersionModules(
    StringRef Entry, uintptr_t AotFallback, ArrayRef<unsigned> DimensionArgs,
    ArrayRef<EJitFinalModuleArm> Arms);

} // namespace ejit
} // namespace llvm

#endif
