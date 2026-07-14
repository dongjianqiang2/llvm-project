//===-- EJitRuntime.cpp - EmbeddedJIT C Runtime API -----------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
// Build-time-generated: EJIT_GIT_COMMIT / EJIT_GIT_BRANCH (git HEAD of the
// llvm-project source tree). Lives in the LLVMEJIT build directory.
#include "EJitVersion.h"
#ifdef EJIT_SRE_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#endif
#ifndef EJIT_FREESTANDING
#include <chrono>
#endif
// ejit_print_version() falls back to std::printf when not routing through the
// SRE platform sink. Mirrors the EJIT_DIAG <cstdio> include (non-SRE path).
#ifndef EJIT_SRE_DIAG
#include <cstdio>
#endif

using namespace llvm;
using namespace llvm::ejit;

static EJit *gEJIT = nullptr;

#ifdef EJIT_FREESTANDING
extern "C" uint64_t SRE_CycleCountGet64(void);
#endif

// ejit_print_version() prints unconditionally (not via EJIT_DIAG), so on SRE
// builds it calls SRE_printf directly. EJitDiag.h only declares SRE_printf
// under EJIT_DIAG_ENABLE; declare it here for the SRE-but-diagnostics-off case
// so the function links. When EJIT_DIAG_ENABLE is also on, EJitDiag.h already
// provides the identical declaration and this guard skips a redundant one.
#if defined(EJIT_SRE_DIAG) && !defined(EJIT_DIAG_ENABLE)
extern "C" int SRE_printf(const char *fmt, ...);
#endif

#ifndef EJIT_WRAPPER_TIMING_REPORT_EVERY
#define EJIT_WRAPPER_TIMING_REPORT_EVERY 100000u
#endif

namespace {
class TimingSpinLock {
public:
  void lock() {
    uint32_t expected = 0;
    while (!flag_.compareExchange(expected, 1u))
      expected = 0;
  }
  void unlock() { flag_.storeRelease(0u); }

private:
  EJitAtomicU32 flag_;
};

struct WrapperTimingSlot {
  bool Valid = false;
  uint32_t FuncIndex = 0;
  uint32_t Status = 0;
  void *FnPtr = nullptr;
  uint32_t BucketIndex = 0;
  uint64_t Count = 0;
  uint64_t GetFnSum = 0;
  uint64_t FnCallSum = 0;
  uint64_t ReleaseSum = 0;
  uint64_t TotalSum = 0;
};

static TimingSpinLock gWrapperTimingLock;
static WrapperTimingSlot gWrapperTimingSlots[32];

static void resetTimingSlot(WrapperTimingSlot &S, uint32_t FuncIndex,
                            uint32_t Status, void *FnPtr,
                            uint32_t BucketIndex) {
  S.Valid = true;
  S.FuncIndex = FuncIndex;
  S.Status = Status;
  S.FnPtr = FnPtr;
  S.BucketIndex = BucketIndex;
  S.Count = 0;
  S.GetFnSum = 0;
  S.FnCallSum = 0;
  S.ReleaseSum = 0;
  S.TotalSum = 0;
}

// [[maybe_unused]]: only called from the EVERY>0 periodic branch; an EVERY=0
// build (suppress periodic output) would otherwise flag this as unused.
[[maybe_unused]] static void reportTimingSlot(const WrapperTimingSlot &S) {
  if (S.Count == 0)
    return;
  EJIT_DIAG("wrapper_timing_agg func=%u status=%u fn=%p bucket=%u count=%llu "
            "get_fn_avg=%llu fn_call_avg=%llu release_avg=%llu total_avg=%llu",
            S.FuncIndex, S.Status, S.FnPtr, S.BucketIndex,
            static_cast<unsigned long long>(S.Count),
            static_cast<unsigned long long>(S.GetFnSum / S.Count),
            static_cast<unsigned long long>(S.FnCallSum / S.Count),
            static_cast<unsigned long long>(S.ReleaseSum / S.Count),
            static_cast<unsigned long long>(S.TotalSum / S.Count));
}
} // namespace

#ifdef EJIT_SRE_SHARED_TASKPOOL
static void bindDumpSharedStateFromRuntime() {
  if (!gEJIT) {
    setDumpSharedState(nullptr);
    return;
  }
  if (EJitSharedTaskPool *sp = gEJIT->sharedTaskPool())
    setDumpSharedState(sp->state());
  else
    setDumpSharedState(nullptr);
}
#endif

static void parseConfig(const ejit_config_t *src, Config &dst) {
  if (!src)
    return;
  dst.compileMode = (src->compileMode == EJIT_COMPILE_ASYNC)
                        ? CompileMode::Async
                        : CompileMode::Sync;
  dst.optLevel = static_cast<OptimizationLevel>(src->optLevel);
  if (src->maxCodeMemory > 0)
    dst.maxCodeMemory = src->maxCodeMemory;
  if (src->maxDataMemory > 0)
    dst.maxDataMemory = src->maxDataMemory;
  if (src->maxCacheEntries > 0)
    dst.maxCacheEntries = src->maxCacheEntries;
  if (src->maxCacheSize > 0)
    dst.maxCacheSize = src->maxCacheSize;
  dst.enableLogger = src->enableLogger;
  dst.forceStaticRegistry = src->forceStaticRegistry;
  if (src->dumpJITDir && src->dumpJITDir[0])
    dst.dumpJITDir = src->dumpJITDir;
  dst.enablePgo = src->enablePgo;
#ifdef EJIT_FREESTANDING
  dst.enableLogger = false;
#endif
}

