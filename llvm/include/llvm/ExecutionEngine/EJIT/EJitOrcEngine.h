//===-- EJitOrcEngine.h - OrcJIT Engine Wrapper ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITORCENGINE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITORCENGINE_H

#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <string>

#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#endif

namespace llvm {
namespace ejit {

class PeriodArrayRegistry;
class EJitRuntimeState;
struct EJitSharedTaskPoolState;
struct PreloadedBitcode;

/// Set a function-name filter for JIT IR+ASM diagnostic capture. When non-
/// empty, the engine captures (saves in memory) the post-optimization IR and
/// the emitted assembly of any specialization whose entry name exactly
/// matches, the next time it is compiled. Empty/null disables further
/// capture (already-captured entries are retained). Captured entries are
/// printed on demand via printDumped(). Used for performance analysis.
void setDumpFuncFilter(const std::string &name);

/// Bind the optional shared taskpool dump state. In a shared-taskpool build this
/// lets any core set the dump filter and print the last worker-captured IR/ASM
/// result. Passing nullptr disables the shared path and falls back to the local
/// process-static dump store.
void setDumpSharedState(EJitSharedTaskPoolState *state);

/// Print the saved IR+ASM for \p name (or all saved entries when \p name is
/// null/empty) through EJIT_DIAG, one line per IR/ASM line. Names with no
/// saved capture are reported as missing. Paired with setDumpFuncFilter():
/// capture at compile time, print selectively later.
void printDumped(const char *name);

struct SpecializationContext {
  std::string fnName;
  uint64_t cacheKey = 0;
  struct DimInfo {
    std::string periodName;
    uint8_t cellIdx;
  };
  SmallVector<DimInfo, 4> dimensions;
  OptimizationLevel optLevel = OptimizationLevel::L2;
};

/// Wraps an LLJIT instance with EmbeddedJIT-specific configuration:
/// custom memory manager and IR transform layer for the JIT pipeline.
class EJitOrcEngine {
public:
  EJitOrcEngine();
  ~EJitOrcEngine();

  /// Create a configured engine. Thread-safe to call once.
  static Expected<std::unique_ptr<EJitOrcEngine>>
  Create(const Config &config,
         PeriodArrayRegistry &periodReg,
         EJitRuntimeState &runtimeState);

  /// Parse \p bitcodeData now and cache the template loadBitcodeModule() would
  /// otherwise build later. Unlike loadBitcodeModule(), which only caches a
  /// blob seen twice, this caches immediately — use it for blobs known to
  /// produce several specializations. Idempotent; same storage contract as
  /// loadBitcodeModule(). Not safe to call concurrently with a compile: call
  /// it during init, before the taskpool worker starts.
  Error preLoadBitcodeUtil(StringRef bitcodeData);

  /// Parsed-bitcode cache footprint and hit rate.
  struct BitcodeCacheStats {
    size_t entries = 0;
    /// Derived from bitcode size, not measured from the allocator.
    size_t approxBytes = 0;
    /// Without caching this would equal the number of cold compiles.
    uint64_t parses = 0;
    uint64_t templateHits = 0;
    uint64_t evictions = 0;
  };
  BitcodeCacheStats getBitcodeCacheStats() const;

  /// Drop every cached template. In-flight modules are unaffected: each holds
  /// its own reference to the context it was cloned into.
  void clearBitcodeCache();

  /// Load a bitcode module into a per-specialization JITDylib identified
  /// by cacheKey. Each specialization gets its own JITDylib so symbols
  /// from the same TU bitcode can be defined multiple times without conflict.
  ///
  /// STORAGE CONTRACT: a blob is identified by its address and size, so
  /// \p bitcodeData must stay alive and unmodified while the engine may
  /// compile from it — rewriting it in place would compile the stale module.
  /// AOT bitcode lives in a const image section and satisfies this; callers
  /// with their own buffers must call clearBitcodeCache() after changing them.
  Error loadBitcodeModule(StringRef bitcodeData,
                          uint64_t cacheKey,
                          const std::string &origFnName);

  /// Look up a compiled function symbol in the specialization JITDylib
  /// identified by cacheKey.
  Expected<void *> lookup(uint64_t cacheKey, const std::string &name);

  /// Set the active specialization context (used during compilation).
  void setActiveContext(const SpecializationContext *ctx);
  const SpecializationContext *getActiveContext() const;

  /// Register a user-defined external symbol (function or global) that the
  /// JIT can resolve when compiling bitcode modules. Required for bare-metal
  /// environments where dynamic symbol lookup is unavailable.
  void addUserSymbol(const std::string &name, void *addr);

#ifdef EJIT_SRE_CODE_POOL
  /// Snapshot of the SRE code-pool statistics (pool / sealed counts, used /
  /// wasted bytes, enable_ex invocations) for diagnostics and tests. Returns a
  /// zeroed snapshot if no pool is active. Available only with
  /// EJIT_SRE_CODE_POOL.
  EJitCodePoolManager::Stats getCodePoolStats() const;

  /// Resolve a compiled function pointer to its real, finalized executable
  /// range + owning code pool (for cross-core 4K execute-permission
  /// preparation). Returns false if \p FnPtr is not pool-backed code with a
  /// recorded finalized range. Available only with EJIT_SRE_CODE_POOL.
  bool findCodeRange(const void *FnPtr, EJitCompiledCodeInfo &Out) const;
#endif

private:
  /// Derive the template module + symbol scaffolding. Does not touch the
  /// cache; the caller decides whether to retain the result.
  Error buildPreloadedBitcode(StringRef bitcodeData, PreloadedBitcode &Out);

  /// Retain \p PB, evicting least-recently-used entries to stay under
  /// Config::maxPreloadCacheSize. Null if it does not fit at all.
  PreloadedBitcode *retainPreloadedBitcode(StringRef bitcodeData,
                                           PreloadedBitcode &&PB);

  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ejit
} // namespace llvm

#endif
