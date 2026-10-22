// Standalone bare-metal smoke for the name-only global-layout contract
// (2026-10-16). Build with this revision's EJIT Clang and the matching
// llvm/ExecutionEngine/EJIT/EJitRuntime.h. No test helper header. Fresh
// isolated image (same shell entries as the other layout demos — never link
// them together):
//   core6:  test_ejit_period           (worker boot)
//   core16: test_ejit_period           (producer: phase 1 drive)
//   core16: test_ejit_nameonly_refresh (producer: phase 2 refresh drive)
//   core6:  test_ejit_nameonly_print   (read-only evidence)
// All shell arguments are ignored.
//
// SPEC (this header is authoritative):
// - PGO ON (quota 64, four admissions), async shared taskpool, worker core 6,
//   producer core 16; override with -DNAMEONLY_WORKER_CORE=<n> /
//   -DNAMEONLY_PRODUCER_CORE=<n> only before a reset image build.
// - 5 ejit_entry roots, ALL 2-dim (cell x trp), each reading ejit_may_const
//   fields of one cell and one trp period array, so different identities
//   specialize different constants. The manifest lists all five as NAME-ONLY
//   entries: entryName only, no dims, no bounds, ranks A..E.
// - Per-function identities: A(cell0,trp0), B(1,1), C(2,0), D(0,2) each
//   dispatch exactly ONE identity. E dispatches TWO: E1=(cell1,trp0) first —
//   through the generated AOT wrapper before any other E request, which fixes
//   the capture order — and E2=(cell2,trp2) every round afterwards.
// - S1 (initialize): ejit_init_global_layout must SUCCEED with name-only
//   entries for dimensional functions. The pre-2026-10-16 runtime rejected
//   exactly this input (rc=-3, misreported as a worker-core failure).
// - S2 (capture + rank): the FIRST identity of each function becomes its
//   member body; after COMPLETE the five member bodies are strictly
//   increasing A..E.
// - S3 (second identity never fails the layout): the layout must reach
//   COMPLETE with error==0 and E2's dispatches must stay business-correct.
//   E2 must reach its final Tier-2 through one of two routes: collective append
//   (unlistedAppended==1, unlistedReady observed during collection);
//   compiled through the ordinary post-COMPLETE path into the same near
//   pool, above every member body. AOT-only service or a retained sampling
//   admission is NOT a passing result. THE DRIVE ORDER IS LOAD-BEARING: the
//   member identities MUST be called before E2 every round, or E2's re-arm
//   churn starves the member's own Tier-1 and the layout never converges
//   (observed: ready frozen 4/5 for 90000+ rounds after a refresh).
// - S4 (refresh survival): after COMPLETE, a real lifecycle flip (deactivate
//   then activate cell instance 1 — the instance E1 specializes on) rebuilds
//   the layout in a new generation; re-driving must COMPLETE again with E2
//   completing its T2 (same two legal routes as S3, member bodies strictly
//   ordered within the new generation). The flip parks a transient
//   error=StaleIdentity marker while the state reads Refreshing — it is not
//   a failure and clears at the new Collecting transition. The pre-fix
//   semantics wiped the name-only declaration on refresh and FAILED the
//   layout here with error=Manifest the moment E2 reappeared.
// - Gates use ONLY API return values (status/stats); serial prints are
//   evidence, never the verdict. Business results are checked against
//   AOT-side recomputation over the same period arrays.
// - No shutdown anywhere: worker and peer must survive across shell calls.
#if !__has_attribute(ejit_entry)
#error "Use the matching EJIT Clang; ignoring unknown attributes is not valid"
#endif
#ifdef EJIT_DISABLE
#error "This smoke requires real EJIT annotations and generated wrappers"
#endif
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"
#ifndef NAMEONLY_SHARED
#define NAMEONLY_SHARED __attribute__((section(".mc_shared")))
#endif
#ifndef NAMEONLY_WORKER_CORE
#define NAMEONLY_WORKER_CORE 6u
#endif
#ifndef NAMEONLY_PRODUCER_CORE
#define NAMEONLY_PRODUCER_CORE 16u
#endif
#ifndef NAMEONLY_RUN_INIT_ARRAY
#define NAMEONLY_RUN_INIT_ARRAY 1
#endif
#ifndef NAMEONLY_WAIT_ROUNDS
// No wall-clock/round deadline on board by default. A positive override bounds
// automated runs; API errors and incorrect results still fail immediately.
#define NAMEONLY_WAIT_ROUNDS 0u
#endif
#ifndef NAMEONLY_DUMP_IR
#define NAMEONLY_DUMP_IR 1
#endif