extern "C" {

ejit_status_t ejit_init(const ejit_config_t *config) {
  if (gEJIT) {
    EJIT_DIAG("already initialized, returning OK");
    return EJIT_OK;
  }

  Config cfg;
  parseConfig(config, cfg);

  gEJIT = new (std::nothrow) EJit(cfg);
  if (!gEJIT) {
    EJIT_DIAG("failed: out of memory");
    return EJIT_ERR_MEMORY;
  }

  // Registration failures during construction (funcIndex/lifecycle capacity
  // exhausted, a malformed or conflicting bitcode payload, or a null fixup
  // pointer) must fail init rather than expose a half-registered taskpool.
  if (gEJIT->initFailed()) {
    const EJitError &e = gEJIT->initError();
    EJIT_DIAG("init failed: code=%d %s (%s)", e.code, e.message.c_str(),
              e.funcName.c_str());
    ejit_status_t st = (e.code != 0) ? static_cast<ejit_status_t>(e.code)
                                     : EJIT_ERR_INVALID_PARAM;
    delete gEJIT;
    gEJIT = nullptr;
    return st;
  }

#ifdef EJIT_SRE_SHARED_TASKPOOL
  bindDumpSharedStateFromRuntime();
#endif
  EJIT_DIAG("initialized: mode=%d opt=%d cache=%zu entries=%u",
            (int)cfg.compileMode, (int)cfg.optLevel, cfg.maxCacheSize,
            (unsigned)cfg.maxCacheEntries);
  return EJIT_OK;
}

void ejit_shutdown(void) {
  EJIT_DIAG("shutting down");
#ifdef EJIT_SRE_SHARED_TASKPOOL
  setDumpSharedState(nullptr);
#endif
  delete gEJIT;
  gEJIT = nullptr;
  EJIT_DIAG("shutdown complete");
}

void ejit_register_symbol(const char *name, void *addr) {
  EJIT_DIAG_VERBOSE("register_symbol name=%s addr=%p", name ? name : "<null>",
                    addr);
  if (gEJIT) {
    gEJIT->registerSymbol(name, addr);
  } else {
    // Constructor-phase call (before ejit_init): stage for later consumption.
    EJitRegistrationStore::instance().registerSymbol(name, addr);
  }
}

void ejit_register_bitcode(const char *funcName, const uint8_t *bitcodeData,
                           uint64_t bitcodeSize) {
  EJIT_DIAG_VERBOSE("register_bitcode name=%s size=%llu",
                    funcName ? funcName : "<null>",
                    static_cast<unsigned long long>(bitcodeSize));
  if (gEJIT) {
    // Post-init runtime registration: the void ABI cannot return a status, so a
    // rejection (null/zero payload, funcIndex capacity, conflicting payload) is
    // recorded in the registration-error sink for observability.
    if (!gEJIT->registerBitcode(funcName, bitcodeData,
                                static_cast<size_t>(bitcodeSize))) {
      EJitRegistrationStore::instance().recordError(
          EJIT_ERR_INVALID_PARAM, "runtime bitcode registration rejected",
          funcName ? funcName : "");
      EJIT_DIAG("register_bitcode FAIL name=%s: runtime registration rejected",
                funcName ? funcName : "<null>");
    }
  } else {
    EJitRegistrationStore::instance().registerBitcode(
        funcName, bitcodeData, static_cast<size_t>(bitcodeSize));
  }
}

void ejit_register_period_array(const char *periodName, const char *varName,
                                void *baseAddr, uint64_t arraySize) {
  EJIT_DIAG_VERBOSE("register_period_array period=%s var=%s size=%llu",
                    periodName ? periodName : "<null>",
                    varName ? varName : "<null>",
                    static_cast<unsigned long long>(arraySize));
  if (gEJIT) {
    // Post-init: rejected once registration is frozen (taskpool); the void ABI
    // records the failure for observability and mutates nothing.
    if (!gEJIT->registerPeriodArray(periodName, varName, baseAddr, arraySize)) {
      EJitRegistrationStore::instance().recordError(
          EJIT_ERR_INVALID_PARAM, "runtime period-array registration rejected",
          periodName ? periodName : "");
      EJIT_DIAG("register_period_array FAIL period=%s: rejected",
                periodName ? periodName : "<null>");
    }
  } else {
    EJitRegistrationStore::instance().registerPeriodArray(periodName, varName,
                                                          baseAddr, arraySize);
  }
}

void ejit_register_static_var(const char *varName, void *varAddr) {
  EJIT_DIAG_VERBOSE("register_static_var var=%s addr=%p",
                    varName ? varName : "<null>", varAddr);
  if (gEJIT) {
    if (!gEJIT->registerStaticVar(varName, varAddr)) {
      EJitRegistrationStore::instance().recordError(
          EJIT_ERR_INVALID_PARAM, "runtime static-var registration rejected",
          varName ? varName : "");
      EJIT_DIAG("register_static_var FAIL var=%s: rejected",
                varName ? varName : "<null>");
    }
  } else {
    EJitRegistrationStore::instance().registerStaticVar(varName, varAddr);
  }
}

void ejit_register_lifecycle(const char *lifecycleName, uint32_t *slotOut) {
  // Self-contained: the dimType-slot assignment lives in a process-global
  // registry that exists independently of the EJit instance, so this works
  // whether called from a global constructor (before ejit_init) or the static
  // registry-table walk. Idempotent — the same name always yields the same
  // slot. A capacity failure (the 9th distinct lifecycle) leaves *slotOut
  // invalid AND is recorded so ejit_init fails instead of silently continuing.
  if (!lifecycleName || !slotOut) {
    EJIT_DIAG("register_lifecycle reject: name=%p slotOut=%p",
              (const void *)lifecycleName, (void *)slotOut);
    return;
  }
  EJIT_DIAG_VERBOSE("register_lifecycle name=%s", lifecycleName);
#ifdef EJIT_SRE_TASKPOOL
  // Once a taskpool init has frozen registration, the worker reads the registry
  // lock-free: refuse to mutate it (leave *slotOut and the registry unchanged).
  if (gEJIT && gEJIT->registrationFrozen()) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_INVALID_PARAM, "lifecycle registration after init is frozen",
        lifecycleName);
    EJIT_DIAG("register_lifecycle reject name=%s: registration frozen",
              lifecycleName);
    return;
  }
#endif
  uint32_t slot =
      EJitLifecycleRegistry::instance().resolveAssign(lifecycleName);
  *slotOut = slot;
  if (slot == kEJitInvalidDimType) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_CACHE_FULL, "lifecycle (dimType) capacity exhausted",
        lifecycleName);
    EJIT_DIAG("register_lifecycle FAIL name=%s: dimType capacity exhausted",
              lifecycleName);
  } else {
    EJIT_DIAG_VERBOSE("register_lifecycle OK name=%s slot=%u", lifecycleName,
                      slot);
  }
}

