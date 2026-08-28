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
#include "llvm/ExecutionEngine/EJIT/EJitSreQueue.h" // EJitDimPair layout
// Build-time-generated: EJIT_GIT_COMMIT / EJIT_GIT_BRANCH (git HEAD of the
// llvm-project source tree). Lives in the LLVMEJIT build directory.
#include "EJitVersion.h"
#ifdef EJIT_SRE_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitTaskPool.h"
#endif
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include "llvm/ExecutionEngine/EJIT/EJitSharedTaskPool.h"
#endif
#ifdef EJIT_SRE_PGO_VALUE_PROFILE
#include "llvm/ExecutionEngine/EJIT/EJitVpCollector.h"
#endif
#ifndef EJIT_FREESTANDING
#include <chrono>
#endif
// ejit_print_version() falls back to std::printf when not routing through the
// SRE platform sink. Mirrors the EJIT_DIAG <cstdio> include (non-SRE path).
#ifndef EJIT_SRE_DIAG
#include <cstdio>
#endif
#include <algorithm>
#include <cstddef>
#include <type_traits>
#include <vector>

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

// Wrapper-timing sentinel: must never collide with a real status. The value
// is structurally RESERVED in the ejit_status_t enum itself
// (EJIT_STATUS_ICACHE_HIT_SENTINEL = 0xFE) — a future real status assigned
// 0xFE is a duplicate-enumerator compile error — and the two constants are
// locked together here. The per-value comparisons below are a second,
// explicit lock against every status that exists today: a collision would
// corrupt timing aggregation in ejit_taskpool_trace_wrapper.
static_assert(kEJitIcacheHitTimingStatus ==
                  static_cast<uint32_t>(EJIT_STATUS_ICACHE_HIT_SENTINEL),
              "kEJitIcacheHitTimingStatus (EJitCommon.h) must equal the "
              "EJIT_STATUS_ICACHE_HIT_SENTINEL enumerator (EJitRuntime.h).");
static_assert(kEJitIcacheHitTimingStatus != static_cast<uint32_t>(EJIT_OK) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_PENDING) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_INVALID_PARAM) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_NOT_ACTIVE) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_COMPILE_FAILED) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_CACHE_FULL) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_MEMORY) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_BITCODE_NOT_FOUND) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_QUEUE_FULL) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_DEDUP_FULL) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_DISABLED) &&
                  kEJitIcacheHitTimingStatus !=
                      static_cast<uint32_t>(EJIT_ERR_INSTANCE_DISABLED),
              "kEJitIcacheHitTimingStatus (0xFE) collides with a real "
              "ejit_status_t value.");

static_assert(kEJitFunctionBodyPathAOT ==
                      static_cast<uint32_t>(EJIT_FUNCTION_BODY_AOT) &&
                  kEJitFunctionBodyPathJIT ==
                      static_cast<uint32_t>(EJIT_FUNCTION_BODY_JIT),
              "function-body timing path constants must match the C ABI");

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

#ifdef EJIT_SRE_SHARED_TASKPOOL
// Shared-taskpool capacity constants must mirror the single-instance ones:
// the shared cache's enabled[dimType][instanceId] tables index by these, and
// a smaller shared copy would go OOB on the shared blob for indices the
// single-instance side accepts.
static_assert(kEJitMaxDimTypes == kEJitSharedDimTypes,
              "kEJitMaxDimTypes (EJitCommon.h) must equal kEJitSharedDimTypes "
              "(EJitSharedTaskPoolState.h).");
static_assert(kEJitMaxInstances == kEJitSharedInstances,
              "kEJitMaxInstances (EJitCommon.h) must equal "
              "kEJitSharedInstances (EJitSharedTaskPoolState.h).");
static_assert(kEJitMaxFuncIndex == kEJitSharedMaxFuncIndex,
              "kEJitMaxFuncIndex (EJitCommon.h) must equal "
              "kEJitSharedMaxFuncIndex (EJitSharedTaskPoolState.h).");
#endif // EJIT_SRE_SHARED_TASKPOOL

static EJit *gEJIT = nullptr;

#ifdef EJIT_DIAG_ENABLE
EJitAtomicU64 gIcacheNullFillSkips;
constexpr uint64_t kIcacheNullFillLogEvery = 1000;
#endif

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
  bool tryLock() {
    uint32_t expected = 0;
    return flag_.compareExchange(expected, 1u);
  }
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

#ifndef EJIT_FUNCTION_BODY_TIMING_SLOTS
#define EJIT_FUNCTION_BODY_TIMING_SLOTS 128u
#endif
static_assert(EJIT_FUNCTION_BODY_TIMING_SLOTS > 0,
              "function-body timing table must contain at least one slot");

struct FunctionBodyTimingSlot {
  bool Valid = false;
  const char *Name = nullptr;
  uint32_t Path = 0;
  uint64_t Count = 0;
  uint64_t Total = 0;
  uint64_t Min = UINT64_MAX;
  uint64_t Max = 0;
  uint64_t WrapperTotal = 0;
  uint64_t WrapperMin = UINT64_MAX;
  uint64_t WrapperMax = 0;
  uint64_t OverheadTotal = 0;
  uint64_t OverheadMin = UINT64_MAX;
  uint64_t OverheadMax = 0;
};

