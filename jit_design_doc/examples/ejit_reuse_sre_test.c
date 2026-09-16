// PR230 on PR233. Replace the old test_ejit_period demo with this file.
// Core 6: test_ejit_period; core 16: test_ejit_period;
// core 6: test_ejit_reuse_print. All four shell arguments are ignored.
// Use a fresh, otherwise idle image. See EJIT_CODE_REUSE_BOARD.md.
#ifdef REUSE_USE_RUNTIME_HEADER
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#else
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef __SIZE_TYPE__ size_t;
typedef _Bool bool;
typedef enum { EJIT_OK = 0, EJIT_PENDING = 1,
               EJIT_ERR_QUEUE_FULL = -7 } ejit_status_t;
typedef enum { EJIT_COMPILE_SYNC = 0, EJIT_COMPILE_ASYNC = 1 }
    ejit_compile_mode_t;
typedef enum { EJIT_OPT_L1 = 1, EJIT_OPT_L2 = 2, EJIT_OPT_L3 = 3 }
    ejit_opt_level_t;
typedef struct {
  ejit_compile_mode_t compileMode;
  ejit_opt_level_t optLevel;
  size_t maxCodeMemory, maxDataMemory, maxCacheEntries, maxCacheSize;
  bool enableLogger, forceStaticRegistry;
  const char *dumpJITDir;
} ejit_config_t;
typedef struct { uint32_t dimType, instanceId; } ejit_dim_pair_t;
typedef struct { uint32_t generation, dimType, instanceId, version; }
    ejit_borrow_fence_t;
typedef struct {
  uint64_t cacheHits, asyncCompiles, asyncEnqueues, alreadyPending, queueFull;
  uint64_t compileFailed, publishFailed, instanceDisabled;
  uint64_t instanceDisabledPreActivate;
  uint32_t readyEntries, pendingEntries, queueApproxSize, reserved;
} ejit_taskpool_stats_t;
typedef struct {
  uint32_t active, groups, representativeLive, bundlesPublished, waitersLive;
  uint32_t representativesElected, representativeReElections;
  uint32_t rejectedAdmissions, schemaRejections, staleSettlements;
  uint64_t logicalRequests, representativeDispatches, waitersJoined;
  uint64_t waitersCompleted, waitersCancelled, bundlePublications;
  uint64_t retainedBundleBytes, physicalCodeObjects, sharedPhysicalReuses;
  uint64_t independentPhysicalObjects, completeProfiles, approximateProfiles;
  uint64_t edgeOnlyProfiles, valueDroppedProfiles;
  uint64_t representativeAttemptToken, representativeSamplingSessionId;
  uint64_t representativeLogicalKey, representativeDispatchCount;
  uint64_t representativeDispatchLimit, representativeQuotaEnd;
  uint64_t bundleGeneration, bundleDispatchCount, bundleDispatchLimit;
  uint64_t bundleQuotaEnd;
} ejit_representative_stats_t;
extern ejit_status_t ejit_init_representative(const ejit_config_t *);
extern ejit_status_t ejit_activate(const char *, uint32_t);
extern void ejit_register_funcindex(const char *, uint32_t *);
extern void ejit_register_lifecycle(const char *, uint32_t *);
extern ejit_status_t ejit_taskpool_compile_or_get(
    uint32_t, const ejit_dim_pair_t *, uint32_t, void **, uint32_t *);
extern void ejit_taskpool_release_read(uint32_t);
extern ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *);
extern unsigned ejit_taskpool_pending_count(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern ejit_status_t ejit_representative_get_stats(ejit_representative_stats_t *);
extern ejit_status_t ejit_representative_deactivate_begin(
    const char *, uint32_t, ejit_borrow_fence_t *);
extern ejit_status_t ejit_representative_borrow_status(const ejit_borrow_fence_t *);
extern void ejit_taskpool_print_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern void ejit_dump_func(const char *);
extern void ejit_print_dumped(const char *);
extern void ejit_print_dumped_module(const char *);
#endif

_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56,
               "64-bit spec5 config ABI required");
