# `ejit_free_dim` — design spec

**Status**: implemented
**Branch**: `ejit_free_dim` (base `origin/ejit_dev_spec4`)
**Related**: `PASS6_EJitStructFieldPass.md`, `EJIT_CONST_DIM.md` (earlier keyed design, superseded)

---

## 1. Problem

A parameter EJIT cannot fold blocks every `may_const` field reached through it,
including the ones that never depended on it.

```c
ejit_entry
void init_unit_params(uint32_t ejit_dim("unit") unitIdx,
                      uint32_t                 slotNo)
{
    UnitParams *p = &g_unitParams[unitIdx * 5 + slotNo % 5];
    // ~30 may_const fields. None can be specialized today.
}
```

PASS6 needs the **whole** GEP index constant. `unitIdx` folds, `slotNo` does not, so
the sum does not — every load fails with `non-const-offset`. That includes the 70–80% of
fields copied from a slot-independent base, which are identical in all five elements.

> `slotNo` does not make the data unstable. It makes the data **unaddressable**.

---

## 2. The attribute

No arguments.

```c
#define ejit_free_dim __attribute__((ejit_free_dim))

ejit_entry
void init_unit_params(uint32_t ejit_dim("unit") unitIdx,
                      uint32_t ejit_free_dim    slotNo);
```

**Applies to**: an integer parameter of an `ejit_entry` function. No width limit:
unlike a specialization dimension, a free dim never reaches the runtime ABI, so
there is no narrowing through which two values could alias. The only value ever
substituted is the witness.

**Asserts**, for every `ejit_may_const` field whose address depends on this parameter:

1. **Value invariance** — the field holds the same value for every value the parameter
   takes. The data is *free of* this dimension; hence the name.
2. **Witness validity** — the address the field has at parameter value `0` is a valid
   address of that field, holding that same value.

(2) does not follow from (1), and needs saying. `arr[slot - 1]` with `slot ∈ 1..5` can
satisfy (1) in all five valid elements while the witness names `arr[-1]` — an address the
program never forms. §3 covers what the implementation can and cannot check.

**Does not assert** anything about the parameter itself. `slotNo` changes every TTI. The
claim is about the memory it addresses, not about the argument.

Because the values do not vary along the axis, the JIT evaluates addresses at a fixed
**witness of 0** — equivalent to dropping the `+ slotNo % 5` term. One specialization
serves every slot value.

---

## 3. The one rule

> **The witness is used only inside PASS6's address arithmetic. It never enters the IR.**

The parameter is **not** RAUW'd. The same GEP feeds the stores:

```c
*p = *baseParams;      // the init function WRITES through this address
```

Substituting `slotNo → 0` in the IR would send all twenty TTIs' stores into element
`unit*5 + 0`, and elements 1–4 would never be updated again. Silent memory corruption, not
a missed optimization.

So the witness authorises **replacing a `may_const` load's result** and nothing else. The
GEP, the stores, and every other use keep the live parameter. A second reason to hold this
line: if the GEP were rewritten and PASS6 then failed to fold the load — unregistered
global, unsupported type — the result would be a runtime load from the wrong element.
Failure must degrade to *no optimization*, never to *wrong address*.

Corollary: the witness picks **which memory to trust**. Witness 0 always reads slot 0's
copy. If the contract holds, all five agree and the choice is invisible; if it does not,
the wrong value is baked in silently. See §7.

### Rejecting an invalid witness

A witness-derived offset is not in bounds by construction the way a source-computed one
is, so `tryReplaceDirectGEP` requires the whole access to fit inside the root global's
declared object before trusting the address. A negative index arrives as a large unsigned
offset and fails the same comparison. `tryReplaceIndirect` takes no assumption at all: its
base is a pointer read out of a global, whose pointee extent is unknown, so there is
nothing to bound against and declining is the only safe answer.

This catches the reachable cases, not all of them: a conditionally executed load, or one
whose object size is not visible, still rests on requirement (2) of §2 being true. The
guard exists so that a wrong annotation degrades to "not specialized" wherever the
compiler can tell, rather than to a read of unrelated memory or a fault during
specialization.

---

## 4. Not a dimension on the wire

`ejit_free_dim` is not part of the specialization identity:

| | `ejit_dim` | `ejit_free_dim` |
|---|---|---|
| in the cache key | yes | **no** |
| inline-cache axis | yes | **no** |
| `dimType` slot | one per period | **none** |
| clones per entry | one per instance | **one, total** |
| counts against the 4-dim budget | yes | **no**, and uncapped |
| invalidated by | its period | its siblings' periods |