static TimingSpinLock gFunctionBodyTimingLock;
static FunctionBodyTimingSlot
    gFunctionBodyTimingSlots[EJIT_FUNCTION_BODY_TIMING_SLOTS];
static EJitAtomicU64 gFunctionBodyTimingDropped;

static uint32_t functionBodyNameHash(const char *Name, uint32_t Path) {
  uint32_t H = 2166136261u ^ Path;
  for (const unsigned char *P = reinterpret_cast<const unsigned char *>(Name);
       *P; ++P)
    H = (H ^ *P) * 16777619u;
  return H;
}

static bool functionBodyNameEqual(const char *A, const char *B) {
  if (A == B)
    return true;
  if (!A || !B)
    return false;
  while (*A && *A == *B) {
    ++A;
    ++B;
  }
  return *A == *B;
}

static FunctionBodyTimingSlot *
findFunctionBodyTimingSlot(const char *Name, uint32_t Path, bool Create) {
  const uint32_t Capacity = EJIT_FUNCTION_BODY_TIMING_SLOTS;
  uint32_t Start = functionBodyNameHash(Name, Path) % Capacity;
  for (uint32_t Probe = 0; Probe < Capacity; ++Probe) {
    FunctionBodyTimingSlot &S =
        gFunctionBodyTimingSlots[(Start + Probe) % Capacity];
    if (!S.Valid) {
      if (!Create)
        return nullptr;
      S.Valid = true;
      S.Name = Name;
      S.Path = Path;
      return &S;
    }
    if (S.Path == Path && functionBodyNameEqual(S.Name, Name))
      return &S;
  }
  return nullptr;
}

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
  // NOTE: online PGO is NOT read from ejit_config_t. The public struct keeps
  // its original (unversioned) layout, so a caller compiled against an older
  // header never gets a tail field over-read. PGO is opted in through the
  // dedicated ejit_init_pgo() entry point instead (dst.enablePgo defaults to
  // false).
#ifdef EJIT_FREESTANDING
  dst.enableLogger = false;
#endif
}

// Compile-time regression guard: the public ejit_config_t layout MUST stay
// exactly as originally released (no new tail field), so ejit_init() never
// over-reads an old caller's smaller struct. Online PGO is exposed additively
// via ejit_init_pgo(), never by growing this struct. If a field is added,
// these asserts fire and force an explicit, versioned ABI decision.
static_assert(offsetof(ejit_config_t, compileMode) == 0,
              "ejit_config_t.compileMode must stay first (ABI)");
static_assert(sizeof(ejit_config_t) ==
                  offsetof(ejit_config_t, dumpJITDir) + sizeof(const char *),
              "ejit_config_t must end at dumpJITDir (no online-PGO tail field "
              "may be appended; use ejit_init_pgo instead)");
static_assert(std::is_standard_layout<ejit_config_t>::value,
              "ejit_config_t must be standard-layout for a stable C ABI");

