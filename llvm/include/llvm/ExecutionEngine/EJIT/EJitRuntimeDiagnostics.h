//===-- EJitRuntimeDiagnostics.h - print_compiled accounting --------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIMEDIAGNOSTICS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIMEDIAGNOSTICS_H

#include "llvm/ADT/ArrayRef.h"
#include <algorithm>
#include <cstdint>
#include <limits>
#include <vector>

namespace llvm {
namespace ejit {
namespace detail {

/// One executable extent reported by a Ready shared-cache slot. A slot's
/// primary range and every extraCodeRanges entry use the same representation.
/// poolKind/poolId are part of the identity: two pools may have overlapping
/// virtual-address intervals only if they are distinct allocation domains.
struct EJitCompiledExecRange {
  uint32_t poolKind = 0;
  uint32_t poolId = 0;
  uintptr_t start = 0;
  uint64_t size = 0;
};

/// Summary used by ejit_taskpool_print_compiled. Entry bytes include every
/// valid primary and companion range. Unique bytes are the union within each
/// (poolKind, poolId) domain, so shared allocations and overlapping cold
/// companions are counted once. Invalid ranges are excluded from all byte
/// totals and counted separately.
struct EJitCompiledExecSummary {
  uint64_t readyEntryExecBytes = 0;
  uint64_t readyUniqueExecBytes = 0;
  uint64_t readyUniqueExecRanges = 0;
  uint64_t sharedSlotExecBytes = 0;
  uint64_t invalidExecRanges = 0;
  uint64_t nearHotUniqueExecBytes = 0;
  uint64_t nearColdUniqueExecBytes = 0;
  uint64_t farUniqueExecBytes = 0;
  uint64_t unknownUniqueExecBytes = 0;
};

inline EJitCompiledExecSummary
summarizeCompiledExecRanges(ArrayRef<EJitCompiledExecRange> Ranges) {
  struct Interval {
    uint32_t Kind;
    uint32_t PoolId;
    uintptr_t Start;
    uintptr_t End;
  };
  std::vector<Interval> Valid;
  EJitCompiledExecSummary Summary;
  Valid.reserve(Ranges.size());
  for (const EJitCompiledExecRange &R : Ranges) {
    if (R.start == 0 || R.size == 0 ||
        R.size > std::numeric_limits<uintptr_t>::max() - R.start) {
      ++Summary.invalidExecRanges;
      continue;
    }
    Summary.readyEntryExecBytes += R.size;
    Valid.push_back({R.poolKind, R.poolId, R.start,
                     R.start + static_cast<uintptr_t>(R.size)});
  }
  std::sort(Valid.begin(), Valid.end(), [](const Interval &A, const Interval &B) {
    if (A.Kind != B.Kind)
      return A.Kind < B.Kind;
    if (A.PoolId != B.PoolId)
      return A.PoolId < B.PoolId;
    if (A.Start != B.Start)
      return A.Start < B.Start;
    return A.End < B.End;
  });

  uint32_t CurrentKind = 0;
  uint32_t CurrentPoolId = 0;
  uintptr_t CurrentStart = 0;
  uintptr_t CurrentEnd = 0;
  bool HaveCurrent = false;
  auto Account = [&](uint32_t Kind, uintptr_t Start, uintptr_t End) {
    const uint64_t Bytes = static_cast<uint64_t>(End - Start);
    Summary.readyUniqueExecBytes += Bytes;
    ++Summary.readyUniqueExecRanges;
    switch (Kind) {
    case 1: // EJitCodePoolKind::Near
      Summary.nearHotUniqueExecBytes += Bytes;
      break;
    case 2: // EJitCodePoolKind::Far
      Summary.farUniqueExecBytes += Bytes;
      break;
    case 3: // EJitCodePoolKind::Cold
      Summary.nearColdUniqueExecBytes += Bytes;
      break;
    default:
      Summary.unknownUniqueExecBytes += Bytes;
      break;
    }
  };
  for (const Interval &I : Valid) {
    if (!HaveCurrent || I.Kind != CurrentKind || I.PoolId != CurrentPoolId ||
        I.Start > CurrentEnd) {
      if (HaveCurrent)
        Account(CurrentKind, CurrentStart, CurrentEnd);
      CurrentKind = I.Kind;
      CurrentPoolId = I.PoolId;
      CurrentStart = I.Start;
      CurrentEnd = I.End;
      HaveCurrent = true;
    } else {
      CurrentEnd = std::max(CurrentEnd, I.End);
    }
  }
  if (HaveCurrent)
    Account(CurrentKind, CurrentStart, CurrentEnd);
  Summary.sharedSlotExecBytes =
      Summary.readyEntryExecBytes >= Summary.readyUniqueExecBytes
          ? Summary.readyEntryExecBytes - Summary.readyUniqueExecBytes
          : 0;
  return Summary;
}

} // namespace detail
} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIMEDIAGNOSTICS_H
