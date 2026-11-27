//===-- EJitRuntime.cpp - EmbeddedJIT C Runtime API -----------------------===//

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/EJIT/EJit.h"
#include "llvm/ExecutionEngine/EJIT/EJitAtomic.h"
#include "llvm/ExecutionEngine/EJIT/EJitCommon.h" // contract constants + kEJitMax*
#include "llvm/ExecutionEngine/EJIT/EJitDiag.h"
#include "llvm/ExecutionEngine/EJIT/EJitFuncRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitLifecycleRegistry.h"
#include "llvm/ExecutionEngine/EJIT/EJitOptions.h"
#include "llvm/ExecutionEngine/EJIT/EJitOrcEngine.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistrationStore.h"
#include "llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h" // ejit_reg_entry_t layout
#include "llvm/ExecutionEngine/EJIT/EJitRuntimeState.h"
#include "llvm/ExecutionEngine/EJIT/EJitTaskPoolTypes.h" // EJitDimPair layout
// Build-time-generated: EJIT_GIT_COMMIT / EJIT_GIT_BRANCH (git HEAD of the
// llvm-project source tree). Lives in the LLVMEJIT build directory.
#include "EJitVersion.h"
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
#include "EJitBootstrapPool.h"
#include "EJitCodePoolCounters.h"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#endif
// ejit_print_version() falls back to std::printf when not routing through the
// SRE platform sink. Mirrors the EJIT_DIAG <cstdio> include (non-SRE path).
#ifndef EJIT_SRE_DIAG
#include <cstdio>
#endif
#include <algorithm>
#include <cstddef>
#include <type_traits>

using namespace llvm;
using namespace llvm::ejit;

//===----------------------------------------------------------------------===//
// Compile-time contract checks: each assert locks two independently defined
// copies of one config value (AOT pass vs runtime, or runtime header vs
// runtime header). Editing one copy without the other now fails the LLVMEJIT
// build instead of silently desynchronizing — the exact class of bug that
// shipped as EJIT_ICACHE_FUNC_SLOTS=64 vs a 4096 funcIndex space.
//===----------------------------------------------------------------------===//

// Log-level gate thresholds (EJitDiag.h macros) vs the public C ABI enum
// (EJitRuntime.h): ejit_set_log_level() feeds the macros, so a drift would
// change which diagnostics fire for a given API level.
static_assert(EJIT_LOG_LVL_OFF == EJIT_LOG_OFF &&
                  EJIT_LOG_LVL_INFO == EJIT_LOG_INFO &&
                  EJIT_LOG_LVL_VERBOSE == EJIT_LOG_VERBOSE &&
                  EJIT_LOG_LVL_DEBUG == EJIT_LOG_DEBUG,
              "EJIT_LOG_LVL_* (EJitDiag.h) must equal ejit_log_level_t "
              "(EJitRuntime.h) values.");

// Registry entry ABI: {i32 type; ptr; ptr; ptr; u64 size}, 8-byte aligned,
// 40 bytes on 64-bit. The AOT passes emit these as constant structs and the
// runtime walks them in EJit.cpp; the linker scripts (ejit_registry.ld,
// ejit_baremetal.ld) hardcode the same alignment. EJIT targets are 64-bit
// (aarch64_be / x86_64).
#if UINTPTR_MAX == UINT64_MAX
static_assert(sizeof(ejit_reg_entry_t) == 40,
              "ejit_reg_entry_t must be 40 bytes on 64-bit "
              "(AOT-emitted registry entry layout).");
static_assert(alignof(ejit_reg_entry_t) == 8,
              "ejit_reg_entry_t must be 8-byte aligned "
              "(linker script ALIGN(8) contract).");
#endif

// Dim-pair identity layout: {uint32_t dimType, uint32_t instanceId}, field
// order dimType-first, identical in the C ABI type and the queue POD type.
// The AOT wrapper builds this struct in IR with the same order; a swap would
// key the cache on the wrong identity.
static_assert(offsetof(ejit_dim_pair_t, dimType) ==
                      offsetof(EJitDimPair, dimType) &&
                  offsetof(ejit_dim_pair_t, instanceId) ==
                      offsetof(EJitDimPair, instanceId) &&
                  sizeof(ejit_dim_pair_t) == sizeof(EJitDimPair),
              "ejit_dim_pair_t (C ABI) and EJitDimPair (queue POD) must have "
              "identical {dimType, instanceId} layout.");

// AOT registry limits must agree with the shared runtime tables.
static_assert(kEJitMaxDimTypes == kEJitSharedDimTypes,
              "kEJitMaxDimTypes (EJitCommon.h) must equal kEJitSharedDimTypes "
              "(EJitSharedTaskPoolState.h).");
static_assert(kEJitMaxInstances == kEJitSharedInstances,
              "kEJitMaxInstances (EJitCommon.h) must equal "
              "kEJitSharedInstances (EJitSharedTaskPoolState.h).");
static_assert(kEJitMaxFuncIndex == kEJitSharedMaxFuncIndex,
              "kEJitMaxFuncIndex (EJitCommon.h) must equal "
              "kEJitSharedMaxFuncIndex (EJitSharedTaskPoolState.h).");

static EJit *gEJIT = nullptr;

#ifdef EJIT_DIAG_ENABLE
EJitAtomicU64 gIcacheNullFillSkips;
constexpr uint64_t kIcacheNullFillLogEvery = 1000;
#endif

// ejit_print_version() prints unconditionally (not via EJIT_DIAG), so on SRE
// builds it calls SRE_printf directly. EJitDiag.h only declares SRE_printf
// under EJIT_DIAG_ENABLE; declare it here for the SRE-but-diagnostics-off case
// so the function links. When EJIT_DIAG_ENABLE is also on, EJitDiag.h already
// provides the identical declaration and this guard skips a redundant one.
#if defined(EJIT_SRE_DIAG) && !defined(EJIT_DIAG_ENABLE)
extern "C" int SRE_printf(const char *fmt, ...);
#endif

static void bindDumpSharedStateFromRuntime() {
  setDumpSharedState(gEJIT->sharedTaskPool()->state());
}

static EJitSharedTaskPool *taskpoolForApi(bool RequireReady = true) {
  if (gEJIT)
    return gEJIT->sharedTaskPool();
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  return getEJitBootstrapPool(RequireReady);
#else
  (void)RequireReady;
  return nullptr;
#endif
}