void ejit_register_funcindex(const char *funcName, uint32_t *slotOut) {
  // Self-contained dense-funcIndex assignment, mirroring ejit_register_
  // lifecycle. Idempotent by name. Capacity exhaustion leaves *slotOut invalid
  // (the wrapper then falls back without entering the taskpool) AND is recorded
  // so ejit_init fails rather than building a half-registered taskpool.
  if (!funcName || !slotOut) {
    EJIT_DIAG("register_funcindex reject: name=%p slotOut=%p",
              (const void *)funcName, (void *)slotOut);
    return;
  }
  EJIT_DIAG_VERBOSE("register_funcindex name=%s", funcName);
#ifdef EJIT_SRE_TASKPOOL
  if (gEJIT && gEJIT->registrationFrozen()) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_INVALID_PARAM, "funcIndex registration after init is frozen",
        funcName);
    EJIT_DIAG("register_funcindex reject name=%s: registration frozen",
              funcName);
    return;
  }
#endif
  uint32_t idx = EJitFuncRegistry::instance().resolveAssign(funcName);
  *slotOut = idx;
  if (idx == kEJitInvalidFuncIndex) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_CACHE_FULL, "funcIndex capacity exhausted for function",
        funcName);
    EJIT_DIAG("register_funcindex FAIL name=%s: funcIndex capacity exhausted",
              funcName);
  } else {
    EJIT_DIAG_VERBOSE("register_funcindex OK name=%s idx=%u", funcName, idx);
  }
}

void ejit_register_icache_slot(const char *funcName, void *slot,
                               uint32_t numDims) {
  // Wire the wrapper's per-function @__ejit_icache_fn_<name> slot into the
  // runtime slot registry, keyed by the SAME registry funcIndex
  // ejit_register_funcindex assigns by name. numDims is the [D]^numDims shape
  // so icacheFill can linearize. The wrapper reads the cell directly on the
  // icache hit path; icacheFill writes the frozen specialization pointer through
  // it on resolve. Idempotent by name (resolveAssign is). A null slot or
  // unresolvable name is recorded; the base stays null and the wrapper's probe
  // cleanly misses -> taskpool fallback.
  if (!funcName || !slot) {
    EJIT_DIAG("register_icache_slot reject: name=%p slot=%p",
              (const void *)funcName, slot);
    return;
  }
  EJIT_DIAG_VERBOSE("register_icache_slot name=%s numDims=%u", funcName, numDims);
#ifdef EJIT_SRE_TASKPOOL
  if (gEJIT && gEJIT->registrationFrozen()) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_INVALID_PARAM, "icache slot registration after init is frozen",
        funcName);
    EJIT_DIAG("register_icache_slot reject name=%s: registration frozen",
              funcName);
    return;
  }
#endif
  uint32_t idx = EJitFuncRegistry::instance().resolveAssign(funcName);
  if (idx == kEJitInvalidFuncIndex) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_CACHE_FULL, "funcIndex capacity exhausted for icache slot",
        funcName);
    EJIT_DIAG("register_icache_slot FAIL name=%s: funcIndex capacity exhausted",
              funcName);
    return;
  }
  ejitIcacheRegisterSlot(idx, slot, numDims);
  EJIT_DIAG_VERBOSE("register_icache_slot OK name=%s idx=%u numDims=%u",
                    funcName, idx, numDims);
}

ejit_status_t ejit_activate(const char *periodName, uint8_t cellIdx) {
  if (!gEJIT) {
    EJIT_DIAG("activate(%s,%u) failed: not initialized", periodName, cellIdx);
    return EJIT_ERR_NOT_ACTIVE;
  }
  EJIT_DIAG("activate(%s,%u)", periodName, cellIdx);
  // In a taskpool build this also syncs the SwitchController and returns false
  // for an unknown lifecycle (no state changed). In the legacy build it always
  // succeeds.
  if (!gEJIT->activate(periodName, cellIdx))
    return EJIT_ERR_INVALID_PARAM;
  return EJIT_OK;
}

ejit_status_t ejit_deactivate(const char *periodName, uint8_t cellIdx) {
  if (!gEJIT) {
    EJIT_DIAG("deactivate(%s,%u) failed: not initialized", periodName, cellIdx);
    return EJIT_ERR_NOT_ACTIVE;
  }
  EJIT_DIAG("deactivate(%s,%u)", periodName, cellIdx);
  if (!gEJIT->deactivate(periodName, cellIdx))
    return EJIT_ERR_INVALID_PARAM; // unknown lifecycle: nothing changed.
  gEJIT->invalidateByPeriod(periodName, cellIdx);
  return EJIT_OK;
}

ejit_status_t ejit_activate_all(const char *periodName) {
  EJIT_DIAG("activate_all(%s)", periodName);
  if (!gEJIT) {
    EJIT_DIAG("activate_all(%s) failed: not initialized", periodName);
    return EJIT_ERR_NOT_ACTIVE;
  }
  if (!gEJIT->activateAll(periodName))
    return EJIT_ERR_INVALID_PARAM;
  return EJIT_OK;
}

ejit_status_t ejit_deactivate_all(const char *periodName) {
  EJIT_DIAG("deactivate_all(%s)", periodName);
  if (!gEJIT) {
    EJIT_DIAG("deactivate_all(%s) failed: not initialized", periodName);
    return EJIT_ERR_NOT_ACTIVE;
  }
  if (!gEJIT->deactivateAll(periodName))
    return EJIT_ERR_INVALID_PARAM;
  gEJIT->invalidateAllByPeriod(periodName);
  return EJIT_OK;
}

bool ejit_is_active(const char *periodName, uint8_t cellIdx) {
  if (!gEJIT) {
    EJIT_DIAG("is_active(%s,%u) failed: not initialized", periodName, cellIdx);
    return false;
  }
  return gEJIT->isActive(periodName, cellIdx);
}

