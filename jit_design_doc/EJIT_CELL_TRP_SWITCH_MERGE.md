# Cell/TRP Final-Version Switch Merge on spec4

Status: requirements confirmed; base migration and final-body assembly
prototype implemented. Production merge and board acceptance pending.
This is not the per-identity runtime-dimension
transform in `EJIT_SWITCH_CASE.md`.

## Confirmed goal

Different configurations of the same entry must be represented by one final
JIT function. Ordinary cell and TRP arguments select specialized arms. Equal
final code is NOT required (unlike representative exact-code reuse).

```c
int merged_entry(unsigned cell, unsigned trp, int x) {
  switch (cell) {
  case 0:
    switch (trp) {
    case 2: return x * 3 + 7;
    case 5: return x * 9 + 11;
    }
    break;
  case 3:
    if (trp == 5) return x * 2 + 13;
    break;
  }
  return original_aot(cell, trp, x);
}
```

The constants above are illustrative, not workload measurements. Passing
cell=3 must never execute cell=0's constants. Runtime uses of either argument,
stores, calls, and return values retain their original semantics. Neither
argument becomes `free_dim`, and neither is reduced modulo a slot period.

## Online PGO

Confirmed on 2026-11-12: retain independent Tier-1 sampling for each member,
then combine the prepared final Tier-2 bodies. Do not introduce shared Tier-1
counters, a shared sample quota, or a second independent profiling scheduler.

Apply each member's own profile before merging. Never replay another member's
profile hash/counters. Unprepared members keep their existing AOT/T1 path;
their configuration must not be read speculatively to manufacture extra arms.

## Implementation boundaries

1. Keep the original per-member identity, activation state, version checks,
   request token, and PGO accounting.
2. Prepare final member IR under the existing compile/publication contract.
   Retain only bounded, owned artifacts, not borrowed configuration pointers.
3. Combine eligible bodies into one dispatched function, not a trampoline
   that calls separately emitted physical per-member JIT functions.
4. Emit/seal one final code resource and publish that same function address to
   the included member cache entries through their own current-version gates.
5. Handle in-flight inline-cache fills and invalidate stale cached pointers.
   Do not free old executable memory: spec4's NO_RECLAIM policy remains.
6. A failed merge leaves usable existing member versions intact. Invalidated,
   unready, unsupported, or over-budget members must not enter frozen arms.
7. Keep module-private state, external bindings, bound-pointer contracts and
   verifier identities distinct unless equivalence is independently proven.
8. Use existing worker, permission preparation and code-pool interfaces. No
   mandatory SDK task-id API, arbitrary shared-heap callback, or demo adapter.

The grouping/admission and publication implementation must explicitly document
late join, partial groups, eviction and rebuild budgets before being enabled
in a product preset. Enabling `EJIT_SWITCH_CASE` alone does not enable this
new capability.

## Current implementation checkpoint

`EJitVersionMerge` assembles already-prepared member functions in one module.
It uses full-width one/two-dimensional switches, inlines member entry bodies,
and keeps one AOT default edge. It does not reread configuration or synthesize
profiles. It rejects conflicting tuples, unsupported ABIs/control flow,
member recursion/address identity, and exceeded instruction/arm budgets.
It preserves member branch weights and diagnostic calls without propagating
member-specific inferred attributes onto the dispatcher or AOT edge.

This API is not called by `EJitCompileDriver` yet. Importing independently
prepared T2 artifacts, bounded group storage, cache publication, inline-cache
refresh, and lifecycle integration are still pending. The native execution
test exercises the assembly result, not that missing online workflow.

## Acceptance gates

- Single cell, single TRP, and cell+TRP tuples with unequal configuration.
- Nonzero/non-one IDs and unsupported tuples; no narrowing/key collisions.
- Equal and unequal configurations, one final entry address, no out-of-line
  member-body calls hidden behind a dispatcher.
- Runtime arguments also used outside configuration loads and in stores.
- Actual independent T1 executions, profile-use success, combined T2 results.
- A cold/unvisited member does not block or acquire another member's profile.
- Deactivate/change/reactivate while compile or publish is in progress.
- Late joins, eviction, stale request completion and owner-generation change.
- Allocation/seal failures and bounded retained/retired resources.
- Verifier: unchanged configuration passes; a deliberately changed authorized
  field is detected at the original live read in the selected arm.
- Feature OFF leaves the original dispatch and PGO behavior unchanged.
- BE compile/link check and a standalone SRE example: core 6 worker, core 16
  owner, core 6 print; no `main`, no default wall-clock cutoff.

Host/IR tests do not establish product readiness or real multicore board
acceptance. Existing #244 slot tests do not satisfy these merge gates.

## Migration ledger

Base: `ejit_dev_spec4` at `6d0d402cdcbbc05730d98c3147ac8e64510552f1`.
Branch: `codex/spec4-cell-trp-switch-verify`.

Ported: #247 local free-dim, #249 bounded pointer period, #252 pointer-context
folding, #256 pointer witnesses and board examples, #258 may-const pointer
chains, #260 fused-index folding, #262 verifier, and #244 runtime-dimension
switch arms with its PGO replay. Keep spec4's fixed-pool and no-vectorization
pipeline. #250's spec5 NEON pipeline, #261's independent fixes, and unrelated
spec5 runtime features are not silently included.

The new cell/TRP merge remains a separate deliverable; this migration must not
be reported as already providing that capability.