#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
static ejit_status_t changeBootstrapPeriod(const char *Name, uint32_t Instance,
                                            bool Enabled, bool All = false) {
  auto *Pool = getEJitBootstrapPool();
  if (!Pool)
    return EJIT_ERR_NOT_ACTIVE;
  uint32_t Dim = lookupEJitBootstrapDim(Name);
  if (Dim == kEJitInvalidDimType || (!All && Instance >= kEJitSharedInstances))
    return EJIT_ERR_INVALID_PARAM;
  if (All)
    Pool->setAllInstancesEnabled(Dim, Enabled);
  else
    Pool->setInstanceEnabled(Dim, Instance, Enabled);
  return EJIT_OK;
}

#ifdef EJIT_DUMP_SUPPORT
// Debug commands may use dump storage; the first business miss must not.
static bool bindBootstrapDumpIfNeeded() {
  if (gEJIT)
    return true;
  auto *Pool = getEJitBootstrapPool();
  if (!Pool)
    return false;
  static EJitAtomicU32 Bound;
  uint32_t Expected = 0;
  if (Bound.compareExchange(Expected, 1)) {
    setDumpSharedState(Pool->state());
    Bound.storeRelease(2);
    return true;
  }
  return Expected == 2;
}
#endif
#endif

static void parseConfig(const ejit_config_t *src, Config &dst) {
  if (!src)
    return;
  dst.compileMode = (src->compileMode == EJIT_COMPILE_ASYNC)
                        ? CompileMode::Async
                        : CompileMode::Off;
  dst.optLevel = static_cast<OptimizationLevel>(src->optLevel);
  dst.forceStaticRegistry = src->forceStaticRegistry;
  if (src->dumpJITDir && src->dumpJITDir[0])
    dst.dumpJITDir = src->dumpJITDir;
}

// Both C init entry points take this same unversioned config. Keep the
// registry flag and directory pointer at the offsets used by firmware.
static_assert(sizeof(ejit_compile_mode_t) == 4 && sizeof(ejit_opt_level_t) == 4);
static_assert(offsetof(ejit_config_t, compileMode) == 0);
static_assert(offsetof(ejit_config_t, optLevel) == 4);
static_assert(offsetof(ejit_config_t, forceStaticRegistry) == 8);
static_assert(offsetof(ejit_config_t, dumpJITDir) ==
              (sizeof(void *) == 8 ? 16 : 12));
static_assert(sizeof(ejit_config_t) == (sizeof(void *) == 8 ? 24 : 16));
static_assert(std::is_standard_layout<ejit_config_t>::value,
              "ejit_config_t must be standard-layout for a stable C ABI");

// Shared init implementation for ejit_init / ejit_init_pgo. \p forcePgo forces
// the online-PGO auto-trigger on regardless of the (unversioned) config.
static ejit_status_t ejitInitImpl(const ejit_config_t *config, bool forcePgo) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  if (EJitCoreId::current() != EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE)
    return EJIT_ERR_DISABLED;
#endif
  if (gEJIT) {
    EJIT_DIAG("already initialized, returning OK");
    return EJIT_OK;
  }

  if (config && config->compileMode != EJIT_COMPILE_ASYNC &&
      config->compileMode != EJIT_COMPILE_OFF) {
    EJIT_DIAG("unsupported compile mode: select Async or Off");
    return EJIT_ERR_INVALID_PARAM;
  }
  Config cfg;
  parseConfig(config, cfg);
#ifdef EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE
  // Fixed-worker deployments may skip EJIT constructors on producer cores.
  // Force every core through the linker-backed registry so funcIndex and
  // lifecycle fingerprints remain identical even when callers use
  // ejit_init(nullptr) and only the worker runs the LLVM init array.
  cfg.forceStaticRegistry = true;
#endif
  if (forcePgo)
    cfg.enablePgo = true;

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

  bindDumpSharedStateFromRuntime();
  EJIT_DIAG("initialized: mode=%d opt=%d pgo=%d static_registry=%d",
            (int)cfg.compileMode, (int)cfg.optLevel, (int)cfg.enablePgo,
            (int)cfg.forceStaticRegistry);
  return EJIT_OK;
}