_Static_assert(sizeof(ejit_taskpool_stats_t) == 88 &&
               sizeof(ejit_representative_stats_t) == 232 &&
               sizeof(ejit_borrow_fence_t) == 16, "Unexpected stats/fence ABI");
extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifdef REUSE_HOST_CHECK
#define REUSE_ENTRY
#define REUSE_DIM(x)
#define REUSE_CONST
#define REUSE_ARRAY(x)
#define REUSE_SHARED
#else
#if !__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) || \
    !__has_attribute(ejit_period_const) || !__has_attribute(ejit_in_period_array)
#error "Compile with the EJIT Clang; do not ignore unknown EJIT attributes"
#endif
#define REUSE_ENTRY __attribute__((ejit_entry))
#define REUSE_DIM(x) __attribute__((ejit_dim(#x)))
#define REUSE_CONST __attribute__((ejit_period_const))
#define REUSE_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#ifndef REUSE_SHARED
#define REUSE_SHARED __attribute__((section(".mc_shared")))
#endif
#endif

#ifndef REUSE_RUN_INIT_ARRAY
#define REUSE_RUN_INIT_ARRAY 1
#endif
#ifndef REUSE_WAIT_ROUNDS
#define REUSE_WAIT_ROUNDS 30000u
#endif
#define REUSE_ENTRIES 20u
#define REUSE_CELLS 6u
#define REUSE_TRP 1u
#define REUSE_EACH(M) \
  M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7) M(8) M(9) \
  M(10) M(11) M(12) M(13) M(14) M(15) M(16) M(17) M(18) M(19)

typedef struct { REUSE_CONST uint32_t gain; uint32_t live[2]; } ReuseCell;
typedef uint32_t (*ReuseFn)(uint8_t, uint8_t, uint32_t);
#define REUSE_DEFINE(N) \
  REUSE_SHARED REUSE_ARRAY(cell) ReuseCell g_reuse_##N[REUSE_CELLS]; \
  REUSE_ENTRY uint32_t reuse_##N(REUSE_DIM(cell) uint8_t cell, \
                                REUSE_DIM(trp) uint8_t trp, uint32_t x) { \
    uint32_t gain = g_reuse_##N[cell].gain; \
    uint32_t v = g_reuse_##N[cell].live[trp]; \
    v += x > gain ? x * gain : x + gain; \
    for (uint32_t i = 0; i < x; ++i) v += i; \
    g_reuse_##N[cell].live[trp] = v; \
    return v + cell + N; \
  }
REUSE_EACH(REUSE_DEFINE)
#define REUSE_ROW(N) g_reuse_##N,
static ReuseCell *const reuse_rows[] = { REUSE_EACH(REUSE_ROW) };
#define REUSE_FN(N) reuse_##N,
static ReuseFn const reuse_entries[] = { REUSE_EACH(REUSE_FN) };
#define REUSE_NAME(N) "reuse_" #N,
static const char *const reuse_names[] = { REUSE_EACH(REUSE_NAME) };
static uint32_t reuse_indices[REUSE_ENTRIES], reuse_cell_dim, reuse_trp_dim;

enum { REUSE_RESET, REUSE_STARTING, REUSE_READY, REUSE_RUNNING,
       REUSE_DONE, REUSE_FAILED };
REUSE_SHARED uint32_t g_reuse_stage;
REUSE_SHARED uint64_t g_reuse_checked, g_reuse_deferred;
REUSE_SHARED void *g_reuse_before[REUSE_ENTRIES][REUSE_CELLS];
REUSE_SHARED void *g_reuse_after[REUSE_ENTRIES][REUSE_CELLS];
static void *reuse_first_unequal;

