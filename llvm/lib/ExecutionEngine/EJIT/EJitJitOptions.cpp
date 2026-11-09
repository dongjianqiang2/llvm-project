//===-- EJitJitOptions.cpp - LLVM options for JIT compiles ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitJitOptions.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/Support/CommandLine.h"

#include <iterator>

using namespace llvm;
using namespace llvm::ejit;

namespace {

struct JitOptionInfo {
  const char *Macro;
  // LLVM cl::opt to set; null if only EJitOptimizer reads the option.
  const char *ClOpt;
  int32_t Max;
};

// Indexed by JitOption. EJitOptimizer schedules the passes for the options
// without a cl::opt (LLVM only runs them in its module pipelines). It also
// schedules LoopDistribute, which additionally checks its cl::opt.
constexpr JitOptionInfo kOptions[] = {
    {"EJIT_JIT_AARCH64_LOAD_STORE_SCAN_LIMIT", "aarch64-load-store-scan-limit",
     INT32_MAX},
    {"EJIT_JIT_AARCH64_UPDATE_SCAN_LIMIT", "aarch64-update-scan-limit",
     INT32_MAX},
    {"EJIT_JIT_AARCH64_ENABLE_GEP_OPT", "aarch64-enable-gep-opt", 1},
    {"EJIT_JIT_ENABLE_INTERLEAVED_MEM_ACCESSES",
     "enable-interleaved-mem-accesses", 1},
    {"EJIT_JIT_SLP_VECTORIZE_HOR_STORE", "slp-vectorize-hor-store", 1},
    {"EJIT_JIT_ENABLE_PARTIAL_INLINING", nullptr, 1},
    {"EJIT_JIT_EXTRA_VECTORIZER_PASSES", nullptr, 1},
    {"EJIT_JIT_ENABLE_LOOP_VERSIONING_LICM", nullptr, 1},
    {"EJIT_JIT_ENABLE_LOOP_DISTRIBUTE", "enable-loop-distribute", 1},
};
static_assert(std::size(kOptions) == EJIT_JIT_OPTION_COUNT);

JitOptionValues gApplied = {-1, -1, -1, -1, -1, -1, -1, -1, -1};
// cl::opts this file has set. Others keep whatever the host process set.
bool gClOptSet[EJIT_JIT_OPTION_COUNT] = {};

cl::Option *findClOpt(unsigned I) {
  auto &Opts = cl::getRegisteredOptions();
  auto It = Opts.find(kOptions[I].ClOpt);
  return It == Opts.end() ? nullptr : It->second;
}

} // namespace

#ifdef __ELF__
// Keeps the section (and so its linker-generated bounds) present when no
// application TU sets an option. All values are EJIT_JIT_DEFAULT.
__attribute__((
    used, retain, section("ejit_jit_options"),
    aligned(4))) static const ejit_jit_options_record_t kDefaultRecord = {
    EJIT_JIT_OPTIONS_MAGIC,
    EJIT_JIT_OPTION_COUNT,
    {-1, -1, -1, -1, -1, -1, -1, -1, -1}};

extern "C" {
extern const ejit_jit_options_record_t __start_ejit_jit_options[];
extern const ejit_jit_options_record_t __stop_ejit_jit_options[];
}
#endif

bool llvm::ejit::mergeJitOptionRecords(
    ArrayRef<ejit_jit_options_record_t> Records, JitOptionValues &Out) {
  for (int32_t &V : Out)
    V = EJIT_JIT_DEFAULT;
  for (const ejit_jit_options_record_t &R : Records) {
    if (R.magic != EJIT_JIT_OPTIONS_MAGIC || R.count != EJIT_JIT_OPTION_COUNT) {
      EJIT_DIAG("jit options: bad record (magic=0x%08x count=%u)", R.magic,
                R.count);
      return false;
    }
    for (unsigned I = 0; I < EJIT_JIT_OPTION_COUNT; ++I) {
      int32_t V = R.values[I];
      if (V == EJIT_JIT_DEFAULT)
        continue;
      if (V < 0 || V > kOptions[I].Max) {
        EJIT_DIAG("jit options: %s=%d out of range [0, %d]", kOptions[I].Macro,
                  V, kOptions[I].Max);
        return false;
      }
      if (Out[I] != EJIT_JIT_DEFAULT && Out[I] != V) {
        EJIT_DIAG("jit options: %s set to both %d and %d", kOptions[I].Macro,
                  Out[I], V);
        return false;
      }
      Out[I] = V;
    }
  }
  return true;
}

bool llvm::ejit::applyJitOptions(const JitOptionValues &Values) {
  for (unsigned I = 0; I < EJIT_JIT_OPTION_COUNT; ++I)
    if (kOptions[I].ClOpt && Values[I] != EJIT_JIT_DEFAULT && !findClOpt(I)) {
      EJIT_DIAG("jit options: %s is not available in this build",
                kOptions[I].Macro);
      return false;
    }

  for (unsigned I = 0; I < EJIT_JIT_OPTION_COUNT; ++I) {
    int32_t V = Values[I];
    gApplied[I] = V;
    if (!kOptions[I].ClOpt || (V == EJIT_JIT_DEFAULT && !gClOptSet[I]))
      continue;
    cl::Option *O = findClOpt(I);
    // reset() also clears the occurrence count, which some options consult.
    O->reset();
    gClOptSet[I] = V != EJIT_JIT_DEFAULT;
    if (gClOptSet[I])
      O->addOccurrence(/*pos=*/0, O->ArgStr, itostr(V));
    if (V != EJIT_JIT_DEFAULT)
      EJIT_DIAG("jit options: %s=%d", kOptions[I].Macro, V);
  }
  return true;
}

bool llvm::ejit::applyJitOptionsFromImage() {
  JitOptionValues Values;
#ifdef __ELF__
  if (!mergeJitOptionRecords(
          ArrayRef(__start_ejit_jit_options, __stop_ejit_jit_options), Values))
    return false;
#else
  if (!mergeJitOptionRecords({}, Values))
    return false;
#endif
  return applyJitOptions(Values);
}

bool llvm::ejit::jitOptionEnabled(JitOption Opt) {
  return gApplied[static_cast<unsigned>(Opt)] > 0;
}