void *ejit_compile_or_get(uint64_t cacheKey, void **out_pfn) {
  if (out_pfn) *out_pfn = nullptr;
  EJIT_DIAG("compile_or_get(key=0x%016lx): retired, use taskpool API", cacheKey);
  return nullptr;
}

void ejit_clear_cache(void) {
  EJIT_DIAG("clear_cache");
  if (!gEJIT)
    return;
  gEJIT->clearCache();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (auto *tp = gEJIT->sharedTaskPool())
    tp->retireDispatchCache();
#endif
}

void ejit_invalidate(const char *periodName, uint8_t cellIdx) {
  EJIT_DIAG("invalidate(%s,%u)", periodName, cellIdx);
  if (!gEJIT)
    return;
  gEJIT->invalidateByPeriod(periodName, cellIdx);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (auto *tp = gEJIT->sharedTaskPool())
    tp->retireDispatchCache();
#endif
}

ejit_status_t ejit_get_stats(ejit_stats_t *stats) {
  if (!gEJIT) return EJIT_ERR_NOT_ACTIVE;
  if (!stats) return EJIT_ERR_INVALID_PARAM;
  memset(stats, 0, sizeof(*stats));
  return EJIT_OK;
}

const ejit_error_t *ejit_get_last_error(void) {
  if (!gEJIT) {
    EJIT_DIAG("get_last_error: not initialized");
    return nullptr;
  }
  static ejit_error_t err;
  const EJitError *last = gEJIT->getLastError();
  if (!last)
    return nullptr;
  err.code = last->code;
  snprintf(err.message, sizeof(err.message), "%s", last->message.c_str());
  snprintf(err.funcName, sizeof(err.funcName), "%s", last->funcName.c_str());
  return &err;
}

void ejit_set_compile_mode(ejit_compile_mode_t mode) {
  EJIT_DIAG("set_compile_mode mode=%u", static_cast<unsigned>(mode));
  if (!gEJIT) {
    EJIT_DIAG("set_compile_mode reject: not initialized");
    return;
  }
  (void)gEJIT->setCompileMode(mode == EJIT_COMPILE_ASYNC ? CompileMode::Async
                                                        : CompileMode::Sync);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (auto *tp = gEJIT->sharedTaskPool())
    tp->retireDispatchCache();
#endif
}

ejit_compile_mode_t ejit_get_compile_mode(void) {
  if (!gEJIT) {
    EJIT_DIAG("get_compile_mode: not initialized (default async)");
    return EJIT_COMPILE_SYNC;
  }
  return gEJIT->getCompileMode() == CompileMode::Async ? EJIT_COMPILE_ASYNC
                                                       : EJIT_COMPILE_SYNC;
}

//===-- SRE taskpool black-box API ----------------------------------------===//

static ejit_status_t taskpoolStatus(EJitCompileOrGetStatus s) {
  switch (s) {
  case EJitCompileOrGetStatus::CacheHit:
    return EJIT_OK;
  case EJitCompileOrGetStatus::EnqueuedPending:
  case EJitCompileOrGetStatus::AlreadyPending:
    return EJIT_PENDING;
  case EJitCompileOrGetStatus::QueueFullFallback:
    return EJIT_ERR_QUEUE_FULL;
  case EJitCompileOrGetStatus::OffMode:
    return EJIT_ERR_DISABLED;
  case EJitCompileOrGetStatus::InstanceDisabled:
    return EJIT_ERR_INSTANCE_DISABLED;
  case EJitCompileOrGetStatus::InvalidParam:
    return EJIT_ERR_INVALID_PARAM;
  case EJitCompileOrGetStatus::CompileFailed:
  default:
    return EJIT_ERR_COMPILE_FAILED;
  }
}

