//===-- ejit_bound_ptr_sre_multicore_test.c - cross-core raw pointer demo --===//
//
// Board flow after a reset:
//   1. Core 8:  test_ejit_period
//      Initializes EJIT, owns the shared compile worker, and arms the dump.
//   2. Core 18: test_ejit_period
//      Enqueues compilation from shared per-cell objects, waits for
//      publication, then verifies JIT cache hits using those same objects.
//   3. Core 8:  test_ejit_bound_ptr_print
//      Prints the worker-local optimized IR/ASM.
//
// Do not call ejit_shutdown(): the owner worker and facade must survive across
// shell invocations.
//
//===----------------------------------------------------------------------===//

// Kept self-contained for direct board integration: no project or libc
// headers are required by this file.
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;
typedef _Bool bool;

#define true 1
#define false 0

#define EJIT_PERIOD_CONST __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x) __attribute__((ejit_dim(#x)))
#define EJIT_BOUND_PTR(x) __attribute__((ejit_bound_ptr(#x)))
#define EJIT_ENTRY __attribute__((ejit_entry))

#define ejit_may_const EJIT_PERIOD_CONST
#define ejit_period_arr(x) EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr_ind(x) EJIT_DIM(x)
#define ejit_entry EJIT_ENTRY

typedef enum {
  EJIT_OK = 0,
  EJIT_COMPILE_ERROR = -3,
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

extern ejit_status_t ejit_init(const ejit_config_t *config);
extern ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx);
extern void ejit_clear_cache(void);
extern unsigned ejit_taskpool_pending_count(void);
extern ejit_status_t ejit_publish_pending_code(void);
extern ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
extern void ejit_taskpool_print_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern void ejit_dump_func(const char *name);
extern void ejit_print_dumped(const char *name);

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define BOUND_PTR_WORKER_CORE 8u
#define BOUND_PTR_PRODUCER_CORE 18u
#define BOUND_PTR_CELL_A 1u
#define BOUND_PTR_CELL_B 2u
#define BOUND_PTR_TRP 2u
#define BOUND_PTR_WAIT_ROUNDS 6000u
#define BOUND_PTR_WAIT_TICKS 10u

enum BoundPtrDemoStage {
  BOUND_PTR_DEMO_RESET = 0,
  BOUND_PTR_DEMO_WORKER_READY = 1,
  BOUND_PTR_DEMO_COMPILED = 2,
  BOUND_PTR_DEMO_PRINTED = 3,
};