extern void SRE_printf(const char *, ...);
extern uint32_t SRE_TaskDelay(uint32_t);
extern void call_init_array_functions(void);
extern uint8_t g_ucLocalCoreID;

// EJitRuntime.h exposes layout states as uint32_t with a documented 0..9 ABI.
enum {
  NAMEONLY_DISABLED,
  NAMEONLY_COLLECTING,
  NAMEONLY_LINKING,
  NAMEONLY_LINKED,
  NAMEONLY_SEALING,
  NAMEONLY_PUBLISHING,
  NAMEONLY_COMPLETE,
  NAMEONLY_FAILED,
  NAMEONLY_CANCELLED,
  NAMEONLY_REFRESHING
};
_Static_assert(NAMEONLY_COMPLETE == 6 && NAMEONLY_REFRESHING == 9,
               "Update local names if the public layout state ABI changes");
_Static_assert(sizeof(void *) == 8 && sizeof(ejit_config_t) == 56 &&
                   sizeof(ejit_global_layout_status_t) == 64 &&
                   sizeof(ejit_taskpool_stats_t) == 88,
               "Matching 64-bit ABI required");

// Attribute spellings come from EJitRuntime.h (ejit_entry / ejit_period_arr /
// ejit_period_arr_ind / ejit_may_const); custom __attribute__ wrappers here
// would double-expand against those aliases.

#define NAMEONLY_INSTANCES 3u
#define NAMEONLY_MEMBERS 5u
// Dispatched identities: member first-identity per rank 0..4, then E's second
// identity. Rank 4 (E) owns two entries in this table.
#define NAMEONLY_IDENTS 6u

typedef struct {
  unsigned ejit_may_const gain;
  unsigned ejit_may_const bias;
} nameonly_cfg_t;
ejit_period_arr(cell) nameonly_cfg_t g_nameonly_cell[NAMEONLY_INSTANCES] = {
    {1u, 3u}, {5u, 7u}, {9u, 11u}};
ejit_period_arr(trp) nameonly_cfg_t g_nameonly_trp[NAMEONLY_INSTANCES] = {
    {13u, 17u}, {19u, 23u}, {29u, 31u}};

// Identity table: {rank/entry, cell instance, trp instance}. Rows 0..4 are
// the members' first identities (row 4 = E1); row 5 is E2.
static const struct {
  uint32_t entry;
  uint32_t cell;
  uint32_t trp;
} nameonly_ident[NAMEONLY_IDENTS] = {
    {0u, 0u, 0u}, // A
    {1u, 1u, 1u}, // B
    {2u, 2u, 0u}, // C
    {3u, 0u, 2u}, // D
    {4u, 1u, 0u}, // E1 (member capture identity)
    {4u, 2u, 2u}, // E2 (unlisted second identity)
};

static __attribute__((noinline)) uint32_t
nameonly_business(uint32_t x, uint32_t entry, uint32_t cell, uint32_t trp) {
  uint32_t value =
      x + entry + g_nameonly_cell[cell].gain + g_nameonly_trp[trp].bias;
  for (uint32_t i = 0; i < 16; ++i)
    if ((value ^ i) & 1u)
      value = value * 3u + i;
    else
      value += 7u + entry;
  return value;
}
#define NAMEONLY_DEFINE(NAME, TAG)                                              \
  ejit_entry uint32_t nameonly_##NAME(                                          \
      ejit_period_arr_ind(cell) uint8_t ci,                                     \
      ejit_period_arr_ind(trp) uint8_t ti, uint32_t x) {                        \
    return nameonly_business(x, TAG, ci, ti);                                   \
  }
