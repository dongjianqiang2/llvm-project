/**
 * EJIT 静态注册表路径测试 (-enable-ejit-global-ctors=false)
 *
 * 编译时通过 -mllvm -enable-ejit-global-ctors=false 禁用构造器。
 * ejit_init() 从 __ejit_registry_*[] 静态表加载 bitcode 和 period。
 * 验证直接调用 ejit_entry 函数能触发 JIT 编译并返回正确结果。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;
  uint32_t trafficLoad;
};

ejit_group(cell)
struct CellCfg g_cellCfg[4] = {{1,100},{2,200},{3,300},{4,400}};

ejit_entry
uint32_t jit_check_cell_type(
    EJIT_DIM(cell) uint8_t cellIndex)
{
  if (g_cellCfg[cellIndex].cellType == 1) return 10;
  if (g_cellCfg[cellIndex].cellType == 2) return 20;
  if (g_cellCfg[cellIndex].cellType == 3) return 30;
  return 40;
}

int main(int argc, char **argv) {
  uint8_t idx0 = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 0;
  uint8_t idx2 = (argc >= 3) ? (uint8_t)atoi(argv[2]) : 2;
  int failures = 0;

  // Constructor path is disabled at compile time (-enable-ejit-global-ctors=false).
  // ejit_init falls back to __ejit_registry_*[] static tables, so the config
  // must set forceStaticRegistry=true. Runtime API comes from ejit_test_helpers.h.
  extern void ejit_shutdown(void);
  ejit_config_t cfg;
  ejit_default_config(&cfg);
  cfg.forceStaticRegistry = true;
  ejit_init(&cfg);

  // Activate cell[0] (cellType=1 → expect 10)
  ejit_activate("cell", idx0);
  uint32_t v0 = jit_check_cell_type(idx0);
  if (v0 == 10) {
    printf("OK: cell[%u]=%u\n", idx0, v0);
  } else {
    printf("FAIL: cell[%u]=%u, expected 10\n", idx0, v0);
    failures++;
  }

  // Activate cell[2] (cellType=3 → expect 30)
  ejit_activate("cell", idx2);
  uint32_t v2 = jit_check_cell_type(idx2);
  if (v2 == 30) {
    printf("OK: cell[%u]=%u\n", idx2, v2);
  } else {
    printf("FAIL: cell[%u]=%u, expected 30\n", idx2, v2);
    failures++;
  }

  // Verify the async path actually compiled (not silent AOT fallback).
  ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
  {
    ejit_taskpool_stats_t ts; memset(&ts, 0, sizeof(ts));
    ejit_taskpool_get_stats(&ts);
    if (ts.asyncCompiles == 0) {
      printf("FAIL: async path produced no compiles (silent AOT)\n");
      failures++;
    }
  }
#endif

  ejit_shutdown();
  printf("OK: ejit_shutdown\n");

  printf("\n=== %s: %d failures ===\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
