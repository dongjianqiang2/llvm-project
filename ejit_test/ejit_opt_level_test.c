/**
 * EJIT 优化等级测试 — L1 / L2 / L3
 *
 * optLevel selects both the post-specialization IR simplification pipeline and
 * the corresponding LLVM machine-code optimization level. This test verifies
 * that JIT compilation succeeds and preserves the result at every level.
 *
 * 每个等级独立进程 (EJitRegistrationStore 只能 consume 一次)。
 *
 * 用法:
 *   ./ejit_opt_level L1|L2|L3
 *
 * 编译:
 *   build/bin/clang -O2 -c ejit_test/ejit_opt_level_test.c -o /tmp/opt_level.o
 * 链接:
 *   LIBS=$(ls build_x86/lib/x.a)
 *   clang++ -Os -Wl,--gc-sections /tmp/opt_level.o \
 *     -Wl,--whole-archive $LIBS -Wl,--no-whole-archive \
 *     -lpthread -ldl -o /tmp/ejit_opt_level
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- 外部 API ------------------------------------------------------------===//
// ejit_config_t / ejit_stats_t / ejit_taskpool_stats_t / enums / drain helper
// come from ejit_test_helpers.h (ABI-matching EJitRuntime.h).

extern void ejit_shutdown(void);

//===-- 数据结构 -------------------------------------------------------------===//

struct DevInfo {
    ejit_may_const uint32_t type;      // 1=BBU, 2=RRU, 3=AAU
    ejit_may_const uint32_t maxPower;
    ejit_may_const uint32_t channels;
    uint32_t uptime;
};

struct CarrCfg {
    ejit_may_const uint32_t freqBand;
    ejit_may_const uint32_t bandwidth;
    ejit_may_const bool     mimo;
    uint32_t txPower;
};

//===-- 全局变量 -------------------------------------------------------------===//

ejit_period(static) struct DevInfo g_dev;

#define N_CARR 8
ejit_period_arr(carrier) struct CarrCfg g_carr[N_CARR];

//===-- always_inline helper (L2 内敛目标) ----------------------------------===//

__attribute__((always_inline))
static inline uint32_t power_offset(uint32_t base, uint32_t nchan) {
    return base + nchan * 3;
}

__attribute__((always_inline))
static inline uint32_t mimo_gain(uint32_t pwr, bool mimo) {
    return mimo ? pwr + 6 : pwr;
}

//===-- ejit_entry ----------------------------------------------------------===//

ejit_entry
uint32_t process_carr(
    ejit_period_arr_ind(carrier) uint8_t ci)
{
    uint32_t r = 0;

    // L1: device type switch → constant-folded
    switch (g_dev.type) {
    case 1: r += 1000 + g_dev.maxPower*2 + g_dev.channels*5; break;
    case 2: r += 2000 + g_dev.maxPower*3; break;
    case 3: r += 3000 + g_dev.maxPower*4; break;
    default: r += 500;
    }

    // L1: carrier fields substituted → constant-folded branches
    uint32_t fb  = g_carr[ci].freqBand;
    uint32_t bw  = g_carr[ci].bandwidth;
    bool     mi  = g_carr[ci].mimo;

    switch (fb) { case 78: r+=78; break; case 41: r+=41; break;
                  case 28: r+=28; break; default: r+=100; }
    if      (bw==100) r+=10; else if (bw==40) r+=40;
    else if (bw==20)  r+=20; else             r+=1;
    if (mi) r += 6;

    // L2: always_inline inlined → constants propagated through
    uint32_t p = power_offset(40, g_dev.channels);
    p = mimo_gain(p, mi);
    r += p;

    return r;
}

//===-- main ----------------------------------------------------------------===//

int main(int argc, char **argv)
{
    const char *ls = (argc>1) ? argv[1] : "L1";
    ejit_opt_level_t lv;
    const char *ln;
    if      (!strcmp(ls,"L3")||!strcmp(ls,"l3")) { lv=EJIT_OPT_L3; ln="L3"; }
    else if (!strcmp(ls,"L2")||!strcmp(ls,"l2")) { lv=EJIT_OPT_L2; ln="L2"; }
    else                                         { lv=EJIT_OPT_L1; ln="L1"; }

    printf("=== EJIT Opt Level: %s ===\n\n", ln);

    // Init globals: type=3(AAU), maxPower=120, channels=4
    g_dev.type=3; g_dev.maxPower=120; g_dev.channels=4;
    for (int i=0; i<N_CARR; i++) {
        g_carr[i].freqBand  = (i%3==0)?78:((i%3==1)?41:28);
        g_carr[i].bandwidth = 100;
        g_carr[i].mimo      = (i%2==0);
    }

    // Init EJIT
    ejit_config_t c; memset(&c,0,sizeof(c));
#ifdef EJIT_SRE_SHARED_TASKPOOL
    // Async mode so the shared taskpool worker starts during ejit_init.
    c.compileMode=EJIT_COMPILE_ASYNC; c.optLevel=lv;
#else
    c.compileMode=EJIT_COMPILE_SYNC; c.optLevel=lv;
#endif
    c.maxCodeMemory=512*1024; c.maxDataMemory=256*1024;
    c.maxCacheEntries=64; c.maxCacheSize=1024*1024;
    if (ejit_init(&c)!=EJIT_OK) { printf("FAIL: ejit_init\n"); return 1; }
    ejit_activate("carrier",0);
    ejit_activate("carrier",1);
    ejit_activate("carrier",2);

    int failures = 0;

    // Run with 3 different carrierIdx values
    for (int t=0; t<3; t++) {
        uint8_t ci = (uint8_t)t;
        uint32_t res = process_carr(ci);

        // Compute expected (AOT ground truth — accesses real globals)
        uint32_t exp = 3480;  // AAU: 3000 + 120*4
        if      (ci%3==0) exp+=78;
        else if (ci%3==1) exp+=41;
        else             exp+=28;
        exp += 10;             // bw=100
        if (ci%2==0) exp += 6; // MIMO on for even ci
        // power: 40+4*3=52, +6 if mimo
        exp += 52 + ((ci%2==0)?6:0);

        ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
        ejit_taskpool_stats_t s; memset(&s,0,sizeof(s));
        ejit_taskpool_get_stats(&s);
        int isJit = (s.asyncCompiles > (size_t)t) ? 1 : 0;
#else
        ejit_stats_t s; memset(&s,0,sizeof(s)); ejit_get_stats(&s);
        int isJit = (s.entryCount > (size_t)t) ? 1 : 0;
#endif

        printf("[Test %d] ci=%u %s  result=%u expected=%u  ",
               t+1, ci, isJit?"[JIT]":"[AOT]", res, exp);
        if (res==exp) printf("[MATCH]\n");
        else { printf("[MISMATCH]\n"); failures++; }
    }

    ejit_drain_taskpool();
#ifdef EJIT_SRE_SHARED_TASKPOOL
    ejit_taskpool_stats_t sf; memset(&sf,0,sizeof(sf));
    ejit_taskpool_get_stats(&sf);
    printf("\nJIT: compiles=%llu hits=%llu  ",
           (unsigned long long)sf.asyncCompiles,
           (unsigned long long)sf.cacheHits);
    if (sf.asyncCompiles > 0) printf("[ACTIVE]\n");
    else { printf("[NOT ACTIVE]\n"); failures++; }
#else
    ejit_stats_t sf; memset(&sf,0,sizeof(sf)); ejit_get_stats(&sf);
    printf("\nJIT: entries=%zu misses=%llu  ", sf.entryCount,
           (unsigned long long)sf.misses);
    if (sf.entryCount > 0) printf("[ACTIVE]\n");
    else { printf("[NOT ACTIVE]\n"); failures++; }
#endif

    ejit_shutdown();
    printf("\n=== %s: %d failures ===\n", ln, failures);
    return failures>0 ? 1 : 0;
}
