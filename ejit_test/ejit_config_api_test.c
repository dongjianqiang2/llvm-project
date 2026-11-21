/**
 * EJIT 配置/统计/缓存 API 完整测试
 *
 * 覆盖 SPEC4 §3.2, §3.3, §3.5 中之前缺少测试的接口:
 *   ejit_config_t (自定义配置)
 *   ejit_set_compile_mode / ejit_get_compile_mode
 *   ejit_clear_cache
 *   ejit_invalidate
 *   ejit_get_last_error
 *
 * cellIdx 来自外部输入 (argv)
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- 数据结构 -------------------------------------------------------------===//

struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  uint32_t counter;
};
#define N 8
ejit_group(cell) struct CellCfg g_cells[N];

ejit_entry
uint32_t check_cell(
    EJIT_DIM(cell) uint8_t ci)
{
  if (g_cells[ci].cellType == 0xFD) return 100;
  return 0;
}

//===-- 运行时 API -----------------------------------------------------------===//
// ejit_config_t / ejit_stats_t / ejit_taskpool_stats_t / enums / drain helper
// come from ejit_test_helpers.h (ABI-matching EJitRuntime.h). Only the
// ejit_error_t getter + the EJIT_OK_C alias (this test's historical name for
// EJIT_OK) remain local.

extern const ejit_error_t *ejit_get_last_error(void);
#define EJIT_OK_C EJIT_OK

//===-- 断言 -----------------------------------------------------------------===//

static int g_fail = 0;
#define T(cond, fmt, ...) do {               \
  if (cond) printf("  OK   " fmt "\n", ##__VA_ARGS__); \
  else      printf("  FAIL " fmt "\n", ##__VA_ARGS__), g_fail++; \
} while(0)

int main(int argc, char **argv) {
  uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;

  printf("=== EJIT Config/Stats/Cache API Test ===\n");
  printf("cellIdx=%u\n\n", ci);

  //===-- 1. 自定义配置初始化 ------------------------------------------------===//
  printf("--- 1. ejit_init with custom config ---\n");

  ejit_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Async mode so the shared taskpool worker starts during ejit_init.
  cfg.compileMode    = EJIT_COMPILE_ASYNC;
#else
  cfg.compileMode    = EJIT_COMPILE_SYNC;
#endif
  cfg.optLevel       = EJIT_OPT_L2;
  cfg.maxCacheEntries = 32;
  cfg.maxCacheSize    = 256 * 1024;
  cfg.enableLogger    = true;

  int rc = ejit_init(&cfg);
  T(rc == EJIT_OK_C, "ejit_init(custom) returns %d (OK)", rc);

  //===-- 2. compile mode get/set -------------------------------------------===//
  printf("\n--- 2. compile mode get/set ---\n");

  ejit_compile_mode_t m = ejit_get_compile_mode();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // The test initialized in Async mode (required to start the shared taskpool
  // worker), so the reported compile mode is ASYNC here.
  T(m == EJIT_COMPILE_ASYNC, "get_compile_mode = %d (ASYNC)", m);
#else
  T(m == EJIT_COMPILE_SYNC, "get_compile_mode = %d (SYNC)", m);
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
  // Verify the initial mode is ASYNC (the shared pool was started with it).
  m = ejit_get_compile_mode();
  T(m == EJIT_COMPILE_ASYNC,
    "get_compile_mode = %d (initial ASYNC)", m);

  // Switch to SYNC and verify JIT still works via the unified inline path.
  ejit_set_compile_mode(EJIT_COMPILE_SYNC);
  m = ejit_get_compile_mode();
  T(m == EJIT_COMPILE_SYNC, "get_compile_mode = %d (SYNC after set)", m);

  // The sync inline path is verified below in §5a (full round-trip after
  // the async worker has processed at least one compile to exercise the
  // shared code path).

  // Switch back to ASYNC for the rest of the test.
  ejit_set_compile_mode(EJIT_COMPILE_ASYNC);
  m = ejit_get_compile_mode();
  T(m == EJIT_COMPILE_ASYNC,
    "get_compile_mode = %d (ASYNC after restore)", m);
#else
  // Only sync mode is supported. Async is excluded in bare-metal builds.
  ejit_set_compile_mode(EJIT_COMPILE_SYNC);
  m = ejit_get_compile_mode();
  T(m == EJIT_COMPILE_SYNC, "get_compile_mode = %d (SYNC after set)", m);
#endif

  //===-- 3. get_stats (before any JIT) -------------------------------------===//
  printf("\n--- 3. get_stats (empty) ---\n");

  ejit_stats_t s;
  rc = ejit_get_stats(&s);
  T(rc == EJIT_OK_C, "get_stats returns %d", rc);
  T(s.entryCount == 0, "entries=0 before any JIT");
  T(s.hits == 0, "hits=0");
  T(s.misses == 0, "misses=0");
  T(s.evictions == 0, "evictions=0");

  //===-- 4. get_last_error (empty) -----------------------------------------===//
  printf("\n--- 4. get_last_error (empty) ---\n");

  const ejit_error_t *err = ejit_get_last_error();
  T(err == NULL, "get_last_error = NULL (no errors yet)");

  //===-- 5. JIT 编译 + cache 验证 -------------------------------------------===//
  printf("\n--- 5. JIT compile + cache ---\n");

  g_cells[ci].cellType = 0xFD;
  ejit_activate("cell", ci);

  uint32_t r = check_cell(ci);
  T(r == 100, "check_cell(%u) = %u (expected 100)", ci, r);

  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  ejit_taskpool_stats_t tp5a; memset(&tp5a, 0, sizeof(tp5a));
  ejit_taskpool_get_stats(&tp5a);
  T(tp5a.asyncCompiles >= 1, "compiles >= 1 after JIT (actual %llu)",
    (unsigned long long)tp5a.asyncCompiles);
  // 第二次调用: cache hit (compile is done since we drained)
  r = check_cell(ci);
  T(r == 100, "check_cell(%u) 2nd call = %u", ci, r);
  ejit_taskpool_stats_t tp5b; memset(&tp5b, 0, sizeof(tp5b));
  ejit_taskpool_get_stats(&tp5b);
  T(tp5b.cacheHits >= 1, "hits >= 1 (actual %llu)",
    (unsigned long long)tp5b.cacheHits);
#else
  ejit_get_stats(&s);
  T(s.entryCount >= 1, "entries >= 1 after JIT compile (actual %zu)", s.entryCount);
  T(s.misses >= 1, "misses >= 1 (actual %llu)", (unsigned long long)s.misses);
  // 第二次调用: cache hit
  r = check_cell(ci);
  T(r == 100, "check_cell(%u) 2nd call = %u", ci, r);
  ejit_get_stats(&s);
  T(s.hits >= 1, "hits >= 1 (actual %llu)", (unsigned long long)s.hits);
#endif

  //===-- 6. ejit_clear_cache -----------------------------------------------===//
  printf("\n--- 6. clear_cache ---\n");

  size_t before = 0;
#ifdef EJIT_SRE_SHARED_TASKPOOL
  // ejit_clear_cache() only evicts the LRU cache; the taskpool's own fixed
  // cache is unaffected, so the next call is still a cache hit, not a
  // recompile. Verify the hit count increments rather than entry/miss counts.
  ejit_taskpool_stats_t tp6a; memset(&tp6a, 0, sizeof(tp6a));
  ejit_taskpool_get_stats(&tp6a);
  ejit_clear_cache();
  r = check_cell(ci);
  T(r == 100, "check_cell(%u) after clear = %u (taskpool: cache hit)", ci, r);
  ejit_taskpool_stats_t tp6b; memset(&tp6b, 0, sizeof(tp6b));
  ejit_taskpool_get_stats(&tp6b);
  T(tp6b.cacheHits > tp6a.cacheHits,
    "cacheHits increased after clear (taskpool cache intact)");
#else
  before = s.entryCount;
  ejit_clear_cache();
  ejit_get_stats(&s);
  T(s.entryCount == 0, "entries=0 after clear_cache (was %zu)", before);
  // 重新 JIT
  r = check_cell(ci);
  T(r == 100, "check_cell(%u) after clear+recompile = %u", ci, r);
  ejit_get_stats(&s);
  T(s.entryCount >= 1, "entries >= 1 after recompile");
  T(s.misses >= 2, "misses increased after clear+recompile");
#endif

  //===-- 7. ejit_invalidate (独立失效，不改变状态) ---------------------------===//
  printf("\n--- 7. invalidate (独立失效，状态不变) ---\n");

  bool active = ejit_is_active("cell", ci);
  T(active, "cell[%u] IS active before invalidate", ci);

  before = s.entryCount;
  ejit_invalidate("cell", ci);

  active = ejit_is_active("cell", ci);
  T(active, "cell[%u] still active after invalidate (state unchanged)", ci);

  ejit_get_stats(&s);
  T(s.entryCount == 0, "entries=0 after invalidate (was %zu)", before);

  // 重新 JIT
  r = check_cell(ci);
  T(r == 100, "check_cell(%u) after invalidate+recompile = %u", ci, r);

  //===-- 8. get_last_error after scenario ----------------------------------===//
  printf("\n--- 8. get_last_error (after operations) ---\n");
  // 调用一个不存在的函数来触发错误
  // (直接调用 get_last_error 看看有没有残留错误)
  err = ejit_get_last_error();
  // 可能有日志记录的错误，也可能为 NULL
  printf("  last_error: %s\n", err ? err->message : "(null)");

  //===-- 9. get_stats with NULL pointer ------------------------------------===//
  printf("\n--- 9. get_stats(NULL) ---\n");
  rc = ejit_get_stats(NULL);
  T(rc != EJIT_OK_C, "get_stats(NULL) returns error %d (expected != 0)", rc);

  ejit_shutdown();

  //===-- 10. 反初始化后调用应报错 -------------------------------------------===//
  printf("\n--- 10. after shutdown ---\n");

  m = ejit_get_compile_mode();
  T(m == EJIT_COMPILE_SYNC, "get_compile_mode after shutdown = %d (safe default SYNC)", m);

  rc = ejit_get_stats(&s);
  T(rc != EJIT_OK_C, "get_stats after shutdown returns error %d", rc);

  err = ejit_get_last_error();
  T(err == NULL, "get_last_error after shutdown = NULL");

  ejit_clear_cache();  // should not crash
  printf("  clear_cache after shutdown: no crash\n");

  printf("\n=== Result: %d failures ===\n", g_fail);
  return g_fail ? 1 : 0;
}
