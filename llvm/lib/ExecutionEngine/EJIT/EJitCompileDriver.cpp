//===-- EJitCompileDriver.cpp - Compilation Scheduler ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include <cassert>
#ifndef EJIT_FREESTANDING
#include "llvm/ExecutionEngine/EJIT/EJitLogger.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRepresentativeGroup.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/Support/SHA256.h"
#endif
#ifndef EJIT_FREESTANDING
#include <chrono>
#endif

using namespace llvm;
using namespace llvm::ejit;

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
namespace {
constexpr unsigned kVpSnapshotAttempts = 3;

bool retireVpSession(uint64_t SessionId, std::vector<EJitVpSiteSample> &Samples,
                     bool PreserveForRetry = false) {
  ejitVpEndSession(SessionId, PreserveForRetry);
  for (unsigned Attempt = 0; Attempt < kVpSnapshotAttempts; ++Attempt)
    if (ejitVpTakeSessionSnapshot(SessionId, Samples))
      return true;
  return false;
}

class VpSessionAbortGuard {
public:
  ~VpSessionAbortGuard() {
    if (!SessionId)
      return;
    std::vector<EJitVpSiteSample> Discarded;
    (void)retireVpSession(SessionId, Discarded);
  }

  void arm(uint64_t Id) { SessionId = Id; }
  void dismiss() { SessionId = 0; }

private:
  uint64_t SessionId = 0;
};
} // namespace
#endif

