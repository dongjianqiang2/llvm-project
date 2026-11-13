//===-- EJitRegistryTable.c - Registry Section Layout (documentation) -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// PASS1 (EJitRegisterBitcode), PASS2 (EJitRegisterPeriod), and PASS3
// (EJitWrapperGen) emit their per-translation-unit entries as *private* arrays
// placed in dedicated registry sections. The linker concatenates these input
// sections across every TU. The leading-dot names are not valid C identifiers,
// so the linker does not auto-synthesize __start_/__stop_; a linker script
// defines __start_/__stop_ejit_bitcode and __start_/__stop_ejit_period and
// brackets the sections, which the runtime (EJit.cpp) walks as [start, stop)
// ranges.
//
// Layout:
//   .ejit_bitcode : embedded bitcode and symbol entries
//   .ejit_period  : period/static entries plus lifecycle/funcIndex fixups
//
// Because the entries are private per TU, several TUs can define ejit_entry
// functions without "duplicate symbol" link errors.
//
// EJit.cpp declares the __start_/__stop_ symbols weak in hosted builds
// (!EJIT_FREESTANDING), so a program with no ejit_entry functions (sections
// absent) resolves them to null and walks an empty range. In freestanding
// builds (EJIT_FREESTANDING) they are strong (weak would create a GOT
// relocation), so a linker script MUST define them. No default array
// definitions are needed here; this file is retained only as documentation of
// the registry layout. The header include keeps the translation unit
// non-empty (ISO C forbids an empty TU).
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h"
