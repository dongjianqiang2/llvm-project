// Run once after a fresh boot: core 16 pre-worker, core 6 worker, core 16 producer.
// Build with the worker-only Clang/runtime and -mllvm -ejit-inline-cache.
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stdint.h>

extern uint32_t SRE_TaskDelay(uint32_t ticks);
extern uint32_t ejit_sre_current_core_id(void);

struct ZeroInitConfig {
  EJIT_PERIOD_CONST uint32_t bias;
};
EJIT_GROUP(zero_init_cell) struct ZeroInitConfig gZeroInitConfig[2] = {{3}, {7}};

EJIT_ENTRY __attribute__((noinline))
uint32_t ejit_zero_init_probe(EJIT_DIM(zero_init_cell) uint32_t cell,
                              uint32_t value) {
  return gZeroInitConfig[cell].bias + value;
}

int test_ejit_zero_init_before_worker(void) {
  if (ejit_sre_current_core_id() != 16 ||
      ejit_taskpool_get_worker_core() != UINT32_MAX)
    return -1;
  if (ejit_activate("zero_init_cell", 1) != EJIT_ERR_NOT_ACTIVE ||
      ejit_is_active("zero_init_cell", 1) || ejit_taskpool_pending_count())
    return -2;
  return ejit_zero_init_probe(1, 5) == 12 ? 0 : -3;
}

int test_ejit_zero_init_worker(void) {
  if (ejit_sre_current_core_id() != 6)
    return -1;
  return ejit_init_pgo(0);
}

int test_ejit_zero_init_producer(void) {
  if (ejit_sre_current_core_id() != 16 || ejit_taskpool_get_worker_core() != 6)
    return -1;
  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK ||
      ejit_activate("zero_init_cell", 1) != EJIT_OK ||
      !ejit_is_active("zero_init_cell", 1))
    return -2;
  for (uint32_t i = 0; i < 4096; ++i) {
    if (ejit_zero_init_probe(1, i) != i + 7)
      return -3;
    if ((i & 7) != 7)
      continue;
    ejit_taskpool_stats_t now = {0};
    if (ejit_taskpool_get_stats(&now) != EJIT_OK ||
        now.compileFailed != before.compileFailed ||
        now.publishFailed != before.publishFailed)
      return -4;
    if (now.asyncEnqueues > before.asyncEnqueues &&
        now.asyncCompiles >= before.asyncCompiles + 2 &&
        now.cacheHits > before.cacheHits) {
      if (ejit_deactivate("zero_init_cell", 1) != EJIT_OK ||
          ejit_is_active("zero_init_cell", 1) ||
          ejit_zero_init_probe(1, 5) != 12)
        return -5;
      return 0;
    }
    (void)SRE_TaskDelay(1);
  }
  return -6;
}
