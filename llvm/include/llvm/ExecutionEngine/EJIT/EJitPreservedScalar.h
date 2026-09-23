//===-- EJitPreservedScalar.h - Bounded target-byte reads ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITPRESERVEDSCALAR_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITPRESERVEDSCALAR_H

#include <stdint.h>

namespace llvm {
namespace ejit {
namespace detail {

/// The complete read must lie in the tracked period element, not merely in
/// the enclosing allocation. Reading cell+1 while tracking cell is unsafe.
inline bool fitsPreservedElement(uint64_t Offset, uint64_t Bytes,
                                 uint64_t Stride, uint64_t Count,
                                 uint64_t Instance) {
  if (!Stride || !Bytes || Bytes > Stride || Instance >= Count ||
      Count > UINT64_MAX / Stride)
    return false;
  return Offset / Stride == Instance && Offset % Stride <= Stride - Bytes;
}

/// Decode an already validated, stable registered/borrowed target byte range.
/// Size must be the actual object extent, not an arbitrary pointee size.
/// Rejection leaves Out unchanged and does not read Data. Floating-point
/// consumers must use the resulting bits, not convert via a host float.
inline bool readPreservedScalar(const uint8_t *Data, uint64_t Size,
                                uint64_t Offset, unsigned Bytes,
                                bool LittleEndian, uint64_t &Out) {
  if (!Data || !Bytes || Bytes > 8 || Offset > Size || Bytes > Size - Offset)
    return false;
  const uintptr_t Base = reinterpret_cast<uintptr_t>(Data);
  if (Offset > UINTPTR_MAX - Base ||
      Bytes - 1 > UINTPTR_MAX - (Base + Offset))
    return false;
  uint64_t Bits = 0;
  for (unsigned I = 0; I != Bytes; ++I) {
    const unsigned Shift = (LittleEndian ? I : Bytes - 1 - I) * 8;
    Bits |= static_cast<uint64_t>(Data[Offset + I]) << Shift;
  }
  Out = Bits;
  return true;
}

} // namespace detail
} // namespace ejit
} // namespace llvm

#endif
