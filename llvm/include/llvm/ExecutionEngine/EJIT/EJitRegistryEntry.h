//===-- EJitRegistryEntry.h - Static Registry Table Entry -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Global constant array entry type for bare-metal auto-registration.
// Each translation unit emits private arrays, avoiding duplicate external
// definitions when several units contain ejit_entry functions. The linker
// concatenates their entries into two ranges, with no terminator needed:
//   .ejit_bitcode: PASS1 embedded bitcode and symbol registrations
//   .ejit_period:  PASS2 period arrays and static variables; PASS3 lifecycle,
//                  dense function index, and inline cache slot fixups
// Static registration walks [__start_ejit_bitcode, __stop_ejit_bitcode) and
// [__start_ejit_period, __stop_ejit_period). Dotted section names are not C
// identifiers, so a linker script must supply these boundaries. Hosted builds
// use weak references so missing ranges are empty; freestanding builds use
// strong references to avoid a GOT relocation and require the linker script.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITREGISTRYENTRY_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITREGISTRYENTRY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  EJIT_REG_BITCODE = 0,
  EJIT_REG_PERIOD_ARRAY = 1,
  EJIT_REG_STATIC_VAR = 2,
  EJIT_REG_SYMBOL = 3,
  EJIT_REG_NONE = 4,      // sentinel (kept at 4 for table ABI stability)
  EJIT_REG_LIFECYCLE = 5, // lifecycle dimType-slot fixup (additive)
  EJIT_REG_FUNCINDEX = 6, // function dense-funcIndex fixup (additive)
  EJIT_REG_ICACHE_SLOT = 7 // function inline-cache slot-pointer fixup (additive)
} ejit_reg_type_t;

typedef struct {
  ejit_reg_type_t type;
  const char
      *name1; // funcName / periodName / varName / symbolName / lifecycleName
  const char *name2; // varName (period/static), MissFn sentinel
                     //   (EJIT_REG_ICACHE_SLOT, branchless tables), else NULL
  const void *ptr;   // bitcode data / baseAddr / symbol addr / &i32 slot /
                     //   &ptr icache slot base (EJIT_REG_ICACHE_SLOT)
  uint64_t size;     // bitcode size / array size / numDims (EJIT_REG_ICACHE_SLOT)
                     //   / 0
} ejit_reg_entry_t;

#ifdef __cplusplus
}
#endif

#endif