#ifdef EJIT_SRE_TASKPOOL
namespace {
/// Adapter so the taskpool can call back into the driver's cold compile path
/// through a plain function pointer (never std::function). The produced JIT
/// pointer still comes from the OrcJIT engine (SRE code pool when enabled).
bool taskpoolCompileThunk(void *ctx, const EJitCompileRequest &req,
                          void **outFn) {
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  void *fn = drv->compileNow(req);
  *outFn = fn;
  return fn != nullptr;
}

void taskpoolPublishThunk(void *ctx, const EJitCompileRequest &req,
                          bool published) {
  static_cast<EJitCompileDriver *>(ctx)->notifyTaskpoolPublished(req,
                                                                 published);
}

void taskpoolPgoLifecycleDropThunk(void *ctx, const EJitCompileRequest &req) {
  static_cast<EJitCompileDriver *>(ctx)->notifyTaskpoolPgoLifecycleDrop(req);
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
// Owner-private timestamp source for the observed Tier-1 dispatch boundary
// (quotaEnd). It is a thunk on purpose: the taskpool TU stays independent of
// the runtime library (its focused test links only LLVMSupport) and a test can
// inject a deterministic clock instead.
uint64_t sharedTraceClockThunk(void * /*ctx*/) {
  return ejit_taskpool_trace_now();
}

[[maybe_unused]] bool sharedPrepareCodeThunk(void * /*ctx*/,
                                             const void *fnPtr) {
#ifdef EJIT_SRE_CODE_POOL
  return prepareSreCodeForCurrentCore(fnPtr);
#else
  (void)fnPtr;
  return false;
#endif
}

// Owner-private: resolve a freshly compiled pointer to its real, finalized
// executable range + owning pool (from the code-pool allocation metadata) so it
// can be published into the shared cache slot for cross-core 4K sealing.
[[maybe_unused]] bool sharedCodeRangeThunk(void *ctx, const void *fnPtr,
                                           EJitCompiledCodeInfo *outInfo) {
#ifdef EJIT_SRE_CODE_POOL
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  EJitOrcEngine *eng = drv->getJitEngine();
  if (eng && outInfo)
    return eng->findCodeRange(fnPtr, *outInfo);
  return false;
#else
  (void)ctx;
  (void)fnPtr;
  (void)outInfo;
  return false;
#endif
}

[[maybe_unused]] bool sharedCodeReadyThunk(void *ctx, const void *fnPtr) {
#if defined(EJIT_SRE_CODE_POOL) && defined(EJIT_CODE_POOL_BATCHED_PUBLISH)
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  EJitOrcEngine *eng = drv->getJitEngine();
  return eng && eng->isCodeReady(fnPtr);
#else
  (void)ctx;
  (void)fnPtr;
  return true;
#endif
}

[[maybe_unused]] bool sharedCodeBatchFlushThunk(void *ctx) {
#if defined(EJIT_SRE_CODE_POOL) && defined(EJIT_CODE_POOL_BATCHED_PUBLISH)
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  EJitOrcEngine *eng = drv->getJitEngine();
  if (!eng)
    return false;
  if (auto Err = eng->flushPendingCode()) {
    EJIT_DIAG("batch enable failed: %s", toString(std::move(Err)).c_str());
    return false;
  }
  return true;
#else
  (void)ctx;
  return true;
#endif
}

/// Owner-private provider: snapshot the owner-core code-pool manager stats for
/// the shared taskpool to mirror cross-core (see CodePoolStatsCallback). The
/// pools are owner-private, so without this a non-owner core's
/// ejit_print_code_pool_stats reads its own empty per-core manager.
[[maybe_unused]] bool sharedCodePoolStatsThunk(void *ctx,
                                               EJitCodePoolStatsOut *out) {
#ifdef EJIT_SRE_CODE_POOL
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  EJitOrcEngine *eng = drv->getJitEngine();
  if (eng && out) {
    EJitTieredCodePoolStats tiered = eng->getTieredCodePoolStats();
    const EJitCodePoolManager::Stats &s = tiered.total;
    out->poolCount = s.poolCount;
    out->sealedCount = s.sealedCount;
    out->activeCount = s.activeCount;
    out->usedBytes = s.usedBytes;
    out->reservedBytes = s.reservedBytes;
    out->wastedBytes = s.wastedBytes;
    out->sealInvocations = s.sealInvocations;
    out->splitInvocations = s.splitInvocations;
    out->finalizedRangeCount = s.finalizedRangeCount;
    auto CopyDetail = [](EJitCodePoolStatsOut::Detail &Dst,
                         const EJitCodePoolManager::Stats &Src) {
      Dst.poolCount = Src.poolCount;
      Dst.sealedCount = Src.sealedCount;
      Dst.activeCount = Src.activeCount;
      Dst.usedBytes = Src.usedBytes;
      Dst.reservedBytes = Src.reservedBytes;
      Dst.wastedBytes = Src.wastedBytes;
      Dst.sealInvocations = Src.sealInvocations;
      Dst.splitInvocations = Src.splitInvocations;
      Dst.finalizedRangeCount = Src.finalizedRangeCount;
    };
    CopyDetail(out->near, tiered.near);
    CopyDetail(out->far, tiered.far);
    return true;
  }
  return false;
#else
  (void)ctx;
  (void)out;
  return false;
#endif
}

bool sharedMayConstRankingThunk(void *ctx) {
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  EJitOrcEngine *engine = drv->getJitEngine();
  return engine && engine->printMayConstRanking();
}

// Per-core platform primitives wrapped so the shared taskpool core never names
// an SRE symbol directly (spec §7). Both are no-ops returning false when the
// code pool / seal support is not built.
[[maybe_unused]] bool sharedSplitPoolThunk(void * /*ctx*/, uintptr_t poolBase,
                                           uint64_t poolSize) {
#ifdef EJIT_SRE_CODE_POOL
  return ejitSreSplitPoolForCurrentCore(poolBase, poolSize);
#else
  (void)poolBase;
  (void)poolSize;
  return false;
#endif
}

[[maybe_unused]] bool sharedSealPageThunk(void * /*ctx*/, uintptr_t pageVA) {
#ifdef EJIT_SRE_CODE_POOL
  return ejitSreSealPageForCurrentCore(pageVA);
#else
  (void)pageVA;
  return false;
#endif
}

// Per-core enable_rw for a JIT function's runtime-writable data pages (e.g.
// Tier-1 __profc_): a non-owner core must make these RW in its own translation
// context before executing code that writes them (the fixed .text.ejit segment
// is RX on every core). Only meaningful in 4K-seal fixed-code-pool builds.
[[maybe_unused]] bool sharedEnableRwPageThunk(void * /*ctx*/,
                                              uintptr_t pageVA) {
#ifdef EJIT_SRE_CODE_POOL
  return ejitSreEnableRwPageForCurrentCore(pageVA);
#else
  (void)pageVA;
  return false;
#endif
}
#endif
} // namespace
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
namespace {
// The single process-global shared taskpool state. Placed in the cross-core
// shared section (an empty attribute on host, where one address space already
// exists). Every EJit instance's driver binds to THIS same blob and elects a
// single worker owner across cores via CAS.
EJIT_SHARED_SECTION EJitSharedTaskPoolState gEJitSharedTaskPoolState;
} // namespace
#endif

EJitCompileDriver::EJitCompileDriver(const Config &config,
                                     EJitRuntimeState &runtimeState,
                                     EJitModuleLoader &loader,
                                     EJitLogger *logger)
    : config_(config), runtimeState_(runtimeState), loader_(loader)
#ifndef EJIT_FREESTANDING
      ,
      logger_(logger)
#endif
{
#ifdef EJIT_SRE_TASKPOOL
  // Build the unified scheduler with the worker STOPPED. The worker must not
  // run until EJit has consumed all registration, completed the funcIndex/
  // lifecycle fixup, frozen registration, and installed the ORC engine — EJit
  // calls startTaskPoolWorker() once everything is ready (spec §3.4).
  taskPool_ = std::make_unique<EJitTaskPool>(EJIT_SRE_TASKPOOL_QUEUE_CAPACITY,
                                             /*autoStartWorker=*/false);
  taskPool_->setCompiler(&taskpoolCompileThunk, this);
  taskPool_->setPublishCallback(&taskpoolPublishThunk, this);
  taskPool_->setPgoLifecycleDropCallback(&taskpoolPgoLifecycleDropThunk, this);
  taskPool_->switchController().setMode(
      config_.compileMode == CompileMode::Async  ? EJitCompileMode::Async
      : config_.compileMode == CompileMode::Sync ? EJitCompileMode::Sync
                                                 : EJitCompileMode::Off);
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Bind the cross-core shared pool to the process-global shared state and wire
  // the owner-private hooks. Election + the single worker start happen later in
  // startSharedTaskPool() (called by EJit once registration is frozen and the
  // ORC engine is installed). Cross-core fnPtr sharing is OFF by default until
  // a platform asserts same-VA, sealed, I/D-coherent code (spec §11).
  sharedPool_.bind(&gEJitSharedTaskPoolState);
  sharedPool_.setCompiler(&taskpoolCompileThunk, this);
  // Observed Tier-1 dispatch boundary clock (experimental sharing contract):
  // quotaEnd is frozen at the final granted Tier-1 dispatch, so it must come
  // from the runtime trace clock, never from a Tier-2 compile-time timestamp.
  sharedPool_.setTraceClock(&sharedTraceClockThunk, nullptr);
  sharedPool_.setPublishCallback(&taskpoolPublishThunk, this);
  sharedPool_.setPgoLifecycleDropCallback(&taskpoolPgoLifecycleDropThunk, this);
  // Representative-PGO group sharing (V1, default OFF). The opt-in is accepted
  // only when the V1 admission policy really holds for this live configuration:
  // Async + normal online PGO with a nonzero dispatch quota. Anything else
  // leaves repGroups_ null, so the pool hooks stay unset and the product path
  // is byte-for-byte the unchanged one.
  if (config_.enableRepresentativeSharing) {
    EJitGroupAdmissionPolicy Policy;
    Policy.pgoEnabled = config_.enablePgo;
    Policy.asyncService = config_.compileMode == CompileMode::Async;
    // Match ctx.profileAuditOnly below: diagnostics do not disable online PGO.
    Policy.normalOnlinePgo = config_.enablePgo;
    Policy.modeChangeInFlight = false;
    Policy.dispatchQuota = kEJitRepresentativeDispatchQuota;
    Policy.poolGeneration = 0;
    if (EJitRepresentativeGroupRegistry::admissionReject(Policy) ==
        EJitGroupAdmitReject::None) {
      repGroups_ = std::make_unique<EJitRepresentativeGroupRegistry>();
      candidateDirectory_ = std::make_unique<EJitCandidateDirectory>();
      repTimeoutTicks_.storeRelease(config_.representativeIdleTimeoutTicks);
      repMaxReelections_.storeRelease(config_.representativeMaxReelections);
      sharedPool_.setOwnerMaintenanceCallback(&representativeMaintenanceThunk, this);
      sharedPool_.setCandidateClassifyCallback(&candidateClassifyThunk, this);
      repAdmissionPolicy_ = Policy;
      sharedPool_.setSamplingAdmissionCallback(&samplingAdmissionThunk, this);
      sharedPool_.setRepresentativeWakeCallback(&representativeWakeThunk,
                                                this);
      sharedPool_.setDispatchObserver(&dispatchObserverThunk, this);
    } else {
      EJIT_DIAG("representative sharing opt-in rejected before any group/queue "
                "side effect");
    }
  } else {
    EJIT_DIAG_VERBOSE("representative sharing not enabled by config");
  }
  sharedPool_.setMayConstRankingCallback(&sharedMayConstRankingThunk, this);
  sharedPool_.setWorkerHooks(&EJitCompileDriver::sharedWorkerStart,
                             &EJitCompileDriver::sharedWorkerStop, this);
  // Inject the platform yield so the worker never busy-spins while waiting for
  // Ready or on an empty queue (spec §11): a high-priority worker that spun
  // could starve the owner core trying to publish Ready / a producer enqueuing.
  sharedPool_.setWorkerIdleHook(&EJitCompileDriver::sharedWorkerIdle, this);
  // Owner-only setup, armed for the pool's WHOLE lifetime with a stable ctx.
  // It must stay armed after the first election: ownerShutdown() returns the
  // blob to Uninitialized, so a later init() can elect a DIFFERENT peer, and a
  // peer that won with no hook would publish Ready with no engine.
  sharedPool_.setOwnerElectedCallback(&EJitCompileDriver::sharedOwnerElected,
                                      this);
  sharedPool_.setOwnerReleasedCallback(&EJitCompileDriver::sharedOwnerReleased,
                                       this);
  sharedPool_.setMode(
      config_.compileMode == CompileMode::Async  ? EJitCompileMode::Async
      : config_.compileMode == CompileMode::Sync ? EJitCompileMode::Sync
                                                 : EJitCompileMode::Off);
  // Cross-core fnPtr sharing is gated by the build capability flag
  // EJIT_SRE_SHARED_CODE_POINTERS (default OFF -> clean fallback for non-owner
  // cores). Only the platform may assert same-VA + sealed + I/D-cache-coherent
  // code (spec §11); we never auto-detect it.
#ifdef EJIT_SRE_CODE_POOL
  // Owner side (always useful when a code pool exists): resolve each compiled
  // pointer to its real executable range so the published cache slot carries
  // the extent a peer must seal. Harmless when sharing is off (no peer reads
  // it).
  sharedPool_.setCodeRangeProvider(&sharedCodeRangeThunk, this);
  // Mirror the owner-core code-pool stats into the shared state so every core's
  // ejit_print_code_pool_stats is consistent (the pools are owner-private).
  sharedPool_.setCodePoolStatsProvider(&sharedCodePoolStatsThunk, this);
#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
  sharedPool_.setCodeBatchCallbacks(&sharedCodeReadyThunk,
                                    &sharedCodeBatchFlushThunk, this);
#endif
#endif
#ifdef EJIT_SRE_SHARED_CODE_POINTERS
  sharedPool_.setCodeSharingEnabled(true);
#ifdef EJIT_CODE_POOL_4K_SEAL
  // 4K page seal: a non-owner core splits its 2MiB pool once and then seals
  // exactly the 4KiB pages the code covers, in its own translation context.
  sharedPool_.setSealMode(true);
  sharedPool_.setSplitPoolCallback(&sharedSplitPoolThunk, this);
  sharedPool_.setSealPageCallback(&sharedSealPageThunk, this);
  // And, for a JIT function with runtime-writable data (Tier-1 __profc_
  // counters), make those data pages writable per-core before execution.
  sharedPool_.setEnableRwPageCallback(&sharedEnableRwPageThunk, this);
#else
  // Legacy whole-2MiB-pool seal: align fnPtr to its pool base and enable_ex.
  sharedPool_.setSealMode(false);
  sharedPool_.setPrepareCodeCallback(&sharedPrepareCodeThunk, this);
#endif
#else
  sharedPool_.setCodeSharingEnabled(false);
#endif
#endif
}

EJitCompileDriver::~EJitCompileDriver() {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Stop + join the single shared worker (if this driver is the owner) BEFORE
  // owner-private ORC/driver state is destroyed — no use-after-free.
  sharedPool_.ownerShutdown();
#endif
}

#ifdef EJIT_SRE_SHARED_TASKPOOL
bool EJitCompileDriver::representativeSnapshot(
    EJitGroupDiagnostics &D, EJitGroupSnapshot &S, size_t &Groups,
    EJitFrozenProfileBundle &Bundle, size_t GroupIndex) const {
  lockRepGroups();
  const bool Active = repGroups_ != nullptr;
  if (Active) {
    D = repGroups_->diagnostics();
    S = repGroups_->snapshotAt(GroupIndex);
    Groups = repGroups_->groupCount();
    if (S.valid)
      Bundle = repGroups_->bundleFor({S.groupId, S.generation});
  }
  unlockRepGroups();
  return Active;
}

bool EJitCompileDriver::sharedWorkerStart(
    void *ctx, EJitSharedTaskPool::WorkerEntryFn entry, void *entryCtx,
    uint64_t *outTaskId) {
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  if (!EJitSreTask::create(drv->sharedWorkerTask_, entry, entryCtx,
                           "ejit-shared-worker")) {
    EJIT_DIAG("shared worker start FAILED: SRE task create rejected");
    return false;
  }
  if (outTaskId)
    *outTaskId = 1; // host has no numeric task id; diagnostic only.
  EJIT_DIAG("shared worker started");
  return true;
}

void EJitCompileDriver::sharedWorkerStop(void *ctx) {
  EJitSreTask::destroy(
      static_cast<EJitCompileDriver *>(ctx)->sharedWorkerTask_);
}

void EJitCompileDriver::sharedWorkerIdle(void * /*ctx*/, uint32_t ticks) {
  // Platform delay: SRE_TaskDelay(ticks) on freestanding, ticks yields on host.
  // ticks=1 is a single yield (idle/wait); ticks=MULT*DELAY_TICKS is the
  // post-task throttle delay. The shared taskpool core never names
  // SRE_TaskDelay directly -- it goes through this injected hook (delay(1) ==
  // yield()).
  EJitSreTask::delay(ticks);
}

bool EJitCompileDriver::sharedOwnerElected(void *ctx) {
  return static_cast<EJitCompileDriver *>(ctx)->ensureJitEngine();
}

void EJitCompileDriver::sharedOwnerReleased(void *ctx) {
  static_cast<EJitCompileDriver *>(ctx)->releaseJitEngine();
}

uint64_t EJitCompileDriver::candidateGroupKey(uint32_t funcIndex,
                                              const EJitDimPair *dims,
                                              uint32_t numDims) const {
  const uint64_t LogicalKey = requestLogicalKey(funcIndex, dims, numDims);
  lockRepGroups();
  auto It = candidateBindings_.find(LogicalKey);
  uint64_t Group = 0;
  if (It != candidateBindings_.end()) {
    const auto &R = It->second.request;
    bool Current = R.numDims == numDims && sharedPool_.state() &&
                   R.generation == sharedPool_.state()->generation.loadAcquire();
    for (uint32_t I = 0; Current && I < numDims; ++I)
      Current = dims && R.dims[I].dimType == dims[I].dimType &&
                R.dims[I].instanceId == dims[I].instanceId &&
                R.versions[I] == sharedPool_.instanceVersionPublic(
                    dims[I].dimType, dims[I].instanceId);
    const auto Failed = repFailedGroups_.find(It->second.groupId);
    const bool Terminal = Failed != repFailedGroups_.end() && Failed->second;
    if (Current && (It->second.finalReadPending || It->second.finalFailed || Terminal ||
                    It->second.groupId == UINT64_MAX)) Group = It->second.groupId;
  }
  unlockRepGroups();
  return Group;
}

uint64_t EJitCompileDriver::requestLogicalKey(uint32_t funcIndex,
                                              const EJitDimPair *dims,
                                              uint32_t numDims) const {
  uint32_t InstanceIds[kEJitMaxRequestDims] = {};
  for (uint32_t I = 0; I < numDims && I < kEJitMaxRequestDims; ++I)
    InstanceIds[I] = dims ? dims[I].instanceId : 0;
  return (static_cast<uint64_t>(funcIndex) << 32) |
         static_cast<uint64_t>(InstanceIds[0]) |
         (static_cast<uint64_t>(InstanceIds[1]) << 8) |
         (static_cast<uint64_t>(InstanceIds[2]) << 16) |
         (static_cast<uint64_t>(InstanceIds[3]) << 24);
}

bool EJitCompileDriver::candidateClassifyThunk(void *Ctx,
                                              const EJitCompileRequest &Req) {
  return static_cast<EJitCompileDriver *>(Ctx)->classifyRepresentativeRequest(Req);
}

bool EJitCompileDriver::classifyRepresentativeRequest(const EJitCompileRequest &Req) {
  if (!repGroups_ || !candidateDirectory_ || !jitEngine_) return false;
#ifdef EJIT_SRE_TASKPOOL_TESTING
  uint32_t Armed = 1;
  if (candidateGate_.compareExchange(Armed, 2)) {
    while (candidateGate_.loadAcquire() == 2) {}
    candidateGate_.storeRelease(0);
  }
#endif
  if (Req.numDims > kEJitMaxRequestDims) return false;
  for (unsigned I = 0; I < Req.numDims; ++I)
    if (Req.dims[I].instanceId > 255u) return false;
  const uint32_t F = stripReqTier(Req.funcIndex);
  const uint64_t LogicalKey = requestLogicalKey(F, Req.dims, Req.numDims);
  auto BC = loader_.getBitcodeByFuncIdx(F);
  if (!BC) { consumeError(BC.takeError()); return false; }
  const auto &Meta = loader_.getOrCacheFuncMeta(F);
  SpecializationContext Ctx;
  Ctx.fnName = loader_.getFuncNameByFuncIdx(F);
  Ctx.cacheKey = LogicalKey;
  Ctx.optLevel = config_.optLevel;
  for (unsigned I = 0; I < Meta.dimCount; ++I)
    for (unsigned J = 0; J < Req.numDims; ++J)
      if (Meta.dimTypes[I] == Req.dims[J].dimType)
        Ctx.dimensions.push_back({Meta.periodNames[I], static_cast<uint8_t>(Req.dims[J].instanceId)});
  EJitCodeIdentityScope Scope;
  Scope.source = SHA256::hash(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(BC->data()), BC->size()));
  Scope.entry = Ctx.fnName;
  Scope.compilerPolicy = "ejit/prefix/preserved/opt" +
                         std::to_string(static_cast<int>(config_.optLevel));
  Scope.bindingGeneration = userSymbols_.size() + 1;
  // Bound-pointer identity needs descriptor-aware admission. Until that path
  // is classified exactly, keep those requests on independent compilation.
  uint64_t Group = UINT64_MAX;
  if (Req.boundCount == 0 && Meta.boundPointerArgIndices.empty()) {
    auto Candidate = jitEngine_->classifyCandidate(*BC, Ctx, *candidateDirectory_, Scope);
    if (Candidate) Group = Candidate->groupId;
    else EJIT_DIAG("candidate independent func=%u: %s", F,
                   toString(Candidate.takeError()).c_str());
  }
  lockRepGroups();
  bool Current = sharedPool_.state() &&
                Req.generation == sharedPool_.state()->generation.loadAcquire();
  for (unsigned I = 0; Current && I < Req.numDims; ++I)
    Current = Req.versions[I] == sharedPool_.instanceVersionPublic(
        Req.dims[I].dimType, Req.dims[I].instanceId);
  if (Current) {
    // A newly classified lifecycle must acquire a fresh waiter token, even if
    // its may_const prefix still matches the previously published group.
    for (auto W = repWaiters_.begin(); W != repWaiters_.end();) {
      if (W->logicalKey == LogicalKey) {
        repGroups_->cancelWaiter(*W);
        W = repWaiters_.erase(W);
      } else {
        ++W;
      }
    }
    const bool NeedsFinalRead = Group != UINT64_MAX && !repFailedGroups_[Group];
    candidateBindings_[LogicalKey] = CandidateBinding{Group, Req, NeedsFinalRead};
  }
  const bool KeepBorrow = Current && candidateBindings_[LogicalKey].finalReadPending;
  unlockRepGroups();
  if (Current && !KeepBorrow) sharedPool_.completeRequestBorrow(Req.attemptToken);
  EJIT_DIAG("candidate classified key=0x%016lx group=%llu current=%u no emit/no sampling",
            LogicalKey, static_cast<unsigned long long>(Group), Current);
  return Current;
}

EJitSharedTaskPool::SamplingAdmission EJitCompileDriver::samplingAdmissionThunk(
    void *ctx, uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims) {
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  return drv->admitSamplingRequest(funcIndex, dims, numDims);
}

bool EJitCompileDriver::representativeMaintenanceThunk(void *Ctx) {
  auto *Driver = static_cast<EJitCompileDriver *>(Ctx);
  return Driver->serviceRepresentativeTimeouts() || Driver->serviceRepresentativeWaiters();
}

void EJitCompileDriver::completeCandidateBorrow(uint64_t Key,
                                               const EJitCompileRequest *Request) {
  uint64_t Token = 0;
  lockRepGroups();
  auto It = candidateBindings_.find(Key);
  if (It != candidateBindings_.end() && It->second.finalReadPending) {
    const auto &Original = It->second.request;
    bool Same = !Request || (Original.generation == Request->generation &&
                             Original.numDims == Request->numDims);
    for (uint32_t I = 0; Same && Request && I < Original.numDims; ++I)
      Same = Original.dims[I].dimType == Request->dims[I].dimType &&
             Original.dims[I].instanceId == Request->dims[I].instanceId &&
             Original.versions[I] == Request->versions[I];
    if (Same) {
      Token = Original.attemptToken;
      It->second.finalReadPending = false;
    }
  }
  unlockRepGroups();
  if (Token) sharedPool_.completeRequestBorrow(Token);
}

