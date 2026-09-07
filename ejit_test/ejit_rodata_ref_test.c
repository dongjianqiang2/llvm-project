/**
 * EJIT 只读数据（AOT rodata）引用集成测试 — 验证方案 1 机制
 *
 * 目的: 验证 JIT 特化代码引用 AOT 只读段中的只读数据(字符串/const 表)时,
 *       引用的是 AOT 镜像中的那一份原件, 而不是 JIT 对象自带的副本。
 *
 * 核心判据 — 地址一致性:
 *   让 ejit_entry 返回"具名只读全局的地址"(以 uintptr_t 形式, 规避
 *   wrapper 对指针返回值的支持问题), 与 main 里同一具名全局的地址比较
 *   (main 与 entry 同 TU, 直接取具名全局地址是严格同源的第一手视图):
 *     * JIT 返回地址 == AOT 地址  => JIT 引用了 AOT 原件 (方案 1 生效)
 *     * JIT 返回地址 != AOT 地址  => JIT 自带副本, 未复用 AOT 只读数据
 *   本测试在方案 1 落地前是红的(JIT 复制一份); 落地后必须变绿。
 *   内容一致性断言仅作为行为兜底(任何时候都不该错)。
 *
 * 对象级佐证 — code pool 占用:
 *   方案 1 落地后 JIT 对象不再携带 rodata 副本, 相同代码的 code pool
 *   usedBytes 应显著小于携带副本时的基线(非严格断言, 打印供人工比对;
 *   严格判据仍以地址一致性为准)。
 *
 * 覆盖的只读数据形态:
 *   1. 具名 const char* 指针   g_ro_msg -> "helloworld"
 *   2. 具名 const char 数组    g_ro_str -> "helloworld"
 *   3. 具名 const int32_t 表   g_ro_tbl (索引访问)
 *   4. 字符串字面量(匿名 GV)  — 仅做内容断言, 不做地址断言
 *
 * 运行: ./ejit_rodata_ref_test
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#ifdef EJIT_SRE_SHARED_TASKPOOL
#include <unistd.h>
#endif

#include "ejit_test_helpers.h"

//===-- AOT 侧只读数据 -------------------------------------------------------===//

// 具名只读数据: JIT 位码中同名 GV 将被外化并按注册 key 解析到 AOT 原件。
// g_ro_msg 指向的字符串与 g_ro_str 内容不同: 链接器会合并相同内容的
// 字面量/常量, 同内容会让两个探针重叠在同一地址, 削弱判据的区分度。
static const char *const g_ro_msg = "GOODBYRE";
static const char       g_ro_str[] = "helloworld";
static const int32_t    g_ro_tbl[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };

//===-- 被测 ejit_entry 函数 --------------------------------------------------===//

// 返回具名 const 指针 g_ro_msg 的目标地址 (地址一致性判据)。
ejit_entry
uintptr_t jit_ro_msg_addr(void)
{
  return (uintptr_t)g_ro_msg;
}

// 返回具名 const 数组 g_ro_str 的首地址 (地址一致性判据)。
ejit_entry
uintptr_t jit_ro_str_addr(void)
{
  return (uintptr_t)g_ro_str;
}

// 内容级检查: 字面量 + g_ro_str/g_ro_msg 内容/长度 (行为兜底)。
ejit_entry
bool jit_ro_check_content(void)
{
  return strcmp(g_ro_str, "helloworld") == 0 &&
         strlen(g_ro_str) == 10 &&
         strcmp(g_ro_msg, "GOODBYRE") == 0;
}

// 常量表索引求和 (行为兜底)。
ejit_entry
int32_t jit_ro_sum_tbl(uint8_t i)
{
  return g_ro_tbl[i] + g_ro_tbl[0];
}

//===-- 运行时 API ------------------------------------------------------------===//

extern void ejit_shutdown(void);

//===-- 断言 ------------------------------------------------------------------===//

static int g_failures = 0;

#define VERIFY(cond, fmt, ...) do {                  \
  if (!(cond)) {                                     \
    printf("  FAIL: " fmt "\n", ##__VA_ARGS__);      \
    g_failures++;                                    \
  } else {                                           \
    printf("  OK:   " fmt "\n", ##__VA_ARGS__);      \
  }                                                  \
} while (0)

//===-- main ------------------------------------------------------------------===//

int main(void)
{
  printf("=== EJIT rodata (AOT read-only data) reference test ===\n\n");

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  int rc = ejit_init(&cfg);
  VERIFY(rc == 0, "ejit_init returned %d", rc);

  printf("\n--- AOT 自检 (同 TU 具名全局的第一手地址/内容) ---\n");
  VERIFY(strcmp(g_ro_str, "helloworld") == 0 && strlen(g_ro_str) == 10,
         "AOT g_ro_str content baseline");
  VERIFY(strcmp(g_ro_msg, "GOODBYRE") == 0,
         "AOT g_ro_msg content baseline");
  VERIFY(g_ro_tbl[3] == 40 && g_ro_tbl[0] == 10, "AOT g_ro_tbl baseline");
  VERIFY((uintptr_t)g_ro_msg != (uintptr_t)g_ro_str,
         "g_ro_msg 与 g_ro_str 地址不同 (链接器未合并, 探针独立)");

  printf("\n--- 首次调用 (AOT 回退 + 触发 JIT 编译) ---\n");
  bool j_content = jit_ro_check_content();
  int32_t j_sum  = jit_ro_sum_tbl(3);
  ejit_drain_taskpool();

  printf("\n--- 行为一致性 (内容/求值, 任何实现下都必须正确) ---\n");
  VERIFY(j_content == true, "jit_ro_check_content() -> %s", j_content ? "true" : "false");
  VERIFY(j_sum == 50, "jit_ro_sum_tbl(3) = %d (expected 50)", (int)j_sum);

  printf("\n--- 地址一致性 (方案 1 核心判据: JIT 引用 AOT 原件) ---\n");
  uintptr_t jit_msg = jit_ro_msg_addr();
  uintptr_t jit_str = jit_ro_str_addr();
  // main 与被测 entry 同一翻译单元: 直接取具名全局地址, 与 JIT 位码里
  // 被外化重命名的同一 GV 是同源对象, 这是严格的第一手比较基准。
  // (不要镜像出第二份 aot_* 变量来比较: 链接器不合并普通 .rodata 数组,
  //  字面量合并只是巧合而非设计保证。)
  uintptr_t aot_m   = (uintptr_t)g_ro_msg;
  uintptr_t aot_s   = (uintptr_t)g_ro_str;
  printf("  g_ro_msg: AOT=0x%llx  JIT=0x%llx\n",
         (unsigned long long)aot_m, (unsigned long long)jit_msg);
  printf("  g_ro_str: AOT=0x%llx  JIT=0x%llx\n",
         (unsigned long long)aot_s, (unsigned long long)jit_str);
  VERIFY(jit_msg == aot_m,
         "JIT g_ro_msg 指向 AOT 原件 (jit=0x%llx aot=0x%llx)",
         (unsigned long long)jit_msg, (unsigned long long)aot_m);
  VERIFY(jit_str == aot_s,
         "JIT g_ro_str 指向 AOT 原件 (jit=0x%llx aot=0x%llx)",
         (unsigned long long)jit_str, (unsigned long long)aot_s);

  printf("\n--- 二次调用: 缓存命中且结果不变 ---\n");
  VERIFY(jit_ro_check_content() == j_content, "content check cached stable");
  VERIFY(jit_ro_sum_tbl(3) == j_sum, "sum cached stable = %d", (int)jit_ro_sum_tbl(3));

  printf("\n--- 对象级佐证: JIT 池字节占用 (无 rodata 副本应更小) ---\n");
#ifdef EJIT_SRE_CODE_POOL
  ejit_code_pool_stats_v2_t ps;
  memset(&ps, 0, sizeof(ps));
  if (ejit_get_code_pool_stats_v2(&ps) == 0) {
    printf("  code pool: used=%llu reserved=%llu wasted=%llu\n",
           (unsigned long long)ps.total.usedBytes,
           (unsigned long long)ps.total.reservedBytes,
           (unsigned long long)ps.total.wastedBytes);
  } else {
    printf("  (code pool stats unavailable — runtime without pool)\n");
  }
#else
  printf("  (test built without -DEJIT_SRE_CODE_POOL — skip)\n");
#endif

#ifdef EJIT_SRE_SHARED_TASKPOOL
  // drain 只保证 pending 清零, 不保证已完成发布: 轮询 stats 直到 4 个
  // entry 全部就绪 (上限 500ms, 参照 ejit_pgo_test 的等待模式)。
  ejit_taskpool_stats_t tp; memset(&tp, 0, sizeof(tp));
  for (int i = 0; i < 50; ++i) {
    usleep(10000);
    memset(&tp, 0, sizeof(tp));
    ejit_taskpool_get_stats(&tp);
    if (tp.readyEntries >= 4)
      break;
  }
  printf("  stats: ready=%u hits=%llu compiles=%llu\n",
         tp.readyEntries, (unsigned long long)tp.cacheHits,
         (unsigned long long)tp.asyncCompiles);
  VERIFY(tp.readyEntries >= 4,
         "JIT entries >= 4 (all entries compiled, actual %u)", tp.readyEntries);
  VERIFY(tp.compileFailed == 0, "no compile failures (%llu)",
         (unsigned long long)tp.compileFailed);
#else
  ejit_stats_t s;
  ejit_get_stats(&s);
  printf("  stats: entries=%zu hits=%llu misses=%llu\n",
         s.entryCount, (unsigned long long)s.hits, (unsigned long long)s.misses);
  VERIFY(s.entryCount >= 4,
         "JIT entries >= 4 (all entries compiled, actual %zu)", s.entryCount);
#endif

  ejit_shutdown();

  printf("\n=== %s (%d failures) ===\n",
         g_failures == 0 ? "PASS" : "FAIL", g_failures);
  return g_failures == 0 ? 0 : 1;
}
