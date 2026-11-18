/**
 * EJIT 配置/统计/缓存 API 完整测试
 *
 * 覆盖 SPEC4 §3.2, §3.3, §3.5 中之前缺少测试的接口:
 *   ejit_config_t (自定义配置)
 *   ejit_clear_cache
 *   ejit_invalidate
 *   ejit_set_log_level / ejit_get_log_level
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
  ejit_may_const uint32_t cellType;
  uint32_t counter;
};
#define N 8
ejit_period_arr(cell) struct CellCfg g_cells[N];

ejit_entry
uint32_t check_cell(
    ejit_period_arr_ind(cell) uint8_t ci)
{
  if (g_cells[ci].cellType == 0xFD) return 100;
  return 0;
}

_Static_assert(offsetof(ejit_config_t, compileMode) == 0 &&
               offsetof(ejit_config_t, optLevel) == 4 &&
               offsetof(ejit_config_t, forceStaticRegistry) == 8 &&
               offsetof(ejit_config_t, dumpJITDir) ==
                   (sizeof(void *) == 8 ? 16 : 12) &&
               sizeof(ejit_config_t) == (sizeof(void *) == 8 ? 24 : 16),
               "unexpected public C config layout");

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
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel       = EJIT_OPT_L2;

  int rc = ejit_init(&cfg);
  T(rc == EJIT_OK, "ejit_init(custom) returns %d (OK)", rc);

  //===-- 3. get_stats (before any JIT) -------------------------------------===//
  printf("\n--- 3. get_stats (empty) ---\n");

  ejit_stats_t s;
  rc = ejit_get_stats(&s);
  T(rc == EJIT_OK, "get_stats returns %d", rc);
  T(s.entryCount == 0, "entries=0 before any JIT");
  T(s.hits == 0, "hits=0");
  T(s.misses == 0, "misses=0");
  T(s.evictions == 0, "evictions=0");

  printf("\n--- 4. diagnostic log level ---\n");
  T(ejit_get_log_level() == EJIT_LOG_INFO, "default diagnostic level is INFO");
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  T(ejit_get_log_level() == EJIT_LOG_VERBOSE, "diagnostic level is VERBOSE");
  ejit_set_log_level(EJIT_LOG_INFO);

  //===-- 5. JIT 编译 + cache 验证 -------------------------------------------===//
  printf("\n--- 5. JIT compile + cache ---\n");

  g_cells[ci].cellType = 0xFD;
  ejit_activate("cell", ci);

  uint32_t r = check_cell(ci);
  T(r == 100, "check_cell(%u) = %u (expected 100)", ci, r);

  ejit_drain_taskpool();
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

  //===-- 6. ejit_clear_cache -----------------------------------------------===//
  printf("\n--- 6. clear_cache ---\n");

  size_t before = 0;
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

  r = check_cell(ci);
  T(r == 100, "check_cell(%u) after dispatch-cache retirement = %u", ci, r);

  // A rejected void registration records a functional error for the next
  // init, even with diagnostic output switched off.
  ejit_set_log_level(EJIT_LOG_OFF);
  uint32_t slot = UINT32_MAX;
  ejit_register_funcindex("late_registration", &slot);
  T(slot == UINT32_MAX, "frozen registration leaves its output untouched");

  //===-- 9. get_stats with NULL pointer ------------------------------------===//
  printf("\n--- 9. get_stats(NULL) ---\n");
  rc = ejit_get_stats(NULL);
  T(rc != EJIT_OK, "get_stats(NULL) returns error %d (expected != 0)", rc);

  ejit_shutdown();

  //===-- 10. 反初始化后调用应报错 -------------------------------------------===//
  printf("\n--- 10. after shutdown ---\n");

  rc = ejit_get_stats(&s);
  T(rc != EJIT_OK, "get_stats after shutdown returns error %d", rc);

  ejit_clear_cache();  // should not crash
  printf("  clear_cache after shutdown: no crash\n");

  rc = ejit_init(&cfg);
  T(rc == EJIT_ERR_INVALID_PARAM, "init reports registration failure: %d", rc);
  ejit_set_log_level(EJIT_LOG_INFO);

  cfg.compileMode = (ejit_compile_mode_t)0; /* retired Sync value */
  T(ejit_init(&cfg) == EJIT_ERR_INVALID_PARAM, "retired mode rejected");
  cfg.compileMode = EJIT_COMPILE_OFF;
  T(ejit_init(&cfg) == EJIT_OK, "explicit AOT-only init");
  void *fn = NULL;
  uint32_t bucket = 0;
  T(ejit_taskpool_compile_or_get(1, NULL, 0, &fn, &bucket) != EJIT_OK &&
    fn == NULL, "Off returns no callable JIT pointer");
  ejit_shutdown();

  printf("\n=== Result: %d failures ===\n", g_fail);
  return g_fail ? 1 : 0;
}