namespace {
// Resolve the taskpool the C ABI drives: the cross-core SHARED pool in a shared
// build, otherwise the per-instance pool. Both expose compileOrGet /
// releaseRead / pendingCount / pollOne / pollBudget with matching shapes, so
// the common call sites use `auto *tp = activeTaskPool();`. (Stats and the
// switch-controller toggle differ in shape and branch explicitly.)
#ifdef EJIT_SRE_SHARED_TASKPOOL
inline EJitSharedTaskPool *activeTaskPool() {
  return gEJIT ? gEJIT->sharedTaskPool() : nullptr;
}
#else
inline EJitTaskPool *activeTaskPool() {
  return gEJIT ? gEJIT->taskPool() : nullptr;
}
#endif

// Fill the calling core's icache slot on a successful taskpool resolve (cache
// hit or fresh compile), so a cold icache is filled on the first taskpool hit,
// not only on a fresh compile. Multi-version: one-shot per cell (the
// specialization is invariant per identity - no version snapshot); dims selects
// the [D]^numDims cell. No-op without the shared pool or a null fnPtr. Always
// defined (a no-op without the shared taskpool) so call sites need no #ifdef
// guards.
inline void ejitIcacheFillOnSuccess(uint32_t funcIndex, void *fnPtr,
                                    const EJitDimPair *dims,
                                    uint32_t numDims) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (!fnPtr)
    return;
  EJitSharedTaskPool *sp = gEJIT ? gEJIT->sharedTaskPool() : nullptr;
  if (sp)
    sp->icacheFill(funcIndex, fnPtr, dims, numDims);
#else
  (void)funcIndex;
  (void)fnPtr;
  (void)dims;
  (void)numDims;
#endif
}
} // namespace

ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims,
                                           uint32_t numDims, void **outFn,
                                           uint32_t *outBucket) {
  if (outFn)
    *outFn = nullptr;
  if (outBucket)
    *outBucket = 0;
  EJIT_DIAG_VERBOSE("taskpool_compile_or_get func=%u dims=%u", funcIndex,
                    numDims);
  // Silently return EJIT_ERR_NOT_ACTIVE when the runtime is not initialized.
  // The AOT wrapper calls this on every ejit_entry invocation, so a per-call
  // "not initialized" log would noise the trace; the caller already handles
  // the status code.
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_compile_or_get reject func=%u: no taskpool", funcIndex);
    return EJIT_ERR_NOT_ACTIVE;
  }

  if (numDims > 4) {
    EJIT_DIAG("taskpool_compile_or_get reject func=%u: numDims=%u > 4",
              funcIndex, numDims);
    return EJIT_ERR_INVALID_PARAM;
  }
  if (numDims > 0 && !dims) {
    EJIT_DIAG("taskpool_compile_or_get reject func=%u: dims=null numDims=%u",
              funcIndex, numDims);
    return EJIT_ERR_INVALID_PARAM;
  }
  for (uint32_t i = 0; i < numDims; ++i) {
    if (dims[i].dimType >= EJitSwitchController::MAX_DIM_TYPES) {
      EJIT_DIAG("taskpool_compile_or_get reject func=%u: dim[%u] dimType=%u OOR",
                funcIndex, i, dims[i].dimType);
      return EJIT_ERR_INVALID_PARAM;
    }
    if (dims[i].instanceId >= EJitSwitchController::MAX_INSTANCES) {
      EJIT_DIAG("taskpool_compile_or_get reject func=%u: dim[%u] instanceId=%u OOR",
                funcIndex, i, dims[i].instanceId);
      return EJIT_ERR_INVALID_PARAM;
    }
  }

  // ejit_dim_pair_t and EJitDimPair share the same layout; pass through
  // to avoid a stack copy of up to 4 dim pairs.
  const EJitDimPair *dimsCast = reinterpret_cast<const EJitDimPair *>(dims);

  // Flattened fast cache-hit path: resolve the common terminal outcomes (cache
  // hit, disabled instance, not-Ready / ready-but-not-shareable fallback)
  // without entering the full compileOrGet slow path. tryCacheHit preserves the
  // exact compileOrGet hot-path semantics (ordering, counters, read tokens),
  // so a hit still hands back outBucket for ejit_taskpool_release_read and a
  // disabled instance never returns stale code. A true miss falls through to
  // compileOrGet unchanged (enqueue/dedup/compile).
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dimsCast, numDims, &l0Fn)) {
    if (outFn) *outFn = l0Fn;
    if (outBucket) *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit(funcIndex, dimsCast, numDims);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    EJIT_DIAG_VERBOSE("taskpool_compile_or_get func=%u fast status=%u fn=%p",
                      funcIndex, static_cast<unsigned>(fast.status),
                      fast.fnPtr);
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dimsCast, numDims);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dimsCast, numDims);
    return taskpoolStatus(fast.status);
  }

  auto r = tp->compileOrGet(funcIndex, dimsCast, numDims,
                            /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  EJIT_DIAG_VERBOSE("taskpool_compile_or_get func=%u status=%u fn=%p",
                    funcIndex, static_cast<unsigned>(r.status), r.fnPtr);
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dimsCast, numDims);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dimsCast, numDims);
  return taskpoolStatus(r.status);
}

//===----------------------------------------------------------------------===//
// Fixed-dimension C ABI fast paths (0-4 dims).
//
// Additive entry points that pass the dim identity as scalar arguments instead
// of an EJitDimPair* + numDims pair. They skip the generic entry's numDims
// bound check, null-dims check, and variable-length validation loop, and drive
// the pool's unrolled tryCacheHitNd() so a cache hit reaches the cache lookup
// with the least overhead. Semantics (status mapping, read-token / outBucket
// ownership, InstanceDisabled / OffMode / readyButNotShareable, and the
// fall-through to compileOrGet on a true miss) are identical to
// ejit_taskpool_compile_or_get with the matching numDims. The original generic
// entry is retained unchanged as the fallback for >4 dims and existing wrapper
// output.
//===----------------------------------------------------------------------===//
namespace {
inline bool ejitTaskpoolDimInRange(uint32_t dimType, uint32_t instanceId) {
  return dimType < EJitSwitchController::MAX_DIM_TYPES &&
         instanceId < EJitSwitchController::MAX_INSTANCES;
}
} // namespace

ejit_status_t ejit_taskpool_compile_or_get_0d(uint32_t funcIndex, void **outFn,
                                              uint32_t *outBucket) {
  if (outFn)
    *outFn = nullptr;
  if (outBucket)
    *outBucket = 0;
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp)
    return EJIT_ERR_NOT_ACTIVE;

  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, nullptr, 0, &l0Fn)) {
    if (outFn) *outFn = l0Fn;
    if (outBucket) *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit0D(funcIndex);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, nullptr, 0);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, nullptr, 0, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, nullptr, 0);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, nullptr, 0);
  return taskpoolStatus(r.status);
}

ejit_status_t ejit_taskpool_compile_or_get_1d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, void **outFn,
                                              uint32_t *outBucket) {
  if (outFn)
    *outFn = nullptr;
  if (outBucket)
    *outBucket = 0;
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp)
    return EJIT_ERR_NOT_ACTIVE;
  if (!ejitTaskpoolDimInRange(dim0, inst0))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[1] = {{dim0, inst0}};
  // Per-core L0: steady-state hit with no rwlock, scan, or read token.
  // kEJitNoBucket tells the caller no token was taken.
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 1, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit1D(funcIndex, dim0, inst0);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 1);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 1);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 1, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 1);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dims, 1);
  return taskpoolStatus(r.status);
}

ejit_status_t ejit_taskpool_compile_or_get_2d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, uint32_t dim1,
                                              uint32_t inst1, void **outFn,
                                              uint32_t *outBucket) {
  if (outFn)
    *outFn = nullptr;
  if (outBucket)
    *outBucket = 0;
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp)
    return EJIT_ERR_NOT_ACTIVE;
  if (!ejitTaskpoolDimInRange(dim0, inst0) ||
      !ejitTaskpoolDimInRange(dim1, inst1))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[2] = {{dim0, inst0}, {dim1, inst1}};
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 2, &l0Fn)) {
    if (outFn) *outFn = l0Fn;
    if (outBucket) *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit2D(funcIndex, dim0, inst0, dim1, inst1);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 2);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 2);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 2, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 2);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dims, 2);
  return taskpoolStatus(r.status);
}

