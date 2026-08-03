//===-- ejit_pgo_staged_benchmark.c - staged SRE online-PGO benchmark ----===//
//
// A board-oriented, single-file benchmark for the complete online-PGO cycle:
//
//   Stage 0: direct AOT baseline (same source body, no EJIT wrapper).
//   Stage 1: Tier-1 instrumented JIT, measured below the 64-hit trigger.
//   Stage 2: cross the trigger and wait for the owner worker to publish Tier-2.
//   Stage 3: Tier-2 steady state (also exercises -ejit-inline-cache).
//
// The workload retains a strongly biased data-dependent branch after may_const
// specialization. Tier-1 records its real edge counts; Tier-2 can feed them to
// MachineBlockPlacement. Even when the baseline layout is already favorable,
// Tier-2 removes Tier-1 instrumentation, so Stage 3 should normally improve
// over Stage 1. AOT versus Tier-2 remains platform/workload dependent.
//
// Multi-core SRE use:
//   1. Run test_ejit_period on one core first. It becomes the compile worker
//      and returns to the shell.
//   2. Run test_ejit_period on a different core. It executes the benchmark.
//   3. At each printed dump pause, switch to the worker core and run:
//        ejit_print_dumped "pgo_layout_bench"
//      The first pause prints Tier-1 IR/ASM; the second prints Tier-2 IR/ASM.
//
// Compile the C input with the PGO-compatible wrapper inline cache if desired:
//   clang -c -O2 ejit_pgo_staged_benchmark.c -o ejit_pgo_bench.o \
//     -mllvm -ejit-inline-cache=true
// Save the compile-time AOT/wrapper IR separately with:
//   clang -S -emit-llvm -O2 ejit_pgo_staged_benchmark.c -o pgo_aot.ll \
//     -mllvm -ejit-inline-cache=true
//
// Expected IR distinction:
//   Tier-1: profile counter updates / __profc_* references are present.
//   Tier-2: counter updates are absent and biased branches carry !prof data.
//
//===----------------------------------------------------------------------===//

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "ejit_test_helpers.h"

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern uint64_t SRE_CycleCountGet64(void);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define SAMPLE_COUNT 256u
#define INNER_ITERS 4096u
#define BENCH_BATCHES 7u
#define AOT_CALLS_PER_BATCH 128u
// Including the Tier-1 confirmation hit, this stays well below the runtime's
// default 64-hit Tier-2 threshold.
#define TIER1_CALLS_PER_BATCH 6u
#define TIER2_CALLS_PER_BATCH 128u
#define TIER2_TRIGGER_CALLS 64u
#define COMPILE_WAIT_ROUNDS 500u
#define COMPILE_WAIT_TICKS 10u
#define DUMP_PAUSE_TICKS 40000u

// Build the same source twice when deciding whether online PGO is worthwhile:
//   0: ordinary stable JIT baseline (ejit_init)
//   1: online-PGO Tier-1 -> Tier-2 path (ejit_init_pgo)
// Keep every workload constant identical between the two builds.
#ifndef EJIT_PGO_BENCH_MODE
#define EJIT_PGO_BENCH_MODE 1
#endif

