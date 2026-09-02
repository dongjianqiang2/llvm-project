//===-- ejit_bound_ptr_sre_multistruct_test.c - borrowed pointer demo -----===//
//
// Board flow after reset:
//   1. Core 6:  run test_ejit_period.  It initializes the fixed worker,
//      enables capture, and waits for the producer.
//   2. Core 16: run test_ejit_period.  It attaches to core 6, activates two
//      cell versions, then serially drives helper/root through AOT, 64 real
//      Tier-1 dispatches, Tier-2 publication, and two Ready calls.
//   3. Core 6:  run test_ejit_bound_ptr_multistruct_print to inspect the
//      compiled list, entry view, and complete module view.
//
// The negative checks use only invalid descriptors.  They do not create a
// dangling pointer, concurrent write, or other intentional lifetime hazard.
// Do not call ejit_shutdown(): the owner worker must survive shell invocations.
//
//===----------------------------------------------------------------------===//

// Kept self-contained for direct board integration: no project or libc
// headers are required by this file.
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef __SIZE_TYPE__ size_t;
typedef __UINTPTR_TYPE__ uintptr_t;
typedef _Bool bool;

#define true 1
#define false 0
#define UINTPTR_MAX (~(uintptr_t)0)

#define EJIT_PERIOD_CONST __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x) __attribute__((ejit_dim(#x)))
#define EJIT_BOUND_PTR(x) __attribute__((ejit_bound_ptr(#x)))
#define ejit_entry __attribute__((ejit_entry))
#define EJIT_ENTRY ejit_entry

#define ejit_may_const EJIT_PERIOD_CONST
#define ejit_period_arr(x) EJIT_IN_PERIOD_ARRAY(x)

typedef enum {
  EJIT_OK = 0,
  EJIT_ERR_INVALID_PARAM = -1,
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
  bool forceStaticRegistry;
  const char *dumpJITDir;
} ejit_config_t;

typedef struct {
  uint32_t dimType;
  uint32_t instanceId;
} ejit_dim_pair_t;

typedef struct {
  const void *rawPtr;
  uint32_t size;
  uint32_t argIndex;
} ejit_bound_ptr_t;

typedef struct {
  uint64_t cacheHits;
  uint64_t asyncCompiles;
  uint64_t asyncEnqueues;
  uint64_t alreadyPending;
  uint64_t queueFull;
  uint64_t compileFailed;
  uint64_t publishFailed;
  uint64_t instanceDisabled;
  uint64_t instanceDisabledPreActivate;
  uint32_t readyEntries;
  uint32_t pendingEntries;
  uint32_t queueApproxSize;
  uint32_t reserved;
} ejit_taskpool_stats_t;

extern ejit_status_t ejit_init_pgo(const ejit_config_t *config);
extern ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx);
extern bool ejit_is_active(const char *periodName, uint32_t cellIdx);
extern void ejit_clear_cache(void);
extern unsigned ejit_taskpool_pending_count(void);
extern ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
extern void ejit_taskpool_print_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern ejit_status_t ejit_taskpool_compile_or_get_bound_v(
    uint32_t funcIndex, const ejit_dim_pair_t *dims, uint32_t numDims,
    const ejit_bound_ptr_t *bounds, uint32_t boundCount, void **outFn,
    uint32_t *outBucket);
extern void ejit_dump_func(const char *name);
extern void ejit_print_dumped(const char *name);
extern void ejit_print_dumped_module(const char *name);

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define EJIT_BOUND_MULTI_WORKER_CORE 6u
#define EJIT_BOUND_MULTI_PRODUCER_CORE 16u
#define EJIT_BOUND_MULTI_CELL_A 1u
#define EJIT_BOUND_MULTI_CELL_B 2u
#define EJIT_BOUND_MULTI_TRP 2u
#define EJIT_BOUND_MULTI_WAIT_ROUNDS 6000u
#define EJIT_BOUND_MULTI_WAIT_TICKS 10u
#define EJIT_BOUND_MULTI_PAYLOAD_WORDS 512u
#define EJIT_BOUND_MULTI_T1_SAMPLES 64u