ejit_status_t ejit_taskpool_compile_or_get_3d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, uint32_t dim1,
                                              uint32_t inst1, uint32_t dim2,
                                              uint32_t inst2, void **outFn,
                                              uint32_t *outBucket) {
  if (outFn)
    *outFn = nullptr;
  if (outBucket)
    *outBucket = 0;
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp)
    return EJIT_ERR_NOT_ACTIVE;
  if (!ejitTaskpoolDimInRange(dim0, inst0) ||
      !ejitTaskpoolDimInRange(dim1, inst1) ||
      !ejitTaskpoolDimInRange(dim2, inst2))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[3] = {{dim0, inst0}, {dim1, inst1}, {dim2, inst2}};
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 3, &l0Fn)) {
    if (outFn) *outFn = l0Fn;
    if (outBucket) *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast =
      tp->tryCacheHit3D(funcIndex, dim0, inst0, dim1, inst1, dim2, inst2);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 3);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 3);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 3, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 3);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dims, 3);
  return taskpoolStatus(r.status);
}

ejit_status_t ejit_taskpool_compile_or_get_4d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, uint32_t dim1,
                                              uint32_t inst1, uint32_t dim2,
                                              uint32_t inst2, uint32_t dim3,
                                              uint32_t inst3, void **outFn,
                                              uint32_t *outBucket) {
  if (outFn)
    *outFn = nullptr;
  if (outBucket)
    *outBucket = 0;
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp)
    return EJIT_ERR_NOT_ACTIVE;
  if (!ejitTaskpoolDimInRange(dim0, inst0) ||
      !ejitTaskpoolDimInRange(dim1, inst1) ||
      !ejitTaskpoolDimInRange(dim2, inst2) ||
      !ejitTaskpoolDimInRange(dim3, inst3))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[4] = {
      {dim0, inst0}, {dim1, inst1}, {dim2, inst2}, {dim3, inst3}};
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 4, &l0Fn)) {
    if (outFn) *outFn = l0Fn;
    if (outBucket) *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit4D(funcIndex, dim0, inst0, dim1, inst1, dim2,
                                inst2, dim3, inst3);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 4);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 4);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 4, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 4);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dims, 4);
  return taskpoolStatus(r.status);
}

void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId,
                                        uint32_t enabled) {
  EJIT_DIAG_VERBOSE("taskpool_set_instance_enabled dim=%u inst=%u enabled=%u",
                    dimType, instanceId, enabled);
  if (!gEJIT) {
    EJIT_DIAG("taskpool_set_instance_enabled reject: not initialized");
    return;
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJitSharedTaskPool *sp = gEJIT->sharedTaskPool();
  if (!sp) {
    EJIT_DIAG("taskpool_set_instance_enabled reject: no shared taskpool");
    return;
  }
  sp->setInstanceEnabled(dimType, instanceId, enabled != 0);
#else
  EJitTaskPool *tp = gEJIT->taskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_set_instance_enabled reject: no taskpool");
    return;
  }
  tp->switchController().setEnabled(dimType, instanceId, enabled != 0);
#endif
}

void ejit_taskpool_release_read(uint32_t bucketIndex) {
  if (!gEJIT) {
    EJIT_DIAG("taskpool_release_read bucket=%u reject: not initialized",
              bucketIndex);
    return;
  }
  auto *tp = activeTaskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_release_read bucket=%u reject: no taskpool", bucketIndex);
    return;
  }
  tp->releaseRead(bucketIndex);
}

#ifdef EJIT_SRE_TASKPOOL_TESTING
unsigned ejit_taskpool_poll_one(void) {
  if (!gEJIT) {
    EJIT_DIAG("taskpool_poll_one reject: not initialized");
    return 0;
  }
  auto *tp = activeTaskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_poll_one reject: no taskpool");
    return 0;
  }
  return tp->pollOne() ? 1u : 0u;
}

unsigned ejit_taskpool_poll_budget(unsigned maxItems) {
  if (!gEJIT) {
    EJIT_DIAG("taskpool_poll_budget max=%u reject: not initialized", maxItems);
    return 0;
  }
  auto *tp = activeTaskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_poll_budget max=%u reject: no taskpool", maxItems);
    return 0;
  }
  return tp->pollBudget(maxItems);
}
#endif

unsigned ejit_taskpool_pending_count(void) {
  if (!gEJIT) {
    EJIT_DIAG("taskpool_pending_count reject: not initialized");
    return 0;
  }
  auto *tp = activeTaskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_pending_count reject: no taskpool");
    return 0;
  }
  return tp->pendingCount();
}

uint64_t ejit_taskpool_trace_now(void) {
#ifdef EJIT_FREESTANDING
  return SRE_CycleCountGet64();
#else
  using clock = std::chrono::steady_clock;
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          clock::now().time_since_epoch())
          .count());
#endif
}

