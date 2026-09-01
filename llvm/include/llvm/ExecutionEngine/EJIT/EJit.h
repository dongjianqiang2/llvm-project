//===-- EJit.h - EmbeddedJIT Main C++ API ---------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJIT_H
#define LLVM_EXECUTIONENGINE_EJIT_EJIT_H

#include "llvm/ExecutionEngine/EJIT/EJitError.h"
#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include <memory>
#include <string>

// Forward declaration of the C ABI stats struct (defined in EJitRuntime.h),
// so getCodePoolStats() can take it without pulling the C API header into the
// C++ core. C language linkage to match the definition in EJitRuntime.h.
extern "C" {
struct ejit_code_pool_stats_t;
struct ejit_code_pool_stats_v2_t;
struct ejit_code_pool_stats_v3_t;
}

namespace llvm {
namespace ejit {

class EJitCompileDriver;
class EJitLogger;
class EJitTaskPool;
class EJitSharedTaskPool;

/// Main user-facing class for EmbeddedJIT. Owns all runtime components.
class EJit {
public:
  EJit(const Config &config = {});
  ~EJit();

  // Lifecycle — period-level (fans out to all arrays under periodName).
  // Returns false only in a taskpool build when periodName is not a registered
  // lifecycle (no state is changed), or when cellIdx >= kEJitMaxInstances;
  // always true in the legacy build. cellIdx is uint32_t so an out-of-range
  // instance index is rejected here instead of being truncated at the ABI
  // boundary (see ejit_activate in EJitRuntime.h).
  bool activate(const std::string &periodName, uint32_t cellIdx);
  bool deactivate(const std::string &periodName, uint32_t cellIdx);

  bool activateAll(const std::string &periodName);
  bool deactivateAll(const std::string &periodName);
  // uint32_t like activate/deactivate: an out-of-range index is rejected
  // (false) before any narrowing, never truncated into instance 0.
  bool isActive(const std::string &periodName, uint32_t cellIdx) const;

  // Compilation
  /// Pre-computed cacheKey = funcIdx(32b) | dims(4x8b). The AOT wrapper
  /// computes this in registers; no dims array construction.

  // Cache management
  void clearCache();
  // uint32_t like activate/deactivate: an out-of-range index is rejected
  // (no-op) before any narrowing, never truncated into instance 0.
  void invalidateByPeriod(const std::string &periodName, uint32_t cellIdx);
  void invalidateAllByPeriod(const std::string &periodName);

  // Configuration
  /// Change compile mode. On failure the current mode is preserved and false
  /// is returned.
  ///
  /// Private taskpool: Async requires a ready ORC engine on this instance and a
  /// successfully started worker.
  ///
  /// Shared taskpool: Async instead requires the SHARED pool to be serviceable
  /// (Ready, owner elected, worker running) -- peers have no engine of their
  /// own, the elected owner compiles for every core. The switch is rejected if
  /// the owner shuts down while it is in flight.
  ///
  /// Shared SYNC compiles on the CALLING core, and only the owner has an
  /// engine, so a non-owner core in Sync mode does not JIT: its calls resolve
  /// to nullptr and the wrapper cleanly runs the AOT body. Sync is therefore a
  /// single-core debugging mode under the shared taskpool, not a way for every
  /// core to compile.
  bool setCompileMode(CompileMode mode);
  CompileMode getCompileMode() const;
  void setOptimizationLevel(OptimizationLevel level);
  OptimizationLevel getOptimizationLevel() const;

  /// Register a user-defined external symbol for JIT resolution.
  /// Required for bare-metal where dlsym is unavailable.
  void registerSymbol(const std::string &name, void *addr);

  /// Manual registration of bitcode / period arrays / static vars.
  /// These can be called after ejit_init() to register data at runtime
  /// (bare-metal friendly, avoiding global constructors).
  /// registerBitcode returns false on a null/zero payload, funcIndex capacity
  /// exhaustion, or a same-name re-registration with a different payload. In a
  /// taskpool build, all three reject once registration is frozen (after
  /// ejit_init) so the running worker never races a registry write.
  bool registerBitcode(const std::string &funcName, const uint8_t *data,
                       size_t size);
  bool registerPeriodArray(const std::string &periodName,
                           const std::string &varName, void *baseAddr,
                           uint64_t arraySize);
  bool registerStaticVar(const std::string &varName, void *varAddr);

