// Control-flow/ABI/arithmetic tests only. This mock does not prove JIT behavior.
#define REUSE_HOST_CHECK 1
#define REUSE_WAIT_ROUNDS 600u
#include "ejit_reuse_sre_test.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

uint8_t g_ucLocalCoreID;
static ejit_taskpool_stats_t mock_stats;
static ejit_representative_stats_t mock_rep;
static unsigned ctors[17], inits[17], grants, releases, held, prints, delay_calls;
static unsigned started[21], samples[21], active_groups, ready[20][6];
static int frozen[17], ctor_ready[17], fail_init, fail_delay, stall, fence_stall;
static int bad_result, bad_stats, fence_done, updated, borrow_waits;
static unsigned worker_core = 6;

static uint32_t mock_t1(unsigned e, unsigned g, uint8_t c, uint8_t t, uint32_t x) {
  assert(held == 1 && samples[g] < 64);
  ++samples[g];
  if (samples[g] == 64) {
    --active_groups;
    ++mock_rep.physicalCodeObjects;
    ++mock_rep.bundlePublications;
    ++mock_stats.asyncCompiles;
  }
  ++mock_rep.representativeDispatches;
  return reuse_entries[e](c, t, x) + (unsigned)bad_result;
}
#define MOCK_T1(N) \
  static uint32_t mock_t1_##N(uint8_t c, uint8_t t, uint32_t x) { \
    return mock_t1(N, N, c, t, x); \
  }