void ejit_taskpool_trace_wrapper(uint32_t funcIndex, uint32_t status,
                                 void *fnPtr, uint32_t bucketIndex,
                                 uint64_t tBeforeLookup,
                                 uint64_t tAfterLookup,
                                 uint64_t tAfterFn,
                                 uint64_t tAfterRelease) {
  uint64_t getFn = tAfterLookup - tBeforeLookup;
  uint64_t fnCall = tAfterFn - tAfterLookup;
  uint64_t release = tAfterRelease - tAfterFn;
  uint64_t total = tAfterRelease - tBeforeLookup;
  // No lock: under per-core BSS each core has its own gWrapperTimingSlots,
  // so there is no cross-core contention on the slot.
  unsigned SlotIdx = funcIndex % (sizeof(gWrapperTimingSlots) /
                                  sizeof(gWrapperTimingSlots[0]));
  WrapperTimingSlot &S = gWrapperTimingSlots[SlotIdx];
  // Aggregate by (funcIndex, status) only - fnPtr/bucket are deliberately NOT
  // part of the key. With EJIT_SRE_SHARED_CODE_POINTERS off the same function
  // returns a different fnPtr per core, and recompilation changes fnPtr too;
  // keying on it displaced the slot on nearly every hit, so reportTimingSlot
  // fired on every churn and flooded the log regardless of
  // EJIT_WRAPPER_TIMING_REPORT_EVERY (which only gates the periodic print
  // below). On a real function change we now reset SILENTLY: the displaced
  // window is always a partial (< EVERY) one, and printing it on every churn
  // was the flood source. Stable functions still report via the EVERY periodic
  // print; colliding functions (>32, funcIndex % 32) just reset silently.
  if (!S.Valid || S.FuncIndex != funcIndex || S.Status != status) {
    resetTimingSlot(S, funcIndex, status, fnPtr, bucketIndex);
  } else {
    // Same function: refresh the informational fnPtr/bucket to the latest hit
    // so the report's fn=/bucket= reflect the current specialization/core
    // rather than a stale first-seen value.
    S.FnPtr = fnPtr;
    S.BucketIndex = bucketIndex;
  }

  ++S.Count;
  S.GetFnSum += getFn;
  S.FnCallSum += fnCall;
  S.ReleaseSum += release;
  S.TotalSum += total;

#if EJIT_WRAPPER_TIMING_REPORT_EVERY > 0
  if ((S.Count % EJIT_WRAPPER_TIMING_REPORT_EVERY) == 0) {
    reportTimingSlot(S);
    S.Count = 0;
    S.GetFnSum = 0;
    S.FnCallSum = 0;
    S.ReleaseSum = 0;
    S.TotalSum = 0;
  }
#else
  (void)tAfterRelease;
#endif
}

ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out) {
  if (!out) {
    EJIT_DIAG("taskpool_get_stats failed: null out pointer");
    return EJIT_ERR_INVALID_PARAM;
  }
  if (!gEJIT) {
    EJIT_DIAG("taskpool_get_stats failed: not initialized");
    return EJIT_ERR_NOT_ACTIVE;
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJitSharedTaskPool *sp = gEJIT->sharedTaskPool();
  if (!sp) {
    EJIT_DIAG("taskpool_get_stats failed: no shared taskpool");
    return EJIT_ERR_NOT_ACTIVE;
  }
  EJitSharedDiagnostics d;
  sp->getDiagnostics(d);
  out->cacheHits = d.cacheHits;
  out->asyncCompiles = d.asyncCompiles;
  out->asyncEnqueues = d.asyncEnqueues;
  out->alreadyPending = d.alreadyPending;
  out->queueFull = d.queueFull;
  out->compileFailed = d.compileFailed;
  out->publishFailed = d.publishFailed;
  out->instanceDisabled = d.instanceDisabled;
  out->instanceDisabledPreActivate = d.instanceDisabledPreActivate;
  out->readyEntries = d.cacheReadyCount;
  out->pendingEntries = d.pendingCount;
  out->queueApproxSize = d.queueDepth;
  out->reserved = 0;
  return EJIT_OK;
#else
  EJitTaskPool *tp = gEJIT->taskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_get_stats failed: no taskpool");
    return EJIT_ERR_NOT_ACTIVE;
  }

  EJitTaskPoolStatsSnapshot s;
  tp->getStats(s);
  out->cacheHits = s.cacheHits;
  out->asyncCompiles = s.asyncCompiles;
  out->asyncEnqueues = s.asyncEnqueues;
  out->alreadyPending = s.alreadyPending;
  out->queueFull = s.queueFull;
  out->compileFailed = s.compileFailed;
  out->publishFailed = s.publishFailed;
  out->instanceDisabled = s.instanceDisabled;
  out->instanceDisabledPreActivate = 0;
  out->readyEntries = s.readyEntries;
  out->pendingEntries = s.pendingEntries;
  out->queueApproxSize = s.queueApproxSize;
  out->reserved = 0;
  return EJIT_OK;
#endif
}

void ejit_taskpool_print_stats() {
  ejit_taskpool_stats_t s = {0};
  ejit_taskpool_get_stats(&s);
  EJIT_DIAG("stats_t:");
  EJIT_DIAG("  cacheHits        = %llu",
            static_cast<unsigned long long>(s.cacheHits));
  EJIT_DIAG("  asyncCompiles    = %llu",
            static_cast<unsigned long long>(s.asyncCompiles));
  EJIT_DIAG("  asyncEnqueues    = %llu",
            static_cast<unsigned long long>(s.asyncEnqueues));
  EJIT_DIAG("  alreadyPending   = %llu",
            static_cast<unsigned long long>(s.alreadyPending));
  EJIT_DIAG("  queueFull        = %llu",
            static_cast<unsigned long long>(s.queueFull));
  EJIT_DIAG("  compileFailed    = %llu",
            static_cast<unsigned long long>(s.compileFailed));
  EJIT_DIAG("  publishFailed    = %llu",
            static_cast<unsigned long long>(s.publishFailed));
  EJIT_DIAG("  instanceDisabled = %llu",
            static_cast<unsigned long long>(s.instanceDisabled));
  EJIT_DIAG("  instanceDisabledPreActivate  = %llu   (init->activate window)",
            static_cast<unsigned long long>(s.instanceDisabledPreActivate));
  EJIT_DIAG("  instanceDisabledPostActivate = %llu   (after first activate)",
            static_cast<unsigned long long>(
                s.instanceDisabled > s.instanceDisabledPreActivate
                    ? s.instanceDisabled - s.instanceDisabledPreActivate
                    : 0));
  EJIT_DIAG("  readyEntries     = %u", s.readyEntries);
  EJIT_DIAG("  pendingEntries   = %u", s.pendingEntries);
  EJIT_DIAG("  queueApproxSize  = %u", s.queueApproxSize);
  EJIT_DIAG("  reserved         = %u", s.reserved);
}

