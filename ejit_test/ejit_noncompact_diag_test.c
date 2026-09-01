//===-- ejit_noncompact_diag_test.c - bare-metal noncompact diag demo -----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Purpose: prove the noncompact-allocation diagnosis (commit ff2162cc37c5,
// issue #197 item 5) fires on the board when a specialized entry's graph
// carries a read-only section. One entry, ncd_rodata_check, references a
// static const table k_ncd_tbl. After specialization the period index (and
// thus the table index) becomes a compile-time constant, so InstCombine
// folds the table load into an immediate and the table is no longer
// referenced. BUT EJIT's pipeline has no GlobalOpt/GlobalDCE, so the
// internal unnamed_addr constant global is NOT removed: it stays in the
// module, lands in .rodata when lowered, and the .rodata section's block
// is carried into the JITLink LinkGraph. That section is pure-Read (R--),
// it breaks ExecOnly, the allocation falls back to page-exclusive layout
// (Compact == false), and allocate() prints:
//
//   allocate noncompact: graph=...ncd_rodata_check... section=.rodata
//                         prot=R-- size=<n> maxAlign=<a> alignExceeds=0
//
// followed by the DEBUG layout line showing total= rounded up to a page and
// layoutAlign=4096 compact=0. The .rodata section forcing a whole 4KiB page
// for a few dozen bytes is exactly the "forced page-exclusive" waste issue
// #197 item 5 describes -- and it is the dead-global case (the table is no
// longer read) that makes this a structural rather than workload-dependent
// trigger.
//
// Scope: this demo ONLY covers issue #197 item 5 (read-only section forcing
// an extra page). It does NOT cover inlined-callee corpses (item #1: same
// .text section, never trips Compact) or inter-function 16B gap (item #2:
// same .text section, never trips Compact) -- those need the print_compiled
// fn_size/overhead/16B_VIOLATIONS metrics (item #3, not yet landed) and are
// out of scope for this section-name diagnosis.
//
// Board flow (mirrors ejit_bound_ptr_sre_multicore_test.c, baseline path):
//   1. Reset the board and run test_ejit_period on core 8. It initializes
//      EJIT (baseline, no PGO), owns the shared compile worker, arms the IR
//      dump capture, prints "worker ready", and returns to its shell.
//   2. Run test_ejit_period on core 18. It attaches as a peer, activates
//      cell 1, sets the may_const field, calls ncd_rodata_check once (AOT
//      fallback, enqueues specialization), waits for the single compile to
//      publish, then calls again and verifies the JIT result matches the
//      independent AOT mirror.
//   3. Run test_ejit_ncd_print on core 8 (or read the core-18 PASS line) to
//      see print_compiled: the offender graph's alloc_start is 4KiB-aligned
//      (page-exclusive), confirming the noncompact fallback at the layout
//      level.
//
// Prerequisite: the board LLVMEJIT must be built with
// EJIT_CODE_POOL_BATCHED_PUBLISH, otherwise makeSreCodePoolManager leaves
// batchedPageSeal=false (EJitSrePlatform.cpp) and the diagnosis gate
// (usesBatchedPageSeal()) is never passed, so no `allocate noncompact:` line
// is printed. If the line is absent on the board, check the
// `makeSreCodePoolManager ... batched=...` VERBOSE line first; this is an
// issue #197 item 5 precondition, not a demo bug.
//
// Worker-core contract: this demo runs test_ejit_period on core 8 as the
// worker (NCD_WORKER_CORE=8u) and core 18 as the producer, mirroring
// ejit_bound_ptr_sre_multicore_test.c. If the board preset pins the shared
// taskpool's fixed worker core to a different core
// (EJIT_SRE_SHARED_TASKPOOL_WORKER_CORE), either run on that core, rebuild
// with -DNCD_WORKER_CORE=<that core>, or use an open-election preset; the
// pinned-core init path rejects any other core winning the owner election.
// EJIT_DIAG_ENABLE must also be ON for the diagnostic block to compile in.
//
// Do not call ejit_shutdown(): the owner worker and attached peers must stay
// alive across the per-core shell invocations.
//
//===----------------------------------------------------------------------===//

// Kept self-contained for direct board integration: no project or libc
// headers are required by this file.
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
typedef unsigned long size_t;
typedef _Bool bool;

#define true 1
#define false 0

