/**
 * EJIT Sync Mode Test — validates the unified API in synchronous compile mode.
 *
 * Tests that JIT compilation works when compileMode is set to SYNC,
 * exercising the inline compile path (taskpool compileOrGet with Sync mode).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

// Simple test data
struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  uint32_t xx;
};
ejit_group(cell) struct CellCfg g_cells[4];

ejit_entry uint32_t check_cell(EJIT_DIM(cell) uint8_t ci) {
  if (g_cells[ci].cellType == 0xFF)
    return 300;
  if (g_cells[ci].cellType == 0xFD)
    return 100;
  return 0;
}

#define VERIFY(cond, fmt, ...)                                                \
  do {                                                                        \
    if (!(cond)) {                                                            \
      printf("  FAIL %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);   \
      failures++;                                                             \
      return 1;                                                               \
    } else {                                                                  \
      printf("  OK   " fmt "\n", ##__VA_ARGS__);                              \
    }                                                                         \
  } while (0)

int main(int argc, char **argv) {
  (void)argc;
  uint8_t ci = (uint8_t)(argv[1] ? atoi(argv[1]) : 0);
  int failures = 0;
  printf("=== EJIT Sync Mode Test ===\n");
  printf("cellIdx=%u\n\n", ci);

  // Init in ASYNC (required to start the shared pool worker).
  printf("--- 1. Start async then switch to sync ---\n");
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  VERIFY(cfg.compileMode == EJIT_COMPILE_ASYNC,
         "default compileMode is ASYNC (%d)", cfg.compileMode);

  int rc = (int)ejit_init(&cfg);
  VERIFY(rc == EJIT_OK, "ejit_init returns %d (OK)", rc);

  // Do one async compile to warm up the engine.
  g_cells[ci].cellType = 0xFD;
  ejit_activate("cell", ci);
  ejit_drain_taskpool();
  uint32_t r = check_cell(ci);
  VERIFY(r == 100, "async warm-up check_cell(%u) = %u (expected 100)", ci, r);

  // Switch to SYNC mode.
  printf("\n--- 2. Switch to SYNC and compile inline ---\n");
  ejit_set_compile_mode(EJIT_COMPILE_SYNC);
  ejit_compile_mode_t m = ejit_get_compile_mode();
  VERIFY(m == EJIT_COMPILE_SYNC, "get_compile_mode = %d (SYNC)", m);

  // Force recompile: deactivate, change data, reactivate.
  ejit_deactivate("cell", ci);
  g_cells[ci].cellType = 0xFF;
  ejit_activate("cell", ci);
  r = check_cell(ci);
  VERIFY(r == 300, "check_cell(%u) sync = %u (expected 300)", ci, r);

  // Cache hit in sync mode.
  r = check_cell(ci);
  VERIFY(r == 300, "check_cell(%u) sync cache hit = %u (expected 300)", ci, r);

  // Switch back to ASYNC.
  ejit_set_compile_mode(EJIT_COMPILE_ASYNC);
  m = ejit_get_compile_mode();
  VERIFY(m == EJIT_COMPILE_ASYNC, "get_compile_mode = %d (ASYNC restored)", m);

  // Verify the async path still works.
  ejit_deactivate("cell", ci);
  g_cells[ci].cellType = 0xFD;
  ejit_activate("cell", ci);
  ejit_drain_taskpool();
  r = check_cell(ci);
  VERIFY(r == 100, "check_cell(%u) back to async = %u (expected 100)", ci, r);

  printf("\n--- 3. Shutdown ---\n");
  ejit_shutdown();
  printf("ejit_shutdown completed\n");

  printf("\n=== Result: %d failures ===\n", failures);
  return failures > 0 ? 1 : 0;
}