struct CellRelated {
  ejit_may_const uint32_t algorithm;
  ejit_may_const uint32_t scale;
  uint32_t runtimeBias;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
uint32_t g_bound_cells[8];
EJIT_SHARED_SECTION_ATTR ejit_period_arr(trp)
uint32_t g_bound_trps[8];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_bound_ptr_demo_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_bound_ptr_demo_sink;

ejit_entry uint32_t
bound_cell_config_mc(ejit_period_arr_ind(cell) uint8_t cellIndex,
                     ejit_period_arr_ind(trp) uint8_t trpIndex,
                     EJIT_BOUND_PTR(cell) const struct CellRelated *cellRelated,
                     uint32_t input) {
  if (cellRelated->algorithm == 7u)
    return input * cellRelated->scale + cellRelated->runtimeBias + cellIndex +
           trpIndex;
  return input + cellRelated->runtimeBias;
}

EJIT_SHARED_SECTION_ATTR struct CellRelated g_bound_configs[8] = {
    {7u, 5u, 100u}, {7u, 9u, 100u}};

static uint32_t enqueue_from_shared(uint8_t cell) {
  return bound_cell_config_mc(cell, BOUND_PTR_TRP, &g_bound_configs[cell], 10u);
}

static int wait_for_compiles(uint64_t baseline, uint64_t expected) {
  for (uint32_t round = 0; round < BOUND_PTR_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    (void)ejit_taskpool_get_stats(&stats);
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[BOUND-PTR-MC] FAIL compile=%llu publish=%llu\n",
                 (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + expected &&
        ejit_taskpool_pending_count() == 0)
      return 1;
    if ((round % 500u) == 0)
      SRE_printf("[BOUND-PTR-MC] waiting compiles=%llu/%llu pending=%u\n",
                 (unsigned long long)(stats.asyncCompiles - baseline),
                 (unsigned long long)expected,
                 ejit_taskpool_pending_count());
    (void)SRE_TaskDelay(BOUND_PTR_WAIT_TICKS);
  }
  return 0;
}

static int run_producer(void) {
  uint32_t stage = __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE);
  if (stage != BOUND_PTR_DEMO_WORKER_READY) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL stage=%u; run core %u first\n",
               BOUND_PTR_PRODUCER_CORE, stage, BOUND_PTR_WORKER_CORE);
    return -3;
  }

  if (ejit_activate("cell", BOUND_PTR_CELL_A) != EJIT_OK ||
      ejit_activate("cell", BOUND_PTR_CELL_B) != EJIT_OK ||
      ejit_activate("trp", BOUND_PTR_TRP) != EJIT_OK) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL activate\n",
               BOUND_PTR_PRODUCER_CORE);
    return -4;
  }

  ejit_clear_cache();
  ejit_taskpool_stats_t before = {0};
  (void)ejit_taskpool_get_stats(&before);

  // In-flight dedup is keyed by funcIndex, not by the complete dimension
  // identity. Submit the two cell versions serially; otherwise cell 2 sees
  // AlreadyPending while cell 1 is compiling and is never enqueued.
  // Each first call returns AOT while the shared object remains alive through
  // worker compilation. The raw pointer is transport-only and is never freed
  // by the queue or worker.
  uint32_t aotA = enqueue_from_shared(BOUND_PTR_CELL_A);
  if (ejit_publish_pending_code() != EJIT_OK) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL cell1 batch publish\n",
               BOUND_PTR_PRODUCER_CORE);
    return -5;
  }
  if (!wait_for_compiles(before.asyncCompiles, 1u)) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL cell1 compile timeout\n",
               BOUND_PTR_PRODUCER_CORE);
    ejit_taskpool_print_stats();
    return -6;
  }

  uint32_t aotB = enqueue_from_shared(BOUND_PTR_CELL_B);
  if (ejit_publish_pending_code() != EJIT_OK) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL cell2 batch publish\n",
               BOUND_PTR_PRODUCER_CORE);
    return -7;
  }
  const uint32_t expectedAotA = 153u;
  const uint32_t expectedAotB = 194u;
  if (aotA != expectedAotA || aotB != expectedAotB) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL AOT cell1=%u/%u "
               "cell2=%u/%u\n",
               BOUND_PTR_PRODUCER_CORE, aotA, expectedAotA, aotB,
               expectedAotB);
    return -8;
  }

  if (!wait_for_compiles(before.asyncCompiles, 2u)) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL cell2 compile timeout\n",
               BOUND_PTR_PRODUCER_CORE);
    ejit_taskpool_print_stats();
    return -9;
  }

  ejit_taskpool_stats_t compiled = {0};
  (void)ejit_taskpool_get_stats(&compiled);

  // Each cell keeps its own stable scale and object address. Reusing cell 1's
  // scale=5 specialization for cell 2 would return 153 instead of 194.
  uint32_t jitA = enqueue_from_shared(BOUND_PTR_CELL_A);
  uint32_t jitB = enqueue_from_shared(BOUND_PTR_CELL_B);
  const uint32_t expectedJitA = expectedAotA;
  const uint32_t expectedJitB = expectedAotB;
  ejit_taskpool_stats_t after = {0};
  (void)ejit_taskpool_get_stats(&after);
  if (jitA != expectedJitA || jitB != expectedJitB) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL JIT cell1=%u/%u "
               "cell2=%u/%u\n",
               BOUND_PTR_PRODUCER_CORE, jitA, expectedJitA, jitB,
               expectedJitB);
    return -10;
  }
  if (after.cacheHits < compiled.cacheHits + 2u) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL no cache hit before=%llu "
               "after=%llu; check shared code-pointer support\n",
               BOUND_PTR_PRODUCER_CORE,
               (unsigned long long)compiled.cacheHits,
               (unsigned long long)after.cacheHits);
    return -11;
  }

  __atomic_store_n(&g_bound_ptr_demo_sink,
                   ((uint64_t)jitA << 32) | (uint64_t)jitB, __ATOMIC_RELEASE);
  __atomic_store_n(&g_bound_ptr_demo_stage, BOUND_PTR_DEMO_COMPILED,
                   __ATOMIC_RELEASE);
  SRE_printf("[BOUND-PTR-MC][core=%u] PASS cell1 AOT/JIT=%u/%u cell2 "
             "AOT/JIT=%u/%u compiles=2 hits=%llu; run "
             "test_ejit_bound_ptr_print on core %u\n",
             BOUND_PTR_PRODUCER_CORE, aotA, jitA, aotB, jitB,
             (unsigned long long)(after.cacheHits - compiled.cacheHits),
             BOUND_PTR_WORKER_CORE);
  return 0;
}

