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
#define EJIT_BOUND_PTR(x)
#define ejit_free_dim
#define ejit_period_arr_ind(x)
#define EJIT_ENTRY
#define ejit_entry
#define EJIT_PERIOD_GUARD(x)
#define ejit_period_lc(x)
#else
// New names (preferred)
#define EJIT_PERIOD_CONST __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD(x) __attribute__((ejit_in_period(#x)))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x)             __attribute__((ejit_dim(#x)))
#define EJIT_BOUND_PTR(x) __attribute__((ejit_bound_ptr(#x)))
#define EJIT_ENTRY              __attribute__((ejit_entry))
#define EJIT_PERIOD_GUARD(x)    __attribute__((ejit_period_guard(#x)))
// Self-referential on purpose, and deliberately WITHOUT an EJIT_FREE_DIM twin:
// the macro name matches the attribute spelling, so the expansion is stopped by
// the "macro is not replaced inside its own expansion" rule. A second macro
// expanding to the same __attribute__ would not be protected — it would rewrite
// the inner ejit_free_dim token and emit __attribute__((__attribute__(...))).
#define ejit_free_dim __attribute__((ejit_free_dim))
// Old names (aliases — use new macros to avoid double expansion)
#define ejit_may_const EJIT_PERIOD_CONST
#define ejit_period(x) EJIT_IN_PERIOD(x)
#define ejit_period_arr(x) EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr_ind(x) EJIT_DIM(x)
#define ejit_entry EJIT_ENTRY
#define ejit_period_lc(x) EJIT_PERIOD_GUARD(x)
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

/// Register the allocation behind a pointer-valued period global before
/// ejit_init. \p varName is the global's symbol name, \p base is the pointer's
/// value (not its address), and \p size is the allocation size in bytes. The
/// allocation must stay mapped until ejit_shutdown; bounds do not follow the
/// pointer to a different allocation. Repeating identical bounds succeeds;
/// invalid, conflicting, or post-init calls return EJIT_ERR_INVALID_PARAM.
ejit_status_t ejit_register_pointer_period_bounds(const char *varName,
                                                  const void *base,
                                                  uint64_t size);

/// Return the size in bytes of the allocation starting exactly at \p base, or
/// zero if unknown. Reported allocations must stay mapped until ejit_shutdown.
/// Called from the compiler worker, so it must be thread-safe, and it must not
/// call back into EJIT.
typedef uint64_t (*ejit_allocation_size_lookup_t)(const void *base,
                                               void *context);

/// Install an allocator size lookup before ejit_init, on every image including
/// the compiler worker. Compilation queries it for pointer-valued period
/// globals so free_dim loads through them can fold without per-global bounds.
/// Explicit bounds take precedence, and folds made without a lookup are
/// unchanged. Compiled code is not invalidated when allocations change.
/// Repeating an identical install succeeds; null, conflicting, or post-init
/// installs return EJIT_ERR_INVALID_PARAM. Install again after ejit_shutdown.
ejit_status_t ejit_set_allocation_size_lookup(ejit_allocation_size_lookup_t lookup,
                                             void *context);

typedef enum {
  // Value 0 was the retired Sync mode. Select a mode explicitly.
  EJIT_COMPILE_ASYNC = 1,
  EJIT_COMPILE_OFF = 2,
} ejit_compile_mode_t;

typedef enum {
  EJIT_OPT_L1 = 1,
  EJIT_OPT_L2 = 2,
  EJIT_OPT_L3 = 3,
} ejit_opt_level_t;