NAMEONLY_DEFINE(A, 0)
NAMEONLY_DEFINE(B, 1)
NAMEONLY_DEFINE(C, 2)
NAMEONLY_DEFINE(D, 3)
NAMEONLY_DEFINE(E, 4)
typedef uint32_t (*nameonly_fn_t)(uint8_t, uint8_t, uint32_t);
// Volatile forces an actual call boundary to the generated AOT wrapper.
static nameonly_fn_t volatile nameonly_entries[NAMEONLY_MEMBERS] = {
    nameonly_A, nameonly_B, nameonly_C, nameonly_D, nameonly_E};
static const char *const nameonly_names[NAMEONLY_MEMBERS] = {
    "nameonly_A", "nameonly_B", "nameonly_C", "nameonly_D", "nameonly_E"};
static uint32_t nameonly_indices[NAMEONLY_MEMBERS];

enum {
  DEMO_RESET,
  DEMO_STARTING,
  DEMO_READY,
  DEMO_RUNNING,
  DEMO_REFRESHING,
  DEMO_DONE,
  DEMO_FAILED
};
NAMEONLY_SHARED uint32_t g_nameonly_stage;
NAMEONLY_SHARED uint64_t g_nameonly_checked, g_nameonly_deferred;
NAMEONLY_SHARED uint32_t g_nameonly_calls[NAMEONLY_IDENTS];
NAMEONLY_SHARED uintptr_t g_nameonly_final[NAMEONLY_IDENTS];
NAMEONLY_SHARED ejit_global_layout_status_t g_nameonly_phase1, g_nameonly_phase2;
NAMEONLY_SHARED uint32_t g_nameonly_sawParked1, g_nameonly_sawParked2;
NAMEONLY_SHARED uint32_t g_nameonly_cellDim, g_nameonly_trpDim;

static int nameonly_fail(const char *why) {
  __atomic_store_n(&g_nameonly_stage, DEMO_FAILED, __ATOMIC_RELEASE);
  SRE_printf("[NAMEONLY] FAIL core=%u: %s; reset before rerun\n",
             g_ucLocalCoreID, why);
  return -1;
}
static int nameonly_delay(void) {
  return SRE_TaskDelay(1u) ? nameonly_fail("TaskDelay failed") : 0;
}
static int nameonly_status(ejit_global_layout_status_t *s,
                           ejit_taskpool_stats_t *t) {
  if (ejit_get_global_layout_status(s) != EJIT_OK ||
      ejit_taskpool_get_stats(t) != EJIT_OK)
    return nameonly_fail("status unavailable");
  if (s->state == NAMEONLY_FAILED || s->state == NAMEONLY_CANCELLED ||
      s->error || s->required != NAMEONLY_MEMBERS || !s->generation ||
      t->compileFailed || t->publishFailed || t->queueFull)
    return nameonly_fail(
        "layout/compile/publish/queue failure; core6 print details");
  if (s->activeProfiles > 4u)
    return nameonly_fail("more than four PGO admissions");
  return 0;
}
static int nameonly_idle(void) {
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (nameonly_status(&s, &t))
    return -1;
  if (s.state != NAMEONLY_COLLECTING || s.ready || s.published ||
      s.activeProfiles || t.asyncCompiles || t.readyEntries ||
      t.pendingEntries || t.queueApproxSize || ejit_taskpool_pending_count())
    return nameonly_fail("requires fresh idle image");
  return 0;
}

// A successful lookup MUST execute before release, even if order is wrong.
// Never inspect a T1 pointer and release without its real function return.
static int nameonly_call(uint32_t ident, uint32_t x, uintptr_t *seen) {
  const uint32_t entry = nameonly_ident[ident].entry;
  void *body = 0;
  uint32_t token = 0;
  ejit_status_t rc = ejit_taskpool_compile_or_get_2d(
      nameonly_indices[entry], g_nameonly_cellDim,
      nameonly_ident[ident].cell, g_nameonly_trpDim,
      nameonly_ident[ident].trp, &body, &token);
  *seen = 0;
  uint32_t actual;
  if (rc == EJIT_OK && body) {
    actual = ((nameonly_fn_t)body)((uint8_t)nameonly_ident[ident].cell,
                                   (uint8_t)nameonly_ident[ident].trp, x);
    ejit_taskpool_release_read(token);
    *seen = (uintptr_t)body;
    ++g_nameonly_calls[ident];
  } else if ((rc == EJIT_PENDING || rc == EJIT_ERR_QUEUE_FULL) && !body) {
    ++g_nameonly_deferred;
    actual = nameonly_entries[entry]((uint8_t)nameonly_ident[ident].cell,
                                     (uint8_t)nameonly_ident[ident].trp, x);
  } else
    return nameonly_fail("lookup returned invalid status/pointer");
  if (actual != nameonly_business(x, entry, nameonly_ident[ident].cell,
                                  nameonly_ident[ident].trp))
    return nameonly_fail("business mismatch");
  ++g_nameonly_checked;
  return 0;
}

