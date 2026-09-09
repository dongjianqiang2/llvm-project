/* Host-runnable command-lifecycle regression. EJIT_MFS_HOST_TEST removes the
 * custom attributes and mocks only the public runtime/platform boundary. It
 * is not an MFS property, profile, code-placement, or AArch64 product test. */

#include <stdarg.h>
#include <stdio.h>

#define EJIT_MFS_HOST_TEST 1
#define MFS_WAIT_ROUNDS 4u
#define MFS_VERIFY_ROUNDS 4u
#include "ejit_mfs_zero_count_sre_multicore_test.c"

uint8_t g_ucLocalCoreID;

static uint8_t g_runtime_ready[256];
static ejit_taskpool_stats_t g_stats;
static uint32_t g_init_calls[256];
static uint32_t g_register_calls[256];
static uint32_t g_activate_calls;
static uint32_t g_print_calls;
static uint32_t g_init_array_calls;
static uint32_t g_delay_calls;
static uint32_t g_failures;
static uint32_t g_classify_enabled;
static uint32_t g_delay_mode;

#define DELAY_NORMAL 0u
#define DELAY_COMPILE_FAIL 1u

#define CHECK(condition, message)                                              \
  do {                                                                         \
    if (!(condition)) {                                                        \
      printf("FAIL: %s\n", message);                                           \
      ++g_failures;                                                            \
    }                                                                          \
  } while (0)

void call_init_array_functions(void) { ++g_init_array_calls; }

int ejit_init_pgo(const ejit_config_t *config) {
  (void)config;
  ++g_init_calls[g_ucLocalCoreID];
  g_runtime_ready[g_ucLocalCoreID] = 1u;
  return 0;
}

uint32_t ejit_taskpool_get_worker_core(void) {
  return g_runtime_ready[g_ucLocalCoreID] ? MFS_WORKER_CORE : 0xffffffffu;
}

void ejit_register_symbol(const char *name, void *address) {
  (void)name;
  (void)address;
  ++g_register_calls[g_ucLocalCoreID];
}

int ejit_activate(const char *name, uint32_t instance) {
  (void)name;
  (void)instance;
  ++g_activate_calls;
  return 0;
}

int ejit_taskpool_get_stats(ejit_taskpool_stats_t *out) {
  *out = g_stats;
  return 0;
}

uint32_t ejit_taskpool_classify_tier2_pc(uintptr_t pc) {
  if (!g_classify_enabled)
    return 0u;
  for (uint32_t cell = 0; cell < 16; ++cell)
    if (g_mfs_hot_pc[cell] == pc)
      return 1u;
  for (uint32_t cell = 0; cell < 16; ++cell)
    if (g_mfs_cold_pc[cell] == pc)
      return MFS_EXPECT_SPLIT ? 2u : 1u;
  return 0u;
}

int ejit_get_cold_code_pool_stats(ejit_code_pool_stats_t *out) {
  ejit_code_pool_stats_t empty = {0};
  *out = empty;
  out->usedBytes = MFS_EXPECT_SPLIT ? 4096u : 0u;
  return 0;
}

void ejit_taskpool_print_compiled(void) { ++g_print_calls; }
void ejit_print_code_pool_stats(void) { ++g_print_calls; }
void ejit_taskpool_print_stats(void) { ++g_print_calls; }
void SRE_printf(const char *format, ...) { (void)format; }

uint32_t SRE_TaskDelay(uint32_t ticks) {
  (void)ticks;
  ++g_delay_calls;
  if (g_delay_mode == DELAY_COMPILE_FAIL)
    g_stats.compileFailed = 1u;
  return 0u;
}

static int run_on(uint8_t core) {
  g_ucLocalCoreID = core;
  return test_ejit_mfs();
}

static void reset_fixture(uint32_t classify, uint32_t delayMode) {
  for (uint32_t i = 0; i < 256; ++i) {
    g_runtime_ready[i] = 0u;
    g_init_calls[i] = 0u;
    g_register_calls[i] = 0u;
  }
  ejit_taskpool_stats_t empty = {0};
  g_stats = empty;
  g_activate_calls = 0u;
  g_print_calls = 0u;
  g_init_array_calls = 0u;
  g_delay_calls = 0u;
  g_classify_enabled = classify;
  g_delay_mode = delayMode;
  for (uint32_t cell = 0; cell < 16; ++cell) {
    g_mfs_config[cell].bias = 0u;
    g_mfs_hot_pc[cell] = 0;
    g_mfs_cold_pc[cell] = 0;
  }
  g_mfs_setup_state[0] = MFS_SETUP_IDLE;
  g_mfs_setup_state[1] = MFS_SETUP_IDLE;
  g_mfs_config_ready = 0u;
  g_mfs_worker_armed = 0u;
  g_mfs_run_state = MFS_RUN_IDLE;
  g_mfs_producer_command_active = 0u;
}