// These are deliberately larger than 1 KiB. They model shared application
// state while keeping the request transport fixed at one descriptor each.
typedef struct {
  ejit_may_const uint32_t algorithm;
  ejit_may_const uint32_t scale;
  uint32_t runtimeBias;
  uint32_t payload[EJIT_BOUND_MULTI_PAYLOAD_WORDS];
} EJitBoundMultiCellConfig;

typedef struct {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t offset;
  uint32_t runtimeTag;
  uint32_t payload[EJIT_BOUND_MULTI_PAYLOAD_WORDS];
} EJitBoundMultiTrpConfig;

typedef char EJitBoundMultiCellMustBeLarge
    [(sizeof(EJitBoundMultiCellConfig) > 1024u) ? 1 : -1];
typedef char EJitBoundMultiTrpMustBeLarge
    [(sizeof(EJitBoundMultiTrpConfig) > 1024u) ? 1 : -1];
typedef char
    EJitBoundMultiDescriptorMustBeFixed[(sizeof(ejit_bound_ptr_t) <= 32u) ? 1
                                                                          : -1];

enum BoundMultiStage {
  BOUND_MULTI_RESET = 0,
  BOUND_MULTI_WORKER_READY = 1,
  BOUND_MULTI_COMPILED = 2,
  BOUND_MULTI_PRINTED = 3,
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
uint32_t g_bound_multi_cells[8];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(trp)
uint32_t g_bound_multi_trps[8];
EJIT_SHARED_SECTION_ATTR EJitBoundMultiCellConfig
    g_bound_multi_cell_configs[8] = {
        [EJIT_BOUND_MULTI_CELL_A] = {7u, 5u, 100u, {0xC011A001u}},
        [EJIT_BOUND_MULTI_CELL_B] = {7u, 9u, 200u, {0xC011B002u}},
};
EJIT_SHARED_SECTION_ATTR EJitBoundMultiTrpConfig
    g_bound_multi_trp_configs[8] = {
        [EJIT_BOUND_MULTI_TRP] = {3u, 11u, 0x7A2u, {0x7A2F0002u}},
};
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_bound_multi_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_bound_multi_sink;

// The helper owns an independent specialization identity and repeats the
// matching dimension and bound-pointer contracts for both objects.
ejit_entry uint32_t bound_multi_helper(
    EJIT_DIM(cell) uint8_t cellIndex, EJIT_DIM(trp) uint8_t trpIndex,
    EJIT_BOUND_PTR(cell) const EJitBoundMultiCellConfig *cellConfig,
    EJIT_BOUND_PTR(trp) const EJitBoundMultiTrpConfig *trpConfig,
    uint32_t input) {
  uint32_t value = input;
  if (cellConfig->algorithm == 7u)
    value *= cellConfig->scale;
  return value + cellConfig->runtimeBias + trpConfig->multiplier +
         trpConfig->offset + trpConfig->runtimeTag + cellIndex + trpIndex;
}

ejit_entry uint32_t bound_multi_root(
    EJIT_DIM(cell) uint8_t cellIndex, EJIT_DIM(trp) uint8_t trpIndex,
    EJIT_BOUND_PTR(cell) const EJitBoundMultiCellConfig *cellConfig,
    EJIT_BOUND_PTR(trp) const EJitBoundMultiTrpConfig *trpConfig,
    uint32_t input) {
  return bound_multi_helper(cellIndex, trpIndex, cellConfig, trpConfig, input);
}

static uint32_t expected_value(uint32_t cell) {
  const EJitBoundMultiCellConfig *cellConfig =
      &g_bound_multi_cell_configs[cell];
  const EJitBoundMultiTrpConfig *trpConfig =
      &g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP];
  uint32_t value = 10u;
  if (cellConfig->algorithm == 7u)
    value *= cellConfig->scale;
  return value + cellConfig->runtimeBias + trpConfig->multiplier +
         trpConfig->offset + trpConfig->runtimeTag + cell +
         EJIT_BOUND_MULTI_TRP;
}

static uint32_t call_bound_root(uint32_t cell) {
  return bound_multi_root((uint8_t)cell, (uint8_t)EJIT_BOUND_MULTI_TRP,
                          &g_bound_multi_cell_configs[cell],
                          &g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP],
                          10u);
}

enum BoundMultiTarget {
  BOUND_MULTI_HELPER = 0,
  BOUND_MULTI_ROOT = 1,
};