// VERBOSE burst for the open post-COMPLETE second-identity stall: a bounded
// run of second-identity lookups (with a member hit interleaved as the
// working control) at EJIT_LOG_VERBOSE, so one board run names the exact
// branch that eats the lookup (2d l0 hit / 2d scan not-ready|gen|stale /
// 2d fast status / 2d slow status / prepareExec). The level is restored on
// every exit; the burst only ever runs when a second-identity compile is
// missing after the drain settled.
static int nameonly_diag_burst(void) {
  const ejit_log_level_t saved = ejit_get_log_level();
  int rc = 0;
  SRE_printf("[NAMEONLY] VERBOSE DIAG BEGIN second-identity stall\n");
  ejit_set_log_level(EJIT_LOG_VERBOSE);
  for (uint32_t i = 0; i < 8u && !rc; ++i) {
    uintptr_t ignored;
    rc = nameonly_call(NAMEONLY_IDENTS - 1u, 101u + i, &ignored);
    if (!rc)
      rc = nameonly_call(i & 1u, 201u + i, &ignored);
  }
  ejit_set_log_level(saved);
  SRE_printf("[NAMEONLY] VERBOSE DIAG END\n");
  return rc;
}

// One convergence cycle for \p generation. The MEMBER identities lead every
// round and E2 rides LAST: with per-funcIndex dedup, the round's first func4
// caller wins the claim, and a hot tolerated identity's re-arm churn (one
// ~1000-round queue residence per re-arm) can otherwise starve the member's
// Tier-1 for the WHOLE collection — the fourth board run's phase 2 froze at
// ready=4/5 exactly this way after the refresh dropped the cold-call seed.
// Member-first keeps E1's Tier-1/Tier-2 pipeline unstarved; E2 then takes
// whichever legal route wins the race (see S3). \p asyncBase is the
// asyncCompiles reading BEFORE this phase's first compile (phase 1: 0,
// guaranteed by the idle gate; phase 2: captured at the refreshed
// collection entry).
static int nameonly_drive(uint32_t generation, uint32_t phase,
                          uint64_t asyncBase) {
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (nameonly_status(&s, &t))
    return -1;
  for (uint32_t round = 0;
       !NAMEONLY_WAIT_ROUNDS || round < NAMEONLY_WAIT_ROUNDS; ++round) {
    uintptr_t ignored;
    for (uint32_t ident = 0; ident < NAMEONLY_IDENTS - 1u; ++ident) {
      if (nameonly_call(ident, round + 11u, &ignored))
        return -1;
    }
    if (nameonly_call(NAMEONLY_IDENTS - 1u, round + 11u, &ignored))
      return -1;
    if (nameonly_status(&s, &t))
      return -1;
    if (s.generation != generation)
      return nameonly_fail("unexpected generation change");
    if (s.state == NAMEONLY_COLLECTING && s.unlistedReady) {
      if (phase == 1u)
        __atomic_store_n(&g_nameonly_sawParked1, 1u, __ATOMIC_RELEASE);
      else
        __atomic_store_n(&g_nameonly_sawParked2, 1u, __ATOMIC_RELEASE);
    }
    if (s.state == NAMEONLY_COMPLETE) {
      // The manifest published; the second identity's Tier-2 arm usually
      // only lands AFTER the window published the member bodies (the member
      // slot flips to PgoUse and its re-arm churn stops) — and that window
      // tail can be shorter than the rounds the drive loop still runs, so a
      // status-only drain might never see the final compile. Keep
      // DISPATCHING every identity through the drain: the arm then lands
      // deterministically, the queue/dedup tail empties, and two compiles
      // per identity in THIS generation is the settle condition.
      for (uint32_t wait = 0;
           !NAMEONLY_WAIT_ROUNDS || wait < NAMEONLY_WAIT_ROUNDS; ++wait) {
        for (uint32_t ident = 0; ident < NAMEONLY_IDENTS; ++ident) {
          uintptr_t ignored;
          if (nameonly_call(ident, wait + 23u, &ignored))
            return -1;
        }
        if (nameonly_status(&s, &t))
          return -1;
        if (s.state != NAMEONLY_COMPLETE || s.generation != generation)
          return nameonly_fail("layout changed after completion");
        // Every identity must finish normal PGO, including a second identity
        // that first publishes T1 after the manifest reached COMPLETE.
        if (!t.pendingEntries && !t.queueApproxSize &&
            !ejit_taskpool_pending_count() &&
            !s.activeProfiles &&
            t.asyncCompiles >= asyncBase + 2u * NAMEONLY_IDENTS &&
            t.readyEntries >= NAMEONLY_IDENTS)
          break;
        if (wait % 500u == 0u)
          SRE_printf("[NAMEONLY] phase=%u drain wait=%u async=%llu ready=%u\n",
                     phase, wait, (unsigned long long)t.asyncCompiles,
                     t.readyEntries);
        if (nameonly_delay())
          return -1;
      }
      if (s.unlistedReady)
        return nameonly_fail("second-identity park gauge did not settle");
      if (t.asyncCompiles < asyncBase + 2u * NAMEONLY_IDENTS ||
          s.activeProfiles || t.pendingEntries || t.queueApproxSize ||
          ejit_taskpool_pending_count()) {
        (void)nameonly_diag_burst();
        return nameonly_fail("ordinary PGO did not settle for every identity");
      }
      SRE_printf("[NAMEONLY] phase=%u second-identity route: "
                 "unlistedAppended=%u sawParked=%u\n",
                 phase, s.unlistedAppended,
                 phase == 1u
                     ? __atomic_load_n(&g_nameonly_sawParked1, __ATOMIC_ACQUIRE)
                     : __atomic_load_n(&g_nameonly_sawParked2,
                                       __ATOMIC_ACQUIRE));
      uintptr_t previous = 0;
      for (uint32_t rank = 0; rank < NAMEONLY_MEMBERS; ++rank) {
        uintptr_t body;
        if (nameonly_call(rank, 19u, &body))
          return -1;
        if (!body || body <= previous)
          return nameonly_fail("member bodies not A..E");
        previous = g_nameonly_final[rank] = body;
        SRE_printf("[NAMEONLY] phase=%u rank=%u name=%s finalBody=%p\n",
                   phase, rank, nameonly_names[rank], (void *)body);
      }
      uintptr_t body;
      if (nameonly_call(NAMEONLY_IDENTS - 1u, 19u, &body))
        return -1;
      g_nameonly_final[NAMEONLY_IDENTS - 1u] = body;
      if (!body || body <= previous)
        return nameonly_fail("second-identity T2 missing or not above members");
      SRE_printf("[NAMEONLY] phase=%u second-identity body=%p (above "
                 "members)\n", phase, (void *)body);
      if (nameonly_status(&s, &t))
        return -1;
      if (s.state != NAMEONLY_COMPLETE || s.generation != generation ||
          s.ready != NAMEONLY_MEMBERS || s.published != NAMEONLY_MEMBERS)
        return nameonly_fail("layout changed during final execution");
      if (phase == 1u)
        g_nameonly_phase1 = s;
      else
        g_nameonly_phase2 = s;
      return 0;
    }
    if (round % 100u == 0u)
      SRE_printf("[NAMEONLY] phase=%u round=%u state=%u gen=%u ready=%u/%u "
                 "unlistedReady=%u checked=%llu\n",
                 phase, round, s.state, s.generation, s.ready, s.required,
                 s.unlistedReady, (unsigned long long)g_nameonly_checked);
    if (nameonly_delay())
      return -1;
  }
  return nameonly_fail("bounded convergence timeout; core6 print partial state");
}