void ejit_taskpool_print_compiled() {
  if (!gEJIT) {
    EJIT_DIAG("print_compiled: not initialized");
    return;
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJitSharedTaskPool *sp = gEJIT->sharedTaskPool();
  if (!sp || !sp->state()) {
    EJIT_DIAG("print_compiled: no shared taskpool / state");
    return;
  }
  EJitModuleLoader &loader = gEJIT->moduleLoader();
  EJIT_DIAG("compiled functions:");
  sp->forEachCompiled(
      [](uint32_t funcIndex, const EJitDimPair *dims, uint32_t numDims,
         void *fnPtr, void *ctx) {
#ifdef EJIT_DIAG_ENABLE
        const auto &loader = *static_cast<EJitModuleLoader *>(ctx);
        const std::string &name = loader.getFuncNameByFuncIdx(funcIndex);
        EJIT_DIAG("  funcIdx=%u name=%s numDims=%u "
                  "dims=[%u:%u,%u:%u,%u:%u,%u:%u] fn=%p",
                  funcIndex, name.empty() ? "<unknown>" : name.c_str(), numDims,
                  numDims > 0 ? dims[0].dimType : 0,
                  numDims > 0 ? dims[0].instanceId : 0,
                  numDims > 1 ? dims[1].dimType : 0,
                  numDims > 1 ? dims[1].instanceId : 0,
                  numDims > 2 ? dims[2].dimType : 0,
                  numDims > 2 ? dims[2].instanceId : 0,
                  numDims > 3 ? dims[3].dimType : 0,
                  numDims > 3 ? dims[3].instanceId : 0, fnPtr);
#else
        (void)funcIndex;
        (void)dims;
        (void)numDims;
        (void)fnPtr;
        (void)ctx;
#endif
      },
      &loader);
#else
  EJIT_DIAG("print_compiled: shared taskpool not enabled");
#endif
}

void ejit_dump_func(const char *name) {
  // gDumpSharedState is bound once in ejit_init (and cleared in ejit_shutdown);
  // no per-call rebind needed here.
  std::string filter = (name && name[0]) ? std::string(name) : std::string();
  EJIT_DIAG("dump_func filter=%s", filter.empty() ? "(off)" : filter.c_str());
  setDumpFuncFilter(filter);
}

void ejit_print_dumped(const char *name) {
  // gDumpSharedState is bound once in ejit_init; no per-call rebind needed.
  EJIT_DIAG("print_dumped name=%s", (name && name[0]) ? name : "(all)");
  printDumped(name);
}

void ejit_dump_all(bool enable) {
  EJIT_DIAG("dump_all enable=%u", enable ? 1u : 0u);
  setDumpFuncFilter(enable ? std::string("*") : std::string());
}

// Sentinel returned when no owner core is elected (e.g. not initialized or the
// shared taskpool has not bound state). Distinct from any valid core id.
constexpr uint32_t kEJitInvalidOwnerCore = 0xFFFFFFFFu;

uint32_t ejit_taskpool_get_worker_core() {
  if (!gEJIT)
    return kEJitInvalidOwnerCore;
#ifdef EJIT_SRE_SHARED_TASKPOOL
  EJitSharedTaskPool *sp = gEJIT->sharedTaskPool();
  if (!sp)
    return kEJitInvalidOwnerCore;
  EJitSharedTaskPoolState *sp_state = sp->state();
  if (!sp_state)
    return kEJitInvalidOwnerCore;
  return sp_state->ownerCoreId.loadAcquire();
#else
  // The single owner-worker model only exists in the shared taskpool build;
  // a private per-instance taskpool has no cross-core owner to report.
  return kEJitInvalidOwnerCore;
#endif
}

void ejit_set_log_level(ejit_log_level_t level) {
  int v = static_cast<int>(level);
  if (v < EJIT_LOG_LVL_OFF)
    v = EJIT_LOG_LVL_OFF;
  if (v > EJIT_LOG_LVL_DEBUG)
    v = EJIT_LOG_LVL_DEBUG;
  gEJitDiagLevel = v;
  EJIT_DIAG("log_level=%d", gEJitDiagLevel);
}

ejit_log_level_t ejit_get_log_level(void) {
  return static_cast<ejit_log_level_t>(gEJitDiagLevel);
}

void ejit_print_registry(void) {
  if (!gEJIT) {
    EJIT_DIAG("print_registry: not initialized");
    return;
  }
  gEJIT->printRegistry();
}

void ejit_print_func_meta(const char *funcName) {
  if (!funcName || !funcName[0]) {
    EJIT_DIAG("print_func_meta: null/empty name");
    return;
  }
  if (!gEJIT) {
    EJIT_DIAG("print_func_meta: not initialized");
    return;
  }
  gEJIT->printFuncMeta(funcName);
}

ejit_status_t ejit_get_code_pool_stats(ejit_code_pool_stats_t *out) {
  if (!out) {
    EJIT_DIAG("get_code_pool_stats: null out pointer");
    return EJIT_ERR_INVALID_PARAM;
  }
  if (!gEJIT) {
    EJIT_DIAG("get_code_pool_stats: not initialized");
    return EJIT_ERR_NOT_ACTIVE;
  }
  if (!gEJIT->getCodePoolStats(out)) {
    EJIT_DIAG("get_code_pool_stats: no code pool (EJIT_SRE_CODE_POOL off or no engine)");
    return EJIT_ERR_DISABLED;
  }
  return EJIT_OK;
}

void ejit_print_code_pool_stats(void) {
  if (!gEJIT) {
    EJIT_DIAG("print_code_pool_stats: not initialized");
    return;
  }
  gEJIT->printCodePoolStats();
}

void ejit_print_active(void) {
  if (!gEJIT) {
    EJIT_DIAG("print_active: not initialized");
    return;
  }
  gEJIT->printActive();
}

void ejit_print_version(void) {
  // LLVM release version (major.minor.patch) comes from llvm/Config/llvm-config.h
  // (LLVM_VERSION_STRING); the git commit + branch come from the build-time
  // generated EJitVersion.h. Printed unconditionally - not through EJIT_DIAG and
  // not gated on EJIT_DIAG_ENABLE or gEJitDiagLevel - so the build identity is
  // always recoverable, even on a build with diagnostics compiled out and before
  // ejit_init(). Routes through the same platform sink as EJIT_DIAG: SRE_printf
  // on SRE/bare-metal builds (declared at file scope above / by EJitDiag.h),
  // std::printf otherwise.
#ifdef EJIT_SRE_DIAG
  SRE_printf("[EJIT] LLVM version %s, branch %s, commit %s\n",
             LLVM_VERSION_STRING, EJIT_GIT_BRANCH, EJIT_GIT_COMMIT);
#else
  std::printf("[EJIT] LLVM version %s, branch %s, commit %s\n",
              LLVM_VERSION_STRING, EJIT_GIT_BRANCH, EJIT_GIT_COMMIT);
#endif
}

} // extern "C"