#define EJIT_PERIOD_CONST __attribute__((ejit_period_const))
#define EJIT_IN_PERIOD_ARRAY(x) __attribute__((ejit_in_period_array(#x)))
#define EJIT_DIM(x) __attribute__((ejit_dim(#x)))
// ejit_period_guard is the canonical spelling (Attr.td's first spelling;
// ejit_period_lc is an alias). Use it for consistency with EJitRuntime.h's
// EJIT_PERIOD_GUARD and the other board demos.
#define EJIT_PERIOD_LC(x) __attribute__((ejit_period_guard(#x)))
#define EJIT_ENTRY __attribute__((ejit_entry))

#define ejit_may_const EJIT_PERIOD_CONST
#define ejit_period_arr(x) EJIT_IN_PERIOD_ARRAY(x)
#define ejit_period_arr_ind(x) EJIT_DIM(x)
#define ejit_entry EJIT_ENTRY

typedef enum {
  EJIT_OK = 0,
  EJIT_COMPILE_ERROR = -3,
} ejit_status_t;

typedef enum {
  EJIT_COMPILE_SYNC = 0,
  EJIT_COMPILE_ASYNC = 1,
} ejit_compile_mode_t;

typedef enum {
  EJIT_OPT_L1 = 1,
  EJIT_OPT_L2 = 2,
  EJIT_OPT_L3 = 3,
} ejit_opt_level_t;

typedef struct {
  ejit_compile_mode_t compileMode;
  ejit_opt_level_t optLevel;
  size_t maxCodeMemory;
  size_t maxDataMemory;
  size_t maxCacheEntries;
  size_t maxCacheSize;
  bool enableLogger;
  bool forceStaticRegistry;
  const char *dumpJITDir;
} ejit_config_t;

typedef struct {
  uint64_t cacheHits;
  uint64_t asyncCompiles;
  uint64_t asyncEnqueues;
  uint64_t alreadyPending;
  uint64_t queueFull;
  uint64_t compileFailed;
  uint64_t publishFailed;
  uint64_t instanceDisabled;
  uint64_t instanceDisabledPreActivate;
  uint32_t readyEntries;
  uint32_t pendingEntries;
  uint32_t queueApproxSize;
  uint32_t reserved;
} ejit_taskpool_stats_t;

extern ejit_status_t ejit_init(const ejit_config_t *config);
extern ejit_status_t ejit_activate(const char *periodName, uint32_t cellIdx);
extern void ejit_clear_cache(void);
// Baseline defers JITLink until explicit flush: the worker polls Baseline
// requests into pendingBatchCompiles_ and does not runCompile them until a
// flush sorts and publishes the batch. Without this call asyncCompiles never
// advances and wait_for_one_compile times out. No-op-required build flag
// EJIT_CODE_POOL_BATCHED_PUBLISH (the preset sets it).
extern ejit_status_t ejit_publish_pending_code(void);
extern unsigned ejit_taskpool_pending_count(void);
extern ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
extern void ejit_taskpool_print_stats(void);
extern void ejit_taskpool_print_compiled(void);
extern uint32_t ejit_taskpool_get_worker_core(void);
extern void ejit_dump_func(const char *name);
extern void ejit_print_dumped(const char *name);

extern void SRE_printf(const char *format, ...);
extern uint32_t SRE_TaskDelay(uint32_t tick);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

#ifndef EJIT_SHARED_SECTION_ATTR
#define EJIT_SHARED_SECTION_ATTR __attribute__((section(".mc_shared")))
#endif

#define NCD_WORKER_CORE 8u
#define NCD_PRODUCER_CORE 18u
#define NCD_CELL 1u
#define NCD_WAIT_ROUNDS 6000u
#define NCD_WAIT_TICKS 10u

enum NcdStage {
  NCD_DEMO_RESET = 0,
  NCD_DEMO_WORKER_READY = 1,
  NCD_DEMO_COMPILED = 2,
  NCD_DEMO_PRINTED = 3,
};

struct NcdCell {
  ejit_may_const uint32_t cellType;
  uint32_t runtimeBias;
};

EJIT_SHARED_SECTION_ATTR ejit_period_arr(cell)
struct NcdCell g_ncd_cells[8];
EJIT_SHARED_SECTION_ATTR volatile uint32_t g_ncd_demo_stage;
EJIT_SHARED_SECTION_ATTR volatile uint64_t g_ncd_demo_sink;

// The read-only offender: a static const table plus a SRE_printf format
// string, both inside the ejit_entry. After specialization the table index
// (from ejit_period_arr_ind) becomes a compile-time constant and InstCombine
// folds the table load into an immediate; JITLink prune would then drop the
// unreferenced table. BUT the SRE_printf format string is a string literal
// whose address is taken (passed as a pointer argument), so it cannot be
// folded and stays live: the specialized graph carries a pure-Read (.rodata)
// section, breaking ExecOnly, and allocate() prints `allocate noncompact: ...
// section=.rodata prot=R--`.
static const uint32_t k_ncd_tbl[8] = {
    11u, 22u, 33u, 44u, 55u, 66u, 77u, 88u,
};