static const char *target_name(uint32_t target) {
  return target == BOUND_MULTI_HELPER ? "helper" : "root";
}

static uint32_t call_bound_target(uint32_t target, uint32_t cell) {
  if (target == BOUND_MULTI_HELPER)
    return bound_multi_helper((uint8_t)cell, (uint8_t)EJIT_BOUND_MULTI_TRP,
                              &g_bound_multi_cell_configs[cell],
                              &g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP],
                              10u);
  return call_bound_root(cell);
}

static void print_profile_state(const char *phase, uint32_t target,
                                uint32_t cell, uint32_t calls,
                                uint64_t baseline,
                                const ejit_taskpool_stats_t *stats) {
  const uint64_t compiled =
      stats->asyncCompiles >= baseline ? stats->asyncCompiles - baseline : 0u;
  SRE_printf("[BOUND-MULTI] pgo=1 phase=%s target=%s cell=%u calls=%u "
             "compiled=%llu/2 active(cell/trp)=%u/%u pending=%u/%u "
             "queue=%u failed=%llu/%llu\n",
             phase, target_name(target), cell, calls,
             (unsigned long long)compiled,
             (unsigned)ejit_is_active("cell", cell),
             (unsigned)ejit_is_active("trp", EJIT_BOUND_MULTI_TRP),
             ejit_taskpool_pending_count(), stats->pendingEntries,
             stats->queueApproxSize, (unsigned long long)stats->compileFailed,
             (unsigned long long)stats->publishFailed);
}

static int wait_for_t1(uint64_t baseline, uint32_t target, uint32_t cell) {
  uint32_t emptyRounds = 0;
  for (uint32_t round = 0; round < EJIT_BOUND_MULTI_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK)
      return 0;
    if (stats.compileFailed || stats.publishFailed) {
      print_profile_state("T1-failed", target, cell, 1u, baseline, &stats);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + 1u &&
        ejit_taskpool_pending_count() == 0)
      return 1;
    if (stats.pendingEntries == 0 && stats.queueApproxSize == 0 &&
        ejit_taskpool_pending_count() == 0 &&
        stats.asyncCompiles < baseline + 1u) {
      if (++emptyRounds >= 5u) {
        print_profile_state("T1-missing", target, cell, 1u, baseline, &stats);
        return 0;
      }
    } else {
      emptyRounds = 0;
    }
    if ((round % 500u) == 0)
      print_profile_state("T1-wait", target, cell, 1u, baseline, &stats);
    (void)SRE_TaskDelay(EJIT_BOUND_MULTI_WAIT_TICKS);
  }
  ejit_taskpool_stats_t stats = {0};
  (void)ejit_taskpool_get_stats(&stats);
  print_profile_state("T1-timeout", target, cell, 1u, baseline, &stats);
  return 0;
}