bool EJitCompileDriver::serviceRepresentativeWaiters() {
  if (!repGroups_) return false;
  CandidateBinding Work;
  uint64_t Key = 0;
  bool Found = false;
  lockRepGroups();
  for (auto &C : candidateBindings_) {
    if (!C.second.finalReadPending || C.second.groupId == UINT64_MAX ||
        repFailedGroups_[C.second.groupId] || repRetiringGroups_[C.second.groupId]) continue;
    auto G = repGroupHandles_.find(C.second.groupId);
    if (G == repGroupHandles_.end()) continue;
    const bool HasBundle = repGroups_->bundleFor(G->second) != nullptr;
    // A cancelled/cold representative need not call again to unblock waiters.
    // Elect one existing legal member; only actual business calls sample it.
    if (HasBundle || !repGroups_->currentRepresentative(G->second)) {
      if (HasBundle && !C.second.finalReadyAt)
        C.second.finalReadyAt = ejit_taskpool_trace_now();
      Work = C.second;
      Key = C.first;
      Found = true;
      break;
    }
  }
  unlockRepGroups();
  if (!Found) return false;
  const auto &R = Work.request;
  bool Current = R.generation == sharedPool_.state()->generation.loadAcquire();
  for (uint32_t I = 0; Current && I < R.numDims; ++I)
    Current = R.versions[I] == sharedPool_.instanceVersionPublic(
        R.dims[I].dimType, R.dims[I].instanceId);
  if (!Current) {
    completeCandidateBorrow(Key, &R);
    return true;
  }
  const uint64_t Now = ejit_taskpool_trace_now();
  if (Work.finalReadyAt && Now >= Work.finalReadyAt &&
      Now - Work.finalReadyAt >= repTimeoutTicks_.loadAcquire()) {
    lockRepGroups();
    auto C = candidateBindings_.find(Key);
    if (C != candidateBindings_.end() && C->second.request.attemptToken == R.attemptToken)
      C->second.finalFailed = true;
    unlockRepGroups();
    completeCandidateBorrow(Key, &R);
    EJIT_DIAG("member final wait timed out key=0x%016lx; source lease ended", Key);
    return false; // queued stale work still drains and observes finalFailed
  }
  // This is owner scheduling after group publication/re-election, never execution:
  // waiters have no private T1 and a representative's closed T1 routes AOT.
  auto Result = sharedPool_.compileOrGet(stripReqTier(R.funcIndex), R.dims,
                                         R.numDims, nullptr);
  if (Result.hasReadToken) sharedPool_.releaseRead(Result.bucketIndex);
  if (Result.status == EJitCompileOrGetStatus::CacheHit)
    completeCandidateBorrow(Key, &R);
  // Let pollOne consume an already queued request in this same worker step;
  // otherwise a maintenance return of Consumed could starve its own work.
  return false;
}

bool EJitCompileDriver::serviceRepresentativeTimeouts() {
  if (!repGroups_) return false;
  const uint64_t Now = ejit_taskpool_trace_now();
  const uint64_t Timeout = repTimeoutTicks_.loadAcquire();
  RepTier1Binding Expired;
  uint64_t Key = 0;
  lockRepGroups();
  for (const auto &H : repGroupHandles_) {
    if (repGroups_->bundleFor(H.second) ||
        repGroups_->snapshot(H.second).hasPhysical || repRetiringGroups_[H.first] ||
        repFailedGroups_[H.first]) continue;
    const auto *Live = repGroups_->currentRepresentative(H.second);
    if (!Live || Live->dispatchCount >= Live->dispatchLimit) continue;
    for (const auto &B : repTier1Bindings_)
      if (B.group.groupId == H.second.groupId &&
          B.group.generation == H.second.generation && B.requestAttemptToken &&
          B.lastProgressAt && Now >= B.lastProgressAt &&
          Now - B.lastProgressAt >= Timeout) {
        Expired = B;
        Key = H.first;
        break;
      }
    if (Key) break;
  }
  if (Key) repRetiringGroups_[Key] = true;
  unlockRepGroups();
  if (!Key) return false;
  // Pool cancellation owns admission/borrow settlement and token drain. It
  // must run outside the group lock, and no new election may pass meanwhile.
  const bool Cancelled = sharedPool_.cancelRequestAttempt(
      Expired.requestAttemptToken, EJitRequestAttemptReason::Cancelled);
  lockRepGroups();
  auto H = repGroupHandles_.find(Key);
  if (Cancelled && H != repGroupHandles_.end()) {
    if (H->second.generation == Expired.group.generation &&
        repGroups_->cancelRepresentative(H->second, Expired.session))
      ++H->second.generation;
    const uint32_t Count = ++repTimeoutCounts_[Key];
    repFailedGroups_[Key] = Count > repMaxReelections_.loadAcquire();
    EJIT_DIAG("representative timeout group=%llu rounds=%u fallback=%u",
              static_cast<unsigned long long>(H->second.groupId), Count,
              static_cast<unsigned>(repFailedGroups_[Key]));
  }
  SmallVector<uint64_t, 8> Ended;
  if (Cancelled) {
    Ended.push_back(Expired.logicalKey);
    if (repFailedGroups_[Key])
      for (const auto &C : candidateBindings_)
        if (C.second.groupId == Key && C.second.finalReadPending)
          Ended.push_back(C.first);
  }
  unlockRepGroups();
  for (uint64_t LogicalKey : Ended)
    completeCandidateBorrow(LogicalKey, LogicalKey == Expired.logicalKey
                                          ? &Expired.requestIdentity : nullptr);
  lockRepGroups();
  repRetiringGroups_[Key] = false;
  unlockRepGroups();
  return Cancelled;
}

void EJitCompileDriver::refreshRepresentativeGroup(uint64_t GroupKey) {
  // Never acquire the pool's attempt lock while holding the group lock.
  RepTier1Binding Binding;
  bool Found = false;
  lockRepGroups();
  if (repRetiringGroups_[GroupKey]) {
    unlockRepGroups();
    return;
  }
  auto It = repGroupHandles_.find(GroupKey);
  if (It != repGroupHandles_.end() && !repGroups_->bundleFor(It->second))
    for (const auto &B : repTier1Bindings_)
      if (B.group.groupId == It->second.groupId &&
          B.group.generation == It->second.generation) {
        Binding = B;
        Found = true;
        break;
      }
  unlockRepGroups();
  if (!Found || !Binding.requestAttemptToken) return;
  EJitSharedTaskPool::RequestAttemptSnapshot Status;
  if (!sharedPool_.requestAttemptStatus(Binding.requestAttemptToken, Status))
    return;
  const bool Cancelled = (Status.flags & EJitAttemptCancelRequested) ||
      (Status.retained && Status.terminalReason != EJitRequestAttemptReason::None &&
       Status.terminalReason != EJitRequestAttemptReason::Published);
  if (!Cancelled) return;
  lockRepGroups();
  It = repGroupHandles_.find(GroupKey);
  bool Changed = false;
  if (!repRetiringGroups_[GroupKey] && It != repGroupHandles_.end() &&
      It->second.generation == Binding.group.generation &&
      !repGroups_->bundleFor(It->second) &&
      repGroups_->cancelRepresentative(It->second, Binding.session)) {
    repRetiringGroups_[GroupKey] = true;
    ++It->second.generation;
    Changed = true;
  }
  unlockRepGroups();
  if (Changed) {
    completeCandidateBorrow(Binding.logicalKey, &Binding.requestIdentity);
    lockRepGroups();
    repRetiringGroups_[GroupKey] = false;
    unlockRepGroups();
  }
}

EJitSharedTaskPool::SamplingAdmission
EJitCompileDriver::admitSamplingRequest(uint32_t funcIndex,
                                        const EJitDimPair *dims,
                                        uint32_t numDims) {
  using Admission = EJitSharedTaskPool::SamplingAdmission;
  if (!repGroups_)
    return Admission::Grant; // sharing off: legacy per-request admission
  const uint64_t GroupKey = candidateGroupKey(funcIndex, dims, numDims);
  if (GroupKey == 0) return Admission::Classify;
  if (GroupKey == UINT64_MAX) return Admission::Grant;
  refreshRepresentativeGroup(GroupKey);
  lockRepGroups();
  if (repRetiringGroups_[GroupKey] || repFailedGroups_[GroupKey]) {
    unlockRepGroups();
    return Admission::Deny;
  }
  auto Candidate = candidateBindings_.find(requestLogicalKey(funcIndex, dims, numDims));
  if (Candidate != candidateBindings_.end() && Candidate->second.finalFailed) {
    unlockRepGroups();
    return Admission::Deny;
  }
  if (Candidate == candidateBindings_.end() || !Candidate->second.finalReadPending) {
    unlockRepGroups();
    return Admission::Classify;
  }

  // This request's per-cell logical identity: the exact cacheKey the wrapper
  // dispatched, so one group tracks independent logical members.
  const uint64_t LogicalKey = requestLogicalKey(funcIndex, dims, numDims);

  auto HandleIt = repGroupHandles_.find(GroupKey);
  if (HandleIt == repGroupHandles_.end()) {
    auto Opened = repGroups_->openGroup(GroupKey, repAdmissionPolicy_);
    if (!Opened) {
      consumeError(Opened.takeError());
      unlockRepGroups();
      return Admission::Grant; // a group-table limit must not strand the request
    }
    HandleIt = repGroupHandles_.emplace(GroupKey, *Opened).first;
  }
  const EJitGroupHandle G = HandleIt->second;
  // The representative's own cell is granted its personal sampling admission so
  // the group really owns ONE Tier-1 session; every other member is denied.
  const EJitRepresentativeSession *Live =
      repGroups_->currentRepresentative(G);
  if (Live && Live->logicalKey == LogicalKey && !repGroups_->bundleFor(G)) {
    unlockRepGroups();
    return Admission::Grant;
  }
  if (Live) {
    // The group already owns its ONE representative sampling session: this
    // member stays on its AOT fallback (no personal T1, no personal sampling
    // admission, no queue entry) until the group's bundle is published. A member
    // is registered ONCE per generation, so a coalesced/retried request never
    // adds a second waiter record.
    bool Registered = false;
    for (const EJitWaiterToken &W : repWaiters_)
      if (W.groupId == G.groupId && W.generation == G.generation &&
          W.logicalKey == LogicalKey) {
        Registered = true;
        break;
      }
    if (!Registered) {
      EJitGroupMember M;
      M.logicalKey = LogicalKey;
      M.funcIndex = funcIndex;
      M.cellActive = true;
      M.cold = false;
      auto W = repGroups_->joinWaiter(G, M);
      if (!W) {
        consumeError(W.takeError());
        unlockRepGroups();
        return Admission::Grant;
      }
      repWaiters_.push_back(*W);
      EJIT_DIAG("representative group %llu gen %llu: member cell=%u joins as "
                "waiter %llu (representative attempt %llu)",
                static_cast<unsigned long long>(G.groupId),
                static_cast<unsigned long long>(G.generation),
                dims ? dims[0].instanceId : 0,
                static_cast<unsigned long long>(W->token),
                static_cast<unsigned long long>(Live->attemptToken));
    }
    // The only admissible work for a member is the group's Tier-2, and only once
    // the bundle exists; before that it stays on its AOT fallback. Returning
    // WakeTier2 when the bundle is live is what turns a member with no sampling
    // session of its own into a consumer of the shared profile.
    const bool Published = repGroups_->bundleFor(G) != nullptr;
    unlockRepGroups();
    return Published ? EJitSharedTaskPool::SamplingAdmission::WakeTier2
                     : EJitSharedTaskPool::SamplingAdmission::Deny;
  }

  // First legal arrival of this generation becomes the representative - never a
  // hardcoded cell 0. The pool then takes this request's ordinary sampling
  // admission and this session's real dispatches consume the group quota.
  EJitGroupMember M;
  M.logicalKey = LogicalKey;
  M.funcIndex = funcIndex;
  M.cellActive = true;
  M.cold = false;
  SmallVector<EJitGroupMember, 4> Offered;
  Offered.push_back(M);
  auto Elected = repGroups_->electRepresentative(G, Offered, 0);
  if (!Elected) {
    consumeError(Elected.takeError());
    unlockRepGroups();
    return Admission::Grant;
  }
  repSessions_.emplace(Elected->attemptToken, *Elected);
  EJIT_DIAG("representative group %llu gen %llu: elected attempt %llu session "
            "%llu logicalKey=0x%llx quota=%llu",
            static_cast<unsigned long long>(G.groupId),
            static_cast<unsigned long long>(G.generation),
            static_cast<unsigned long long>(Elected->attemptToken),
            static_cast<unsigned long long>(Elected->samplingSessionId),
            static_cast<unsigned long long>(Elected->logicalKey),
            static_cast<unsigned long long>(Elected->dispatchLimit));
  unlockRepGroups();
  return Admission::Grant;
}

