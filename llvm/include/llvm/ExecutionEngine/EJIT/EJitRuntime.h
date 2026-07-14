//===-- EJitRuntime.h - EmbeddedJIT C Runtime API -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIME_H
#define LLVM_EXECUTIONENGINE_EJIT_EJITRUNTIME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

//===----------------------------------------------------------------------===//
// EmbeddedJIT attribute convenience macros.
//
// Define EJIT_DISABLE before including this header to compile out all
// EmbeddedJIT annotations (the code builds and runs without JIT
// specialization). Useful for A/B testing, porting to non-clang
// compilers, or debugging.
//
// Example:
//   typedef struct {
//     int ejit_may_const threshold;
//   } Config;
//   ejit_period(static) Config g_config;
//   ejit_entry void process(ejit_period_arr_ind(cell) uint8_t idx) { ... }
//===----------------------------------------------------------------------===//

#ifdef EJIT_DISABLE
#define EJIT_PERIOD_CONST
#define ejit_may_const
#define EJIT_IN_PERIOD(x)
#define ejit_period(x)
#define EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr(x)
#define EJIT_DIM(x)
#define ejit_period_arr_ind(x)
#define EJIT_ENTRY
#define ejit_entry
#define EJIT_PERIOD_GUARD(x)
#define ejit_period_lc(x)
#else
// New names (preferred)
#define EJIT_PERIOD_CONST       __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD(x)       __attribute__((ejit_in_period(#x)))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x)             __attribute__((ejit_dim(#x)))
#define EJIT_ENTRY              __attribute__((ejit_entry))
#define EJIT_PERIOD_GUARD(x)    __attribute__((ejit_period_guard(#x)))
// Old names (aliases — use new macros to avoid double expansion)
#define ejit_may_const          EJIT_PERIOD_CONST
#define ejit_period(x)          EJIT_IN_PERIOD(x)
#define ejit_period_arr(x)      EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr_ind(x)  EJIT_DIM(x)
#define ejit_entry              EJIT_ENTRY
#define ejit_period_lc(x)       EJIT_PERIOD_GUARD(x)
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  EJIT_OK = 0,
  EJIT_ERR_INVALID_PARAM = -1,
  EJIT_ERR_NOT_ACTIVE = -2,
  EJIT_ERR_COMPILE_FAILED = -3,
  EJIT_ERR_CACHE_FULL = -4,
  EJIT_ERR_MEMORY = -5,
  EJIT_ERR_BITCODE_NOT_FOUND = -6,
  /* SRE taskpool statuses (additive; original values above are unchanged). */
  EJIT_ERR_QUEUE_FULL = -7,
  EJIT_ERR_DEDUP_FULL = -8,
  EJIT_ERR_DISABLED = -9,
  EJIT_ERR_INSTANCE_DISABLED = -10,
  EJIT_PENDING = 1,
} ejit_status_t;

typedef enum {
  EJIT_COMPILE_SYNC = 0,
  EJIT_COMPILE_ASYNC = 1,
} ejit_compile_mode_t;

typedef enum {
  EJIT_OPT_L1 = 1,
  EJIT_OPT_L2 = 2,
  EJIT_OPT_L3 = 3,
} ejit_opt_level_t;

typedef struct {
  ejit_compile_mode_t compileMode;
  ejit_opt_level_t optLevel;
  size_t maxCodeMemory;
  size_t maxDataMemory;
  size_t maxCacheEntries;
  size_t maxCacheSize;
  bool enableLogger;
  /// If true, force the static registry table path (skip constructors).
  bool forceStaticRegistry;
  /// If non-NULL, dump JIT-optimized LLVM IR (.ll) to this directory.
  const char *dumpJITDir;
  /// Enable online PGO (Tier-1 instrumentation + lazy Tier-2 PGOUse recompile).
  /// Off by default.  Requires the runtime to be linked with LLVMProfileData +
  /// LLVMInstrumentation (lipo includes them when this flag is anticipated).
  bool enablePgo;
} ejit_config_t;

typedef struct {
  size_t entryCount;
  size_t totalCodeSize;
  size_t maxSize;
  uint64_t hits;
  uint64_t misses;
  uint64_t evictions;
} ejit_stats_t;