It exists only as metadata read by the JIT optimizer. **No runtime ABI change, no wrapper
change, no `dimType` reservation, no activation gate, no version bookkeeping.**

Invalidation comes free: the clone is keyed by the surviving lifecycle dims, so
`ejit_deactivate("unit", n)` bumps a version, the lookup's snapshot compare fails, and the
clone is retired exactly as today.

---

## 5. What changes

| Layer | Change |
|---|---|
| `Attr.td` | `EjitFreeDim : InheritableParamAttr`, spelling `ejit_free_dim`, no args |
| Sema | `handleEjitFreeDimAttr` checks the parameter is an integer. `checkEjitFreeDim`, after `MergeFunctionDecl`, checks the function is `ejit_entry` and that no parameter carries both this and `ejit_dim` — post-merge so it is order-independent and sees an `ejit_entry` written on an earlier declaration. It also warns when the entry has no lifecycle dim, since nothing could then invalidate the frozen values. No `ejit_bound_ptr` conflict check: that requires a pointer and this requires an integer, so no parameter can carry both |
| CodeGen | emit `!{!"ejit_free_dim", !"", i32 argIndex}` in its own parameter-ordered loop, as `ejit_bound_ptr` already does. Ordering is free here precisely because the tag is not a wire dim — nothing packs these positionally. The index is an **LLVM IR argument** number, obtained from `getEjitIRArgIndex` (CGCall.cpp), not a source parameter number. Getting there takes two steps. First, the parameter's position within `CGFunctionInfo`'s argument list: two things move it, in opposite directions, so neither can be recovered from the total — a *prefix* of implicit arguments the source list lacks (`this`), and arguments *interleaved after* a parameter, since `appendParameterTypes` pushes a size argument immediately after each `pass_object_size` parameter and shifts every later one. The helper counts the interleaved arguments, recovers the prefix from the remainder, and walks to the parameter. Second, that position becomes an IR argument number through `ClangToLLVMArgMapping`, which separately accounts for an sret pointer, padding arguments, inalloca and aggregate expansion — sret never appears in `arg_size()`, so it shifts the IR index without affecting the walk. Between them: on x86-64 `f(int, struct{long,long}, int slot)` puts `slot` at IR argument 3 for source index 2, while `f(int dim, int slot, int live, const char *p __attribute__((pass_object_size(0))))` leaves both annotations where they are. A type cross-check on the chosen slot backstops an unmodelled insertion that lands a differently-typed argument there, but is blind to a shift among same-typed parameters, so the walk is what has to be right. A parameter that does not lower to exactly one directly-passed IR argument — a `_BitInt(256)` passed `byval`, say — is dropped with `-Wembedded-jit` rather than annotated at a guessed index: a missing dim costs an optimization, a wrong one specializes on an argument nobody marked. That is a CodeGen diagnostic rather than a Sema one because whether it happens depends on the target. `ejit_period_arr_ind` and `ejit_bound_ptr` record indices the same way and are mapped through the same helper |
| AOT passes | **none.** Every dim-enumerating loop filters by tag, so an unknown tag is skipped by construction; PASS1 clones the whole `!ejit.metadata` node into the blob |
| `EJitOptimizer` | **none functionally** — `preReplacePeriodIndices` already matches `TAG_EJIT_PERIOD_ARR_IND` only, so the tag is skipped by construction. A comment records that this must stay true: adding the tag there would redirect the stores |
| `EJitStructFieldPass` | `initFreeDimAssumptions` builds an `AssumedArgMap` from the metadata; `computeGEPOffset` / `accumulateFullOffset` / `accumulateArgumentOffset` take it and fall back to a bounded evaluator when an index is non-constant. Bound-pointer *propagation* deliberately passes an empty map — it proves an argument relationship that has to hold for every call, not the value at one address |
| Runtime | **none** |

### The evaluator

Bounded recursive constant-fold over the index expression with `AssumedArgs` supplying the
argument's value. Leaves: `ConstantInt`, mapped `Argument`. Opcodes:
`add sub mul shl lshr ashr and or xor urem udiv srem sdiv zext sext trunc select icmp`.
Depth-capped, no memory reads, bail on anything else.

**Poison-generating flags are honoured.** `nuw`, `nsw`, `exact`, `disjoint`, `nneg` and
`icmp samesign` are preconditions InstCombine attached using facts that hold for the
values the program actually passes. The witness is a value it may never pass, so a flag the witness breaks
means the expression is poison there and the address derived from it is fiction:
`sub nuw i32 %slot, 1` at witness 0 would otherwise yield `0xFFFFFFFF`, and a later mask
can launder that into a plausible in-bounds index that no bounds check would catch;
`icmp samesign slt i32 %slot, -1` is poison at witness 0 and the select it feeds produces
an index that is in bounds by construction. Evaluation is abandoned instead.

