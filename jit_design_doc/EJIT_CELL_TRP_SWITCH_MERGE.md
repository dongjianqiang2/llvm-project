# Ordinary Cell/TRP Final-T2 Switch Merge

## Baseline

Branch: `codex/spec5-cell-trp-switch-pr261`.
Base: bruteforceboy #244 `ejit_switch_case_v1` at `4b0b21b80`.
Integrated: #261 at `960a96793`, including its #258/local-free-dim and
pointer-witness ancestry, may_const metadata preservation, and bounded equal
PHI witness folding. Keep spec5's pipeline and pools. #260 and #262 verifier
are NOT dependencies of this branch. The older `-verify` branch is preserved
as a separate checkpoint, not this delivery's baseline.
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
```

`ejit_config_t` ABI is unchanged. Runtime mode is immutable after init and
included in the cross-core fingerprint. Default OFF; `EJIT_SWITCH_CASE` alone
does not enable merging. Old compiler output without the registered AOT body
keeps independent T2 with `final-merge no AOT fallback`, not silent success.
The compiler opt-in costs an additional retained AOT body.

### Global objects and AOT identity

An ordinary external data declaration continues to refer to its AOT storage.
With the merge compiler option, the extractor also registers the original
address for supported address-sensitive constant definitions and qualifies
unannotated private writable objects by translation unit. Both constructor
and static-section registration carry the same binding. Local data keys use
a strong hidden registration string: a collision at firmware link must fail
rather than silently bind one TU's object to another's. This is not an SDK
callback or a new source annotation. Recompile the entry TU to get the binding;
the runtime will not guess the address of an old private definition by name.

A private immutable lookup table without `unnamed_addr` is also eligible if
all uses of its address are simple loads through GEPs or bitcasts. Taking the
address out of this chain (for example returning it, storing it, or converting
it to an integer) requires an original-object binding; it is not permission
to copy an observable object. TLS and unbound observable/writable definitions
still fail closed. No new load is frozen just because its base is bound.

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
- Capacity: 64 full tuples per cohort, sufficient for 6 cell x 6 TRP (36)
  with headroom for 8 x 8 (64). This is not a restriction on the numeric IDs.
  These final-member limits are separate from `EJIT_SWITCH_CASE_MAX_ARMS`,
  which controls #244's original runtime-dimension remainder/mask transform.
- Budgets: 64,000 combined entry instructions; 8,000 instructions and 512 KiB
  artifact/constants per member. The 64-tuple limit is **per cohort**, not
  across functions. Driver-wide build defaults are 8,192 retained member
  records (`EJIT_FINAL_MERGE_MAX_MEMBERS`), 8,192 pending T1 fallback records
  (`EJIT_FINAL_MERGE_MAX_FALLBACKS`), 32 MiB total for recipes and retained bitcode
  (`EJIT_FINAL_MERGE_MAX_ARTIFACT_BYTES`), and 4,096 lifetime emission attempts
  (`EJIT_FINAL_MERGE_MAX_EMISSION_ATTEMPTS`, including failed emissions).
  These are ceilings, not reservations. The byte cap does not include indexes,
  temporary LLVM working memory or executable code. Exhaustion preserves
  the currently published T2. Shared cache slots
  and executable pool size remain independent constraints.
- Reject unbound private writable/TLS state, unbound observable private object
  identity, observable function identity, aliases/ifuncs/module asm, entry recursion, incompatible ABI/targets,
  and indirect calls without verified target remapping. These use independent
  T2, not unsafe sharing. Imported helpers/constants are member-namespaced.
- No verifier initializer or runtime is required by this branch.

### On-demand member rebuild (ordinary cell/TRP, online PGO)

The worker records the indexed profile, scalar VP selections, bound argument
size/index facts and successful field substitutions from each specialization
pass. The journal contains typed literal bits, **not** borrowed configuration
objects, T1 counters or an LLVM context. The registered source bitcode is shared.
If a recipe is smaller than the final bitcode, it is admitted instead of
requiring a permanent full body. Recent replayable bodies may also occupy a
FIFO cache within the same total budget: `EJIT_FINAL_MERGE_IR_CACHE_BYTES`
defaults to 1 MiB. Set this **runtime-package CMake cache variable** to `0`
to force replay for replayable members. No business-Clang or init change.

On a cache miss the owner worker re-runs the member's T2 IR pipeline from
the matching source, substituting the recorded folds without accessing live
business data. Both source and serialized final IR must match their original
SHA-256 digests. No sampling, independent executable emission, ORC member
replacement or business-core callback occurs during replay. Only a successful
cohort is emitted, using the existing version/pointer publication gates.
Already-ready dirty members in a cohort are considered together; staggered
late arrivals can still incur repeated worker work. This is a space/time
trade-off, not executable-code reclamation.

For runtime-dimension key discovery, unrepresentable folds, oversized journals
or recipes not smaller than IR, admission retains the original full body if
budget permits. A source/IR mismatch or an exhausted budget keeps the previous
published code; it does not substitute current configuration. A successful
`final-merge entry=...` line reports `rebuilt` (this emission),
`recipe_bytes` and `full_ir_bytes` (driver-wide current retention).

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
It also records the address of a function-local object on the first AOT call:
T1, independent T2, merged generations and the default arm must report that same
address, and a separate local table must still give the right live-index value.
Inspect the merged IR/ASM as well.

`MERGE_WAIT_ROUNDS` defaults to zero (unlimited); only the host runner sets a
finite bound. This demo uses ordinary online PGO, not verifier mode.

## Validation boundary

Revalidated on the PR261-based branch on 2026-11-12:

- Native component suite: 298 passed, one expected batch-only skip. Four
  PHI-witness tests cover 484 combinations, including unequal/unauthorized
  negative controls. The metadata merge suite passes 17/17.
- Five shuffled rounds: 297 passed per round, one batch-only skip, with the
  existing registration-freeze order-dependent producer excluded. A mutable
  static fixture in the inherited default-arm test was reset per invocation;
  its frozen-arm/live-default assertions are unchanged.
- Batch publication: 20/20 focused merge/PHI/20-function worker tests passed.
  Switch feature OFF: 260 passed, one expected batch-only skip.
- Real native independent T1/T2 processing publishes all 36 cell/TRP tuples
  to one entry. The 64-arm execution and 65-arm rejection tests also pass.
- Actual board-demo C through generated wrappers/native worker: all four
  phases pass with normal and batched publication, using ordinary online PGO.
- Frontend RAW/AOT/embedded metadata checks pass for LE/BE x O2/O3. Final
  merge wrapper opt-in, repeated execution and default-OFF checks pass.
- Seventeen BE production/demo objects compile with audit enabled; checked
  architecture/endian and undefined symbols have no wmemchr/main/verifier
  dependencies. The demo has no main and retains its shared section.

Validation uses task-owned component builds and compiler archive overlays,
reusing compatible retained LLVM/Clang support libraries rather than a fresh
complete toolchain build. Host core-ID simulation is NOT real SRE multicore
acceptance. BE objects are NOT a final SDK link. A matching full package build,
actual shared-memory/permission validation and the 6/16/6 board run remain
product acceptance gates. No performance or physical reclamation claim is made.
