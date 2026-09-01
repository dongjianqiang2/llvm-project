//===-- EJitCompileDriver.cpp - Compilation Scheduler ---------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitCompileDriver.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include <cassert>
#include <cstring>
#ifndef EJIT_FREESTANDING
#include "llvm/ExecutionEngine/EJIT/EJitLogger.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitOptimizer.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitProfileMerge.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifdef EJIT_SRE_CODE_POOL
#include "llvm/ExecutionEngine/EJIT/EJitSrePlatform.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#endif
#ifndef EJIT_FREESTANDING
#include <chrono>
#endif

using namespace llvm;
using namespace llvm::ejit;

#ifdef EJIT_SRE_TASKPOOL
namespace {
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
bool sameAuditRequest(const EJitCompileRequest &L,
                      const EJitCompileRequest &R) {
  if (L.funcIndex != R.funcIndex || L.numDims != R.numDims ||
      L.fallbackPtr != R.fallbackPtr || L.generation != R.generation ||
      L.boundArgIndex != R.boundArgIndex || L.boundSize != R.boundSize ||
      L.boundSize > EJIT_BOUND_PTR_MAX_BYTES)
    return false;
  for (uint32_t I = 0; I < 4; ++I)
    if (L.dims[I].dimType != R.dims[I].dimType ||
        L.dims[I].instanceId != R.dims[I].instanceId ||
        L.versions[I] != R.versions[I])
      return false;
  return L.boundSize == 0 ||
         std::memcmp(L.boundData, R.boundData, L.boundSize) == 0;
}
#endif

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

#ifdef EJIT_SRE_SHARED_TASKPOOL
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
    out->finalizedExecBytes = s.finalizedExecBytes;
    out->pendingExecBytes = s.pendingExecBytes;
    out->pendingRangeCount = s.pendingRangeCount;
    out->pendingAllocationCount = s.pendingAllocationCount;
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
      Dst.finalizedExecBytes = Src.finalizedExecBytes;
      Dst.pendingExecBytes = Src.pendingExecBytes;
      Dst.pendingRangeCount = Src.pendingRangeCount;
      Dst.pendingAllocationCount = Src.pendingAllocationCount;
    };
    CopyDetail(out->near, tiered.near);
    CopyDetail(out->cold, tiered.cold);
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
  sharedPool_.setPublishCallback(&taskpoolPublishThunk, this);
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

