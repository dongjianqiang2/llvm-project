/* PR231 SRE board fixture. Link this instead of other test_ejit_period demos.
 *
 * Compile this ONE C file with patched EJIT clang, like other period demos.
 * A volatile callback slot keeps the real AOT observer outside the JIT closure;
 * no second business TU, handwritten bitcode, or handwritten JIT is required.
 * Configuration and output must be mapped coherent/shared at the same VA on
 * both cores by the product linker. A .mc_shared name alone is not proof.
 */
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableSreBridge.h"
#include <string.h>

#ifndef EJIT_PR231_SHARED
#define EJIT_PR231_SHARED __attribute__((section(".mc_shared"), aligned(64)))
#endif
#define PR231_CELLS 6u
#define PR231_TRPS 2u
#define PR231_WORKER 6u
#define PR231_PRODUCER 16u
typedef struct {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t cellGain;
  ejit_may_const uint32_t trpBias;
  ejit_may_const uint32_t jointBias;
  uint32_t liveBias;
} PR231Config;

ejit_period_arr(pr231_cell) PR231Config
    g_pr231_config[PR231_CELLS][PR231_TRPS] EJIT_PR231_SHARED;
int64_t g_pr231_output[PR231_CELLS][PR231_TRPS] EJIT_PR231_SHARED;

void pr231_probe_inflight(void);
/* PASS1 externalizes mutable globals without following their initializer.
 * Volatile prevents folding this into a same-TU direct call, even with LTO.
 * The pointer remains a genuine AOT address; it is never a fabricated hook.
 */
void (*volatile g_pr231_probe_dispatch)(void) EJIT_PR231_SHARED =
    pr231_probe_inflight;

__attribute__((noinline)) int64_t
pr231_positive(int64_t x, uint32_t gain, uint32_t bias) {
  return x * (int64_t)gain + (int64_t)bias;
}
__attribute__((noinline)) int64_t
pr231_negative(int64_t x, uint32_t gain, uint32_t bias) {
  return x * (int64_t)gain - (int64_t)bias;
}

ejit_entry int64_t pr231_smalltable_entry(
    ejit_period_arr_ind(pr231_cell) uint8_t cell,
    ejit_period_arr_ind(pr231_trp) uint8_t trp, int64_t x) {
  g_pr231_probe_dispatch();
  const PR231Config *cfg = &g_pr231_config[cell][trp];
  const uint32_t bias = cfg->trpBias + cfg->jointBias;
  int64_t result = x >= 0 ? pr231_positive(x, cfg->cellGain, bias)
                          : pr231_negative(x, cfg->cellGain, bias);
  /* Uniform, cell-only, TRP-only and joint fields, plus a genuine live load
   * and store. Dynamic sign branches keep the profile nontrivial. */
  if (cfg->mode == 1u)
    result += (int64_t)cfg->liveBias;
  else
    result -= (int64_t)cfg->liveBias;
  g_pr231_output[cell][trp] = result;
  return result;
}

/* Like the reuse/MFS demos, runtime owns platform dispatch and permissions.
 * No task-self API or extra SDK binding header is required. Static shared
 * objects must be inside the real __ejit_shared_start/end linker bounds;
 * common tables use separate RW/NX pages from the existing fixed pool domain.
 * The product still owns same-VA/same-physical coherent mapping of both areas.
 * Missing bounds, unsafe ranges or refused permissions remain BLOCKED.
 */
#ifndef EJIT_PR231_CALL_INIT_ARRAY
#define EJIT_PR231_CALL_INIT_ARRAY 1
#endif
#ifndef EJIT_PR231_WAIT_ROUNDS
#define EJIT_PR231_WAIT_ROUNDS 10000u
#endif

extern int SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern uint8_t g_ucLocalCoreID;
#if EJIT_PR231_CALL_INIT_ARRAY
extern void call_init_array_functions(void);
#endif