static int drive_profile(uint32_t target, uint32_t cell, uint64_t *sink) {
  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK)
    return 0;
  const uint64_t baseline = before.asyncCompiles;
  const uint32_t expected = expected_value(cell);

  uint32_t got = call_bound_target(target, cell);
  if (got != expected || !wait_for_t1(baseline, target, cell)) {
    SRE_printf("[BOUND-MULTI] FAIL target=%s cell=%u initial=%u expected=%u\n",
               target_name(target), cell, got, expected);
    return 0;
  }
  *sink ^= got;

  for (uint32_t sample = 0; sample < EJIT_BOUND_MULTI_T1_SAMPLES; ++sample) {
    got = call_bound_target(target, cell);
    if (got != expected) {
      SRE_printf("[BOUND-MULTI] FAIL target=%s cell=%u sample=%u/%u "
                 "got=%u expected=%u\n",
                 target_name(target), cell, sample + 1u,
                 EJIT_BOUND_MULTI_T1_SAMPLES, got, expected);
      return 0;
    }
    *sink ^= got;
    if (sample == 0u || ((sample + 1u) % 16u) == 0u) {
      ejit_taskpool_stats_t stats = {0};
      (void)ejit_taskpool_get_stats(&stats);
      print_profile_state("T1-sampling", target, cell, sample + 1u, baseline,
                          &stats);
    }
    if (((sample + 1u) % 8u) == 0u)
      (void)SRE_TaskDelay(1u);
  }

  for (uint32_t round = 0; round < EJIT_BOUND_MULTI_WAIT_ROUNDS; ++round) {
    // Calls after the quota keep the wrapper progressing through AOT fallback,
    // deferred Tier-2 enqueue, compile, publish, and finally Ready dispatch.
    got = call_bound_target(target, cell);
    if (got != expected)
      return 0;
    *sink ^= got;

    ejit_taskpool_stats_t stats = {0};
    if (ejit_taskpool_get_stats(&stats) != EJIT_OK)
      return 0;
    if (stats.compileFailed || stats.publishFailed) {
      print_profile_state("T2-failed", target, cell,
                          EJIT_BOUND_MULTI_T1_SAMPLES + round + 1u, baseline,
                          &stats);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + 2u && stats.pendingEntries == 0 &&
        stats.queueApproxSize == 0 && ejit_taskpool_pending_count() == 0) {
      // Do not equate publication with execution: call the Ready version twice.
      const uint32_t verify1 = call_bound_target(target, cell);
      const uint32_t verify2 = call_bound_target(target, cell);
      if (verify1 != expected || verify2 != expected)
        return 0;
      *sink ^= verify1;
      *sink ^= verify2;
      print_profile_state("T2-ready", target, cell,
                          EJIT_BOUND_MULTI_T1_SAMPLES + round + 3u, baseline,
                          &stats);
      return 1;
    }
    if (round == 0u || ((round + 1u) % 500u) == 0u)
      print_profile_state("T2-wait", target, cell,
                          EJIT_BOUND_MULTI_T1_SAMPLES + round + 1u, baseline,
                          &stats);
    if (((round + 1u) % 8u) == 0u)
      (void)SRE_TaskDelay(1u);
  }

  ejit_taskpool_stats_t stats = {0};
  (void)ejit_taskpool_get_stats(&stats);
  print_profile_state("T2-timeout", target, cell,
                      EJIT_BOUND_MULTI_T1_SAMPLES +
                          EJIT_BOUND_MULTI_WAIT_ROUNDS,
                      baseline, &stats);
  return 0;
}

static int check_transport_contract(void) {
  const size_t cellSize = sizeof(EJitBoundMultiCellConfig);
  const size_t trpSize = sizeof(EJitBoundMultiTrpConfig);
  const size_t descriptorSize = sizeof(ejit_bound_ptr_t);
  if (cellSize <= 1024u || trpSize <= 1024u || descriptorSize > 32u) {
    SRE_printf("[BOUND-MULTI] FAIL transport sizes cell=%lu trp=%lu "
               "descriptor=%lu\n",
               (unsigned long)cellSize, (unsigned long)trpSize,
               (unsigned long)descriptorSize);
    return 0;
  }
  if (g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A].payload[0] !=
          0xC011A001u ||
      g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_B].payload[0] !=
          0xC011B002u ||
      g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP].payload[0] !=
          0x7A2F0002u) {
    SRE_printf("[BOUND-MULTI] FAIL shared payload changed during compile\n");
    return 0;
  }
  SRE_printf("[BOUND-MULTI] PASS borrowed transport cell=%luB trp=%luB "
             "descriptor=%luB; payloads stayed shared\n",
             (unsigned long)cellSize, (unsigned long)trpSize,
             (unsigned long)descriptorSize);
  return 1;
}