int main(void) {
  reset_fixture(1u, DELAY_NORMAL);
  CHECK(run_on(3u) == -1, "wrong core rejected before setup");
  CHECK(g_init_calls[3] == 0u && g_register_calls[3] == 0u,
        "wrong core has no setup side effects");

  CHECK(run_on(MFS_WORKER_CORE) == 0, "first worker setup");
  CHECK(g_init_calls[MFS_WORKER_CORE] == 1u, "worker runtime initialized once");
  CHECK(g_register_calls[MFS_WORKER_CORE] == 4u,
        "worker symbols registered once");
  CHECK(g_mfs_config_ready == 1u && g_mfs_worker_armed == 1u,
        "worker config and readiness published");
  uint32_t savedBias = g_mfs_config[5].bias;
  g_mfs_hot_pc[5] = 0x1234u;
  CHECK(run_on(MFS_WORKER_CORE) == 0, "repeated worker setup");
  CHECK(g_init_calls[MFS_WORKER_CORE] == 1u &&
            g_register_calls[MFS_WORKER_CORE] == 4u,
        "repeated worker does not initialize or register");
  CHECK(g_mfs_config[5].bias == savedBias && g_mfs_hot_pc[5] == 0x1234u,
        "repeated worker preserves config and witnesses");

  g_mfs_producer_command_active = 1u;
  CHECK(run_on(MFS_PRODUCER_CORE) == -4,
        "concurrent producer command rejected");
  CHECK(g_init_calls[MFS_PRODUCER_CORE] == 0u &&
            g_register_calls[MFS_PRODUCER_CORE] == 0u,
        "rejected producer has no setup side effects");
  g_mfs_producer_command_active = 0u;

  CHECK(run_on(MFS_PRODUCER_CORE) == 0, "first producer completion");
  CHECK(g_mfs_run_state == MFS_RUN_COMPLETE, "run completed");
  CHECK(g_init_calls[MFS_PRODUCER_CORE] == 1u &&
            g_register_calls[MFS_PRODUCER_CORE] == 4u,
        "producer setup performed once");
  CHECK(g_activate_calls == 16u, "all identities activated once");
  CHECK(g_init_array_calls == 0u,
        "repeatable commands never invoke product init-array");

  const uint32_t initBeforeRepeat = g_init_calls[MFS_PRODUCER_CORE];
  const uint32_t registerBeforeRepeat = g_register_calls[MFS_PRODUCER_CORE];
  const uint32_t activateBeforeRepeat = g_activate_calls;
  CHECK(test_ejit_mfs_print() == 0 && test_ejit_mfs_print() == 0,
        "read-only print repeats");
  CHECK(g_print_calls == 6u, "both print commands emit all three views");
  CHECK(run_on(MFS_PRODUCER_CORE) == 0,
        "completed producer validates existing Tier-2 code");
  CHECK(g_init_calls[MFS_PRODUCER_CORE] == initBeforeRepeat &&
            g_register_calls[MFS_PRODUCER_CORE] == registerBeforeRepeat &&
            g_activate_calls == activateBeforeRepeat,
        "completed producer requires no new setup or activation");
  CHECK(run_on(MFS_WORKER_CORE) == 0, "completed worker command is read-only");
  CHECK(g_init_calls[MFS_WORKER_CORE] == 1u &&
            g_register_calls[MFS_WORKER_CORE] == 4u,
        "completed worker preserves live setup");
  CHECK(g_runtime_ready[MFS_WORKER_CORE] != 0u,
        "worker remains live after repeated commands");

  reset_fixture(0u, DELAY_COMPILE_FAIL);
  CHECK(run_on(MFS_WORKER_CORE) == 0, "failure case worker setup");
  CHECK(run_on(MFS_PRODUCER_CORE) == -6,
        "compile failure enters terminal state");
  CHECK(g_mfs_run_state == MFS_RUN_FAILED, "compile failure is terminal");
  uint32_t failureDelays = g_delay_calls;
  uint32_t failureInits = g_init_calls[MFS_PRODUCER_CORE];
  CHECK(run_on(MFS_PRODUCER_CORE) == MFS_RC_PREVIOUS_FAILED,
        "failed run does not restart");
  CHECK(g_delay_calls == failureDelays &&
            g_init_calls[MFS_PRODUCER_CORE] == failureInits,
        "failed rerun neither waits nor initializes");
  CHECK(test_ejit_mfs_print() == 0 && g_mfs_run_state == MFS_RUN_FAILED,
        "print is read-only after failure");

  reset_fixture(0u, DELAY_NORMAL);
  CHECK(run_on(MFS_WORKER_CORE) == 0, "timeout case worker setup");
  CHECK(run_on(MFS_PRODUCER_CORE) == -7, "timeout is reported");
  CHECK(g_mfs_run_state == MFS_RUN_FAILED, "timeout is terminal");
  uint32_t timeoutDelays = g_delay_calls;
  uint32_t timeoutInits = g_init_calls[MFS_PRODUCER_CORE];
  CHECK(run_on(MFS_PRODUCER_CORE) == MFS_RC_PREVIOUS_FAILED,
        "timed-out run does not restart");
  CHECK(g_delay_calls == timeoutDelays &&
            g_init_calls[MFS_PRODUCER_CORE] == timeoutInits,
        "timeout rerun neither waits nor initializes");

  if (g_failures != 0u)
    return 1;
  printf("PASS: MFS board command lifecycle only (not real PGO)\n");
  return 0;
}
