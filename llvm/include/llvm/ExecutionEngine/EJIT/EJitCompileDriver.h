//===-- EJitCompileDriver.h - Compilation Scheduler -----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILEDRIVER_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITCOMPILEDRIVER_H

#include "llvm/ExecutionEngine/EJIT/EJitModuleLoader.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#ifdef EJIT_SRE_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#endif
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {
namespace ejit {

class EJitOrcEngine;
class EJitLogger;

/// Unified entry point for sync and async compilation. Handles cache
/// lookup, time-window state verification, bitcode retrieval, and
/// compilation dispatch.
class EJitCompileDriver {
public:
  struct Result {
    void *funcPtr = nullptr;
    size_t compileTimeMs = 0;
    size_t codeSize = 0;
  };

  EJitCompileDriver(const Config &config,
                    EJitRuntimeState &runtimeState, EJitModuleLoader &loader,
                    EJitLogger *logger = nullptr);

  ~EJitCompileDriver();

  /// Hot path: cache lookup on pre-computed uint64_t cacheKey.
  /// Cold path: decode cacheKey → load bitcode → JIT compile.
  /// Returns nullptr on miss that cannot be compiled (time window not
  /// active, no bitcode, or compile failure).

#ifdef EJIT_SRE_TASKPOOL
  /// Cold compile path WITHOUT storing into the LRU EJitCache. Used as the
  /// taskpool's compile callback (the taskpool owns its own fixed cache).
  /// Returns the JIT function pointer or nullptr.
  void *compileNow(const EJitCompileRequest &req);

  /// The SRE taskpool scheduler (non-null when EJIT_SRE_TASKPOOL is built).
  EJitTaskPool *taskPool() { return taskPool_.get(); }

  /// Start the taskpool's single async worker. Called by EJit ONLY after all
  /// registration is consumed/frozen and the ORC engine is installed. Returns
  /// false if the worker could not be started.
  bool startTaskPoolWorker() { return taskPool_ && taskPool_->startWorker(); }

  /// Whether the taskpool worker is currently running (test/diagnostic).
  bool isTaskPoolWorkerRunning() const {
    return taskPool_ && taskPool_->isWorkerRunning();
  }

  bool hasJitEngine() const { return jitEngine_ != nullptr; }

  void stopTaskPoolWorker() {
    if (taskPool_)
      taskPool_->stopWorker();
  }
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
  /// The cross-core shared taskpool driving the process-global shared state.
  /// When EJIT_SRE_SHARED_TASKPOOL is built, the taskpool C ABI binds here
  /// instead of the per-instance taskPool_.
  EJitSharedTaskPool *sharedTaskPool() { return &sharedPool_; }
  const EJitSharedTaskPool *sharedTaskPool() const { return &sharedPool_; }

  /// Run owner election over the shared state and, if this core becomes the
  /// owner, start the ONE shared worker. Returns false on a clean init failure
  /// (owner worker-start failed / ABI mismatch). Idempotent across instances.
  bool startSharedTaskPool();

  /// Owner-only orderly shutdown of the shared worker (soft-stop + join).
  void stopSharedTaskPool() { sharedPool_.ownerShutdown(); }
#endif

  /// The dedup mode actually in effect for the next compile: the configured
  /// mode, force-lowered to Off whenever a physical-code releaser is wired on
  /// any taskpool (a dedup hit hands the release paths a fnPtr shared with
  /// other identities - EJIT_SPECIALIZATION_DEDUP.md §5.5 hard gate). Also
  /// clears the engine's dedup index so no stale aliases survive the wiring.
  DedupMode effectiveDedupMode();

  EJitRuntimeState &getRuntimeState() { return runtimeState_; }
  EJitModuleLoader &getLoader() { return loader_; }
  const Config &getConfig() { return config_; }
  EJitOrcEngine *getJitEngine() { return jitEngine_.get(); }
#ifndef EJIT_FREESTANDING
  EJitLogger *getLogger() { return logger_; }
#else
  EJitLogger *getLogger() { return nullptr; }
#endif

  void setJitEngine(std::unique_ptr<EJitOrcEngine> engine);

  /// Build the ORC engine on THIS core if one is not already installed.
  /// Idempotent, and never replaces a live engine (published code is reached
  /// through it). Under the shared taskpool this is the owner-elected hook:
  /// ownership is won by CAS and released by ownerShutdown, so the engine must
  /// follow whoever wins, whenever they win. Hence a member of a lifetime-
  /// stable object rather than a lambda over a caller's stack.
  bool ensureJitEngine();

  /// Drop this core's ORC engine. Called when ownership is given up, so a
  /// handoff does not leave one engine per former owner. Safe for already
  /// published code: the code pool never recycles memory, so specializations
  /// peers are still running outlive the compiler that produced them.
  void releaseJitEngine();

  /// Stage a user symbol for the engine. The list is durable because the engine
  /// may not exist yet: a peer elected owner after a re-election builds its
  /// engine long after registration is over and must still see every symbol.
  void registerSymbol(const std::string &name, void *addr);

private:
  const Config &config_;
  EJitRuntimeState &runtimeState_;
  EJitModuleLoader &loader_;
#ifndef EJIT_FREESTANDING
  EJitLogger *logger_;
#endif

  std::unique_ptr<EJitOrcEngine> jitEngine_;
  /// Durable record of every user symbol, replayed into whichever engine this
  /// driver ends up building (see ensureJitEngine).
  std::vector<std::pair<std::string, void *>> userSymbols_;
#ifdef EJIT_SRE_TASKPOOL
  std::unique_ptr<EJitTaskPool> taskPool_;
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJitSharedTaskPool sharedPool_;
  EJitSreTask sharedWorkerTask_;
  // Worker start/stop adapters bridging the shared pool to the platform task
  // abstraction (host std::thread / SRE platform task). Static so the shared
  // pool can call them through plain function pointers (never std::function).
  static bool sharedWorkerStart(void *ctx,
                                EJitSharedTaskPool::WorkerEntryFn entry,
                                void *entryCtx, uint64_t *outTaskId);
  static void sharedWorkerStop(void *ctx);
  /// Worker idle/delay hook: defers to the platform task abstraction
  /// (EJitSreTask::delay) so the shared worker never busy-spins. ticks=1 is a
  /// single yield; ticks=MULT*DELAY_TICKS is the post-task throttle delay.
  static void sharedWorkerIdle(void *ctx, uint32_t ticks);
  /// Owner-elected hook: builds the engine on whichever core wins the election.
  /// ctx is the driver, which owns sharedPool_ and so always outlives it.
  static bool sharedOwnerElected(void *ctx);
  /// Owner-release hook: the counterpart of sharedOwnerElected.
  static void sharedOwnerReleased(void *ctx);
#endif
  // Async compiler will be added in EJitAsyncCompiler phase

  /// Cold compile path (decode → verify active → load bitcode → JIT compile).
  /// When \p storeLru is true the result is inserted into the LRU EJitCache.
  void *compileCold(uint64_t cacheKey, bool storeLru);
};

} // namespace ejit
} // namespace llvm

#endif