bool EJitCompileDriver::representativeWakeThunk(void *ctx, uint32_t funcIndex,
                                                const EJitDimPair *dims,
                                                uint32_t numDims,
                                                EJitCompileRequest &Out) {
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  if (!drv->repGroups_)
    return false;
  const uint64_t GroupKey = drv->candidateGroupKey(funcIndex, dims, numDims);
  if (GroupKey == 0 || GroupKey == UINT64_MAX)
    return false;
  drv->lockRepGroups();
  auto It = drv->repGroupHandles_.find(GroupKey);
  if (It == drv->repGroupHandles_.end()) {
    drv->unlockRepGroups();
    return false;
  }
  const EJitGroupHandle G = It->second;
  EJitFrozenProfileBundle Bundle = drv->repGroups_->bundleFor(G);
  if (!Bundle) {
    drv->unlockRepGroups();
    return false; // publication barrier: the member stays on AOT
  }
  // The wake-up request is a PGOUse compile of THIS member's identity carrying
  // the group's frozen observation. It consumes the shared bundle (the driver
  // resolves it by the group of the request identity during compileCold) and
  // takes no sampling admission of its own.
  EJitCompileRequest Wake{};
  Wake.funcIndex = encodeReqTier(funcIndex, kEJitTierPgoUse);
  Wake.numDims = numDims;
  for (uint32_t I = 0; I < numDims && I < kEJitMaxRequestDims; ++I) {
    Wake.dims[I] = dims[I];
    Wake.versions[I] = drv->sharedPool_.instanceVersionPublic(
        dims[I].dimType, dims[I].instanceId);
  }
  // Pool lifecycle and group re-election have independent generations.
  Wake.generation = drv->sharedPool_.state()->generation.loadAcquire();
  Wake.t1DispatchCount = Bundle->actualDispatchCount;
  Wake.t1QuotaEnd = Bundle->quotaEnd;
  Wake.t1DispatchLimit = Bundle->dispatchLimit;
  Out = Wake;
  drv->unlockRepGroups();
  return true;
}

EJitGroupHandle EJitCompileDriver::repGroupFor(uint32_t funcIndex,
                                               const EJitDimPair *dims,
                                               uint32_t numDims) {
  if (!repGroups_)
    return {};
  const uint64_t GroupKey = candidateGroupKey(funcIndex, dims, numDims);
  if (GroupKey == 0 || GroupKey == UINT64_MAX)
    return {};
  lockRepGroups();
  auto It = repGroupHandles_.find(GroupKey);
  const EJitGroupHandle G =
      It == repGroupHandles_.end() ? EJitGroupHandle{} : It->second;
  unlockRepGroups();
  return G;
}

bool EJitCompileDriver::bindRepresentativeTier1(
    const EJitCompileRequest *Request, uint64_t CacheKey,
    const EJitRepresentativeSession **OutSession) {
  if (OutSession)
    *OutSession = nullptr;
  if (!repGroups_ || !Request || Request->attemptToken == 0)
    return false;
  const uint32_t FuncIdx = stripReqTier(Request->funcIndex);
  const EJitGroupHandle G =
      repGroupFor(FuncIdx, Request->dims, Request->numDims);
  if (!G.valid())
    return false;
  lockRepGroups();
  const EJitRepresentativeSession *Live =
      repGroups_->currentRepresentative(G);
  if (!Live) {
    unlockRepGroups();
    return false;
  }
  // The Tier-1 request must be the representative member's own cell: compare
  // the full dispatch identity (funcIdx + every dim instance), so a different
  // cell of the same entry can never be mistaken for the representative.
  const uint64_t LogicalKey =
      requestLogicalKey(FuncIdx, Request->dims, Request->numDims);
  if (Live->logicalKey != LogicalKey) {
    unlockRepGroups();
    return false;
  }
  RepTier1Binding *Binding = nullptr;
  for (RepTier1Binding &B : repTier1Bindings_)
    if (B.group.groupId == G.groupId && B.group.generation == G.generation) {
      Binding = &B;
      break;
    }
  if (!Binding) {
    RepTier1Binding B;
    B.group = G;
    B.session = *Live;
    repTier1Bindings_.push_back(B);
    Binding = &repTier1Bindings_.back();
  }
  if (Binding->requestAttemptToken != 0 &&
      Binding->requestAttemptToken != Request->attemptToken) {
    // A second, different Tier-1 attempt for the same generation would be a
    // competing sampling session; the group owns exactly one, so the older
    // binding is kept and this request is refused a shared profile.
    unlockRepGroups();
    return false;
  }
  Binding->requestAttemptToken = Request->attemptToken;
  Binding->requestGeneration = Request->generation;
  Binding->requestIdentity = *Request;
  Binding->logicalKey = LogicalKey;
  Binding->lastProgressAt = ejit_taskpool_trace_now();
  if (OutSession)
    *OutSession = &Binding->session;
  EJIT_DIAG("representative group %llu gen %llu: Tier-1 request attempt %llu "
            "bound to sampling session %llu",
            static_cast<unsigned long long>(G.groupId),
            static_cast<unsigned long long>(G.generation),
            static_cast<unsigned long long>(Request->attemptToken),
            static_cast<unsigned long long>(Binding->session.samplingSessionId));
  unlockRepGroups();
  return true;
}

void EJitCompileDriver::dispatchObserverThunk(    void *ctx, const EJitSharedTaskPool::DispatchObservation &Obs) {
  auto *drv = static_cast<EJitCompileDriver *>(ctx);
  if (!drv->repGroups_)
    return;
  drv->lockRepGroups();
  // Exact-attempt routing: the observation carries the publish identity of the
  // slot the dispatch was granted from, so a retired attempt's late dispatch
  // can never be counted against the replacement session.
  for (RepTier1Binding &Binding : drv->repTier1Bindings_) {
    if (Binding.requestAttemptToken != Obs.attemptToken ||
        Binding.requestGeneration != Obs.generation ||
        Binding.session.funcIndex != Obs.funcIndex)
      continue;
    const EJitGroupHandle &G = Binding.group;
    const EJitRepresentativeSession *Live =
        drv->repGroups_->currentRepresentative(G);
    if (!Live || Live->attemptToken != Binding.session.attemptToken ||
        Live->samplingSessionId != Binding.session.samplingSessionId)
      break; // exact pool request belonged to a retired group session
    const uint64_t Now = Obs.closedQuota ? Obs.quotaEnd : 0;
    const EJitDispatchOutcome Outcome =
        drv->repGroups_->recordRepresentativeDispatch(G, *Live, Now);
    if (Outcome == EJitDispatchOutcome::Counted ||
        Outcome == EJitDispatchOutcome::CountedAndClosed)
      Binding.lastProgressAt = ejit_taskpool_trace_now();
    // A granted dispatch must be counted exactly once: if the registry reports
    // anything other than a real count, the group and the pool disagree about
    // the session, which is a lifecycle fault, not a hot-path condition.
    if (Outcome != EJitDispatchOutcome::Counted &&
        Outcome != EJitDispatchOutcome::CountedAndClosed)
      EJIT_DIAG("representative dispatch NOT counted attempt=%llu outcome=%u",
                static_cast<unsigned long long>(Obs.attemptToken),
                static_cast<unsigned>(Outcome));
    break;
  }
  drv->unlockRepGroups();
}

void *EJitCompileDriver::sharePhysicalTier2(
    const EJitCompileRequest *Request, uint64_t CacheKey,
    const std::string &FuncName, StringRef Bitcode,
    const SpecializationContext &Ctx, const EJitGroupHandle &G,
    const EJitRepresentativeSession &RepSession, bool HaveRepSession) {
  if (!repGroups_ || !jitEngine_ || !Request)
    return nullptr;
  EJitPreparedCodeEmitter *Emitter = jitEngine_->preparedEmitter();
  if (!Emitter) {
    EJIT_DIAG("representative group %llu gen %llu: no prepared-code emitter, "
              "key=0x%016lx stays on the ordinary route",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation), CacheKey);
    return nullptr;
  }

  // Who is this compile? The representative's own Tier-2 carries the bound Tier-1
  // attempt identity; every other member must have joined as a waiter of THIS
  // generation, looked up by the exact per-cell logical identity.
  const uint32_t FuncIdx = stripReqTier(Request->funcIndex);
  const uint64_t LogicalKey =
      requestLogicalKey(FuncIdx, Request->dims, Request->numDims);
  EJitWaiterToken Waiter;
  bool HaveWaiter = false;
  if (!HaveRepSession) {
    lockRepGroups();
    for (const EJitWaiterToken &T : repWaiters_)
      if (T.groupId == G.groupId && T.generation == G.generation &&
          T.logicalKey == LogicalKey) {
        Waiter = T;
        HaveWaiter = true;
        break;
      }
    unlockRepGroups();
    if (!HaveWaiter) {
      // Not an admitted member of this generation (for example the compile owner
      // is not the one that admitted the request): the ordinary ORC route is the
      // only honest fallback, and it never claims to be shared code.
      EJIT_DIAG("representative group %llu gen %llu: key=0x%016lx has no member "
                "record, staying on the ordinary route",
                static_cast<unsigned long long>(G.groupId),
                static_cast<unsigned long long>(G.generation), CacheKey);
      return nullptr;
    }
  }

  // The identity scope is the same for every member of the generation: the
  // source bitcode digest, the entry name, the compile policy and the durable
  // registered-symbol generation. The EXACT binding list is compared by
  // EJitPreparedCode::create/EJitFinalCodeIdentity::equals, never this scope
  // alone.
  EJitCodeIdentityScope Scope;
  Scope.source = SHA256::hash(ArrayRef<uint8_t>(
      reinterpret_cast<const uint8_t *>(Bitcode.data()), Bitcode.size()));
  Scope.entry = FuncName;
  Scope.compilerPolicy = "ejit/representative-sharing/pgouse/opt" +
                         std::to_string(static_cast<int>(config_.optLevel));
  Scope.bindingGeneration =
      static_cast<uint64_t>(userSymbols_.size()) + 1;

  // The REAL pipeline (same optimizer, same context, same module
  // normalization) WITHOUT linking: the identity below describes this exact
  // module, so a later compare can never qualify code the pipeline did not
  // produce.
  jitEngine_->setActiveContext(&Ctx);
  auto Prepared =
      jitEngine_->prepareFinalCode(Bitcode, CacheKey, FuncName, Scope);
  jitEngine_->setActiveContext(nullptr);
  if (!Prepared) {
    EJIT_DIAG("representative group %llu gen %llu: key=0x%016lx final-code "
              "preparation rejected (%s), ordinary route",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation), CacheKey,
              toString(Prepared.takeError()).c_str());
    return nullptr;
  }

  if (HaveRepSession) {
    // The representative emits the generation's first physical object. It is
    // linked through the SAME emitter every member reuses from, so the physical
    // object count is real and the stored identity is comparable.
    auto Linked = Emitter->link(std::move(*Prepared));
    if (!Linked) {
      EJIT_DIAG("representative group %llu gen %llu: representative Tier-2 link "
                "failed (%s)",
                static_cast<unsigned long long>(G.groupId),
                static_cast<unsigned long long>(G.generation),
                toString(Linked.takeError()).c_str());
      return nullptr;
    }
    bool Noted = false;
    {
      lockRepGroups();
      Noted = repGroups_->noteRepresentativeCode(
          G, RepSession, Linked->codeId, Linked->fn, !Linked->reused);
      unlockRepGroups();
    }
    if (!Noted) {
      EJIT_DIAG("representative group %llu gen %llu: representative code not "
                "recorded (stale session or an object already exists)",
                static_cast<unsigned long long>(G.groupId),
                static_cast<unsigned long long>(G.generation));
      return nullptr;
    }
    EJIT_DIAG("representative group %llu gen %llu: ONE physical Tier-2 emitted "
              "codeId=%llu fn=%p key=0x%016lx (reused=%d)",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation),
              static_cast<unsigned long long>(Linked->codeId), Linked->fn,
              CacheKey, Linked->reused ? 1 : 0);
    return Linked->fn;
  }

  // A member: compare the FINAL identity with the generation's physical object
  // (full IR + effective bindings + scope) and only then emit or reuse.
  EJitShareDecision Decision;
  {
    lockRepGroups();
    Decision = repGroups_->decideMember(Waiter, (*Prepared)->identity(), *Emitter);
    unlockRepGroups();
  }
  if (Decision.kind == EJitMemberShare::Reuse) {
    bool Settled = false;
    {
      lockRepGroups();
      Settled = repGroups_->completeMember(Waiter, Decision.codeId, Decision.fn,
                                           /*EmittedNew=*/false);
      unlockRepGroups();
    }
    if (!Settled) {
      EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx reuse "
                "not settled (stale/cancelled)",
                static_cast<unsigned long long>(G.groupId),
                static_cast<unsigned long long>(G.generation), CacheKey);
      return nullptr;
    }
    EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx reuses "
              "physical codeId=%llu fn=%p (final identity equal)",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation), CacheKey,
              static_cast<unsigned long long>(Decision.codeId), Decision.fn);
    return Decision.fn;
  }
  if (Decision.kind != EJitMemberShare::Emit) {
    // NotReady / Stale / Cancelled / SchemaRejected: never share, never emit
    // here. The ordinary route (or AOT) is the honest outcome.
    EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx not "
              "shared (kind=%u %s)",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation), CacheKey,
              static_cast<unsigned>(Decision.kind), Decision.reason.c_str());
    return nullptr;
  }

  // A distinct final identity: this member emits its OWN physical object, kept
  // as an independent logical member record.
  auto Linked = Emitter->link(std::move(*Prepared));
  if (!Linked) {
    EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx link "
              "failed (%s)",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation), CacheKey,
              toString(Linked.takeError()).c_str());
    return nullptr;
  }
  bool Settled = false;
  {
    lockRepGroups();
    Settled = repGroups_->completeMember(Waiter, Linked->codeId, Linked->fn,
                                         /*EmittedNew=*/!Linked->reused);
    unlockRepGroups();
  }
  if (!Settled) {
    EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx emit not "
              "settled (stale/cancelled)",
              static_cast<unsigned long long>(G.groupId),
              static_cast<unsigned long long>(G.generation), CacheKey);
    return nullptr;
  }
  EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx emitted an "
            "independent physical Tier-2 codeId=%llu fn=%p (%s)",
            static_cast<unsigned long long>(G.groupId),
            static_cast<unsigned long long>(G.generation), CacheKey,
            static_cast<unsigned long long>(Linked->codeId), Linked->fn,
            Decision.reason.c_str());
  return Linked->fn;
}

