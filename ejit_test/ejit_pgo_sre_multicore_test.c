//===-- ejit_pgo_sre_multicore_test.c - SRE multicore PGO smoke test -----===//
//
// Board flow (matching the original staged PGO benchmark):
//   1. Reset the board and run test_ejit_period on core 8. It initializes EJIT,
//      starts the single shared worker, prints "worker ready", and returns.
//   2. From three core sessions, run test_ejit_period on cores 18, 19 and 20.
//      They attach to core 8's shared taskpool and wait at a shared start gate.
//   3. The three cores then exercise three distinct ejit_entry functions. With
//      EJIT_SRE_PGO_MAX_CONCURRENT_PROFILES=2, two functions profile while the
//      third stays on AOT; it receives a slot after either Tier-2 completes.
//
// Do not call ejit_shutdown(): the owner worker and attached facades must stay
// alive across the per-core shell invocations.
//
//===----------------------------------------------------------------------===//

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ejit_test_helpers.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#ifndef PGO_WORKER_CORE
#define PGO_WORKER_CORE 8u
#endif
#define PGO_PRODUCER_COUNT 3u
#define PGO_EXPECTED_COMPILES (PGO_PRODUCER_COUNT * 2u)
#define PGO_WAIT_ROUNDS 6000u
#define PGO_WAIT_TICKS 10u

static const uint32_t kProducerCores[PGO_PRODUCER_COUNT] = {18u, 19u, 20u};

