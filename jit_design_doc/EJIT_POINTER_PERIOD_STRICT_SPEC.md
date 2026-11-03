# Pointer-Period Folding: Strict Integration (PR251)

Status: implementation specification, first increment.
Baseline: `ejit_dev_spec5` at `700234661017`, including PR249.
Related design discussion: PR251, `ejit_mayconst_fix_plan.md`.

## 1. Goal and boundaries

Fold authorized `may_const` loads through pointer-valued period globals using
PR249's existing allocation bounds, compiler layout and lifecycle contracts.
Keep the original slot argument, stores, calls and unmarked loads dynamic.
Do not create a specialization per slot and do not replace the slot parameter
with zero in IR. This is constant substitution, not the PR231 small-table model.

This increment closes one compiler gap: after a pointer has become an absolute
address, a constant nonzero element currently misses the legacy field-offset
matcher even when the allocator lookup can prove its bounds. Discovery will be
allowed as a strict fallback for such misses. Existing successful legacy folds
retain precedence and their old behavior.

Non-goals:

- No unbounded element-zero witness path or removal of lifecycle checks.
- No inferred lifecycle or invented dimension for an entry that lacks one.
- No propagation of free-dimension assumptions into unannotated callees.
- No automatic support for arbitrary `void *` or double-indirect objects.
- No new SDK symbol, required CMake option, public ABI or shared-memory layout.
- No claim that all six product sites are fixed without their deployment data.

## 2. Required contracts

For every new strict fold, all of the following must hold:

1. The pointer-valued global is registered and its current pointee is non-null.
2. Explicit bounds or the existing allocation lookup describe that exact base.
   Bounds refer to the allocation, not to the address of the pointer variable.
3. Clang supplied a valid `ejit_pointer_layout`: a nonzero element stride and
   complete marked-field byte ranges. The allocation is a whole number of
   elements. Padding and unmarked neighboring fields are not authorized.
4. The consuming function satisfies the existing entry/period check. The
   built-in static period is the existing exception to requiring a dimension.
   A free slot does not substitute for a retained cell/TRP lifecycle.
5. Address arithmetic is non-poison and representable; every flagged GEP step
   meets the existing inbounds checks. The whole access fits both the allocation
   and one marked field after stride normalization.
6. The load is neither volatile nor atomic, and matching registrations are
   unambiguous under the existing precedence policy.
7. When a free-dimension witness is used, the application guarantees value
   invariance and validity of the zero witness. Bounds do not prove either.

Before changing folded data, retire the corresponding lifecycle instance,
update it at the existing product synchronization point, then reactivate it.
Allocation lifetime and pointer changes follow PR249's existing shutdown and
re-registration contract; this feature does not implement allocation tracking.

## 3. Decision order

For pointer-valued global candidates:

| Situation | Decision |
| --- | --- |
| Explicit bounds registered | Strict checks for constant and witness addresses; never retry legacy on failure |
| No explicit bounds, constant address accepted by legacy matcher | Preserve that match; do not require discovery or a new contract |
| No explicit bounds, legacy constant-address miss | Try allocator discovery, then the complete strict checks |
| No explicit bounds, witness address | Try allocator discovery, then the complete strict checks, as in PR249 |
| Unknown allocation or failed strict checks | Leave the load dynamic |

Keep discovered candidates separate from pre-existing eligible matches. A
discovered candidate is used only if there is no pre-existing match and exactly
one discovered match. Two discovered matches remain ambiguous even if their
physical addresses coincide. An explicit failure never borrows wider discovered
bounds. Overflow, stale bases and malformed metadata must not cause a fallback
to an unsafe read.

The change is in `tryReplacePeriodAbsoluteAddress`, not in the generic indirect
load matcher. In particular, legacy indirect constant-index folding is not
globally disabled. The phrase "nonzero indices never fold" applies only to the
old absolute-address offset-matching limitation, not to every pointer pattern.

## 4. Deployment using existing interfaces

Use the Clang that emits `ejit_pointer_layout`, and regenerate business bitcode.
Before initialization, on every image including the compile worker, either:

- Register `ejit_register_pointer_period_bounds(name, pointee, bytes)` for each
  known allocation; or
- Install `ejit_set_allocation_size_lookup(callback, context)` backed by the
  product allocator's real records. The callback returns zero for unknown or
  interior pointers, is thread-safe, and never calls EJIT recursively.

Keep the existing worker/owner initialization, permissions and lifecycle flow.
Do not add a callback that guesses a size or merely returns success. A product
without allocation records can use explicit bounds; without either source,
the new strict path deliberately does not fold.

Site triage must record: global and period, retained entry dimensions, pointer
base and size source, emitted layout, load width, constant/witness address form,
and whether the consuming function is the entry or a surviving helper.
Missing lifecycle identity is a prerequisite failure, not permission to mark
slot as `ejit_dim`. Helper-contract propagation needs a separate proof/design.

## 5. Implementation and validation

First increment:

1. Add failing-first coverage for absolute constant nonzero elements with
   discovered bounds, including flat i8 addressing.
2. Make discovery a strict fallback after legacy matching; reuse existing
   arithmetic, range, layout, lifecycle and ambiguity checks.
3. Update the free-dimension specification's discovery behavior.
4. Run focused tests plus neighboring field-offset, runtime and PGO regressions.

Second increment:

1. Preserve the original specialization context through the post-unroll field
   pass in Baseline, PGOUse and audit-only optimization. Bound-pointer views,
   root entry identity and period instances must not be replaced by an empty
   context. A missing snapshot still authorizes no fold.
2. Report strict pointer-candidate rejection reasons at VERBOSE level:
   `unknown-bounds`, `stale-pointer-base`, `missing-entry-contract`,
   `access-out-of-bounds`, `invalid-gep-witness`, and
   `layout-or-field-unauthorized`. Report ambiguous eligible registrations
   separately. A candidate rejection does not imply that every candidate for
   that load failed. Diagnostics must not query the allocator a second time.
3. Exercise the existing C runtime's asynchronous online PGO, not just a
   standalone optimizer: register before init, activate, execute 64 T1 samples,
   observe T2 publication, release every read token, deactivate, update marked
   bytes, reactivate and repeat. Check disabled requests and new functional
   results. This does not replace multi-core board acceptance.

Required regression matrix:

- Constant and witness cases; nested slots `%5` and `%10`; L2 and L3.
- Little- and big-endian IR; typed GEP and flattened absolute/i8 addresses.
- Baseline, Instrumented and PGOUse pipeline paths. Empty-profile pipeline
  tests are not evidence of real online T1 sampling and T2 execution.
- No bounds, unknown/short/nonintegral allocation, stale explicit base, missing
  layout or lifecycle, unauthorized/cross-field loads, volatile and atomic.
- Ambiguous discovery, explicit-bounds precedence, and unchanged legacy folds.
- Witness loads fold while live slot stores and call arguments remain dynamic.

Product acceptance remains separate: real allocator/explicit registration on
worker and owner, actual 64-sample T1 to T2 transition, load-fold evidence plus
functional results, lifecycle update/recompile, and actual BE multicore board
execution. Retain audit/diagnostics. A host pass, configure-only build or IR
load count alone must not be reported as board acceptance.

## 6. Deferred lightweight path

PR251's unbounded element-zero proposal is not enabled by this specification.
Equal final byte addresses do not establish full-access authorization, valid
intermediate inbounds arithmetic, lifecycle invalidation or allocation lifetime.
Any future design must explicitly prove those obligations without changing
`free_dim` semantics or overriding an explicit strict refusal.