bool EJitCompileDriver::startSharedTaskPool() {
  // Publish this core's funcIndex/dimType registration digest so a peer with a
  // divergent mapping is cleanly rejected at attach (spec §11), never silently
  // running against mismatched indices.
  sharedPool_.setRegistrationFingerprint(
      EJitFuncRegistry::instance().fingerprint() * 0x9e3779b97f4a7c15ULL ^
      EJitLifecycleRegistry::instance().fingerprint());
  // V1 representative sharing needs the observed Tier-1 dispatch contract: the
  // group quota must be owned by a REAL committed dispatch, which requires the
  // request-attempt protocol (otherwise the pool never arms a slot's
  // t1DispatchLimit and no group could ever see a grant). It is immutable while
  // the blob is Ready, so it is enabled here, before init() elects the owner.
  if (repGroups_ &&
      !sharedPool_.setRequestAttemptsEnabled(/*enabled=*/true)) {
    // A peer already published a Ready blob without the contract: representative
    // sharing cannot be honored on this core, so the opt-in is dropped with no
    // group created rather than silently falling back to a private T1.
    repGroups_.reset();
    repGroupHandles_.clear();
    repSessions_.clear();
    repTier1Bindings_.clear();
    sharedPool_.setSamplingAdmissionCallback(nullptr, nullptr);
    sharedPool_.setDispatchObserver(nullptr, nullptr);
    EJIT_DIAG("representative sharing dropped: the live shared pool does not "
              "carry the request-attempt/observed-dispatch contract");
  }
  EJitSharedTaskPool::InitResult r = sharedPool_.init();
  switch (r) {
  case EJitSharedTaskPool::InitResult::BecameOwner:
    EJIT_DIAG("shared taskpool init: became owner");
    return true;
  case EJitSharedTaskPool::InitResult::AttachedReady:
    EJIT_DIAG("shared taskpool init: attached ready");
    return true;
  case EJitSharedTaskPool::InitResult::OwnerFailed:
    EJIT_DIAG("shared taskpool init FAILED: owner worker start failed");
    return false;
  case EJitSharedTaskPool::InitResult::InitInProgress:
    EJIT_DIAG("shared taskpool init FAILED: peer still initializing");
    return false;
  case EJitSharedTaskPool::InitResult::AbiMismatch:
    EJIT_DIAG("shared taskpool init FAILED: ABI mismatch (magic/version/size)");
    return false;
  case EJitSharedTaskPool::InitResult::FingerprintMismatch:
    EJIT_DIAG("shared taskpool init FAILED: registration fingerprint mismatch");
    return false;
  case EJitSharedTaskPool::InitResult::NoState:
    EJIT_DIAG("shared taskpool init FAILED: no shared state bound");
    return false;
  }
  EJIT_DIAG("shared taskpool init FAILED: unknown result=%u",
            static_cast<unsigned>(r));
  return false;
}
#endif

void EJitCompileDriver::setJitEngine(std::unique_ptr<EJitOrcEngine> engine) {
  jitEngine_ = std::move(engine);
}

bool EJitCompileDriver::ensureJitEngine() {
  // Ownership can be won more than once by the same core, and a live engine is
  // how already-published code is reached, so never rebuild.
  if (jitEngine_)
    return true;

  auto engine = EJitOrcEngine::Create(config_, runtimeState_.getRegistry(),
                                      runtimeState_);
  if (!engine) {
    EJIT_DIAG("FAILED to create OrcJIT engine");
#ifndef EJIT_FREESTANDING
    std::string errStr;
    llvm::handleAllErrors(
        engine.takeError(),
        [&](const llvm::ErrorInfoBase &E) { errStr = E.message(); });
    if (logger_)
      logger_->log(EJIT_ERR_COMPILE_FAILED,
                   "Failed to create OrcJIT engine: " + errStr, "", "");
#else
    consumeError(engine.takeError());
#endif
    return false;
  }
  jitEngine_ = std::move(*engine);
  // Replay the staged symbols: on a re-elected owner this engine is built long
  // after registration ran, so the list is the only record of them left.
  for (auto &sym : userSymbols_)
    jitEngine_->addUserSymbol(sym.first, sym.second);
  EJIT_DIAG("OrcJIT engine created successfully");
  return true;
}

void EJitCompileDriver::releaseJitEngine() {
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  // This runs on the outgoing owner. No peer cancellation callback touches
  // these maps; settle their shared sessions here before owner-private state
  // and profd addresses become unusable.
  for (const auto &Entry : tier1ProfileIdentities_) {
    const uint64_t SessionId = Entry.second.samplingSessionId;
    if (!SessionId)
      continue;
    auto Pending = pendingVpSamples_.find(SessionId);
    std::vector<EJitVpSiteSample> Discarded;
    std::vector<EJitVpSiteSample> &Samples =
        Pending == pendingVpSamples_.end() ? Discarded : Pending->second;
    (void)retireVpSession(SessionId, Samples,
                          /*PreserveForRetry=*/false);
  }
  pendingVpSamples_.clear();
  tier1Vp_.clear();
  tier1Counters_.clear();
  tier1ProfileIdentities_.clear();
  frozenProfileBundles_.clear();
#endif
  if (!jitEngine_)
    return;
  EJIT_DIAG("OrcJIT engine released: ownership given up");
  jitEngine_.reset();
}

void EJitCompileDriver::registerSymbol(const std::string &name, void *addr) {
  userSymbols_.emplace_back(name, addr);
  if (jitEngine_)
    jitEngine_->addUserSymbol(name, addr);
}

void *EJitCompileDriver::compileCold(uint64_t cacheKey, uint32_t tier,
                                     bool storeLru,
                                     const EJitCompileRequest *request) {
#if defined(EJIT_SRE_SHARED_TASKPOOL) && defined(EJIT_SRE_TASKPOOL_TESTING)
  if (repGroups_ && tier == kEJitTierPgoUse) {
    EJitSharedTaskPool::RequestAttemptSnapshot Status;
    if (request && sharedPool_.requestAttemptStatus(request->attemptToken, Status) &&
        !(Status.flags & EJitAttemptHoldsAdmission)) {
      uint32_t Count = failMemberT2_.loadAcquire();
      while (Count && !failMemberT2_.compareExchange(Count, Count - 1)) {}
      if (Count) {
        EJIT_DIAG("member Tier-2 test failure before final read");
        return nullptr;
      }
    }
    uint32_t Armed = 1;
    if (failRepresentativeT2_.compareExchange(Armed, 0)) {
      EJIT_DIAG("representative Tier-2 test failure before profile capture");
      return nullptr;
    }
  }
#endif
  // ── Cold path: decode cacheKey, verify, compile ────────────────────────
  uint32_t funcIdx = static_cast<uint32_t>(cacheKey >> 32);
  uint8_t dims[4] = {
      static_cast<uint8_t>(cacheKey & 0xFF),
      static_cast<uint8_t>((cacheKey >> 8) & 0xFF),
      static_cast<uint8_t>((cacheKey >> 16) & 0xFF),
      static_cast<uint8_t>((cacheKey >> 24) & 0xFF),
  };

  // Resolve funcName from loader
  const std::string &funcName = loader_.getFuncNameByFuncIdx(funcIdx);
  if (funcName.empty()) {
    EJIT_DIAG("cache MISS key=0x%016lx funcIdx=%u: unknown funcIdx", cacheKey,
              funcIdx);
    return nullptr;
  }

  EJIT_DIAG("cache MISS key=0x%016lx func=%s dims=[%u,%u,%u,%u]", cacheKey,
            funcName.c_str(), dims[0], dims[1], dims[2], dims[3]);

  // Get bitcode
  auto bitcodeOrErr = loader_.getBitcodeByFuncIdx(funcIdx);
  if (!bitcodeOrErr) {
    EJIT_DIAG("compile FAIL key=0x%016lx func=%s: bitcode not found", cacheKey,
              funcName.c_str());
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(EJIT_ERR_BITCODE_NOT_FOUND, "No bitcode for function",
                   funcName, std::to_string(cacheKey));
#endif
    return nullptr;
  }
  StringRef bitcode = *bitcodeOrErr;

  // Resolve period names from cached metadata (parsed once per funcIdx).
  const auto &meta = loader_.getOrCacheFuncMeta(funcIdx);
  const auto &periodNames = meta.periodNames;
  unsigned dimCount = meta.dimCount;

  // Verify time-window state for each dimension.
  for (unsigned i = 0; i < dimCount; ++i) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
    // Cross-core: gate on the SHARED enabled bit (the one the producer's
    // ejit_activate writes), NOT the owner-private runtimeState_. The shared
    // SwitchController defaults to INACTIVE (initSharedStorage sets enabled=0),
    // matching the non-shared path: a period instance must be explicitly
    // ejit_activate'd before the JIT will compile it. activate flips 0->1 +
    // bumps version; deactivate flips 1->0 + bumps version. Race protection
    // during compilation is handled by runCompile's version checkpoints
    // (cp1/cp2), not this gate.
    uint32_t dt = meta.dimTypes[i];
    if (dt == kEJitInvalidDimType ||
        !sharedPool_.isInstanceActive(dt, dims[i])) {
      EJIT_DIAG("compile SKIP key=0x%016lx func=%s: period %s[%u] not active",
                cacheKey, funcName.c_str(), periodNames[i].c_str(), dims[i]);
      return nullptr;
    }
#else
    if (!runtimeState_.isActive(periodNames[i], dims[i])) {
      EJIT_DIAG("compile SKIP key=0x%016lx func=%s: period %s[%u] not active",
                cacheKey, funcName.c_str(), periodNames[i].c_str(), dims[i]);
#ifndef EJIT_FREESTANDING
      if (logger_)
        logger_->log(EJIT_ERR_NOT_ACTIVE,
                     "Time window not active for " + periodNames[i], funcName,
                     std::to_string(cacheKey));
#endif
      return nullptr;
    }
#endif
  }

  // Build specialization context
  SpecializationContext ctx;
  ctx.fnName = funcName;
  ctx.cacheKey = cacheKey;
  ctx.optLevel = config_.optLevel;
  for (unsigned i = 0; i < dimCount; ++i)
    ctx.dimensions.push_back({periodNames[i], dims[i]});
  if (request && request->boundCount) {
    if (!validateBoundPtrDescriptors(request->boundPointers,
                                     request->boundCount)) {
      EJIT_DIAG("compile FAIL key=0x%016lx func=%s: invalid bound pointer list",
                cacheKey, funcName.c_str());
      return nullptr;
    }
    for (uint32_t I = 0; I < request->boundCount; ++I) {
      const EJitBoundPtrDescriptor &B = request->boundPointers[I];
      // Keep this lookup as an explicit loop. libc++ may lower generic
      // uint32_t range searches to wmemchr, which is unavailable on the
      // freestanding SRE target.
      bool HasMatchingBoundFormal = false;
      for (uint32_t ArgIndex : meta.boundPointerArgIndices) {
        if (ArgIndex == B.argIndex) {
          HasMatchingBoundFormal = true;
          break;
        }
      }
      if (!HasMatchingBoundFormal) {
        EJIT_DIAG("compile FAIL key=0x%016lx func=%s: bound argIndex=%u "
                  "has no matching EJIT_BOUND_PTR pointer formal",
                  cacheKey, funcName.c_str(), B.argIndex);
        return nullptr;
      }
      ctx.boundPointers.push_back(
          {static_cast<const uint8_t *>(B.rawPtr), B.size, B.argIndex});
    }
  }

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  // A production value-profile session is reserved before any expensive ORC
  // work. Every early return retires it through this guard.
  VpSessionAbortGuard VpSessionGuard;
#endif

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  // Owner-only maintenance for cancellation that arrived while a T2 freeze
  // was between bounded attempts. Shared cancellation marks the exact session;
  // the next owner cold-path entry erases only that session's private samples.
  for (auto It = pendingVpSamples_.begin(); It != pendingVpSamples_.end();) {
    if (ejitVpSessionDiscarded(It->first))
      It = pendingVpSamples_.erase(It);
    else
      ++It;
  }
