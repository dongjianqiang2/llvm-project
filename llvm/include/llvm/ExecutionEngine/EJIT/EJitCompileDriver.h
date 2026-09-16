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
#ifdef EJIT_SRE_SHARED_TASKPOOL
// The group lifecycle is only reachable from the cross-core shared driver (the
// only owner of the shared request/observation protocol); a freestanding or
// per-instance build must not pull in the prepared-code/ORC headers.
#include "llvm/ExecutionEngine/EJIT/EJitRepresentativeGroup.h"
#endif
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
#include "llvm/ExecutionEngine/EJIT/EJitBranchProfile.h"
#endif
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h"
#ifdef EJIT_SRE_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#endif
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {
namespace ejit {

class EJitOrcEngine;
class EJitLogger;
struct SpecializationContext;

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
                    EJitModuleLoader &loader, EJitLogger *logger = nullptr);

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

  /// Taskpool commit notification. A value-profile round is consumed only
  /// after Tier-2 is actually published, so failed commits keep collecting.
  void notifyTaskpoolPublished(const EJitCompileRequest &req, bool published);
  void notifyTaskpoolPgoLifecycleDrop(const EJitCompileRequest &req);

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
#ifdef EJIT_SRE_TASKPOOL_TESTING
  void failMemberTier2ForTest(uint32_t Count) { failMemberT2_.storeRelease(Count); }
  uint32_t candidateGateForTest(uint32_t Command) {
    if (Command == 1)
      candidateGate_.storeRelease(1);
    else if (Command == 3)
      candidateGate_.storeRelease(0);
    return candidateGate_.loadAcquire();
  }
  void failNextRepresentativeTier2ForTest() { failRepresentativeT2_.storeRelease(1); }
  void setRepresentativeTimeoutForTest(uint64_t Ticks, uint32_t MaxReelections) {
    repMaxReelections_.storeRelease(MaxReelections);
    repTimeoutTicks_.storeRelease(Ticks);
  }
#endif
  const EJitSharedTaskPool *sharedTaskPool() const { return &sharedPool_; }

  /// Run owner election over the shared state and, if this core becomes the
  /// owner, start the ONE shared worker. Returns false on a clean init failure
  /// (owner worker-start failed / ABI mismatch). Idempotent across instances.
  bool startSharedTaskPool();

  /// Owner-only orderly shutdown of the shared worker (soft-stop + join).
  void stopSharedTaskPool() { sharedPool_.ownerShutdown(); }
#endif

  EJitRuntimeState &getRuntimeState() { return runtimeState_; }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  /// The driver-owned representative-PGO group registry (V1 sharing). Non-null
  /// only while Config::enableRepresentativeSharing is on AND the build really
  /// supports it; a caller reads group/publication state through it instead of
  /// a second bookkeeping table. The registry is touched from the compile owner
  /// (cold path) and from the pool's committed-dispatch hook, so it is the
  /// production serialization point for the group lifecycle.
  bool representativeSnapshot(EJitGroupDiagnostics &D, EJitGroupSnapshot &S,
                              size_t &Groups, EJitFrozenProfileBundle &Bundle,
                              size_t GroupIndex = 0) const;
  EJitRepresentativeGroupRegistry *representativeGroups() {
    return repGroups_.get();
  }
  /// Whether representative sharing really drove this driver (opt-in accepted
  /// by the admission gate). False keeps the unchanged product path.
  bool representativeSharingActive() const { return repGroups_ != nullptr; }
  /// The live registry (null when the opt-in was rejected). Exposed for the
  /// runtime's read-only diagnostics entry point; the compile owner remains the
  /// only writer.
  const EJitRepresentativeGroupRegistry *representativeGroups() const {
    return repGroups_.get();
  }
#endif
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
  /// Pool hook: in-flight sampling admission for one async PGO request. Denies
  /// a personal sampling admission to a member whose candidate group already
  /// owns its ONE representative sampling session.
  static EJitSharedTaskPool::SamplingAdmission
  samplingAdmissionThunk(void *ctx, uint32_t funcIndex,
                         const EJitDimPair *dims, uint32_t numDims);
  /// Pool hook: ONE real granted Tier-1 dispatch. Routes the exact publish
  /// identity into the group registry so the group quota is owned by the real
  /// runtime dispatch, never by a test-side call.
  static void dispatchObserverThunk(void *ctx,
                                    const EJitSharedTaskPool::DispatchObservation &Obs);
  /// Pool hook: build the Tier-2 request of a represented member that owns no
  /// sampling session of its own. Returns false (and leaves \p Out untouched)
  /// while the group has no published bundle, so the member stays on AOT.
  static bool representativeWakeThunk(void *ctx, uint32_t funcIndex,
                                      const EJitDimPair *dims, uint32_t numDims,
                                      EJitCompileRequest &Out);
  /// The production candidate group key of a request identity: the entry's
  /// function identity (funcIdx + dim TYPES) with the per-cell instance ids
  /// removed, so the legal cells of ONE entry share a group and a different
  /// entry - or a different dim-type set for the same entry - is a different
  /// group. Returns 0 when the identity cannot form a group.
  uint64_t candidateGroupKey(uint32_t funcIndex, const EJitDimPair *dims,
                             uint32_t numDims) const;
  /// The per-cell logical identity a request dispatches: funcIdx plus every dim
  /// instance id. This is the ONE place the encoding lives, so admission, the
  /// Tier-1 binding and the physical-code decision can never disagree about
  /// which member a request belongs to.
  uint64_t requestLogicalKey(uint32_t funcIndex, const EJitDimPair *dims,
                             uint32_t numDims) const;
  /// Emit or reuse the group generation's ONE physical Tier-2 object for a
  /// validated group compile. The member's FINAL module is produced by the real
  /// pipeline (EJitOrcEngine::prepareFinalCode, no link), its exact identity
  /// (full IR + effective bindings + scope) is compared against the
  /// generation's object and only then is the emitter called. Returns the
  /// function pointer to publish, or nullptr when the caller must use the
  /// ordinary ORC route (not a legal member record, prepared-code or binding
  /// rejection, physical budget, stale/cancelled member). Never emits code
  /// before the identity compare.
  void *sharePhysicalTier2(const EJitCompileRequest *Request, uint64_t CacheKey,
                           const std::string &FuncName, StringRef Bitcode,
                           const SpecializationContext &Ctx,
                           const EJitGroupHandle &G,
                           const EJitRepresentativeSession &RepSession,
                           bool HaveRepSession);
  /// The pool's admission decision for a request of this identity: Grant (own
  /// the ordinary sampling admission - the representative or a non-grouped
  /// identity), Deny (a represented member with no published bundle: stay on
  /// AOT), or WakeTier2 (a represented member whose group bundle is live).
  static bool candidateClassifyThunk(void *Ctx, const EJitCompileRequest &Req);
  bool classifyRepresentativeRequest(const EJitCompileRequest &Req);
  EJitSharedTaskPool::SamplingAdmission
  admitSamplingRequest(uint32_t funcIndex, const EJitDimPair *dims,
                       uint32_t numDims);
  /// The live group handle of an identity, or an invalid handle when this
  /// identity never formed a group. Never creates one: the cold compile path
  /// must not invent a group for a request the admission gate never grouped.
  EJitGroupHandle repGroupFor(uint32_t funcIndex, const EJitDimPair *dims,
                              uint32_t numDims);
  /// Bind the group's representative session to the REAL Tier-1 compile request
  /// that the pool admitted for it. The pool owns the request-attempt token, the
  /// group owns the group/sampling session; this is the one production place
  /// where the two identities are joined, so a later Tier-2 request can be
  /// proven to belong to the exact sampling session that produced the bundle.
  /// Returns false (with no bound session) for an unknown group or for a
  /// Tier-1 request that is NOT the group's representative member.
  bool bindRepresentativeTier1(const EJitCompileRequest *Request,
                               uint64_t CacheKey,
                               const EJitRepresentativeSession **OutSession);
#endif
  // Async compiler will be added in EJitAsyncCompiler phase

  /// Cold compile path (decode → verify active → load bitcode → JIT compile).
  /// When \p storeLru is true the result is inserted into the LRU EJitCache.
  /// \p tier is the CompileTier (uint32_t, EJitOrcEngine.h) decoded from
  /// the request's funcIndex high bits (EJitSreQueue.h); gated by
  /// Config::enablePgo (off => Baseline regardless).
  void *compileCold(uint64_t cacheKey, uint32_t tier, bool storeLru,
                    const EJitCompileRequest *request = nullptr);

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
  struct Tier1ProfileIdentity {
    uint64_t samplingSessionId = 0;
    uint64_t representativeAttemptToken = 0;
    uint32_t generation = 0;
    /// Representative-PGO group identity of the session (0 when this Tier-1 is
    /// not a group representative). The pool's request identity alone cannot
    /// name the group, so it is recorded with the same capture.
    uint64_t groupId = 0;
    uint64_t groupGeneration = 0;
    uint32_t numDims = 0;
    uint64_t versions[kEJitMaxRequestDims] = {};
  };
  std::unordered_map<uint64_t, Tier1ProfileIdentity> tier1ProfileIdentities_;
  std::unordered_map<uint64_t, EJitFrozenProfileBundle> frozenProfileBundles_;
  uint64_t nextSamplingSessionId_ = 1;
#ifdef EJIT_SRE_SHARED_TASKPOOL
  /// V1 representative sharing (default OFF). Created only when the opt-in
  /// passes the admission gate, so a product default build never allocates or
  /// consults it.
  std::unique_ptr<EJitRepresentativeGroupRegistry> repGroups_;
  /// The accepted V1 admission policy of this driver (valid only while
  /// repGroups_ is non-null).
  EJitGroupAdmissionPolicy repAdmissionPolicy_;
  /// Candidate-group key -> live group handle. Only ever touched on the compile
  /// owner (admission runs on the producer wavefront before the pool claims the
  /// request, and the group handle is published with the sampling admission).
  std::unordered_map<uint64_t, EJitGroupHandle> repGroupHandles_;
  /// Attempt token -> the elected representative session, so the pool's
  /// committed-dispatch hook routes by EXACT publish identity instead of
  /// guessing which group a Tier-1 pointer belonged to.
  std::unordered_map<uint64_t, EJitRepresentativeSession> repSessions_;
  /// The representative binding of one group generation: the REAL Tier-1
  /// request the pool admitted for the elected representative. The pooled
  /// attempt token is the identity a later Tier-2 request carries, so this is
  /// what proves both requests belong to the same sampling session.
#ifdef EJIT_SRE_TASKPOOL_TESTING
  EJitAtomicU32 failRepresentativeT2_{0};
  EJitAtomicU32 candidateGate_{0};
  EJitAtomicU32 failMemberT2_{0};
#endif
  static bool representativeMaintenanceThunk(void *Ctx);
  bool serviceRepresentativeTimeouts();
  bool serviceRepresentativeWaiters();
  void completeCandidateBorrow(uint64_t LogicalKey, const EJitCompileRequest *Request = nullptr);
  EJitAtomicU64 repTimeoutTicks_{0};
  EJitAtomicU32 repMaxReelections_{0};
  std::unordered_map<uint64_t, uint32_t> repTimeoutCounts_;
  std::unordered_map<uint64_t, bool> repRetiringGroups_;
  std::unordered_map<uint64_t, bool> repFailedGroups_;
  void refreshRepresentativeGroup(uint64_t GroupKey);
  struct RepTier1Binding {
    EJitGroupHandle group;
    EJitRepresentativeSession session;
    uint64_t requestAttemptToken = 0;
    uint32_t requestGeneration = 0;
    uint64_t logicalKey = 0;
    uint64_t lastProgressAt = 0;
    EJitCompileRequest requestIdentity{};
  };
  std::vector<RepTier1Binding> repTier1Bindings_;
  /// Every waiter token this driver registered, so a coalesced/retried request
  /// never adds a second logical member for the same cell.
  std::vector<EJitWaiterToken> repWaiters_;
  /// Serializes the representative-group lifecycle across the threads that can
  /// reach it: the producer wavefront (sampling admission) and the real granted
  /// dispatches (the pool's committed-return hook). It never guards a stable
  /// published-Tier-2 wrapper hit - those return AOT/terminal results without
  /// entering any of this.
  struct CandidateBinding {
    uint64_t groupId = 0;
    EJitCompileRequest request{};
    bool finalReadPending = true;
    bool finalFailed = false;
    uint32_t finalFailures = 0;
    uint64_t finalReadyAt = 0;
  };
  std::unique_ptr<EJitCandidateDirectory> candidateDirectory_;
  std::unordered_map<uint64_t, CandidateBinding> candidateBindings_;
  mutable EJitAtomicU32 repGroupLock_{0};
  void lockRepGroups() const {
    uint32_t Expected = 0;
    while (!repGroupLock_.compareExchange(Expected, 1))
      Expected = 0;
  }
  void unlockRepGroups() const { repGroupLock_.storeRelease(0); }
  /// The candidate-group key of each group id, so the compact key and the
  /// identity-bearing group id stay distinguishable in diagnostics.
  struct RepGroupIdentity {
    uint64_t candidateKey = 0;
    uint32_t funcIndex = 0;
    uint32_t numDims = 0;
    EJitDimPair dims[kEJitMaxRequestDims] = {};
  };
  std::vector<RepGroupIdentity> repGroupIdentities_;
#endif
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  struct Tier1MayConstState {
    uintptr_t counterBase = 0;
    uint64_t sampleStart = 0;
    std::vector<EJitMayConstLoadSite> sites;
  };
  std::unordered_map<uint64_t, Tier1MayConstState> tier1MayConst_;
  /// The owner worker compiles and publishes serially, so the publish callback
  /// can use this key to timestamp the Tier-1 sample window exactly at publish.
  uint64_t pendingTier1MayConstKey_ = 0;
  bool hasPendingTier1MayConstKey_ = false;
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
  /// Partial destructive snapshots retained across bounded T2 retries, keyed
  /// by the exact shared sampling-session identity.
  std::unordered_map<uint64_t, std::vector<EJitVpSiteSample>> pendingVpSamples_;
#endif
};

} // namespace ejit
} // namespace llvm

#endif