/// Unversioned config ABI. Rebuild firmware and all callers with this header;
/// objects using the previous, larger config layout are not compatible.
typedef struct {
  ejit_compile_mode_t compileMode;
  ejit_opt_level_t optLevel;
  /// If true, force the static registry table path (skip constructors).
  bool forceStaticRegistry;
  /// If non-NULL, dump JIT-optimized LLVM IR (.ll) to this directory.
  const char *dumpJITDir;
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
  uint64_t poolCount;     ///< total 2MiB pools created
  uint64_t sealedCount;   ///< pools currently sealed (RX)
  uint64_t activeCount;   ///< pools still RW
  uint64_t usedBytes;     ///< sum of bump offsets across all pools
  uint64_t reservedBytes; ///< sum of pool sizes across all pools
  uint64_t wastedBytes;   ///< unused tail bytes inside sealed pools
  uint64_t
      sealInvocations; ///< successful enable_ex calls (per 4K page in 4K mode)
  uint64_t splitInvocations;    ///< successful split_2m_to_4k calls (4K mode)
  uint64_t finalizedRangeCount; ///< distinct executable ranges recorded
} ejit_code_pool_stats_t;

/// Placement-aware code-pool statistics. The original stats API remains ABI
/// stable and returns `total`; use this v2 shape to distinguish final code in
/// the near fixed pool from temporary Tier-1 code in the far dynamic pool.
typedef struct ejit_code_pool_stats_v2_t {
  ejit_code_pool_stats_t total;
  ejit_code_pool_stats_t near;
  ejit_code_pool_stats_t far;
} ejit_code_pool_stats_v2_t;

// Initialization
ejit_status_t ejit_init(const ejit_config_t *config);
/// Initialize with online PGO enabled (Tier-1 instrumentation + lazy Tier-2
/// PGOUse recompile). Uses the same config layout as ejit_init(config) but forces
/// the runtime's online-PGO auto-trigger on. Requires the runtime to be linked
/// with LLVMProfileData + LLVMInstrumentation (a build without PGO support
/// treats this as ejit_init). PGO is off for plain ejit_init().
ejit_status_t ejit_init_pgo(const ejit_config_t *config);
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

// Register the wrapper's per-function inline-cache cell table
// (@__ejit_icache_fn_<name> global address) by name, with its dimensionality
// (number of ejit_dim params) and, for sentinel-form slots, the MissFn pointer
// the table is pre-filled with. The runtime writes the specialization pointer
// through the [D]^numDims cell at [i0][i1]... on a successful resolve
// (icacheFill) and writes every cell back to its empty value on a period
// toggle (icacheDrainAll) -- the &MissFn sentinel when \p missFn is non-null
// (the branchless probe BLRs the cell, so the empty value must be callable),
// 0 otherwise. Keys the slot by the SAME registry funcIndex assigned by
// ejit_register_funcindex. Called by AOT auto-registration on EVERY core: the
// table is one shared object, so each core records the same base and a drain
// on any of them clears the cells all of them read. A null slot, capacity
// exhaustion, or a numDims above the cap is recorded; the base stays null and
// the probe misses. \p missFn has no default: this header is shared with pure
// C translation units (default arguments are C++-only).
void ejit_register_icache_slot(const char *funcName, void *slot,
                               uint32_t numDims, void *missFn);

// Lifecycle. Activation is keyed by lifecycle/period name + instance index
// only; there is no array-pointer dimension in the active state (a period name
// with multiple arrays is activated as a whole for that instance).
// cellIdx is uint32_t on the ABI so an out-of-range instance index is
// REJECTED by the runtime instead of being silently truncated to 8 bits at
// the call boundary (the AOT wrapper passes an i32). The runtime checks
// cellIdx < kEJitMaxInstances.
ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx);
ejit_status_t ejit_deactivate(const char *periodName, uint32_t cellIdx);
ejit_status_t ejit_activate_all(const char *periodName);
ejit_status_t ejit_deactivate_all(const char *periodName);
// uint32_t for the same reason as ejit_activate above: an out-of-range index
// is rejected (returns false) instead of being truncated at the ABI boundary
// and silently querying instance 0.
bool ejit_is_active(const char *periodName, uint32_t cellIdx);

// Compile misses run on the worker. Off is selected only at initialization.

