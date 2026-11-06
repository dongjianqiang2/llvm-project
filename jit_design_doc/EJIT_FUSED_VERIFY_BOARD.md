# PR260 Substitution Verifier

## Purpose

Check the values that PR260 would freeze, including
`g_ptr[cell * N + slot % N].may_const_field`. The diagnostic keeps the
original load and its live slot/pointer chain, compares its runtime value with
the compiler's witness value, and leaves the real loaded value in business use.
It is a diagnostic image, not a performance optimization or automatic fallback.

This ports the existing PR239 verifier onto PR260, including shared multicore
counters. It does not change PR260's folding eligibility or include PR261.
The normal build and ordinary initializers remain non-verifying.

## Product Integration

1. Rebuild the runtime with `-DEJIT_VERIFY_SUBSTITUTION=ON`. Keep
   `EJIT_SRE_PGO_BRANCH_AUDIT=ON`, diagnostics ON and the existing shared-section
   deployment. Regenerate the lipo/runtime object using this branch's lipo tool.
2. On both participating cores, replace the existing first initializer:

   | Existing | Diagnostic replacement |
   | --- | --- |
   | `ejit_init(cfg)` | `ejit_init_verify(cfg)` |
   | `ejit_init_pgo(cfg)` | `ejit_init_verify_pgo(cfg)` |

   Pass the same config or NULL as before. Do not first initialize normally and
   then try to enable verification. Conflicting repeated init returns an error.
   This does not grow `ejit_config_t` or add SDK functions.
3. Keep existing registration, activation, wrappers, publication and lifetime
   procedures. AOT execution does not perform checks. With PGO, T1 and T2 are
   instrumented; pending/AOT intervals have no verifier coverage.
4. On the compiler core, call `ejit_verify_dump` (no arguments) after workload
   execution. It prints totals and up to 64 retained records. Call
   `ejit_verify_reset_stats` only when intentionally starting a new measurement.

No extra business Clang verifier flag is required. Use the matching EJIT
compiler/annotations and embedded bitcode as usual. The feature is runtime
opt-in, and the API symbols are absent from verifier-OFF builds.

Under the existing SRE core-private log-level deployment, the worker can keep
level 1 while the business core uses level 0 to avoid diagnostic printing there.
The verifier still executes loads, calls, shared counters and synchronization
on the business core. Suppressing logs does not make this a performance build.

## Reading Results

- `checks == 0`: **no verification coverage**, even if `sites > 0` and
  `mismatches == 0`. Check dispatch, active versions and path execution first.
- `checks > 0, mismatches == 0`: agreement for executed checks only, not proof
  for every field, branch, slot, cell or later configuration generation.
- `mismatches > 0`: runtime load differs from the candidate frozen value.
  Investigate annotation invariance, slot variation, pointer updates,
  registration, offsets and cross-core visibility. It is not automatically a
  business annotation error.
- `sites` counts emitted instrumentation since reset, not distinct executed
  sites. Warm code can have `sites == 0` after reset while checks increase.
- `records == 64`: the table is full. Extra identities still contribute to
  totals but may have no retained row. Printed names can be truncated; do not
  use display text as an identity key. `lastFrozen/lastActual` describe the last
  mismatch, not the last successful check.
- Loads that cannot be folded (for example unresolved `no-root-gv` cases) are
  not verified. This does not replace folding-rejection diagnostics.

## Standalone Board Test

Source: `ejit_test/ejit_fused_verify_sre_test.c`. Link only this demo controller,
not another file also defining `test_ejit_period`. It does not define `main`.
Use the normal board compiler/target/include flags, for example the existing
`clang -c -O2 ...` command with this filename; no new annotation or binding macro.

Requirements: verifier ON, stats ON, audit ON, existing worker-core-6 shared
taskpool and code-pool configuration. Existing `.mc_shared` and fixed-pool
mapping/coherence/permissions remain product responsibilities, as in PR256.
The source defaults to running the normal init-array procedure on each core.
If the product already does so, use `-DVERIFY260_RUN_INIT_ARRAY=0` consistently.

After a fresh boot with the same image:

```text
core[6]->test_ejit_period
core[16]->test_ejit_period
core[6]->test_ejit_fused_verify_print
```

Expected progression:

1. `WORKER_READY pgo=1` on core 6.
2. Cell 2, fused index `cell * 5 + slot % 5`, unknown pointer bounds; all five
   marked values initially agree. Normal T1 sampling and T2 publication finish.
3. With no other caller/compiler request outstanding, the demo deliberately
   changes only slot 4, then swaps the root pointer. Both calls must return the
   real new data and each must add exactly one mismatch.
4. `OWNER_DONE` on core 16, then `PASS` on core 6. Two deliberate mismatches are
   expected; they prove the detector works. The positive phase must have zero.
   Printed IR must contain a live indexed load and `__ejit_verify_check`.

The wait loop has **no deadline**. Compilation, publication and queue errors
still fail explicitly. A missing runtime stats build can leave the diagnostic
waiting; periodic progress is not PASS. Printing is read-only and cannot turn a
failed or incomplete owner run into success. Use a fresh image to repeat.
`-DVERIFY260_ONLINE_PGO=0` selects a separate baseline-only test image.

The intentional writes violate the very annotation being tested. Never copy
that mutation procedure into a production update path. Successful host tests,
BE objects or this demo's existence are not board acceptance; retain the real
6/16/6 output and final shared-section map for release verification.