extern "C" {

ejit_status_t ejit_init(const ejit_config_t *config) {
  return ejitInitImpl(config, /*forcePgo=*/false);
}

ejit_status_t ejit_init_pgo(const ejit_config_t *config) {
  return ejitInitImpl(config, /*forcePgo=*/true);
}

void ejit_shutdown(void) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  if (!gEJIT)
    return;
#endif
  EJIT_DIAG("shutting down");
  setDumpSharedState(nullptr);
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

ejit_status_t ejit_register_pointer_period_bounds(const char *varName,
                                                  const void *base,
                                                  uint64_t size) {
  // Registry readers on compiler workers are lock-free. Keep this additive
  // registration strictly pre-init, including in hosted/non-taskpool builds.
  if (gEJIT || !varName || !*varName)
    return EJIT_ERR_INVALID_PARAM;
  return EJitRegistrationStore::instance().registerPointerPeriodBounds(
             varName, base, size)
             ? EJIT_OK
             : EJIT_ERR_INVALID_PARAM;
}

ejit_status_t ejit_set_allocation_size_lookup(ejit_allocation_size_lookup_t lookup,
                                             void *context) {
  if (gEJIT)
    return EJIT_ERR_INVALID_PARAM;
  return EJitRegistrationStore::instance().setAllocationSizeLookup(
             {lookup, context})
             ? EJIT_OK
             : EJIT_ERR_INVALID_PARAM;
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
  if (gEJIT && gEJIT->registrationFrozen()) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_INVALID_PARAM, "funcIndex registration after init is frozen",
        funcName);
    EJIT_DIAG("register_funcindex reject name=%s: registration frozen",
              funcName);
    return;
  }
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
                               uint32_t numDims, void *missFn) {
  // Wire the wrapper's per-function @__ejit_icache_fn_<name> cell table into
  // the runtime slot registry, keyed by the SAME registry funcIndex
  // ejit_register_funcindex assigns by name. numDims is the [D]^numDims shape
  // so icacheFill can linearize and icacheDrainAll knows how far to walk.
  // missFn is non-null exactly for sentinel-form slots (their table is defined
  // pre-filled with &MissFn and the branchless probe BLRs the cell): the
  // runtime writes it back on drain / fill-retract so the cell never holds a
  // non-callable value. The wrapper reads a cell directly on the icache hit
  // path. Idempotent by name
  // (resolveAssign is), and every core registering the same shared table
  // records the same base. A null slot, an unresolvable name, or a numDims
  // above the cap is recorded; the base stays null and the wrapper's probe
  // cleanly misses -> taskpool fallback.
  if (!funcName || !slot) {
    EJIT_DIAG("register_icache_slot reject: name=%p slot=%p",
              (const void *)funcName, slot);
    return;
  }
  EJIT_DIAG_VERBOSE("register_icache_slot name=%s numDims=%u missFn=%p",
                    funcName, numDims, missFn);
  if (gEJIT && gEJIT->registrationFrozen()) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_INVALID_PARAM, "icache slot registration after init is frozen",
        funcName);
    EJIT_DIAG("register_icache_slot reject name=%s: registration frozen",
              funcName);
    return;
  }
  uint32_t idx = EJitFuncRegistry::instance().resolveAssign(funcName);
  if (idx == kEJitInvalidFuncIndex) {
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_CACHE_FULL, "funcIndex capacity exhausted for icache slot",
        funcName);
    EJIT_DIAG("register_icache_slot FAIL name=%s: funcIndex capacity exhausted",
              funcName);
    return;
  }
  switch (ejitIcacheRegisterSlot(idx, slot, numDims, missFn)) {
  case EJitIcacheRegResult::Ok:
    EJIT_DIAG_VERBOSE("register_icache_slot OK name=%s idx=%u numDims=%u",
                      funcName, idx, numDims);
    break;
  case EJitIcacheRegResult::CapacityMiss:
    // Expected in a large application: more ejit_entry functions than
    // EJIT_ICACHE_FUNC_SLOTS. Not recorded as an error -- a registration error
    // during construction fails ejit_init, and losing an optional fast path on
    // one function must never do that.
    EJIT_DIAG("register_icache_slot name=%s: no free inline-cache slot "
              "(idx=%u >= %u), continuing without the fast path",
              funcName, idx, EJIT_ICACHE_FUNC_SLOTS);
    break;
  case EJitIcacheRegResult::Invalid:
    EJitRegistrationStore::instance().recordError(
        EJIT_ERR_INVALID_PARAM,
        "icache slot invalid: null base or numDims above the cap", funcName);
    EJIT_DIAG("register_icache_slot FAIL name=%s: numDims=%u above the cap %u",
              funcName, numDims, EJIT_ICACHE_MAX_DIMS);
    break;
  }
}

ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx) {
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    return changeBootstrapPeriod(periodName, cellIdx, true);
#else
    EJIT_DIAG("activate(%s,%u) failed: not initialized", periodName, cellIdx);
    return EJIT_ERR_NOT_ACTIVE;
#endif
  }
  // The independent C++ entry checks cellIdx before narrowing it.
  if (!periodName)
    return EJIT_ERR_INVALID_PARAM;
  EJIT_DIAG("activate(%s,%u)", periodName, cellIdx);
  // In a taskpool build this also syncs the SwitchController and returns false
  // for an unknown lifecycle (no state changed). In the legacy build it always
  // succeeds.
  if (!gEJIT->activate(periodName, cellIdx))
    return EJIT_ERR_INVALID_PARAM;
  return EJIT_OK;
}

ejit_status_t ejit_deactivate(const char *periodName, uint32_t cellIdx) {
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    return changeBootstrapPeriod(periodName, cellIdx, false);
#else
    EJIT_DIAG("deactivate(%s,%u) failed: not initialized", periodName, cellIdx);
    return EJIT_ERR_NOT_ACTIVE;
#endif
  }
  if (!periodName)
    return EJIT_ERR_INVALID_PARAM;
  EJIT_DIAG("deactivate(%s,%u)", periodName, cellIdx);
  if (!gEJIT->deactivate(periodName, cellIdx))
    return EJIT_ERR_INVALID_PARAM; // unknown lifecycle: nothing changed.
  gEJIT->invalidateByPeriod(periodName, cellIdx);
  return EJIT_OK;
}

ejit_status_t ejit_activate_all(const char *periodName) {
  EJIT_DIAG("activate_all(%s)", periodName);
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    return changeBootstrapPeriod(periodName, 0, true, true);
#else
    EJIT_DIAG("activate_all(%s) failed: not initialized", periodName);
    return EJIT_ERR_NOT_ACTIVE;
#endif
  }
  if (!periodName || !gEJIT->activateAll(periodName))
    return EJIT_ERR_INVALID_PARAM;
  return EJIT_OK;
}

ejit_status_t ejit_deactivate_all(const char *periodName) {
  EJIT_DIAG("deactivate_all(%s)", periodName);
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    return changeBootstrapPeriod(periodName, 0, false, true);
#else
    EJIT_DIAG("deactivate_all(%s) failed: not initialized", periodName);
    return EJIT_ERR_NOT_ACTIVE;
#endif
  }
  if (!periodName || !gEJIT->deactivateAll(periodName))
    return EJIT_ERR_INVALID_PARAM;
  gEJIT->invalidateAllByPeriod(periodName);
  return EJIT_OK;
}

bool ejit_is_active(const char *periodName, uint32_t cellIdx) {
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    auto *Pool = getEJitBootstrapPool();
    return Pool && Pool->isInstanceActive(lookupEJitBootstrapDim(periodName),
                                          cellIdx);
#else
    EJIT_DIAG("is_active(%s,%u) failed: not initialized", periodName, cellIdx);
    return false;
#endif
  }
  if (!periodName)
    return false;
  return gEJIT->isActive(periodName, cellIdx);
}

void ejit_clear_cache(void) {
  EJIT_DIAG("clear_cache");
  if (gEJIT)
    gEJIT->clearCache();
  if (auto *Pool = taskpoolForApi())
    Pool->retireDispatchCache();
}

