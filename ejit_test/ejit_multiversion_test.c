/**
 * EJIT 多版本 Cache 测试 — 同一函数在不同 cellIdx 下产生独立特化版本
 *
 * 验证:
 *   1. f(cellIdx=A) 首次调用 → JIT miss, entries+1
 *   2. f(cellIdx=B) 首次调用 → JIT miss, entries+1 (新特化版本)
 *   3. f(cellIdx=A) 再次调用 → cache HIT (命中之前编译的版本)
 *   4. 每个版本产生正确结果 (取决于各自 cellIdx 对应的 may_const 值)
 *   5. 所有 cellIdx 均来自外部输入 (argv)
 *
 * 运行:
 *   ./ejit_multiversion 0 3 7     # 3 个不同 cellIdx
 *   ./ejit_multiversion 1 5 9 13  # 4 个不同 cellIdx
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- 数据结构 -------------------------------------------------------------===//

struct CellCfg {
  ejit_may_const uint32_t cellType;
  ejit_may_const uint32_t cellId;
  uint32_t counter;
};

#define N 16
ejit_period_arr(cell) struct CellCfg g_cells[N];

//===-- JIT entry: 分支依赖 may_const 字段 -----------------------------------===//

ejit_entry
uint32_t classify_cell(
    ejit_period_arr_ind(cell) uint8_t ci)
{
  uint32_t t = g_cells[ci].cellType;

  // 根据 cellType 返回不同值，JIT 应折叠此分支
  if (t == 0xFD)       return 100;
  else if (t == 0xEC)  return 200;
  else if (t == 0xAA)  return 300;
  else                 return 0;
}

//===-- 运行时 API -----------------------------------------------------------===//
// ejit_config_t / ejit_stats_t / ejit_taskpool_stats_t / ejit_drain_taskpool
// come from ejit_test_helpers.h (ABI-matching EJitRuntime.h).

extern void ejit_shutdown(void);

//===-- 测试 -----------------------------------------------------------------===//

static int g_fail = 0;

static void init_cell(uint8_t ci, uint32_t cellType) {
  g_cells[ci].cellType = cellType;
  g_cells[ci].cellId   = ci * 100;
  g_cells[ci].counter  = 0;
  ejit_activate("cell", ci);
}

static void check(const char *label, int cond,
                  const char *fmt, ...) {
  if (cond) printf("  OK   %s\n", label);
  else      printf("  FAIL %s\n", label), g_fail++;
  (void)fmt;
}

int main(int argc, char **argv) {
  if (argc < 3) {
    printf("Usage: %s <cellIdx1> <cellIdx2> [cellIdx3 ...]\n", argv[0]);
    return 2;
  }

  printf("=== EJIT Multi-Version Cache Test ===\n");
  printf("cellIdx args:");
  for (int i = 1; i < argc; i++)
    printf(" %s", argv[i]);
  printf("\n\n");

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  ejit_init(&cfg);

  // 为每个外部输入的 cellIdx 设置数据并激活
  for (int i = 1; i < argc; i++) {
    uint8_t ci = (uint8_t)atoi(argv[i]);
    init_cell(ci, 0xFD + i - 1); // 第1个=0xFD, 第2个=0xFE, 第3个=0xFF
  }

  // --- 第一轮: 每个 cellIdx 首次调用 (全部应该 miss) ---
  printf("--- 第一轮: 首次调用 (全部 miss) ---\n");
  ejit_stats_t s0;
  ejit_get_stats(&s0);

  for (int i = 1; i < argc; i++) {
    uint8_t ci = (uint8_t)atoi(argv[i]);
    uint32_t r = classify_cell(ci);
    uint32_t expected = (g_cells[ci].cellType == 0xFD) ? 100 :
                        (g_cells[ci].cellType == 0xEC) ? 200 :
                        (g_cells[ci].cellType == 0xAA) ? 300 : 0;
    check("", r == expected, "classify(%u)=%u expected=%u", ci, r, expected);
    // Async dedup is keyed by funcIndex (not by dims), so only one
    // specialization of classify_cell can be in flight at a time. Drain after
    // each call so every distinct cellIdx is compiled and published before the
    // next request, yielding one ready entry per cellIdx.
    ejit_drain_taskpool();
  }

  // 每个不同的 cellIdx 应该产生一个独立的 cache entry
  int n_unique = argc - 1;
  ejit_drain_taskpool();
  ejit_taskpool_stats_t tp1; memset(&tp1, 0, sizeof(tp1));
  ejit_taskpool_get_stats(&tp1);
  check("entries == n_unique", (int)tp1.readyEntries == n_unique,
        "readyEntries=%u expected=%d", tp1.readyEntries, n_unique);
  check("compiles >= n_unique", tp1.asyncCompiles >= (unsigned long long)n_unique,
        "asyncCompiles=%llu expected>=%d",
        (unsigned long long)tp1.asyncCompiles, n_unique);
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         tp1.readyEntries, (unsigned long long)tp1.cacheHits,
         (unsigned long long)tp1.asyncCompiles);

  // --- 第二轮: 再次调用相同 cellIdx (全部应该 hit) ---
  printf("\n--- 第二轮: 再次调用相同 cellIdx (全部 hit) ---\n");

  for (int i = 1; i < argc; i++) {
    uint8_t ci = (uint8_t)atoi(argv[i]);
    uint32_t r = classify_cell(ci);
    uint32_t expected = (g_cells[ci].cellType == 0xFD) ? 100 :
                        (g_cells[ci].cellType == 0xEC) ? 200 :
                        (g_cells[ci].cellType == 0xAA) ? 300 : 0;
    check("", r == expected, "classify(%u)=%u  (cache hit)", ci, r);
    ejit_drain_taskpool();
  }

  ejit_taskpool_stats_t tp2; memset(&tp2, 0, sizeof(tp2));
  ejit_taskpool_get_stats(&tp2);
  check("entries unchanged", (int)tp2.readyEntries == n_unique,
        "readyEntries=%u (was %u)", tp2.readyEntries, tp1.readyEntries);
  check("hits increased",
        tp2.cacheHits >= tp1.cacheHits + (unsigned long long)n_unique,
        "cacheHits=%llu (was %llu)",
        (unsigned long long)tp2.cacheHits, (unsigned long long)tp1.cacheHits);
  check("compiles unchanged", tp2.asyncCompiles == tp1.asyncCompiles,
        "asyncCompiles=%llu (was %llu)",
        (unsigned long long)tp2.asyncCompiles, (unsigned long long)tp1.asyncCompiles);
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         tp2.readyEntries, (unsigned long long)tp2.cacheHits,
         (unsigned long long)tp2.asyncCompiles);

  // --- 第三轮: 修改某个 cellIdx 的数据，deactivate → 重新 JIT ---
  if (argc >= 3) {
    uint8_t ci0 = (uint8_t)atoi(argv[1]);
    printf("\n--- 第三轮: deactivate(%u) → 改值 → activate → 重新 JIT ---\n", ci0);

    ejit_deactivate("cell", ci0);
    unsigned long long tp_compiles_before = tp2.asyncCompiles;
    unsigned tp_entries_before = tp2.readyEntries;

    // 把 cellType 改成 0xEC (原来可能是 0xFD)
    g_cells[ci0].cellType = 0xEC;
    ejit_activate("cell", ci0);

    uint32_t r = classify_cell(ci0);
    check("new value after recompile", r == 200,
          "classify(%u)=%u (expected 200, now cellType=0xEC)", ci0, r);

    ejit_drain_taskpool();
    ejit_taskpool_stats_t tp3; memset(&tp3, 0, sizeof(tp3));
    ejit_taskpool_get_stats(&tp3);
    check("new compile triggered", tp3.asyncCompiles > tp_compiles_before,
          "compiles %llu -> %llu",
          (unsigned long long)tp_compiles_before,
          (unsigned long long)tp3.asyncCompiles);
    check("entries stable after recompile",
          (int)tp3.readyEntries == (int)tp_entries_before,
          "readyEntries=%u (was %u: old evicted, new added)",
          tp3.readyEntries, tp_entries_before);
  }

  ejit_shutdown();

  printf("\n=== Result: %d failures ===\n", g_fail);
  return g_fail ? 1 : 0;
}