REUSE_EACH(MOCK_T1)
#define MOCK_T1_PTR(N) mock_t1_##N,
static ReuseFn const mock_t1_entries[] = { REUSE_EACH(MOCK_T1_PTR) };
static uint32_t mock_unequal_t1(uint8_t c, uint8_t t, uint32_t x) {
  return mock_t1(0, 20, c, t, x);
}
static uint32_t mock_unequal_t2(uint8_t c, uint8_t t, uint32_t x) {
  return reuse_0(c, t, x);
}
ejit_status_t ejit_taskpool_compile_or_get(uint32_t e, const ejit_dim_pair_t *d,
                                         uint32_t n, void **out, uint32_t *b) {
  assert(g_ucLocalCoreID == 16 && !held && e < 20 && n == 2);
  assert(d[0].dimType == 0 && d[1].dimType == 1 && d[1].instanceId == 1);
  unsigned c = d[0].instanceId;
  assert(c < 6);
  unsigned g = e == 0 && c == 5 && !updated ? 20 : e;
  *out = 0;
  *b = 0;
  if (stall) return EJIT_PENDING;
  if (!started[g]) {
    if (active_groups == 4) return EJIT_ERR_QUEUE_FULL;
    assert(c == 0 || g == 20);
    started[g] = 1;
    ++active_groups;
    ++mock_rep.representativesElected;
    ++mock_rep.groups;
    ++mock_stats.asyncCompiles;
  }
  if (samples[g] < 64) {
    if (c != 0 && g != 20) return EJIT_PENDING;
    *out = (void *)(g == 20 ? mock_unequal_t1 : mock_t1_entries[e]);
  } else {
    *out = (void *)(g == 20 ? mock_unequal_t2 : reuse_entries[e]);
    if (!ready[e][c]) {
      ready[e][c] = 1;
      ++mock_stats.readyEntries;
      if ((c && g != 20) || updated) {
        ++mock_rep.sharedPhysicalReuses;
        ++mock_rep.waitersJoined;
        ++mock_rep.waitersCompleted;
      }
    }
  }
  ++grants;
  held = 1;
  *b = 7;
  return EJIT_OK;
}
void ejit_taskpool_release_read(uint32_t b) {
  assert(held == 1 && b == 7);
  held = 0;
  ++releases;
}
void ejit_register_funcindex(const char *name, uint32_t *slot) {
  assert(ctor_ready[g_ucLocalCoreID] && !frozen[g_ucLocalCoreID]);
  for (unsigned e = 0; e < 20; ++e)
    if (!strcmp(name, reuse_names[e])) { *slot = e; return; }
  assert(0);
}
void ejit_register_lifecycle(const char *name, uint32_t *slot) {
  assert(!frozen[g_ucLocalCoreID]);
  *slot = !strcmp(name, "cell") ? 0 : 1;
}
ejit_status_t ejit_init_representative(const ejit_config_t *c) {
  assert(ctor_ready[g_ucLocalCoreID] && !frozen[g_ucLocalCoreID]);
  assert(c->compileMode == EJIT_COMPILE_ASYNC && c->optLevel == EJIT_OPT_L2);
  assert(c->forceStaticRegistry && c->enableLogger);
  ++inits[g_ucLocalCoreID];
  frozen[g_ucLocalCoreID] = 1;
  mock_rep.active = 1;
  return (ejit_status_t)fail_init;
}
ejit_status_t ejit_activate(const char *name, uint32_t c) {
  assert(!held && g_ucLocalCoreID == 16);
  if (fence_done && !strcmp(name, "cell") && c == 5) {
    assert(g_reuse_0[5].gain == 3);
    updated = 1;
  }
  return EJIT_OK;
}
ejit_status_t ejit_representative_deactivate_begin(const char *n, uint32_t c,
                                                  ejit_borrow_fence_t *f) {
  assert(!held && !strcmp(n, "cell") && c == 5 && !active_groups);
  assert(g_reuse_0[5].gain == 17 && mock_stats.readyEntries == 120);
  f->generation = 1; f->dimType = 0; f->instanceId = 5; f->version = 2;
  for (unsigned e = 0; e < 20; ++e) ready[e][5] = 0;
  mock_stats.readyEntries -= 20;
  return EJIT_OK;
}
ejit_status_t ejit_representative_borrow_status(const ejit_borrow_fence_t *f) {
  assert(f->generation == 1 && f->instanceId == 5 && f->version == 2);
  assert(g_reuse_0[5].gain == 17);
  if (fence_stall || ++borrow_waits < 3) return EJIT_PENDING;
  fence_done = 1;
  return EJIT_OK;
}
ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *s) {
  mock_stats.pendingEntries = active_groups;
  *s = mock_stats;
  return EJIT_OK;
}
unsigned ejit_taskpool_pending_count(void) { return active_groups; }
uint32_t ejit_taskpool_get_worker_core(void) { return worker_core; }
ejit_status_t ejit_representative_get_stats(ejit_representative_stats_t *s) {
  assert(g_ucLocalCoreID == 6);
  *s = mock_rep;
  if (bad_stats) s->physicalCodeObjects = 120;
  return EJIT_OK;
}
void ejit_taskpool_print_stats(void) { ++prints; }
void ejit_taskpool_print_compiled(void) { ++prints; }
void ejit_dump_func(const char *n) { assert(!strcmp(n, "reuse_0")); }
void ejit_print_dumped(const char *n) { assert(!strcmp(n, "reuse_0")); ++prints; }
void ejit_print_dumped_module(const char *n) { ejit_print_dumped(n); }
void SRE_printf(const char *format, ...) { (void)format; }
uint32_t SRE_TaskDelay(uint32_t n) {
  assert(n == 1);
  ++delay_calls;
  return (uint32_t)fail_delay;
}
void call_init_array_functions(void) {
  assert(!ctor_ready[g_ucLocalCoreID]);
  ctor_ready[g_ucLocalCoreID] = 1;
  ++ctors[g_ucLocalCoreID];
}
static void reset(void) {
  memset(&mock_stats, 0, sizeof(mock_stats));
  memset(&mock_rep, 0, sizeof(mock_rep));
  memset(ctors, 0, sizeof(ctors)); memset(inits, 0, sizeof(inits));
  memset(frozen, 0, sizeof(frozen)); memset(ctor_ready, 0, sizeof(ctor_ready));
  memset(started, 0, sizeof(started)); memset(samples, 0, sizeof(samples));
  memset(ready, 0, sizeof(ready));
  memset(g_reuse_before, 0, sizeof(g_reuse_before));
  memset(g_reuse_after, 0, sizeof(g_reuse_after));
  g_reuse_checked = g_reuse_deferred = 0; reuse_first_unequal = 0;
  grants = releases = held = prints = delay_calls = active_groups = 0;
  fail_init = fail_delay = stall = fence_stall = bad_result = bad_stats = 0;
  fence_done = updated = borrow_waits = 0;
  worker_core = 6; g_reuse_stage = REUSE_RESET; g_ucLocalCoreID = 6;
#if !REUSE_RUN_INIT_ARRAY
  ctor_ready[6] = ctor_ready[16] = 1;
#endif
}
static void ready_owner(void) {
  assert(test_ejit_period(9,8,7,6) == 0);
  g_ucLocalCoreID = 16;
}
int main(void) {
  reset();
  g_ucLocalCoreID = 16;
  assert(test_ejit_period(0,0,0,0) == -1 && !inits[16]);
  g_ucLocalCoreID = 6;
  assert(test_ejit_reuse_print(0,0,0,0) == -1 && !prints);
  assert(test_ejit_period(0,0,0,0) == 0);
  assert(test_ejit_period(1,1,1,1) == 0 && inits[6] == 1);
  g_ucLocalCoreID = 16;
  assert(test_ejit_period(1,2,3,4) == 0 && g_reuse_stage == REUSE_DONE);
  assert(test_ejit_period(0,0,0,0) == -1 && inits[16] == 1);
  assert(grants == releases && !held && g_reuse_deferred > 0);
  assert(ctors[6] == REUSE_RUN_INIT_ARRAY && ctors[16] == REUSE_RUN_INIT_ARRAY);
  assert(mock_rep.physicalCodeObjects == 21 && mock_rep.representativeDispatches == 1344);
  assert(mock_rep.sharedPhysicalReuses == 119 && borrow_waits == 3);
  g_ucLocalCoreID = 6;
  assert(test_ejit_reuse_print(0,0,0,0) == 0);
  unsigned old_grants = grants;
  assert(test_ejit_reuse_print(1,2,3,4) == 0 && grants == old_grants);
  bad_stats = 1;
  assert(test_ejit_reuse_print(0,0,0,0) == -1 && grants == old_grants);
  reset(); fail_init = -3;
  assert(test_ejit_period(0,0,0,0) == -1 && g_reuse_stage == REUSE_FAILED);
  assert(test_ejit_period(0,0,0,0) == -1 && inits[6] == 1);
  reset(); worker_core = 7;
  assert(test_ejit_period(0,0,0,0) == -1);
  reset(); ready_owner(); stall = 1;
  assert(test_ejit_period(0,0,0,0) == -1 && !updated);
  reset(); ready_owner(); fail_delay = 1;
  assert(test_ejit_period(0,0,0,0) == -1 && grants == releases);
  reset(); ready_owner(); bad_result = 1;
  assert(test_ejit_period(0,0,0,0) == -1 && grants == releases && !held);
  reset(); ready_owner(); fence_stall = 1;
  assert(test_ejit_period(0,0,0,0) == -1 && !updated && g_reuse_0[5].gain == 17);
  reset(); ready_owner(); mock_stats.compileFailed = 1;
  assert(test_ejit_period(0,0,0,0) == -1 && !grants);
  puts("PASS: mock startup, 120-identity round-robin, admission deferral, tokens, "
       "unequal/update, borrow timeout, errors and read-only print; not JIT acceptance");
  return 0;
}