void ejit_invalidate(const char *periodName, uint32_t cellIdx) {
  EJIT_DIAG("invalidate(%s,%u)", periodName, cellIdx);
  auto *Pool = taskpoolForApi();
  if (!Pool)
    return;
  // Reject out-of-range indices instead of truncating at the ABI boundary and
  // invalidating the wrong instance (pre-fix uint8_t signature).
  if (cellIdx >= kEJitMaxInstances) {
    EJIT_DIAG(
        "invalidate(%s,%u) rejected: instance index >= kEJitMaxInstances=%u",
        periodName, cellIdx, kEJitMaxInstances);
    return;
  }
  if (!periodName)
    return;
  if (gEJIT)
    gEJIT->invalidateByPeriod(periodName, cellIdx);
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  else if (lookupEJitBootstrapDim(periodName) == kEJitInvalidDimType)
    return;
#endif
  Pool->retireDispatchCache();
}

ejit_status_t ejit_get_stats(ejit_stats_t *stats) {
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  if (!stats)
    return EJIT_ERR_INVALID_PARAM;
  memset(stats, 0, sizeof(*stats));
  return EJIT_OK;
}

//===-- SRE taskpool black-box API ----------------------------------------===//

static ejit_status_t taskpoolStatus(EJitCompileOrGetStatus s) {
  switch (s) {
  case EJitCompileOrGetStatus::CacheHit:
    return EJIT_OK;
  case EJitCompileOrGetStatus::EnqueuedPending:
  case EJitCompileOrGetStatus::AlreadyPending:
    return EJIT_PENDING;
  case EJitCompileOrGetStatus::PgoAdmissionDeferred:
    // No work was queued. Surface a clean resource-limit fallback instead of
    // claiming that an asynchronous compilation is pending.
    return EJIT_ERR_QUEUE_FULL;
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
// Fill the icache cell on a successful taskpool resolve (cache hit or fresh
// compile), so a cold icache is filled on the first taskpool hit, not only on a
// fresh compile. dims selects the [D]^numDims cell.
inline void ejitIcacheFillOnSuccess(EJitSharedTaskPool &pool,
                                    uint32_t funcIndex, void *fnPtr,
                                    const EJitDimPair *dims, uint32_t numDims,
                                    uint64_t token) {
  // Deliberately silent. A null fnPtr is the NORMAL outcome of a resolve while
  // a compile is still pending, so every call to every entry takes this path
  // until the JIT warms up -- logging it drowned the board's log in lines that
  // report nothing wrong. The decline reasons that ARE worth seeing live in
  // icacheFill(), one-shot per reason.
  if (!fnPtr) {
#ifdef EJIT_DIAG_ENABLE
    if (gEJitDiagLevel >= EJIT_LOG_LVL_DEBUG) {
      uint64_t skips = gIcacheNullFillSkips.fetchAdd(1) + 1;
      if (skips % kIcacheNullFillLogEvery == 0)
        EJIT_DIAG_DEBUG("icacheFillOnSuccess null skips=%llu latest_func=%u",
                        static_cast<unsigned long long>(skips), funcIndex);
    }
#endif
    return;
  }
  pool.icacheFill(funcIndex, fnPtr, dims, numDims, token);
}
} // namespace

} // extern "C"

namespace llvm::ejit {
struct EJitRuntimeDispatch {
  static ejit_status_t resolve(uint32_t funcIndex, const ejit_dim_pair_t *dims,
                               uint32_t numDims,
                               const EJitBoundPtrDescriptor *bounds,
                               uint32_t boundCount, void **outFn,
                               uint32_t *outBucket);
};
} // namespace llvm::ejit

