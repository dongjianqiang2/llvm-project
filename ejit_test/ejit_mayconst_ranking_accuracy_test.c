// Board flow after reset:
//   core 6:  test_ejit_mayconst_accuracy
//   core 16: test_ejit_mayconst_accuracy
//   core 16: test_ejit_mayconst_accuracy_rank
//
// The owner remains alive between commands. No dump or manual code publish is
// required. Build with branch audit, PGO, shared taskpool, and max-active=4.

#include <stdbool.h>
#include <stdint.h>

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#define MC_WORKER_CORE 6u
#define MC_PRODUCER_CORE 16u
#define MC_ENTRY_COUNT 5u
#define MC_SAMPLE_COUNT 64u
#define MC_WAIT_ROUNDS 12000u
#define MC_SHARED __attribute__((section(".mc_shared")))

struct McAccuracyConfig {
  ejit_may_const uint32_t multiplier;
  ejit_may_const uint32_t bias;
  ejit_may_const uint32_t mask;
  uint32_t salt;
};

MC_SHARED ejit_period_arr(cell)
struct McAccuracyConfig g_mc_accuracy_cfg[3];
MC_SHARED volatile uint32_t g_mc_accuracy_initialized;
MC_SHARED volatile uint32_t g_mc_accuracy_done;
MC_SHARED volatile uint64_t g_mc_accuracy_sink;

static bool g_mc_accuracy_ctors_done;

#define MC_ENTRY_BODY(CELL, SEED, TAG)                                         \
  do {                                                                         \
    const struct McAccuracyConfig *_cfg = &g_mc_accuracy_cfg[(CELL)];          \
    uint32_t _value = (SEED) ^ _cfg->salt ^ (TAG);                             \
    _value = _value * _cfg->multiplier + _cfg->bias;                           \
    if ((_value & _cfg->mask) == _cfg->bias)                                   \
      _value ^= 0x9e3779b9u;                                                   \
    return _value;                                                             \
  } while (0)

ejit_entry __attribute__((noinline)) uint32_t
mc_accuracy_entry0(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  MC_ENTRY_BODY(cell, seed, 0x101u);
}
ejit_entry __attribute__((noinline)) uint32_t
mc_accuracy_entry1(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  MC_ENTRY_BODY(cell, seed, 0x202u);
}
ejit_entry __attribute__((noinline)) uint32_t
mc_accuracy_entry2(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  MC_ENTRY_BODY(cell, seed, 0x303u);
}
ejit_entry __attribute__((noinline)) uint32_t
mc_accuracy_entry3(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  MC_ENTRY_BODY(cell, seed, 0x404u);
}
ejit_entry __attribute__((noinline)) uint32_t
mc_accuracy_entry4(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  MC_ENTRY_BODY(cell, seed, 0x505u);
}

static uint32_t mc_accuracy_call(uint32_t entry, uint8_t cell, uint32_t seed) {
  switch (entry) {
  case 0:
    return mc_accuracy_entry0(cell, seed);
  case 1:
    return mc_accuracy_entry1(cell, seed);
  case 2:
    return mc_accuracy_entry2(cell, seed);
  case 3:
    return mc_accuracy_entry3(cell, seed);
  default:
    return mc_accuracy_entry4(cell, seed);
  }
}

static uint32_t mc_accuracy_expected(uint32_t entry, uint8_t cell,
                                     uint32_t seed) {
  static const uint32_t tags[MC_ENTRY_COUNT] = {0x101u, 0x202u, 0x303u, 0x404u,
                                                0x505u};
  const struct McAccuracyConfig *cfg = &g_mc_accuracy_cfg[cell];
  uint32_t value = seed ^ cfg->salt ^ tags[entry];
  value = value * cfg->multiplier + cfg->bias;
  if ((value & cfg->mask) == cfg->bias)
    value ^= 0x9e3779b9u;
  return value;
}

static void mc_accuracy_init_shared(void) {
  if (__atomic_load_n(&g_mc_accuracy_initialized, __ATOMIC_ACQUIRE))
    return;
  for (uint32_t cell = 0; cell != 3; ++cell) {
    g_mc_accuracy_cfg[cell].multiplier = 3u + cell * 2u;
    g_mc_accuracy_cfg[cell].bias = 1u + cell;
    g_mc_accuracy_cfg[cell].mask = 7u;
    g_mc_accuracy_cfg[cell].salt = 0x13579bdu + cell * 0x10203u;
  }
  __atomic_store_n(&g_mc_accuracy_done, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_mc_accuracy_sink, 0u, __ATOMIC_RELAXED);
  __atomic_store_n(&g_mc_accuracy_initialized, 1u, __ATOMIC_RELEASE);
}

static ejit_status_t mc_accuracy_init(void) {
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;
  return ejit_init_pgo(&cfg);
}

static bool mc_accuracy_stats(ejit_taskpool_stats_t *stats) {
  *stats = (ejit_taskpool_stats_t){0};
  return ejit_taskpool_get_stats(stats) == EJIT_OK;
}