#endif

  // PGO tier (EJIT_ONLINE_PGO.md §4). Gated by Config::enablePgo: off => the
  // default Baseline (unchanged pipeline). On => first compile is Tier-1
  // (Instrumented); a Tier-2 (PGOUse) recompile synthesizes the in-memory
  // profile from Tier-1's captured counters BEFORE loadBitcode (§5.3: PGOUse
  // consumes ctx.profileData during the JIT transform).
  const bool RunProfileStages =
      config_.enablePgo ||
      (config_.enableProfileAudit && config_.compileMode == CompileMode::Async);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Representative sharing state this compile may consume. Filled by the
  // PGOUse profile selection below and used by the shared physical-code step
  // after the engine/context checks.
  EJitGroupHandle PgoGroup{};
  EJitFrozenProfileBundle GroupBundle;
  EJitRepresentativeSession RepSessionForPhysical;
  bool HaveRepSessionForPhysical = false;
  bool IsRepresentativeTier2 = false;
#endif
  if (RunProfileStages) {
    if (static_cast<CompileTier>(tier) == CompileTier::PGOUse) {
      ctx.tier = CompileTier::PGOUse;
      auto Identity = tier1ProfileIdentities_.find(cacheKey);
      if (Identity != tier1ProfileIdentities_.end())
        ctx.samplingSessionId = Identity->second.samplingSessionId;
#ifdef EJIT_SRE_SHARED_TASKPOOL
      // Representative sharing: the GROUP's frozen bundle is the source of
      // truth for every member of the generation. A member consumes the SAME
      // immutable object the representative published - never a second
      // synthesis from its own (nonexistent) counters - and only after the
      // member's schema was compared against it, so a mismatching member is
      // rejected instead of silently consuming a different profile.
      const uint32_t PgoFuncIdx = stripReqTier(request ? request->funcIndex : 0);
      PgoGroup = repGroupFor(PgoFuncIdx, request ? request->dims : nullptr,
                             request ? request->numDims : 0);
      if (repGroups_ && PgoGroup.valid()) {
        const auto PgoIdentity = tier1ProfileIdentities_.find(cacheKey);
        IsRepresentativeTier2 =
            PgoIdentity != tier1ProfileIdentities_.end() &&
            PgoIdentity->second.groupId != 0 &&
            request != nullptr &&
            PgoIdentity->second.representativeAttemptToken ==
                request->attemptToken &&
            request->generation == PgoIdentity->second.generation;
        lockRepGroups();
        GroupBundle = repGroups_->bundleFor(PgoGroup);
        unlockRepGroups();
        if (!GroupBundle && !IsRepresentativeTier2) {
          // A plain member reached Tier-2 before the group published anything.
          // Its own counters are not a group profile, so compiling it from them
          // would silently become a private (non-shared) Tier-2: stay on AOT.
          EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx "
                    "has no published bundle, staying on AOT",
                    static_cast<unsigned long long>(PgoGroup.groupId),
                    static_cast<unsigned long long>(PgoGroup.generation),
                    cacheKey);
          return nullptr;
        }
        if (GroupBundle) {
          std::string SchemaReason;
          lockRepGroups();
          const bool Compatible = repGroups_->schemaCompatible(
              PgoGroup, GroupBundle->schema, &SchemaReason);
          unlockRepGroups();
          if (!Compatible) {
            EJIT_DIAG("representative group member rejected key=0x%016lx: %s",
                      cacheKey, SchemaReason.c_str());
            return nullptr;
          }
          ctx.profileBundle = GroupBundle;
          ctx.profileData = GroupBundle->indexedProfile;
          ctx.scalarValueSites = GroupBundle->scalarSites;
          ctx.samplingSessionId = GroupBundle->samplingSessionId;
          EJIT_DIAG("representative group %llu gen %llu: member key=0x%016lx "
                    "consumes the published bundle (count=%llu limit=%llu)",
                    static_cast<unsigned long long>(GroupBundle->groupId),
                    static_cast<unsigned long long>(GroupBundle->groupGeneration),
                    cacheKey,
                    static_cast<unsigned long long>(
                        GroupBundle->actualDispatchCount),
                    static_cast<unsigned long long>(GroupBundle->dispatchLimit));
        }
        // else: the representative's own Tier-2. It builds the bundle from its
        // own captured counters below and publishes it through the registry.
      }
#endif
      auto Frozen = frozenProfileBundles_.find(cacheKey);
      if (!ctx.profileBundle && Frozen != frozenProfileBundles_.end()) {
        ctx.profileBundle = Frozen->second;
        ctx.profileData = Frozen->second->indexedProfile;
        ctx.scalarValueSites = Frozen->second->scalarSites;
      }
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
      ctx.profileAuditOnly = !config_.enablePgo;
      auto mayConstIt = tier1MayConst_.find(cacheKey);
      if (mayConstIt != tier1MayConst_.end()) {
        ctx.mayConstLoadSites = mayConstIt->second.sites;
        const uint64_t SampleEnd = ejit_taskpool_trace_now();
        if (mayConstIt->second.sampleStart != 0)
          ctx.mayConstSampleCycles = SampleEnd - mayConstIt->second.sampleStart;
        const auto *Counters = reinterpret_cast<const EJitAtomicU64 *>(
            mayConstIt->second.counterBase);
        if (Counters) {
          for (size_t I = 0; I < ctx.mayConstLoadSites.size(); ++I)
            mayConstIt->second.sites[I].runtimeHits = Counters[I].loadAcquire();
          mayConstIt->second.counterBase = 0;
          ctx.mayConstLoadSites = mayConstIt->second.sites;
        }
      }
#endif
      if (!ctx.profileBundle) {
        auto it = tier1Counters_.find(cacheKey);
        if (it != tier1Counters_.end() && !it->second.empty()) {
          std::vector<PgoCounterRef> refs;
          refs.reserve(it->second.size());
          for (const auto &c : it->second)
            refs.push_back({c.pgoName.c_str(), c.profcAddr, c.profdAddr});
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
          // Value-profile merge (EJIT_VALUE_PROFILE.md §5): snapshot every
          // core's retired payload half, aggregate per site, map verified
          // indirect-call targets to IR-PGO-name MD5s, and carry the official
          // kinds in the SAME indexed profile as the edge counters. Scalar
          // sites (with the min-samples / confidence thresholds applied) ride
          // the ctx side table to the Tier-2 transform.
          SmallVector<PgoValueSite, 8> valueSites;
          SmallVector<PgoValueFunction, 8> vpFuncs;
          SmallVector<PgoScalarSite, 8> scalarSites;
          auto vpIt = tier1Vp_.find(cacheKey);
          bool haveVp =
              vpIt != tier1Vp_.end() && readValueSiteInventory(refs, vpFuncs);
          if (ejitVpSessionDiscarded(ctx.samplingSessionId)) {
            pendingVpSamples_.erase(ctx.samplingSessionId);
            EJIT_DIAG("VP freeze cancelled key=0x%016lx session=%llu", cacheKey,
                      static_cast<unsigned long long>(ctx.samplingSessionId));
            return nullptr;
          }
          std::vector<EJitVpSiteSample> &PendingSamples =
              pendingVpSamples_[ctx.samplingSessionId];
          if (!retireVpSession(ctx.samplingSessionId, PendingSamples,
                               /*PreserveForRetry=*/true)) {
            EJIT_DIAG("VP snapshot incomplete key=0x%016lx session=%llu "
                      "partial=%zu",
                      cacheKey,
                      static_cast<unsigned long long>(ctx.samplingSessionId),
                      PendingSamples.size());
            return nullptr;
          }
          std::vector<EJitVpSiteSample> samples = std::move(PendingSamples);
          pendingVpSamples_.erase(ctx.samplingSessionId);
          if (haveVp) {
            // Patch per-function scalar site counts from the Tier-1 capture.
            // The counts are keyed by the IR-PGO-name hash
            // (EJIT_VALUE_PROFILE.md §5.2); the inventory carries that hash as
            // pgoNameHash (profd NameRef) next to the CFG hash funcHash.
            DenseMap<uint64_t, uint32_t> scalarByHash;
            for (const PgoValueFunction &f : vpIt->second.functions)
              scalarByHash[f.pgoNameHash] = f.numScalarSites;
            for (PgoValueFunction &f : vpFuncs)
              f.numScalarSites = scalarByHash.lookup(f.pgoNameHash);
            haveVp =
                aggregateValueSamples(samples, vpFuncs, vpIt->second.targets,
                                      valueSites, scalarSites);
            ctx.valueProfileSnapshotComplete = haveVp;
            const size_t totalScalar = scalarSites.size();
            size_t dropped = 0;
            for (PgoScalarSite &s : scalarSites) {
              if (s.topCount < EJIT_SRE_VP_MIN_SAMPLES ||
                  s.topCount * 100 < s.total * EJIT_SRE_VP_MIN_CONF_PERCENT)
                dropped++;
            }
            // Apply the dominance thresholds: below them a site stays on the
            // generic fallback path.
            // The shared immutable bundle retains all supported scalar sites.
            // EJitScalarValueSpecPass applies the same thresholds at use time.
            if (!config_.enableRepresentativeSharing)
              llvm::erase_if(scalarSites, [](const PgoScalarSite &s) {
              return s.topCount < EJIT_SRE_VP_MIN_SAMPLES ||
                     s.topCount * 100 < s.total * EJIT_SRE_VP_MIN_CONF_PERCENT;
            });
            ctx.scalarValueSites.assign(scalarSites.begin(), scalarSites.end());
            ctx.profileData = synthesizeProfileBuffer(refs, valueSites);
            size_t icSites = 0, memSites = 0;
            for (const PgoValueSite &s : valueSites)
              (s.valueKind == IPVK_IndirectCallTarget ? icSites : memSites)++;
            ejitVpBumpMergeCounts(icSites, memSites, totalScalar, dropped);
            EJIT_DIAG("VP merge key=0x%016lx func=%s: rawSites=%zu "
                      "ics=%zu memops=%zu scalars=%zu dropped=%zu",
                      cacheKey, funcName.c_str(), samples.size(), icSites,
                      memSites, scalarSites.size(), dropped);
          }
          if (!haveVp) {
            // Edge-only profile: value data unavailable (no inventory / no
            // capture). Tier-2 still consumes the edge counters.
            EJIT_DIAG_DEBUG("VP merge key=0x%016lx: value data unavailable, "
                            "edge-only profile",
                            cacheKey);
            ctx.profileData = synthesizeProfileBuffer(refs, {});
          }
#else
          ctx.profileData = synthesizeProfileBuffer(refs, {});
#endif
          if (ctx.profileData.empty())
            EJIT_DIAG(
                "compileCold Tier-2 key=0x%016lx: profile synthesis empty",
                cacheKey);
        } else {
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
          std::vector<EJitVpSiteSample> Discarded;
          if (!retireVpSession(ctx.samplingSessionId, Discarded)) {
            EJIT_DIAG("VP snapshot incomplete key=0x%016lx session=%llu",
                      cacheKey,
                      static_cast<unsigned long long>(ctx.samplingSessionId));
            return nullptr;
          }
#endif
          EJIT_DIAG(
              "compileCold Tier-2 key=0x%016lx: no Tier-1 counters captured",
              cacheKey);
        }
      }
    } else {
      ctx.tier = CompileTier::Instrumented;
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
      auto OldIdentity = tier1ProfileIdentities_.find(cacheKey);
      if (OldIdentity != tier1ProfileIdentities_.end() &&
          OldIdentity->second.samplingSessionId) {
        const uint64_t OldSessionId = OldIdentity->second.samplingSessionId;
        std::vector<EJitVpSiteSample> Discarded;
        (void)retireVpSession(OldSessionId, Discarded,
                              /*PreserveForRetry=*/false);
        pendingVpSamples_.erase(OldSessionId);
      }
#endif
      Tier1ProfileIdentity &Identity = tier1ProfileIdentities_[cacheKey];
      // Production join (experimental sharing contract, ABI v21): capture the
      // exact Tier-1 attempt/generation that the Tier-2 bundle is later
      // validated against. Kept in one helper so the observed-dispatch
      // integration gate drives the same capture with a real pool-published
      // Tier-1 request instead of a replica.
      const Tier1ProfileAttemptIdentity Attempt =
          captureTier1ProfileAttemptIdentity(request);
      Identity.representativeAttemptToken = Attempt.representativeAttemptToken;
      Identity.generation = Attempt.generation;
      Identity.numDims = request ? request->numDims : 0;
      for (uint32_t I = 0; I < kEJitMaxRequestDims; ++I)
        Identity.versions[I] =
            request && I < request->numDims ? request->versions[I] : 0;
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
      if (config_.enablePgo) {
        Identity.samplingSessionId =
            ejitVpCreateSession(Identity.representativeAttemptToken);
        if (Identity.samplingSessionId == 0) {
          tier1ProfileIdentities_.erase(cacheKey);
          EJIT_DIAG("VP session capacity unavailable before Tier-1 codegen "
                    "key=0x%016lx",
                    cacheKey);
          return nullptr;
        }
        VpSessionGuard.arm(Identity.samplingSessionId);
      } else
#endif
      {
        Identity.samplingSessionId = nextSamplingSessionId_++;
        if (Identity.samplingSessionId == 0)
          Identity.samplingSessionId = nextSamplingSessionId_++;
      }
      ctx.samplingSessionId = Identity.samplingSessionId;
      frozenProfileBundles_.erase(cacheKey);
#ifdef EJIT_SRE_SHARED_TASKPOOL
      // Representative sharing: this Tier-1 request is the group's ONE
      // representative session only if it is the representative member's own
      // cell; bind the pool's attempt identity to the group session here, so
      // the later Tier-2 request can be proven to belong to it.
      {
        const EJitRepresentativeSession *RepSession = nullptr;
        if (bindRepresentativeTier1(request, cacheKey, &RepSession) &&
            RepSession) {
          Identity.representativeAttemptToken = request->attemptToken;
          Identity.generation = request->generation;
          Identity.groupId = repGroupFor(stripReqTier(request->funcIndex),
                                         request->dims, request->numDims)
                                 .groupId;
          Identity.groupGeneration = RepSession->generation;
        }
      }
#endif
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
      ctx.profileAuditOnly = !config_.enablePgo;
#endif
    }
  }

  if (ctx.tier == CompileTier::PGOUse && !ctx.profileBundle &&
      !ctx.profileData.empty()) {
    auto Bundle = std::make_shared<EJitProfileBundle>();
    Bundle->samplingSessionId = ctx.samplingSessionId;
    Bundle->representativeLogicalKey = cacheKey;
    auto Identity = tier1ProfileIdentities_.find(cacheKey);
    const uint64_t RepresentativeToken =
        Identity != tier1ProfileIdentities_.end()
            ? Identity->second.representativeAttemptToken
            : 0;
    const uint32_t RepresentativeGeneration =
        Identity != tier1ProfileIdentities_.end() ? Identity->second.generation
                                                  : 0;
    Bundle->representativeAttemptToken = RepresentativeToken;
    // Observed Tier-1 dispatch boundary (experimental sharing contract, ABI
    // v21): the Tier-2 request carries the count/quotaEnd frozen at the real
    // granted Tier-1 dispatch by the shared taskpool. The helper accepts them
    // only when the request still identifies this exact Tier-1
    // attempt/generation; otherwise the bundle honestly reports Unavailable
    // with count/quotaEnd 0. The configured threshold and the Tier-2 compile
    // time are never substituted for an observation.
    applyT1DispatchObservation(
        *Bundle, request,
        Tier1ProfileAttemptIdentity{RepresentativeToken,
                                    RepresentativeGeneration});
    Bundle->quality = ProfileSnapshotQuality::ApproximateInFlight;
#ifdef EJIT_SRE_SHARED_TASKPOOL
    if (repGroups_ && IsRepresentativeTier2) {
      // The representative worker drained execution tokens before entering
      // this capture (including opt-in NO_RECLAIM sampling tokens).
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
      Bundle->quality = ctx.valueProfileSnapshotComplete
                            ? ProfileSnapshotQuality::Complete
                            : ProfileSnapshotQuality::ValueDataDropped;
#else
      Bundle->quality = ProfileSnapshotQuality::EdgeOnly;
#endif
    }
#endif
    Bundle->hasEdgeProfile = true;
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    Bundle->valueProfileEnabled = true;
    Bundle->valueProfileComplete = ctx.valueProfileSnapshotComplete;
#endif
    Bundle->indexedProfile = ctx.profileData;
    Bundle->scalarSites = ctx.scalarValueSites;
    std::vector<PgoCounterRef> BundleRefs;
    auto Counters = tier1Counters_.find(cacheKey);
    if (Counters != tier1Counters_.end())
      for (const Tier1CounterInfo &C : Counters->second)
        BundleRefs.push_back({C.pgoName.c_str(), C.profcAddr, C.profdAddr});
    SmallVector<PgoValueFunction, 8> BundleFunctions;
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    if (!BundleRefs.empty() &&
        readValueSiteInventory(BundleRefs, BundleFunctions)) {
      auto Vp = tier1Vp_.find(cacheKey);
      if (Vp != tier1Vp_.end()) {
        DenseMap<uint64_t, uint32_t> ScalarCounts;
        for (const PgoValueFunction &F : Vp->second.functions)
          ScalarCounts[F.pgoNameHash] = F.numScalarSites;
        for (PgoValueFunction &F : BundleFunctions)
          F.numScalarSites = ScalarCounts.lookup(F.pgoNameHash);
        for (const PgoValueTarget &T : Vp->second.targets)
          Bundle->verifiedTargets.push_back({T.addr, T.md5Hash});
      }
    }
#endif
    if (!readProfileSchema(BundleRefs, BundleFunctions, Bundle->schema)) {
      EJIT_DIAG(
          "profile bundle reject key=0x%016lx session=%llu: invalid schema",
          cacheKey, static_cast<unsigned long long>(ctx.samplingSessionId));
      ctx.profileData.clear();
      ctx.scalarValueSites.clear();
    } else {
      Bundle->freezeCompletedAt = ejit_taskpool_trace_now();
      ctx.profileBundle = Bundle;
      frozenProfileBundles_[cacheKey] = Bundle;
      EJIT_DIAG("profile bundle frozen key=0x%016lx session=%llu dispatch=%llu "
                "limit=%llu quotaEnd=%llu quality=%u",
                cacheKey,
                static_cast<unsigned long long>(ctx.samplingSessionId),
                static_cast<unsigned long long>(Bundle->actualDispatchCount),
                static_cast<unsigned long long>(Bundle->dispatchLimit),
                static_cast<unsigned long long>(Bundle->quotaEnd),
                static_cast<unsigned>(Bundle->dispatchQuality));
#ifdef EJIT_SRE_SHARED_TASKPOOL
      // Representative sharing: publish the ONE immutable bundle of this group
      // generation from the REAL Tier-2 request of the representative. The
      // registry owns the session identity fields and rejects a bundle whose
      // observation is unavailable, so a synthetic/partial profile can never
      // become shared code.
      const auto RepIdentity = tier1ProfileIdentities_.find(cacheKey);
      IsRepresentativeTier2 =
          RepIdentity != tier1ProfileIdentities_.end() &&
          RepIdentity->second.groupId != 0 &&
          RepIdentity->second.representativeAttemptToken ==
              (request ? request->attemptToken : 0) &&
          (!request || request->generation ==
                           RepIdentity->second.generation);
      if (repGroups_ && PgoGroup.valid() && IsRepresentativeTier2) {
        lockRepGroups();
        EJitRepresentativeSession RepSession;
        bool HaveSession = false;
        for (const RepTier1Binding &B : repTier1Bindings_)
          if (B.group.groupId == PgoGroup.groupId &&
              B.group.generation == PgoGroup.generation) {
            RepSession = B.session;
            HaveSession = true;
            break;
          }
        if (HaveSession) {
          // Re-read the LIVE session under the lock: the registry validates the
          // publisher against its own current representative, so a copy taken
          // at bind time must never be allowed to publish into a replacement.
          const EJitRepresentativeSession *Live =
              repGroups_->currentRepresentative(PgoGroup);
          if (Live && Live->attemptToken == RepSession.attemptToken &&
              Live->samplingSessionId == RepSession.samplingSessionId) {
            EJitProfileBundle Publishable = *Bundle;
            const EJitPublishOutcome Outcome =
                repGroups_->publishBundle(PgoGroup, *Live,
                                          std::move(Publishable));
            EJIT_DIAG("representative group %llu gen %llu: bundle publish "
                      "outcome=%u (dispatch=%llu/%llu quotaEnd=%llu)",
                      static_cast<unsigned long long>(PgoGroup.groupId),
                      static_cast<unsigned long long>(PgoGroup.generation),
                      static_cast<unsigned>(Outcome),
                      static_cast<unsigned long long>(
                          Bundle->actualDispatchCount),
                      static_cast<unsigned long long>(Bundle->dispatchLimit),
                      static_cast<unsigned long long>(Bundle->quotaEnd));
            if (Outcome == EJitPublishOutcome::Published) {
              // Every member of the generation now consumes THIS object, and
              // the bundle's own session identity replaces the driver's local
              // sampling id for the rest of this compile.
              ctx.profileBundle = repGroups_->bundleFor(PgoGroup);
              GroupBundle = ctx.profileBundle;
              if (ctx.profileBundle) {
                ctx.profileData = ctx.profileBundle->indexedProfile;
                ctx.scalarValueSites = ctx.profileBundle->scalarSites;
                ctx.samplingSessionId = ctx.profileBundle->samplingSessionId;
              }
              // The shared physical-code step later in THIS compile records the
              // representative's object against the session that published the
              // bundle, so a retired session can never be credited this code.
              RepSessionForPhysical = *Live;
              HaveRepSessionForPhysical = true;
            }
          }
        }
        unlockRepGroups();
      }
#endif
    }
  }

  if (!jitEngine_) {
    EJIT_DIAG("compile FAIL key=0x%016lx func=%s: no sync engine", cacheKey,
              funcName.c_str());
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(EJIT_ERR_NOT_ACTIVE, "Sync engine not initialized", funcName,
                   std::to_string(cacheKey));
#endif
    return nullptr;
  }

