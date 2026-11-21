/**
 * Closure-slimming end-to-end test.
 *
 * PASS1 externalizes closure helpers at or above 16 raw-IR instructions:
 * the extracted bitcode keeps only a declaration, and the AOT side registers
 * the original body (internal helpers under a deterministic
 * ejit_static.<module>.<name> key) so the isolated spec JITDylib can resolve
 * the call. Helpers below the threshold stay as definitions; small inline
 * helpers are consumed by the preopt inliner.
 *
 * All non-inline helpers here carry noinline, mirroring the
 * -finline-hint-functions world (where the frontend marks every non-inline
 * function noinline) so the externalization set is annotation-driven.
 *
 * Verifies: results match AOT semantics AND the JIT really compiled
 * (entryCount > 0; a broken registration would fall back to AOT silently).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- test data ---------------------------------------------------------===//

struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  uint32_t traffic;
};
ejit_group(cell) struct CellCfg g_cells[16];

//===-- helpers -----------------------------------------------------------===//

// external + big: externalized, name kept
uint32_t __attribute__((noinline)) ext_helper_big(uint32_t x) {
  uint32_t r = x + 1;
  for (uint32_t i = 0; i < 64; i++) {
    r = r * 31 + i;
    r ^= (r << 7) ^ i;
    r -= r % 29;
    r += (r >> 2) + x;
  }
  return r;
}

// static + big: externalized + renamed, resolved via registration
static uint32_t __attribute__((noinline)) st_helper_big(uint32_t x) {
  uint32_t r = x + 1;
  for (uint32_t i = 0; i < 64; i++) {
    r = r * 13 + i;
    r ^= (r << 5) ^ i;
    r -= r % 31;
    r += (r >> 3) + x;
  }
  return r;
}

// static + small: below the externalize threshold, stays in the bitcode
static uint32_t __attribute__((noinline)) st_helper_small(uint32_t x) {
  return x ^ 0x55;
}

// static + reads period_const: externalized like any other helper; its AOT
// body reads live values, which the entry-side guard keeps consistent
static uint32_t __attribute__((noinline)) st_helper_period_const(uint32_t i) {
  uint32_t r = g_cells[i].cellType;
  for (uint32_t k = 0; k < 8; k++)
    r = r * 3 + g_cells[(i + k) % 16].traffic;
  return r;
}

// inlinehint: consumed by the preopt inliner
static inline uint32_t hint_helper(uint32_t x) { return x * 7 + 3; }

//===-- entries -----------------------------------------------------------===//

ejit_entry uint32_t entry1(EJIT_DIM(cell) uint8_t ci) {
  uint32_t r = ext_helper_big(ci);
  r += st_helper_big(r);
  r += st_helper_small(r);
  r += st_helper_period_const(ci);
  r = hint_helper(r);
  return r;
}

ejit_entry uint32_t entry2(EJIT_DIM(cell) uint8_t ci) {
  uint32_t r = st_helper_big(ci);
  r = ext_helper_big(r);
  r += st_helper_period_const(ci);
  r = hint_helper(r);
  return r;
}

//===-- main --------------------------------------------------------------===//

int main(int argc, char **argv) {
  uint8_t ci = (uint8_t)(argc > 1 ? atoi(argv[1]) : 0);
  int failures = 0;

  printf("=== Closure Slim Test ===\ncellIdx=%u\n\n", ci);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = (int)ejit_init(&cfg);
  if (rc != 0) { printf("FAIL: init %d\n", rc); return 1; }

  g_cells[ci].cellType = 42;
  g_cells[ci].traffic = 7;
  ejit_activate("cell", ci);
  ejit_drain_taskpool();

  uint32_t r1 = entry1(ci);
  uint32_t e1 = ext_helper_big(ci);
  e1 += st_helper_big(e1);
  e1 += st_helper_small(e1);
  e1 += st_helper_period_const(ci);
  e1 = hint_helper(e1);
  printf("  entry1: got=%u expected=%u [%s]\n", r1, e1,
         r1 == e1 ? "MATCH" : "MISMATCH");
  if (r1 != e1) failures++;

  uint32_t r2 = entry2(ci);
  uint32_t e2 = st_helper_big(ci);
  e2 = ext_helper_big(e2);
  e2 += st_helper_period_const(ci);
  e2 = hint_helper(e2);
  printf("  entry2: got=%u expected=%u [%s]\n", r2, e2,
         r2 == e2 ? "MATCH" : "MISMATCH");
  if (r2 != e2) failures++;

  ejit_stats_t s;
  ejit_get_stats(&s);
  printf("  entries: %zu  JIT: %s\n", s.entryCount,
         s.entryCount > 0 ? "ACTIVE" : "AOT-FALLBACK-ONLY");
  if (s.entryCount == 0) {
    printf("  FAIL: no JIT entries (silent AOT fallback)\n");
    failures++;
  }

  ejit_shutdown();
  printf("=== Closure Slim Test %s ===\n", failures ? "FAILED" : "PASSED");
  return failures ? 1 : 0;
}