typedef struct {
  uint32_t dimType;
  uint32_t instanceId;
} ejit_dim_pair_t;

/// Borrowed bound object descriptor. The object must remain valid and mapped
/// at the same virtual address from enqueue through the end of compilation.
/// The runtime copies this descriptor only; it never copies, allocates, or
/// frees the pointed-to bytes.
typedef struct {
  const void *rawPtr;
  uint32_t size;
  uint32_t argIndex;
} ejit_bound_ptr_t;

/// All three taskpool resolve calls accept omitted outputs (for example,
/// status-only input validation). A caller that can hit in a reclaiming build
/// must collect the bucket and release the read after using the function.
/// In NO_RECLAIM a successful lookup needs no reader release; releasing the
/// returned sentinel is harmless. Callers must not retain tokens across init
/// generations.
ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                           const ejit_dim_pair_t *dims,
                                           uint32_t numDims, void **outFn,
                                           uint32_t *outBucket);

// Slow-path compatibility variant used by wrappers with one EJIT_BOUND_PTR.
// rawPtr is borrowed until the compile callback returns. It requires a
// non-null, non-empty, non-overflowing object; invalid input returns
// EJIT_ERR_INVALID_PARAM so the wrapper can take its AOT path.
ejit_status_t ejit_taskpool_compile_or_get_bound(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const void *rawPtr, uint32_t rawSize, uint32_t boundArgIndex, void **outFn,
    uint32_t *outBucket);

// Multi-pointer raw transport variant. The fixed descriptor list supports up
// to eight bound parameters and has no relationship to cache identity. Each
// argIndex must name a pointer formal declared with EJIT_BOUND_PTR in the
// registered root; the cold compile path rejects other indices. The caller
// owns every object and must apply the deactivate -> modify -> reactivate
// protocol before changing an active specialization's object.
ejit_status_t ejit_taskpool_compile_or_get_bound_v(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const ejit_bound_ptr_t *bounds, uint32_t boundCount, void **outFn,
    uint32_t *outBucket);

void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId,
                                        uint32_t enabled);
void ejit_taskpool_release_read(uint32_t bucketIndex);

unsigned ejit_taskpool_pending_count(void);

/// Seal and publish all completed asynchronous specializations staged in the
/// compact near code pool. Under online PGO, Tier-1 is already executable in
/// the far pool and linked Tier-2 batches normally publish automatically when
/// the compile queue drains. This call forces publication or retries a failed
/// automatic publication.
/// EJIT_OK guarantees enable_ex completed and the shared cache / inline-cache
/// entries were backfilled by the owner worker.
ejit_status_t ejit_publish_pending_code(void);

// SRE taskpool statistics. Separate from ejit_stats_t (which reports the legacy
// LRU EJitCache); these counters describe the taskpool cache/dedup/queue
// shared taskpool pipeline. Fixed layout
// (uint64_t/uint32_t only) for stable ABI across the aarch64_be target.
typedef struct {
  uint64_t cacheHits;      ///< Calls served from the taskpool cache.
  uint64_t asyncCompiles;  ///< Successful compiles via the worker.
  uint64_t asyncEnqueues;  ///< Requests pushed onto the async queue.
  uint64_t alreadyPending; ///< Duplicate submissions coalesced.
  uint64_t queueFull;      ///< Enqueues rejected because the queue was full.
  uint64_t compileFailed;  ///< Compiles that failed, were cancelled or dropped.
  uint64_t publishFailed;  ///< Results that could not enter the cache.
  uint64_t instanceDisabled; ///< Per-instance disable fast-path hits.
  uint64_t instanceDisabledPreActivate; ///< Subset of instanceDisabled that hit
                                        ///< before the first activate (init→
                                        ///< activate window). 0 in non-shared.
  uint32_t readyEntries;                ///< Live ready cache entries.
  uint32_t pendingEntries;              ///< Live in-flight dedup slots.
  uint32_t queueApproxSize;             ///< Approximate async queue depth.
  uint32_t reserved;                    ///< Padding/reserved; always 0.
} ejit_taskpool_stats_t;

ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);