static ejit_small_table_sre_source_state_t g_pr231_source EJIT_PR231_SHARED;
static uint32_t g_pr231_stage EJIT_PR231_SHARED;
static uint32_t g_pr231_probe_arm EJIT_PR231_SHARED;
static int32_t g_pr231_probe_result EJIT_PR231_SHARED;
/* These are deliberately core-private: constructors, bindings and the request
 * builder are not shared C++/callback state across cores. */
static uint32_t pr231_initialized;
#if EJIT_PR231_CALL_INIT_ARRAY
static uint32_t pr231_init_array_done;
#endif
static uint32_t pr231_func = UINT32_MAX;
static ejit_small_table_sre_request_t pr231_request;
/* Keep the bounded inventories off the product shell's small task stack.
 * Main/probe use separate core-private buffers because the probe is nested in
 * an actual call made by main. The stage lock prevents concurrent main runs. */
static ejit_small_table_sre_snapshot_t pr231_main_snapshot, pr231_full_snapshot;
static ejit_small_table_sre_snapshot_t pr231_probe_before, pr231_probe_after;

/* Independent plain AOT reference: no annotated entry, no helper reuse, no
 * resolver calls and no fake invocation count. It also checks destination
 * stores made by the actual generated wrapper/JIT entry.
 */
static int64_t pr231_expected(uint32_t cell, uint32_t trp, int64_t x) {
  const PR231Config *cfg = &g_pr231_config[cell][trp];
  const int64_t bias = (int64_t)cfg->trpBias + (int64_t)cfg->jointBias;
  int64_t result = x * (int64_t)cfg->cellGain;
  result += x >= 0 ? bias : -bias;
  result += cfg->mode == 1u ? (int64_t)cfg->liveBias : -(int64_t)cfg->liveBias;
  return result;
}

static void pr231_delay(void *unused, uint32_t ticks) {
  (void)unused;
  (void)SRE_TaskDelay(ticks);
}
static int pr231_snapshot(ejit_small_table_sre_snapshot_t *s) {
  memset(s, 0, sizeof(*s));
  s->abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  s->structSize = sizeof(*s);
  const int rc = ejit_small_table_sre_get_snapshot(pr231_func, s);
  if (rc != EJIT_STAB_SRE_OK) {
    SRE_printf("[STAB231] snapshot rc=%d reason=%s\n", rc, s->reason);
    return -1;
  }
  pr231_func = s->funcIndex;
  return 0;
}

static int pr231_call(uint32_t n) {
  const uint32_t cell = n % PR231_CELLS;
  const uint32_t trp = (n / PR231_CELLS) % PR231_TRPS;
  const int64_t x = (n & 1u) ? -(int64_t)(n + 1u) : (int64_t)(n + 1u);
  const int64_t expected = pr231_expected(cell, trp, x);
  const int64_t got = pr231_smalltable_entry((uint8_t)cell, (uint8_t)trp, x);
  if (got != expected || g_pr231_output[cell][trp] != expected) {
    SRE_printf("[STAB231] FAIL value cell=%u trp=%u got=%lld want=%lld\n",
               cell, trp, (long long)got, (long long)expected);
    return -1;
  }
  return 0;
}

/* Called by the real business body, not by the test before/after a call. When
 * armed this observes an actual outstanding wrapper ticket, cancels it on the
 * owner worker, and confirms cancel did NOT stand in for the real leave.
 * No configuration writes or source freeing take place in this hook.
 */
