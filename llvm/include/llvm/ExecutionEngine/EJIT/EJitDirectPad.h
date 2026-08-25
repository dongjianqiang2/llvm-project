//===-- EJitDirectPad.h - AArch64 direct-dispatch pad helpers -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTPAD_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTPAD_H

#include <cstdint>

namespace llvm {
namespace ejit {

/// Encode `b Target` at Site. AArch64 B carries a signed 26-bit word offset,
/// hence the byte displacement must be aligned and lie in [-128MiB, 128MiB).
inline bool ejitEncodeAArch64DirectBranch(uintptr_t Site, uintptr_t Target,
                                          uint32_t &Instruction) {
  constexpr int64_t MinDelta = -(int64_t{1} << 27);
  constexpr int64_t MaxDelta = (int64_t{1} << 27) - 4;
  int64_t Delta;
  if (Target >= Site) {
    const uintptr_t Distance = Target - Site;
    if (Distance > static_cast<uintptr_t>(MaxDelta))
      return false;
    Delta = static_cast<int64_t>(Distance);
  } else {
    const uintptr_t Distance = Site - Target;
    if (Distance > static_cast<uintptr_t>(-MinDelta))
      return false;
    Delta = -static_cast<int64_t>(Distance);
  }
  if ((Site & 3u) != 0 || (Target & 3u) != 0 || Delta < MinDelta ||
      Delta > MaxDelta)
    return false;
  const uint32_t Imm26 = static_cast<uint32_t>(Delta / 4) & 0x03ffffffu;
  Instruction = 0x14000000u | Imm26;
  return true;
}

/// A64 instruction bytes are little-endian even in an aarch64_be ELF. Convert
/// the architectural instruction word to the value a native uint32_t store
/// must use for the target's data endianness.
inline uint32_t ejitAArch64InstructionStoreWord(uint32_t Instruction,
                                                bool DataBigEndian) {
  if (!DataBigEndian)
    return Instruction;
  return ((Instruction & 0x000000ffu) << 24) |
         ((Instruction & 0x0000ff00u) << 8) |
         ((Instruction & 0x00ff0000u) >> 8) |
         ((Instruction & 0xff000000u) >> 24);
}

} // namespace ejit
} // namespace llvm

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITDIRECTPAD_H