void ejit_taskpool_print_stats();
/// Print every published shared-cache version and its dimensions. Stats builds
/// also report whether a later taskpool lookup reused each published version;
/// wrapper inline-cache calls after that first reuse remain intentionally free.
/// An instrumented entry also reports its current sample count and PGO quota.
void ejit_taskpool_print_compiled();
uint32_t ejit_taskpool_get_worker_core();

#ifdef EJIT_SRE_PGO_VALUE_PROFILE
// Online-PGO value-profiling observability (EJIT_VALUE_PROFILE.md §9). All
// counters are cold-path (Tier-2 merge / transform), so they are always
// compiled when the feature is; the per-call cost stays zero. Fixed layout
// (uint64_t only) for stable ABI across the aarch64_be target.
typedef struct {
  uint64_t merges;            ///< Tier-2 value-profile merges performed.
  uint64_t icValueSites;      ///< Indirect-call value sites merged.
  uint64_t memopValueSites;   ///< Memop-size value sites merged.
  uint64_t scalarValueSites;  ///< Scalar sites merged (pre-threshold).
  uint64_t scalarDropped;     ///< Scalar sites dropped by the thresholds.
  uint64_t scalarSpecialized; ///< Guarded scalar specializations created.
  uint64_t reserved;          ///< Reserved; always 0.
} ejit_vp_stats_t;

ejit_status_t ejit_vp_get_stats(ejit_vp_stats_t *out);
#endif

/// Enable name-filtered JIT IR+ASM capture. When \p name is non-null and
/// non-empty, the next time a specialization whose entry name exactly matches
/// \p name is JIT-compiled, the engine saves (in memory) post-optimization IR
/// and emitted assembly for both the entry function and its complete
/// specialization module. Pass NULL or "" to disable further capture
/// (already-saved entries are retained). Capture is exact-name unless name is
/// "*", which captures every specialization. Full payloads stay on the worker
/// core and are never copied into shared taskpool memory.
void ejit_dump_func(const char *name);

/// Print the saved worker-local IR+ASM for \p name through the platform log.
/// Passing NULL or "" prints every entry saved on the calling core. A
/// non-worker core reports which worker owns the latest matching capture.
void ejit_print_dumped(const char *name);

/// Print the complete specialization module captured for \p name, including
/// all retained function definitions in that module. Passing NULL or ""
/// prints every module saved on the calling core. This is worker-local and
/// does not perform cross-core lookup.
void ejit_print_dumped_module(const char *name);

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

/// Placement-aware counterpart of ejit_get_code_pool_stats().
ejit_status_t ejit_get_code_pool_stats_v2(ejit_code_pool_stats_v2_t *out);

/// Print code pool usage statistics through the platform log. Paired with
/// ejit_get_code_pool_stats() (human-readable form).
void ejit_print_code_pool_stats(void);

/// Print a ranking for every sampled ejit_entry function. The ranking is
/// ordered by dynamically removed may_const load executions per million
/// platform timestamp units (removed work per entry times entry frequency).
/// Call this explicitly after the audit sampling windows have completed. In a
/// shared-taskpool build, a non-owner core forwards the request to the
/// compile-owner worker and waits for printing to finish.
void ejit_print_mayconst_ranking(void);

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

// Retire dispatch shortcuts. These calls do not start a new period version;
// change specialized configuration between deactivate and activate.
void ejit_clear_cache(void);
// uint32_t for the same reason as ejit_activate above: an out-of-range index
// is rejected (no-op) instead of being truncated at the ABI boundary and
// silently invalidating instance 0.
void ejit_invalidate(const char *periodName, uint32_t cellIdx);

// Statistics
ejit_status_t ejit_get_stats(ejit_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
