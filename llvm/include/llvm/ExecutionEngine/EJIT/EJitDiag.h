//===-- EJitDiag.h - EmbeddedJIT Diagnostic Logging ------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Diagnostic logging for the EmbeddedJIT runtime, used for bring-up,
// field debugging, and production monitoring.  All output goes through
// the platform-provided SRE_printf() so it integrates with existing
// device log infrastructure.
//
// Compile with -DEJIT_DIAG_ENABLE to activate logging.  When not defined,
// all macros expand to nothing and incur zero runtime cost.
// Define EJIT_SRE_DIAG together with EJIT_DIAG_ENABLE to route diagnostics
// through the platform-provided SRE_printf(); otherwise diagnostics use
// std::printf.
//
// Runtime log levels (gEJitDiagLevel, default INFO):
//   OFF(0)    — no output
//   INFO(1)   — key events: init/shutdown, compile begin/OK/FAIL, cache
//               HIT/MISS, activation, errors, registration consume summary,
//               specialization replacement failures (period-index arg
//               substitution; final-round may_const loads, one line each)
//               and the per-function "spec summary" totals line
//   VERBOSE(2)— per-item detail: each first-time registration, per-function
//               struct-field stats, per-call compile_or_get, taskpool
//               requests, mid-pipeline-round may_const replace failures
//   DEBUG(3)  — internals: idempotent registration skips, staging internals,
//               funcMeta caching
//
// The level mirrors enum ejit_log_level in EJitRuntime.h; raise it at runtime
// via ejit_set_log_level() to recover full detail without recompiling.  All
// EJIT_DIAG* call sites are on cold paths (registration, compile, diagnostics),
// so the single integer compare per call is negligible even on bare-metal.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITDIAG_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITDIAG_H

#include <cstdint>

// Runtime log-level thresholds. Mirrored by enum ejit_log_level in
// EJitRuntime.h (the public C ABI contract) — keep the two in sync.
#define EJIT_LOG_LVL_OFF 0
#define EJIT_LOG_LVL_INFO 1
#define EJIT_LOG_LVL_VERBOSE 2
#define EJIT_LOG_LVL_DEBUG 3

// Process-wide runtime log level. Defined in EJitLogger.cpp (always compiled,
// hosted and freestanding). Default INFO. Set via ejit_set_log_level().
extern int gEJitDiagLevel;

//===-- Diagnostic dump print delay -----------------------------------------===//
// Diagnostic dumps (registry, active cells, compiled list, captured IR/ASM,
// icache slots) emit one line per item inside a loop. On SRE builds the
// serial-log shell ring buffer is small (512 B default, ~7 aligned 64-byte
// lines) and its consumer polls once per drain period (10 ticks; 1 tick =
// 1 ms), so those loops call ejitDiagPrintThrottle() once per printed line:
// each call delays the producer by EJIT_DIAG_PRINT_THROTTLE_TICKS scheduler
// ticks (SRE_TaskDelay), letting the consumer drain between lines. The
// default of 50 ticks buys 5 drain periods per printed line, leaving enough
// margin for long ranking rows, consumer jitter, and interleaved multi-core
// logs (measured: 2 ticks/line drops lines).
// Compile-time configurable; 0 disables. Hosted builds do not delay;
// diagnostics compiled out (no EJIT_DIAG_ENABLE) => pure no-op.

#ifndef EJIT_DIAG_PRINT_THROTTLE_TICKS
#define EJIT_DIAG_PRINT_THROTTLE_TICKS 50
#endif

#if defined(EJIT_DIAG_ENABLE) && defined(EJIT_FREESTANDING)
// Provided by the platform task API.  Must be declared exactly as written so
// the linker can resolve it against the device firmware / BSP.
extern "C" uint32_t SRE_TaskDelay(uint32_t tick);
#endif

