# Nested-array field metadata for absolute-address folding

This follow-up depends on PR252. It changes Clang metadata emission, not the
runtime ABI, lookup policy, SDK bindings or initialization API.

## Problem

For a non-pointer period global such as:

```c
struct Slot {
  unsigned char prefix[31];
  __attribute__((ejit_may_const)) _Bool flag;
  unsigned char payload[96];
};
struct Cell { struct Slot slots[5]; };
__attribute__((ejit_period_arr("cell"))) struct Cell cells[2];
```

The old global field-offset walker visited nested records but not arrays of
records. It emitted the period registration without the nested flag offsets.
A marked load rooted at `@cells` could still fold. Once its address was expressed
as an absolute byte pointer plus `(slot % 5) * 128`, the strict matcher could
not authorize it against the global field list and left it dynamic.

## Change

Non-pointer globals now reuse the existing bounded pointer-layout field walker.
The example emits element-relative offsets 31, 159, 287, 415 and 543. The
outer period-array element count remains 2, not 10. Multi-dimensional arrays
and static-period objects use the same traversal.

Only existing marked fields are described. The walker retains its exclusions
for volatile/atomic types, bit-fields, unions and unsupported base subobjects.
Its 4096-unit work budget also bounds expanded field metadata; large unmarked
arrays are traversed by element type without visiting every element. An
incomplete traversal emits no partial field list. Offsets that cannot fit the
existing i32 metadata format likewise discard the entire list rather than
wrapping into an unrelated field.

The legacy pointer-global offset path and strict pointer-layout emission are
unchanged. Runtime range, width, lifecycle, free_dim and ambiguity checks are
not relaxed. Slot remains a live argument; only authorized may_const reads
are substituted. This does not add per-slot specializations or a small table.

## Deployment and validation

Rebuild the EJIT-enabled Clang with this change and regenerate the affected
business objects and their embedded bitcode. Old bitcode does not acquire the
missing metadata by replacing the runtime library. Use the PR252 runtime or
its descendant; there is no new option or init function.

The CodeGen regression checks both target byte orders, nested arrays, exact
field offsets and exclusion/budget/overflow cases. The EJIT regression checks
the absolute modulo/shift address form across L2/L3 and compilation tiers,
including missing metadata/free_dim/registration, neighboring or over-wide
loads, volatile and atomic negative controls. A private actual-Clang to
embedded-bitcode to optimizer reproducer connects both layers.

These compiler tests do not establish product lifecycle correctness, actual
online PGO sampling or physical BE multicore board acceptance. The product
site still needs the matching period/free_dim contract and valid registration.