// Shared init implementation for ejit_init / ejit_init_pgo. \p forcePgo forces
// the online-PGO auto-trigger on regardless of the (unversioned) config.
static ejit_status_t ejitInitImpl(const ejit_config_t *config, bool forcePgo) {
  if (gEJIT) {
    EJIT_DIAG("already initialized, returning OK");
    return EJIT_OK;
  }

  Config cfg;
  parseConfig(config, cfg);
#if defined(EJIT_SRE_SHARED_TASKPOOL) &&                                  \
    defined(EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE)
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

#ifdef EJIT_SRE_SHARED_TASKPOOL
  bindDumpSharedStateFromRuntime();
#endif
  EJIT_DIAG("initialized: mode=%d opt=%d cache=%zu entries=%u pgo=%d "
            "static_registry=%d",
            (int)cfg.compileMode, (int)cfg.optLevel, cfg.maxCacheSize,
            (unsigned)cfg.maxCacheEntries, (int)cfg.enablePgo,
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
    EJIT_DIAG("activate(%s,%u) failed: not initialized", periodName, cellIdx);
    return EJIT_ERR_NOT_ACTIVE;
  }
  // The AOT call site passes the instance index as an i32; reject anything the
  // runtime tables cannot hold instead of silently activating the wrong
  // instance (the pre-fix uint8_t signature truncated at the call boundary).
  if (cellIdx >= kEJitMaxInstances) {
    EJIT_DIAG("activate(%s,%u) failed: instance index >= kEJitMaxInstances=%u",
              periodName, cellIdx, kEJitMaxInstances);
    return EJIT_ERR_INVALID_PARAM;
  }
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
    EJIT_DIAG("deactivate(%s,%u) failed: not initialized", periodName, cellIdx);
    return EJIT_ERR_NOT_ACTIVE;
  }
  if (cellIdx >= kEJitMaxInstances) {
    EJIT_DIAG(
        "deactivate(%s,%u) failed: instance index >= kEJitMaxInstances=%u",
        periodName, cellIdx, kEJitMaxInstances);
    return EJIT_ERR_INVALID_PARAM;
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

bool ejit_is_active(const char *periodName, uint32_t cellIdx) {
  if (!gEJIT) {
    EJIT_DIAG("is_active(%s,%u) failed: not initialized", periodName, cellIdx);
    return false;
  }
  // Reject out-of-range indices instead of truncating at the ABI boundary and
  // querying the wrong instance (pre-fix uint8_t signature).
  if (cellIdx >= kEJitMaxInstances) {
    EJIT_DIAG(
        "is_active(%s,%u) rejected: instance index >= kEJitMaxInstances=%u",
        periodName, cellIdx, kEJitMaxInstances);
    return false;
  }
  return gEJIT->isActive(periodName, cellIdx);
}

void *ejit_compile_or_get(uint64_t cacheKey, void **out_pfn) {
  if (out_pfn)
    *out_pfn = nullptr;
  EJIT_DIAG("compile_or_get(key=0x%016lx): retired, use taskpool API",
            cacheKey);
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

void ejit_invalidate(const char *periodName, uint32_t cellIdx) {
  EJIT_DIAG("invalidate(%s,%u)", periodName, cellIdx);
  if (!gEJIT)
    return;
  // Reject out-of-range indices instead of truncating at the ABI boundary and
  // invalidating the wrong instance (pre-fix uint8_t signature).
  if (cellIdx >= kEJitMaxInstances) {
    EJIT_DIAG(
        "invalidate(%s,%u) rejected: instance index >= kEJitMaxInstances=%u",
        periodName, cellIdx, kEJitMaxInstances);
    return;
  }
  gEJIT->invalidateByPeriod(periodName, cellIdx);
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (auto *tp = gEJIT->sharedTaskPool())
    tp->retireDispatchCache();
#endif
}

ejit_status_t ejit_get_stats(ejit_stats_t *stats) {
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  if (!stats)
    return EJIT_ERR_INVALID_PARAM;
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

// Open a resolve window and return the token icacheFill needs to prove no drain
// overlapped it. Runs at the taskpool entry point, BEFORE any bucket read
// token. The token lives in the caller's LOCAL, so a higher-priority task
// preempting this core mid-resolve cannot clobber it. 0 without the shared
// pool, so call sites need no #ifdef guards.
inline uint64_t ejitIcacheBeginResolve() {
#ifdef EJIT_SRE_SHARED_TASKPOOL
  if (EJitSharedTaskPool *sp = gEJIT ? gEJIT->sharedTaskPool() : nullptr)
    return sp->icacheBeginResolve();
#endif
  return 0;
}

// Fill the icache cell on a successful taskpool resolve (cache hit or fresh
// compile), so a cold icache is filled on the first taskpool hit, not only on a
// fresh compile. dims selects the [D]^numDims cell. No-op without the shared
// pool or a null fnPtr. Always defined (a no-op without the shared taskpool) so
// call sites need no #ifdef guards.
inline void ejitIcacheFillOnSuccess(uint32_t funcIndex, void *fnPtr,
                                    const EJitDimPair *dims, uint32_t numDims,
                                    uint64_t token) {
#ifdef EJIT_SRE_SHARED_TASKPOOL
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
  EJitSharedTaskPool *sp = gEJIT ? gEJIT->sharedTaskPool() : nullptr;
  if (!sp) {
    // Unlike the above this is a real misconfiguration, but it would repeat on
    // every call just the same, so report it once.
#ifdef EJIT_DIAG_ENABLE
    static bool NoPoolLogged = false;
    if (!NoPoolLogged) {
      NoPoolLogged = true;
      EJIT_DIAG("icacheFillOnSuccess DECLINE func=%u: no shared pool "
                "(gEJIT=%p) -- the inline cache cannot be filled at all",
                funcIndex, (void *)gEJIT);
    }
#endif
    return;
  }
  sp->icacheFill(funcIndex, fnPtr, dims, numDims, token);
#else
  (void)funcIndex;
  (void)fnPtr;
  (void)dims;
  (void)numDims;
  (void)token;
#endif
}
} // namespace

static ejit_status_t
taskpoolCompileOrGetImpl(uint32_t funcIndex, const ejit_dim_pair_t *dims,
                         uint32_t numDims, const void *snapshotData,
                         uint32_t snapshotSize, uint32_t boundArgIndex,
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
  if (!gEJIT)
    return EJIT_ERR_NOT_ACTIVE;
  auto *tp = activeTaskPool();
  if (!tp) {
    EJIT_DIAG("taskpool_compile_or_get reject func=%u: no taskpool", funcIndex);
    return EJIT_ERR_NOT_ACTIVE;
  }
  const uint64_t icTok = ejitIcacheBeginResolve();

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
      EJIT_DIAG(
          "taskpool_compile_or_get reject func=%u: dim[%u] dimType=%u OOR",
          funcIndex, i, dims[i].dimType);
      return EJIT_ERR_INVALID_PARAM;
    }
    if (dims[i].instanceId >= EJitSwitchController::MAX_INSTANCES) {
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
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dimsCast, numDims, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
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
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dimsCast, numDims, icTok);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dimsCast, numDims);
    return taskpoolStatus(fast.status);
  }

  auto r = tp->compileOrGet(funcIndex, dimsCast, numDims,
                            /*fallback=*/nullptr, snapshotData, snapshotSize,
                            boundArgIndex);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  EJIT_DIAG_VERBOSE("taskpool_compile_or_get func=%u status=%u fn=%p",
                    funcIndex, static_cast<unsigned>(r.status), r.fnPtr);
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dimsCast, numDims, icTok);
  if (r.fnPtr)
    tp->l0Fill(funcIndex, r.fnPtr, dimsCast, numDims);
  return taskpoolStatus(r.status);
}

ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims,
                                           uint32_t numDims, void **outFn,
                                           uint32_t *outBucket) {
  return taskpoolCompileOrGetImpl(funcIndex, dims, numDims, nullptr, 0, 0,
                                  outFn, outBucket);
}

