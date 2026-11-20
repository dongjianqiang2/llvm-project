/**
 * Switch-case mode end to end (jit_design_doc/EJIT_SWITCH_CASE.md §1.1).
 *
 * process() folds its cell through ejit_dim as usual. Its slot number is an
 * ejit_runtime_dim: the JIT finds `slotNo % 3`, and builds one arm per slot
 * inside cell 3's single specialization.
 *
 * Built only against a runtime with EJIT_SWITCH_CASE (see build.sh).
 *
 * To see which code ran, the test briefly changes slot data WITHOUT a
 * deactivate, which breaks the period contract on purpose. Arms froze the
 * old values; the AOT body and the default arm read the new ones. A proper
 * deactivate / modify / activate then rebuilds the arms from the new data.
 */
#include <stdint.h>
#include <stdio.h>

#include "ejit_test_helpers.h"

typedef struct {
  ejit_may_const uint32_t shift;
  ejit_may_const uint32_t scale;
  ejit_may_const uint32_t mode;
} SlotCfg;

typedef struct {
  ejit_may_const uint32_t len;
  ejit_may_const int32_t coef[4];
  ejit_may_const int32_t clip;
  SlotCfg slot[3];
} CellCfg;

ejit_period_arr(cell) CellCfg g_cellCfg[8];

ejit_entry int32_t process(ejit_period_arr_ind(cell) uint8_t cellIndex,
                           ejit_runtime_dim uint32_t slotNo,
                           const int16_t *in) {
  const CellCfg *c = &g_cellCfg[cellIndex];

  int32_t acc = 0; /* part A: cell only */
  for (uint32_t i = 0; i < c->len; ++i)
    acc += in[i] * c->coef[i];
  if (acc > c->clip)
    acc = c->clip;

  const SlotCfg *s = &c->slot[slotNo % 3]; /* part B: slot */
  int32_t r = acc >> s->shift;
  if (s->mode)
    r *= s->scale;
  return r + slotNo;
}

/* Not an entry: the same computation over a snapshot, for expected values. */
static int32_t reference(const CellCfg *c, uint32_t slotNo,
                         const int16_t *in) {
  int32_t acc = 0;
  for (uint32_t i = 0; i < c->len; ++i)
    acc += in[i] * c->coef[i];
  if (acc > c->clip)
    acc = c->clip;
  const SlotCfg *s = &c->slot[slotNo % 3];
  int32_t r = acc >> s->shift;
  if (s->mode)
    r *= s->scale;
  return r + (int32_t)slotNo;
}

static const uint32_t kSlots[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 1000000};
#define NUM_SLOTS (sizeof(kSlots) / sizeof(kSlots[0]))

static int check_all(const char *phase, const CellCfg *expect,
                     const int16_t *in) {
  int failures = 0;
  for (unsigned i = 0; i < NUM_SLOTS; ++i) {
    int32_t got = process(3, kSlots[i], in);
    int32_t want = reference(expect, kSlots[i], in);
    if (got != want) {
      printf("  FAIL %s: slot=%u got=%d want=%d\n", phase, kSlots[i], got,
             want);
      ++failures;
    }
  }
  if (!failures)
    printf("  OK: %s (%u slots)\n", phase, (unsigned)NUM_SLOTS);
  return failures;
}

static void set_cell3(void) {
  CellCfg *c = &g_cellCfg[3];
  c->len = 4;
  c->coef[0] = 1;
  c->coef[1] = -2;
  c->coef[2] = 3;
  c->coef[3] = 1;
  c->clip = 1000;
  c->slot[0] = (SlotCfg){2, 3, 1};
  c->slot[1] = (SlotCfg){0, 1, 0};
  c->slot[2] = (SlotCfg){4, 5, 1};
}

int main(void) {
  const int16_t in[4] = {100, 20, 30, 7};
  int failures = 0;

  printf("=== Switch-Case Mode Test ===\n");
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if ((int)ejit_init(&cfg) != 0) {
    printf("FAIL: init\n");
    return 1;
  }

  set_cell3();
  ejit_activate("cell", 3);
  (void)process(3, 0, in); /* submits the compile */
  ejit_drain_taskpool();

  CellCfg compiled = g_cellCfg[3];
  failures += check_all("specialized", &compiled, in);

  /* acc = 100 - 40 + 90 + 7 = 157; slot 5 is arm 2: (157 >> 4) * 5 + 5. */
  int32_t r5 = process(3, 5, in);
  if (r5 != 50) {
    printf("  FAIL: process(3, 5)=%d (expected 50)\n", r5);
    ++failures;
  } else {
    printf("  OK: slot 5 -> arm 2, r + 5 = %d\n", r5);
  }

  /* Contract broken on purpose: every slot must still return the
   * compiled-in configuration, so every key ran an arm. */
  for (int k = 0; k < 3; ++k)
    g_cellCfg[3].slot[k] = (SlotCfg){1, 7, 1};
  failures += check_all("arms kept the compiled-in slot data", &compiled, in);
  g_cellCfg[3] = compiled;

  /* Configuration churn: the whole specialization, arms included, is
   * rebuilt from the new data. */
  ejit_deactivate("cell", 3);
  g_cellCfg[3].slot[1] = (SlotCfg){3, 2, 1};
  ejit_activate("cell", 3);
  (void)process(3, 0, in);
  ejit_drain_taskpool();
  CellCfg updated = g_cellCfg[3];
  failures += check_all("rebuilt after deactivate/activate", &updated, in);

  printf("\n=== Result: %d failures ===\n", failures);
  ejit_shutdown();
  return failures > 0 ? 1 : 0;
}
