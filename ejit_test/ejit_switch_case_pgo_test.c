/**
 * Switch-case mode under online PGO (EJIT_SWITCH_CASE.md §10): cold wrapper
 * -> AOT -> Tier-1 -> the runtime's sampling threshold -> Tier-2 -> inline
 * cache, through the ordinary wrapper only; the test just waits.
 *
 * Each JIT tier folds the slot data it was compiled with; the AOT body reads
 * live data. Changing slot data without a deactivate (on purpose) tells them
 * apart. Only shifts and scales change, so Tier-1's profile still fits.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

/* Slot data versions. Modes never change; a mode-1 scale is never 1. With
 * the input below every version gives a distinct (key 0, 1, 2) result. */
static const SlotCfg kVersions[][3] = {
    {{2, 3, 1}, {0, 1, 0}, {4, 5, 1}}, /* 0: first Tier-1 */
    {{1, 2, 1}, {3, 1, 0}, {3, 7, 1}}, /* 1: live while sampling */
    {{0, 9, 1}, {2, 1, 0}, {1, 2, 1}}, /* 2: probe */
    {{3, 4, 1}, {1, 1, 0}, {2, 3, 1}}, /* 3: after deactivate/update */
    {{4, 6, 1}, {4, 1, 0}, {0, 5, 1}}, /* 4: live while sampling */
    {{1, 8, 1}, {2, 1, 0}, {4, 3, 1}}, /* 5: probe */
};
#define NUM_VERSIONS ((int)(sizeof(kVersions) / sizeof(kVersions[0])))

static const uint32_t kSlots[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 1000000};
#define NUM_SLOTS (sizeof(kSlots) / sizeof(kSlots[0]))

static const int16_t kIn[4] = {100, 20, 30, 7};

static int32_t reference(int version, uint32_t slotNo) {
  const CellCfg *c = &g_cellCfg[3];
  int32_t acc = 0;
  for (uint32_t i = 0; i < c->len; ++i)
    acc += kIn[i] * c->coef[i];
  if (acc > c->clip)
    acc = c->clip;
  const SlotCfg *s = &kVersions[version][slotNo % 3];
  int32_t r = acc >> s->shift;
  if (s->mode)
    r *= s->scale;
  return r + (int32_t)slotNo;
}

static unsigned compiles_in_flight(void) {
  ejit_taskpool_stats_t ts;
  memset(&ts, 0, sizeof(ts));
  ejit_taskpool_get_stats(&ts);
  return ts.pendingEntries;
}

/* Never while a compile runs: it could fold a torn mix of two versions. */
static void set_slots(int version) {
  for (int i = 0; i < 60000 && compiles_in_flight(); ++i)
    usleep(2000);
  memcpy(g_cellCfg[3].slot, kVersions[version], sizeof(g_cellCfg[3].slot));
}

/* One call per slot; the version every result agrees with, or -1. */
static int which_version(void) {
  int32_t got[NUM_SLOTS];
  for (unsigned i = 0; i < NUM_SLOTS; ++i)
    got[i] = process(3, kSlots[i], kIn);
  for (int v = 0; v < NUM_VERSIONS; ++v) {
    unsigned i = 0;
    while (i < NUM_SLOTS && got[i] == reference(v, kSlots[i]))
      ++i;
    if (i == NUM_SLOTS)
      return v;
  }
  for (unsigned i = 0; i < NUM_SLOTS; ++i)
    printf("    slot=%u got=%d\n", kSlots[i], got[i]);
  return -1;
}

/* Wait without calling process(), which would count as a sample. */
static int wait_published(const char *what) {
  for (int i = 0; i < 60000; ++i) {
    ejit_taskpool_stats_t ts;
    memset(&ts, 0, sizeof(ts));
    ejit_taskpool_get_stats(&ts);
    if (ts.pendingEntries == 0 && ts.readyEntries >= 1)
      return 0;
    usleep(2000);
  }
  printf("  FAIL: %s was never published\n", what);
  return 1;
}

static int expect(const char *what, int got, int want) {
  if (got == want) {
    printf("  OK: %s (version %d, %u slots)\n", what, got, (unsigned)NUM_SLOTS);
    return 0;
  }
  printf("  FAIL: %s: results match version %d, expected %d\n", what, got,
         want);
  return 1;
}

static int run_cycle(int base, int live, int probe) {
  int failures = 0;

  failures += expect("cold call runs the AOT body", which_version(), base);
  failures += wait_published("Tier-1");

  set_slots(live);
  failures += expect("Tier-1 arms keep the data they were compiled with",
                     which_version(), base);

  /* Past the sampling threshold the runtime compiles Tier-2 itself. Its arms
   * keep their results while the data changes; Tier-1 keeps `base`. */
  int t2 = -1;
  for (int i = 0; i < 20000 && t2 < 0; ++i) {
    set_slots(live);
    int a = which_version();
    set_slots(probe);
    int b = which_version();
    set_slots(live);
    if (a == b && (a == live || a == probe))
      t2 = a;
    else
      usleep(1000);
  }
  if (t2 < 0) {
    printf("  FAIL: no Tier-2 arms observed\n");
    return failures + 1;
  }
  printf("  OK: Tier-2 published with arms (folded version %d)\n", t2);

  set_slots(t2 == live ? probe : live);
  int served = 0;
  while (served < 200 && which_version() == t2)
    ++served;
  if (served == 200)
    printf("  OK: %u more calls served by Tier-2\n",
           (unsigned)(served * NUM_SLOTS));
  else
    failures += expect("Tier-2 keeps serving", which_version(), t2);
  set_slots(t2);
  return failures;
}

int main(void) {
  int failures = 0;
  printf("=== Switch-Case Mode + Online PGO Test ===\n");

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if ((int)ejit_init_pgo(&cfg) != 0) {
    printf("FAIL: ejit_init_pgo\n");
    return 1;
  }

  CellCfg *c = &g_cellCfg[3];
  c->len = 4;
  c->coef[0] = 1;
  c->coef[1] = -2;
  c->coef[2] = 3;
  c->coef[3] = 1;
  c->clip = 1000;
  set_slots(0);
  ejit_activate("cell", 3);
  printf("-- first identity lifetime\n");
  failures += run_cycle(0, 1, 2);

  ejit_deactivate("cell", 3);
  set_slots(3);
  ejit_activate("cell", 3);
  printf("-- after deactivate / update / activate\n");
  failures += run_cycle(3, 4, 5);

  printf("\n=== Result: %d failures ===\n", failures);
  ejit_shutdown();
  return failures > 0 ? 1 : 0;
}