int test_ejit_period(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != NAMEONLY_WORKER_CORE &&
      g_ucLocalCoreID != NAMEONLY_PRODUCER_CORE)
    return -1;
  uint32_t stage =
      g_ucLocalCoreID == NAMEONLY_WORKER_CORE ? DEMO_RESET : DEMO_READY;
  uint32_t next = g_ucLocalCoreID == NAMEONLY_WORKER_CORE ? DEMO_STARTING
                                                          : DEMO_RUNNING;
  if (!__atomic_compare_exchange_n(&g_nameonly_stage, &stage, next, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[NAMEONLY] rejected core=%u stage=%u; no reinit/rerun\n",
               g_ucLocalCoreID, stage);
    return -1;
  }
#if NAMEONLY_RUN_INIT_ARRAY
  call_init_array_functions();
#endif
  // Resolve the numeric dim types BEFORE init: ejit_register_lifecycle is
  // the AOT registration-time API (idempotent — the constructors already
  // claimed the "cell"/"trp" slots, and a repeat call returns the same
  // slots), and it is REJECTED once init freezes registration. Both cores
  // resolve the identical process-global slots.
  g_nameonly_cellDim = g_nameonly_trpDim = ~0u;
  ejit_register_lifecycle("cell", &g_nameonly_cellDim);
  ejit_register_lifecycle("trp", &g_nameonly_trpDim);
  if (g_nameonly_cellDim == ~0u || g_nameonly_trpDim == ~0u ||
      g_nameonly_cellDim == g_nameonly_trpDim)
    return nameonly_fail("lifecycle dim types unresolved");
  // Core-local registration happens after each core's constructors, before
  // init. The manifest is NAME-ONLY: five entryNames, no dims, no bounds —
  // for DIMENSIONAL (cell x trp) functions.
  ejit_layout_target_t manifest[NAMEONLY_MEMBERS] = {{0}};
  for (uint32_t slot = 0; slot < NAMEONLY_MEMBERS; ++slot) {
    nameonly_indices[slot] = ~0u;
    ejit_register_funcindex(nameonly_names[slot], &nameonly_indices[slot]);
    if (nameonly_indices[slot] == ~0u)
      return nameonly_fail("unresolved public funcIndex");
    manifest[slot].entryName = nameonly_names[slot];
  }
  ejit_config_t cfg = {0};
  cfg.compileMode = EJIT_COMPILE_ASYNC;
  cfg.optLevel = EJIT_OPT_L2;
  cfg.enableLogger = cfg.forceStaticRegistry = true;
  // S1: this call itself is the headline check — a name-only manifest for
  // 2-dim functions must initialize (the old runtime rejected it with -3).
  if (ejit_init_global_layout(&cfg, manifest, NAMEONLY_MEMBERS, true) !=
      EJIT_OK)
    return nameonly_fail("name-only manifest init/attachment failed");
  if (ejit_taskpool_get_worker_core() != NAMEONLY_WORKER_CORE)
    return nameonly_fail("worker is not the configured core");
  if (nameonly_idle())
    return -1;
  if (g_ucLocalCoreID == NAMEONLY_WORKER_CORE) {
#if NAMEONLY_DUMP_IR
    // Object-level evidence for the dual-identity function: both of E's
    // specializations compile real specialized modules.
    ejit_dump_func("nameonly_E");
#endif
    __atomic_store_n(&g_nameonly_stage, DEMO_READY, __ATOMIC_RELEASE);
    SRE_printf("[NAMEONLY] WORKER_READY members=5 all-2dim name-only "
               "normalPGO quota=64 cap=4; producer test_ejit_period\n");
    return 0;
  }
  // Producer: activate every instance, then push E's FIRST identity through
  // the generated wrapper BEFORE anything else can request E — that fixes
  // the capture identity deterministically.
  if (ejit_activate_all("cell") != EJIT_OK ||
      ejit_activate_all("trp") != EJIT_OK)
    return nameonly_fail("instance activation failed");
  if (nameonly_entries[4](1u, 0u, 11u) !=
      nameonly_business(11u, 4u, 1u, 0u))
    return nameonly_fail("cold E1 wrapper business mismatch");
  ++g_nameonly_checked;
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (nameonly_status(&s, &t))
    return -1;
  // Phase-1 async base is 0: the idle gate proved no compile happened before
  // the cold call, and the cold E1 Tier-1 is one of this phase's 12.
  if (nameonly_drive(s.generation, 1u, 0u))
    return -1;
  SRE_printf("[NAMEONLY] PHASE1_PASS gen=%u 5 name-only member bodies in "
             "rank order, second identity placed above; refresh next\n",
             g_nameonly_phase1.generation);
  return 0;
}