ejit_entry uint32_t
ncd_rodata_check(ejit_period_arr_ind(cell) uint8_t cellIndex, uint32_t key) {
  uint32_t v = k_ncd_tbl[cellIndex & 7u];
  // The format string is a .rodata string literal whose address is taken;
  // it survives specialization and prune, keeping the .rodata section live in
  // the JITLink graph. Printed every call -- this is a diagnosis demo.
  SRE_printf("[NCD] ncd_rodata_check cell=%u v=%u\n",
             (unsigned)cellIndex, (unsigned)v);
  return v ^ g_ncd_cells[cellIndex].cellType ^ key;
}

// Writing a may_const (ejit_period_const) field requires ejit_period_lc on the
// writer; without it clang warns (-Wembedded-jit). The field is the
// specialization trigger, the table read is the offender.
EJIT_PERIOD_LC(cell)
void ncd_set_cell_type(ejit_period_arr_ind(cell) uint8_t cellIdx,
                       uint32_t cellType) {
  g_ncd_cells[cellIdx].cellType = cellType;
}

// Independent AOT mirror of the same computation, for verifying the JIT
// specialization returns the correct value.
static uint32_t ncd_rodata_expected(uint8_t cellIndex, uint32_t key) {
  uint32_t v = k_ncd_tbl[cellIndex & 7u];
  return v ^ g_ncd_cells[cellIndex].cellType ^ key;
}

static int wait_for_one_compile(uint64_t baseline) {
  for (uint32_t round = 0; round < NCD_WAIT_ROUNDS; ++round) {
    ejit_taskpool_stats_t stats = {0};
    (void)ejit_taskpool_get_stats(&stats);
    if (stats.compileFailed || stats.publishFailed) {
      SRE_printf("[NCD] FAIL compile=%llu publish=%llu\n",
                 (unsigned long long)stats.compileFailed,
                 (unsigned long long)stats.publishFailed);
      return 0;
    }
    if (stats.asyncCompiles >= baseline + 1u &&
        ejit_taskpool_pending_count() == 0)
      return 1;
    if ((round % 500u) == 0)
      SRE_printf("[NCD][core=18] waiting compiles=%llu/1 pending=%u\n",
                 (unsigned long long)(stats.asyncCompiles - baseline),
                 ejit_taskpool_pending_count());
    (void)SRE_TaskDelay(NCD_WAIT_TICKS);
  }
  return 0;
}

static int run_producer(void) {
  uint32_t stage = __atomic_load_n(&g_ncd_demo_stage, __ATOMIC_ACQUIRE);
  if (stage != NCD_DEMO_WORKER_READY) {
    SRE_printf("[NCD][core=18] FAIL stage=%u; run core 8 first\n", stage);
    return -3;
  }

  if (ejit_activate("cell", NCD_CELL) != EJIT_OK) {
    SRE_printf("[NCD][core=18] FAIL activate\n");
    return -4;
  }

  ejit_clear_cache();
  ejit_taskpool_stats_t before = {0};
  (void)ejit_taskpool_get_stats(&before);

  // Set the may_const field so the specialization has a reason to exist; the
  // table read is the offender, the may_const field is the specialization
  // trigger. runtimeBias is non-may_const and must stay live.
  ncd_set_cell_type(NCD_CELL, 0xC0u);
  g_ncd_cells[NCD_CELL].runtimeBias = 0;

  // First call returns AOT while the worker is pending.
  SRE_printf("[NCD][core=18] trigger ncd_rodata_check\n");
  const uint32_t key = 0x12345678u;
  uint32_t aot = ncd_rodata_check(NCD_CELL, key);
  uint32_t expectedAot = ncd_rodata_expected(NCD_CELL, key);
  if (aot != expectedAot) {
    SRE_printf("[NCD][core=18] FAIL AOT got=0x%x expected=0x%x\n", aot,
               expectedAot);
    return -5;
  }

  // Baseline path: the worker defers this compile until a flush. Request it
  // now so wait_for_one_compile can observe asyncCompiles advancing.
  if (ejit_publish_pending_code() != EJIT_OK) {
    SRE_printf("[NCD][core=18] FAIL publish_pending_code\n");
    return -5;
  }

  if (!wait_for_one_compile(before.asyncCompiles)) {
    SRE_printf("[NCD][core=18] FAIL compile timeout\n");
    ejit_taskpool_print_stats();
    return -6;
  }

  // Second call takes the JIT specialization; result must match the AOT
  // mirror (the table read is unchanged by specialization).
  uint32_t jit = ncd_rodata_check(NCD_CELL, key);
  if (jit != expectedAot) {
    SRE_printf("[NCD][core=18] FAIL JIT got=0x%x expected=0x%x\n", jit,
               expectedAot);
    return -7;
  }

  __atomic_store_n(&g_ncd_demo_sink, (uint64_t)jit, __ATOMIC_RELEASE);
  __atomic_store_n(&g_ncd_demo_stage, NCD_DEMO_COMPILED, __ATOMIC_RELEASE);
  SRE_printf("[NCD][core=18] PASS AOT/JIT=0x%x/0x%x compiles=1; grep "
             "`allocate noncompact:' for section=.rodata prot=R--\n",
             aot, jit);
  return 0;
}

