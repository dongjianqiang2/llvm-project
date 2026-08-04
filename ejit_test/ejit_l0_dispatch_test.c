/**
 * EJIT per-core L0 dispatch cache test.
 *
 * A missed invalidation shows up as a plausible number rather than an obvious
 * failure, so each phase checks WHICH specialization ran.
 *
 * SCOPE: end-to-end smoke test, not per-hook coverage. It cannot isolate which
 * retire hook did the work -- any path producing a new specialization also
 * republishes, and cachePublish() retires every entry on its own. Removing the
 * ejit_invalidate() or version-bump hook individually was verified NOT to fail
 * this test. Per-hook invariants live in EJitSharedTaskPoolTest.cpp.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

#define N 8

struct Cfg {
  ejit_may_const uint32_t mode;
  uint32_t counter;
};
ejit_period_arr(cell) struct Cfg g_cfg[N];

/* The specialization folds g_cfg[ci].mode, so the returned value identifies
 * which specialization ran. */
ejit_entry uint32_t l0_probe(ejit_period_arr_ind(cell) uint8_t ci, uint32_t seed)
{
  return g_cfg[ci].mode * 1000u + seed;
}

static int failures;

static void expect(uint32_t got, uint32_t want, const char *what) {
  if (got == want) {
    printf("  OK  : %s (= %u)\n", what, got);
  } else {
    printf("  FAIL: %s: got %u want %u\n", what, got, want);
    failures++;
  }
}

/* Resolve the specialization and fill the L0 for this identity. */
static void warm(uint8_t ci) {
  for (int i = 0; i < 64; i++)
    (void)l0_probe(ci, 0);
}

int main(int argc, char **argv) {
  const uint8_t ci = (argc > 1) ? (uint8_t)atoi(argv[1]) : 1;
  ejit_config_t cfg;

  for (int i = 0; i < N; i++) {
    g_cfg[i].mode = 1;
    g_cfg[i].counter = 0;
  }

  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_SYNC;
  cfg.optLevel = EJIT_OPT_L2;
  if (ejit_init(&cfg) != EJIT_OK) {
    printf("  FAIL: ejit_init\n");
    return 1;
  }
  ejit_activate("cell", ci);

  printf("=== EJIT L0 dispatch cache ===\n");

  /* 1. Baseline: a repeat dispatch still returns the right answer. */
  warm(ci);
  expect(l0_probe(ci, 7), 1007, "repeat dispatch after warm-up");

  /* 2. ejit_invalidate() drops the cached specialization. It does not go
   *    through cachePublish(), so the L0 is only retired if the runtime hooks
   *    it explicitly -- this phase is what catches a missing hook. */
  g_cfg[ci].mode = 2;
  ejit_invalidate("cell", ci);
  /* Checked on the FIRST call after the flush, before anything republishes.
   * A republish bumps the epoch on its own, so warming up first would retire a
   * stale entry for the wrong reason and the missing hook would go unnoticed. */
  expect(l0_probe(ci, 7), 2007, "first call after ejit_invalidate");
  warm(ci);
  expect(l0_probe(ci, 7), 2007, "value after ejit_invalidate + recompile");

  /* 3. Same for ejit_clear_cache(), which flushes everything at once. */
  g_cfg[ci].mode = 3;
  ejit_clear_cache();
  expect(l0_probe(ci, 7), 3007, "first call after ejit_clear_cache");
  warm(ci);
  expect(l0_probe(ci, 7), 3007, "value after ejit_clear_cache + recompile");

  /* 4. deactivate/activate bumps the instance version: a stale entry would run
   *    a specialization built from the old constants. */
  g_cfg[ci].mode = 4;
  ejit_deactivate("cell", ci);
  ejit_activate("cell", ci);
  expect(l0_probe(ci, 7), 4007, "first call after deactivate/activate");
  warm(ci);
  expect(l0_probe(ci, 7), 4007, "value after deactivate/activate + recompile");

  /* 5. While deactivated the call must fall back to the AOT body. */
  g_cfg[ci].mode = 5;
  ejit_deactivate("cell", ci);
  expect(l0_probe(ci, 7), 5007, "value while the instance is deactivated");
  ejit_activate("cell", ci);

  /* 6. Two live instances must not share an entry -- what the per-function
   *    inline cache cannot express. */
  {
    const uint8_t other = (uint8_t)((ci + 1) % N);
    g_cfg[ci].mode = 6;
    g_cfg[other].mode = 9;
    ejit_activate("cell", other);
    warm(ci);
    warm(other);
    expect(l0_probe(ci, 7), 6007, "instance A after two instances are warm");
    expect(l0_probe(other, 7), 9007, "instance B after two instances are warm");
  }

  /* 7. A direct-mapped table evicts: an eviction must miss, never return
   *    another identity's pointer. */
  {
    const uint8_t other = (uint8_t)((ci + 1) % N);
    for (int i = 0; i < 200; i++) {
      if (l0_probe(ci, 0) != 6000) {
        printf("  FAIL: interleaved dispatch returned the wrong "
               "specialization for A\n");
        failures++;
        break;
      }
      if (l0_probe(other, 0) != 9000) {
        printf("  FAIL: interleaved dispatch returned the wrong "
               "specialization for B\n");
        failures++;
        break;
      }
    }
    if (!failures)
      printf("  OK  : 200 interleaved dispatches across two instances\n");
  }

  /* 8. The table outlives the shared blob, so its entries survive a teardown
   *    that invalidates every pointer they hold. */
  ejit_shutdown();
  memset(&cfg, 0, sizeof(cfg));
  cfg.compileMode = EJIT_COMPILE_SYNC;
  cfg.optLevel = EJIT_OPT_L2;
  if (ejit_init(&cfg) != EJIT_OK) {
    printf("  FAIL: ejit_init after shutdown\n");
    return 1;
  }
  ejit_activate("cell", ci);
  g_cfg[ci].mode = 8;
  warm(ci);
  expect(l0_probe(ci, 7), 8007, "value after shutdown + re-init");

  ejit_shutdown();

  if (failures) {
    printf("\n=== FAIL: %d failure(s) ===\n", failures);
    return 1;
  }
  printf("\n=== PASS ===\n");
  return 0;
}
