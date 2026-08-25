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

/// Specialization-dedup mode (EJIT_SPECIALIZATION_DEDUP.md). Off = today's
/// behavior exactly (no fingerprint cost). DryRun = fingerprint + count
/// would-be merges, still compile (measurement, behavior-identical). On =
/// reuse the canonical fnPtr for an equal-fingerprint compile instead of
/// consuming another code-pool allocation.
enum class DedupMode : uint8_t { Off = 0, DryRun = 1, On = 2 };

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
  /// Specialization dedup (EJIT_SPECIALIZATION_DEDUP.md). The compile driver
  /// force-lowers this to Off whenever a releaser is wired on a taskpool:
  /// a dedup hit returns a fnPtr shared with other identities, which the
  /// version-mismatch/eviction release paths must never free.
  DedupMode dedupMode = DedupMode::Off;
};

} // namespace ejit
} // namespace llvm

#endif