static bool mc_accuracy_wait_delta(uint64_t baseline, uint64_t delta,
                                   const char *phase) {
  for (uint32_t round = 0; round != MC_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats;
    if (!mc_accuracy_stats(&stats) || stats.compileFailed ||
        stats.publishFailed)
      return false;
    if (stats.asyncCompiles >= baseline + delta &&
        ejit_taskpool_pending_count() == 0)
      return true;
    if ((round % 1000u) == 0)
      SRE_printf("[MC-ACCURACY] waiting %s compiles=%llu/%llu pending=%u\n",
                 phase, (unsigned long long)(stats.asyncCompiles - baseline),
                 (unsigned long long)delta, ejit_taskpool_pending_count());
    (void)SRE_TaskDelay(10u);
  }
  SRE_printf("[MC-ACCURACY] FAIL timeout phase=%s\n", phase);
  ejit_taskpool_print_stats();
  return false;
}

static int mc_accuracy_run_five_entries(void) {
  ejit_taskpool_stats_t initial;
  if (!mc_accuracy_stats(&initial))
    return -10;

  // Four entries may profile immediately. The fifth repeatedly follows AOT
  // until a slot is released, proving max-active handoff without a stall.
  for (uint32_t round = 0; round != MC_WAIT_ROUNDS; ++round) {
    for (uint32_t entry = 0; entry != MC_ENTRY_COUNT; ++entry) {
      const uint32_t seed = 0x10000u + round * 17u + entry;
      const uint32_t got = mc_accuracy_call(entry, 0, seed);
      if (got != mc_accuracy_expected(entry, 0, seed))
        return -11;
      __atomic_fetch_xor(&g_mc_accuracy_sink, got, __ATOMIC_RELAXED);
    }
    ejit_taskpool_stats_t stats;
    if (!mc_accuracy_stats(&stats) || stats.compileFailed ||
        stats.publishFailed)
      return -12;
    if (stats.asyncCompiles >= initial.asyncCompiles + MC_ENTRY_COUNT * 2u &&
        ejit_taskpool_pending_count() == 0)
      return 0;
    (void)SRE_TaskDelay(1u);
  }
  return -13;
}

static int mc_accuracy_run_serial_version(uint8_t cell) {
  ejit_taskpool_stats_t initial;
  if (!mc_accuracy_stats(&initial))
    return -20;

  uint32_t seed = 0x20000u + cell;
  if (mc_accuracy_call(0, cell, seed) != mc_accuracy_expected(0, cell, seed))
    return -21;
  if (!mc_accuracy_wait_delta(initial.asyncCompiles, 1u, "serial Tier-1"))
    return -22;

  for (uint32_t sample = 0; sample != MC_SAMPLE_COUNT; ++sample) {
    seed = 0x30000u + cell * 0x1000u + sample;
    if (mc_accuracy_call(0, cell, seed) != mc_accuracy_expected(0, cell, seed))
      return -23;
  }
  if (!mc_accuracy_wait_delta(initial.asyncCompiles, 2u, "serial Tier-2"))
    return -24;
  return 0;
}

int test_ejit_mayconst_accuracy(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (!g_mc_accuracy_ctors_done) {
    call_init_array_functions();
    g_mc_accuracy_ctors_done = true;
  }
  if (core == MC_WORKER_CORE)
    mc_accuracy_init_shared();
  else if (!__atomic_load_n(&g_mc_accuracy_initialized, __ATOMIC_ACQUIRE))
    return -1;

  if (mc_accuracy_init() != EJIT_OK)
    return -2;
  if (ejit_taskpool_get_worker_core() != MC_WORKER_CORE)
    return -3;
  if (core == MC_WORKER_CORE) {
    SRE_printf("[MC-ACCURACY] worker ready; run on core %u\n",
               MC_PRODUCER_CORE);
    return 0;
  }
  if (core != MC_PRODUCER_CORE)
    return -4;

  for (uint32_t cell = 0; cell != 3; ++cell)
    if (ejit_activate("cell", cell) != EJIT_OK)
      return -5;
  int result = mc_accuracy_run_five_entries();
  if (result != 0)
    return result;
  for (uint8_t cell = 1; cell != 3; ++cell) {
    result = mc_accuracy_run_serial_version(cell);
    if (result != 0)
      return result;
  }

  __atomic_store_n(&g_mc_accuracy_done, 1u, __ATOMIC_RELEASE);
  SRE_printf("[MC-ACCURACY] PASS; run test_ejit_mayconst_accuracy_rank on "
             "core %u\n",
             MC_PRODUCER_CORE);
  return 0;
}

int test_ejit_mayconst_accuracy_rank(uint8_t a, uint8_t b, uint8_t c,
                                     uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (!__atomic_load_n(&g_mc_accuracy_done, __ATOMIC_ACQUIRE))
    return -1;
  ejit_print_mayconst_ranking();
  return 0;
}
