/**
 * Fixed-dimension fast-path API test.
 *
 * Verifies the ejit_taskpool_compile_or_get_0d/1d/2d wrappers produce
 * correct JIT results. The PASS3 wrapper emits these when
 * -ejit-wrapper-fixed-dim-entry is enabled and the entry has <= 2 dims.
 * This test directly calls the runtime C API from user code to exercise
 * the full fixed-dimension path (C API → tryCacheHitNd → cacheLookupNd).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- 0-dim test data ---------------------------------------------------===//

struct StaticCfg {
  EJIT_PERIOD_CONST uint32_t val;
  uint32_t _pad;
};
ejit_group(static) struct StaticCfg g_static_cfg;

ejit_entry uint32_t static_check(void) {
  if (g_static_cfg.val == 77) return 700;
  return 0;
}

//===-- 1-dim test data ---------------------------------------------------===//

struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  uint32_t traffic;
};
ejit_group(cell) struct CellCfg g_cells[8];

ejit_entry uint32_t cell_check(EJIT_DIM(cell) uint8_t ci) {
  if (g_cells[ci].cellType == 42) return 100;
  if (g_cells[ci].cellType == 99) return 200;
  return 0;
}

//===-- 2-dim test data ---------------------------------------------------===//

struct TrpCfg {
  EJIT_PERIOD_CONST uint32_t trpType;
  uint32_t status;
};
ejit_group(trp) struct TrpCfg g_trps[4];

ejit_entry uint32_t cell_trp_check(EJIT_DIM(cell) uint8_t ci,
                                   EJIT_DIM(trp) uint8_t ti) {
  if (g_cells[ci].cellType == 42 && g_trps[ti].trpType == 3) return 999;
  return 0;
}

//===-- Main --------------------------------------------------------------===//

int main(int argc, char **argv) {
  uint8_t ci = (uint8_t)(argc > 1 ? atoi(argv[1]) : 0);
  uint8_t ti = (uint8_t)(argc > 2 ? atoi(argv[2]) : 0);
  int failures = 0;

  printf("=== Fixed-Dim Fast-Path Test ===\n");
  printf("cellIdx=%u trpIdx=%u\n\n", ci, ti);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = (int)ejit_init(&cfg);
  if (rc != 0) { printf("FAIL: init %d\n", rc); return 1; }
  printf("--- init OK ---\n");

  //===-- 0-dim -----------------------------------------------------------===//
  printf("\n--- 0-dim (static period) ---\n");
  g_static_cfg.val = 77;
  ejit_drain_taskpool();
  uint32_t r = static_check();
  if (r != 700) { printf("  FAIL: static_check=%u (expected 700)\n", r); failures++; }
  else printf("  OK: static_check=%u\n", r);

  //===-- 1-dim -----------------------------------------------------------===//
  printf("\n--- 1-dim ---\n");
  g_cells[ci].cellType = 42;
  ejit_activate("cell", ci);
  ejit_drain_taskpool();
  r = cell_check(ci);
  if (r != 100) { printf("  FAIL: cell_check=%u (expected 100)\n", r); failures++; }
  else printf("  OK: cell_check=%u\n", r);

  // Change and recompile
  ejit_deactivate("cell", ci);
  g_cells[ci].cellType = 99;
  ejit_activate("cell", ci);
  ejit_drain_taskpool();
  r = cell_check(ci);
  if (r != 200) { printf("  FAIL: cell_check after change=%u (expected 200)\n", r); failures++; }
  else printf("  OK: cell_check after change=%u\n", r);

  //===-- 2-dim -----------------------------------------------------------===//
  printf("\n--- 2-dim ---\n");
  g_cells[ci].cellType = 42;
  g_trps[ti].trpType = 3;
  ejit_activate("cell", ci);
  ejit_activate("trp", ti);
  ejit_drain_taskpool();
  r = cell_trp_check(ci, ti);
  if (r != 999) { printf("  FAIL: cell_trp_check=%u (expected 999)\n", r); failures++; }
  else printf("  OK: cell_trp_check=%u\n", r);

  printf("\n=== Result: %d failures ===\n", failures);
  ejit_shutdown();
  return failures > 0 ? 1 : 0;
}