ejit_status_t EJitRuntimeDispatch::resolve(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const EJitBoundPtrDescriptor *boundPointers, uint32_t boundCount,
    void **outFn, uint32_t *outBucket) {
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
  auto *tp = taskpoolForApi();
  if (!tp)
    return EJIT_ERR_NOT_ACTIVE;
  if (!validateBoundPtrDescriptors(boundPointers, boundCount)) {
    EJIT_DIAG("taskpool_compile_or_get reject func=%u: invalid bound pointer "
              "list count=%u",
              funcIndex, boundCount);
    return EJIT_ERR_INVALID_PARAM;
  }
  if (numDims > kEJitSharedMaxDims) {
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
    if (dims[i].dimType >= kEJitSharedDimTypes) {
      EJIT_DIAG(
          "taskpool_compile_or_get reject func=%u: dim[%u] dimType=%u OOR",
          funcIndex, i, dims[i].dimType);
      return EJIT_ERR_INVALID_PARAM;
    }
    if (dims[i].instanceId >= kEJitSharedInstances) {
      EJIT_DIAG(
          "taskpool_compile_or_get reject func=%u: dim[%u] instanceId=%u OOR",
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
  const uint64_t icTok = tp->icacheBeginResolve();
  void *l0Fn = nullptr;
  // L0 remains valid for bound calls: it contains only the identity -> code
  // pointer mapping, while bound descriptors are needed only on a miss or a
  // PGO Tier-2 arm. PGO enable/disable retires L0 via dispatchEpoch, so a
  // successful L0 hit cannot bypass an active PGO counter.
  if (tp->l0Try(funcIndex, dimsCast, numDims, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto r = tp->tryCacheHitValidated(funcIndex, dimsCast, numDims, boundPointers,
                                    boundCount);
  if (!r.fastPathTerminal)
    r = tp->compileOrGetValidated(funcIndex, dimsCast, numDims, nullptr,
                                  boundPointers, boundCount);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  EJIT_DIAG_VERBOSE("taskpool_compile_or_get func=%u status=%u fn=%p",
                    funcIndex, static_cast<unsigned>(r.status), r.fnPtr);
  ejitIcacheFillOnSuccess(*tp, funcIndex, r.fnPtr, dimsCast, numDims, icTok);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dimsCast, numDims);
  return taskpoolStatus(r.status);
}

extern "C" {

ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims,
                                           uint32_t numDims, void **outFn,
                                           uint32_t *outBucket) {
  return EJitRuntimeDispatch::resolve(funcIndex, dims, numDims, nullptr, 0,
                                      outFn, outBucket);
}

ejit_status_t ejit_taskpool_compile_or_get_bound(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const void *rawPtr, uint32_t rawSize, uint32_t boundArgIndex, void **outFn,
    uint32_t *outBucket) {
  // This compatibility entry point promises a bound request. Treat an empty
  // object as invalid instead of silently converting it into the generic,
  // unbound API request (which could return an unbound JIT cache entry).
  if (!rawPtr || rawSize == 0) {
    if (outFn)
      *outFn = nullptr;
    if (outBucket)
      *outBucket = 0;
    EJIT_DIAG("taskpool_compile_or_get_bound reject func=%u: null/empty "
              "bound object",
              funcIndex);
    return EJIT_ERR_INVALID_PARAM;
  }
  EJitBoundPtrDescriptor Bound{rawPtr, rawSize, boundArgIndex};
  return EJitRuntimeDispatch::resolve(funcIndex, dims, numDims, &Bound, 1u,
                                      outFn, outBucket);
}

ejit_status_t ejit_taskpool_compile_or_get_bound_v(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const ejit_bound_ptr_t *bounds, uint32_t boundCount, void **outFn,
    uint32_t *outBucket) {
  static_assert(sizeof(ejit_bound_ptr_t) == sizeof(EJitBoundPtrDescriptor),
                "C and C++ bound descriptor layouts must match");
  static_assert(offsetof(ejit_bound_ptr_t, rawPtr) ==
                    offsetof(EJitBoundPtrDescriptor, rawPtr),
                "C and C++ bound pointer offsets must match");
  static_assert(offsetof(ejit_bound_ptr_t, size) ==
                    offsetof(EJitBoundPtrDescriptor, size),
                "C and C++ bound size offsets must match");
  static_assert(offsetof(ejit_bound_ptr_t, argIndex) ==
                    offsetof(EJitBoundPtrDescriptor, argIndex),
                "C and C++ bound arg offsets must match");
  return EJitRuntimeDispatch::resolve(
      funcIndex, dims, numDims,
      reinterpret_cast<const EJitBoundPtrDescriptor *>(bounds), boundCount,
      outFn, outBucket);
}

void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId,
                                        uint32_t enabled) {
  EJIT_DIAG_VERBOSE("taskpool_set_instance_enabled dim=%u inst=%u enabled=%u",
                    dimType, instanceId, enabled);
  auto *sp = taskpoolForApi();
  if (!sp) {
    EJIT_DIAG("taskpool_set_instance_enabled reject: not initialized");
    return;
  }
  sp->setInstanceEnabled(dimType, instanceId, enabled != 0);
}

void ejit_taskpool_release_read(uint32_t bucketIndex) {
  auto *tp = taskpoolForApi(false);
  if (!tp) {
    EJIT_DIAG("taskpool_release_read bucket=%u reject: not initialized",
              bucketIndex);
    return;
  }
  tp->releaseRead(bucketIndex);
}

unsigned ejit_taskpool_pending_count(void) {
  auto *tp = taskpoolForApi();
  if (!tp) {
    EJIT_DIAG("taskpool_pending_count reject: not initialized");
    return 0;
  }
  return tp->pendingCount();
}

ejit_status_t ejit_publish_pending_code(void) {
#ifdef EJIT_CODE_POOL_BATCHED_PUBLISH
  auto *sp = taskpoolForApi();
  if (!sp) {
    EJIT_DIAG("publish_pending_code failed: not initialized");
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    return EJIT_ERR_NOT_ACTIVE;
#else
    return EJIT_ERR_DISABLED;
#endif
  }
  if (!sp->requestCodeBatchFlushAndWait()) {
    EJIT_DIAG("publish_pending_code failed: worker flush/enable");
    return EJIT_ERR_COMPILE_FAILED;
  }
  EJIT_DIAG("publish_pending_code OK");
  return EJIT_OK;
#else
  EJIT_DIAG("publish_pending_code disabled: manual batching not built");
  return EJIT_ERR_DISABLED;
#endif
}

ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out) {
  if (!out) {
    EJIT_DIAG("taskpool_get_stats failed: null out pointer");
    return EJIT_ERR_INVALID_PARAM;
  }
  auto *sp = taskpoolForApi();
  if (!sp) {
    EJIT_DIAG("taskpool_get_stats failed: not initialized");
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
}

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
ejit_status_t ejit_vp_get_stats(ejit_vp_stats_t *out) {
  if (!out) {
    EJIT_DIAG("vp_get_stats failed: null out pointer");
    return EJIT_ERR_INVALID_PARAM;
  }
  // The stats live in the collector's shared blob (not behind gEJIT), so they
  // are readable once the collector is initialized, with or without an EJIT
  // facade; the blob is zero until the first merge, which is also a valid
  // answer.
  EJitVpStatsOut s;
  ejitVpStatsSnapshot(s);
  out->merges = s.merges;
  out->icValueSites = s.icValueSites;
  out->memopValueSites = s.memopValueSites;
  out->scalarValueSites = s.scalarValueSites;
  out->scalarDropped = s.scalarDropped;
  out->scalarSpecialized = s.scalarSpecialized;
  out->reserved = 0;
  return EJIT_OK;
}
#endif

void ejit_taskpool_print_stats() {
  ejit_taskpool_stats_t s = {0};
  ejit_taskpool_get_stats(&s);
  EJIT_DIAG_RAW("stats_t:");
  EJIT_DIAG_RAW("  cacheHits        = %llu",
                static_cast<unsigned long long>(s.cacheHits));
  EJIT_DIAG_RAW("  asyncCompiles    = %llu",
                static_cast<unsigned long long>(s.asyncCompiles));
  EJIT_DIAG_RAW("  asyncEnqueues    = %llu",
                static_cast<unsigned long long>(s.asyncEnqueues));
  EJIT_DIAG_RAW("  alreadyPending   = %llu",
                static_cast<unsigned long long>(s.alreadyPending));
  EJIT_DIAG_RAW("  queueFull        = %llu",
                static_cast<unsigned long long>(s.queueFull));
  EJIT_DIAG_RAW("  compileFailed    = %llu",
                static_cast<unsigned long long>(s.compileFailed));
  EJIT_DIAG_RAW("  publishFailed    = %llu",
                static_cast<unsigned long long>(s.publishFailed));
  EJIT_DIAG_RAW("  instanceDisabled = %llu",
                static_cast<unsigned long long>(s.instanceDisabled));
  EJIT_DIAG_RAW(
      "  instanceDisabledPreActivate  = %llu   (init->activate window)",
      static_cast<unsigned long long>(s.instanceDisabledPreActivate));
  EJIT_DIAG_RAW(
      "  instanceDisabledPostActivate = %llu   (after first activate)",
      static_cast<unsigned long long>(
          s.instanceDisabled > s.instanceDisabledPreActivate
              ? s.instanceDisabled - s.instanceDisabledPreActivate
              : 0));
  EJIT_DIAG_RAW("  readyEntries     = %u", s.readyEntries);
  EJIT_DIAG_RAW("  pendingEntries   = %u", s.pendingEntries);
  EJIT_DIAG_RAW("  queueApproxSize  = %u", s.queueApproxSize);
  EJIT_DIAG_RAW("  reserved         = %u", s.reserved);

  // Shared-pool fields not mirrored in the C ABI stats struct: read straight
  // from the diagnostics snapshot (public getDiagnostics()), no ABI change.
  if (EJitSharedTaskPool *sp = gEJIT ? gEJIT->sharedTaskPool() : nullptr) {
    EJitSharedDiagnostics d;
    sp->getDiagnostics(d);
    EJIT_DIAG_RAW("  initState=%u ownerCore=%u gen=%u lastInitErr=%u "
                  "initAttempts=%u share=%u",
                  d.initState, d.ownerCoreId, d.generation, d.lastInitError,
                  d.initAttempts, d.codeSharingEnabled);
    EJIT_DIAG_RAW("  workerTaskId=%llu regFingerprint=%llu execPrepFailed=%llu",
                  static_cast<unsigned long long>(d.workerTaskId),
                  static_cast<unsigned long long>(d.registrationFingerprint),
                  static_cast<unsigned long long>(d.executePrepareFailed));
    EJIT_DIAG_RAW("  pgo active=%u/%u completed=%llu deferred=%llu "
                  "tier1=%llu tier2=%llu mergeFailed=%llu",
                  d.pgoActiveFunctionCount, d.pgoMaxActiveFunctions,
                  static_cast<unsigned long long>(d.pgoCompletedFunctions),
                  static_cast<unsigned long long>(d.pgoDeferredMisses),
                  static_cast<unsigned long long>(d.tier1Compiles),
                  static_cast<unsigned long long>(d.tier2Compiles),
                  static_cast<unsigned long long>(d.profileMergeFails));
  }
  // Diagnostic: dump the inline-cache slot registry to help diagnose
  // why cacheHits keeps growing despite icache being enabled. Hand the
  // module loader in so slots resolve to function names.
  ejitDumpIcacheSlots(gEJIT ? &gEJIT->moduleLoader() : nullptr);
}

#ifdef EJIT_DIAG_ENABLE
// NO_RECLAIM dumps are best-effort. Never use unchecked shared data as
// an array index, even when it can only affect diagnostic output.
static uint32_t compiledDimCount(const EJitSharedCacheSlot &slot) {
  return std::min<uint32_t>(slot.numDims, kEJitSharedMaxDims);
}
#endif

void ejit_taskpool_print_compiled() {
#ifndef EJIT_DIAG_ENABLE
  // Diagnostics compiled out: nothing can print, so do not walk the cache
  // (the walk takes bucket read locks for no output).
  return;
#else
  auto *sp = taskpoolForApi();
  if (!sp) {
    EJIT_DIAG_RAW("print_compiled: not initialized");
    return;
  }
  // Every entry line is INFO-gated; below INFO the walk would hold bucket
  // read locks while printing nothing.
  if (gEJitDiagLevel < EJIT_LOG_LVL_INFO)
    return;
  if (!sp->state()) {
    EJIT_DIAG_RAW("print_compiled: no shared taskpool / state");
    return;
  }
  EJitModuleLoader *loader = gEJIT ? &gEJIT->moduleLoader() : nullptr;
  // Two walks so the summary line can print FIRST: a count-only pass tallies
  // the numDims spread, then a print pass emits the throttled entry lines.
  // The walk is documented best-effort (not a snapshot), so a concurrent
  // publish between the passes can make the summary and the printed lines
  // diverge by the same margin as any walk-based dump.
  struct CountCtx {
    uint32_t byDims[kEJitSharedMaxDims + 1];
    uint32_t byTier[3];
    uint32_t byPool[3];
    uint32_t finalPostPublishSeen;
  } count = {{0}, {0}, {0}, 0};
  EJitSharedTaskPool::ForEachCompiledStats st = sp->forEachCompiled(
      [](const EJitSharedCacheSlot &slot, void *cbCtx) {
        ++static_cast<CountCtx *>(cbCtx)->byDims[compiledDimCount(slot)];
        uint8_t tier = slot.tier.loadRelaxed();
        if (tier <= kEJitTierPgoUse)
          ++static_cast<CountCtx *>(cbCtx)->byTier[tier];
        if (slot.poolKind <= static_cast<uint32_t>(EJitCodePoolKind::Far))
          ++static_cast<CountCtx *>(cbCtx)->byPool[slot.poolKind];
        if ((tier == kEJitTierBaseline || tier == kEJitTierPgoUse) &&
            slot.postPublishSeen.loadRelaxed() != 0)
          ++static_cast<CountCtx *>(cbCtx)->finalPostPublishSeen;
      },
      &count);
  // Summary line first (fixed line count, so no throttle): total, cache
  // occupancy, contended buckets skipped by the count walk, and the numDims
  // spread. Only the nonzero spread buckets are listed. 14 B per entry
  // (" 4d=4294967295").
  char byDims[(kEJitSharedMaxDims + 1) * 14 + 1];
  char *p = byDims;
  char *end = byDims + sizeof(byDims);
  for (uint32_t d = 0; d <= kEJitSharedMaxDims && p < end; ++d) {
    if (!count.byDims[d])
      continue;
    p += snprintf(p, (size_t)(end - p), "%s%ud=%u", p == byDims ? "" : " ", d,
                  count.byDims[d]);
  }
  if (p >= end)
    p = end - 1;
  *p = '\0';
  EJIT_DIAG_RAW("compiled: %u entries (%u/%u slots, %u buckets skipped)%s%s",
                st.visitedSlots, st.visitedSlots,
                kEJitSharedCacheBuckets * kEJitSharedCacheSlots,
                st.skippedBuckets, byDims[0] ? " byDims: " : "", byDims);
  EJIT_DIAG_RAW("compiled tiers: baseline=%u tier1_collecting=%u tier2=%u",
                count.byTier[kEJitTierBaseline],
                count.byTier[kEJitTierInstrumented],
                count.byTier[kEJitTierPgoUse]);
  EJIT_DIAG_RAW("compiled pools: near=%u far=%u unknown=%u",
                count.byPool[static_cast<uint32_t>(EJitCodePoolKind::Near)],
                count.byPool[static_cast<uint32_t>(EJitCodePoolKind::Far)],
                count.byPool[static_cast<uint32_t>(EJitCodePoolKind::Unknown)]);
#ifdef EJIT_STATS_ENABLE
  constexpr bool ReuseTrackingEnabled = true;
  const uint32_t FinalVersions = count.byTier[kEJitTierBaseline] +
                                 count.byTier[kEJitTierPgoUse];
  EJIT_DIAG_RAW("compiled final reuse: post_publish_seen=%u unseen=%u "
                "(tier1_collecting_excluded=%u)",
                count.finalPostPublishSeen,
                FinalVersions - count.finalPostPublishSeen,
                count.byTier[kEJitTierInstrumented]);
#else
  constexpr bool ReuseTrackingEnabled = false;
  EJIT_DIAG_RAW("compiled reuse: disabled (build with EJIT_STATS_ENABLE)");
#endif
  // Entry lines: one RAW (prefix-free) line per Ready slot, throttled after
  // each printed line.
  struct PrintCtx {
    EJitModuleLoader *loader;
    bool reuseTrackingEnabled;
    uint32_t threshold;
  } printCtx{loader, ReuseTrackingEnabled,
             sp->state()->tier2Threshold.loadAcquire()};
  EJitSharedTaskPool::ForEachCompiledStats st2 = sp->forEachCompiled(
      [](const EJitSharedCacheSlot &slot, void *cbCtx) {
        PrintCtx &ctx = *static_cast<PrintCtx *>(cbCtx);
        StringRef name;
        if (ctx.loader)
          name = ctx.loader->getFuncNameByFuncIdx(slot.funcIndex);
        const char *displayName = name.empty() ? "<unknown>" : name.data();
        // Per the publish protocol: fnPtr is read with acquire only after
        // state==Ready was observed with acquire (forEachCompiled did).
        void *fn = reinterpret_cast<void *>(slot.fnPtr.loadAcquire());
        const uint32_t n = compiledDimCount(slot);
        // dims=[d:i,...] for the n meaningful pairs, e.g. "1:5" / "1:5,2:7".
        // Sized for the uint32 worst case: kEJitSharedMaxDims x
        // ("4294967295:4294967295" = 21) + separators + NUL.
        char dims[kEJitSharedMaxDims * 21 + (kEJitSharedMaxDims - 1) + 1];
        char *p = dims;
        char *end = dims + sizeof(dims);
        for (uint32_t i = 0; i < n && p < end; ++i)
          p += snprintf(p, (size_t)(end - p), "%s%u:%u", i ? "," : "",
                        slot.dims[i].dimType, slot.dims[i].instanceId);
        if (p >= end)
          p = end - 1;
        *p = '\0';
        const char *PostPublishSeen =
            !ctx.reuseTrackingEnabled
                ? "disabled"
                : (slot.postPublishSeen.loadRelaxed() != 0 ? "yes" : "no");
        const uint8_t SlotTier = slot.tier.loadRelaxed();
        const char *Tier = SlotTier == kEJitTierPgoUse ? "tier2"
                           : SlotTier == kEJitTierInstrumented
                               ? "tier1-collecting"
                               : "baseline";
        const char *PoolKind =
            slot.poolKind == static_cast<uint32_t>(EJitCodePoolKind::Near)
                ? "near"
                : slot.poolKind == static_cast<uint32_t>(EJitCodePoolKind::Far)
                      ? "far"
                      : "unknown";
        char samples[sizeof(" samples=18446744073709551615/4294967295")] = {};
        if (SlotTier == kEJitTierInstrumented && ctx.threshold)
          snprintf(samples, sizeof(samples), " samples=%llu/%u",
                   static_cast<unsigned long long>(slot.hitCount.loadRelaxed()),
                   ctx.threshold);
        if (gEJitDiagLevel >= EJIT_LOG_LVL_VERBOSE) {
          // Same shape for the per-instance version snapshot (uint32 x
          // kEJitSharedMaxDims + separators + NUL).
          char ver[kEJitSharedMaxDims * 10 + (kEJitSharedMaxDims - 1) + 1];
          p = ver;
          end = ver + sizeof(ver);
          for (uint32_t i = 0; i < n && p < end; ++i)
            p += snprintf(p, (size_t)(end - p), "%s%u", i ? "," : "",
                          slot.versions[i]);
          if (p >= end)
            p = end - 1;
          *p = '\0';
          EJIT_DIAG_RAW("funcIdx=%u name=%s tier=%s numDims=%u dims=[%s] fn=%p "
                        "post_publish_seen=%s ver=[%s] size=%llu "
                        "pool=%s pool_id=%u "
                        "gen=%u%s",
                        slot.funcIndex,
                        displayName, Tier,
                        slot.numDims, dims, fn, PostPublishSeen, ver,
                        static_cast<unsigned long long>(slot.codeSize),
                        PoolKind, slot.poolId, slot.generation, samples);
        } else {
          EJIT_DIAG_RAW("funcIdx=%u name=%s tier=%s numDims=%u dims=[%s] fn=%p "
                        "pool=%s post_publish_seen=%s%s",
                        slot.funcIndex,
                        displayName, Tier,
                        slot.numDims, dims, fn, PoolKind, PostPublishSeen, samples);
        }
        ejitDiagPrintThrottle();
      },
      &printCtx);
  // The summary counted the first walk; if the entry walk itself skipped
  // contended buckets, its lines are incomplete relative to it — report the
  // shortfall (fixed line count, so no throttle) instead of dropping it.
  if (st2.skippedBuckets)
    EJIT_DIAG_RAW("compiled: %u buckets skipped during entry walk",
                  st2.skippedBuckets);
#endif
}

void ejit_dump_func(const char *name) {
#ifdef EJIT_DUMP_SUPPORT
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  if (!bindBootstrapDumpIfNeeded())
    return;
#endif
  // gDumpSharedState is bound once in ejit_init (and cleared in ejit_shutdown);
  // no per-call rebind needed here.
  std::string filter = (name && name[0]) ? std::string(name) : std::string();
  EJIT_DIAG("dump_func filter=%s", filter.empty() ? "(off)" : filter.c_str());
  setDumpFuncFilter(filter);
#endif
}

void ejit_print_dumped(const char *name) {
#ifdef EJIT_DUMP_SUPPORT
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  if (!bindBootstrapDumpIfNeeded())
    return;
#endif
  // gDumpSharedState is bound once in ejit_init; no per-call rebind needed.
  EJIT_DIAG_RAW("print_dumped name=%s", (name && name[0]) ? name : "(all)");
  (void)printDumped(name);
#endif
}

void ejit_print_dumped_module(const char *name) {
#ifdef EJIT_DUMP_SUPPORT
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
  if (!bindBootstrapDumpIfNeeded())
    return;
#endif
  EJIT_DIAG_RAW("print_dumped_module name=%s",
                (name && name[0]) ? name : "(all)");
  (void)printDumpedModule(name);
#endif
}

// Sentinel returned when no owner core is elected (e.g. not initialized or the
// shared taskpool has not bound state). Distinct from any valid core id.
constexpr uint32_t kEJitInvalidOwnerCore = 0xFFFFFFFFu;

uint32_t ejit_taskpool_get_worker_core() {
  auto *sp = taskpoolForApi();
  if (!sp)
    return kEJitInvalidOwnerCore;
  EJitSharedTaskPoolState *sp_state = sp->state();
  if (!sp_state)
    return kEJitInvalidOwnerCore;
  return sp_state->ownerCoreId.loadAcquire();
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
    EJIT_DIAG_RAW("print_registry: not initialized");
    return;
  }
  gEJIT->printRegistry();
}

void ejit_print_func_meta(const char *funcName) {
  if (!funcName || !funcName[0]) {
    EJIT_DIAG_RAW("print_func_meta: null/empty name");
    return;
  }
  if (!gEJIT) {
    EJIT_DIAG_RAW("print_func_meta: not initialized");
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
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    ejit_code_pool_stats_v2_t Stats;
    auto Status = ejit_get_code_pool_stats_v2(&Stats);
    if (Status == EJIT_OK)
      *out = Stats.total;
    return Status;
#else
    EJIT_DIAG("get_code_pool_stats: not initialized");
    return EJIT_ERR_NOT_ACTIVE;
#endif
  }
  if (!gEJIT->getCodePoolStats(out)) {
    EJIT_DIAG("get_code_pool_stats: no code pool (EJIT_SRE_CODE_POOL off or no "
              "engine)");
    return EJIT_ERR_DISABLED;
  }
  return EJIT_OK;
}

ejit_status_t ejit_get_code_pool_stats_v2(ejit_code_pool_stats_v2_t *out) {
  if (!out) {
    EJIT_DIAG("get_code_pool_stats_v2: null out pointer");
    return EJIT_ERR_INVALID_PARAM;
  }
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    auto *Pool = getEJitBootstrapPool();
    if (!Pool)
      return EJIT_ERR_NOT_ACTIVE;
    EJitCodePoolStatsOut Stats{};
    if (!Pool->readCodePoolStats(&Stats))
      return EJIT_ERR_DISABLED;
    copyPoolCounters(out->total, Stats);
    copyPoolCounters(out->near, Stats.near);
    copyPoolCounters(out->far, Stats.far);
    return EJIT_OK;
#else
    EJIT_DIAG("get_code_pool_stats_v2: not initialized");
    return EJIT_ERR_NOT_ACTIVE;
#endif
  }
  if (!gEJIT->getCodePoolStatsV2(out)) {
    EJIT_DIAG("get_code_pool_stats_v2: no code pool "
              "(EJIT_SRE_CODE_POOL off or no engine)");
    return EJIT_ERR_DISABLED;
  }
  return EJIT_OK;
}

void ejit_print_code_pool_stats(void) {
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    ejit_code_pool_stats_t Stats{};
    if (ejit_get_code_pool_stats(&Stats) == EJIT_OK) {
      EJIT_DIAG_RAW("code pool total: pools=%llu used=%llu reserved=%llu",
                    (unsigned long long)Stats.poolCount,
                    (unsigned long long)Stats.usedBytes,
                    (unsigned long long)Stats.reservedBytes);
      return;
    }
#endif
    EJIT_DIAG_RAW("print_code_pool_stats: not initialized");
    return;
  }
  gEJIT->printCodePoolStats();
}