struct PgoMcCfg {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t mode;
  uint32_t salt;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct PgoMcCfg g_pgo_mc_cfg[PGO_PRODUCER_COUNT];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_mc_ready_mask;
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_mc_done_mask;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_pgo_mc_sink;

#define PGO_MC_BODY(CELL, SEED, TAG)                                           \
  do {                                                                         \
    const struct PgoMcCfg *_cfg = &g_pgo_mc_cfg[(CELL)];                       \
    uint32_t _v = (SEED) ^ _cfg->salt ^ (TAG);                                \
    for (uint32_t _i = 0; _i < 64u; ++_i) {                                   \
      if (((_v + _i) & 7u) < _cfg->mode)                                      \
        _v = (_v * _cfg->multiplier) ^ (_v >> 7) ^ _i;                        \
      else                                                                      \
        _v = (_v + 0x9e3779b9u) ^ (_v << 5);                                  \
    }                                                                          \
    return _v;                                                                 \
  } while (0)

ejit_entry uint32_t
pgo_mc_func0(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  PGO_MC_BODY(cell, seed, 0x101u);
}

ejit_entry uint32_t
pgo_mc_func1(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  PGO_MC_BODY(cell, seed, 0x202u);
}

ejit_entry uint32_t
pgo_mc_func2(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  PGO_MC_BODY(cell, seed, 0x303u);
}

static uint32_t run_lane(uint32_t lane, uint32_t seed) {
  switch (lane) {
  case 0:
    return pgo_mc_func0(0, seed);
  case 1:
    return pgo_mc_func1(1, seed);
  default:
    return pgo_mc_func2(2, seed);
  }
}

static uint32_t expected_lane(uint32_t lane, uint32_t seed) {
  const struct PgoMcCfg *cfg = &g_pgo_mc_cfg[lane];
  const uint32_t tags[PGO_PRODUCER_COUNT] = {0x101u, 0x202u, 0x303u};
  uint32_t v = seed ^ cfg->salt ^ tags[lane];
  for (uint32_t i = 0; i < 64u; ++i) {
    if (((v + i) & 7u) < cfg->mode)
      v = (v * cfg->multiplier) ^ (v >> 7) ^ i;
    else
      v = (v + 0x9e3779b9u) ^ (v << 5);
  }
  return v;
}

static int producer_lane(uint32_t core) {
  for (uint32_t i = 0; i < PGO_PRODUCER_COUNT; ++i)
    if (kProducerCores[i] == core)
      return (int)i;
  return -1;
}

static void init_shared_test_data(void) {
  for (uint32_t i = 0; i < PGO_PRODUCER_COUNT; ++i) {
    g_pgo_mc_cfg[i].multiplier = 5u + i * 2u;
    g_pgo_mc_cfg[i].mode = 6u;
    g_pgo_mc_cfg[i].salt = 0x13579bdu + i * 0x10203u;
  }
  __atomic_store_n(&g_pgo_mc_ready_mask, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_mc_done_mask, 0u, __ATOMIC_RELEASE);
  __atomic_store_n(&g_pgo_mc_sink, 0u, __ATOMIC_RELEASE);
}

static bool wait_for_mask(volatile uint32_t *word, uint32_t target,
                          const char *what, uint32_t core) {
  for (uint32_t round = 0; round < PGO_WAIT_ROUNDS; ++round) {
    uint32_t value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
    if ((value & target) == target)
      return true;
    if ((round % 500u) == 0)
      SRE_printf("[PGO-MC][core=%u] waiting %s mask=0x%x/0x%x\n", core,
                 what, value, target);
    (void)SRE_TaskDelay(PGO_WAIT_TICKS);
  }
  SRE_printf("[PGO-MC][core=%u] FAIL: timeout waiting %s\n", core, what);
  return false;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT concurrent online-PGO test (core=%u) ===\n", core);
  call_init_array_functions();

  if (core == PGO_WORKER_CORE)
    init_shared_test_data();

  ejit_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init_pgo(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[PGO-MC][core=%u] init rc=%d worker=%u\n", core, (int)rc,
             worker);
  if (rc != EJIT_OK)
    return -1;
  if (worker != PGO_WORKER_CORE) {
    SRE_printf("[PGO-MC][core=%u] FAIL: worker=%u; reset and run core %u "
               "first\n",
               core, worker, PGO_WORKER_CORE);
    return -2;
  }

  // Match the original benchmark: the first invocation only starts the owner
  // worker, then returns to leave its shell available for diagnostics.
  if (core == worker) {
    SRE_printf("[PGO-MC][core=%u] worker ready; run test_ejit_period on cores "
               "%u, %u and %u\n",
               core, kProducerCores[0], kProducerCores[1], kProducerCores[2]);
    return 0;
  }

  int laneValue = producer_lane(core);
  if (laneValue < 0) {
    SRE_printf("[PGO-MC][core=%u] skip: expected producer core %u/%u/%u\n",
               core, kProducerCores[0], kProducerCores[1], kProducerCores[2]);
    return -3;
  }
  uint32_t lane = (uint32_t)laneValue;
  const uint32_t allMask = (1u << PGO_PRODUCER_COUNT) - 1u;

  if (ejit_activate("cell", (uint8_t)lane) != EJIT_OK)
    return -4;
  uint32_t ready = __atomic_fetch_or(&g_pgo_mc_ready_mask, 1u << lane,
                                    __ATOMIC_ACQ_REL) |
                   (1u << lane);
  SRE_printf("[PGO-MC][core=%u] lane=%u attached ready=0x%x/0x%x\n", core,
             lane, ready, allMask);
  if (!wait_for_mask(&g_pgo_mc_ready_mask, allMask, "producer start", core))
    return -5;

  // All three functions keep calling. Two receive the configured profiling
  // slots immediately; the third keeps executing AOT until a Tier-2 publish
  // releases one. INFO logs show 0/64 ... 64/64 for each admitted function.
  bool complete = false;
  for (uint32_t round = 0; round < PGO_WAIT_ROUNDS; ++round) {
    uint32_t seed = (lane + 1u) * 0x10000u + round;
    uint32_t got = run_lane(lane, seed);
    uint32_t expected = expected_lane(lane, seed);
    __atomic_fetch_xor(&g_pgo_mc_sink, got, __ATOMIC_RELAXED);
    if (got != expected) {
      SRE_printf("[PGO-MC][core=%u] FAIL lane=%u round=%u got=0x%x "
                 "expected=0x%x\n",
                 core, lane, round, got, expected);
      return -6;
    }

    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ejit_taskpool_get_stats(&stats);
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[PGO-MC][core=%u] FAIL compile=%llu publish=%llu\n", core,
                 (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return -7;
    }
    if (stats.asyncCompiles >= PGO_EXPECTED_COMPILES &&
        ejit_taskpool_pending_count() == 0) {
      complete = true;
      break;
    }
    (void)SRE_TaskDelay(1u);
  }
  if (!complete) {
    SRE_printf("[PGO-MC][core=%u] FAIL: Tier-2 completion timeout\n", core);
    ejit_taskpool_print_stats();
    return -8;
  }

  __atomic_fetch_or(&g_pgo_mc_done_mask, 1u << lane, __ATOMIC_ACQ_REL);
  if (!wait_for_mask(&g_pgo_mc_done_mask, allMask, "producer completion",
                     core))
    return -9;

  ejit_taskpool_stats_t stats;
  memset(&stats, 0, sizeof(stats));
  ejit_taskpool_get_stats(&stats);
  SRE_printf("[PGO-MC][core=%u] PASS lane=%u compiles=%llu hits=%llu "
             "enqueues=%llu sink=0x%llx\n",
             core, lane, (unsigned long long)stats.asyncCompiles,
             (unsigned long long)stats.cacheHits,
             (unsigned long long)stats.asyncEnqueues,
             (unsigned long long)__atomic_load_n(&g_pgo_mc_sink,
                                                  __ATOMIC_RELAXED));
  return 0;
}
