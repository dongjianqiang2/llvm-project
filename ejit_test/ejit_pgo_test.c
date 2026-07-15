/**
 * EJIT Online PGO Integration Test
 *
 * Verifies the full Tier-1 -> Tier-2 PGO cycle end-to-end:
 *   1. PGO enabled, init in Async mode (starts background std::thread worker).
 *   2. Activate period instance, call ejit_entry function (cache miss).
 *   3. Tier-1 (Instrumented) compiles with __profc_/__profd_ counters.
 *   4. Hit the function to cross the PGO threshold (default=3).
 *      The background worker's pollOne() processes the deferred Tier-2
 *      request inside its PGO handler.
 *   5. Wait for the worker, then verify Tier-2 happened (asyncCompiles >= 2).
 *
 * Tier-2 failure is a hard test failure — no silent AOT fallback.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ejit_test_helpers.h"

struct CellCfg {
  ejit_may_const uint32_t cellType;
  uint32_t data;
};
ejit_period_arr(cell) struct CellCfg g_cells[4];

ejit_entry uint32_t pgo_branch(ejit_period_arr_ind(cell) uint8_t ci) {
  if (g_cells[ci].cellType == 0xFF)
    return g_cells[ci].data + 100;
  else
    return g_cells[ci].data;
}

extern void ejit_shutdown(void);

int main(int argc, char **argv) {
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  printf("=== EJIT PGO Test ===\ncellIdx=%u\n", ci);

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  cfg.enablePgo = true;
  printf("PGO enabled, mode=%d\n", (int)cfg.compileMode);

  int rc = (int)ejit_init(&cfg);
  if (rc != EJIT_OK) { printf("FAIL: ejit_init=%d\n", rc); return 1; }

  ejit_activate("cell", ci);
  g_cells[ci].cellType = 0xFF;
  g_cells[ci].data = 42;

  // Tier-1: cache miss -> Instrumented compile.
  uint32_t r = pgo_branch(ci);
  if (r != 142) { printf("FAIL: first call %u != 142\n", r); return 1; }

  ejit_drain_taskpool();
  { ejit_taskpool_stats_t ts; memset(&ts, 0, sizeof(ts));
    ejit_taskpool_get_stats(&ts);
    if (ts.asyncCompiles < 1) {
      printf("FAIL: Tier-1 compile did not happen (compiles=%lu)\n", ts.asyncCompiles);
      return 1;
    }
    printf("Tier-1 OK (compiles=%lu)\n", ts.asyncCompiles);
  }

  // Hit the function to cross the PGO threshold (default=3).
  // The background worker will process the deferred Tier-2 request.
  for (int i = 0; i < 5; ++i) {
    r = pgo_branch(ci);
    if (r != 142) { printf("FAIL: hit %d -> %u\n", i + 1, r); return 1; }
  }

  // Wait for the worker to process Tier-2.
  // The worker thread runs pollOne() which calls runCompile() inline.
  // Give it up to 500 ms (50 × 10 ms) to complete.
  ejit_taskpool_stats_t ts;
  for (int i = 0; i < 50; ++i) {
    usleep(10000);
    memset(&ts, 0, sizeof(ts));
    ejit_taskpool_get_stats(&ts);
    if (ts.asyncCompiles >= 2) break;
  }

  printf("Final: ready=%u compiles=%lu hits=%lu\n",
         ts.readyEntries, ts.asyncCompiles, ts.cacheHits);

  if (ts.asyncCompiles < 2) {
    printf("FAIL: Tier-2 recompile did not happen (compiles=%lu, need >=2)\n",
           ts.asyncCompiles);
    return 1;
  }

  // Post-Tier-2 hit: returns correctly from cached Tier-2 code.
  r = pgo_branch(ci);
  if (r != 142) { printf("FAIL: post-Tier2 hit %u\n", r); return 1; }
  printf("Post-Tier2 cache hit OK (r=%u)\n", r);

  ejit_shutdown();
  printf("=== PGO Test PASSED ===\n");
  return 0;
}