static int run_worker(void) {
  uint32_t stage = __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE);
  if (stage == BOUND_PTR_DEMO_RESET) {
    __atomic_store_n(&g_bound_ptr_demo_sink, 0u, __ATOMIC_RELEASE);
    ejit_dump_func("bound_cell_config_mc");
    __atomic_store_n(&g_bound_ptr_demo_stage, BOUND_PTR_DEMO_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[BOUND-PTR-MC][core=%u] worker ready; run test_ejit_period "
               "on core %u\n",
               BOUND_PTR_WORKER_CORE, BOUND_PTR_PRODUCER_CORE);
    return 0;
  }

  SRE_printf("[BOUND-PTR-MC][core=%u] worker already initialized stage=%u; "
             "use test_ejit_bound_ptr_print for output\n",
             BOUND_PTR_WORKER_CORE, stage);
  return 0;
}

int test_ejit_bound_ptr_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != BOUND_PTR_WORKER_CORE) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL: dump is worker-local; run "
               "test_ejit_bound_ptr_print on core %u\n",
               core, BOUND_PTR_WORKER_CORE);
    return -9;
  }

  uint32_t stage = __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE);
  if (stage < BOUND_PTR_DEMO_COMPILED) {
    SRE_printf("[BOUND-PTR-MC][core=%u] FAIL stage=%u; run test_ejit_period "
               "on core %u first\n",
               BOUND_PTR_WORKER_CORE, stage, BOUND_PTR_PRODUCER_CORE);
    return -10;
  }
  if (stage == BOUND_PTR_DEMO_PRINTED) {
    SRE_printf("[BOUND-PTR-MC][core=%u] dump already printed\n",
               BOUND_PTR_WORKER_CORE);
    return 0;
  }

  ejit_dump_func("");
  SRE_printf("\n[BOUND-PTR-MC] === COMPILED VERSIONS ===\n");
  ejit_taskpool_print_compiled();
  SRE_printf("\n[BOUND-PTR-MC] === OPTIMIZED FUNCTION ===\n");
  ejit_print_dumped("bound_cell_config_mc");
  SRE_printf("[BOUND-PTR-MC][core=%u] expect: algorithm/scale loads and "
             "branch removed; runtimeBias load retained. The function dump "
             "is the latest capture (cell=2, scale=9).\n",
             BOUND_PTR_WORKER_CORE);
  SRE_printf("[BOUND-PTR-MC][core=%u] PASS sink=0x%llx\n",
             BOUND_PTR_WORKER_CORE,
             (unsigned long long)__atomic_load_n(&g_bound_ptr_demo_sink,
                                                 __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_bound_ptr_demo_stage, BOUND_PTR_DEMO_PRINTED,
                   __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT bound-pointer multicore demo (core=%u) ===\n", core);
  call_init_array_functions();

  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[BOUND-PTR-MC][core=%u] init rc=%d worker=%u stage=%u\n", core,
             (int)rc, worker,
             __atomic_load_n(&g_bound_ptr_demo_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != BOUND_PTR_WORKER_CORE) {
    SRE_printf("[BOUND-PTR-MC] FAIL worker=%u; reset and run core %u first\n",
               worker, BOUND_PTR_WORKER_CORE);
    return -2;
  }

  if (core == BOUND_PTR_WORKER_CORE)
    return run_worker();
  if (core == BOUND_PTR_PRODUCER_CORE)
    return run_producer();

  SRE_printf("[BOUND-PTR-MC][core=%u] skip: use core %u or core %u\n", core,
             BOUND_PTR_WORKER_CORE, BOUND_PTR_PRODUCER_CORE);
  return 0;
}
