# Ordinary Cell/TRP Final-T2 Switch Merge

## Baseline

Branch: `codex/spec5-cell-trp-switch-verify`.
Base: bruteforceboy #244 `ejit_switch_case_v1` at `4b0b21b80`.
Integrated: #262 at `34c187340`, including its folding/local-free-dim,
pointer-witness/PHI and verifier ancestry. Keep spec5's pipeline and pools.
The spec4 workspace is a preserved checkpoint, NOT the delivery target.

This adds ordinary cell/TRP merging with DIFFERENT configurations. It is not
#230 equal-code sharing or #244's existing TTI remainder/mask specialization.
The latter remains available independently.

## Runtime behavior

Each member follows independent T1 sampling and its own PGOUse pipeline.
The existing worker retains bounded final T2 bitcode, imports eligible members,
inlines entry bodies into exact cell/TRP switch arms, and publishes ONE entry
address to the included member cache slots. No hidden calls to separately
emitted member JIT functions. Live arguments, writes and returns are preserved.

```c
int merged(unsigned cell, unsigned trp, int live) {
  switch (cell) {
  case 0: if (trp == 2) return 11 + 41 + live; break;
  case 3: if (trp == 5) return 14 * 3 + 47 + live; break;
  }
  return original_aot(cell, trp, live);
}
```

This illustrates unequal values, not a measurement. Cell/TRP are not free_dim
and are not reduced modulo a slot period. Each arm keeps its own applied
profile and branch weights; no cross-member frequency distribution is invented.

## Enable

Build with `EJIT_SWITCH_CASE`, shared taskpool/shared code pointers, code pool
and `EJIT_SRE_TASKPOOL_NO_RECLAIM`. Compile entry translation units with
`-mllvm -ejit-final-version-merge`, plus normal wrapper flags such as
`-mllvm -ejit-inline-cache`. The compiler registers an ABI-identical unwrapped
AOT body through the EXISTING symbol registry, including static `.ejit_period`
registration. No SDK callbacks or additional platform headers are required.

Before init on EVERY participating core, checking return values:

```c
ejit_set_final_version_merge(1);
ejit_init_pgo(&cfg); /* cfg.compileMode = EJIT_COMPILE_ASYNC */
/* Or ejit_init_verify_pgo(&cfg) for diagnostics. */
```

`ejit_config_t` ABI is unchanged. Runtime mode is immutable after init and
included in the cross-core fingerprint. Default OFF; `EJIT_SWITCH_CASE` alone
does not enable merging. Old compiler output without the registered AOT body
keeps independent T2 with `final-merge no AOT fallback`, not silent success.
The compiler opt-in costs an additional retained AOT body.

## Publication and bounds

- Use the existing worker, near-code pool, sealing and peer permissions.
- Replace only an existing Ready T2 with matching full identity, versions,
  generation and expected pointer. Reject inactive/evicted/stale members.
  Drain inline caches with the existing drain sequence after replacement.
- Late members rebuild an eligible cohort. Cold members keep AOT/T1. Config
  changes require the existing deactivate/synchronize/update/activate contract.
- Failed assembly/emission/sealing/publication keeps existing versions usable.
  Partial publication updates only members that pass their own commit gates.
- NO_RECLAIM retains old independent T2 and merged generations. V1 initially
  emits independent T2 before incremental merging: fewer active entry addresses
  does NOT mean fewer retained code-pool bytes. Prepare-only emission and
  physical reclamation are separate future work.
- One/two named cell/TRP integer arguments; scalar C ABI. Full tuple matching,
  no modulo/key truncation. Existing runtime/wrapper dimension capacities apply.
- Limits: 8 arms, 8,000 instructions, 512 KiB/member artifact and constants,
  64 retained artifacts/4 MiB aggregate, 64 merged emission attempts (failed
  emissions included) per driver lifetime. Extra members remain independent;
  this is not an unlimited all-cell merge.
- Reject private writable/TLS state, observable private object/function
  identity, aliases/ifuncs/module asm, entry recursion, incompatible ABI/targets,
  and indirect calls without verified target remapping. These use independent
  T2, not unsafe sharing. Imported helpers/constants are member-namespaced.
- Verifier live reads, calls and site identities stay in their selected arms.

## 6/16/6 example

`ejit_test/ejit_cell_trp_merge_sre_test.c`, fresh standalone image, existing
shared-section/init-array setup. No `main`, no default wait cutoff.

```text
core[6]  -> test_ejit_period
core[16] -> test_ejit_period
core[6]  -> test_ejit_cell_trp_merge_print
```

Expected: WORKER_READY, four PHASE lines (single, pair merge, late third,
deactivate/change/retrain), OWNER_DONE, FUNCTIONAL_PASS. It checks numerical
results, live stores, equal entry pointers, and an unvisited tuple's AOT default.
Inspect the merged IR/ASM as well.

Optional `-DMERGE_VERIFY_MODE=1`: verifies real coverage with zero initial
mismatches, deliberately changes one isolated test field, requires a mismatch
from the merged arm, restores it, and prints VERIFY_PASS. Do not reproduce this
unsynchronized negative in production. `MERGE_WAIT_ROUNDS` defaults to zero
(unlimited); only the host runner sets a finite bound.

## Validation boundary

See the task validation report for native tests, feature-off, batching, host
wrapper simulation and BE probes. Host core-ID simulation is NOT real SRE
multicore acceptance. BE objects are NOT a final SDK link. A matching full
toolchain/package build, actual shared-memory/permission validation and the
6/16/6 board run remain product acceptance gates. No performance percentage or
physical reclamation claim is made.
