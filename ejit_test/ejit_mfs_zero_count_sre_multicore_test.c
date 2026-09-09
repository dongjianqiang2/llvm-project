/* Header-free board acceptance. Product startup owns init-array execution.
 * This repeatable command never calls call_init_array_functions(). In a
 * standalone shell without product startup, expose init-array as a separate,
 * coordinated one-shot setup command per core.
 *
 * Run test_ejit_mfs on worker core 6, then producer core 16. Repeated worker
 * calls preserve shared configuration and the live worker. A completed
 * producer call validates existing Tier-2 code without requiring new PGO
 * compiles. Failed or timed-out runs are terminal until a coordinated reset.
 * MFS_EXPECT_SPLIT=0 is the feature-OFF control. */

#if !defined(EJIT_MFS_HOST_TEST) && !defined(__has_attribute)
#error "EJIT MFS board acceptance requires Clang attribute detection"
#endif
#if !defined(EJIT_MFS_HOST_TEST) &&                                            \
    (!__has_attribute(ejit_entry) || !__has_attribute(ejit_dim) ||             \
     !__has_attribute(ejit_period_const) ||                                    \
     !__has_attribute(ejit_in_period_array))
#error "rebuild and use the EJIT Clang before compiling this board test"
#endif

typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long uintptr_t;
typedef unsigned long size_t;
typedef _Bool bool;
typedef struct {
  int compileMode;
  int optLevel;
  size_t maxCodeMemory;
  size_t maxDataMemory;
  size_t maxCacheEntries;
  size_t maxCacheSize;
  bool enableLogger;
  bool forceStaticRegistry;
  const char *dumpJITDir;
} ejit_config_t;
typedef struct {
  uint64_t poolCount, sealedCount, activeCount, usedBytes, reservedBytes;
  uint64_t wastedBytes, sealInvocations, splitInvocations, finalizedRangeCount;
} ejit_code_pool_stats_t;
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
extern int ejit_init_pgo(const ejit_config_t *);
extern int ejit_activate(const char *, uint32_t);
extern void ejit_register_symbol(const char *, void *);
extern uint32_t ejit_taskpool_classify_tier2_pc(uintptr_t);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern int ejit_taskpool_get_stats(ejit_taskpool_stats_t *);
extern int ejit_get_cold_code_pool_stats(ejit_code_pool_stats_t *);
extern void ejit_taskpool_print_compiled(void);
extern void ejit_print_code_pool_stats(void);
extern void ejit_taskpool_print_stats(void);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void SRE_printf(const char *, ...);
extern uint8_t g_ucLocalCoreID;

#ifdef EJIT_MFS_HOST_TEST
#define SHARED
#define ENTRY
#define CELL
#define PERIOD_CONST
#define PERIOD_ARRAY
#else
#define SHARED __attribute__((section(".mc_shared")))
#define ENTRY __attribute__((ejit_entry))
#define CELL __attribute__((ejit_dim("cell")))
#define PERIOD_CONST __attribute__((ejit_period_const))
#define PERIOD_ARRAY __attribute__((ejit_in_period_array("cell")))
#endif

#define MFS_WORKER_CORE 6u
#define MFS_PRODUCER_CORE 16u
#define MFS_SETUP_IDLE 0u
#define MFS_SETUP_ACTIVE 1u
#define MFS_SETUP_READY 2u
#define MFS_SETUP_FAILED 3u
#define MFS_RUN_IDLE 0u
#define MFS_RUN_ACTIVE 1u
#define MFS_RUN_COMPLETE 2u
#define MFS_RUN_FAILED 3u
#define MFS_RC_PREVIOUS_FAILED -20
#ifndef MFS_EXPECT_SPLIT
#define MFS_EXPECT_SPLIT 1
#endif
#ifndef MFS_WAIT_ROUNDS
#define MFS_WAIT_ROUNDS 6000u
#endif
#ifndef MFS_VERIFY_ROUNDS
#define MFS_VERIFY_ROUNDS 128u
#endif