bool EJitCompileDriver::startSharedTaskPool() {
  // Publish this core's funcIndex/dimType registration digest so a peer with a
  // divergent mapping is cleanly rejected at attach (spec §11), never silently
  // running against mismatched indices.
  sharedPool_.setRegistrationFingerprint(
      EJitFuncRegistry::instance().fingerprint() * 0x9e3779b97f4a7c15ULL ^
      EJitLifecycleRegistry::instance().fingerprint());
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
  if (request && request->boundSize) {
    if (request->boundSize > EJIT_BOUND_PTR_MAX_BYTES) {
      EJIT_DIAG("compile FAIL key=0x%016lx func=%s: bound snapshot too large",
                cacheKey, funcName.c_str());
      return nullptr;
    }
    ctx.boundArgIndex = request->boundArgIndex;
    ctx.boundData.append(request->boundData,
                         request->boundData + request->boundSize);
  }

  // PGO tier (EJIT_ONLINE_PGO.md §4). Gated by Config::enablePgo: off => the
  // default Baseline (unchanged pipeline). On => first compile is Tier-1
  // (Instrumented); a Tier-2 (PGOUse) recompile synthesizes the in-memory
  // profile from Tier-1's captured counters BEFORE loadBitcode (§5.3: PGOUse
  // consumes ctx.profileData during the JIT transform).
  const bool RunProfileStages =
      config_.enablePgo ||
      (config_.enableProfileAudit && config_.compileMode == CompileMode::Async);
  if (RunProfileStages) {
    if (static_cast<CompileTier>(tier) == CompileTier::PGOUse) {
      ctx.tier = CompileTier::PGOUse;
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
      ctx.profileAuditOnly = !config_.enablePgo;
      auto mayConstIt = tier1MayConst_.find(cacheKey);
      if (mayConstIt != tier1MayConst_.end()) {
        ctx.mayConstLoadSites = mayConstIt->second.sites;
        const uint64_t SampleEnd = ejit_taskpool_trace_now();
        if (mayConstIt->second.sampleStart != 0)
          ctx.mayConstSampleCycles =
              SampleEnd - mayConstIt->second.sampleStart;
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
        if (haveVp) {
          // Patch per-function scalar site counts from the Tier-1 capture.
          // The counts are keyed by the IR-PGO-name hash (EJIT_VALUE_PROFILE.md
          // §5.2); the inventory carries that hash as pgoNameHash (profd
          // NameRef) next to the CFG hash funcHash.
          DenseMap<uint64_t, uint32_t> scalarByHash;
          for (const PgoValueFunction &f : vpIt->second.functions)
            scalarByHash[f.pgoNameHash] = f.numScalarSites;
          for (PgoValueFunction &f : vpFuncs)
            f.numScalarSites = scalarByHash.lookup(f.pgoNameHash);
          std::vector<EJitVpSiteSample> samples;
          haveVp = ejitVpTakeSnapshot(samples);
          if (haveVp) {
            (void)aggregateValueSamples(samples, vpFuncs, vpIt->second.targets,
                                        valueSites, scalarSites);
            const size_t totalScalar = scalarSites.size();
            size_t dropped = 0;
            for (PgoScalarSite &s : scalarSites) {
              if (s.topCount < EJIT_SRE_VP_MIN_SAMPLES ||
                  s.topCount * 100 < s.total * EJIT_SRE_VP_MIN_CONF_PERCENT)
                dropped++;
            }
            // Apply the dominance thresholds: below them a site stays on the
            // generic fallback path.
            llvm::erase_if(scalarSites, [](const PgoScalarSite &s) {
              return s.topCount < EJIT_SRE_VP_MIN_SAMPLES ||
                     s.topCount * 100 < s.total * EJIT_SRE_VP_MIN_CONF_PERCENT;
            });
            ctx.scalarValueSites.assign(scalarSites.begin(), scalarSites.end());
            ctx.profileData = synthesizeProfileBuffer(refs, valueSites);
            // Consume: forget this function's collected values so a later
            // round starts clean (the instrumented Tier-1 stays live until
            // publish, so post-snapshot records just re-populate the shard
            // harmlessly for the retry path).
            for (const PgoValueFunction &f : vpFuncs) {
              EJitVpKindSiteCount counts[] = {
                  {kEJitVpIndirectCall, f.numIcSites},
                  {kEJitVpMemOpSize, f.numMemSites},
                  {kEJitVpScalar, f.numScalarSites}};
              ejitVpResetFunction(f.pgoNameHash,
                                  ArrayRef<EJitVpKindSiteCount>(counts));
            }
            size_t icSites = 0, memSites = 0;
            for (const PgoValueSite &s : valueSites)
              (s.valueKind == IPVK_IndirectCallTarget ? icSites : memSites)++;
            ejitVpBumpMergeCounts(icSites, memSites, totalScalar, dropped);
            EJIT_DIAG("VP merge key=0x%016lx func=%s: rawSites=%zu "
                      "ics=%zu memops=%zu scalars=%zu dropped=%zu",
                      cacheKey, funcName.c_str(), samples.size(), icSites,
                      memSites, scalarSites.size(), dropped);
          }
        }
        if (!haveVp) {
          // Edge-only profile: value data unavailable (no snapshot / no
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
          EJIT_DIAG("compileCold Tier-2 key=0x%016lx: profile synthesis empty",
                    cacheKey);
      } else {
        EJIT_DIAG(
            "compileCold Tier-2 key=0x%016lx: no Tier-1 counters captured",
            cacheKey);
      }
    } else {
      ctx.tier = CompileTier::Instrumented;
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
      ctx.profileAuditOnly = !config_.enablePgo;
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
      if (readValueSiteInventory(refs, inv)) {
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
      ejitVpEnsureInitialized();
      ejitVpSetArmed(true);
      ++vpRoundsActive_;
    }
    EJIT_DIAG_DEBUG("VP capture key=0x%016lx: %zu function(s), %zu verified "
                    "target(s)",
                    cacheKey, vp.functions.size(), vp.targets.size());
#endif
  }

  // PGO Tier-2: profile consumed; drop the captured counters (§7.1).
  if (ctx.tier == CompileTier::PGOUse)
    tier1Counters_.erase(cacheKey);

#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE) &&         \
    defined(EJIT_SRE_CODE_POOL)
  // The current pipeline emits one executable range without a separate cold
  // section, so its finalized footprint is the hot-code approximation.
  EJitCompiledCodeInfo CodeInfo;
  if (ctx.tier != CompileTier::Instrumented &&
      jitEngine_->findCodeRange(funcPtr, CodeInfo)) {
    if (request) {
      pendingMayConstCodeAudits_.push_back(
          {*request, funcName, cacheKey, CodeInfo.codeStart,
           CodeInfo.codeSize});
    } else {
      (void)jitEngine_->recordMayConstPublishedCode(
          funcName, cacheKey,
          reinterpret_cast<const void *>(CodeInfo.codeStart),
          CodeInfo.codeSize);
    }
  }
#endif

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
  return compileCold(cacheKey, tier, /*storeLru=*/false, &req);
}

void EJitCompileDriver::notifyTaskpoolPublished(const EJitCompileRequest &req,
                                                bool published) {
#if defined(EJIT_SRE_PGO_BRANCH_AUDIT) && defined(EJIT_DIAG_ENABLE)
  auto CodeAudit = llvm::find_if(
      pendingMayConstCodeAudits_, [&](const PendingMayConstCodeAudit &Pending) {
        return sameAuditRequest(Pending.request, req);
      });
  if (CodeAudit != pendingMayConstCodeAudits_.end()) {
    if (published && jitEngine_)
      (void)jitEngine_->recordMayConstPublishedCode(
          CodeAudit->entry, CodeAudit->cacheKey,
          reinterpret_cast<const void *>(CodeAudit->codeStart),
          CodeAudit->codeSize);
    pendingMayConstCodeAudits_.erase(CodeAudit);
  }

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
  const bool consumesRound = (tier == kEJitTierPgoUse && published) ||
                             (tier == kEJitTierInstrumented && !published);
  if (consumesRound && vpRoundsActive_ > 0 && --vpRoundsActive_ == 0)
    ejitVpSetArmed(false);
#else
  (void)req;
  (void)published;
#endif
}
#endif
