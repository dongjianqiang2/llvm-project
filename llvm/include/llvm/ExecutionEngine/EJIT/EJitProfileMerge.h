//===-- EJitProfileMerge.h - in-memory PGO profile synthesis --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Synthesizes an indexed InstrProf profile buffer in memory from Tier-1's
// captured counter addresses, for consumption by Tier-2's
// PGOInstrumentationUse via an InMemoryFileSystem (EJIT_ONLINE_PGO.md §5.3).
// No file I/O, no compiler-rt runtime: reads __profc_*/__profd_* directly and
// hands records to InstrProfWriter.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITPROFILEMERGE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITPROFILEMERGE_H

#include "llvm/ADT/ArrayRef.h"
#include <cstdint>
#include <string>

namespace llvm {
namespace ejit {

/// A Tier-1 captured counter reference: the PGO function name (suffix of
/// __profc_<pgoName>) and the raw addresses of the counter/data globals that
/// captureCounterGlobals recorded + forced external.
struct PgoCounterRef {
  const char *pgoName = nullptr;
  uintptr_t profcAddr = 0; ///< __profc_<pgoName>: i64 counter array
  uintptr_t profdAddr = 0; ///< __profd_<pgoName>: __llvm_profile_data struct
};

/// Synthesize an indexed profile buffer from captured Tier-1 counters.
/// Returns an empty string on failure (caller skips Tier-2 / falls back to
/// Tier-1). Reads the __llvm_profile_data layout via InstrProfData.inc
/// (same LLVM build -> identical layout) for FuncHash + NumCounters + the
/// counter values at profcAddr.
std::string synthesizeProfileBuffer(ArrayRef<PgoCounterRef> counters);

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITPROFILEMERGE_H