struct MfsConfig {
  uint32_t bias PERIOD_CONST;
};
SHARED struct MfsConfig g_mfs_config[16] PERIOD_ARRAY;
SHARED volatile uintptr_t g_mfs_hot_pc[16];
SHARED volatile uintptr_t g_mfs_cold_pc[16];
SHARED volatile uint32_t g_mfs_setup_state[2];
SHARED volatile uint32_t g_mfs_config_ready;
SHARED volatile uint32_t g_mfs_worker_armed;
SHARED volatile uint32_t g_mfs_run_state;
SHARED volatile uint32_t g_mfs_producer_command_active;

__attribute__((noinline)) void mfs_hot_witness(uint32_t cell) {
  g_mfs_hot_pc[cell] = (uintptr_t)__builtin_return_address(0);
}
__attribute__((noinline)) void mfs_cold_witness(uint32_t cell) {
  g_mfs_cold_pc[cell] = (uintptr_t)__builtin_return_address(0);
}

ENTRY uint32_t mfs_zero_entry(CELL uint8_t cell, uint32_t cold,
                              volatile uint8_t *data) {
  uint32_t bias = g_mfs_config[cell].bias;
  if (cold) {
    /* Volatile writes keep the never-sampled path executable and
     * substantial. */
    for (uint32_t i = 0; i < 96; ++i)
      data[i] = (uint8_t)((i * 17u + bias) ^ 0x5au);
    mfs_cold_witness(cell);
    return bias ^ 0x1234u;
  }
  mfs_hot_witness(cell);
  return bias + 7u;
}

/* Pure read-only diagnostic command: no init, registration, reset, or state
 * transition. It is safe to call repeatedly after success or failure. */
int test_ejit_mfs_print(void) {
  ejit_taskpool_print_compiled();
  ejit_print_code_pool_stats();
  ejit_taskpool_print_stats();
  return 0;
}

static uint32_t mfs_setup_index(uint32_t core) {
  return core == MFS_WORKER_CORE ? 0u : 1u;
}

static int mfs_ensure_ejit_ready(uint32_t core) {
  uint32_t worker = ejit_taskpool_get_worker_core();
  if (worker == MFS_WORKER_CORE)
    return 0;
  ejit_config_t config = {0};
  config.compileMode = 1;
  config.optLevel = 2;
  config.enableLogger = 1;
  config.forceStaticRegistry = 1;
  int rc = ejit_init_pgo(&config);
  worker = ejit_taskpool_get_worker_core();
  SRE_printf("[MFS][core=%u] init rc=%d worker=%u\n", core, rc, worker);
  if (rc != 0)
    return -2;
  if (worker != MFS_WORKER_CORE)
    return -3;
  return 0;
}

static int mfs_setup_current_core(uint32_t core) {
  uint32_t index = mfs_setup_index(core);
  uint32_t state = __atomic_load_n(&g_mfs_setup_state[index], __ATOMIC_ACQUIRE);
  if (state == MFS_SETUP_READY)
    return ejit_taskpool_get_worker_core() == MFS_WORKER_CORE ? 0 : -3;
  if (state == MFS_SETUP_ACTIVE)
    return -4;
  if (state == MFS_SETUP_FAILED)
    return MFS_RC_PREVIOUS_FAILED;
  uint32_t expected = MFS_SETUP_IDLE;
  if (!__atomic_compare_exchange_n(&g_mfs_setup_state[index], &expected,
                                   MFS_SETUP_ACTIVE, 0, __ATOMIC_ACQ_REL,
                                   __ATOMIC_ACQUIRE))
    return expected == MFS_SETUP_FAILED ? MFS_RC_PREVIOUS_FAILED : -4;

  int rc = mfs_ensure_ejit_ready(core);
  if (rc != 0) {
    __atomic_store_n(&g_mfs_setup_state[index], MFS_SETUP_FAILED,
                     __ATOMIC_RELEASE);
    return rc;
  }
  ejit_register_symbol("mfs_hot_witness", (void *)&mfs_hot_witness);
  ejit_register_symbol("mfs_cold_witness", (void *)&mfs_cold_witness);
  ejit_register_symbol("g_mfs_hot_pc", (void *)g_mfs_hot_pc);
  ejit_register_symbol("g_mfs_cold_pc", (void *)g_mfs_cold_pc);
  __atomic_store_n(&g_mfs_setup_state[index], MFS_SETUP_READY,
                   __ATOMIC_RELEASE);
  return 0;
}