static int run_worker(void) {
  uint32_t stage = __atomic_load_n(&g_ncd_demo_stage, __ATOMIC_ACQUIRE);
  if (stage == NCD_DEMO_RESET) {
    __atomic_store_n(&g_ncd_demo_sink, 0u, __ATOMIC_RELEASE);
    ejit_dump_func("ncd_rodata_check");
    __atomic_store_n(&g_ncd_demo_stage, NCD_DEMO_WORKER_READY,
                     __ATOMIC_RELEASE);
    SRE_printf("[NCD][core=8] worker ready; run test_ejit_period on core 18\n");
    return 0;
  }

  SRE_printf("[NCD][core=8] worker already initialized stage=%u; use "
             "test_ejit_ncd_print for output\n",
             stage);
  return 0;
}

int test_ejit_ncd_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  if (core != NCD_WORKER_CORE) {
    SRE_printf("[NCD][core=%u] FAIL: print is worker-local; run "
               "test_ejit_ncd_print on core 8\n",
               core);
    return -9;
  }

  uint32_t stage = __atomic_load_n(&g_ncd_demo_stage, __ATOMIC_ACQUIRE);
  if (stage < NCD_DEMO_COMPILED) {
    SRE_printf("[NCD][core=8] FAIL stage=%u; run test_ejit_period on core 18 "
               "first\n",
               stage);
    return -10;
  }
  if (stage == NCD_DEMO_PRINTED) {
    SRE_printf("[NCD][core=8] dump already printed\n");
    return 0;
  }

  ejit_dump_func("");
  SRE_printf("\n[NCD] === COMPILED VERSIONS ===\n");
  ejit_taskpool_print_compiled();
  SRE_printf("\n[NCD] === OPTIMIZED FUNCTION ===\n");
  ejit_print_dumped("ncd_rodata_check");
  SRE_printf("[NCD][core=8] expect: k_ncd_tbl load survives (read-only table "
             "not constant-folded); if `allocate noncompact:` named a section "
             "other than .rodata, inspect this IR.\n");
  SRE_printf("[NCD][core=8] PASS sink=0x%llx\n",
             (unsigned long long)__atomic_load_n(&g_ncd_demo_sink,
                                                 __ATOMIC_ACQUIRE));
  __atomic_store_n(&g_ncd_demo_stage, NCD_DEMO_PRINTED, __ATOMIC_RELEASE);
  return 0;
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;

  const uint32_t core = (uint32_t)g_ucLocalCoreID;
  SRE_printf("\n=== EJIT noncompact-rodata diagnosis demo (core=%u) ===\n",
             core);
  call_init_array_functions();

  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = true;
  cfg.forceStaticRegistry = true;

  ejit_status_t rc = ejit_init(&cfg);
  uint32_t worker = ejit_taskpool_get_worker_core();
  SRE_printf("[NCD][core=%u] init rc=%d worker=%u stage=%u\n", core, (int)rc,
             worker,
             __atomic_load_n(&g_ncd_demo_stage, __ATOMIC_ACQUIRE));
  if (rc != EJIT_OK)
    return -1;
  if (worker != NCD_WORKER_CORE) {
    SRE_printf("[NCD] FAIL worker=%u; reset and run core 8 first\n", worker);
    return -2;
  }

  if (core == NCD_WORKER_CORE)
    return run_worker();
  if (core == NCD_PRODUCER_CORE)
    return run_producer();

  SRE_printf("[NCD][core=%u] skip: use core 8 or core 18\n", core);
  return 0;
}
