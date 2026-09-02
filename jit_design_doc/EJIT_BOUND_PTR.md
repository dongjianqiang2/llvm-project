# EJIT dimension-bound pointers

`EJIT_BOUND_PTR(period)` associates a pointer parameter with an existing
`EJIT_DIM(period)` parameter. It lets EJIT specialize `ejit_may_const` fields
whose values are stable for a period instance without requiring a global array
name.

```c
typedef struct {
  uint32_t ejit_may_const algorithm;
  uint32_t ejit_may_const scale;
  uint32_t runtime_bias;
} CellConfig;

EJIT_ENTRY uint32_t process(
    EJIT_DIM(cell) uint8_t cell,
    EJIT_BOUND_PTR(cell) const CellConfig *config,
    uint32_t input) {
  return input * config->scale + config->runtime_bias;
}
```

## Runtime model

The wrapper passes a fixed descriptor containing `{rawPtr, size, argIndex}`.
The descriptor is copied into the request and queue, but the pointee bytes are
never copied. The worker reads the object through `rawPtr` only during the
actual compilation pass. The compile callback must finish all reads before it
returns.

The caller owns every object and must guarantee all of the following from
enqueue until compilation finishes:

- the object remains alive and mapped;
- producer and worker cores can use the same virtual address;
- no thread writes the object while the worker reads it;
- an active dimension specialization keeps the object's relevant contents
  stable.

To update a bound object, deactivate the dimension, modify the object, and
reactivate the dimension. Changing a pointer or its contents without that
protocol is a contract violation. The pointer and pointee contents are
transport data, not cache identity, so changing them does not create a new
cache version by itself.

There is no bound-payload allocation, copy, destructor, or cross-core free.
Queue retry, deduplication, batch layout, PGO Tier-1 and
Tier-2, delayed publication, and shutdown retain only the fixed descriptors
until the compile callback is done. Publication and cache state do not depend
on dereferencing the pointer after compilation.

## Multiple pointers

An entry may have up to eight `EJIT_BOUND_PTR` parameters. Each one must point
to a complete object type and name exactly one matching `EJIT_DIM` parameter.
The `_bound_v` runtime API carries the fixed descriptor table. The original
single-pointer `ejit_taskpool_compile_or_get_bound` API remains source and ABI
compatible, but its pointer is borrowed under the same lifetime contract.

The size in each descriptor is the readable byte range for that object. Null,
zero-sized, overflowing, duplicate-argument, and more-than-eight descriptor
lists are rejected and fall back cleanly to AOT. An object may be larger than
1 KiB; request size is constant and does not grow with the object payload.

Only marked `ejit_may_const` fields are read from the bound object. Other
fields remain ordinary loads through the pointer passed to each invocation.
No nested pointer is followed.

## Helper propagation

The root and every direct helper own their pointer contracts independently.
A helper that receives propagated bound facts must repeat
`EJIT_BOUND_PTR(period)` on that pointer formal, be an `EJIT_ENTRY`, and declare
the matching `EJIT_DIM(period)` parameter. Its bound size and `ejit_may_const`
field metadata must describe the same object range. Together these annotations
give the helper an independent wrapper/cache identity for the same cell.

Every direct call edge is checked. The helper's period argument must come from
the caller's same period formal, or from the same specialization constant after
dimension replacement; pointer and integer casts are accepted where the
existing IR pipeline proves them equivalent. Each pointer formal must also
receive the same bound pointer source and offset. A helper pointer formal
without matching `EJIT_BOUND_PTR`, an indirect or address-taken call, missing
or ambiguous dimension, different constant, or inconsistent callsite is a
conservative boundary: the bound pointer is not propagated across that edge.

## Build and compatibility

The shared request ABI is versioned for fixed raw-pointer descriptors. Rebuild
and relink the EJIT runtime library and every business package that exchanges
shared requests. Rebuild the EJIT-enabled Clang when using the multi-pointer
attribute and wrapper generation; an unchanged Clang binary cannot emit the
new wrapper path. No payload allocator or free hook is required.

Volatile, atomic, bit-field, union, dynamically indexed, and unmarked field
loads are never folded from a bound pointer.