#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Representative sharing: the generation's ONE physical Tier-2 object. Every
  // validated member of the group goes through the owner-side prepared-code
  // emitter, which compares the member's FINAL identity (full optimized IR +
  // effective bindings + scope) with the generation's object and reuses it only
  // on exact equality; a distinct identity emits its own independent object.
  // The representative's own Tier-2 emits that first object through the same
  // emitter. Nothing here runs unless the opt-in created the registry and this
  // request really belongs to a published generation.
  if (repGroups_ && ctx.tier == CompileTier::PGOUse && PgoGroup.valid() &&
      GroupBundle) {
    void *SharedFn =
        sharePhysicalTier2(request, cacheKey, funcName, bitcode, ctx, PgoGroup,
                           RepSessionForPhysical, HaveRepSessionForPhysical);
    if (SharedFn) {
      tier1Counters_.erase(cacheKey); // profile consumed (as the ordinary route)
      EJIT_DIAG("compile OK (group physical Tier-2) key=0x%016lx func=%s → "
                "pfn=%p",
                cacheKey, funcName.c_str(), SharedFn);
      return SharedFn;
    }
  }
#endif

  jitEngine_->setActiveContext(&ctx);

  if (auto Err = jitEngine_->loadBitcodeModule(bitcode, cacheKey, funcName)) {
    jitEngine_->setActiveContext(nullptr);
    EJIT_DIAG("compile FAIL key=0x%016lx func=%s: load bitcode module failed",
              cacheKey, funcName.c_str());
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(EJIT_ERR_COMPILE_FAILED, "Failed to load bitcode module",
                   funcName, std::to_string(cacheKey));
#else
    consumeError(std::move(Err));
#endif
    return nullptr;
  }

  auto addrOrErr = jitEngine_->lookup(cacheKey, funcName);
  jitEngine_->setActiveContext(nullptr);

  if (!addrOrErr) {
    EJIT_DIAG("compile FAIL key=0x%016lx func=%s: lookup after compile failed",
              cacheKey, funcName.c_str());
#ifndef EJIT_FREESTANDING
    if (logger_)
      logger_->log(EJIT_ERR_COMPILE_FAILED,
                   "Failed to look up compiled function", funcName,
                   std::to_string(cacheKey));
#else
    consumeError(addrOrErr.takeError());
#endif
    return nullptr;
  }

  void *funcPtr = *addrOrErr;

  // PGO Tier-1: capture counter addresses for a later Tier-2 synthesis (§5.2).
  // The __profc_*/__profd_* globals were forced External by
  // captureCounterGlobals during the transform; resolve them by name in the
  // specialization JITDylib.
  if (ctx.tier == CompileTier::Instrumented) {
    auto &counters = tier1Counters_[cacheKey];
    counters.clear();
    for (const std::string &name : jitEngine_->getLastCounterNames()) {
      auto profc = jitEngine_->lookup(cacheKey, "__profc_" + name);
      auto profd = jitEngine_->lookup(cacheKey, "__profd_" + name);
      if (profc && profd) {
        counters.push_back({name, reinterpret_cast<uintptr_t>(*profc),
                            reinterpret_cast<uintptr_t>(*profd)});
      } else {
        if (!profc)
          consumeError(profc.takeError());
        if (!profd)
          consumeError(profd.takeError());
      }
    }
    EJIT_DIAG("compileCold Tier-1 key=0x%016lx: captured %zu counter set(s)",
              cacheKey, counters.size());

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
    Tier1MayConstState &MayConst = tier1MayConst_[cacheKey];
    MayConst.counterBase = 0;
    MayConst.sampleStart = 0;
    MayConst.sites.assign(jitEngine_->getLastMayConstLoadSites().begin(),
                          jitEngine_->getLastMayConstLoadSites().end());
    if (!MayConst.sites.empty()) {
      if (auto Addr = jitEngine_->lookup(cacheKey, "__ejit_mayconst_hits"))
        MayConst.counterBase = reinterpret_cast<uintptr_t>(*Addr);
      else
        consumeError(Addr.takeError());
    }
    EJIT_DIAG("mayconst T0 published key=0x%016lx func=%s sites=%zu", cacheKey,
              funcName.c_str(), MayConst.sites.size());
    pendingTier1MayConstKey_ = cacheKey;
    hasPendingTier1MayConstKey_ = true;
#endif

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
    // Value-profile capture (EJIT_VALUE_PROFILE.md §5.1): build the verified
    // target table from the module's function list - each function's runtime
    // address (ORC lookup for module definitions, registered user symbols for
    // externals) mapped to the MD5 of its IR-level PGO name. Addresses that
    // resolve to nothing are simply absent from the table, so the merge can
    // never pass an unverified raw address as a profile value.
    Tier1VpState &vp = tier1Vp_[cacheKey];
    vp.targets.clear();
    vp.functions.clear();
    for (const EJitVpFunctionInfo &f : jitEngine_->getLastVpFunctions()) {
      uintptr_t addr = 0;
      if (auto a = jitEngine_->lookup(cacheKey, f.name))
        addr = reinterpret_cast<uintptr_t>(*a);
      else
        consumeError(a.takeError());
      if (!addr)
        for (const auto &us : userSymbols_)
          if (us.first == f.name) {
            addr = reinterpret_cast<uintptr_t>(us.second);
            break;
          }
      if (addr)
        vp.targets.push_back({addr, f.pgoHash});
      // The capture records the IR-PGO-name hash in BOTH hash fields: it is
      // the scalar site key, and the merge re-pairs the entry with the
      // inventory (CFG hash) via this name hash.
      vp.functions.push_back({f.pgoHash, f.pgoHash, 0, 0, f.numScalarSites});
    }
    // Start this function's collection round clean: reset its own sites (IC /
    // memop counts come from the just-captured __profd_ structs), leave other
    // admitted functions untouched, then arm the collector.
    {
      std::vector<PgoCounterRef> refs;
      refs.reserve(counters.size());
      for (const auto &c : counters)
        refs.push_back({c.pgoName.c_str(), c.profcAddr, c.profdAddr});
      SmallVector<PgoValueFunction, 8> inv;
      if (ctx.samplingSessionId == 0 && readValueSiteInventory(refs, inv)) {
        for (const PgoValueFunction &pf : inv) {
          EJitVpKindSiteCount counts[] = {{kEJitVpIndirectCall, pf.numIcSites},
                                          {kEJitVpMemOpSize, pf.numMemSites},
                                          {kEJitVpScalar, pf.numScalarSites}};
          ejitVpResetFunction(pf.pgoNameHash,
                              ArrayRef<EJitVpKindSiteCount>(counts));
        }
      }
    }
    if (config_.enablePgo) {
      bool SessionReady = true;
      for (const Tier1CounterInfo &Counter : counters)
        SessionReady &=
            ejitVpBindProfileData(ctx.samplingSessionId, Counter.profdAddr);
      if (!SessionReady) {
        EJIT_DIAG("VP profd binding unavailable key=0x%016lx session=%llu",
                  cacheKey,
                  static_cast<unsigned long long>(ctx.samplingSessionId));
        return nullptr;
      }
      VpSessionGuard.dismiss();
    }
    EJIT_DIAG_DEBUG("VP capture key=0x%016lx: %zu function(s), %zu verified "
                    "target(s)",
                    cacheKey, vp.functions.size(), vp.targets.size());
#endif
  }

  // PGO Tier-2: profile consumed; drop the captured counters (§7.1).
  if (ctx.tier == CompileTier::PGOUse)
    tier1Counters_.erase(cacheKey);

  EJIT_DIAG("compile OK key=0x%016lx func=%s → pfn=%p", cacheKey,
            funcName.c_str(), funcPtr);
  return funcPtr;
}

