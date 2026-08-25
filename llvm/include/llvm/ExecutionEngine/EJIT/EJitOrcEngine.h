//===-- EJitOrcEngine.h - OrcJIT Engine Wrapper ---------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITORCENGINE_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITORCENGINE_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/EJIT/EJitDedupIndex.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/Support/Error.h"
#include <memory>
#include <string>

#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitCodePool.h"
#endif

namespace llvm {
class Module;

namespace ejit {

class PeriodArrayRegistry;
class EJitRuntimeState;
struct EJitSharedTaskPoolState;

namespace detail {
/// Render one function definition without the rest of its specialization
/// module. Exposed for focused dump-path testing; callers should use the
/// ejit_dump_* APIs.
bool renderDumpFunctionIR(const Module &M, StringRef fnName, std::string &out);
void renderDumpModuleIR(const Module &M, std::string &out);
} // namespace detail

/// Set a function-name filter for JIT IR+ASM diagnostic capture. When non-
/// empty, the engine captures (saves in memory) the post-optimization IR and
/// emitted assembly for both the matching entry function and its complete
/// specialization module, the next time it is compiled. "*" matches every
/// specialization. Empty/null disables further capture (already-captured
/// entries are retained). Use printDumped() for the entry-only view and
/// printDumpedModule() for the complete module.
void setDumpFuncFilter(const std::string &name);

/// Bind the optional shared taskpool dump state. In a shared-taskpool build
/// this lets any core set the dump filter and discover which worker core owns
/// the latest capture. Full IR/ASM payloads remain in the worker-local dump
/// store. Passing nullptr disables shared filter and ownership metadata.
void setDumpSharedState(EJitSharedTaskPoolState *state);

/// Print the saved IR+ASM for \p name (or all saved entries when \p name is
/// null/empty) through EJIT_DIAG, one line per IR/ASM line. Names with no
/// saved capture are reported as missing. Paired with setDumpFuncFilter():
/// capture at compile time, print selectively later.
/// Returns true when a local payload was printed or matching remote-owner
/// metadata was found.
bool printDumped(const char *name);

/// Print the complete specialization module captured for \p name (or every
/// saved module when \p name is null/empty). Payloads are worker-local; unlike
/// printDumped(), this function does not consult cross-core metadata.
bool printDumpedModule(const char *name);

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

  /// Load a bitcode module into a per-specialization JITDylib identified
  /// by cacheKey. Each specialization gets its own JITDylib so symbols
  /// from the same TU bitcode can be defined multiple times without conflict.
  /// The specialization pipeline runs lazily in the IR transform layer at
  /// lookup time (legacy path; kept for direct/test callers).
  Error loadBitcodeModule(StringRef bitcodeData,
                          uint64_t cacheKey,
                          const std::string &origFnName);

  /// Result of the one-shot compile entry point below.
  struct SpecializeResult {
    /// The compiled entry pointer - or, on a dedup hit, the canonical pointer
    /// of an earlier equal-fingerprint compile (no new code was emitted and
    /// no code-pool bytes were consumed).
    void *fnPtr = nullptr;
    /// True when fnPtr was reused from the dedup index.
    bool deduped = false;
  };

  /// One-shot specialization compile (EJIT_SPECIALIZATION_DEDUP.md §5.3):
  /// parse -> run the specialization pipeline EAGERLY (pre-materialization)
  /// -> fingerprint the specialized IR -> dedup check -> on miss emit into a
  /// per-cacheKey JITDylib and look the entry up. On a dedup hit in On mode
  /// no JITDylib is created and no machine code is generated. Must be called
  /// on the owner compile thread with setActiveContext() set (compilation is
  /// serialized there; asserted via the pass-through flag).
  Expected<SpecializeResult>
  specializeAndResolve(StringRef bitcodeData, uint64_t cacheKey,
                       uint32_t funcIndex, const std::string &origFnName,
                       DedupMode dedupMode);

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

  /// Owner-private dedup index state (test/diagnostic read access).
  const EJitDedupIndex &dedupIndex() const;

  /// Drop every dedup index entry (used when a releaser is wired while dedup
  /// is configured on, so aliases to potentially-freed code disappear).
  void clearDedupIndex();

private:
  /// Parsed specialization bitcode: module + its owning context (both move
  /// together into the ThreadSafeModule at emit time).
  struct ParsedSpecModule;
  /// Parse the bitcode and apply the pre-pipeline fixups (module name,
  /// dso_local on AArch64 ELF declarations, external-linkage promotion of a
  /// static entry). Steps 1-3 shared by loadBitcodeModule and
  /// specializeAndResolve.
  Expected<ParsedSpecModule> parseSpecModule(StringRef bitcodeData,
                                             uint64_t cacheKey,
                                             const std::string &origFnName);
  /// Collect external symbols, create/populate the per-cacheKey JITDylib and
  /// add the module. Steps 4-10 shared by both paths.
  Error emitSpecModule(ParsedSpecModule PM, uint64_t cacheKey);
  /// Run the specialization pipeline + IR/ASM dumps + diagnostic capture on
  /// \p M. Shared by the eager path (specializeAndResolve, before codegen)
  /// and the legacy lazy path (IR transform layer during materialization).
  void specializeModuleEagerly(Module &M, const SpecializationContext &ctx);

  struct Impl;
  std::unique_ptr<Impl> P;
};

} // namespace ejit
} // namespace llvm

#endif