ejit_status_t ejit_taskpool_compile_or_get_bound(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const void *snapshotData, uint32_t snapshotSize, uint32_t boundArgIndex,
    void **outFn, uint32_t *outBucket) {
  return taskpoolCompileOrGetImpl(funcIndex, dims, numDims, snapshotData,
                                  snapshotSize, boundArgIndex, outFn,
                                  outBucket);
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
  const uint64_t icTok = ejitIcacheBeginResolve();

  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, nullptr, 0, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit0D(funcIndex);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, nullptr, 0, icTok);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, nullptr, 0, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, nullptr, 0, icTok);
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
  const uint64_t icTok = ejitIcacheBeginResolve();
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
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 1, icTok);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 1);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 1, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 1, icTok);
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
  const uint64_t icTok = ejitIcacheBeginResolve();
  if (!ejitTaskpoolDimInRange(dim0, inst0) ||
      !ejitTaskpoolDimInRange(dim1, inst1))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[2] = {{dim0, inst0}, {dim1, inst1}};
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 2, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit2D(funcIndex, dim0, inst0, dim1, inst1);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 2, icTok);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 2);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 2, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 2, icTok);
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
  const uint64_t icTok = ejitIcacheBeginResolve();
  if (!ejitTaskpoolDimInRange(dim0, inst0) ||
      !ejitTaskpoolDimInRange(dim1, inst1) ||
      !ejitTaskpoolDimInRange(dim2, inst2))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[3] = {{dim0, inst0}, {dim1, inst1}, {dim2, inst2}};
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 3, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast =
      tp->tryCacheHit3D(funcIndex, dim0, inst0, dim1, inst1, dim2, inst2);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 3, icTok);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 3);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 3, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 3, icTok);
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
  const uint64_t icTok = ejitIcacheBeginResolve();
  if (!ejitTaskpoolDimInRange(dim0, inst0) ||
      !ejitTaskpoolDimInRange(dim1, inst1) ||
      !ejitTaskpoolDimInRange(dim2, inst2) ||
      !ejitTaskpoolDimInRange(dim3, inst3))
    return EJIT_ERR_INVALID_PARAM;

  const EJitDimPair dims[4] = {
      {dim0, inst0}, {dim1, inst1}, {dim2, inst2}, {dim3, inst3}};
  void *l0Fn = nullptr;
  if (tp->l0Try(funcIndex, dims, 4, &l0Fn)) {
    if (outFn)
      *outFn = l0Fn;
    if (outBucket)
      *outBucket = kEJitNoBucket;
    return EJIT_OK;
  }
  auto fast = tp->tryCacheHit4D(funcIndex, dim0, inst0, dim1, inst1, dim2,
                                inst2, dim3, inst3);
  if (fast.fastPathTerminal) {
    if (outFn)
      *outFn = fast.fnPtr;
    if (outBucket)
      *outBucket = fast.bucketIndex;
    ejitIcacheFillOnSuccess(funcIndex, fast.fnPtr, dims, 4, icTok);
    if (fast.fnPtr)
      tp->l0Fill(funcIndex, fast.fnPtr, dims, 4);
    return taskpoolStatus(fast.status);
  }
  auto r = tp->compileOrGet(funcIndex, dims, 4, /*fallback=*/nullptr);
  if (outFn)
    *outFn = r.fnPtr;
  if (outBucket)
    *outBucket = r.bucketIndex;
  ejitIcacheFillOnSuccess(funcIndex, r.fnPtr, dims, 4, icTok);
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
    EJIT_DIAG("taskpool_release_read bucket=%u reject: no taskpool",
              bucketIndex);
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