__attribute__((noinline)) void pr231_probe_inflight(void) {
  if (__atomic_exchange_n(&g_pr231_probe_arm, 0u, __ATOMIC_ACQ_REL) == 0u)
    return;
  ejit_small_table_sre_snapshot_t *before = &pr231_probe_before;
  ejit_small_table_sre_snapshot_t *after = &pr231_probe_after;
  int result = -1;
  if (pr231_snapshot(before) == 0 && before->physicalExecutions != 0u &&
      before->borrowReaders != 0u &&
      ejit_small_table_sre_cancel(pr231_func) == EJIT_STAB_SRE_OK &&
      pr231_snapshot(after) == 0 && after->physicalExecutions != 0u &&
      after->borrowReaders != 0u) {
    result = 0;
    SRE_printf("[STAB231] HELD_CANCEL physical=%llu borrow=%llu retained=%llu\n",
               (unsigned long long)after->physicalExecutions,
               (unsigned long long)after->borrowReaders,
               (unsigned long long)after->retainedExecutions);
  }
  __atomic_store_n(&g_pr231_probe_result, result, __ATOMIC_RELEASE);
}

static int pr231_setup(void) {
  if (pr231_initialized)
    return 0;
#if EJIT_PR231_CALL_INIT_ARRAY
  /* A later permission/runtime failure must not cause constructors to run
   * twice when setup is retried. This is per-core, not runtime readiness. */
  if (!pr231_init_array_done) {
    SRE_printf("[STAB231][core=%u] init-array begin (once)\n", g_ucLocalCoreID);
    call_init_array_functions();
    pr231_init_array_done = 1u;
    SRE_printf("[STAB231][core=%u] init-array done\n", g_ucLocalCoreID);
  }
#endif
  ejit_small_table_sre_bindings_t bindings;
  memset(&bindings, 0, sizeof(bindings));
  bindings.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  bindings.structSize = sizeof(bindings);
  bindings.flags = g_ucLocalCoreID == PR231_WORKER
                       ? EJIT_STAB_SRE_ENABLE_FIXED_DOMAIN : 0u;
  bindings.waitRounds = EJIT_PR231_WAIT_ROUNDS;
  bindings.current_task_id = 0; /* Unknown SDK task ID; never a fabricated ID. */
  /* NULL selects runtime's existing SRE scheduler and strict shared-range
   * preparation. It does not install a successful no-op permission hook. */
  bindings.delay_ticks = 0;
  bindings.prepare_shared_data = 0;
  int rc = ejit_small_table_sre_prepare(&bindings);
  if (rc != EJIT_STAB_SRE_OK) {
    SRE_printf("[STAB231] prepare BLOCKED rc=%d (fresh boot required)\n", rc);
    return -20;
  }
  /* Check every real static object before using it. The observer runs in AOT
   * against this core's private controller; only its slot/flags are shared. */
  const uint32_t rw = EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE;
  if (ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_probe_dispatch,
          sizeof(g_pr231_probe_dispatch), EJIT_STAB_SRE_DATA_READ) !=
          EJIT_STAB_SRE_OK ||
      ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_config,
          sizeof(g_pr231_config), rw) != EJIT_STAB_SRE_OK ||
      ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_output,
          sizeof(g_pr231_output), rw) != EJIT_STAB_SRE_OK ||
      ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_source,
          sizeof(g_pr231_source), rw) != EJIT_STAB_SRE_OK ||
      ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_stage,
          sizeof(g_pr231_stage), rw) != EJIT_STAB_SRE_OK ||
      ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_probe_arm,
          sizeof(g_pr231_probe_arm), rw) != EJIT_STAB_SRE_OK ||
      ejit_small_table_sre_prepare_data((uintptr_t)&g_pr231_probe_result,
          sizeof(g_pr231_probe_result), rw) != EJIT_STAB_SRE_OK) {
    SRE_printf("[STAB231] BLOCKED: real shared bounds/data permissions; "
               "inspect runtime diagnostics (no SDK task-id binding needed)\n");
    return -20;
  }
  /* Register the actual mutable slot identically on BOTH cores, before init.
   * PASS1 also auto-registers it. Its initializer relocation keeps the real
   * observer alive through GC/lipo; no controller bodies enter the JIT closure.
   */
  ejit_register_symbol("g_pr231_probe_dispatch", (void *)&g_pr231_probe_dispatch);
  ejit_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L3;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;
  rc = ejit_init_pgo(&cfg);
  if (rc != EJIT_OK || ejit_taskpool_get_worker_core() != PR231_WORKER) {
    SRE_printf("[STAB231] FAIL init rc=%d worker=%u\n", rc,
               ejit_taskpool_get_worker_core());
    return -2;
  }
  pr231_initialized = 1u;
  return 0;
}