/// Code pool usage statistics (EJIT_SRE_CODE_POOL builds). All fixed-width so
/// the layout is stable across the aarch64_be target and a host reader.
/// Mirrors EJitCodePoolManager::Stats.
typedef struct ejit_code_pool_stats_t {
  uint64_t poolCount;          ///< total 2MiB pools created
  uint64_t sealedCount;        ///< pools currently sealed (RX)
  uint64_t activeCount;        ///< pools still RW
  uint64_t usedBytes;          ///< sum of bump offsets across all pools
  uint64_t reservedBytes;      ///< sum of pool sizes across all pools
  uint64_t wastedBytes;        ///< unused tail bytes inside sealed pools
  uint64_t sealInvocations;    ///< successful enable_ex calls (per 4K page in 4K mode)
  uint64_t splitInvocations;   ///< successful split_2m_to_4k calls (4K mode)
  uint64_t finalizedRangeCount; ///< distinct executable ranges recorded
} ejit_code_pool_stats_t;

typedef struct {
  int code;
  char message[256];
  char funcName[128];
} ejit_error_t;

// Initialization
ejit_status_t ejit_init(const ejit_config_t *config);
void ejit_shutdown(void);

// Symbol registration for bare-metal (no dlsym)
void ejit_register_symbol(const char *name, void *addr);

// Lifecycle dimType-slot fixup. Resolves \p lifecycleName to its process-global
// dimType slot (assigning the next free slot on first sight) and writes it to
// *slotOut, or kEJitInvalidDimType when all 8 lifecycle slots are taken. Called
// by AOT auto-registration to fill the wrapper's per-lifecycle dimType global.
void ejit_register_lifecycle(const char *lifecycleName, uint32_t *slotOut);

// Function dense-funcIndex fixup. Resolves \p funcName to its process-global
// dense funcIndex (assigning the next free index on first sight) and writes it
// to *slotOut, or kEJitInvalidFuncIndex when the funcIndex capacity is
// exhausted. Called by AOT auto-registration to fill the wrapper's per-function
// funcIndex global; a capacity failure is recorded so ejit_init can fail.
void ejit_register_funcindex(const char *funcName, uint32_t *slotOut);

// Register the wrapper's per-function inline-cache slot (@__ejit_icache_fn_<name>
// global address) by name, with its dimensionality (number of ejit_dim params).
// The runtime writes the frozen specialization pointer through the [D]^numDims
// cell at [i0][i1]... on a successful resolve (icacheFill); the wrapper reads
// the cell directly on the hit path (GEP + one atomic load + null-check +
// indirect call, no ejit_icache_try call). Keys the slot by the SAME registry
// funcIndex assigned by ejit_register_funcindex. Called by AOT auto-registration.
// A null slot or capacity exhaustion is recorded; the base stays null and the
// probe misses.
void ejit_register_icache_slot(const char *funcName, void *slot,
                               uint32_t numDims);

// Lifecycle. Activation is keyed by lifecycle/period name + instance index
// only; there is no array-pointer dimension in the active state (a period name
// with multiple arrays is activated as a whole for that instance).
ejit_status_t ejit_activate(const char *periodName, uint8_t cellIdx);
ejit_status_t ejit_deactivate(const char *periodName, uint8_t cellIdx);
ejit_status_t ejit_activate_all(const char *periodName);
ejit_status_t ejit_deactivate_all(const char *periodName);
bool ejit_is_active(const char *periodName, uint8_t cellIdx);

// Compilation
// ejit_taskpool_compile_or_get is the single compilation entry point for both
// Sync and Async modes (runtime-configurable via ejit_set_compile_mode).

typedef struct {
  uint32_t dimType;
  uint32_t instanceId;
} ejit_dim_pair_t;

ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims,
                                           uint32_t numDims, void **outFn,
                                           uint32_t *outBucket);

// Fixed-dimension fast paths (0-4 dims). Additive alternatives to
// ejit_taskpool_compile_or_get that pass the dim identity as scalar arguments
// instead of an ejit_dim_pair_t* + numDims pair, trimming the cache-hit path
// (no numDims bound/null checks, no variable-length validation loop, dims built
// directly on the stack). Return status and outFn/outBucket semantics are
// identical to ejit_taskpool_compile_or_get with the matching numDims; on a
// cache hit the caller still owns the read token and must call
// ejit_taskpool_release_read(*outBucket). 4 dims is the maximum; callers with
// more dims use the generic entry above.
ejit_status_t ejit_taskpool_compile_or_get_0d(uint32_t funcIndex, void **outFn,
                                              uint32_t *outBucket);