ejit_status_t ejit_publish_pending_code(void) {
#if defined(EJIT_SRE_SHARED_TASKPOOL) && defined(EJIT_CODE_POOL_BATCHED_PUBLISH)
  if (!gEJIT) {
    EJIT_DIAG("publish_pending_code failed: not initialized");
    return EJIT_ERR_DISABLED;
  }
  EJitSharedTaskPool *sp = gEJIT->sharedTaskPool();
  if (!sp) {
    EJIT_DIAG("publish_pending_code failed: no shared taskpool");
    return EJIT_ERR_DISABLED;
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
                                 uint64_t tBeforeLookup, uint64_t tAfterLookup,
                                 uint64_t tAfterFn, uint64_t tAfterRelease) {
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

void ejit_function_body_cycles_record(const char *funcName, uint32_t path,
                                      uint64_t wrapperBegin, uint64_t bodyBegin,
                                      uint64_t bodyEnd, uint64_t wrapperEnd) {
  if (!funcName || !*funcName || path > kEJitFunctionBodyPathJIT)
    return;

  const uint64_t BodyCycles = bodyEnd - bodyBegin;
  const uint64_t WrapperCycles = wrapperEnd - wrapperBegin;
  // Use the two non-body intervals instead of subtracting aggregate values so
  // counter wraparound remains well-defined for each individual sample.
  const uint64_t OverheadCycles =
      (bodyBegin - wrapperBegin) + (wrapperEnd - bodyEnd);
  // Never spin in an instrumented function's return path. A nested/preempting
  // sample on the same core is diagnostic-only and is safer to drop than to
  // wait for a recorder that cannot run until the nested call returns.
  if (!gFunctionBodyTimingLock.tryLock()) {
    gFunctionBodyTimingDropped.fetchAdd(1);
    return;
  }
  FunctionBodyTimingSlot *S =
      findFunctionBodyTimingSlot(funcName, path, /*Create=*/true);
  if (!S) {
    gFunctionBodyTimingDropped.fetchAdd(1);
    gFunctionBodyTimingLock.unlock();
    return;
  }
  ++S->Count;
  S->Total += BodyCycles;
  if (BodyCycles < S->Min)
    S->Min = BodyCycles;
  if (BodyCycles > S->Max)
    S->Max = BodyCycles;
  S->WrapperTotal += WrapperCycles;
  if (WrapperCycles < S->WrapperMin)
    S->WrapperMin = WrapperCycles;
  if (WrapperCycles > S->WrapperMax)
    S->WrapperMax = WrapperCycles;
  S->OverheadTotal += OverheadCycles;
  if (OverheadCycles < S->OverheadMin)
    S->OverheadMin = OverheadCycles;
  if (OverheadCycles > S->OverheadMax)
    S->OverheadMax = OverheadCycles;
  gFunctionBodyTimingLock.unlock();
}

unsigned ejit_function_body_cycles_get(const char *funcName, uint32_t path,
                                       ejit_function_body_cycles_t *out) {
  if (!funcName || !*funcName || !out || path > kEJitFunctionBodyPathJIT)
    return 0;

  gFunctionBodyTimingLock.lock();
  FunctionBodyTimingSlot *S =
      findFunctionBodyTimingSlot(funcName, path, /*Create=*/false);
  if (!S || S->Count == 0) {
    gFunctionBodyTimingLock.unlock();
    return 0;
  }
  out->count = S->Count;
  out->total = S->Total;
  out->min = S->Min;
  out->max = S->Max;
  out->wrapper_total = S->WrapperTotal;
  out->wrapper_min = S->WrapperMin;
  out->wrapper_max = S->WrapperMax;
  out->overhead_total = S->OverheadTotal;
  out->overhead_min = S->OverheadMin;
  out->overhead_max = S->OverheadMax;
  gFunctionBodyTimingLock.unlock();
  return 1;
}

void ejit_function_body_cycles_print(void) {
  gFunctionBodyTimingLock.lock();
  EJIT_DIAG_RAW("function_body_cycles: unit=%s slots=%u dropped=%llu",
#ifdef EJIT_FREESTANDING
                "cycles",
#else
                "host-ns",
#endif
                static_cast<unsigned>(EJIT_FUNCTION_BODY_TIMING_SLOTS),
                static_cast<unsigned long long>(
                    gFunctionBodyTimingDropped.loadRelaxed()));
  for (const FunctionBodyTimingSlot &S : gFunctionBodyTimingSlots) {
    if (!S.Valid || S.Count == 0)
      continue;
    EJIT_DIAG_RAW("  func=%s path=%s count=%llu body_avg=%llu body_min=%llu "
                  "body_max=%llu body_total=%llu wrapper_avg=%llu "
                  "wrapper_min=%llu wrapper_max=%llu wrapper_total=%llu "
                  "overhead_avg=%llu overhead_min=%llu overhead_max=%llu "
                  "overhead_total=%llu",
                  S.Name, S.Path == kEJitFunctionBodyPathAOT ? "AOT" : "JIT",
                  static_cast<unsigned long long>(S.Count),
                  static_cast<unsigned long long>(S.Total / S.Count),
                  static_cast<unsigned long long>(S.Min),
                  static_cast<unsigned long long>(S.Max),
                  static_cast<unsigned long long>(S.Total),
                  static_cast<unsigned long long>(S.WrapperTotal / S.Count),
                  static_cast<unsigned long long>(S.WrapperMin),
                  static_cast<unsigned long long>(S.WrapperMax),
                  static_cast<unsigned long long>(S.WrapperTotal),
                  static_cast<unsigned long long>(S.OverheadTotal / S.Count),
                  static_cast<unsigned long long>(S.OverheadMin),
                  static_cast<unsigned long long>(S.OverheadMax),
                  static_cast<unsigned long long>(S.OverheadTotal));
    ejitDiagPrintThrottle();
  }
  gFunctionBodyTimingLock.unlock();
}

void ejit_function_body_cycles_reset(void) {
  gFunctionBodyTimingLock.lock();
  for (FunctionBodyTimingSlot &S : gFunctionBodyTimingSlots)
    S = FunctionBodyTimingSlot{};
  gFunctionBodyTimingDropped.storeRelaxed(0);
  gFunctionBodyTimingLock.unlock();
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

#ifdef EJIT_SRE_SHARED_TASKPOOL
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
#else
  EJIT_DIAG_RAW("  icacheSlots       = (n/a: shared taskpool not built)");
#endif
}

void ejit_taskpool_print_compiled() {
#ifndef EJIT_DIAG_ENABLE
  // Diagnostics compiled out: nothing can print, so do not walk the cache
  // (the walk takes bucket read locks for no output).
  return;
#else
  if (!gEJIT) {
    EJIT_DIAG_RAW("print_compiled: not initialized");
    return;
  }
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Every entry line is INFO-gated; below INFO the walk would hold bucket
  // read locks while printing nothing.
  if (gEJitDiagLevel < EJIT_LOG_LVL_INFO)
    return;
  EJitSharedTaskPool *sp = gEJIT->sharedTaskPool();
  if (!sp || !sp->state()) {
    EJIT_DIAG_RAW("print_compiled: no shared taskpool / state");
    return;
  }
  EJitModuleLoader &loader = gEJIT->moduleLoader();
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
        // Valid numDims is 0..kEJitSharedMaxDims; clamp a corrupt slot's
        // value so the tally cannot index past byDims.
        uint32_t n = slot.numDims < kEJitSharedMaxDims ? slot.numDims
                                                       : kEJitSharedMaxDims;
        ++static_cast<CountCtx *>(cbCtx)->byDims[n];
        uint8_t tier = slot.tier.loadRelaxed();
        if (tier <= kEJitTierPgoUse)
          ++static_cast<CountCtx *>(cbCtx)->byTier[tier];
        if (slot.poolKind <= static_cast<uint32_t>(EJitCodePoolKind::Far))
          ++static_cast<CountCtx *>(cbCtx)->byPool[slot.poolKind];
        if (tier != kEJitTierInstrumented &&
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
  const uint32_t FinalVersions =
      count.byTier[kEJitTierBaseline] + count.byTier[kEJitTierPgoUse];
  EJIT_DIAG_RAW("compiled final reuse: post_publish_seen=%u unseen=%u "
                "(tier1_collecting_excluded=%u)",
                count.finalPostPublishSeen,
                FinalVersions >= count.finalPostPublishSeen
                    ? FinalVersions - count.finalPostPublishSeen
                    : 0,
                count.byTier[kEJitTierInstrumented]);
#else
  constexpr bool ReuseTrackingEnabled = false;
  EJIT_DIAG_RAW("compiled reuse: disabled (build with EJIT_STATS_ENABLE)");
#endif
  // Entry lines: one RAW (prefix-free) line per Ready slot, throttled after
  // each printed line.
  struct LayoutEntry {
    uint32_t funcIndex;
    uint32_t numDims;
    EJitDimPair dims[kEJitSharedMaxDims];
    uint32_t versions[kEJitSharedMaxDims];
    uint8_t tier;
    uintptr_t fn;
    uintptr_t codeStart;
    uint64_t codeSize;
    uint32_t poolKind;
    uint32_t poolId;
    uint32_t generation;
    uint8_t postPublishSeen;
  };
  struct PrintCtx {
    std::vector<LayoutEntry> layout;
  } printCtx{{}};
  EJitSharedTaskPool::ForEachCompiledStats st2 = sp->forEachCompiled(
      [](const EJitSharedCacheSlot &slot, void *cbCtx) {
        PrintCtx &ctx = *static_cast<PrintCtx *>(cbCtx);
        // Per the publish protocol: fnPtr is read with acquire only after
        // state==Ready was observed with acquire (forEachCompiled did).
        void *fn = reinterpret_cast<void *>(slot.fnPtr.loadAcquire());
        // Only the numDims leading pairs are meaningful; clamp a corrupt
        // slot's numDims to the ABI maximum so the builders cannot overflow.
        const uint32_t n = slot.numDims < kEJitSharedMaxDims
                               ? slot.numDims
                               : kEJitSharedMaxDims;
        LayoutEntry Entry{};
        Entry.funcIndex = slot.funcIndex;
        Entry.numDims = n;
        for (uint32_t i = 0; i < n; ++i) {
          Entry.dims[i] = slot.dims[i];
          Entry.versions[i] = slot.versions[i];
        }
        Entry.tier = slot.tier.loadRelaxed();
        Entry.fn = reinterpret_cast<uintptr_t>(fn);
        Entry.codeStart = slot.codeStart;
        Entry.codeSize = slot.codeSize;
        Entry.poolKind = slot.poolKind;
        Entry.poolId = slot.poolId;
        Entry.generation = slot.generation;
        Entry.postPublishSeen = slot.postPublishSeen.loadRelaxed();
        ctx.layout.push_back(Entry);
      },
      &printCtx);
  // The summary counted the first walk; if the entry walk itself skipped
  // contended buckets, its lines are incomplete relative to it — report the
  // shortfall (fixed line count, so no throttle) instead of dropping it.
  if (st2.skippedBuckets)
    EJIT_DIAG_RAW("compiled: %u buckets skipped during entry walk",
                  st2.skippedBuckets);
  std::sort(printCtx.layout.begin(), printCtx.layout.end(),
            [](const LayoutEntry &A, const LayoutEntry &B) {
              // Ready slots should always have a non-null fnPtr. Keep a corrupt
              // zero pointer last instead of presenting it as the lowest code
              // address.
              if ((A.fn == 0) != (B.fn == 0))
                return A.fn != 0;
              if (A.fn != B.fn)
                return A.fn < B.fn;
              if (A.codeStart != B.codeStart)
                return A.codeStart < B.codeStart;
              return A.funcIndex < B.funcIndex;
            });
  EJIT_DIAG_RAW("compiled layout: %zu entries sorted by fn address",
                printCtx.layout.size());
  {
    uintptr_t PreviousStart = 0;
    uintptr_t PreviousEnd = 0;
    uint32_t PreviousPoolKind = ~0u;
    uint32_t PreviousPoolId = ~0u;
    bool PreviousRangeValid = false;
    for (size_t Index = 0; Index < printCtx.layout.size(); ++Index) {
      const LayoutEntry &Entry = printCtx.layout[Index];
      const std::string &Name = loader.getFuncNameByFuncIdx(Entry.funcIndex);
      char Dims[kEJitSharedMaxDims * 21 + (kEJitSharedMaxDims - 1) + 1];
      char *DimPos = Dims;
      char *DimEnd = Dims + sizeof(Dims);
      for (uint32_t I = 0; I < Entry.numDims && DimPos < DimEnd; ++I)
        DimPos += snprintf(DimPos, static_cast<size_t>(DimEnd - DimPos),
                           "%s%u:%u", I ? "," : "", Entry.dims[I].dimType,
                           Entry.dims[I].instanceId);
      if (DimPos >= DimEnd)
        DimPos = DimEnd - 1;
      *DimPos = '\0';
      char Versions[kEJitSharedMaxDims * 10 + (kEJitSharedMaxDims - 1) + 1];
      char *VersionPos = Versions;
      char *VersionEnd = Versions + sizeof(Versions);
      for (uint32_t I = 0; I < Entry.numDims && VersionPos < VersionEnd; ++I)
        VersionPos +=
            snprintf(VersionPos, static_cast<size_t>(VersionEnd - VersionPos),
                     "%s%u", I ? "," : "", Entry.versions[I]);
      if (VersionPos >= VersionEnd)
        VersionPos = VersionEnd - 1;
      *VersionPos = '\0';
      const char *Tier = Entry.tier == kEJitTierPgoUse ? "tier2"
                         : Entry.tier == kEJitTierInstrumented
                             ? "tier1-collecting"
                             : "baseline";
      const char *PoolKind =
          Entry.poolKind == static_cast<uint32_t>(EJitCodePoolKind::Near)
              ? "near"
          : Entry.poolKind == static_cast<uint32_t>(EJitCodePoolKind::Far)
              ? "far"
              : "unknown";
      const uintptr_t CodeEnd =
          Entry.codeStart + static_cast<uintptr_t>(Entry.codeSize);
      const bool RangeValid = Entry.codeStart != 0 && Entry.codeSize != 0 &&
                              CodeEnd >= Entry.codeStart;
      const bool SamePool = PreviousRangeValid &&
                            PreviousPoolKind == Entry.poolKind &&
                            PreviousPoolId == Entry.poolId;
      const bool SameAllocation = SamePool && RangeValid &&
                                  Entry.codeStart == PreviousStart &&
                                  CodeEnd == PreviousEnd;
      const char *FnInAllocation =
          !RangeValid
              ? "unknown"
              : (Entry.fn >= Entry.codeStart && Entry.fn < CodeEnd ? "yes"
                                                                   : "no");
      const char *PostPublishSeen =
          !ReuseTrackingEnabled ? "disabled"
                                : (Entry.postPublishSeen != 0 ? "yes" : "no");
      if (gEJitDiagLevel < EJIT_LOG_LVL_VERBOSE) {
        EJIT_DIAG_RAW(
            "layout[%zu] fn=0x%llx alloc_start=0x%llx alloc_size=%llu "
            "pool=%s:%u funcIdx=%u name=%s tier=%s dims=[%s] "
            "fn_in_alloc=%s post_publish_seen=%s",
            Index, static_cast<unsigned long long>(Entry.fn),
            static_cast<unsigned long long>(Entry.codeStart),
            static_cast<unsigned long long>(Entry.codeSize), PoolKind,
            Entry.poolId, Entry.funcIndex,
            Name.empty() ? "<unknown>" : Name.c_str(), Tier, Dims,
            FnInAllocation, PostPublishSeen);
      } else if (!SamePool || !RangeValid) {
        EJIT_DIAG_RAW(
            "layout[%zu] fn=0x%llx alloc_start=0x%llx alloc_end=0x%llx "
            "alloc_size=%llu gap=n/a pool=%s:%u funcIdx=%u name=%s tier=%s "
            "dims=[%s] ver=[%s] gen=%u fn_in_alloc=%s "
            "post_publish_seen=%s",
            Index, static_cast<unsigned long long>(Entry.fn),
            static_cast<unsigned long long>(Entry.codeStart),
            static_cast<unsigned long long>(CodeEnd),
            static_cast<unsigned long long>(Entry.codeSize), PoolKind,
            Entry.poolId, Entry.funcIndex,
            Name.empty() ? "<unknown>" : Name.c_str(), Tier, Dims, Versions,
            Entry.generation, FnInAllocation, PostPublishSeen);
      } else if (SameAllocation) {
        EJIT_DIAG_RAW(
            "layout[%zu] fn=0x%llx alloc_start=0x%llx alloc_end=0x%llx "
            "alloc_size=%llu gap=shared_alloc pool=%s:%u funcIdx=%u name=%s "
            "tier=%s dims=[%s] ver=[%s] gen=%u fn_in_alloc=%s "
            "post_publish_seen=%s",
            Index, static_cast<unsigned long long>(Entry.fn),
            static_cast<unsigned long long>(Entry.codeStart),
            static_cast<unsigned long long>(CodeEnd),
            static_cast<unsigned long long>(Entry.codeSize), PoolKind,
            Entry.poolId, Entry.funcIndex,
            Name.empty() ? "<unknown>" : Name.c_str(), Tier, Dims, Versions,
            Entry.generation, FnInAllocation, PostPublishSeen);
      } else if (Entry.codeStart >= PreviousEnd) {
        EJIT_DIAG_RAW(
            "layout[%zu] fn=0x%llx alloc_start=0x%llx alloc_end=0x%llx "
            "alloc_size=%llu gap=%llu pool=%s:%u funcIdx=%u name=%s tier=%s "
            "dims=[%s] ver=[%s] gen=%u fn_in_alloc=%s "
            "post_publish_seen=%s",
            Index, static_cast<unsigned long long>(Entry.fn),
            static_cast<unsigned long long>(Entry.codeStart),
            static_cast<unsigned long long>(CodeEnd),
            static_cast<unsigned long long>(Entry.codeSize),
            static_cast<unsigned long long>(Entry.codeStart - PreviousEnd),
            PoolKind, Entry.poolId, Entry.funcIndex,
            Name.empty() ? "<unknown>" : Name.c_str(), Tier, Dims, Versions,
            Entry.generation, FnInAllocation, PostPublishSeen);
      } else {
        EJIT_DIAG_RAW(
            "layout[%zu] fn=0x%llx alloc_start=0x%llx alloc_end=0x%llx "
            "alloc_size=%llu OVERLAP=%llu pool=%s:%u funcIdx=%u name=%s "
            "tier=%s dims=[%s] ver=[%s] gen=%u fn_in_alloc=%s "
            "post_publish_seen=%s",
            Index, static_cast<unsigned long long>(Entry.fn),
            static_cast<unsigned long long>(Entry.codeStart),
            static_cast<unsigned long long>(CodeEnd),
            static_cast<unsigned long long>(Entry.codeSize),
            static_cast<unsigned long long>(PreviousEnd - Entry.codeStart),
            PoolKind, Entry.poolId, Entry.funcIndex,
            Name.empty() ? "<unknown>" : Name.c_str(), Tier, Dims, Versions,
            Entry.generation, FnInAllocation, PostPublishSeen);
      }
      PreviousStart = Entry.codeStart;
      PreviousEnd = CodeEnd;
      PreviousPoolKind = Entry.poolKind;
      PreviousPoolId = Entry.poolId;
      PreviousRangeValid = RangeValid;
      ejitDiagPrintThrottle();
    }
  }
#else
  EJIT_DIAG_RAW("print_compiled: shared taskpool not enabled");
#endif
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
  EJIT_DIAG_RAW("print_dumped name=%s", (name && name[0]) ? name : "(all)");
  (void)printDumped(name);
}

void ejit_print_dumped_module(const char *name) {
  EJIT_DIAG_RAW("print_dumped_module name=%s",
                (name && name[0]) ? name : "(all)");
  (void)printDumpedModule(name);
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
    EJIT_DIAG("get_code_pool_stats: not initialized");
    return EJIT_ERR_NOT_ACTIVE;
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
    EJIT_DIAG("get_code_pool_stats_v2: not initialized");
    return EJIT_ERR_NOT_ACTIVE;
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
    EJIT_DIAG_RAW("print_code_pool_stats: not initialized");
    return;
  }
  gEJIT->printCodePoolStats();
}

void ejit_print_mayconst_ranking(void) {
  if (!gEJIT) {
    EJIT_DIAG_RAW("mayconst-ranking: not initialized");
    return;
  }
  (void)gEJIT->printMayConstRanking();
}

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