static int mfs_fail_run(int rc) {
  __atomic_store_n(&g_mfs_run_state, MFS_RUN_FAILED, __ATOMIC_RELEASE);
  return rc;
}

static int mfs_verify_tier2(uint32_t rounds) {
  volatile uint8_t data[96] = {0};
  for (uint32_t repeat = 0; repeat < rounds; ++repeat) {
    for (uint32_t cell = 0; cell < 16; ++cell) {
      uint32_t bias = cell + 31u;
      g_mfs_cold_pc[cell] = 0;
      if (mfs_zero_entry((uint8_t)cell, 1u, data) != (bias ^ 0x1234u))
        return -8;
      for (uint32_t i = 0; i < 96; ++i)
        if (data[i] != (uint8_t)((i * 17u + bias) ^ 0x5au))
          return -9;
      uint32_t coldKind = ejit_taskpool_classify_tier2_pc(g_mfs_cold_pc[cell]);
      if (coldKind != (MFS_EXPECT_SPLIT ? 2u : 1u)) {
        SRE_printf("[MFS] FAIL cold witness cell=%u kind=%u\n", cell, coldKind);
        return -11;
      }
      g_mfs_hot_pc[cell] = 0;
      if (mfs_zero_entry((uint8_t)cell, 0u, data) != bias + 7u ||
          ejit_taskpool_classify_tier2_pc(g_mfs_hot_pc[cell]) != 1u)
        return -12;
    }
  }
  return 0;
}

static int mfs_verify_cold_stats(void) {
  ejit_code_pool_stats_t stats = {0};
  if (ejit_get_cold_code_pool_stats(&stats) != 0 ||
      (MFS_EXPECT_SPLIT && !stats.usedBytes) ||
      (!MFS_EXPECT_SPLIT && stats.usedBytes))
    return -13;
  return 0;
}

