//===-- EJitOptions.h - EmbeddedJIT Configuration -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITOPTIONS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITOPTIONS_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm {
namespace ejit {

/// Off  — no JIT compilation; the wrapper falls through to AOT on every call.
/// Sync — compile inline on the calling thread (blocking, for deterministic
///         environments or when background threads are not available).
/// Async — enqueue to a background worker; the first call falls through to
///         AOT and subsequent calls hit the cache.
enum class CompileMode { Off, Sync, Async };
enum class OptimizationLevel { L1 = 1, L2 = 2, L3 = 3 };

struct Config {
  CompileMode compileMode = CompileMode::Async;
  OptimizationLevel optLevel = OptimizationLevel::L2;
  size_t maxCodeMemory = 2 * 1024 * 1024;
  size_t maxDataMemory = 128 * 1024;
  size_t maxCacheEntries = 4096;
  size_t maxCacheSize = 32 * 1024 * 1024;
  size_t maxSingleFuncSize = 512 * 1024;
  bool enableLogger = true;
  /// If true, skip the constructor-based registration path and use the
  /// static registry table (__ejit_registry_*[]).  For bare-metal where
  /// global constructors are unavailable, or for testing.
  bool forceStaticRegistry = false;
  /// If non-empty, dump JIT-optimized LLVM IR (.ll) to this directory.
  /// One file per specialization, named <funcName>_<cacheKey>.ll.
  std::string dumpJITDir;
  /// Online PGO opt-in (EJIT_ONLINE_PGO.md). Off => the JIT pipeline is
  /// unchanged (Baseline only, no instrumentation, no Tier-2). On => Tier-1
  /// instrumentation + lazy Tier-2 PGOUse recompile. The footprint cost
  /// (~640 KB stripped runtime, P0-1) is incurred whenever the PGO component
  /// libs are linked, regardless of this flag; this flag only gates behavior.
  bool enablePgo = false;
  /// Representative-PGO group sharing (V1, default OFF). When on, the compile
  /// driver groups the cells of one candidate identity, elects ONE legal
  /// representative per group generation, keeps non-representatives on their
  /// AOT fallback until the group's immutable profile bundle is published, and
  /// lets a member reuse the group's physical Tier-2 only after the emitter's
  /// exact final-identity compare. Requires enablePgo (Async + normal online
  /// PGO). Profile audit diagnostics may remain enabled alongside PGO; with
  /// PGO off, the opt-in is rejected before any group/queue side effect.
  bool enableRepresentativeSharing = false;
  /// No-progress bound in ejit_taskpool_trace_now units (host nanoseconds,
  /// SRE cycle counter ticks). Deployments must configure their clock scale.
  uint64_t representativeIdleTimeoutTicks = 5000000000ULL;
  /// Number of new sampling rounds allowed after timeout of the first round.
  uint32_t representativeMaxReelections = 2;
  /// Failed member final transforms may retry this many times after the first.
  uint32_t representativeMaxFinalRetries = 2;
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  /// Build-option-gated runtime sampling. This reuses the temporary
  /// instrumented tier but does not require profile-guided optimization.
  /// With enablePgo, diagnostics accompany normal PGOUse, not audit-only mode.
  bool enableProfileAudit = true;
#else
  bool enableProfileAudit = false;
#endif
};

} // namespace ejit
} // namespace llvm

#endif
