/**
 * EJIT JIT 优化验证测试 — 严格验证 JIT 编译确实发生并产生优化效果
 *
 * 验证点:
 *   1. JIT entries > 0: 确有函数被 JIT 编译
 *   2. Cache hits > 0:  同参数第二次调用命中缓存 (证明首次是 JIT 产出)
 *   3. 优化效果验证:    JIT 分支折叠后不同 cellIdx 产生不同特化代码
 *                      (每个独立的 JIT entry)
 *
 * 运行 (cellIdx 来自外部输入，测试不同 idx 产生独立 specialization):
 *   ./ejit_jit_verify 0
 *   ./ejit_jit_verify 3
 *   ./ejit_jit_verify 7
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- EJIT 属性 -----------------------------------------------------------===//

struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  EJIT_PERIOD_CONST uint32_t cellId;
  uint32_t trafficLoad;
};

struct TrpCfg {
  EJIT_PERIOD_CONST uint32_t trpType;
  uint32_t activeBeams;
};

ejit_group(static) uint32_t g_sysVer;

#define N_CELL 16
ejit_group(cell) struct CellCfg g_cellCfg[N_CELL];

#define M_TRP 8
ejit_group(trp) struct TrpCfg g_trpCfg[M_TRP];

//===-- JIT entry 函数 -----------------------------------------------------===//

// 单维 cellIdx: 分支依赖于 period_const 字段
ejit_entry
uint32_t jit_cell_check(
    EJIT_DIM(cell) uint8_t cellIdx)
{
  // JIT 应替换 cellType 为常量并折叠此分支
  if (g_cellCfg[cellIdx].cellType == 0xFD)
    return 1000;
  else
    return 999;
}

// 多维 cellIdx + trpIdx: 复合条件
ejit_entry
uint32_t jit_cell_trp_check(
    EJIT_DIM(cell) uint8_t cellIdx,
    EJIT_DIM(trp)  uint8_t trpIdx)
{
  uint32_t ct = g_cellCfg[cellIdx].cellType;
  uint32_t tt = g_trpCfg[trpIdx].trpType;

  // JIT 应替换两个 period_const 字段并折叠此分支
  if (ct == 0xFD && tt == 1)      return 777;
  else if (ct == 0xEC && tt == 2) return 888;
  return 0;
}

//===-- 运行时 API ---------------------------------------------------------===//
// ejit_config_t / ejit_stats_t / ejit_taskpool_stats_t / ejit_drain_taskpool
// come from ejit_test_helpers.h (single source of truth, ABI-matching
// EJitRuntime.h). Only the test-specific entry-point decls remain here.

extern void ejit_shutdown(void);

//===-- 断言 ---------------------------------------------------------------===//

static int g_failures = 0;

#define VERIFY(cond, fmt, ...) do {                                \
  if (!(cond)) {                                                    \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                    \
    g_failures++;                                                   \
  } else {                                                          \
    printf("  OK:   " fmt "\n", ##__VA_ARGS__);                    \
  }                                                                 \
} while(0)

//===-- main ---------------------------------------------------------------===//

int main(int argc, char **argv)
{
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  uint8_t ti = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 0;

  printf("=== EJIT JIT Optimization Verification ===\n");
  printf("cellIdx=%u  trpIdx=%u\n\n", ci, ti);

  // Init data
  g_cellCfg[ci].cellType = 0xFD;
  g_cellCfg[ci].cellId   = 42;
  g_cellCfg[ci].trafficLoad = 0;
  g_trpCfg[ti].trpType = 1;

  // Init EJIT (ejit_default_config picks ASYNC under EJIT_SRE_SHARED_TASKPOOL,
  // SYNC otherwise — matching the canonical ejit_config_t ABI).
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  ejit_activate("cell", ci);
  ejit_activate("trp", ti);

  //=== 验证 1: 首次调用触发 JIT 编译 ===

  printf("\n--- 验证 1: 首次调用应触发 JIT 编译 (miss) ---\n");

  uint32_t r1 = jit_cell_check(ci);
  VERIFY(r1 == 1000, "jit_cell_check(%u) = %u (expected 1000, cellType=0xFD)", ci, r1);

#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_drain_taskpool();
  ejit_taskpool_stats_t s1; memset(&s1, 0, sizeof(s1));
  ejit_taskpool_get_stats(&s1);
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         s1.readyEntries, (unsigned long long)s1.cacheHits,
         (unsigned long long)s1.asyncCompiles);
  VERIFY(s1.asyncCompiles >= 1, "JIT compiles >= 1 (actual %llu)",
         (unsigned long long)s1.asyncCompiles);
#else
  ejit_stats_t s1;
  ejit_get_stats(&s1);
  printf("  stats: entries=%zu hits=%llu misses=%llu\n", s1.entryCount, (unsigned long long)s1.hits, (unsigned long long)s1.misses);
  VERIFY(s1.entryCount >= 1, "JIT entries >= 1 (actual %zu)", s1.entryCount);
  VERIFY(s1.misses >= 1, "JIT misses >= 1 (actual %llu)", (unsigned long long)s1.misses);
#endif

  //=== 验证 2: 同参数第二次调用应命中缓存 ===

  printf("\n--- 验证 2: 同参数第二次调用应命中缓存 (hit) ---\n");

  uint32_t r2 = jit_cell_check(ci);
  VERIFY(r2 == 1000, "jit_cell_check(%u) 2nd call = %u", ci, r2);

#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_taskpool_stats_t s2; memset(&s2, 0, sizeof(s2));
  ejit_taskpool_get_stats(&s2);
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         s2.readyEntries, (unsigned long long)s2.cacheHits,
         (unsigned long long)s2.asyncCompiles);
  VERIFY(s2.cacheHits >= 1, "Cache hits >= 1 (actual %llu)",
         (unsigned long long)s2.cacheHits);
#else
  ejit_stats_t s2;
  ejit_get_stats(&s2);
  printf("  stats: entries=%zu hits=%llu misses=%llu\n", s2.entryCount, (unsigned long long)s2.hits, (unsigned long long)s2.misses);
  VERIFY(s2.hits >= 1, "Cache hits >= 1 (actual %llu)", (unsigned long long)s2.hits);
#endif

  //=== 验证 3: 多维函数首次调用触发独立 JIT entry ===

  printf("\n--- 验证 3: 多维函数应触发独立 JIT entry ---\n");

  uint32_t r3 = jit_cell_trp_check(ci, ti);
  VERIFY(r3 == 777, "jit_cell_trp(%u,%u) = %u (expected 777, FD+1)", ci, ti, r3);

#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_drain_taskpool();
  ejit_taskpool_stats_t s3; memset(&s3, 0, sizeof(s3));
  ejit_taskpool_get_stats(&s3);
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         s3.readyEntries, (unsigned long long)s3.cacheHits,
         (unsigned long long)s3.asyncCompiles);
  VERIFY(s3.asyncCompiles >= 2, "JIT compiles >= 2 (2 functions, actual %llu)",
         (unsigned long long)s3.asyncCompiles);
#else
  ejit_stats_t s3;
  ejit_get_stats(&s3);
  printf("  stats: entries=%zu hits=%llu misses=%llu\n", s3.entryCount, (unsigned long long)s3.hits, (unsigned long long)s3.misses);
  VERIFY(s3.entryCount >= 2, "JIT entries >= 2 (2 functions, actual %zu)", s3.entryCount);
#endif

  //=== 验证 4: 不同 cellIdx 产生不同 specialization ===

  if (argc >= 4) {
    // 第二个外部 cellIdx 应该在第一次调用时触发新的 JIT miss (不同 specialization)
    uint8_t ci2 = (uint8_t)atoi(argv[3]);
    printf("\n--- 验证 4: 不同 cellIdx 应触发新 specialization ---\n");
    printf("  第二个 cellIdx = %u\n", ci2);

    g_cellCfg[ci2].cellType = 0xEC;  // 不是 0xFD
    ejit_activate("cell", ci2);

    uint32_t r4 = jit_cell_check(ci2);
    VERIFY(r4 == 999, "jit_cell_check(%u) = %u (expected 999, cellType=0xEC)", ci2, r4);

#ifdef EJIT_SRE_SHARED_TASKPOOL
    ejit_drain_taskpool();
    ejit_taskpool_stats_t s4; memset(&s4, 0, sizeof(s4));
    ejit_taskpool_get_stats(&s4);
    printf("  stats: ready=%u hits=%llu compiles=%llu\n",
           s4.readyEntries, (unsigned long long)s4.cacheHits,
           (unsigned long long)s4.asyncCompiles);
    VERIFY(s4.asyncCompiles >= 3, "JIT compiles increased (>=3, actual %llu)",
           (unsigned long long)s4.asyncCompiles);
#else
    ejit_stats_t s4;
    ejit_get_stats(&s4);
    printf("  stats: entries=%zu hits=%llu misses=%llu\n", s4.entryCount, (unsigned long long)s4.hits, (unsigned long long)s4.misses);
    VERIFY(s4.entryCount >= 3, "JIT entries increased (>=3, actual %zu)", s4.entryCount);
    VERIFY(s4.misses >= 3, "JIT misses increased (>=3, actual %llu)", (unsigned long long)s4.misses);
#endif
  }

  //=== 验证 5: Deactivate 后不同 cellType 触发重新编译 ===

  printf("\n--- 验证 5: Deactivate 后重新激活应触发新编译 ---\n");

  ejit_deactivate("cell", ci);
  g_cellCfg[ci].cellType = 0xEC;  // 改成 0xEC
  ejit_activate("cell", ci);

#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_taskpool_stats_t s5a; memset(&s5a, 0, sizeof(s5a));
  ejit_taskpool_get_stats(&s5a);
  unsigned long long compiles_before = s5a.asyncCompiles;

  uint32_t r5 = jit_cell_check(ci);
  VERIFY(r5 == 999, "jit_cell_check(%u) after type change = %u (expected 999)", ci, r5);

  ejit_drain_taskpool();
  ejit_taskpool_stats_t s5; memset(&s5, 0, sizeof(s5));
  ejit_taskpool_get_stats(&s5);
  printf("  stats: compiles %llu -> %llu\n",
         compiles_before, (unsigned long long)s5.asyncCompiles);
  VERIFY(s5.asyncCompiles > compiles_before,
         "New compile after deactivate/re-activate (compiles increased)");
#else
  ejit_stats_t s5a;
  ejit_get_stats(&s5a);
  uint64_t misses_before = s5a.misses;

  uint32_t r5 = jit_cell_check(ci);
  // 现在 cellType=0xEC, 应该走 else 分支返回 999
  VERIFY(r5 == 999, "jit_cell_check(%u) after type change = %u (expected 999)", ci, r5);

  ejit_stats_t s5;
  ejit_get_stats(&s5);
  printf("  stats: misses %llu -> %llu\n", (unsigned long long)misses_before, (unsigned long long)s5.misses);
  VERIFY(s5.misses > misses_before, "New miss after deactivate/re-activate (misses increased)");
#endif

  //=== 总结 ===

  ejit_shutdown();

  printf("\n=== Result: %d failures ===\n", g_failures);
  return g_failures > 0 ? 1 : 0;
}