struct PgoBenchCfg {
  ejit_may_const uint32_t mode;
  ejit_may_const uint32_t multiplier;
  uint32_t cutoff;
  uint32_t salt;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct PgoBenchCfg g_pgo_bench_cfg[1];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_pgo_bench_samples[SAMPLE_COUNT];
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_pgo_bench_sink;

static inline uint32_t local_core_id(void) { return (uint32_t)g_ucLocalCoreID; }

static void init_bench_data(void) {
  g_pgo_bench_cfg[0].mode = 3u;
  g_pgo_bench_cfg[0].multiplier = 7u;
  g_pgo_bench_cfg[0].cutoff = 250u;
  g_pgo_bench_cfg[0].salt = 0x13579bdu;

  // 250/256 samples take the hot arm, but the cutoff and samples remain
  // runtime data, so static specialization cannot fold or prove the branch.
  for (uint32_t i = 0; i < SAMPLE_COUNT; ++i)
    g_pgo_bench_samples[i] = i;
  g_pgo_bench_sink = 0;
}

// Keep the body duplicated in AOT and EJIT functions without introducing a
// helper call whose own placement/linkage would contaminate the measurement.
#define PGO_BENCH_BODY(CELL, SEED)                                             \
  do {                                                                         \
    uint32_t _mode = g_pgo_bench_cfg[(CELL)].mode;                             \
    uint32_t _mul = g_pgo_bench_cfg[(CELL)].multiplier;                        \
    uint32_t _cutoff = g_pgo_bench_cfg[(CELL)].cutoff;                         \
    uint32_t _salt = g_pgo_bench_cfg[(CELL)].salt;                             \
    uint32_t _acc = (SEED) ^ _salt;                                            \
    for (uint32_t _i = 0; _i < INNER_ITERS; ++_i) {                            \
      uint32_t _v = g_pgo_bench_samples[(_i + (SEED)) & (SAMPLE_COUNT - 1u)];  \
      if (_v >= _cutoff) {                                                     \
        /* Cold 6/256 arm: deliberately non-trivial but deterministic. */      \
        _acc ^= (_v + _salt) * 0x45d9f3bu;                                     \
        _acc = (_acc << 7) | (_acc >> 25);                                     \
      } else {                                                                 \
        /* Hot 250/256 arm. */                                                 \
        _acc += (_v ^ _mode) * _mul + (_acc >> 13);                            \
      }                                                                        \
    }                                                                          \
    return _acc;                                                               \
  } while (0)

__attribute__((noinline)) static uint32_t pgo_layout_aot(uint8_t cell,
                                                         uint32_t seed) {
  PGO_BENCH_BODY(cell, seed);
}

ejit_entry __attribute__((noinline)) uint32_t
pgo_layout_bench(ejit_period_arr_ind(cell) uint8_t cell, uint32_t seed) {
  PGO_BENCH_BODY(cell, seed);
}

typedef uint32_t (*BenchFn)(uint8_t, uint32_t);

struct BenchResult {
  uint64_t totalCycles;
  uint64_t bestCycles;
  uint64_t checksum;
  uint32_t calls;
};

static struct BenchResult run_benchmark(const char *label, BenchFn fn,
                                        uint32_t core, uint32_t callsPerBatch) {
  struct BenchResult r;
  memset(&r, 0, sizeof(r));
  r.bestCycles = UINT64_MAX;

  for (uint32_t batch = 0; batch < BENCH_BATCHES; ++batch) {
    uint64_t checksum = 0;
    uint64_t begin = SRE_CycleCountGet64();
    for (uint32_t call = 0; call < callsPerBatch; ++call)
      checksum += fn(0, batch * callsPerBatch + call + 1u);
    uint64_t cycles = SRE_CycleCountGet64() - begin;
    r.totalCycles += cycles;
    r.calls += callsPerBatch;
    if (cycles < r.bestCycles)
      r.bestCycles = cycles;
    r.checksum ^= checksum;
    g_pgo_bench_sink ^= checksum;
    SRE_printf("[PGO-BENCH][core=%u] %-12s batch=%u/%u total=%llu "
               "avg_x1000=%llu checksum=0x%llx\n",
               core, label, batch + 1u, BENCH_BATCHES,
               (unsigned long long)cycles,
               (unsigned long long)(cycles * 1000u / callsPerBatch),
               (unsigned long long)checksum);
  }

  SRE_printf("[PGO-BENCH][core=%u] %-12s summary calls=%u avg_x1000=%llu "
             "best_batch_avg_x1000=%llu checksum=0x%llx\n",
             core, label, r.calls,
             (unsigned long long)(r.totalCycles * 1000u / r.calls),
             (unsigned long long)(r.bestCycles * 1000u / callsPerBatch),
             (unsigned long long)r.checksum);
  return r;
}

static bool wait_for_compiles(uint32_t core, uint64_t target) {
  for (uint32_t round = 0; round < COMPILE_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ejit_taskpool_get_stats(&stats);
    if (stats.asyncCompiles >= target && ejit_taskpool_pending_count() == 0)
      return true;
    SRE_TaskDelay(COMPILE_WAIT_TICKS);
  }
  SRE_printf("[PGO-BENCH][core=%u] FAIL: wait compiles >= %llu timeout\n", core,
             (unsigned long long)target);
  return false;
}

static void dump_pause(uint32_t core, uint32_t worker, const char *tier,
                       const char *expected) {
  SRE_printf("\n[PGO-BENCH][core=%u] %s dump is ready on worker core %u\n",
             core, tier, worker);
  SRE_printf("[PGO-BENCH] NOW run on core %u: "
             "ejit_print_dumped \"pgo_layout_bench\"\n",
             worker);
  SRE_printf("[PGO-BENCH] IR/ASM check: %s\n", expected);
  SRE_TaskDelay(DUMP_PAUSE_TICKS);
  SRE_printf("[PGO-BENCH][core=%u] %s dump pause complete\n\n", core, tier);
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = local_core_id();
  SRE_printf("\n=== EJIT staged online-PGO benchmark (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = false;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc =
      EJIT_PGO_BENCH_MODE ? ejit_init_pgo(&cfg) : ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[PGO-BENCH][core=%u] init mode=%s rc=%d worker=%u\n", core,
             EJIT_PGO_BENCH_MODE ? "PGO" : "baseline", (int)rc, worker);
  if (rc != EJIT_OK)
    return -1;

  // The first invocation only elects/starts the persistent compile worker.
  // Returning keeps that core's shell available for the two dump commands.
  if (core == worker) {
    SRE_printf("[PGO-BENCH][core=%u] worker ready; run this test on a business "
               "core next\n",
               core);
    return 0;
  }

  init_bench_data();
  ejit_activate("cell", 0);

  SRE_printf("\n--- Stage 0: direct AOT baseline ---\n");
  struct BenchResult aot =
      run_benchmark("AOT", &pgo_layout_aot, core, AOT_CALLS_PER_BATCH);

  // Arm capture before the first compile. Tier-1 and Tier-2 use the same exact
  // function name; Tier-2 replaces the saved entry after the first dump pause.
  ejit_dump_func("pgo_layout_bench");
  SRE_printf("\n--- Stage 1a: enqueue Tier-1 ---\n");
  uint32_t fallback = pgo_layout_bench(0, 1u);
  uint32_t fallbackExpected = pgo_layout_aot(0, 1u);
  SRE_printf("[PGO-BENCH][core=%u] first active call result=0x%x "
             "expected=0x%x (AOT fallback while compile is pending)\n",
             core, fallback, fallbackExpected);
  if (fallback != fallbackExpected)
    return -2;
  if (!wait_for_compiles(core, 1u))
    return -3;

  // This first hit resolves Tier-1. With the PGO/inline-cache compatibility
  // protocol it deliberately does NOT fill the wrapper cell yet.
  uint32_t tier1Check = pgo_layout_bench(0, 2u);
  uint32_t tier1Expected = pgo_layout_aot(0, 2u);
  SRE_printf("[PGO-BENCH][core=%u] Tier-1 ready result=0x%x expected=0x%x\n",
             core, tier1Check, tier1Expected);
  if (tier1Check != tier1Expected)
    return -4;

  if (!EJIT_PGO_BENCH_MODE) {
    dump_pause(core, worker, "Baseline",
               "no profile counters or PGO branch weights; save ASM for the "
               "Tier-2 layout comparison");
    SRE_printf("\n--- Baseline stable JIT benchmark ---\n");
    struct BenchResult baseline = run_benchmark(
        "Baseline-JIT", &pgo_layout_bench, core, TIER2_CALLS_PER_BATCH);
    ejit_taskpool_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    ejit_taskpool_get_stats(&stats);
    SRE_printf("\n--- Baseline result ---\n");
    SRE_printf(
        "[PGO-BENCH] avg_x1000: AOT=%llu Baseline-JIT=%llu\n",
        (unsigned long long)(aot.totalCycles * 1000u / aot.calls),
        (unsigned long long)(baseline.totalCycles * 1000u / baseline.calls));
    SRE_printf("[PGO-BENCH] stats ready=%u hits=%llu compiles=%llu "
               "compileFailed=%llu publishFailed=%llu sink=0x%llx\n",
               stats.readyEntries, (unsigned long long)stats.cacheHits,
               (unsigned long long)stats.asyncCompiles,
               (unsigned long long)stats.compileFailed,
               (unsigned long long)stats.publishFailed,
               (unsigned long long)g_pgo_bench_sink);
    if (stats.asyncCompiles < 1 || stats.compileFailed != 0 ||
        stats.publishFailed != 0)
      return -7;
    SRE_printf("=== EJIT baseline benchmark complete ===\n");
    return 0;
  }
  dump_pause(core, worker, "Tier-1",
             "profile counter updates should be present");

  SRE_printf("\n--- Stage 1b: Tier-1 instrumented benchmark ---\n");
  struct BenchResult tier1 =
      run_benchmark("Tier-1", &pgo_layout_bench, core, TIER1_CALLS_PER_BATCH);

  SRE_printf("\n--- Stage 2: trigger and wait for Tier-2 ---\n");
  for (uint32_t i = 0; i < TIER2_TRIGGER_CALLS; ++i)
    g_pgo_bench_sink ^= pgo_layout_bench(0, 0x1000u + i);
  if (!wait_for_compiles(core, 2u))
    return -5;

  // The first post-publish resolve both verifies Tier-2 and fills this core's
  // private wrapper inline-cache cell. Later Stage-3 calls are direct hits.
  uint32_t tier2Check = pgo_layout_bench(0, 3u);
  uint32_t tier2Expected = pgo_layout_aot(0, 3u);
  SRE_printf("[PGO-BENCH][core=%u] Tier-2 ready result=0x%x expected=0x%x\n",
             core, tier2Check, tier2Expected);
  if (tier2Check != tier2Expected)
    return -6;
  dump_pause(core, worker, "Tier-2",
             "counter updates should be absent; biased branches should carry "
             "profile weights and show a profile-guided block layout in ASM");

  SRE_printf("\n--- Stage 3: Tier-2 steady-state benchmark ---\n");
  struct BenchResult tier2 =
      run_benchmark("Tier-2", &pgo_layout_bench, core, TIER2_CALLS_PER_BATCH);

  ejit_taskpool_stats_t stats;
  memset(&stats, 0, sizeof(stats));
  ejit_taskpool_get_stats(&stats);
  SRE_printf("\n--- Result ---\n");
  SRE_printf("[PGO-BENCH] avg_x1000: AOT=%llu Tier-1=%llu Tier-2=%llu\n",
             (unsigned long long)(aot.totalCycles * 1000u / aot.calls),
             (unsigned long long)(tier1.totalCycles * 1000u / tier1.calls),
             (unsigned long long)(tier2.totalCycles * 1000u / tier2.calls));
  SRE_printf(
      "[PGO-BENCH] Tier-2 vs Tier-1 improvement_permille=%lld "
      "(100 means 10%% faster; positive is faster)\n",
      (long long)(1000 - (int64_t)(tier2.totalCycles * tier1.calls * 1000u /
                                   (tier1.totalCycles * tier2.calls))));
  SRE_printf("[PGO-BENCH] stats ready=%u hits=%llu compiles=%llu enqueues=%llu "
             "compileFailed=%llu publishFailed=%llu sink=0x%llx\n",
             stats.readyEntries, (unsigned long long)stats.cacheHits,
             (unsigned long long)stats.asyncCompiles,
             (unsigned long long)stats.asyncEnqueues,
             (unsigned long long)stats.compileFailed,
             (unsigned long long)stats.publishFailed,
             (unsigned long long)g_pgo_bench_sink);

  if (stats.asyncCompiles < 2 || stats.compileFailed != 0 ||
      stats.publishFailed != 0) {
    SRE_printf("[PGO-BENCH] FAIL: Tier-1/Tier-2 pipeline was not clean\n");
    return -7;
  }
  SRE_printf("=== EJIT staged online-PGO benchmark complete ===\n");
  return 0;
}