// Producer only: flip the cell instance E1 specializes on (deactivate +
// activate bumps its version), wait for the refreshed generation, re-drive.
int test_ejit_nameonly_refresh(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != NAMEONLY_PRODUCER_CORE)
    return -1;
  uint32_t stage = DEMO_RUNNING;
  if (!__atomic_compare_exchange_n(&g_nameonly_stage, &stage, DEMO_REFRESHING,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    SRE_printf("[NAMEONLY] refresh rejected stage=%u\n", stage);
    return -1;
  }
  if (g_nameonly_phase1.state != NAMEONLY_COMPLETE)
    return nameonly_fail("phase 1 did not complete");
  const uint32_t nextGen = g_nameonly_phase1.generation + 1u;
  if (ejit_deactivate("cell", 1u) != EJIT_OK ||
      ejit_activate("cell", 1u) != EJIT_OK)
    return nameonly_fail("lifecycle flip failed");
  // The worker observes the version bump and rebuilds autonomously; wait for
  // the refreshed collection phase of the new generation. The flip path
  // parks a TRANSIENT error=StaleIdentity marker while the state reads
  // Refreshing (invalidateMaterializedLayout on the Complete->Refreshing
  // CAS); the worker clears it at the new Collecting transition — only a
  // real failure (FAILED/CANCELLED, or an error outside Refreshing) aborts.
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  for (uint32_t wait = 0;
       !NAMEONLY_WAIT_ROUNDS || wait < NAMEONLY_WAIT_ROUNDS; ++wait) {
    if (ejit_get_global_layout_status(&s) != EJIT_OK ||
        ejit_taskpool_get_stats(&t) != EJIT_OK)
      return nameonly_fail("status unavailable");
    if (s.state == NAMEONLY_FAILED || s.state == NAMEONLY_CANCELLED ||
        (s.state != NAMEONLY_REFRESHING && s.error))
      return nameonly_fail("refresh failed the layout");
    if (s.generation == nextGen &&
        (s.state == NAMEONLY_COLLECTING || s.state == NAMEONLY_COMPLETE))
      break;
    if (s.generation > nextGen)
      return nameonly_fail("generation advanced too far");
    if (wait % 200u == 0u)
      SRE_printf("[NAMEONLY] refresh wait=%u state=%u gen=%u err=%u\n", wait,
                 s.state, s.generation, s.error);
    if (nameonly_delay())
      return -1;
  }
  if (s.generation != nextGen)
    return nameonly_fail("refresh never reached the new generation");
  if (s.state == NAMEONLY_COMPLETE)
    return nameonly_fail("refresh completed without any traffic");
  // The Collecting store is release-ordered AFTER the error/required
  // clears, so reaching here means the transient marker is gone.
  if (s.error || s.required != NAMEONLY_MEMBERS || t.compileFailed ||
      t.publishFailed || t.queueFull)
    return nameonly_fail("refreshed generation is unhealthy");
  // No generation-2 compile can start before traffic; the retained
  // generation-1 count is this phase's baseline.
  const uint64_t asyncBase2 = t.asyncCompiles;
  if (nameonly_drive(nextGen, 2u, asyncBase2))
    return -1;
  __atomic_store_n(&g_nameonly_stage, DEMO_DONE, __ATOMIC_RELEASE);
  SRE_printf("[NAMEONLY] PHASE2_PASS gen=%u name-only tolerance survived the "
             "refresh; second identity still tolerated and placed above, not a failure; "
             "worker core test_ejit_nameonly_print\n",
             g_nameonly_phase2.generation);
  return 0;
}

