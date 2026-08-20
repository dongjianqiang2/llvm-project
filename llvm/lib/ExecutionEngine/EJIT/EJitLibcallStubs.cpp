//===-- EJitLibcallStubs.cpp - Codegen-synthesized runtime symbols --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Addresses for the symbols exposed by EJitLibcallStubs.h. See the header
// for the rationale.
//
// None of these symbols are defined here. memset/memcpy/memmove/memcmp are
// the freestanding-mandated C library functions (already linked into the AOT
// binary that hosts the EJIT runtime); __stack_chk_guard/__stack_chk_fail are
// the stack-protector ABI symbols provided by the target's underlying
// pseudo-OS / compiler-rt runtime. We only take their addresses so the engine
// can install them as absolute symbols in each specialization JITDylib —
// exactly like the user-registered symbols, just for names the AOT pass
// cannot collect.
//
// One exception is defined here (weak): __llvm_profile_instrument_target, the
// PGO
// value-profiling hook lowered from llvm.instrprof.value.profile (a no-op —
// online PGO consumes only the __profc_ counters and the __profd_ FuncHash).
// See the definitions below for the rationale.
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitLibcallStubs.h"
#include "llvm/Support/Compiler.h"
#include "llvm/ExecutionEngine/EJIT/EJitVerify.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

// The stack-protector ABI symbols have no standard C++ header. Declare them
// extern so taking their address resolves to the target's existing
// definitions (uintptr_t matches the compiler-rt declaration). Provided by
// the bare-metal pseudo-OS runtime already linked into the AOT binary.
//
// Keep __stack_chk_guard as an undefined weak reference on ELF. A deployment
// without a platform guard then fails at JIT link instead of silently using a
// fixed, predictable canary. JIT'd code that references the guard reads this
// same global, so any value is self-consistent — a nonzero constant is used
// because SRE / freestanding targets have no ASLR to randomize one.
extern "C" {
#if !defined(_WIN32)
extern void __stack_chk_fail(void);
extern uintptr_t __stack_chk_guard LLVM_ATTRIBUTE_WEAK;
#endif
}

#if defined(_WIN32)
// COFF has no ELF-style undefined weak data symbol. This address is used only
// by host-side cross-target tests; production SRE builds use the platform's
// real guard above.
static uintptr_t HostStackChkGuard =
    reinterpret_cast<uintptr_t>(&HostStackChkGuard) ^ 0x0badf00ddeadbeefULL;
[[noreturn]] static void hostStackChkFail() { std::abort(); }
#endif

// The PGO indirect-call value-profiling hook, signature matching compiler-rt's
// InstrProfilingValue.c. Online PGO consumes only the __profc_ counters and
// the __profd_ FuncHash (Stage 1 is block layout; indirect-call promotion is
// not planned), so discarding the sample is intentional.
// Weak so a real profile runtime linked into the host binary takes precedence.
extern "C" LLVM_ATTRIBUTE_WEAK void
__llvm_profile_instrument_target(uint64_t TargetValue, void *Data,
                                 uint32_t CounterIndex) {
  (void)TargetValue;
  (void)Data;
  (void)CounterIndex;
}

namespace llvm {
namespace ejit {

ArrayRef<LibcallSymbol> getLibcallSymbols() {
  static const LibcallSymbol Symbols[] = {
      {"memset", reinterpret_cast<void *>(&std::memset)},
      {"memcpy", reinterpret_cast<void *>(&std::memcpy)},
      {"memmove", reinterpret_cast<void *>(&std::memmove)},
      {"memcmp", reinterpret_cast<void *>(&std::memcmp)},
#if defined(_WIN32)
      {"__stack_chk_fail", reinterpret_cast<void *>(&hostStackChkFail)},
#else
      {"__stack_chk_fail", reinterpret_cast<void *>(&__stack_chk_fail)},
#endif
      {"__llvm_profile_instrument_target",
       reinterpret_cast<void *>(&__llvm_profile_instrument_target)},
#ifdef EJIT_VERIFY_SUBSTITUTION
      // Only a verifier build's pass emits calls to this, and only that build
      // defines it.
      {"__ejit_verify_check", reinterpret_cast<void *>(&__ejit_verify_check)},
#endif
#if defined(_WIN32)
      {"__stack_chk_guard", reinterpret_cast<void *>(&HostStackChkGuard)},
#else
      {"__stack_chk_guard", reinterpret_cast<void *>(&__stack_chk_guard)},
#endif
  };
  // The guard is last so an unresolved ELF weak reference can be omitted
  // without allocating or mutating the freestanding symbol table.
  constexpr size_t NumSymbols = sizeof(Symbols) / sizeof(Symbols[0]);
  const size_t Count = Symbols[NumSymbols - 1].addr ? NumSymbols
                                                    : NumSymbols - 1;
  return ArrayRef<LibcallSymbol>(Symbols, Count);
}

} // namespace ejit
} // namespace llvm
