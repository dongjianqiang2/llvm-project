//===-- EJitColdReservation.h - Fixed cold reservation bounds -----*- C++ -*-===//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCOLDRESERVATION_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCOLDRESERVATION_H

#include <cstdint>

namespace llvm {
namespace ejit {

/// DLIB may place the reservation at only 4KiB alignment. Keep only complete
/// 2MiB pages inside it; never expand into adjacent sections or the near pool.
/// Returns a diagnostic reason on failure and clears both output bounds.
inline const char *alignColdReservation(uintptr_t Base, uintptr_t End,
                                        uintptr_t NearBase, uintptr_t NearEnd,
                                        uintptr_t &AlignedBase,
                                        uintptr_t &AlignedEnd) {
  AlignedBase = AlignedEnd = 0;
  constexpr uintptr_t Page = 2u * 1024u * 1024u;
  constexpr uintptr_t Mask = Page - 1;
  if (!Base || End <= Base)
    return "missing-or-invalid-cold-range";
  if (!NearBase || NearEnd <= NearBase)
    return "missing-or-invalid-near-range";
  if (!(End <= NearBase || Base >= NearEnd))
    return "cold-near-overlap";
  if (Base > UINTPTR_MAX - Mask)
    return "cold-alignment-overflow";
  const uintptr_t Start = (Base + Mask) & ~Mask;
  const uintptr_t Stop = End & ~Mask;
  if (Stop <= Start)
    return "cold-too-small-after-alignment";
  AlignedBase = Start;
  AlignedEnd = Stop;
  return nullptr;
}

} // namespace ejit
} // namespace llvm

#endif