static int mfs_run_producer(void) {
  uint32_t state = __atomic_load_n(&g_mfs_run_state, __ATOMIC_ACQUIRE);
  if (state == MFS_RUN_FAILED)
    return MFS_RC_PREVIOUS_FAILED;
  if (state == MFS_RUN_ACTIVE)
    return -4;
  if (state == MFS_RUN_COMPLETE) {
    if (__atomic_load_n(&g_mfs_setup_state[1], __ATOMIC_ACQUIRE) !=
            MFS_SETUP_READY ||
        ejit_taskpool_get_worker_core() != MFS_WORKER_CORE)
      return mfs_fail_run(-3);
    int rc = mfs_verify_tier2(1u);
    if (rc == 0)
      rc = mfs_verify_cold_stats();
    if (rc != 0)
      return mfs_fail_run(rc);
    SRE_printf("[MFS] already complete; existing Tier-2 versions OK\n");
    return 0;
  }

  uint32_t expected = MFS_RUN_IDLE;
  if (!__atomic_compare_exchange_n(&g_mfs_run_state, &expected, MFS_RUN_ACTIVE,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
    return expected == MFS_RUN_FAILED ? MFS_RC_PREVIOUS_FAILED : -4;
  if (__atomic_load_n(&g_mfs_worker_armed, __ATOMIC_ACQUIRE) != 1u ||
      __atomic_load_n(&g_mfs_config_ready, __ATOMIC_ACQUIRE) != 1u)
    return mfs_fail_run(-3);

  int rc = mfs_setup_current_core(MFS_PRODUCER_CORE);
  if (rc != 0)
    return mfs_fail_run(rc);
  for (uint32_t cell = 0; cell < 16; ++cell)
    if (ejit_activate("cell", cell) != 0)
      return mfs_fail_run(-5);

  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != 0)
    return mfs_fail_run(-5);
  volatile uint8_t data[96] = {0};
  uint32_t witnessed = 0;
  for (uint32_t round = 0; round < MFS_WAIT_ROUNDS; ++round) {
    witnessed = 0;
    /* Round-robin all identities continuously. Queue state and pending counts
     * are never accepted as proof of Tier-2 execution. */
    for (uint32_t cell = 0; cell < 16; ++cell) {
      g_mfs_hot_pc[cell] = 0;
      if (mfs_zero_entry((uint8_t)cell, 0u, data) != cell + 38u)
        return mfs_fail_run(-5);
      if (ejit_taskpool_classify_tier2_pc(g_mfs_hot_pc[cell]) == 1u)
        witnessed |= 1u << cell;
      if (g_mfs_cold_pc[cell] != 0)
        return mfs_fail_run(-5);
    }
    ejit_taskpool_stats_t now = {0};
    if (ejit_taskpool_get_stats(&now) != 0 ||
        now.compileFailed != before.compileFailed ||
        now.publishFailed != before.publishFailed) {
      SRE_printf("[MFS] FAIL compile/publish failure\n");
      return mfs_fail_run(-6);
    }
    if (witnessed == 0xffffu)
      break;
    (void)SRE_TaskDelay(10u);
  }
  if (witnessed != 0xffffu) {
    SRE_printf("[MFS] FAIL T2 execution timeout mask=%x\n", witnessed);
    return mfs_fail_run(-7);
  }

  rc = mfs_verify_tier2(MFS_VERIFY_ROUNDS);
  if (rc == 0)
    rc = mfs_verify_cold_stats();
  if (rc != 0)
    return mfs_fail_run(rc);
  __atomic_store_n(&g_mfs_run_state, MFS_RUN_COMPLETE, __ATOMIC_RELEASE);
  SRE_printf("[MFS] PASS 16 hot witnesses, first cold execution, "
             "%u repeated rounds\n",
             MFS_VERIFY_ROUNDS);
  return 0;
}

int test_ejit_mfs(void) {
  uint32_t core = g_ucLocalCoreID;
  if (core != MFS_WORKER_CORE && core != MFS_PRODUCER_CORE)
    return -1;

  uint32_t runState = __atomic_load_n(&g_mfs_run_state, __ATOMIC_ACQUIRE);
  if (runState == MFS_RUN_FAILED)
    return MFS_RC_PREVIOUS_FAILED;
  if (core == MFS_WORKER_CORE) {
    if (runState == MFS_RUN_COMPLETE)
      return test_ejit_mfs_print();
    int rc = mfs_setup_current_core(core);
    if (rc != 0)
      return mfs_fail_run(rc);
    if (__atomic_load_n(&g_mfs_config_ready, __ATOMIC_ACQUIRE) == 0u) {
      for (uint32_t cell = 0; cell < 16; ++cell) {
        g_mfs_config[cell].bias = cell + 31u;
        g_mfs_hot_pc[cell] = 0;
        g_mfs_cold_pc[cell] = 0;
      }
      __atomic_thread_fence(__ATOMIC_RELEASE);
      __atomic_store_n(&g_mfs_config_ready, 1u, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&g_mfs_worker_armed, 1u, __ATOMIC_RELEASE);
    SRE_printf("[MFS] worker6 ready; run producer16\n");
    return 0;
  }

  uint32_t expected = 0u;
  if (!__atomic_compare_exchange_n(&g_mfs_producer_command_active, &expected,
                                   1u, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[MFS] producer command already active; duplicate rejected\n");
    return -4;
  }
  int rc = mfs_run_producer();
  __atomic_store_n(&g_mfs_producer_command_active, 0u, __ATOMIC_RELEASE);
  return rc;
}