ejit_status_t ejit_taskpool_compile_or_get_1d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, void **outFn,
                                              uint32_t *outBucket);
ejit_status_t ejit_taskpool_compile_or_get_2d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, uint32_t dim1,
                                              uint32_t inst1, void **outFn,
                                              uint32_t *outBucket);
ejit_status_t ejit_taskpool_compile_or_get_3d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, uint32_t dim1,
                                              uint32_t inst1, uint32_t dim2,
                                              uint32_t inst2, void **outFn,
                                              uint32_t *outBucket);
ejit_status_t ejit_taskpool_compile_or_get_4d(uint32_t funcIndex, uint32_t dim0,
                                              uint32_t inst0, uint32_t dim1,
                                              uint32_t inst1, uint32_t dim2,
                                              uint32_t inst2, uint32_t dim3,
                                              uint32_t inst3, void **outFn,
                                              uint32_t *outBucket);
void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId,
                                        uint32_t enabled);
void ejit_taskpool_release_read(uint32_t bucketIndex);

unsigned ejit_taskpool_pending_count(void);

// Diagnostic wrapper timing helpers. AOT wrappers only call these when built
// with -ejit-wrapper-timing. Runtime aggregates repeated calls and prints one
// summary per EJIT_WRAPPER_TIMING_REPORT_EVERY samples (default 10000; set to 0
// to suppress periodic output) to avoid flooding board logs. The timestamp unit
// is platform-defined: SRE/freestanding builds use SRE_CycleCountGet64(), host
// fallback uses steady_clock nanoseconds.
uint64_t ejit_taskpool_trace_now(void);
void ejit_taskpool_trace_wrapper(uint32_t funcIndex, uint32_t status,
                                 void *fnPtr, uint32_t bucketIndex,
                                 uint64_t tBeforeLookup,
                                 uint64_t tAfterLookup,
                                 uint64_t tAfterFn,
                                 uint64_t tAfterRelease);

#ifdef EJIT_SRE_TASKPOOL_TESTING
unsigned ejit_taskpool_poll_one(void);
unsigned ejit_taskpool_poll_budget(unsigned maxItems);
#endif

// SRE taskpool statistics. Separate from ejit_stats_t (which reports the legacy
// LRU EJitCache); these counters describe the taskpool cache/dedup/queue
// pipeline used when EJIT_SRE_TASKPOOL is built. Fixed layout
// (uint64_t/uint32_t only) for stable ABI across the aarch64_be target.
typedef struct {
  uint64_t cacheHits;       ///< Calls served from the taskpool cache.
  uint64_t asyncCompiles;   ///< Successful compiles via the worker.
  uint64_t asyncEnqueues;   ///< Requests pushed onto the async queue.
  uint64_t alreadyPending;  ///< Duplicate submissions coalesced.
  uint64_t queueFull;       ///< Enqueues rejected because the queue was full.
  uint64_t compileFailed;   ///< Compiles that failed, were cancelled or dropped.
  uint64_t publishFailed;   ///< Results that could not enter the cache.
  uint64_t instanceDisabled; ///< Per-instance disable fast-path hits.
  uint64_t instanceDisabledPreActivate; ///< Subset of instanceDisabled that hit
                                        ///< before the first activate (init→
                                        ///< activate window). 0 in non-shared.
  uint32_t readyEntries;     ///< Live ready cache entries.
  uint32_t pendingEntries;   ///< Live in-flight dedup slots.
  uint32_t queueApproxSize;  ///< Approximate async queue depth.
  uint32_t reserved;         ///< Padding/reserved; always 0.
} ejit_taskpool_stats_t;

ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);

void ejit_taskpool_print_stats();
void ejit_taskpool_print_compiled();
uint32_t ejit_taskpool_get_worker_core();

/// Enable name-filtered JIT IR+ASM capture. When \p name is non-null and
/// non-empty, the next time a specialization whose entry name exactly matches
/// \p name is JIT-compiled, the engine saves (in memory) its post-optimization
/// IR and emitted assembly for later printing. Pass NULL or "" to disable
/// further capture (already-saved entries are retained). Capture is exact-name
/// unless name is "*", which captures every specialization. Full payloads stay
/// on the worker core and are never copied into shared taskpool memory.
void ejit_dump_func(const char *name);