/// Delay the caller for EJIT_DIAG_PRINT_THROTTLE_TICKS scheduler ticks so a
/// diagnostic dump loop stays behind the serial-log consumer. Call once
/// right after each line the loop printed; a no-op when the line was not
/// actually emitted (log level below INFO) or the delay is disabled.
inline void ejitDiagPrintThrottle() {
#if defined(EJIT_DIAG_ENABLE) && defined(EJIT_FREESTANDING) &&                  \
    EJIT_DIAG_PRINT_THROTTLE_TICKS > 0
  if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)
    (void)SRE_TaskDelay(EJIT_DIAG_PRINT_THROTTLE_TICKS);
#endif
}

/// Integer permille (used / reserved * 1000) for diagnostic "usage"
/// rendering: freestanding-safe (no FPU), 0 when reserved == 0, truncating
/// (never rounds up). The caller prints it as permille/10 "." permille%10.
inline uint64_t ejitDiagPermille(uint64_t used, uint64_t reserved) {
  return reserved ? used * 1000 / reserved : 0;
}

#ifdef EJIT_DIAG_ENABLE

#ifdef EJIT_SRE_DIAG
// Provided by the platform.  Must be declared exactly as written so the
// linker can resolve it against the device firmware's libc / BSP.
extern "C" int SRE_printf(const char *fmt, ...);

// Keep diagnostics in a single call so the prefix and payload stay on one line.
#define EJIT_DIAG(fmt, ...)                                                  \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)                                 \
      SRE_printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,               \
                 ##__VA_ARGS__);                                             \
  } while (0)
// Same as EJIT_DIAG but WITHOUT the "[EJIT] func:line" prefix. ALL
// diagnostic DUMP output prints through this macro - header and footer
// lines as well as per-item entry lines - so a dump block carries no
// per-line prefix and ring-buffer space goes to payload only. Grep
// anchors are the blocks' own text labels ("registry:", "code pool:",
// "stats_t:", "=== ... ===", "compiled:"). Normal-path (non-dump)
// logging keeps EJIT_DIAG so the func:line origin stays attached.
#define EJIT_DIAG_RAW(fmt, ...)                                              \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)                                 \
      SRE_printf(fmt "\n", ##__VA_ARGS__);                                   \
  } while (0)
#define EJIT_DIAG_VERBOSE(fmt, ...)                                          \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_VERBOSE)                              \
      SRE_printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,               \
                 ##__VA_ARGS__);                                             \
  } while (0)
#define EJIT_DIAG_DEBUG(fmt, ...)                                            \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_DEBUG)                                \
      SRE_printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,               \
                 ##__VA_ARGS__);                                             \
  } while (0)
#else
#include <cstdio>

#define EJIT_DIAG(fmt, ...)                                                  \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)                                 \
      std::printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,              \
                  ##__VA_ARGS__);                                            \
  } while (0)
// See the SRE variant above: prefix-free lines for ALL diagnostic dump
// output (headers and entry lines alike).
#define EJIT_DIAG_RAW(fmt, ...)                                              \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_INFO)                                 \
      std::printf(fmt "\n", ##__VA_ARGS__);                                  \
  } while (0)
#define EJIT_DIAG_VERBOSE(fmt, ...)                                          \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_VERBOSE)                              \
      std::printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,              \
                  ##__VA_ARGS__);                                            \
  } while (0)
#define EJIT_DIAG_DEBUG(fmt, ...)                                            \
  do {                                                                       \
    if (gEJitDiagLevel >= EJIT_LOG_LVL_DEBUG)                                \
      std::printf("[EJIT] %s:%d " fmt "\n", __func__, __LINE__,              \
                  ##__VA_ARGS__);                                            \
  } while (0)
#endif

#else // !EJIT_DIAG_ENABLE

// Expand to ((void)0) regardless of argument count by matching everything.
#define EJIT_DIAG(...) ((void)0)
#define EJIT_DIAG_RAW(...) ((void)0)
#define EJIT_DIAG_VERBOSE(...) ((void)0)
#define EJIT_DIAG_DEBUG(...) ((void)0)

#endif // EJIT_DIAG_ENABLE

#endif // LLVM_EXECUTIONENGINE_EJIT_EJITDIAG_H