#ifdef EJIT_SRE_TASKPOOL
void *EJitCompileDriver::compileNow(const EJitCompileRequest &req) {
  // PGO tier rides in req.funcIndex's top 2 bits (EJitSreQueue.h). Strip it to
  // recover the real funcIndex - the loader lookup and cacheKey must NOT carry
  // tier (Tier-1 and Tier-2 of the same (funcIndex, dims) share one cacheKey,
  // EJIT_ONLINE_PGO.md §2). tier is passed to compileCold, gated by enablePgo.
  uint32_t tier = decodeReqTier(req.funcIndex);
  uint32_t funcIdx = stripReqTier(req.funcIndex);

  EJIT_DIAG("compileNow begin func=%u dims=%u tier=%u", funcIdx, req.numDims,
            tier);
  if (req.numDims > 4) {
    EJIT_DIAG("compileNow reject func=%u: numDims=%u > 4", funcIdx,
              req.numDims);
    return nullptr;
  }

  // Validate the request: instanceIds must be encodable in the legacy 8-bit
  // cacheKey slots, and no two dims may share a dimType (a duplicated lifecycle
  // dimension).
  uint32_t seenDimTypes[4] = {};
  uint32_t seenCount = 0;

  for (uint32_t i = 0; i < req.numDims; ++i) {
    if (req.dims[i].instanceId > 255u) {
      EJIT_DIAG("compileNow reject func=%u: instanceId=%u > 255 (dim[%u])",
                funcIdx, req.dims[i].instanceId, i);
      return nullptr;
    }

    for (uint32_t j = 0; j < seenCount; ++j)
      if (seenDimTypes[j] == req.dims[i].dimType) {
        EJIT_DIAG("compileNow reject func=%u: duplicate dimType=%u (dim[%u])",
                  funcIdx, req.dims[i].dimType, i);
        return nullptr;
      }

    assert(seenCount < 4 && "seenDimTypes overflow: numDims guard broken");
    seenDimTypes[seenCount++] = req.dims[i].dimType;
  }

  // meta.dimTypes[i] is the explicit dimType slot the loader read back BY NAME
  // from the process-global EJitLifecycleRegistry - the SAME slot the wrapper
  // baked into req.dims via its per-lifecycle global.
  const auto &meta = loader_.getOrCacheFuncMeta(funcIdx);
  uint8_t packedDims[4] = {0, 0, 0, 0};
  for (unsigned i = 0; i < meta.dimCount && i < 4; ++i) {
    uint32_t wantedType = meta.dimTypes[i];
    if (wantedType == kEJitInvalidDimType) {
      EJIT_DIAG("compileNow reject func=%u: meta dim[%u] dimType invalid",
                funcIdx, i);
      return nullptr;
    }
    bool found = false;
    for (uint32_t j = 0; j < req.numDims; ++j) {
      if (req.dims[j].dimType == wantedType) {
        packedDims[i] = static_cast<uint8_t>(req.dims[j].instanceId);
        found = true;
        break;
      }
    }
    if (!found) {
      EJIT_DIAG("compileNow reject func=%u: no request dim for meta dimType=%u",
                funcIdx, wantedType);
      return nullptr;
    }
  }

  uint64_t cacheKey = (static_cast<uint64_t>(funcIdx) << 32) |
                      static_cast<uint64_t>(packedDims[0]) |
                      (static_cast<uint64_t>(packedDims[1]) << 8) |
                      (static_cast<uint64_t>(packedDims[2]) << 16) |
                      (static_cast<uint64_t>(packedDims[3]) << 24);
  EJIT_DIAG("compileNow dispatch func=%u key=0x%016lx dims=[%u,%u,%u,%u]",
            funcIdx, cacheKey, packedDims[0], packedDims[1], packedDims[2],
            packedDims[3]);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (repGroups_ && tier == kEJitTierPgoUse) {
    lockRepGroups();
    auto C = candidateBindings_.find(cacheKey);
    const bool Failed = C != candidateBindings_.end() && C->second.finalFailed;
    unlockRepGroups();
    if (Failed) return nullptr; // raced a terminal decision: no new source read
  }
#endif
  void *Fn = compileCold(cacheKey, tier, /*storeLru=*/false, &req);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (repGroups_ && tier == kEJitTierPgoUse) {
    bool Terminal = false;
    if (!Fn) {
      EJitSharedTaskPool::RequestAttemptSnapshot Status;
      if (sharedPool_.requestAttemptStatus(req.attemptToken, Status) &&
          !(Status.flags & EJitAttemptHoldsAdmission)) {
        lockRepGroups();
        auto C = candidateBindings_.find(cacheKey);
        if (C != candidateBindings_.end() && C->second.finalReadPending) {
          Terminal = ++C->second.finalFailures > config_.representativeMaxFinalRetries;
          C->second.finalFailed = Terminal;
        }
        unlockRepGroups();
      }
    }
    if (Fn || Terminal) completeCandidateBorrow(cacheKey, &req);
  }
#endif
  return Fn;
}

void EJitCompileDriver::notifyTaskpoolPgoLifecycleDrop(
    const EJitCompileRequest &req) {
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  if (decodeReqTier(req.funcIndex) != kEJitTierPgoUse)
    return;
  const uint32_t FuncIdx = stripReqTier(req.funcIndex);
  const auto &Meta = loader_.getOrCacheFuncMeta(FuncIdx);
  uint8_t PackedDims[4] = {0, 0, 0, 0};
  for (unsigned I = 0; I < Meta.dimCount && I < 4; ++I)
    for (uint32_t J = 0; J < req.numDims; ++J)
      if (req.dims[J].dimType == Meta.dimTypes[I])
        PackedDims[I] = static_cast<uint8_t>(req.dims[J].instanceId);
  const uint64_t CacheKey = (static_cast<uint64_t>(FuncIdx) << 32) |
                            static_cast<uint64_t>(PackedDims[0]) |
                            (static_cast<uint64_t>(PackedDims[1]) << 8) |
                            (static_cast<uint64_t>(PackedDims[2]) << 16) |
                            (static_cast<uint64_t>(PackedDims[3]) << 24);
  auto Identity = tier1ProfileIdentities_.find(CacheKey);
  if (Identity == tier1ProfileIdentities_.end() ||
      Identity->second.representativeAttemptToken != req.attemptToken ||
      Identity->second.generation != req.generation ||
      Identity->second.numDims != req.numDims)
    return;
  for (uint32_t I = 0; I < req.numDims; ++I)
    if (Identity->second.versions[I] != req.versions[I])
      return;
  const uint64_t SessionId = Identity->second.samplingSessionId;
  std::vector<EJitVpSiteSample> Discarded;
  (void)retireVpSession(SessionId, Discarded,
                        /*PreserveForRetry=*/false);
  pendingVpSamples_.erase(SessionId);
  tier1Vp_.erase(CacheKey);
  tier1Counters_.erase(CacheKey);
  tier1ProfileIdentities_.erase(Identity);
  frozenProfileBundles_.erase(CacheKey);
#else
  (void)req;
#endif
}

void EJitCompileDriver::notifyTaskpoolPublished(const EJitCompileRequest &req,
                                                bool published) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (repGroups_ && published && decodeReqTier(req.funcIndex) == kEJitTierInstrumented) {
    lockRepGroups();
    for (auto &B : repTier1Bindings_)
      if (B.requestAttemptToken == req.attemptToken && B.requestGeneration == req.generation)
        B.lastProgressAt = ejit_taskpool_trace_now();
    unlockRepGroups();
  }
#endif
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  const uint32_t AuditTier = decodeReqTier(req.funcIndex);
  if (AuditTier == kEJitTierInstrumented) {
    if (published && hasPendingTier1MayConstKey_) {
      auto It = tier1MayConst_.find(pendingTier1MayConstKey_);
      if (It != tier1MayConst_.end())
        It->second.sampleStart = ejit_taskpool_trace_now();
    }
    pendingTier1MayConstKey_ = 0;
    hasPendingTier1MayConstKey_ = false;
  }
#endif
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
  const uint32_t tier = decodeReqTier(req.funcIndex);
  if (tier == kEJitTierInstrumented && !published) {
    const uint32_t funcIdx = stripReqTier(req.funcIndex);
    const auto &meta = loader_.getOrCacheFuncMeta(funcIdx);
    uint8_t packedDims[4] = {0, 0, 0, 0};
    for (unsigned I = 0; I < meta.dimCount && I < 4; ++I)
      for (uint32_t J = 0; J < req.numDims; ++J)
        if (req.dims[J].dimType == meta.dimTypes[I])
          packedDims[I] = static_cast<uint8_t>(req.dims[J].instanceId);
    const uint64_t cacheKey = (static_cast<uint64_t>(funcIdx) << 32) |
                              static_cast<uint64_t>(packedDims[0]) |
                              (static_cast<uint64_t>(packedDims[1]) << 8) |
                              (static_cast<uint64_t>(packedDims[2]) << 16) |
                              (static_cast<uint64_t>(packedDims[3]) << 24);
    auto Identity = tier1ProfileIdentities_.find(cacheKey);
    if (Identity != tier1ProfileIdentities_.end()) {
      std::vector<EJitVpSiteSample> Discarded;
      if (!retireVpSession(Identity->second.samplingSessionId, Discarded))
        EJIT_DIAG("VP cancel snapshot incomplete key=0x%016lx session=%llu",
                  cacheKey,
                  static_cast<unsigned long long>(
                      Identity->second.samplingSessionId));
    }
  }
#else
  (void)req;
  (void)published;
#endif
}
#endif