Substitution rather than subexpression deletion, deliberately: by the time PASS6 runs
IPSCCP has folded `unitIdx`, so the expression is `add(15, urem(slotNo, 5))` or whatever
InstCombine rewrote it into. Folding is immune to that; pattern-matching a term is not.
Deletion is also undefined for shapes like `(slotNo + unitIdx) * 5`, where substitution is not.

---

## 6. What it buys

Against specializing on the slot axis instead:

- **One clone, not twenty.** Warm-up drops by the same factor — the in-flight dedup table
  is keyed by `funcIndex` only, so clones are gained one per compile-duration and other
  requests are dropped rather than queued.
- **No `maxCodeMemory` multiplication** (2 MB default).
- **The `D = 16` cliff never applies.** `EJIT_ICACHE_DIM_SIZE` is 16; with 20 slot values,
  a slot axis would leave slots 16–19 with no inline-cache cell at all, calling into the
  runtime every TTI. With no slot axis there is nothing to overflow.
- **No orphaned specializations.** A dimension with no lifecycle has no invalidation path;
  a free dim adds no axis, so nothing is orphaned.

---

## 7. Verification — already covered, by construction

Nothing in the compiler or the runtime can check the contract. It must be measurable —
and `EJIT_VERIFY_SUBSTITUTION` already measures it, with no code change.

That mode keeps the `may_const` load and emits `__ejit_verify_check` comparing what it
loads against the value substitution would have frozen. The retained load still indexes
with the **live** parameter; the frozen value came from the **witness** address. So every
execution compares "the field at the slot this call actually used" against "the field at
slot 0", and a field that is not uniform along that axis reports a mismatch naming the
function and offset. `EJitVerify.h` documents this.

Run the workload under this build before trusting any `ejit_free_dim` annotation. It is
the only evidence the annotation is correct.

---

## 8. What it does not fix

Freeing the index makes blocked fields **reachable**; it says nothing about whether they
are **stable in time**.

- Fields written from live load metrics stay unsafe to freeze, and are unaffected by this
  attribute in either direction.
- `may_const` freezes a whole field. A bitmap mixing a slot-invariant bit with a
  load-driven bit still cannot be marked at all.
- Annotate the **raw** parameter, never a pre-reduced one: `slotNo % 5` would merge slots
  that share a buffer element but sit at different points in the TDD pattern.

---

## 9. Tests

What this branch actually adds:

| File | Coverage |
|---|---|
| `clang/test/Sema/ejit_free_dim.c` | non-integer and floating parameters; non-`ejit_entry`; conflict with `ejit_dim` in either written order; no-lifecycle warning; four dims plus a free dim (no budget interaction); a wide parameter is accepted; `ejit_entry` inherited from an earlier declaration |
| `clang/test/CodeGen/ejit_free_dim.c` | metadata node shape; several free dims on one function; and, under an x86-64 triple, both argument-mapping shapes — an aggregate expanded into two IR arguments (which shifts a later parameter) and a `pass_object_size` pointer (whose hidden argument must not shift earlier ones) |
| `EJitRuntimeTest` (`FreeDim*`, 12) | folds `15 + slot%5` at the witness; leaves the store's index non-constant; does not fold without the annotation; folds `select`/`icmp`; bails on an index rooted at a load; rejects a witness before and past the object; rejects `sub nuw` and `icmp samesign` poison, while still folding each same shape without the flag; `preReplacePeriodIndices` does not substitute the argument |

Deliberately **not** covered here, and why:

- **No `Transforms/EmbeddedJIT` test.** There is nothing to assert: the AOT passes take no
  code change, and "an unknown tag is skipped" is a property of a `!=` on a tag string
  that every existing wrapper test already exercises.
- **No `ejit_test/` end-to-end test.** That suite does not run on this host — every binary
  in it faults at startup, and even when it links, a specialization module that still
  references a period global fails its `Page21` relocation because the JIT slab sits
  outside ADRP's ±4GB reach under emulation. `~/testdir/ejit_free_dim_demo.c` covers the
  same ground by inspection instead: it dumps `_pre.ll`/`_opt.ll` for both modes, checks
  at run time that all five slots write their own element, and shows what a violated
  contract looks like.
- **No verify-mismatch test.** §7 holds by construction rather than by new code, so there
  is no new path to regress; a test would be exercising `EJIT_VERIFY_SUBSTITUTION`, which
  is a build-flag-gated mode with its own coverage.