static int pr231_commit_configuration(void) {
  uint32_t expected = 0;
  if (!__atomic_compare_exchange_n(&g_pr231_source.writerBlocked, &expected,
                                   1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return -1;
  uint32_t wait;
  for (wait = 0; wait < EJIT_PR231_WAIT_ROUNDS; ++wait) {
    if (__atomic_load_n(&g_pr231_source.readers, __ATOMIC_ACQUIRE) == 0u)
      break;
    pr231_delay(0, 1u);
  }
  if (wait == EJIT_PR231_WAIT_ROUNDS) {
    __atomic_store_n(&g_pr231_source.writerBlocked, 0u, __ATOMIC_RELEASE);
    return -1;
  }
  for (uint32_t cell = 0; cell < PR231_CELLS; ++cell)
    for (uint32_t trp = 0; trp < PR231_TRPS; ++trp) {
      PR231Config *cfg = &g_pr231_config[cell][trp];
      cfg->mode = 1u;
      cfg->cellGain = 3u + cell;
      cfg->trpBias = 5u + trp;
      cfg->jointBias = 7u + 2u * cell + 3u * trp;
      cfg->liveBias = 11u + cell + trp;
      g_pr231_output[cell][trp] = 0;
    }
  __atomic_store_n(&g_pr231_source.epoch, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pr231_source.revision, 1u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pr231_source.writerBlocked, 0u, __ATOMIC_RELEASE);
  return 0;
}

static void pr231_make_request(uint64_t quota, uint64_t generation) {
  memset(&pr231_request, 0, sizeof(pr231_request));
  pr231_request.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  pr231_request.structSize = sizeof(pr231_request);
  strcpy(pr231_request.entryName, "pr231_smalltable_entry");
  strcpy(pr231_request.sourceVarName, "g_pr231_config");
  pr231_request.sourceAddress = (uintptr_t)&g_pr231_config[0][0];
  pr231_request.sourceBytes = sizeof(g_pr231_config);
  pr231_request.sourceState = (uintptr_t)&g_pr231_source;
  pr231_request.aotEntry = (uintptr_t)&pr231_smalltable_entry;
  pr231_request.sourceEpoch = 1u;
  pr231_request.configurationRevision = 1u;
  pr231_request.codeGeneration = generation;
  pr231_request.sampleLimit = quota;
  pr231_request.numDims = 2u;
  pr231_request.numMembers = PR231_CELLS * PR231_TRPS;
  pr231_request.domainCoverage = 1u;
  pr231_request.dims[0].argumentIndex = 0u;
  pr231_request.dims[0].extent = PR231_CELLS;
  strcpy(pr231_request.dims[0].periodName, "pr231_cell");
  pr231_request.dims[1].argumentIndex = 1u;
  pr231_request.dims[1].extent = PR231_TRPS;
  strcpy(pr231_request.dims[1].periodName, "pr231_trp");
  for (uint32_t cell = 0; cell < PR231_CELLS; ++cell)
    for (uint32_t trp = 0; trp < PR231_TRPS; ++trp) {
      ejit_small_table_sre_member_t *m =
          &pr231_request.members[cell * PR231_TRPS + trp];
      m->coordinate[0] = cell;
      m->coordinate[1] = trp;
      m->configurationGeneration = 1u;
      m->fieldsInitialized = 1u;
    }
}

/* Shell ABI deliberately matches the existing period demos. a=0: cold 64;
 * a=1: cold configurable quota 8. b/c/d are reserved and must be zero.
 * Worker command returns immediately. Producer owns the bounded test sequence.
 */
int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  if (a > 1u || b || c || d)
    return -1;
  if (g_ucLocalCoreID != PR231_WORKER && g_ucLocalCoreID != PR231_PRODUCER)
    return -1;
  int rc = pr231_setup();
  if (rc)
    return rc;
  if (g_ucLocalCoreID == PR231_WORKER) {
    uint32_t initial = 0u;
    if (!__atomic_compare_exchange_n(&g_pr231_stage, &initial, 1u, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      SRE_printf("[STAB231] BLOCKED: worker already started; fresh boot per run\n");
      return -20;
    }
    SRE_printf("[STAB231] worker=6 ready; run test_ejit_period on core16\n");
    return 0;
  }
  uint32_t expected = 1u;
  if (!__atomic_compare_exchange_n(&g_pr231_stage, &expected, 2u, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[STAB231] BLOCKED: worker first / fresh boot per run\n");
    return -20;
  }
  const uint64_t quota = a == 1u ? 8u : 64u;
  /* Alias core-private scratch while keeping assertions easy to compare to
   * printed fields. No copied inventory lives on the shell task stack. */
#define s pr231_main_snapshot
#define full pr231_full_snapshot
#define PR231_CHECK(condition, label) do {                                  \
    if (!(condition)) {                                                   \
      SRE_printf("[STAB231] FAIL %s\n", label);                             \
      __atomic_store_n(&g_pr231_stage, 4u, __ATOMIC_RELEASE);               \
      return -1;                                                          \
    }                                                                     \
  } while (0)
  /* Setup validated the actual shared source, state, output and controller
   * ranges on this core before any configuration mutation. */
  PR231_CHECK(pr231_commit_configuration() == 0, "configuration commit");
  for (uint32_t cell = 0; cell < PR231_CELLS; ++cell)
    PR231_CHECK(ejit_activate("pr231_cell", cell) == EJIT_OK, "activate cell");
  for (uint32_t trp = 0; trp < PR231_TRPS; ++trp)
    PR231_CHECK(ejit_activate("pr231_trp", trp) == EJIT_OK, "activate TRP");
  /* Arm the real shared dump filter BEFORE compiling common T1/T2. Merely
   * printing afterward cannot reconstruct IR that was never captured. */
  ejit_dump_func("pr231_smalltable_entry");
  pr231_make_request(quota, 1u);
  PR231_CHECK(ejit_small_table_sre_request(&pr231_request) == EJIT_STAB_SRE_OK,
               "real registered request");
  PR231_CHECK(pr231_snapshot(&s) == 0 && s.tier == 1u && s.sampleCount == 0u &&
               s.admittedMembers == PR231_CELLS * PR231_TRPS &&
               s.expectedCounterPairs != 0u &&
               s.counterPairs == s.expectedCounterPairs &&
               s.workerTaskIdentity == 0u && s.ownerIdentity != 0u &&
               s.ownerWorkerOperations != 0u && s.genericPending == 0u &&
               s.genericAsyncEnqueues == 0u, "cold common T1 / actual counters");
  for (uint32_t n = 0; n < quota; ++n)
    PR231_CHECK(pr231_call(n) == 0, "real generated wrapper value/store");
  PR231_CHECK(pr231_snapshot(&s) == 0 && s.sampleCount == quota &&
               s.counterWordCount != 0u && s.inFlight == 0u &&
               s.physicalExecutions == 0u && s.genericPending == 0u &&
               s.genericAsyncEnqueues == 0u, "quota / real counters / no generic PGO");
  const uint64_t digest = s.countersDigest;
  PR231_CHECK(pr231_call((uint32_t)quota) == 0 && pr231_snapshot(&s) == 0 &&
               s.sampleCount == quota &&
               s.countersDigest == digest, "quota+1 AOT before T2");
  PR231_CHECK(ejit_small_table_sre_finish(pr231_func) == EJIT_STAB_SRE_OK &&
               pr231_snapshot(&full) == 0 && full.tier == 2u &&
               full.fullProfileValid == 1u && full.profileBytes != 0u &&
               full.rootEntryCountValid == 1u && full.rootEntryCount == quota &&
               full.counterPairs == full.expectedCounterPairs &&
               full.publishedSlots == PR231_CELLS * PR231_TRPS,
               "complete profile / common T2 / all slots");
  for (uint32_t n = 0; n < PR231_CELLS * PR231_TRPS; ++n)
    PR231_CHECK(pr231_call(100u + n) == 0, "common T2 members");
  /* Deny a NEW borrow without modifying the old source. No new ticket and no
   * quota consumption are permitted; the correct old AOT value still returns. */
  PR231_CHECK(pr231_snapshot(&s) == 0, "pre-refusal snapshot");
  const uint64_t borrowBaseline = s.borrowReaders;
  __atomic_store_n(&g_pr231_source.writerBlocked, 1u, __ATOMIC_RELEASE);
  rc = pr231_call(220u);
  __atomic_store_n(&g_pr231_source.writerBlocked, 0u, __ATOMIC_RELEASE);
  PR231_CHECK(rc == 0 && pr231_snapshot(&s) == 0 && s.physicalExecutions == 0u &&
               s.borrowReaders == borrowBaseline,
               "borrow refused -> AOT / no physical lease leak");
  __atomic_store_n(&g_pr231_probe_result, -1, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pr231_probe_arm, 1u, __ATOMIC_RELEASE);
  PR231_CHECK(pr231_call(240u) == 0 &&
               __atomic_load_n(&g_pr231_probe_result, __ATOMIC_ACQUIRE) == 0 &&
               pr231_snapshot(&s) == 0 && s.physicalExecutions == 0u &&
               s.retainedExecutions == 0u && s.borrowReaders == 0u,
               "cancel survives until REAL leave / borrow drains");
  pr231_make_request(quota, 2u);
  PR231_CHECK(ejit_small_table_sre_request(&pr231_request) == EJIT_STAB_SRE_OK &&
               pr231_snapshot(&s) == 0 && s.codeGeneration == 2u &&
               s.sampleCount == 0u && pr231_call(260u) == 0 &&
               pr231_snapshot(&s) == 0 && s.sampleCount == 1u &&
               s.counterWordCount != 0u, "replacement uses new real T1");
  for (uint32_t n = 1; n < quota; ++n)
    PR231_CHECK(pr231_call(260u + n) == 0, "replacement common T1 members");
  PR231_CHECK(ejit_small_table_sre_finish(pr231_func) == EJIT_STAB_SRE_OK &&
               pr231_snapshot(&s) == 0 && s.codeGeneration == 2u &&
               s.tier == 2u && s.fullProfileValid == 1u &&
               s.sampleCount == quota && s.rootEntryCountValid == 1u &&
               s.rootEntryCount == quota &&
               s.counterPairs == s.expectedCounterPairs &&
               s.physicalExecutions == 0u && s.borrowReaders == 0u &&
               s.publishedSlots == PR231_CELLS * PR231_TRPS,
               "replacement full profile / final common T2");
  SRE_printf("[STAB231] PASS cold: members=12 samples=%llu pairs=%u words=%u "
             "profile=%llu commonT2=1 generation=2; borrow-refusal/held-cancel/real-leave/replacement OK\n",
             (unsigned long long)quota, full.counterPairs, full.counterWordCount,
             (unsigned long long)full.profileBytes);
  __atomic_store_n(&g_pr231_stage, 3u, __ATOMIC_RELEASE);
  return 0;
#undef PR231_CHECK
#undef s
#undef full
}

int test_ejit_smalltable_print(void) {
  if (g_ucLocalCoreID != PR231_WORKER || !pr231_initialized)
    return -20;
  SRE_printf("[STAB231] stage=%u (3=PASS,4=FAIL; not product acceptance)\n",
             __atomic_load_n(&g_pr231_stage, __ATOMIC_ACQUIRE));
  return ejit_small_table_sre_print(UINT32_MAX);
}