// Read-only: no constructor/init/refresh/lookup/activation/shutdown call.
int test_ejit_nameonly_print(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  if (g_ucLocalCoreID != NAMEONLY_WORKER_CORE)
    return -1;
  uint32_t stage = __atomic_load_n(&g_nameonly_stage, __ATOMIC_ACQUIRE);
  if (stage != DEMO_DONE && stage != DEMO_FAILED)
    return -1;
  ejit_taskpool_print_stats();
  ejit_taskpool_print_compiled();
  ejit_print_code_pool_stats();
#if NAMEONLY_DUMP_IR
  ejit_print_dumped("nameonly_E");
  ejit_print_dumped_module("nameonly_E");
#endif
  ejit_global_layout_status_t s = {0};
  ejit_taskpool_stats_t t = {0};
  if (ejit_get_global_layout_status(&s) != EJIT_OK ||
      ejit_taskpool_get_stats(&t) != EJIT_OK)
    return -1;
  // Bounded quiescence wait (read-only): the second identity's LAST Tier-1 —
  // enqueued by the diagnostic burst or the sweep mere moments before the
  // shell reaches this entry — can still be compiling. The sixth board run
  // failed ONLY this gate: the worker was publishing that Tier-1 while this
  // entry read a non-zero pending count. Let the pipeline drain first.
  for (uint32_t wait = 0;
       !NAMEONLY_WAIT_ROUNDS || wait < NAMEONLY_WAIT_ROUNDS; ++wait) {
    if (!t.pendingEntries && !t.queueApproxSize &&
        !ejit_taskpool_pending_count())
      break;
    if (wait % 1000u == 0u)
      SRE_printf("[NAMEONLY] print quiescence wait=%u pending=%u queue=%u\n",
                 wait, t.pendingEntries, t.queueApproxSize);
    if (SRE_TaskDelay(1u))
      return -1;
    if (ejit_taskpool_get_stats(&t) != EJIT_OK)
      return -1;
  }
  SRE_printf("[NAMEONLY] state=%u gen=%u required=%u ready=%u published=%u "
             "error=%u rank=%u active=%u unlistedReady=%u unlistedAppended=%u "
             "near=%llu far=%llu\n",
             s.state, s.generation, s.required, s.ready, s.published, s.error,
             s.errorRank, s.activeProfiles, s.unlistedReady, s.unlistedAppended,
             (unsigned long long)s.retainedNearBytes,
             (unsigned long long)s.retainedFarBytes);
  if (stage != DEMO_DONE || s.state != NAMEONLY_COMPLETE || s.error ||
      s.generation != g_nameonly_phase2.generation ||
      s.required != NAMEONLY_MEMBERS || s.ready != NAMEONLY_MEMBERS ||
      s.published != NAMEONLY_MEMBERS || s.activeProfiles ||
      s.unlistedReady || s.unlistedAppended > 1u ||
      t.compileFailed || t.publishFailed || t.queueFull ||
      ejit_taskpool_pending_count())
    return -1;
  // Both phases' captures stay pinned: every dispatched identity executed,
  // member bodies rank-ordered within each generation. E2 must also have an
  // executed final T2 body; an AOT-only second identity is not a passing test.
  uintptr_t previous = 0;
  for (uint32_t rank = 0; rank < NAMEONLY_MEMBERS; ++rank) {
    if (!g_nameonly_final[rank] || !g_nameonly_calls[rank] ||
        g_nameonly_final[rank] <= previous)
      return -1;
    previous = g_nameonly_final[rank];
  }
  if (!g_nameonly_calls[NAMEONLY_IDENTS - 1u] ||
      !g_nameonly_final[NAMEONLY_IDENTS - 1u] ||
      g_nameonly_final[NAMEONLY_IDENTS - 1u] <= previous)
    return -1;
  SRE_printf("[NAMEONLY] RUNTIME_CHECKS_PASS 5 name-only 2-dim members "
             "captured + rank-ordered, second identity above them (collective append or post-COMPLETE near pool), "
             "tolerance held across a lifecycle refresh (2 generations); NOT "
             "BOARD_ACCEPTED until worker PGO64/cap4/merge/preparation "
             "evidence is checked\n");
  return 0;
}