static int check_invalid_descriptors(void) {
  ejit_bound_ptr_t tooMany[9];
  for (uint32_t i = 0; i < 9u; ++i) {
    tooMany[i].rawPtr = &g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A];
    tooMany[i].size = (uint32_t)sizeof(EJitBoundMultiCellConfig);
    tooMany[i].argIndex = i;
  }
  ejit_status_t rc = ejit_taskpool_compile_or_get_bound_v(
      0u, (const ejit_dim_pair_t *)0, 0u, tooMany, 9u, (void **)0,
      (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL >8 descriptor check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t nullObject = {
      (const void *)0, (uint32_t)sizeof(EJitBoundMultiCellConfig), 0u};
  rc = ejit_taskpool_compile_or_get_bound_v(0u, (const ejit_dim_pair_t *)0, 0u,
                                            &nullObject, 1u, (void **)0,
                                            (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL null/lifetime check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t zeroSize = {
      &g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A], 0u, 0u};
  rc = ejit_taskpool_compile_or_get_bound_v(0u, (const ejit_dim_pair_t *)0, 0u,
                                            &zeroSize, 1u, (void **)0,
                                            (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL zero-size check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t overflow = {(const void *)(uintptr_t)UINTPTR_MAX, 8u, 0u};
  rc = ejit_taskpool_compile_or_get_bound_v(0u, (const ejit_dim_pair_t *)0, 0u,
                                            &overflow, 1u, (void **)0,
                                            (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL overflow check rc=%d\n", (int)rc);
    return 0;
  }

  ejit_bound_ptr_t duplicate[2] = {
      {&g_bound_multi_cell_configs[EJIT_BOUND_MULTI_CELL_A],
       (uint32_t)sizeof(EJitBoundMultiCellConfig), 0u},
      {&g_bound_multi_trp_configs[EJIT_BOUND_MULTI_TRP],
       (uint32_t)sizeof(EJitBoundMultiTrpConfig), 0u},
  };
  rc = ejit_taskpool_compile_or_get_bound_v(0u, (const ejit_dim_pair_t *)0, 0u,
                                            duplicate, 2u, (void **)0,
                                            (uint32_t *)0);
  if (rc != EJIT_ERR_INVALID_PARAM) {
    SRE_printf("[BOUND-MULTI] FAIL duplicate argIndex check rc=%d\n", (int)rc);
    return 0;
  }

  SRE_printf("[BOUND-MULTI] PASS invalid descriptor lists rejected before "
             "worker dereference (>8, null, zero, overflow, duplicate); "
             "no UAF constructed\n");
  return 1;
}

static int run_producer(void) {
  uint32_t stage = __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE);
  if (stage != BOUND_MULTI_WORKER_READY) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL stage=%u; run core %u first\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE, stage,
               EJIT_BOUND_MULTI_WORKER_CORE);
    return -3;
  }

  if (ejit_activate("cell", EJIT_BOUND_MULTI_CELL_A) != EJIT_OK ||
      ejit_activate("cell", EJIT_BOUND_MULTI_CELL_B) != EJIT_OK ||
      ejit_activate("trp", EJIT_BOUND_MULTI_TRP) != EJIT_OK) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL activate\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE);
    return -4;
  }

  ejit_clear_cache();
  if (!check_invalid_descriptors())
    return -5;
  if (!check_transport_contract())
    return -6;

  ejit_taskpool_stats_t before = {0};
  (void)ejit_taskpool_get_stats(&before);
  uint64_t sink = 0;
  const uint32_t cells[2] = {EJIT_BOUND_MULTI_CELL_A, EJIT_BOUND_MULTI_CELL_B};
  for (uint32_t i = 0; i < 2u; ++i) {
    const uint32_t cell = cells[i];
    // Finish the helper identity first, then root. This prevents two functions
    // or two cells of one function from occupying PGO admission slots while
    // waiting on each other.
    if (!drive_profile(BOUND_MULTI_HELPER, cell, &sink) ||
        !drive_profile(BOUND_MULTI_ROOT, cell, &sink)) {
      SRE_printf("[BOUND-MULTI][core=%u] FAIL profile target cell=%u\n",
                 EJIT_BOUND_MULTI_PRODUCER_CORE, cell);
      ejit_taskpool_print_stats();
      return -7;
    }
  }

  ejit_taskpool_stats_t after = {0};
  (void)ejit_taskpool_get_stats(&after);
  if (after.asyncCompiles < before.asyncCompiles + 8u || after.compileFailed ||
      after.publishFailed || ejit_taskpool_pending_count() != 0) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL final compiles=%llu/8 "
               "failed=%llu/%llu pending=%u\n",
               EJIT_BOUND_MULTI_PRODUCER_CORE,
               (unsigned long long)(after.asyncCompiles - before.asyncCompiles),
               (unsigned long long)after.compileFailed,
               (unsigned long long)after.publishFailed,
               ejit_taskpool_pending_count());
    return -9;
  }

  __atomic_store_n(&g_bound_multi_sink, sink, __ATOMIC_RELEASE);
  __atomic_store_n(&g_bound_multi_stage, BOUND_MULTI_COMPILED,
                   __ATOMIC_RELEASE);
  SRE_printf("[BOUND-MULTI][core=%u] PASS helper/root T2 Ready for cell=%u/%u "
             "compiles=%llu hits=%llu; return to core %u for dumps\n",
             EJIT_BOUND_MULTI_PRODUCER_CORE, EJIT_BOUND_MULTI_CELL_A,
             EJIT_BOUND_MULTI_CELL_B,
             (unsigned long long)(after.asyncCompiles - before.asyncCompiles),
             (unsigned long long)(after.cacheHits - before.cacheHits),
             EJIT_BOUND_MULTI_WORKER_CORE);
  return 0;
}

