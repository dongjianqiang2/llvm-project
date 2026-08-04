/**
 * EJIT volatile / atomic may_const test.
 *
 * A volatile or atomic field must be read on every access, so it can never be
 * folded into a constant -- even when it also carries ejit_may_const. Clang
 * already withholds the per-load !ejit.may_const marker from such loads, but the
 * GV-level ejit_may_const_field offset list records the field regardless, so the
 * offset-matching paths (reAnnotateMayConst at AOT, isMayConstLoad's fallback at
 * JIT time) could hand the marker back and let the value be substituted.
 *
 * The failure is invisible in a single call: the specialization is compiled with
 * whatever the field held at the time, and only a later write reveals that the
 * value was frozen. So the test compiles, then mutates, then re-reads.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

struct Cfg {
  ejit_may_const volatile uint32_t vol; /* must NOT be folded */
  ejit_may_const uint32_t atom;         /* read atomically -> must NOT be folded */
  ejit_may_const uint32_t norm;         /* plain -> folding is expected */
};
#define N 8
ejit_period_arr(cell) struct Cfg g_cfg[N];

/* Union members alias, so a may_const member shares its offset with a mutable
   sibling. Folding a load of the mutable member would be a miscompile. */
union Sel {
  ejit_may_const uint32_t konst;
  uint32_t mut;
};
struct UCfg { union Sel u; };
ejit_period_arr(ucell) struct UCfg g_ucfg[N];

ejit_entry
uint32_t read_vol(ejit_period_arr_ind(cell) uint8_t ci) {
  return g_cfg[ci].vol + g_cfg[ci].norm;
}

ejit_entry
uint32_t read_atomic(ejit_period_arr_ind(cell) uint8_t ci) {
  return __atomic_load_n(&g_cfg[ci].atom, __ATOMIC_RELAXED) + g_cfg[ci].norm;
}

ejit_entry
uint32_t read_union_mut(ejit_period_arr_ind(ucell) uint8_t ci) {
  return g_ucfg[ci].u.mut; /* the MUTABLE member: must NOT be folded */
}

static int failures = 0;

#define CHECK(cond, fmt, ...)                                                 \
  do {                                                                        \
    if (cond) {                                                               \
      printf("  OK  : " fmt "\n", ##__VA_ARGS__);                             \
    } else {                                                                  \
      printf("  FAIL: " fmt "\n", ##__VA_ARGS__);                             \
      failures++;                                                             \
    }                                                                         \
  } while (0)

static uint64_t cache_hits(void) {
  ejit_taskpool_stats_t s;
  memset(&s, 0, sizeof s);
  ejit_taskpool_get_stats(&s);
  return s.cacheHits;
}

static uint32_t ready_entries(void) {
  ejit_taskpool_stats_t s;
  memset(&s, 0, sizeof s);
  ejit_taskpool_get_stats(&s);
  return s.readyEntries;
}

/* Call fn(ci) and prove the call was served by published JIT code rather than by
   the AOT fallback: a JIT hit takes the taskpool cache, which bumps cacheHits.
   Without this, every mutation check below would still pass if the JIT silently
   never ran -- the checks assert that a value is NOT frozen, which is exactly
   what the unspecialized fallback does.
   cacheHits is compiled out unless EJIT_STATS_ENABLE, so fall back to
   readyEntries (a live count of published entries, always available) there. */
static uint32_t jit_call(const char *what, uint32_t (*fn)(uint8_t), uint8_t ci) {
  uint64_t before = cache_hits();
  uint32_t v = fn(ci);
  uint64_t after = cache_hits();
  if (before == 0 && after == 0) {
    /* counters disabled: assert a specialization at least exists */
    CHECK(ready_entries() > 0, "%s: JIT-served (readyEntries > 0; counters off)",
          what);
  } else {
    CHECK(after > before, "%s: JIT-served (cacheHits %llu -> %llu)", what,
          (unsigned long long)before, (unsigned long long)after);
  }
  return v;
}

int main(int argc, char **argv) {
  const uint8_t ci = (argc >= 2) ? (uint8_t)atoi(argv[1]) : 5;

  printf("=== EJIT volatile / atomic may_const test (cellIdx=%u) ===\n", ci);

  g_cfg[ci].vol = 10;
  g_cfg[ci].atom = 30;
  g_cfg[ci].norm = 100;
  g_ucfg[ci].u.mut = 7;

  ejit_config_t cfg;
  ejit_default_config(&cfg);
  if (ejit_init(&cfg) != EJIT_OK) {
    printf("  FAIL: ejit_init\n");
    return 1;
  }
  ejit_activate("cell", ci);
  ejit_activate("ucell", ci);

  /* Compile the three specializations against the current values. */
  (void)read_vol(ci);
  (void)read_atomic(ci);
  (void)read_union_mut(ci);
  ejit_drain_taskpool();

  /* Draining only waits for pending work; it does not prove anything compiled or
     was published. Assert that it did, so a silent compile failure cannot make
     the mutation checks below pass through the AOT fallback. */
  ejit_taskpool_stats_t st;
  memset(&st, 0, sizeof st);
  ejit_taskpool_get_stats(&st);
  CHECK(st.readyEntries >= 3,
        "3 specializations published after drain (readyEntries=%u, "
        "compiles=%llu, failed=%llu)",
        st.readyEntries, (unsigned long long)st.asyncCompiles,
        (unsigned long long)st.compileFailed);
  CHECK(st.compileFailed == 0, "no compile failures (%llu)",
        (unsigned long long)st.compileFailed);

  CHECK(jit_call("volatile", read_vol, ci) == 110, "volatile: read = 110");
  CHECK(jit_call("atomic", read_atomic, ci) == 130, "atomic:   read = 130");
  CHECK(jit_call("union", read_union_mut, ci) == 7, "union:    read = 7");

  /* Mutate the fields the JIT must NOT have frozen. A substituted load keeps
     returning the stale value; a real load picks the new one up. */
  g_cfg[ci].vol = 20;
  __atomic_store_n(&g_cfg[ci].atom, 60, __ATOMIC_RELAXED);
  g_ucfg[ci].u.mut = 9;

  CHECK(jit_call("volatile", read_vol, ci) == 120,
        "volatile field is re-read after it changes (want 120)");
  CHECK(jit_call("atomic", read_atomic, ci) == 160,
        "atomic field is re-read after it changes (want 160)");
  CHECK(jit_call("union", read_union_mut, ci) == 9,
        "union member aliasing a may_const member is re-read (want 9)");

  ejit_shutdown();

  printf("\n=== %s: %d failure(s) ===\n", failures ? "FAIL" : "PASS", failures);
  return failures ? 1 : 0;
}
