# Pointer-period anchored witness folding

Implementation follow-up to [PR254](https://github.com/dongjianqiang2/llvm-project/pull/254),
on top of PR252. This is independent of PR255's frontend nested-array metadata fix.

## Purpose and usage

Permit a marked load such as `p[slot % 5].flag` or
`p[cell].slots[slot % 10].flag` to fold when the pointer's allocation size is
unknown and cell is already specialized. Nonzero cells are included under the
extended deployment contract approved on 2026-11-03.
The slot must already be an `ejit_free_dim`, and the load/field must already
carry the existing `may_const` authorization. The compiler evaluates the
address with the free dimension set to zero, reads that representative field,
and replaces only the load's result. The argument, live payload accesses and
stores keep their real slot values.

No new SDK hook, public registration API, initialization function, CMake option
or Clang flag is required. Existing `ejit_init_pgo` usage is unchanged. Rebuild
the runtime; business bitcode with PR249 pointer-layout metadata can be reused.
Missing frontend field metadata is a separate problem, not repaired by this
runtime fallback.

### Local slot variables (PR247 integration)

PR256 now also includes PR247's local-variable value boundaries. When slot is
loaded inside the entry instead of passed as a free-dim parameter, use:

```c
const uint16_t slot ejit_free_dim = input->slot;
uint8_t flag = g_1[cell].slots[slot % 5].flag;
```

The initialized integer local must remain read-only and not address-taken,
inside an `ejit_entry` with a managed dimension. The usual field invariance,
registered-root and lifecycle contracts still apply. This does not make every
memory-loaded index constant or authorize an unmarked sibling use of
`input->slot`. Real initializer evaluation, payload accesses and stores remain
dynamic; only eligible marked loads consume the temporary zero witness.

For this syntax, rebuild **both Clang and runtime from the integrated PR256**
and regenerate business embedded bitcode. Updating the runtime alone cannot
add a missing local marker to old bitcode. No additional flag, SDK callback,
initializer or public ABI is needed. See `EJIT_LOCAL_FREE_DIM.md` for limits.

## Deployment contract

The application explicitly guarantees all of the following, including for
marked loads in branches not taken by the call that triggered compilation:

1. The registered pointer slot and the contiguous allocation prefix through
   the entire selected witness element are live, initialized, coherent and
   readable by the compiler worker throughout its read. For `p[1]`, this
   includes the second element, not just `p[0]`. There is no inferred element
   count or new memory probe. A non-null pointer is not proof of this property.
2. The marked values are equal for all supported free-dimension values in the
   active configuration. The representative value is not a guess at the most
   frequent slot.
3. Existing period/lifecycle synchronization protects the pointer and the
   marked data. Updates, pointer rebinding and destruction must invalidate old
   specializations and synchronize with outstanding calls and compilation.
   A static-period object must remain valid and invariant for its lifetime.
4. A surviving helper may lack its own entry metadata, as in the legacy
   pointer-folding path. The consuming entry still owns the lifecycle contract;
   missing helper metadata does not exempt the application from it.

These are trusted annotation/deployment promises, not properties inferred from
layout metadata. The fallback adds no speculative memory probe, fault catcher,
automatic lifetime tracking or runtime equality test.

## Selection rules

- Preserve the existing constant-address legacy path and strict winners.
- Explicit bounds always select strict checking. Allocator-discovered bounds,
  including a known invalid/overflowing extent, also select strict checking.
  No lightweight retry rescues a strict rejection.
- Only an unknown-bounds, witness-derived pointer candidate can use this path.
  A size-lookup callback returning zero means unknown; installing such a
  callback does not prevent fallback for static buffers it cannot describe.
- With `ejit_pointer_layout`, the complete access must lie in one authorized
  field interval of the selected element. Nonnegative witness offsets are
  split into an element offset and an element-relative field offset. Reads
  use the selected element's actual address, never element zero's value.
- Nonzero elements require an address-chain root inside the registered
  pointer's first element. A bare absolute root already advanced beyond that
  element is not enough to identify which unknown-sized registration owns it;
  it remains unsupported. In particular, normalizing arbitrary unrelated
  addresses by the stride must not create eligible matches.
- The nonzero outer element must be selected by a retained constant typed
  index, a separate constant address step, or a fused outer index
  `add(true-constant, witness)`: the constant part must be provable without
  witness assumptions, and every other term of the index must fold to exactly
  the zero witness. Integer promotions (`zext`/`sext`/`trunc`) around the
  fused add are looked through with the same poison checks. After period
  substitution a fused `k * N + free % N` index is indistinguishable from a
  source-level `slot+K`, so this family is accepted as a whole; the declared
  costs below follow from that. A wrapped `slot-1` and a witness leaf that
  evaluates nonzero (for example `xor(slot,1)`) are still not granted a
  larger readable extent.
- After trying existing legacy/strict paths, the pass materializes registered
  roots and retries unresolved field loads in the same round. This preserves
  the anchor before InstCombine folds the nonzero cell offset into the root.
  It supports normal source `g_1[cell].slots[slot % N]`, including cell 1,
  without changing business source or requiring another SDK registration.
- Integer poison/wrap checks and every intermediate flagged `inbounds` GEP
  remain checked. The guaranteed prefix through the selected element is used
  for these checks; it is not published as an allocation size. A chain that
  leaves this prefix and comes back is conservatively rejected. Extent/address
  overflow is rejected before any pointed-to field is read.
- Null bases, malformed layouts, unauthorized/widened fields, volatile/atomic
  loads and ambiguous eligible registrations remain rejected. An unbounded
  alias cannot rescue strict rejection of the same pointer base.
- Pure constant `p[k]` accesses gain no new permission. Existing legacy folds
  and PR252's bounded/discovered folds keep their existing behavior.

Accepting the fused-index family revises PR254 v2's "element zero only,
`free+K` refused" stance under the same deployment contract as the typed
nonzero extension: the constant part selects the element exactly as a
literal `p[k]` would, the readable-prefix promise covers the selected
witness element, and value correctness across free dimensions remains the
application's `free_dim + may_const` invariance promise — the fused
acceptance adds no value-level trust. The cost is coverage, not safety:
annotations that previously never folded because of the refusal now
activate, so an incorrectly promised `slot+K` site freezes a wrong value
instead of staying live, and board acceptance must diff audit counts (the
expected delta equals the known fused sites — including raw `free*N + K`
shapes, not just `k*N + free%N`; anything beyond that needs site-by-site
investigation). A successful fold still reports
`lightweight-anchored-element-witness`; a declined fused index still
reports `nonzero-witness-dynamic-element`.

Legacy offset-only metadata can authorize an exact field start only when an
aggregate-rooted GEP still describes the first element and its field width.
The element-relative field offset may be used for a nonzero element only
while that aggregate root still equals the registered pointer value.
The shared field-size walker checks the innermost field, not a later pointer
cast's apparent type. Malformed present layouts never fall back to offsets.
Opaque byte chains with neither a layout nor recoverable aggregate type remain
unfolded. This intentionally narrows PR254 v2's unrestricted offset-only
fallback: a bare offset is not a width bound.

## Diagnostics and acceptance

Verbose diagnostics report `lightweight-anchored-element-witness` on a selected
fallback. Rejection reasons include `witness-before-pointer-base`,
`nonzero-witness-unanchored-root`, `nonzero-witness-dynamic-element`,
`witness-extent-overflow`,
`legacy-witness-missing-type`, `invalid-gep-witness`,
`layout-or-field-unauthorized`, `strict-pointer-alias` and
`ambiguous-lightweight-registration`. Existing may-const and branch audits stay
enabled. Successful optimization does not prove the deployment contract.

Tests cover typed/nested and flattened `slot % 5 / % 10` accesses, including
`inttoptr(base + field) + ((slot % N) << 7)`, LE/BE IR, L2/L3,
Baseline/Instrumented/PGOUse, live stores, legacy typed offsets and negative
authorization cases. A real native online-PGO test uses no allocation-size
callback or bounds registration, reaches 64 T1 executions, publishes T2,
deactivates, changes configuration and repeats.
The nonzero-element extension repeats this online test at element 1 and adds
cell 1/2/15, both byte orders, typed legacy/layout metadata, dynamic-cell,
overflow, intermediate-escape, unrelated-root and strict-alias controls.
The fused-index extension covers elements 0/1/2/15 with N=5/10 in typed and
single-index byte form across byte orders and tiers, rejects `xor(slot,1)`,
`slot-1`, unmapped dynamic outer arguments and extent overflow, keeps an
unmarked sibling load live, and flips the former `slot+1` refusal into an
accepted case with a per-element value check.

Board acceptance still requires rebuilding the product runtime, running the
same business workload with the established worker/owner initialization,
checking final T2 IR and audit counts, and comparing outputs before and after
a synchronized configuration update. Host tests and BE object compilation are
not multicore SRE/SDK acceptance. Enabling previously dormant annotations can
expose incorrect business promises; additional folds must be audited rather
than assuming that only previously reported sites will change.

## C-source reproduction

For the dedicated **core6 -> core16 -> core6** board test, use
[`EJIT_POINTER_WITNESS_BOARD.md`](EJIT_POINTER_WITNESS_BOARD.md) and its single
C `ejit_test/ejit_pointer_witness_sre_test.c`. It exercises unknown bounds at
cell1, seven marked fields, full-width i64, `%5/%10`, offsets and strides,
normal 64-entry online PGO, live slot reads/writes and two generations. It
includes `load(input+20) -> local free_dim -> %5 -> *184 -> +14`. It
does not install the exact allocation-size callback used by the older
pointer-period board demo. Default board waits are unlimited.

`ejit_test/ejit_pointer_witness_source_repro.c` is compiler-only input, not an
SRE shell test. It deliberately declares, rather than supplies, the external
storage and sink. It has no `main` or board initialization entry. The three
entries cover direct access, taking `&g_1[cell].g_2[slot % N]` then reading,
and taking the same address through an always-inline reader. Each sink also
receives the real selected address, so removing the load must not redirect
the pointer to slot zero. Slot stride is 128 bytes and the marked byte is at
offset 31, matching the reported modulo/shift addressing shape.

The C-source validation feeds the actual frontend embedded bitcode to the
original PR252 field pass and the PR256 field pass with identical remaining
pipeline components. It does not replace function bodies or synthesize field
metadata. Set up two readable Cell objects; flags in cell0 are all one and
flags in cell1 all zero. Register only the normal `g_1` period pointer slot,
without pointer bounds or an allocation lookup for the positive case.

Acceptance matrix (three entries x five cases x two optimization levels x
three pipeline tiers x two slot counts x two byte orders = 360 cases):

| Case | Expected final marked load |
| --- | --- |
| cell0, unknown bounds, free_dim present | Removed, sink flag is constant 1 |
| cell1, unknown bounds, native LE source root | Removed, sink flag is constant 0 |
| cell1, unknown bounds, BE-on-LE pre-materialized root | Retained: specialization has erased the anchor |
| cell1, explicit complete bounds | Removed, sink flag is constant 0 |
| cell0, free_dim absent | Retained |
| cell0, pointer registration absent | Retained |

Both `%5` and `%10`, L2/L3 and Baseline/Instrumented/PGOUse configurations
were checked. The original element-zero version of PR256 retained the 72
unknown-bounds cell1 combinations. The 36 native LE combinations are now
positive, with distinct values checking that cell1 is not redirected to cell0.
The 36 BE-on-LE model combinations remain negative: the fixture materializes
the root before specialization, which then absorbs the constant cell offset.
That is not the root materialization order of a native worker. Anchored
nonzero-element IR is checked separately in both byte orders.
The real slot argument
and its dependent selected address remain live in every case. These tier
configuration checks do not themselves run a live PGO profile; real online
64-entry PGO and renewal validation is separate, as described above.

The LE matrix uses the frontend pointer load unchanged. BE checks run on an
LE host only after explicit root-pointer materialization; the frontend GEP,
load and metadata remain intact. This is not a BE runtime or board execution.

In the positive final IR, the flag load disappears and the call becomes
`repro_sink(1, slot, selected)`. The `urem`/GEP may remain because `selected`
must still identify the real slot. A surviving `urem` alone is not evidence
that may_const folding failed. Product sites still require their real
metadata and rejection reason to establish the same diagnosis.
