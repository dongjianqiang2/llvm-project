// Build with normal spec5 Clang and a verifier-enabled runtime; the demo
// period is asan_cell. No new Clang flag.
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

__attribute__((section(".mc_shared"), aligned(64)))
EJIT_GROUP(asan_cell)
struct VerifyCell gEJitAsanCells[2];

EJIT_ENTRY __attribute__((noinline))
uint32_t ejit_asan_probe(EJIT_DIM(asan_cell) uint8_t cell,
                                 EJIT_FREE_DIM uint16_t slot, uint32_t token) {
  return gEJitAsanCells[cell].slot5[slot % 5].value +
         gEJitAsanCells[cell].slot10[slot % 10].value + token;
}

// Entry-instrumentation demo: a bound_ptr "family" of objects whose
// period_const members are promised consistent across every pointer the
// product may pass. FamilyA is the compile-time exemplar (scale=7); FamilyB
// carries the same layout but a divergent scale (9) — the family-consistency
// break the entry stub exists to catch at the call site.
struct AsanBoundCfg {
  EJIT_PERIOD_CONST uint32_t scale; // +0: family-consistent, folded
  uint32_t runtimeBias;             // +4: per-member, never folded
};

__attribute__((section(".mc_shared"), aligned(64)))
struct AsanBoundCfg gAsanFamilyA = {7u, 100u};
__attribute__((section(".mc_shared"), aligned(64)))
struct AsanBoundCfg gAsanFamilyB = {9u, 100u};

EJIT_ENTRY __attribute__((noinline))
uint32_t ejit_asan_bound_probe(EJIT_DIM(asan_cell) uint8_t cell,
                               EJIT_BOUND_PTR(asan_cell)
                                   const struct AsanBoundCfg *cfg,
                               uint32_t input) {
  return cfg->scale * input + cell;
}

int test_ejit_asan_start(void) {
  if (ejit_sre_current_core_id() != 6)
    return -1;
  ejit_set_log_level(EJIT_LOG_INFO);
  // Must precede ejit_init: subsequent worker compiles emit the
  // bound_ptr entry stubs (see EJIT_ASAN_CHECK.md).
  ejit_status_t st = ejit_enable_asan_mode();
  if (st != EJIT_OK)
    return st;
  return ejit_init_pgo(0);
}

int test_ejit_asan_sample(void) {
  if (ejit_sre_current_core_id() != 16 || ejit_init_pgo(0) != EJIT_OK)
    return -1;
  for (uint32_t i = 0; i != 5; ++i)
    __atomic_store_n(&gEJitAsanCells[1].slot5[i].value, 7,
                     __ATOMIC_RELEASE);
  for (uint32_t i = 0; i != 10; ++i)
    __atomic_store_n(&gEJitAsanCells[1].slot10[i].value, 9,
                     __ATOMIC_RELEASE);
  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK ||
      ejit_activate("asan_cell", 1) != EJIT_OK)
    return -2;
  if (ejit_asan_probe(1, 0, 0) != 16)
    return -3;
  ejit_status_t publish = ejit_publish_pending_code();
  if (publish != EJIT_OK && publish != EJIT_ERR_DISABLED)
    return -4;
  for (uint32_t i = 1; i <= 65536; ++i) {
    if (ejit_asan_probe(1, (uint16_t)i, i) != i + 16)
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
int test_ejit_asan_inject(void) {
  if (ejit_sre_current_core_id() != 16 ||
      !ejit_is_active("asan_cell", 1))
    return -1;
  __atomic_store_n(&gEJitAsanCells[1].slot5[4].value, 17,
                   __ATOMIC_RELEASE);
  __atomic_store_n(&gEJitAsanCells[1].slot10[9].value, 19,
                   __ATOMIC_RELEASE);
  return 0;
}

// Compile the bound probe from the family exemplar (FamilyA). The AOT body
// and the specialization agree on FamilyA, so the return value cannot tell
// them apart; completion is observed through the taskpool stats.
int test_ejit_asan_bound_sample(void) {
  if (ejit_sre_current_core_id() != 16 ||
      !ejit_is_active("asan_cell", 1))
    return -1;
  ejit_taskpool_stats_t before = {0};
  if (ejit_taskpool_get_stats(&before) != EJIT_OK)
    return -2;
  (void)ejit_asan_bound_probe(1, &gAsanFamilyA, 3);
  for (uint32_t i = 1; i <= 65536; ++i) {
    if (ejit_asan_bound_probe(1, &gAsanFamilyA, i) != 7u * i + 1u)
      return -3;
    if ((i & 15) != 0)
      continue;
    ejit_taskpool_stats_t now = {0};
    if (ejit_taskpool_get_stats(&now) != EJIT_OK ||
        now.compileFailed != before.compileFailed ||
        now.publishFailed != before.publishFailed)
      return -4;
    if (now.asyncCompiles >= before.asyncCompiles + 1 &&
        now.cacheHits > before.cacheHits)
      return 0;
    (void)SRE_TaskDelay(1);
  }
  return -5;
}

// The family break: call the bound probe with the divergent member
// FamilyB. Report-only: every call still returns the BAKED computation
// (scale=7), which is exactly the wrong-answer symptom the entry stub is
// there to surface. Only this function is instrumented in the demo, so its
// slot is the only one: the rotation serves it a check on the very first
// call after any re-arm, regardless of window state.
int test_ejit_asan_bound_inject(void) {
  if (ejit_sre_current_core_id() != 16 ||
      !ejit_is_active("asan_cell", 1))
    return -1;
  for (uint32_t i = 1; i <= 512; ++i)
    if (ejit_asan_bound_probe(1, &gAsanFamilyB, i) != 7u * i + 1u)
      return -2;
  return 0;
}
