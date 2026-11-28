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
// break the entry stub exists to catch at the call site. probeBias is read
// with atomic semantics (__atomic_load_n): substitution never folds atomics
// and the marker never rides atomic loads, so it survives to the final
// module as an unfolded annotated field — exactly the group the stub's
// drift half observes on every activated call.
struct AsanBoundCfg {
  EJIT_PERIOD_CONST uint32_t scale; // +0: family-consistent, folded
  EJIT_PERIOD_CONST uint32_t probeBias; // +4: atomic read → drift site
  uint32_t runtimeBias;                  // +8: per-member, never annotated
};

__attribute__((section(".mc_shared"), aligned(64)))
struct AsanBoundCfg gAsanFamilyA = {7u, 0u, 100u};
__attribute__((section(".mc_shared"), aligned(64)))
struct AsanBoundCfg gAsanFamilyB = {9u, 0u, 100u};

EJIT_ENTRY __attribute__((noinline))
uint32_t ejit_asan_bound_probe(EJIT_DIM(asan_cell) uint8_t cell,
                               EJIT_BOUND_PTR(asan_cell)
                                   const struct AsanBoundCfg *cfg,
                               uint32_t input) {
  return cfg->scale * input + cell +
         __atomic_load_n(&cfg->probeBias, __ATOMIC_ACQUIRE);
}

int test_ejit_asan_start(void) {
  if (ejit_sre_current_core_id() != 6)
    return -1;
  ejit_set_log_level(EJIT_LOG_INFO);
  // ASAN test mode: the plain pipeline (PGO off) with the bound_ptr entry
  // stubs forced on (see EJIT_ASAN_CHECK.md).
  return ejit_init_asan(0);
}

int test_ejit_asan_sample(void) {
  if (ejit_sre_current_core_id() != 16 || ejit_init_asan(0) != EJIT_OK)
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

// The drift half of the entry stub: probeBias never folds, so the stub
// observes it through the CURRENT bound argument on every activated call.
// Move FamilyB's copy away from the compile-time baseline (0): every call
// now returns the REAL value (7i+1+3) and the dump reports a kind=drift
// row whose object=bound@<FamilyB> names the member that moved.
int test_ejit_asan_drift_inject(void) {
  if (ejit_sre_current_core_id() != 16 ||
      !ejit_is_active("asan_cell", 1))
    return -1;
  __atomic_store_n(&gAsanFamilyB.probeBias, 3u, __ATOMIC_RELEASE);
  for (uint32_t i = 1; i <= 512; ++i)
    if (ejit_asan_bound_probe(1, &gAsanFamilyB, i) != 7u * i + 1u + 3u)
      return -2;
  return 0;
}

// The full-scope half: cell 0's annotated fields are covered by no folded
// watch (the probe specializes cell 1), so the worker's full-scope scan
// samples them directly from the registered object. Change one and let a
// scan slice run (~5 s) before the dump: the row is src=metadata with
// instance=0/2 — the annotation-truthfulness evidence the folded probes
// structurally cannot produce.
int test_ejit_asan_drift_fullscope_inject(void) {
  if (ejit_sre_current_core_id() != 16 ||
      !ejit_is_active("asan_cell", 1))
    return -1;
  __atomic_store_n(&gEJitAsanCells[0].slot5[2].value, 23,
                   __ATOMIC_RELEASE);
  return 0;
}
