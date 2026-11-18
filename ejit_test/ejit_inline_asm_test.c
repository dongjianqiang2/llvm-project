/**
 * EJIT AArch64 inline-asm regression test.
 *
 * This covers ejit_entry functions whose IR contains InlineAsm.  On AArch64,
 * ORC codegen must have the AArch64 asm parser initialized, otherwise
 * AsmPrinterInlineAsm reports that inline asm is not supported by the streamer.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ejit_test_helpers.h"

struct CellCfg {
  ejit_may_const uint32_t base;
  ejit_may_const uint32_t bonus;
  uint32_t noise;
};

ejit_period_arr(cell) struct CellCfg g_cellCfg[4] = {
    {10, 3, 100},
    {20, 5, 200},
    {30, 7, 300},
    {40, 9, 400},
};

static int g_failures = 0;

#define VERIFY(cond, fmt, ...)                                                 \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                              \
      ++g_failures;                                                            \
    } else {                                                                   \
      printf("  OK:   " fmt "\n", ##__VA_ARGS__);                              \
    }                                                                          \
  } while (0)

ejit_entry uint32_t inline_asm_add(ejit_period_arr_ind(cell) uint8_t cellIdx,
                                   uint32_t dynamic) {
  uint32_t base = g_cellCfg[cellIdx].base;
  uint32_t bonus = g_cellCfg[cellIdx].bonus;
  uint32_t asmSum;

#if defined(__aarch64__)
  __asm__ volatile("add %w0, %w1, %w2" : "=r"(asmSum) : "r"(base), "r"(bonus));
#else
  asmSum = base + bonus;
#endif

  return asmSum + dynamic;
}

static void check_one(uint8_t idx, uint32_t dynamic) {
  ejit_activate("cell", idx);

  uint32_t expected = g_cellCfg[idx].base + g_cellCfg[idx].bonus + dynamic;
  uint32_t first = inline_asm_add(idx, dynamic);
  ejit_drain_taskpool();
  uint32_t second = inline_asm_add(idx, dynamic);

  printf("\n--- inline_asm_add(cell=%u, dynamic=%u) ---\n", idx, dynamic);
  VERIFY(first == expected, "first=%u expected=%u", first, expected);
  VERIFY(second == expected, "second=%u expected=%u", second, expected);
}

int main(void) {
  printf("=== EJIT AArch64 InlineAsm Regression Test ===\n");

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  check_one(0, 11);
  check_one(2, 13);

  ejit_taskpool_stats_t tp;
  memset(&tp, 0, sizeof(tp));
  ejit_taskpool_get_stats(&tp);
  printf("\n--- Taskpool Stats ---\n");
  printf("  ready=%u hits=%llu asyncCompiles=%llu compileFailed=%llu\n",
         tp.readyEntries, (unsigned long long)tp.cacheHits,
         (unsigned long long)tp.asyncCompiles,
         (unsigned long long)tp.compileFailed);
  VERIFY(tp.compileFailed == 0, "compileFailed == 0 (actual %llu)",
         (unsigned long long)tp.compileFailed);

  ejit_shutdown();
  printf("\n=== %s (%d failure(s)) ===\n", g_failures ? "FAILED" : "ALL PASSED",
         g_failures);
  return g_failures ? 1 : 0;
}
