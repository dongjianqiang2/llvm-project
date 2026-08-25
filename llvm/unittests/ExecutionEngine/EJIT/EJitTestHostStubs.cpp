//===-- EJitTestHostStubs.cpp - host-link stubs for the EJIT gtests --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception.
//
//===----------------------------------------------------------------------===//
//
//  EJitLibcallStubs registers __stack_chk_guard/__stack_chk_fail as
//  JIT-visible symbols by referencing the HOST binary's definitions. The SRE
//  target's libc provides them; host x86-64 glibc does NOT export a global
//  __stack_chk_guard (its own stack protector uses the FS:0x28 TLS slot), so
//  a host gtest binary linking LLVMEJIT needs these stubs. Test-link only:
//  production binaries must never pull this TU in.
//
//===----------------------------------------------------------------------===//

#include <cstdint>

extern "C" {

// Nonzero, fixed: JIT-compiled code with -fstack-protector only ever
// compares against it; host code on x86-64 uses the FS slot instead and
// never reads this symbol.
uintptr_t __stack_chk_guard = 0x5eedba5e5eedba5eULL;

void __stack_chk_fail(void) {
  // No test path should reach a stack smash; trap loudly if one does.
  __builtin_trap();
}

} // extern "C"