static int reuse_fail(const char *reason) {
  __atomic_store_n(&g_reuse_stage, REUSE_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[REUSE230] FAIL: %s; core 6 may print partial state\n", reason);
  return -1;
}
static int reuse_delay(void) {
  return SRE_TaskDelay(1u) ? reuse_fail("TaskDelay failed") : 0;
}
static int reuse_stats(ejit_taskpool_stats_t *s) {
  if (ejit_taskpool_get_stats(s) != EJIT_OK)
    return reuse_fail("taskpool stats unavailable");
  if (s->compileFailed || s->publishFailed || s->queueFull)
    return reuse_fail("compile/publish/actual queue failure");
  return 0;
}

// Every successful lookup is immediately executed while its read token is
// held. Probing a T1 pointer without executing would consume false samples.
static int reuse_call(unsigned entry, uint8_t cell, uint32_t x, void **seen,
                      int wrapper) {
  void *fn = 0;
  uint32_t bucket = 0;
  if (!wrapper) {
    ejit_dim_pair_t dims[2] = {{reuse_cell_dim, cell},
                              {reuse_trp_dim, REUSE_TRP}};
    ejit_status_t rc = ejit_taskpool_compile_or_get(
        reuse_indices[entry], dims, 2u, &fn, &bucket);
    if (rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) {
      // The public ABI also maps PGO admission deferral to QUEUE_FULL.
      // Actual queue failures are independently rejected by reuse_stats.
      if (fn) return reuse_fail("fallback unexpectedly returned a pointer");
      ++g_reuse_deferred;
      *seen = 0;
      return 0;
    }
    if (rc != EJIT_OK || !fn) return reuse_fail("lookup failed");
  }
  ReuseCell *rows = reuse_rows[entry];
  for (unsigned c = 0; c < REUSE_CELLS; ++c)
    for (unsigned t = 0; t < 2u; ++t) rows[c].live[t] = 100u + c * 7u + t;
  uint32_t gain = rows[cell].gain;
  uint32_t expected = 100u + cell * 7u + REUSE_TRP +
      (x > gain ? x * gain : x + gain) + x * (x - 1u) / 2u;
  uint32_t actual = wrapper ? reuse_entries[entry](cell, REUSE_TRP, x)
                           : ((ReuseFn)fn)(cell, REUSE_TRP, x);
  if (!wrapper) ejit_taskpool_release_read(bucket);
  if (actual != expected + cell + entry) return reuse_fail("wrong return value");
  for (unsigned c = 0; c < REUSE_CELLS; ++c)
    for (unsigned t = 0; t < 2u; ++t) {
      uint32_t want = c == cell && t == REUSE_TRP
                          ? expected : 100u + c * 7u + t;
      if (rows[c].live[t] != want) return reuse_fail("cell/trp live write frozen");
    }
  ++g_reuse_checked;
  if (seen) *seen = fn;
  return 1;
}

static int reuse_drive(unsigned updated) {
  void *(*pointers)[REUSE_CELLS] = updated ? g_reuse_after : g_reuse_before;
  unsigned stable = 0;
  for (unsigned round = 0; round < REUSE_WAIT_ROUNDS; ++round) {
    int complete = 1;
    // Visit ALL 120 identities on every round. Never wait for a wave of T1s
    // to become ready before sampling the representatives already admitted.
    for (unsigned e = 0; e < REUSE_ENTRIES; ++e) {
      for (uint8_t c = 0; c < REUSE_CELLS; ++c) {
        int rc = reuse_call(e, c, round % 31u + 1u, &pointers[e][c], 0);
        if (rc < 0) return -1;
        complete &= rc != 0;
        if (!updated && e == 0u && c == 5u && pointers[e][c] &&
            !reuse_first_unequal) reuse_first_unequal = pointers[e][c];
      }
      for (unsigned c = 1; c < REUSE_CELLS; ++c) {
        if (!updated && e == 0u && c == 5u)
          complete &= pointers[e][c] && pointers[e][c] != pointers[e][0] &&
                      pointers[e][c] != reuse_first_unequal;
        else complete &= pointers[e][c] == pointers[e][0];
      }
      if (updated)
        for (unsigned c = 0; c < REUSE_CELLS; ++c)
          complete &= pointers[e][c] == g_reuse_before[e][0];
    }
    ejit_taskpool_stats_t s = {0};
    if (reuse_stats(&s)) return -1;
    complete &= s.readyEntries == 120u && !s.pendingEntries &&
                !s.queueApproxSize && !ejit_taskpool_pending_count();
    stable = complete ? stable + 1u : 0u;
    if (stable >= 4u) {
      // Also exercise the generated AOT wrapper's normal cache-hit dispatch.
      for (unsigned e = 0; e < REUSE_ENTRIES; ++e)
        for (uint8_t c = 0; c < REUSE_CELLS; ++c)
          if (reuse_call(e, c, 23u, 0, 1) < 0) return -1;
      SRE_printf("[REUSE230] PHASE_%s checked: 120 ready, same-value pointers "
                 "shared; %s\n", updated ? "UPDATE" : "INITIAL",
                 updated ? "updated cell rejoined original T2" :
                           "unequal cell has distinct post-T1 code");
      return 0;
    }
    if (reuse_delay()) return -1;
  }
  return reuse_fail("T2 convergence timeout; inspect all 120 entries on core 6");
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a; (void)b; (void)c; (void)d;
  if (g_ucLocalCoreID != 6u && g_ucLocalCoreID != 16u) return -1;
  uint32_t stage = __atomic_load_n(&g_reuse_stage, __ATOMIC_ACQUIRE);
  if (g_ucLocalCoreID == 6u) {
    if (stage != REUSE_RESET) {
      SRE_printf("[REUSE230] stage=%u; no re-init, reset board to rerun\n", stage);
      return stage == REUSE_READY || stage == REUSE_DONE ? 0 : -1;
    }
    if (!__atomic_compare_exchange_n(&g_reuse_stage, &stage, REUSE_STARTING,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -1;
    for (unsigned e = 0; e < REUSE_ENTRIES; ++e)
      for (unsigned c = 0; c < REUSE_CELLS; ++c)
        reuse_rows[e][c].gain = e == 0u && c == 5u ? 17u : e + 3u;
  } else {
    if (stage != REUSE_READY ||
        !__atomic_compare_exchange_n(&g_reuse_stage, &stage, REUSE_RUNNING,
                                     0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return -1;
  }
#if REUSE_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  // Resolve only before registration freezes, after BOTH cores' constructors.
  // Duplicate names are idempotent; no new lifecycle/function is introduced.
  for (unsigned e = 0; e < REUSE_ENTRIES; ++e) {
    reuse_indices[e] = ~0u;
    ejit_register_funcindex(reuse_names[e], &reuse_indices[e]);
    if (reuse_indices[e] == ~0u) return reuse_fail("funcIndex unresolved");
  }
  reuse_cell_dim = reuse_trp_dim = ~0u;
  ejit_register_lifecycle("cell", &reuse_cell_dim);
  ejit_register_lifecycle("trp", &reuse_trp_dim);
  if (reuse_cell_dim == ~0u || reuse_trp_dim == ~0u)
    return reuse_fail("lifecycle unresolved");
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = 1;
  if (ejit_init_representative(&cfg) != EJIT_OK)
    return reuse_fail("representative runtime initialization failed");
  if (ejit_taskpool_get_worker_core() != 6u) return reuse_fail("worker is not core 6");
  if (g_ucLocalCoreID == 6u) {
    ejit_dump_func("reuse_0");
    __atomic_store_n(&g_reuse_stage, REUSE_READY, __ATOMIC_RELEASE);
    SRE_printf("[REUSE230] WORKER_READY; core 16: test_ejit_period\n");
    return 0;
  }
  ejit_taskpool_stats_t s = {0};
  if (reuse_stats(&s)) return -1;
  if (s.asyncCompiles || s.readyEntries || s.pendingEntries || s.queueApproxSize ||
      ejit_taskpool_pending_count()) return reuse_fail("requires fresh idle image");
  for (uint32_t c = 0; c < REUSE_CELLS; ++c)
    if (ejit_activate("cell", c) != EJIT_OK) return reuse_fail("activate cell");
  if (ejit_activate("trp", REUSE_TRP) != EJIT_OK) return reuse_fail("activate trp");
  if (reuse_drive(0u)) return -1;
  // This isolated owner has stopped all business calls. Do not mutate any
  // frozen byte until the compiler-source borrow fence has completed.
  ejit_borrow_fence_t fence = {0};
  if (ejit_representative_deactivate_begin("cell", 5u, &fence) != EJIT_OK)
    return reuse_fail("deactivate/fence begin failed");
  unsigned waited = 0;
  for (;;) {
    ejit_status_t rc = ejit_representative_borrow_status(&fence);
    if (rc == EJIT_OK) break;
    if (rc != EJIT_PENDING || ++waited >= REUSE_WAIT_ROUNDS)
      return reuse_fail("borrow fence failed/timed out; source NOT modified");
    if (reuse_delay()) return -1;
  }
  g_reuse_0[5].gain = 3u;
  if (ejit_activate("cell", 5u) != EJIT_OK) return reuse_fail("reactivate cell");
  if (reuse_drive(1u)) return -1;
  __atomic_store_n(&g_reuse_stage, REUSE_DONE, __ATOMIC_RELEASE);
  SRE_printf("[REUSE230] OWNER_DONE checked=%llu deferred=%llu; "
             "core 6: test_ejit_reuse_print for worker-side acceptance\n",
             (unsigned long long)g_reuse_checked,
             (unsigned long long)g_reuse_deferred);
  return 0;
}

// Read-only: no init-array, init, activation, cache clear or compile request.
int test_ejit_reuse_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a; (void)b; (void)c; (void)d;
  if (g_ucLocalCoreID != 6u) return -1;
  uint32_t stage = __atomic_load_n(&g_reuse_stage, __ATOMIC_ACQUIRE);
  if (stage != REUSE_DONE && stage != REUSE_FAILED) return -1;
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_dumped("reuse_0");
  ejit_print_dumped_module("reuse_0");
  ejit_representative_stats_t r = {0};
  ejit_taskpool_stats_t s = {0};
  if (ejit_representative_get_stats(&r) != EJIT_OK ||
      ejit_taskpool_get_stats(&s) != EJIT_OK) return -1;
  SRE_printf("[REUSE230] groups=%u elected=%u dispatch=%llu bundles=%llu "
             "physical=%llu reused=%llu independent=%llu\n",
             r.groups, r.representativesElected,
             (unsigned long long)r.representativeDispatches,
             (unsigned long long)r.bundlePublications,
             (unsigned long long)r.physicalCodeObjects,
             (unsigned long long)r.sharedPhysicalReuses,
             (unsigned long long)r.independentPhysicalObjects);
  // Stats are owned by the compiler on core 6, not the producer's empty
  // core-local group registry. Pointer equality alone is not codegen evidence.
  if (stage != REUSE_DONE || !r.active || r.physicalCodeObjects != 21u ||
      r.representativesElected != 21u || r.representativeDispatches != 1344u ||
      r.bundlePublications != 21u || r.sharedPhysicalReuses < 99u ||
      r.independentPhysicalObjects || r.representativeReElections ||
      r.schemaRejections || r.waitersJoined != r.waitersCompleted ||
      r.waitersCancelled || s.compileFailed || s.publishFailed || s.queueFull ||
      s.readyEntries != 120u || s.pendingEntries ||
      s.queueApproxSize || ejit_taskpool_pending_count()) {
    SRE_printf("[REUSE230] NOT_ACCEPTED: inspect worker diagnostics; "
               "OWNER_DONE alone is not acceptance\n");
    return -1;
  }
  SRE_printf("[REUSE230] PASS: 120 logical entries, 21 physical T2 objects, "
             "21 x 64 representative dispatches; unequal/update/live checks passed\n");
  return 0;
}
