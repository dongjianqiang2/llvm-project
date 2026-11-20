//===-- EJitOptions.h - EmbeddedJIT Configuration -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITOPTIONS_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITOPTIONS_H

#include "llvm/Config/llvm-config.h"
#include <cstddef>
#include <cstdint>
#include <string>

// Standalone harnesses can use an older installed LLVM configuration header.
#ifndef EJIT_FINAL_MERGE_MAX_MEMBERS
#define EJIT_FINAL_MERGE_MAX_MEMBERS 8192
#define EJIT_FINAL_MERGE_MAX_FALLBACKS 8192
#define EJIT_FINAL_MERGE_MAX_ARTIFACT_BYTES 33554432
#define EJIT_FINAL_MERGE_MAX_EMISSION_ATTEMPTS 4096
#endif
#ifndef EJIT_FINAL_MERGE_IR_CACHE_BYTES
#define EJIT_FINAL_MERGE_IR_CACHE_BYTES 1048576
#endif

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
  /// Ordinary cell/TRP members keep independent T1 profiles; eligible final
  /// T2 bodies are assembled into a shared switch entry on the shared worker.
  bool enableFinalVersionMerge = false;
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  /// Build-option-gated runtime sampling. This reuses the temporary
  /// instrumented tier but does not require profile-guided optimization.
  bool enableProfileAudit = true;
#else
  bool enableProfileAudit = false;
#endif
  // Owner-private bounds. These never enlarge the executable pool or a cohort.
  size_t finalMergeMaxMembers = EJIT_FINAL_MERGE_MAX_MEMBERS;
  size_t finalMergeMaxFallbacks = EJIT_FINAL_MERGE_MAX_FALLBACKS;
  size_t finalMergeMaxArtifactBytes = EJIT_FINAL_MERGE_MAX_ARTIFACT_BYTES;
  size_t finalMergeIRCacheBytes = EJIT_FINAL_MERGE_IR_CACHE_BYTES;
  size_t finalMergeMaxEmissionAttempts = EJIT_FINAL_MERGE_MAX_EMISSION_ATTEMPTS;
};

} // namespace ejit
} // namespace llvm

#endif