  // Registry access (for C API validation)
  const PeriodArrayRegistry &getRegistry() const {
    return runtimeState_->getRegistry();
  }

  // Statistics

  // Error
  const EJitError *getLastError() const;

  /// True if registration during construction failed (funcIndex/lifecycle
  /// capacity exhausted, a malformed or conflicting bitcode payload, or a null
  /// fixup pointer). ejit_init() returns failure and tears down the instance
  /// rather than exposing a half-registered taskpool.
  bool initFailed() const { return initFailed_; }
  const EJitError &initError() const { return initError_; }

  /// True once registration is frozen (taskpool build: after ejit_init has
  /// consumed all registration data, completed funcIndex/lifecycle fixup and
  /// started the worker). While frozen, every runtime registration entry point
  /// rejects so the single worker never races a lock-free registry write.
  bool registrationFrozen() const {
    return regPhase_ != RegistrationPhase::Open;
  }

#ifdef EJIT_SRE_TASKPOOL
  /// Access the SRE taskpool scheduler (used by the taskpool C ABI). May be
  /// null if the compile driver was not constructed.
  EJitTaskPool *taskPool();
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
  /// Access the cross-core shared taskpool (the taskpool C ABI binds here in a
  /// shared build). May be null if the compile driver was not constructed.
  EJitSharedTaskPool *sharedTaskPool();
  const EJitSharedTaskPool *sharedTaskPool() const;
#endif

  /// Access the module loader (for funcIndex → funcName resolution in
  /// diagnostics). Always constructed; valid for the lifetime of the instance.
  EJitModuleLoader &moduleLoader() { return *moduleLoader_; }

  /// Print the registered registry (bitcodes, period arrays, static vars) and
  /// funcIndex/lifecycle counts through EJIT_DIAG. For ejit_print_registry().
  void printRegistry() const;

  /// Print the !ejit.metadata parsed from \p funcName's registered bitcode
  /// (ejit_entry marker, period_arr_ind slots, period arrays, may_const field
  /// offsets). For ejit_print_func_meta(). Non-const: caches func metadata.
  void printFuncMeta(const std::string &funcName);

  /// Fill \p out with code pool usage stats. Returns false if not initialized
  /// or built without EJIT_SRE_CODE_POOL. For ejit_get_code_pool_stats().
  bool getCodePoolStats(ejit_code_pool_stats_t *out) const;

  /// Fill aggregate plus ABI-compatible near/far statistics. near includes
  /// both hot and MFS-cold fixed pools.
  bool getCodePoolStatsV2(ejit_code_pool_stats_v2_t *out) const;
  /// Fill aggregate plus near-hot/near-cold/far-Tier-1 statistics.
  bool getCodePoolStatsV3(ejit_code_pool_stats_v3_t *out) const;

  /// Print code pool usage stats through EJIT_DIAG. For
  /// ejit_print_code_pool_stats().
  void printCodePoolStats() const;

  /// Print every active (period, cell) through EJIT_DIAG. For ejit_print_active().
  void printActive() const;

  /// Print completed per-entry may_const benefit samples in descending order.
  /// A non-owner facade forwards the request to the compile-owner worker.
  bool printMayConstRanking();

private:
  Config config_;
  std::unique_ptr<EJitRuntimeState> runtimeState_;
  std::unique_ptr<EJitModuleLoader> moduleLoader_;
#ifndef EJIT_FREESTANDING
  std::unique_ptr<EJitLogger> logger_;
#endif
  std::unique_ptr<EJitCompileDriver> compileDriver_;

  /// Record the first construction-time registration failure (later ones are
  /// ignored so the earliest root cause is reported).
  void recordInitError(int code, const std::string &message,
                       const std::string &funcName);
  bool initFailed_ = false;
  EJitError initError_;

  /// Registration lifecycle: Open during construction (and forever in a legacy
  /// build), Frozen once a taskpool init completes, Failed on init error.
  enum class RegistrationPhase { Open, Frozen, Failed };
  RegistrationPhase regPhase_ = RegistrationPhase::Open;
};

} // namespace ejit
} // namespace llvm

#endif
