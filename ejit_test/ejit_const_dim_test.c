/**
 * EJIT ejit_const_dim runtime test
 *
 * The driving case: a composite index `trpIdx * N_SLOTS + slotNo % N_SLOTS`.
 * Without ejit_const_dim, slotNo keeps the whole index non-constant, PASS6
 * cannot attribute any load to a fixed field offset, and NONE of the
 * may_const fields fold -- not even the ones whose values never depended on
 * slotNo. Marking slotNo ejit_const_dim makes the sum constant and unlocks all
 * of them.
 *
 * The assertions are written so that a FAILURE TO SPECIALIZE IS DETECTED, not
 * merely a wrong answer: each entry reads a may_const field, then the test
 * mutates that field WITHOUT a deactivate/activate cycle and calls again. A
 * specialized clone keeps returning the value baked in at compile time; an
 * unspecialized AOT body sees the new value. That difference is the proof the
 * fold happened.
 *
 * Run:
 *   ./ejit_const_dim 3 7   # trpIdx=3 slotNo=7
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#include "ejit_test_helpers.h"

//===-- Period array indexed by a COMPOSITE (trp, slot) index ---------------===//

#define N_SLOTS 5
#define N_TRP   8

struct TrpAlgoPara {
  ejit_may_const uint32_t cpriMode;
  ejit_may_const uint32_t realTxAntNum;
  ejit_may_const uint32_t featureBitmap;
  uint32_t usageCount;
};

ejit_period_arr(trp) struct TrpAlgoPara g_trpAlgoPara[N_TRP * N_SLOTS];

//===-- JIT entry: the composite index --------------------------------------===//

ejit_entry
uint32_t init_trp_para(ejit_period_arr_ind(trp) uint8_t trpIdx,
                       ejit_const_dim uint8_t slotNo)
{
  struct TrpAlgoPara *p = &g_trpAlgoPara[trpIdx * N_SLOTS + slotNo % N_SLOTS];
  uint32_t r = 0;
  if (p->cpriMode == 0xC1)
    r += 1000;
  else if (p->cpriMode == 0xC2)
    r += 2000;
  r += p->realTxAntNum * 10;
  r += p->featureBitmap & 0xF;
  return r;
}

//===-- JIT entry: a const dim with NO period array at all ------------------===//
// Pure constant folding + dead-branch elimination. ejit_dim could not express
// this: there is no period array to hang it off.

ejit_entry
uint32_t slots_per_frame(ejit_const_dim uint8_t numerology)
{
  uint32_t slots = 10u << numerology;
  if (numerology >= 2)
    slots += 5;
  return slots;
}

//===-- Assertions ----------------------------------------------------------===//

static int g_fail = 0;
#define T(cond, fmt, ...) do {                                   \
  if (cond) printf("  OK   " fmt "\n", ##__VA_ARGS__);           \
  else      printf("  FAIL " fmt "\n", ##__VA_ARGS__), g_fail++; \
} while(0)

static void seed(uint32_t idx, uint32_t cpri, uint32_t ant, uint32_t bits) {
  g_trpAlgoPara[idx].cpriMode      = cpri;
  g_trpAlgoPara[idx].realTxAntNum  = ant;
  g_trpAlgoPara[idx].featureBitmap = bits;
}

static uint32_t expected(uint32_t cpri, uint32_t ant, uint32_t bits) {
  uint32_t r = 0;
  if (cpri == 0xC1) r += 1000;
  else if (cpri == 0xC2) r += 2000;
  return r + ant * 10 + (bits & 0xF);
}

int main(int argc, char **argv) {
  uint8_t trpIdx = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 3;
  uint8_t slotNo = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 7;

  printf("=== EJIT const-dim Test ===\n");
  printf("trpIdx=%u slotNo=%u\n\n", trpIdx, slotNo);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  ejit_init(&cfg);

  const uint32_t cell = trpIdx * N_SLOTS + slotNo % N_SLOTS;

  //===-- A: the composite index folds, and may_const actually specializes --===//
  printf("--- A: init_trp_para(%u, %u) ---\n", trpIdx, slotNo);
  seed(cell, 0xC1, 4, 0x7);
  ejit_activate("trp", trpIdx);

  uint32_t r = init_trp_para(trpIdx, slotNo);
  uint32_t exp_a = expected(0xC1, 4, 0x7);
  T(r == exp_a, "init_trp_para = %u (expected %u)", r, exp_a);
  ejit_drain_taskpool();

  // Re-read after the specialization is published: same answer, now from the
  // JIT clone rather than the AOT body.
  r = init_trp_para(trpIdx, slotNo);
  T(r == exp_a, "init_trp_para (specialized) = %u (expected %u)", r, exp_a);

  //===-- B: the may_const fields really were baked in ----------------------===//
  // Mutate WITHOUT deactivate/activate. A specialized clone is frozen at the
  // values seen when it compiled; only an unspecialized body would follow the
  // new ones. This is what distinguishes "it compiled" from "it specialized".
  printf("\n--- B: frozen may_const (no lifecycle cycle) ---\n");
  seed(cell, 0xC2, 9, 0xF);
  r = init_trp_para(trpIdx, slotNo);
  T(r == exp_a,
    "init_trp_para still = %u (frozen; unspecialized would give %u)",
    r, expected(0xC2, 9, 0xF));

  //===-- C: a lifecycle cycle DOES pick the new values up ------------------===//
  // The trp lifecycle still owns this data: deactivate/activate must drop the
  // clone and recompile against the new values. The const dim does not block
  // that -- it has no lifecycle of its own, but it is not a barrier to the one
  // that does.
  printf("\n--- C: deactivate/activate re-specializes ---\n");
  ejit_deactivate("trp", trpIdx);
  ejit_activate("trp", trpIdx);
  r = init_trp_para(trpIdx, slotNo);
  ejit_drain_taskpool();
  r = init_trp_para(trpIdx, slotNo);
  uint32_t exp_c = expected(0xC2, 9, 0xF);
  T(r == exp_c, "init_trp_para after re-activate = %u (expected %u)", r, exp_c);

  //===-- D: a different slotNo is a DIFFERENT specialization ---------------===//
  // The §1 invariant, end to end: the const dim's value is part of the cache
  // key, so another value gets its own clone against its own array element --
  // it must NOT be served the clone compiled for slotNo.
  printf("\n--- D: a second slotNo gets its own clone ---\n");
  uint8_t slotNo2 = (uint8_t)((slotNo + 1) % N_SLOTS);
  uint32_t cell2 = trpIdx * N_SLOTS + slotNo2 % N_SLOTS;
  if (cell2 != cell) {
    seed(cell2, 0xC1, 1, 0x2);
    r = init_trp_para(trpIdx, slotNo2);
    ejit_drain_taskpool();
    r = init_trp_para(trpIdx, slotNo2);
    uint32_t exp_d = expected(0xC1, 1, 0x2);
    T(r == exp_d, "init_trp_para(slotNo=%u) = %u (expected %u)", slotNo2, r,
      exp_d);
    // And the original identity still answers with ITS values.
    uint32_t r0 = init_trp_para(trpIdx, slotNo);
    T(r0 == exp_c, "init_trp_para(slotNo=%u) still = %u (expected %u)", slotNo,
      r0, exp_c);
  } else {
    printf("  SKIP slotNo2 aliases slotNo\n");
  }

  //===-- E: a const dim with no period array behind it ---------------------===//
  printf("\n--- E: slots_per_frame (no period array) ---\n");
  for (uint8_t n = 0; n <= 3; n++) {
    uint32_t got = slots_per_frame(n);
    ejit_drain_taskpool();
    got = slots_per_frame(n);
    uint32_t exp_e = (10u << n) + (n >= 2 ? 5u : 0u);
    T(got == exp_e, "slots_per_frame(%u) = %u (expected %u)", n, got, exp_e);
  }

  //===-- F: an out-of-range const dim still answers correctly --------------===//
  // Beyond the inline cache's per-axis bound the probe is guarded off and the
  // value takes the slow path; beyond 255 it is not specialized at all. Either
  // way the ANSWER must stay right -- degradation is a performance outcome,
  // never a correctness one.
  printf("\n--- F: out-of-range const dim falls back correctly ---\n");
  for (uint8_t n = 16; n <= 20; n++) {
    uint32_t got = slots_per_frame(n);
    ejit_drain_taskpool();
    // 10u << n overflows for large n exactly as it does in the AOT body; the
    // point is that the JIT path agrees with plain C.
    uint32_t exp_f = (10u << n) + (n >= 2 ? 5u : 0u);
    T(got == exp_f, "slots_per_frame(%u) = %u (expected %u)", n, got, exp_f);
  }

  printf("\n=== %s ===\n", g_fail == 0 ? "ALL PASS" : "FAILURES");
  ejit_shutdown();
  return g_fail == 0 ? 0 : 1;
}
