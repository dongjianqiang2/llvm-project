//===-- PreservedScalarProbe.cpp - Production byte-reader probe -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// Standalone test of the actual production byte reader, not an EJIT mock.
// Cross-compilation of this file alone is not validation of the EJIT library.
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitPreservedScalar.h"

using llvm::ejit::detail::fitsPreservedElement;
using llvm::ejit::detail::readPreservedScalar;

// Dynamic inputs prevent the freestanding dependency audit being optimized
// into a constant-return function. These are test-only exported seams.
extern "C" bool ejit_preserved_scalar_read(const uint8_t *Data, uint64_t Size,
                                          uint64_t Offset, unsigned Bytes,
                                          bool LittleEndian, uint64_t *Out) {
  return Out && readPreservedScalar(Data, Size, Offset, Bytes, LittleEndian, *Out);
}

extern "C" bool ejit_preserved_element_fits(uint64_t Offset, uint64_t Bytes,
                                           uint64_t Stride, uint64_t Count,
                                           uint64_t Instance) {
  return fitsPreservedElement(Offset, Bytes, Stride, Count, Instance);
}

extern "C" int ejit_preserved_scalar_probe() {
  const uint8_t Data[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};
  uint64_t Bits = 0;
  if (!readPreservedScalar(Data, 8, 0, 8, false, Bits) ||
      Bits != UINT64_C(0x0123456789abcdef))
    return 1;
  if (!readPreservedScalar(Data, 8, 0, 8, true, Bits) ||
      Bits != UINT64_C(0xefcdab8967452301))
    return 2;
  for (unsigned Bytes = 1; Bytes <= 8; ++Bytes) {
    for (unsigned Offset = 0; Offset + Bytes <= 8; ++Offset) {
      uint64_t Big = 0, Little = 0;
      for (unsigned I = 0; I != Bytes; ++I) {
        Big = (Big << 8) | Data[Offset + I];
        Little = (Little << 8) | Data[Offset + Bytes - 1 - I];
      }
      if (!readPreservedScalar(Data, 8, Offset, Bytes, false, Bits) || Bits != Big)
        return 3;
      if (!readPreservedScalar(Data, 8, Offset, Bytes, true, Bits) || Bits != Little)
        return 4;
    }
  }
  Bits = 42;
  if (readPreservedScalar(nullptr, 8, 0, 1, true, Bits) ||
      readPreservedScalar(Data, 8, 0, 0, true, Bits) ||
      readPreservedScalar(Data, 8, 0, 9, true, Bits) ||
      readPreservedScalar(Data, 8, 8, 1, true, Bits) ||
      readPreservedScalar(Data, 8, UINT64_MAX, 1, true, Bits) ||
      readPreservedScalar(Data, UINT64_MAX, UINT64_MAX - 1, 1, true, Bits) ||
      Bits != 42)
    return 5;
  const uint8_t NegZero[] = {0x80, 0, 0, 0};
  const uint8_t NaN[] = {0x7f, 0xc1, 0x23, 0x45};
  if (!readPreservedScalar(NegZero, 4, 0, 4, false, Bits) ||
      Bits != UINT64_C(0x80000000))
    return 6;
  if (!readPreservedScalar(NaN, 4, 0, 4, false, Bits) ||
      Bits != UINT64_C(0x7fc12345))
    return 7;
  for (unsigned Cell = 0; Cell != 6; ++Cell)
    for (unsigned Other = 0; Other != 6; ++Other)
      if (fitsPreservedElement(Other * 12 + 4, 4, 12, 6, Cell) != (Cell == Other))
        return 8;
  if (fitsPreservedElement(11, 4, 12, 6, 0) ||
      fitsPreservedElement(0, 0, 12, 6, 0) ||
      fitsPreservedElement(0, 1, 0, 6, 0) ||
      fitsPreservedElement(0, 1, UINT64_MAX, 2, 0) ||
      fitsPreservedElement(72, 4, 12, 6, 6))
    return 9;
  return 0;
}

#ifndef EJIT_PROBE_NO_MAIN
int main() { return ejit_preserved_scalar_probe(); }
#endif