void ejit_print_period_const_ranking(void) {
  if (!gEJIT) {
#ifdef EJIT_SRE_SHARED_TASKPOOL_ZERO_INIT
    if (auto *Pool = getEJitBootstrapPool()) {
      (void)Pool->requestPeriodConstRanking();
      return;
    }
#endif
    EJIT_DIAG_RAW("period_const-ranking: not initialized");
    return;
  }
  (void)gEJIT->printPeriodConstRanking();
}

void ejit_print_mayconst_ranking(void) { ejit_print_period_const_ranking(); }

void ejit_print_active(void) {
  if (!gEJIT) {
    EJIT_DIAG_RAW("print_active: not initialized");
    return;
  }
  gEJIT->printActive();
}

void ejit_print_version(void) {
  // LLVM release version (major.minor.patch) comes from
  // llvm/Config/llvm-config.h (LLVM_VERSION_STRING); the git commit + branch
  // come from the build-time generated EJitVersion.h. Printed unconditionally -
  // not through EJIT_DIAG and not gated on EJIT_DIAG_ENABLE or gEJitDiagLevel -
  // so the build identity is always recoverable, even on a build with
  // diagnostics compiled out and before ejit_init(). Routes through the same
  // platform sink as EJIT_DIAG: SRE_printf on SRE/bare-metal builds (declared
  // at file scope above / by EJitDiag.h), std::printf otherwise.
#ifdef EJIT_SRE_DIAG
  SRE_printf("[EJIT] LLVM version %s, branch %s, commit %s\n",
             LLVM_VERSION_STRING, EJIT_GIT_BRANCH, EJIT_GIT_COMMIT);
#else
  std::printf("[EJIT] LLVM version %s, branch %s, commit %s\n",
              LLVM_VERSION_STRING, EJIT_GIT_BRANCH, EJIT_GIT_COMMIT);
#endif
}

} // extern "C"