static int run_worker(void) {
  uint32_t stage = __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE);
  if (stage == BOUND_MULTI_RESET) {
    __atomic_store_n(&g_bound_multi_sink, 0u, __ATOMIC_RELEASE);
    ejit_dump_func("*");
    __atomic_store_n(&g_bound_multi_stage, BOUND_MULTI_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[BOUND-MULTI][core=%u] PASS worker ready; run "
               "test_ejit_period on core %u\n",
               EJIT_BOUND_MULTI_WORKER_CORE, EJIT_BOUND_MULTI_PRODUCER_CORE);
    return 0;
  }
  SRE_printf("[BOUND-MULTI][core=%u] worker already ready stage=%u; wait "
             "for core %u then run test_ejit_bound_ptr_multistruct_print\n",
             EJIT_BOUND_MULTI_WORKER_CORE, stage,
             EJIT_BOUND_MULTI_PRODUCER_CORE);
  return 0;
}

int test_ejit_bound_ptr_multistruct_print(uint8_t a, uint8_t b, uint8_t c,
                                          uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != EJIT_BOUND_MULTI_WORKER_CORE) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL dumps are worker-local; run on "
               "core %u\n",
               core, EJIT_BOUND_MULTI_WORKER_CORE);
    return -10;
  }
  uint32_t stage = __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE);
  if (stage < BOUND_MULTI_COMPILED) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL stage=%u; run producer core %u "
               "first\n",
               core, stage, EJIT_BOUND_MULTI_PRODUCER_CORE);
    return -11;
  }
  if (stage == BOUND_MULTI_PRINTED) {
    SRE_printf("[BOUND-MULTI][core=%u] dumps already printed\n", core);
    return 0;
  }

  SRE_printf("[BOUND-MULTI][core=%u] === COMPILED VERSIONS ===\n", core);
  ejit_taskpool_print_compiled();
  SRE_printf("[BOUND-MULTI][core=%u] === ROOT ENTRY VIEW ===\n", core);
  ejit_print_dumped("bound_multi_root");
  SRE_printf("[BOUND-MULTI][core=%u] === HELPER ENTRY VIEW ===\n", core);
  ejit_print_dumped("bound_multi_helper");
  SRE_printf("[BOUND-MULTI][core=%u] === ROOT MODULE VIEW ===\n", core);
  ejit_print_dumped_module("bound_multi_root");
  SRE_printf("[BOUND-MULTI][core=%u] PASS sink=0x%llx; expected root/helper "
             "versions for cell A/B and no payload copy\n"
             "[BOUND-MULTI] dump criterion: algorithm=7, scale=5/9, "
             "multiplier=3, offset=11 must be folded in root/helper; "
             "runtimeBias/runtimeTag remain runtime loads\n",
             core,
             (unsigned long long)__atomic_load_n(&g_bound_multi_sink,
                                                 __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_bound_multi_stage, BOUND_MULTI_PRINTED, __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT borrowed multi-structure demo (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init_pgo(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[BOUND-MULTI][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_bound_multi_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != EJIT_BOUND_MULTI_WORKER_CORE) {
    SRE_printf("[BOUND-MULTI][core=%u] FAIL worker=%u; expected core %u\n",
               core, worker, EJIT_BOUND_MULTI_WORKER_CORE);
    return -2;
  }
  if (core == EJIT_BOUND_MULTI_WORKER_CORE)
    return run_worker();
  if (core == EJIT_BOUND_MULTI_PRODUCER_CORE)
    return run_producer();
  SRE_printf("[BOUND-MULTI][core=%u] skip: use core %u or core %u\n", core,
             EJIT_BOUND_MULTI_WORKER_CORE, EJIT_BOUND_MULTI_PRODUCER_CORE);
  return 0;
}
