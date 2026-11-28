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
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#endif
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitSreTask.h"
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
namespace ejit {

class EJitOrcEngine;
class EJitAsan;

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

  EJitCompileDriver(const Config &config, EJitRuntimeState &runtimeState,
                    EJitModuleLoader &loader);

  ~EJitCompileDriver();

  /// Cold compile path WITHOUT storing into the LRU EJitCache. Used as the
  /// taskpool's compile callback (the taskpool owns its own fixed cache).
  /// Returns the JIT function pointer or nullptr.
  void *compileNow(const EJitCompileRequest &req, bool CollectAsan = false);

  /// Taskpool commit notification. A value-profile round is consumed only
  /// after Tier-2 is actually published, so failed commits keep collecting.
  void notifyTaskpoolPublished(const EJitCompileRequest &req, bool published);

  bool hasJitEngine() const { return jitEngine_ != nullptr; }

  /// The cross-core shared taskpool driving the process-global shared state.
  EJitSharedTaskPool *sharedTaskPool() { return &sharedPool_; }
  const EJitSharedTaskPool *sharedTaskPool() const { return &sharedPool_; }

  /// Run owner election over the shared state and, if this core becomes the
  /// owner, start the ONE shared worker. Returns false on a clean init failure
  /// (owner worker-start failed / ABI mismatch). Idempotent across instances.
  bool startSharedTaskPool();

  /// Owner-only orderly shutdown of the shared worker (soft-stop + join).
  void stopSharedTaskPool() { sharedPool_.ownerShutdown(); }

  EJitRuntimeState &getRuntimeState() { return runtimeState_; }
  EJitModuleLoader &getLoader() { return loader_; }
  const Config &getConfig() { return config_; }
  EJitOrcEngine *getJitEngine() { return jitEngine_.get(); }

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
  /// A dequeued request has already passed the shared-record boundary.
  /// CollectAsan additionally records the asan watch capture (all periods)
  /// when the feature is compiled in.
  void *compileNowValidated(const EJitCompileRequest &req,
                            bool CollectAsan = false);
  static bool taskpoolCompileThunk(void *ctx, const EJitCompileRequest &req,
                                   void **outFn);

  const Config &config_;
  EJitRuntimeState &runtimeState_;
  EJitModuleLoader &loader_;

  std::unique_ptr<EJitOrcEngine> jitEngine_;
  /// Durable record of every user symbol, replayed into whichever engine this
  /// driver ends up building (see ensureJitEngine).
  std::vector<std::pair<std::string, void *>> userSymbols_;
  EJitSharedTaskPool sharedPool_;
#ifdef EJIT_ASAN
  std::unique_ptr<EJitAsan> asan_;
#endif
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
  // Async compiler will be added in EJitAsyncCompiler phase

  /// Cold compile path (decode → verify active → load bitcode → JIT compile).
  /// When \p storeLru is true the result is inserted into the LRU EJitCache.
  /// \p tier is the CompileTier (uint32_t, EJitOrcEngine.h) decoded from
  /// the request's funcIndex high bits (EJitTaskPoolTypes.h); gated by
  /// Config::enablePgo (off => Baseline regardless).
  void *compileCold(uint64_t cacheKey, uint32_t tier, bool storeLru,
                    const EJitCompileRequest *request = nullptr,
                    bool CollectAsan = false);

  /// PGO: Tier-1 captured counter refs per cacheKey (EJIT_ONLINE_PGO.md
  /// §5.2). Filled after a Tier-1 compile (ORC lookup of __profc_/__profd_
  /// by the optimizer's captured names); consumed by a Tier-2 compile to
  /// synthesize the in-memory profile before loadBitcode (§5.3).
  struct Tier1CounterInfo {
    std::string pgoName;
    uintptr_t profcAddr = 0;
    uintptr_t profdAddr = 0;
  };
  std::unordered_map<uint64_t, std::vector<Tier1CounterInfo>> tier1Counters_;
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  struct Tier1PeriodConstState {
    uintptr_t counterBase = 0;
    uint64_t sampleStart = 0;
    std::vector<EJitPeriodConstLoadSite> sites;
  };
  std::unordered_map<uint64_t, Tier1PeriodConstState> tier1PeriodConst_;
  /// The owner worker compiles and publishes serially, so the publish callback
  /// can use this key to timestamp the Tier-1 sample window exactly at publish.
  uint64_t pendingTier1PeriodConstKey_ = 0;
  bool hasPendingTier1PeriodConstKey_ = false;
#endif
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  /// Value-profile capture per cacheKey (EJIT_VALUE_PROFILE.md §5.1): the
  /// verified target table (runtime address -> IR-PGO-name MD5) and the
  /// function site inventory, filled after a Tier-1 compile and consumed by
  /// the Tier-2 merge. The address table is verified at capture time: only
  /// symbols the engine resolves (module functions via ORC lookup, externals
  /// via registered user symbols) are entered.
  struct Tier1VpState {
    std::vector<PgoValueTarget> targets;
    std::vector<PgoValueFunction> functions;
  };
  std::unordered_map<uint64_t, Tier1VpState> tier1Vp_;
  /// Active collection rounds (Tier-1 publishes not yet consumed by a Tier-2
  /// merge), touched only by the single owner worker: the collector is armed
  /// at the first Tier-1 capture and disarmed once the last round merged.
  uint32_t vpRoundsActive_ = 0;
#endif
};

} // namespace ejit
} // namespace llvm

#endif
