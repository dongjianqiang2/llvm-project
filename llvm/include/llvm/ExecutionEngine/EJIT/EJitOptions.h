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
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  /// Build-option-gated runtime sampling. This reuses the temporary
  /// instrumented tier but does not require profile-guided optimization.
  bool enableProfileAudit = true;
#else
  bool enableProfileAudit = false;
#endif
  /// Diagnostic: instead of freezing may_const values, keep the loads and
  /// check them against what would have been frozen (EJitVerify.h). Every
  /// mismatch names a field that must not be ejit_may_const. Disables
  /// specialization while on — the loads survive, so nothing folds.
  bool verifySubstitution = false;
};

} // namespace ejit
} // namespace llvm

#endif
