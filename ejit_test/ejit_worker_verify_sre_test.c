// Build with normal spec5 Clang. Use a verifier runtime configured with
// EJIT_WORKER_VERIFY_PERIOD=worker_verify_cell. No new Clang flag.
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#include <stdint.h>

extern uint32_t ejit_sre_current_core_id(void);
extern uint32_t SRE_TaskDelay(uint32_t ticks);

struct VerifySlot {
  EJIT_PERIOD_CONST uint32_t value;
};

struct VerifyCell {
  struct VerifySlot slot5[5];
  struct VerifySlot slot10[10];
};

EJIT_IN_PERIOD_ARRAY(worker_verify_cell)
__attribute__((section(".mc_shared"), aligned(64)))
struct VerifyCell gEJitWorkerVerifyCells[2];

ejit_entry __attribute__((noinline))
uint32_t ejit_worker_verify_probe(EJIT_DIM(worker_verify_cell) uint8_t cell,
                                 ejit_free_dim uint16_t slot, uint32_t token) {
  return gEJitWorkerVerifyCells[cell].slot5[slot % 5].value +
         gEJitWorkerVerifyCells[cell].slot10[slot % 10].value + token;
}

int test_ejit_worker_verify_start(void) {
  if (ejit_sre_current_core_id() != 6)
    return -1;
  ejit_set_log_level(EJIT_LOG_INFO);
  return ejit_init_pgo(0);
}

int test_ejit_worker_verify_sample(void) {
  if (ejit_sre_current_core_id() != 16 || ejit_init_pgo(0) != EJIT_OK)
    return -1;
  for (uint32_t i = 0; i != 5; ++i)
    __atomic_store_n(&gEJitWorkerVerifyCells[1].slot5[i].value, 7,
                     __ATOMIC_RELEASE);
  for (uint32_t i = 0; i != 10; ++i)
    __atomic_store_n(&gEJitWorkerVerifyCells[1].slot10[i].value, 9,
                     __ATOMIC_RELEASE);
  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK ||
      ejit_activate("worker_verify_cell", 1) != EJIT_OK)
    return -2;
  if (ejit_worker_verify_probe(1, 0, 0) != 16)
    return -3;
  ejit_status_t publish = ejit_publish_pending_code();
  if (publish != EJIT_OK && publish != EJIT_ERR_DISABLED)
    return -4;
  for (uint32_t i = 1; i <= 65536; ++i) {
    if (ejit_worker_verify_probe(1, (uint16_t)i, i) != i + 16)
      return -5;
    if ((i & 15) != 0)
      continue;
    ejit_taskpool_stats_t now = {0};
    if (ejit_taskpool_get_stats(&now) != EJIT_OK ||
        now.compileFailed != before.compileFailed ||
        now.publishFailed != before.publishFailed)
      return -6;
    if (now.asyncCompiles >= before.asyncCompiles + 2 &&
        now.cacheHits > before.cacheHits)
      return 0;
    (void)SRE_TaskDelay(1);
  }
  return -7;
}

// Diagnostic injection. Leave the period active and do not call the probe.
int test_ejit_worker_verify_inject(void) {
  if (ejit_sre_current_core_id() != 16 ||
      !ejit_is_active("worker_verify_cell", 1))
    return -1;
  __atomic_store_n(&gEJitWorkerVerifyCells[1].slot5[4].value, 17,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&gEJitWorkerVerifyCells[1].slot10[9].value, 19,
                   __ATOMIC_RELEASE);
  return 0;
}
