/* PR231 SRE board fixture. Link this instead of other test_ejit_period demos.
 *
 * Link with ejit_smalltable_sre_business.c compiled separately by patched EJIT
 * clang. Do not LTO the controller into entry bitcode. No handwritten JIT.
 * Configuration and output must be mapped coherent/shared at the same VA on
 * both cores by the product linker. A .mc_shared name alone is not proof.
 */
#include "ejit_smalltable_sre_fixture.h"
#include "llvm/ExecutionEngine/EJIT/EJitSmallTableSreBridge.h"
#include <string.h>

/* The SDK integration header must implement the two documented bindings.
 * Missing SDK bindings are a visible BLOCKED, never a simulated task ID or a
 * successful no-op data permission check. There are no weak undefined hooks.
 */
#ifdef EJIT_PR231_PLATFORM_HEADER
#include EJIT_PR231_PLATFORM_HEADER
#endif
#if defined(EJIT_PR231_CURRENT_TASK_ID) && defined(EJIT_PR231_PREPARE_SHARED_DATA)
#define PR231_HAVE_PLATFORM 1
#else
#define PR231_HAVE_PLATFORM 0
#endif
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

static uint64_t pr231_task(void *unused) {
  (void)unused;
#if PR231_HAVE_PLATFORM
  return EJIT_PR231_CURRENT_TASK_ID();
#else
  return 0;
#endif
}
static void pr231_delay(void *unused, uint32_t ticks) {
  (void)unused;
  (void)SRE_TaskDelay(ticks);
}
static int pr231_data(void *unused, uintptr_t address, uint64_t bytes,
                      uint32_t access) {
  (void)unused;
#if PR231_HAVE_PLATFORM
  return EJIT_PR231_PREPARE_SHARED_DATA(address, bytes, access);
#else
  (void)address;
  (void)bytes;
  (void)access;
  return -1;
#endif
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
  if (!PR231_HAVE_PLATFORM) {
    SRE_printf("[STAB231] BLOCKED: SDK task-id/shared-data bindings missing\n");
    return -20;
  }
  if (pr231_initialized)
    return 0;
#if EJIT_PR231_CALL_INIT_ARRAY
  SRE_printf("[STAB231][core=%u] init-array begin (once)\n", g_ucLocalCoreID);
  call_init_array_functions();
  SRE_printf("[STAB231][core=%u] init-array done\n", g_ucLocalCoreID);
#endif
  if (pr231_task(0) == 0u) {
    SRE_printf("[STAB231] BLOCKED: no actual task identity\n");
    return -20;
  }
  ejit_small_table_sre_bindings_t bindings;
  memset(&bindings, 0, sizeof(bindings));
  bindings.abiVersion = EJIT_STAB_SRE_ABI_VERSION;
  bindings.structSize = sizeof(bindings);
  bindings.flags = g_ucLocalCoreID == PR231_WORKER
                       ? EJIT_STAB_SRE_ENABLE_FIXED_DOMAIN : 0u;
  bindings.waitRounds = EJIT_PR231_WAIT_ROUNDS;
  bindings.current_task_id = pr231_task;
  bindings.delay_ticks = pr231_delay;
  bindings.prepare_shared_data = pr231_data;
  int rc = ejit_small_table_sre_prepare(&bindings);
  if (rc != EJIT_STAB_SRE_OK) {
    SRE_printf("[STAB231] prepare BLOCKED rc=%d (fresh boot required)\n", rc);
    return -20;
  }
  /* The hook is defined in this object and registered identically on BOTH
   * cores, before init. No forged profile-runtime or task-self symbols. */
  ejit_register_symbol("pr231_probe_inflight", (void *)&pr231_probe_inflight);
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
  PR231_CHECK(pr231_data(0, (uintptr_t)&g_pr231_config, sizeof(g_pr231_config),
                         EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE) == 0 &&
               pr231_data(0, (uintptr_t)&g_pr231_source, sizeof(g_pr231_source),
                          EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE) == 0 &&
               pr231_data(0, (uintptr_t)&g_pr231_output, sizeof(g_pr231_output),
                          EJIT_STAB_SRE_DATA_READ | EJIT_STAB_SRE_DATA_WRITE) == 0,
               "shared configuration permissions");
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
               s.workerTaskIdentity != 0u && s.genericPending == 0u &&
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