/// Print the saved worker-local IR+ASM for \p name through the platform log.
/// Passing NULL or "" prints every entry saved on the calling core. A
/// non-worker core reports which worker owns the latest matching capture.
void ejit_print_dumped(const char *name);

/// Enable or disable capture of every JIT-compiled specialization. Equivalent
/// to ejit_dump_func("*") when enabled. Captures are keyed by function name in
/// the worker-local store and replaced when the same function is recompiled.
void ejit_dump_all(bool enable);

/// Runtime diagnostic log level. Mirrors the EJIT_DIAG* macro thresholds.
///   EJIT_LOG_OFF    — no diagnostic output
///   EJIT_LOG_INFO   — key events (default): init, compile begin/OK/FAIL,
///                      cache MISS, activation, errors, registration summary
///   EJIT_LOG_VERBOSE — per-item detail: each registration, per-function
///                      struct-field stats, per-call compile_or_get, taskpool
///   EJIT_LOG_DEBUG  — internals: idempotent skips, per-load replacement
///                      failures, dump mechanics
typedef enum {
  EJIT_LOG_OFF = 0,
  EJIT_LOG_INFO = 1,
  EJIT_LOG_VERBOSE = 2,
  EJIT_LOG_DEBUG = 3,
} ejit_log_level_t;

/// Set the runtime diagnostic log level. Takes effect immediately for all
/// subsequent EJIT_DIAG* output. Lower the level to reduce log volume in
/// production; raise it to VERBOSE/DEBUG when diagnosing a problem.
void ejit_set_log_level(ejit_log_level_t level);

/// Current runtime diagnostic log level.
ejit_log_level_t ejit_get_log_level(void);

/// Print the registered registry through the platform log: every registered
/// bitcode (funcIdx, name, size), period array (period, var, base, size),
/// static var (var, addr), plus funcIndex/lifecycle counts. For verifying
/// that AOT registration populated the runtime as expected.
void ejit_print_registry(void);

/// Print the !ejit.metadata of \p funcName (parsed from its registered
/// bitcode): whether it is an ejit_entry, its period_arr_ind parameter slots,
/// period arrays, and may_const field offsets. For diagnosing specialization
/// parameter binding and constant-substitution eligibility.
void ejit_print_func_meta(const char *funcName);

/// Fill \p out with code pool usage statistics (pools, sealed/active, used/
/// reserved/wasted bytes, seal/split invocations, finalized ranges). Returns
/// EJIT_OK on success, EJIT_ERR_NOT_ACTIVE if the runtime is not initialized,
/// EJIT_ERR_INVALID_PARAM on a null out pointer, EJIT_ERR_DISABLED if the
/// runtime was built without EJIT_SRE_CODE_POOL (no pool). For monitoring
/// embedded code-memory exhaustion. Mirrors EJitCodePoolManager::Stats.
ejit_status_t ejit_get_code_pool_stats(ejit_code_pool_stats_t *out);

/// Print code pool usage statistics through the platform log. Paired with
/// ejit_get_code_pool_stats() (human-readable form).
void ejit_print_code_pool_stats(void);

/// Print the currently-active time-window instances through the platform log:
/// for each registered period, every active (period, cell) is listed. Works
/// across builds (queries the same isActive() path the JIT gate uses: the
/// shared SwitchController in shared-taskpool builds, the per-instance
/// arrayStates_ otherwise). Static vars are always active. For diagnosing
/// "why did/didn't this period instance compile".
void ejit_print_active(void);

/// Print the EJIT runtime's build identity through the platform log: the LLVM
/// release version (major.minor.patch, from llvm/Config/llvm-config.h) and the
/// git commit + branch of the llvm-project source tree the runtime was built
/// from. The commit is captured at build time, so it tracks the source HEAD
/// even across incremental rebuilds. Needs no initialized runtime and is not
/// gated on the diagnostic log level, so the build identity is always
/// recoverable - useful for correlating a field device's behavior with the
/// exact source it was compiled from.
void ejit_print_version(void);

// Cache
void ejit_clear_cache(void);
void ejit_invalidate(const char *periodName, uint8_t cellIdx);

// Statistics
ejit_status_t ejit_get_stats(ejit_stats_t *stats);
const ejit_error_t *ejit_get_last_error(void);

// Configuration
void ejit_set_compile_mode(ejit_compile_mode_t mode);
ejit_compile_mode_t ejit_get_compile_mode(void);

#ifdef __cplusplus
}
#endif

#endif
