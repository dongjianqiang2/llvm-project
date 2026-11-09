//===-- EJitJitOptions.h - LLVM options for JIT compiles ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// `-mllvm` flags on the application's compile line only reach the AOT
// compiler. The JIT instead takes the EJIT_JIT_* macros from that compile line:
// EJitRuntime.h stores them in an ejit_jit_options_record_t in the
// ejit_jit_options section, and ejit_init merges and applies the records.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITJITOPTIONS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITJITOPTIONS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#include <cstdint>

namespace llvm {
namespace ejit {

/// Index into ejit_jit_options_record_t::values.
enum class JitOption : unsigned {
  AArch64LoadStoreScanLimit,
  AArch64UpdateScanLimit,
  AArch64EnableGepOpt,
  EnableInterleavedMemAccesses,
  SlpVectorizeHorStore,
  EnablePartialInlining,
  ExtraVectorizerPasses,
  EnableLoopVersioningLicm,
  EnableLoopDistribute,
};
static_assert(static_cast<unsigned>(JitOption::EnableLoopDistribute) + 1 ==
                  EJIT_JIT_OPTION_COUNT,
              "JitOption must match ejit_jit_options_record_t");

using JitOptionValues = int32_t[EJIT_JIT_OPTION_COUNT];

/// Merge \p Records into \p Out. Fails on a malformed record, an out-of-range
/// value, or two records that set an option to different values.
bool mergeJitOptionRecords(ArrayRef<ejit_jit_options_record_t> Records,
                           JitOptionValues &Out);

/// Apply \p Values to the JIT pipeline and LLVM's cl::opts. Fails if a set
/// option does not exist in this build. Must not run concurrently with a
/// compile.
bool applyJitOptions(const JitOptionValues &Values);

/// Merge and apply the records linked into this image (called by ejit_init).
bool applyJitOptionsFromImage();

/// True if \p Opt is switched on.
bool jitOptionEnabled(JitOption Opt);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITJITOPTIONS_H
