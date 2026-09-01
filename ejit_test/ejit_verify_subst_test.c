/**
 * Substitution verifier test (config.verifySubstitution). See EJitVerify.h.
 *
 * Two fields, deliberately different in kind:
 *   stableField   — written once at configuration time, never again
 *   volatileField — rewritten from live state after the JIT read it
 *
 * Asserts the verifier stays quiet for the first and fires for the second, per
 * site rather than in the totals, since a total cannot tell one moving field
 * from a whole unstable set; and that verify mode is behaviourally transparent,
 * because the loads survive.
 *
 * Section F asserts the emitted IR itself, via dumpJITDir. PASS6 runs inside
 * the runtime against a live PeriodArrayRegistry, so it is not opt-runnable and
 * cannot be covered by a lit test; the post-pipeline .ll dump is the only place
 * the instrumentation is observable as IR.
 *
 * Needs a runtime built with EJIT_VERIFY_SUBSTITUTION — build.sh reads
 * CMakeCache and only adds this test when the flag is on.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ejit_test_helpers.h"

//===-- Period data -------------------------------------------------------===//

#define N_CELLS 4

struct Cfg {
    ejit_may_const uint32_t stableField;
    ejit_may_const uint32_t volatileField;
    uint32_t pad;
};

ejit_period_arr(cell) struct Cfg g_cfg[N_CELLS];

ejit_entry
uint32_t probe(ejit_period_arr_ind(cell) uint8_t ci) {
    const struct Cfg *p = &g_cfg[ci];
    return p->stableField * 100u + p->volatileField;
}

extern void ejit_shutdown(void);

//===-- Assertions --------------------------------------------------------===//

static int g_fail = 0;
#define T(cond, fmt, ...) do {                                    \
    if (cond) printf("  OK   " fmt "\n", ##__VA_ARGS__);          \
    else      printf("  FAIL " fmt "\n", ##__VA_ARGS__), g_fail++; \
} while (0)

// Under async the first call falls back to AOT while the compile is enqueued,
// so the instrumented body is only reached on a later call.
static uint32_t settle(uint8_t ci) {
    (void)probe(ci);
    ejit_drain_taskpool();
    return probe(ci);
}

static void snapshot(ejit_verify_stats_t *s) {
    memset(s, 0, sizeof(*s));
    ejit_verify_get_stats(s);
}

//===-- Per-site lookup ---------------------------------------------------===//

#define MAX_SITES 16

// Sites are named "<func>:<global>+<byteOffset>"; the field is identified by
// the tail, which is what stays stable across builds.
static int site_ends_with(const ejit_verify_site_t *s, const char *suffix) {
    size_t n = strlen(s->site), m = strlen(suffix);
    return m <= n && strcmp(s->site + n - m, suffix) == 0;
}

static const ejit_verify_site_t *
find_site(const ejit_verify_site_t *sites, size_t n, const char *suffix) {
    for (size_t i = 0; i < n; i++)
        if (site_ends_with(&sites[i], suffix))
            return &sites[i];
    return NULL;
}

static void dump_sites(const ejit_verify_site_t *sites, size_t n) {
    for (size_t i = 0; i < n; i++)
        printf("    site[%zu] %-24s checks=%llu mismatches=%llu "
               "lastFrozen=0x%llx lastActual=0x%llx\n",
               i, sites[i].site,
               (unsigned long long)sites[i].checks,
               (unsigned long long)sites[i].mismatches,
               (unsigned long long)sites[i].lastFrozen,
               (unsigned long long)sites[i].lastActual);
}

// Does any post-pipeline dump contain \p needle? Shells out rather than
// walking the directory: the dump file name carries a cache key this test has
// no way to predict.
static int opt_ir_has(const char *dir, const char *needle) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "grep -qF -- \"%s\" %s/*_opt.ll 2>/dev/null",
             needle, dir);
    return system(cmd) == 0;
}

int main(int argc, char **argv) {
    const int verifyOn = !(argc >= 2 && argv[1][0] == 'o');
    const uint8_t ci = 0;
    ejit_verify_stats_t st;
    ejit_verify_site_t sites[MAX_SITES];
    size_t nsites;
    uint32_t r;

    printf("=== EJIT Substitution Verifier Test (verify=%d) ===\n\n", verifyOn);

    const char *tmp = getenv("TMPDIR");
    char dumpDir[256], cmd[512];
    if (!tmp) tmp = "/tmp";
    snprintf(dumpDir, sizeof(dumpDir), "%s/ejit_verify_subst_ir_%d", tmp,
             verifyOn);
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dumpDir, dumpDir);
    if (system(cmd) != 0)
        printf("  NOTE: could not prepare %s; section F will be skipped\n",
               dumpDir);

    ejit_config_t cfg;
    ejit_default_config(&cfg);
    cfg.verifySubstitution = verifyOn;
    cfg.dumpJITDir = dumpDir;
    ejit_init(&cfg);
    ejit_verify_reset_stats();

    //===-- A: instrumentation is emitted and executed -------------------===//
    printf("--- A: memory untouched since the JIT read it ---\n");

    g_cfg[ci].stableField = 4;
    g_cfg[ci].volatileField = 0;
    ejit_activate("cell", ci);

    r = settle(ci);
    T(r == 400, "probe = %u (expected 400)", r);

    snapshot(&st);
    T(st.mismatches == 0, "no mismatch while memory is untouched (%llu)",
      (unsigned long long)st.mismatches);

    if (!verifyOn) {
        // Substitution mode: the frozen value is what the JIT serves. Prove it,
        // so this run documents the failure the verifier exists to find.
        T(st.sites == 0, "verify off: nothing instrumented (%llu)",
          (unsigned long long)st.sites);
        g_cfg[ci].volatileField = 7;
        r = probe(ci);
        T(r == 400, "verify off: serves the frozen value: %u (expected 400)", r);
        T(!opt_ir_has(dumpDir, "__ejit_verify_check"),
          "verify off: no check call in the IR");
        T(!opt_ir_has(dumpDir, "!ejit.may_const"),
          "verify off: the may_const loads are substituted away");
        ejit_shutdown();
        printf("\n=== Result: %d failures ===\n", g_fail);
        return g_fail ? 1 : 0;
    }

    // sites>0 with checks==0 means the pass instrumented the module but the
    // specialized body never ran — the wrapper served AOT for every call, so
    // nothing was verified at all. That is a failure of this test's premise,
    // not a clean run: report it as one rather than passing on an empty check.
    T(st.sites > 0, "instrumented %llu may_const site(s)",
      (unsigned long long)st.sites);
    T(st.checks > 0, "executed %llu check(s)", (unsigned long long)st.checks);
    if (st.checks == 0) {
        printf("\n  The JIT never served the instrumented body, so nothing was\n"
               "  verified. Check that the runtime dispatches JIT code at all\n"
               "  (ejit_jit_verify_test) before reading anything into this.\n");
        printf("\n=== Result: %d failures (B/C/D unreachable) ===\n", g_fail);
        ejit_shutdown();
        return 1;
    }

    //===-- B: a field moves after the JIT read it -> mismatch ------------===//
    printf("\n--- B: volatileField rewritten, no activate cycle ---\n");

    const uint64_t before = st.mismatches;
    g_cfg[ci].volatileField = 7;   // exactly what a live-state field does

    r = probe(ci);
    // Verify mode kept the load, so the answer still tracks memory. In a
    // normal build this call would return the frozen 400.
    T(r == 407, "still tracks live memory: %u (expected 407)", r);

    snapshot(&st);
    T(st.mismatches > before,
      "verifier flagged the change: %llu mismatch(es)",
      (unsigned long long)st.mismatches);

    //===-- C: the stable field never trips it ---------------------------===//
    printf("\n--- C: repeated calls, stableField untouched ---\n");

    const uint64_t mid = st.mismatches;
    for (int i = 0; i < 4; i++)
        (void)probe(ci);

    snapshot(&st);
    // volatileField diverges on every call; stableField must not add to it.
    // Four calls, one diverging field -> exactly four new mismatches.
    T(st.mismatches == mid + 4,
      "only the volatile field diverges: %llu new over 4 calls (expected 4)",
      (unsigned long long)(st.mismatches - mid));

    //===-- D: the report names the field --------------------------------===//
    printf("\n--- D: per-site classification ---\n");

    memset(sites, 0, sizeof(sites));
    nsites = ejit_verify_get_sites(sites, MAX_SITES);
    dump_sites(sites, nsites);
    T(nsites == 2, "two sites recorded (actual %zu)", nsites);

    // stableField is at offset 0 of the struct, volatileField at 4.
    const ejit_verify_site_t *stable = find_site(sites, nsites, "g_cfg+0");
    const ejit_verify_site_t *moving = find_site(sites, nsites, "g_cfg+4");

    T(stable != NULL, "stableField site recorded (…g_cfg+0)");
    T(moving != NULL, "volatileField site recorded (…g_cfg+4)");

    if (stable) {
        T(stable->checks > 0, "stableField checked %llu time(s)",
          (unsigned long long)stable->checks);
        T(stable->mismatches == 0,
          "stableField never diverged (%llu) — safe to freeze",
          (unsigned long long)stable->mismatches);
    }
    if (moving) {
        T(moving->mismatches > 0,
          "volatileField diverged %llu time(s) — must not be ejit_may_const",
          (unsigned long long)moving->mismatches);
        T(moving->lastActual == 7 && moving->lastFrozen != moving->lastActual,
          "records the evidence: frozen=0x%llx actual=0x%llx",
          (unsigned long long)moving->lastFrozen,
          (unsigned long long)moving->lastActual);
    }

    //===-- E: reset ------------------------------------------------------===//
    printf("\n--- E: counter reset ---\n");

    ejit_verify_reset_stats();
    snapshot(&st);
    T(st.checks == 0 && st.mismatches == 0 && st.sites == 0,
      "counters cleared (sites=%llu checks=%llu mismatches=%llu)",
      (unsigned long long)st.sites, (unsigned long long)st.checks,
      (unsigned long long)st.mismatches);
    nsites = ejit_verify_get_sites(sites, MAX_SITES);
    T(nsites == 0, "site records cleared (%zu)", nsites);

    //===-- F: the emitted IR ---------------------------------------------===//
    printf("\n--- F: instrumented IR ---\n");

    T(opt_ir_has(dumpDir, "call void @__ejit_verify_check("),
      "post-pipeline IR calls __ejit_verify_check");
    T(opt_ir_has(dumpDir, "!ejit.verified"),
      "the checked load survives, marked !ejit.verified");
    T(opt_ir_has(dumpDir, "@.ejit.verify.site"),
      "the site name is emitted as a private constant");

    ejit_shutdown();

    printf("\n=== Result: %d failures ===\n", g_fail);
    return g_fail ? 1 : 0;
}
