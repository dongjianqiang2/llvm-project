# EJIT Switch-Case Mode — one compiled function per `funcIndex`

**Status**: design, not implemented
**Related**: `EJIT_ICACHE_MULTIVERSION.md`, `EJIT_ICACHE_SHARED_TABLE.md`,
`EJIT_FREE_DIM.md`, `PASS3_EJitWrapperGen.md`, `PASS4_EJitPeriodHandler.md`,
`PASS6_EJitStructFieldPass.md`

---

## 1. Problem

Today one `ejit_entry` produces **one compiled function per `(funcIndex, dims)`
identity**. Two calls to the same entry with different `ejit_dim` values are two
compiles, two code-pool allocations, and two fully duplicated bodies — including
the part of the body that does not depend on the folded period constants at all.
The inline cache selects between them before the call, by indexing a
`[D]^numDims` cell table (`EJIT_ICACHE_MULTIVERSION.md` §3).

Each identity pays its own bitcode parse, `createJITDylib`, symbol interning and
JITLink invocation, plus a code-pool allocation — and in 4K-seal mode every
allocation consumes a whole page (`EJitCodePool.cpp:243`), so 4 KiB per identity
beyond the first. Merging collapses all of it to one.

**Switch-case mode** compiles **one function per `funcIndex`**, holding one
specialized *arm* per identity behind an in-function dispatch, with the
always-correct AOT body as the fallback (§3.2):

```c
/* conceptual; the emitted dispatch is an indexed branch, not a C switch (§3.2) */
int entry(uint32_t cell, ...) {
  switch (cell) {
    case 3:  /* arm specialized for cell 3 */
    case 7:  /* arm specialized for cell 7 */
    default: return entry_aot(cell, ...);    /* AOT body, always correct */
  }
}
```

The motivating entries are 2-dimensional: the board marks cases as
`(cellIdx, slotNo)` per TTI, `slotNo` changes several times within a TTI, and the
goal is to stop emitting one compiled function per pair.

### 1.1 Scope: flag-gated, per function, never the default

Switch-case mode is gated twice: a CMake option `EJIT_SWITCH_CASE` builds the
runtime support (§1.2), and the AOT flag `-ejit-switch-case` selects the mode per
compilation. The two shapes **coexist** in one image: icache slot registration
is already per function and already carries a `numDims` argument
(`EJitRuntime.cpp:609`). The one addition the runtime needs is a
**per-`funcIndex` mode bit** recorded at registration, so the fill and lookup
paths know which shape an entry uses (§5.4).

### 1.2 Default-path invariant

An AOT flag cannot remove code from an already-built runtime library, which is
why §1.1's CMake option exists and why the invariant has two levels.

**`EJIT_SWITCH_CASE=OFF`** — every runtime addition (the mode-bit checks in
`icacheFill` and `publish`, the registry walk in `setInstanceEnabled`, the
arm-presence check in `tryCacheHit`) is compiled out. `ejit.o` is then
**byte-identical** to today's, and `-ejit-switch-case` is rejected at the AOT
side rather than silently producing code no runtime can serve.

**`EJIT_SWITCH_CASE=ON`** — the runtime grows those checks, so byte-identity is
gone by construction. What must hold instead:

* non-switch-case AOT output is unchanged, instruction for instruction;
* the shared-state ABI is unchanged — same blob size, same field offsets;
* non-switch-case behaviour is unchanged;
* the added cost stays inside a stated budget: **one predicated branch** on each
  path named above, and no added allocation, lock or loop on any path a
  non-switch-case entry executes — `setInstanceEnabled`'s registry walk is an
  empty-list test unless switch-case entries were registered.

Two choices are forced either way:

| Touchpoint | Forced |
|---|---|
| Switch-case state (identity set, versions, arm table) | **Must not** be added to `EJitSharedTaskPoolState`. That blob is a zero-init `.bss` global with a 512 KiB budget and asserted standard layout (`EJitSharedTaskPoolState.h:696-761`); adding a field shifts offsets and grows the blob for every build. State goes in per-function AOT-emitted globals instead (§5.1). |
| `MachineOutliner` | `TargetOptions` is process-global, so enabling it would put the pass into every per-identity compile too. Must be neutralized per function (§4). |

Stage 0 (§8) gates both levels: byte-identical `ejit.o` at OFF, and the
unchanged-output/ABI/behaviour set plus the budget at ON.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **identity** | `(funcIndex, dims[])` — what the cache is keyed on today |
| **arm** | one specialized body inside the merged function, serving one identity |
| **AOT arm** | `<name>_aot`, the unconditional AOT body; terminates, never resolves (§3.2) |
| **request thunk** | `<name>_request`, enqueues a compile for a missing arm then tail-calls the AOT arm (§3.2) |
| **arm table** | per-function table of arm addresses, indexed by the linearized identity; a non-arm entry holds the AOT arm or the request thunk (§3.2) |
| **identity set** | the identities a given merged function was compiled for |
| **selected dims** | the dimensions the arm set is a product over; the rest stay generic (§6.4) |
| **`d`** | `numDims` of an entry, 0–4 (`kEJitSharedMaxDims`, `EJitSharedTaskPoolState.h:107`) |

---

## 3. Shape

### 3.1 Specialize first, merge second

Cloning the body N times into one function and *then* running the optimization
pipeline would defeat the purpose of the system: every size-thresholded pass in
`buildFunctionSimplificationPipeline` (the inliner, `LoopFullUnrollPass`,
SimplifyCFG's speculation budget) would see a function N times larger and back
off. The order is therefore fixed:

1. **Per arm, in its own module.** For each identity, clone the *whole
   specialization module* and run `EJitOptimizer::runPipeline` on it exactly as
   today — same phases 1a–1f, same `simplifyO1/2/3_`, no pipeline change at all.
2. **Then merge** the batch's optimized modules into one, renaming as §3.3
   describes, and link it as a single allocation.
3. **Then recover sharing** within that batch (§4).

**The dispatcher is stable; arms are replaced in batches.** The dispatcher,
`<name>_aot`, `<name>_request` and the arm table are emitted once per entry and
never rebuilt. An update recompiles only the arms it invalidated (§5.5 fan-out)
as a **new batch in a new allocation**, and repoints their table entries.
Rebuilding the whole merged module instead would redo `O(N)` work for an
`O(N/r_i)` invalidation (§6.5), and is why §5.3's protocol is per-arm.

Two consequences follow, and both are load-bearing:

* **Outlining is scoped to a batch** (§4). Arms compiled in different batches
  cannot share with each other, so duplication between batches persists and the
  footprint measurement must use the real update model, not a single first
  compile.
* **An entry accumulates allocations over time**, one per batch — which is
  exactly what §6.5's residency budget has to charge.

In exchange the dispatcher's address never changes, so its icache cell is filled
once and never rewritten: `EJIT_ICACHE_MULTIVERSION.md` §2's frozen-cell
precondition is *restored* rather than worked around.

**Cloning the entry function alone would be wrong**, and not merely suboptimal.
Phases 1a–1f are module-wide: `runStructFieldPass` substitutes `may_const` loads
in every function, and `runInterproceduralPropagation` internalizes every
non-entry definition and runs IPSCCP across the module
(`EJitOptimizer.cpp:900-922`). Helpers the AOT inliner chose not to inline are
therefore specialized too. Share one helper between arms and the first arm's
constants are baked into it for all of them — mixed specialization state, not
just lost optimization. Per-module cloning gives each arm its own copy of every
helper and private global by construction, and needs no pass to be made
arm-aware.

Arms stay separate `internal` functions rather than being spliced into one body.
`musttail` makes each dispatch edge a single `b` on AArch64, and co-residence in
one module after the merge is the precondition §4 needs.

### 3.2 The dispatch and the arm table

The dispatch is emitted explicitly, not as an LLVM `switch` left to the backend,
so its shape is fixed by construction rather than by whatever SimplifyCFG and the
switch-lowering heuristics decide this release:

```
  for i in 0..d-1:
      if dim_i >= r_i: goto default        // mandatory, see (4)
      idx = <per-d index computation>      // §6
  musttail br armTable[idx]                // arm, <name>_aot, or <name>_request
```

Four properties are fixed for every dimensionality; only the index computation
and the table shape vary (§6).

1. **The arm table** is a per-function table of arm addresses in `.mc_shared`,
   written by the runtime at publish. A non-arm entry holds one of exactly two
   addresses, and the distinction is load-bearing (see 2).
2. **Totality, and it must not route through the resolver.** `<name>_miss` is
   **not** an AOT body: `emitSlowPath` makes it funcIndex guard →
   `ejit_taskpool_compile_or_get` → dispatch to the resolved pointer, reaching
   the spliced AOT body only when resolution fails
   (`EJitWrapperGen.cpp:1141-1191`). In switch-case mode that resolution returns
   **the merged function itself**, so defaulting to `<name>_miss` would cycle
   dispatcher → miss → dispatcher indefinitely. PASS3 therefore emits the AOT
   body as its own symbol, and the two non-arm entry values separate termination
   from progress:

   | Entry value | Meaning | Behaviour |
   |---|---|---|
   | `<name>_aot` | deactivated arm, or out-of-range dim | run the AOT body and return; **no** taskpool call, guaranteed termination |
   | `<name>_request` | identity has no compiled arm yet | enqueue once through the existing dedup, then tail-call `<name>_aot` |

   `<name>_request` is the only path that may enqueue and it never dispatches to
   a resolved pointer. `<name>_miss` keeps its present meaning for
   non-switch-case entries and is unreachable from a merged dispatcher. The
   taskpool must correspondingly **not hand a merged function to a caller whose
   arm is absent**: `tryCacheHit` gains an arm-presence check, so a miss stays a
   miss.
3. **Deactivation is a store** into the arm table (§5.5), never a hot-path test.
4. **The bounds check is mandatory**, per dimension. Dim arguments are
   `uint32_t`; the `[0, D)` bound is a *product contract* the icache enforces by
   declining out-of-range fills (`icacheDimsInRange`,
   `EJitSharedTaskPool.cpp:276`), not an ABI guarantee. Without it an
   out-of-range value indexes past the table (a wild branch) or aliases another
   identity (a silent miscompile).

**Pin the emitted form in a lit test**: it must not degenerate into a jump table
plus a redundant compare chain, and the bounds check must not be hoisted above
the load (§7.4).

### 3.3 Where it goes

A new step in `EJitOptimizer`, **after** N independent `runPipeline` calls, each
on its own cloned module (§3.1). `preReplacePeriodIndices`
(`EJitOptimizer.cpp:846`) is unchanged — it keeps its whole-module RAUW, because
in switch-case mode the module *is* one arm.

**PASS6 does change**, and it is the one pipeline change the mode requires.
§5.3 forbids reading live memory, so `createConstantFromMemory` must resolve
against the arm's snapshot instead of the address it is handed today. Its six
call sites span distinct roots — period-array base, registered static var,
registered absolute `inttoptr`, bound-pointer object, and a pointer slot that is
itself loaded and then dereferenced (`EJitStructFieldPass.cpp:1104`) — and the
snapshot must cover every root a given arm reaches, including **both the slot and
its pointee** for the indirect case.

> **Uncovered means unspecialized.** A load whose source the snapshot does not
> cover is left alone, exactly as an unresolvable `may_const` load is today. It
> must never fall back to reading live memory, which would reintroduce the race
> the snapshot exists to remove. The existing per-site diagnostics already have a
> category for declining a load; this is one more reason.

The merge links the N modules into one and must make every internal name unique,
since each module carries its own specialized copy of the same helpers and
private globals:

* each arm's entry becomes `internal` as `<name>_arm_<i>`, its
  `ejit_period_arr_ind` argument now dead but retained — `musttail` requires an
  exact signature match;
* every other internal definition is suffixed per arm; nothing is merged by name,
  because same-named helpers from two arms hold *different* constants;
* the dispatcher, `<name>_aot` and `<name>_request` (§3.2) are emitted fresh and
  carry the external names.

Recovering whatever the arms genuinely share is §4's job, at the machine level,
where identity can be checked rather than assumed. **Test a non-inlined helper
whose result differs between identities** — that is the case per-module cloning
exists to get right.

`SpecializationContext` grows a `dimensions` vector per arm. `optLevel` and
`tier` are genuinely shared; **`boundPointers` is not**.
`EJitBoundPointerView::rawPtr` is the live pointer supplied by *one* identity's
invocation (`EJitBoundPtr.h:39-46`), and `periodInstance` is compile metadata
that does not redirect it — so building arm 7 from the request that carried cell
3's descriptor would read cell 3's object and freeze its values into arm 7. The
descriptor is also only valid for the compile callback that borrowed it, and a
merged compile outlives any single invocation. **v1 excludes entries with bound
pointers** via the §6.5 admission check; admitting them needs per-identity
descriptors with explicit lifetime guarantees, which is a separate design.

---

## 4. Recovering the shared code

§3 delivers one compile, one module, one pool allocation and a scalar cache cell,
but each arm is still a full body — the shared code is duplicated N times in the
emitted object.

> **This layer carries the feature's value, and is not optional.** The motivation
> for switch-case is precisely *not to hold N specialized versions of one
> function*, and §3 alone does not deliver that: N arms in one function is the
> same total body count as today, plus a dispatch. If this step shares little,
> switch-case is a footprint loss for that entry and should not be enabled on it.
> Measure it in stage 1, under the real update model (§3.1) rather than on a
> single first compile — cross-batch duplication is part of the answer.

Sharing is recovered at the **machine level**, by the `MachineOutliner`
(`llvm/lib/CodeGen/MachineOutliner.cpp`), enabled through `EnableMachineOutliner`
on the `TargetOptions` of the JIT's `JITTargetMachineBuilder`
(`EJitOrcEngine.cpp:718-748`). AArch64 is its best-supported target. It compares
actual emitted instructions, so "identical" needs no heuristic about IR
equivalence after independent optimization, and it has no correctness surface
here — outlining nothing still leaves switch-case correct.

It runs over a whole module, which is why it cannot help today: per-identity
compiles are in separate modules with nothing to match against. §3.1's batch
model only partly lifts that — arms in one batch can share, arms in *different*
batches cannot — so duplication between batches persists and grows with the
update count.

**The pass is absent from the shipping build.** Its insertion in
`TargetPassConfig::addMachinePasses` sits inside
`#ifndef EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL` (`TargetPassConfig.cpp:1246-1257`),
and `TargetPassConfig.cpp:14-15` makes plain `EJIT_TRIM_LLVM_BACKEND` *imply*
that guard — which `build_aarch64_stable.sh:15` sets and the configured cache
confirms. Setting `EnableMachineOutliner` there does nothing at all.

So stage 1 must restore outliner support under the trim guard and measure the
runtime-library footprint that costs against the generated-code footprint it
saves. Those are opposite-signed terms in one budget and the feature's value is
the net; a negative net is the same stop signal §9.1 gives from the sharing side.
Per-region cost is a `bl`/`ret` pair, so gate the whole thing on
`EJIT_SWITCH_CASE_OUTLINE`.

**Neutralizing it on the default path.** `TargetOptions` is process-global, so the
outliner cannot be scoped to one compile from there. Mark every function the
merge step did **not** produce with the `nooutline` attribute, which the outliner
honours (`MachineOutliner.cpp:1211`). Two `TargetMachine` instances would work
too, but that is more state for the same effect.

IR-level region merging (`CodeExtractor` tail merging across independently
optimized clones) is **out of scope for v1**: large, real miscompile risk, and
the outliner answers the same question with a `TargetOptions` field.

---

## 5. Runtime changes

### 5.1 Per-function state

`EJitCompileRequest` (`EJitSreQueue.h:55`) carries `dims[4]`/`versions[4]` for
exactly **one** identity and is pinned by
`static_assert(sizeof(EJitCompileRequest) == 208)`. It cannot carry N identities,
and growing it would break the queue ABI on every core.

So **the request stays one identity wide**: it names the `funcIndex` that needs
recompiling and a snapshot sequence number, and the worker reads the current
identity set out of an out-of-line structure. The request fields keep their
present meaning — the identity that triggered the recompile — which keeps the
dedup and PGO admission token paths unchanged.

That structure holds, per switch-case `funcIndex`: the identity set
(`N × dims[]`), the per-arm `armGen` counters and the arm lock (§5.3), the arm
table (§3.2), and the declared ranges the dispatch linearizes against — the
single source of truth tying a merged function to the state that invalidates it.

Per §1.2 it does not live in `EJitSharedTaskPoolState`. PASS3 emits a per-function
global `@__ejit_sc_state_<name>` in `.mc_shared`, exactly as it already emits
`@__ejit_icache_fn_<name>`, and registers it by name alongside the icache slot.
The storage then exists only in images that contain switch-case entries, is sized
to the arms that entry declares, and costs nothing when the flag is off. The arm
table is a field of this global, so the emitted code addresses it with the same
PC-relative form the icache probe already uses.

### 5.2 Nothing is replaced; only arms are repointed

Because the dispatcher is stable (§3.1), no cached pointer ever goes stale: the
icache cell and the taskpool slot hold one address for the entry's lifetime, and
an update changes only arm-table *contents*. So there is no replacement problem
to solve — no per-call validation, no redirection manager, no cell rewrite.

What remains is staleness *within* the table: totality (§3.2) covers an
**absent** arm, not a **present** arm holding overwritten constants. That is
§5.3.

### 5.3 Arm eligibility, synchronization and publication

`EJitSharedCacheSlot` (`EJitSharedTaskPoolState.h:267`) snapshots `versions[4]`
at publish, but a merged function spans N identities and there are only four
fields. That path is left untouched; the merged path needs its own protocol, and
a counter compared before and after a compile cannot be it — it is blind to an
instance that was *already* disabled when the request was made, and a
pointer-valued CAS over such a window admits ABA (`request → aot → request`
returns the address to its original value over a full update cycle).

#### The arm lock

No existing lock spans a merged function. `setInstanceEnabled` — the entire
invalidation path — takes none at all: it CASes `enabled[][]`, bumps the epoch,
drains and bumps `version[][]` (`EJitSharedTaskPool.cpp:909-936`). And the one
lock publication does take, `cachePublish`'s bucket write lock
(`EJitSharedTaskPool.cpp:3038-3040`), guards cache *slots* — not the arm table,
not `armGen`, and not the `may_const` bytes. Even with the cache identity reduced
to `funcIndex` (§5.4), so that a merged function's calls all land in one bucket,
that lock is the wrong object and the deactivation side never acquires it.

Each switch-case entry therefore owns an `EJitRwLock` in
`@__ejit_sc_state_<name>` — the **arm lock** — the single synchronization domain
for that entry's arm table, `armGen` values and `may_const` snapshots. Publication
takes it, as do per-instance activate/deactivate and bulk
`setAllInstancesEnabled` (once per affected entry for the whole operation, not
once per instance).

**Ordering**: the arm lock precedes the cache bucket locks. A thread may take a
bucket lock while holding an arm lock, never the reverse — publication validates
under the arm lock, then writes its slot under the bucket lock. Deactivation
takes only the arm lock, and a registry that is empty unless switch-case entries
exist keeps all of this off the default path (§1.2).

#### Protocol: snapshot, then specialize

`EJitStructFieldPass` reads live memory with plain `std::memcpy`
(`EJitStructFieldPass.cpp:1030-1050`). Racing that against a handler's writes is
a data race whether or not the result is later discarded — a *discarded*
compilation still has to execute safely. Detecting a generation change afterwards
is a freshness check, not a substitute for exclusion.

So the arm's inputs are **snapshotted under the lock and specialized from the
snapshot**, never read live:

1. **Under the arm read guard**, in one critical section: check
   `isInstanceEnabled` for every dim, read `armGen`, and copy the arm's
   `may_const` source bytes into a private buffer. An ineligible identity is
   skipped and its entry published as `<name>_aot`. Deactivation takes the
   corresponding write guard **before returning to the handler**, so no
   modification can overlap this section.
2. **Specialize from the snapshot**, lock-free. PASS6 resolves every `may_const`
   load against the buffer, so the long optimization pipeline holds no lock and a
   concurrent handler is never excluded by it — which matters, because PASS4 puts
   deactivation on every handler entry.
3. **Validate and install under the write guard.** Re-check eligibility and
   `armGen` per arm and install only the entries that still match, so a snapshot
   the update cycle has since invalidated is dropped rather than published.
   `armGen` is monotonic — bumped on deactivate *and* reactivate — so no value
   ever returns and ABA cannot arise. Check and store are not separable.

Snapshotting rather than holding the read guard across the compile is what keeps
lock hold time proportional to the data read instead of to the optimization
pipeline.

**Tests.** The interleavings that motivated this protocol are now impossible
inside it, so testing must assert the exclusion rather than reproduce the race:
a deactivation attempt **blocks** while publication holds the write guard; a
handler modification cannot overlap a snapshot in progress; and the orderings
that remain legal behave — an arm **disabled before the compile starts** is never
specialized, and an update cycle completing **before** publication takes the lock
causes its arms to be dropped, not installed.

### 5.4 Inline cache

**The cell's job changes, which is why it collapses.** The cell table is a
lookup whose key is `(funcIndex, dims)` and whose value is a *specialization*,
because the dims are what select which compiled function to call and that
selection happens before the call. Switch-case moves the selection inside the
function, so the key becomes `funcIndex` and the value a *merged function*. One
key per entry, therefore one cell.

Three separate things are called `numDims`, and **the entry's own dimensionality
is not one of the two that change**:

| | Switch-case | Why |
|---|---|---|
| The entry's `ejit_dim` parameter count | **unchanged**, 1–4 | the function still takes those arguments and its identity still has those dims; the in-function dispatch is simply what consumes them now |
| The icache table shape | **0** | `getOrCreateIcacheFnGlobal` builds `[D]^NumDims`, and a scalar `ptr` at 0 (`EJitWrapperGen.cpp:335-353`). PASS3 passes 0 in place of `getPeriodArrIndInfo(F).size()`, and gives `ejit_register_icache_slot` the same 0, so `icacheFill` linearizes nothing and `icacheDrainAll` walks one cell |
| The taskpool cache identity (`EJitSharedCacheSlot::numDims`/`dims[]`) | **0 / empty** | one merged function per `funcIndex` is one slot; keying on dims would scatter one function's identities across `hashIdentity` buckets |

One consequence of the collapse: `EJIT_ICACHE_DIM_SIZE` stops bounding dim
values, and §3.2's bounds check takes over that job against the declared ranges.

**The dims must still reach `tryCacheHit`, even though the key ignores them.**
Its per-dim `isInstanceEnabled` loop iterates `numDims`, so passing 0 there would
silently delete the enable check and re-open §5.5's hazard at the taskpool as
well as the cache. Keying and checking therefore separate: the *key* is
`funcIndex` alone, while the call's real dims are still passed for the enable
reject and the arm-presence check (§3.2). For the same reason the slot's
`versions[4]` gate is inert for a switch-case entry — there are no dims in its
identity to version — which is *why* §5.3's `armGen` protocol is required rather
than merely additional.

At `D = 16` the collapse takes a 2-dim entry from 2 KiB of `.mc_shared` to 8
bytes. `icacheDrainAll` is unchanged and gets simpler: one cell.

**What the per-identity table was doing implicitly.** It carried two guarantees
nobody had to state, and collapsing it loses both:

1. *Cells were partitioned by identity*, and cores drive disjoint instance
   indices, so a core only ever read a cell it had filled itself — after
   resolving through the taskpool, where it did whatever per-core execute
   preparation the platform needs. A shared scalar has no such partition: core A
   can fill it, and core B branch through it on its first call.
2. *A deactivated identity's cell stayed empty*, which is what made deactivation
   safe without any check. That is §5.5, and the arm table replaces it.

Guarantee 1 is not something the arm table can restore — it is about code-page
permissions, not about which pointer is correct — so it stays a platform gate:
`icacheFill` consults `icacheCrossCoreExecutable()` for `numDims == 0`
(`EJitSharedTaskPool.cpp:771`).

**That gate holds on the shipping configuration.** It reduces to
`prepareCodeFn_ == nullptr`, plus a shared `icachePerCorePrepare` override
(`EJitSharedTaskPool.h:579-604`). 4K-seal mode is deliberately *not* counted as
per-core preparation — the seal acts on an address space every core translates
through, so a page sealed by one core is executable on all of them — and this is
verified on the board by `ejit_icache_multiverify_test`'s 0-dim entry executing
on cores that never resolved it. It closes only under the legacy whole-2MiB
`prepareCodeFn_` path, or for a facade never handed the seal mode.

**Where it does not hold, v1 declines the mode outright** — the admission check
(§6.5) rejects the entry and it stays on the per-identity path. Falling back to
"the taskpool serves every call" is *not* sufficient here, and the reason is
§3.1's batch model: an update places replacement arms in a **new allocation**
while the same dispatcher stays reachable through the shared arm table. Having
prepared the dispatcher's allocation says nothing about those later arms or
their outlined helpers, so a peer core can branch through a correctly-prepared
dispatcher into an arm allocation it has never sealed. That also contradicts
§5.5's requirement that every published arm be executable on every consuming
core before its entry is repointed. Admitting such a platform would need
preparation and invalidation specified for *every reachable allocation*, per
batch — a design v1 does not have.

**The exposure switch-case adds** is not the gate but the assumption under it.
`EJitSharedPoolSplit` tracks `splitDoneMask` per core, which is consistent with
per-core bookkeeping over a shared mapping — and also with the mapping not being
shared. The source flags this as the thing to re-check if a 0-dim entry ever
faults. Today only genuinely 0-dim entries rest on it; switch-case makes **every**
entry 0-dim for cache purposes, so it broadens that exposure from a small subset
to the whole board.

### 5.5 Deactivation

**Today**, `ejit_deactivate` bumps `version[dimType][instanceId]` and calls
`icacheDrainAll("period-toggle")` (`EJitSharedTaskPool.cpp:924-936`). This is not
a rare administrative event: PASS4 wraps every period-handler body in
deactivate/activate (`EJitPeriodHandler.cpp:157-186`), so deactivate → modify →
reactivate **is** the normal data-update cycle.

**What breaks.** Per §5.4 guarantee 2, while instance 3 is disabled `cell[3]`
stays empty and a call with `cell = 3` misses to the taskpool, which rejects it
per dim; `cell[7]` is a different cell and is unaffected.

Collapse that table to one scalar cell (§5.4) and the enforcement disappears. A
call with `cell = 7` fills the single cell, and the next call with `cell = 3`
**hits it on the fast path** — bypassing the taskpool and its
`isInstanceEnabled` check — and dispatches into arm 3, whose constants the
handler is at that moment rewriting. A miscompile, reachable on every
deactivation window.

**Why it is fixable cheaply.** The arm that executes is chosen by the call's own
arguments, not by which arm is stale, so the requirement is narrow: *a call with
`cell = c` must not enter arm `c` while instance `c` is deactivated.* The arm
table already expresses that — storing `<name>_aot` into entry `c` routes that
identity to the AOT body through the load the fast path already performs. No
mask, no extra test, nothing added to the hot path.

* **Deactivate** stores `<name>_aot` — not the request thunk — into the entry of
  every arm whose identity contains `(dimType, instanceId)`, resolved through the
  §5.1 identity set. A deactivated identity must neither enqueue on every call
  nor re-enter the resolver while its data is being rewritten. No drain is
  needed, and identities not mentioning the instance keep the full fast path, so
  granularity is *better* than today's global `icacheDrainAll`.
* **Reactivate does not restore the arm** — its constants are now stale. It
  repoints the entry from `<name>_aot` to `<name>_request`, so the next call
  enqueues exactly one recompile and runs the AOT body meanwhile. Both
  transitions bump `armGen`, which is what makes §5.3's publication check
  ABA-free.
* **Atomicity and ordering.** Natural alignment is not sufficient on its own —
  the arm lock (§5.3) serializes *writers*, and dispatchers read lock-free. The
  arm table's contract, matching the icache probe's for the same reason
  (`EJitWrapperGen.cpp:1247-1256`):
  * the dispatcher's load is an **atomic monotonic** pointer-width load, which
    makes a concurrent writer a defined race rather than UB and lowers to a plain
    `LDR`; nothing stronger is needed because the indirect branch carries an
    address dependency on the loaded value;
  * every writer — publication, deactivate, reactivate — uses an **atomic release
    store**;
  * an entry may be repointed at an arm **only after that arm's code is finalized
    and executable on every core that can reach the entry** — which, because each
    batch is its own allocation, is what §5.4 requires the platform to guarantee
    rather than the mode to arrange. `<name>_aot` and `<name>_request` are AOT
    symbols live since load, so deactivation never publishes unrunnable code.
* **In-flight calls are out of scope**, on the deployment contract the icache
  already relies on: cores drive disjoint instance indices, so the core
  deactivating instance 3 is not concurrently executing an entry with `cell = 3`.

**Fan-out.** At `d` = 1 a deactivation repoints one entry; at `d` = 2 a row or a
column, a fixed stride over the linearized index. Applying it is trivial at any
`d`. The *consequence* is that one deactivation can invalidate a large fraction of
a 2-dim entry's arms, each needing recompilation before it is specialized again —
with `slotNo` churning, a 2-dim entry could spend much of its life with most of
its table pointing at `<name>_aot`. This is the term that most deserves
measurement on the real workload.

---

## 6. Dimensionality

**This is deliberately not one generic `d`-dimensional mechanism.** A single
linearize-over-`d` implementation builds a `∏ r_i` table that is trivially small
at `d` = 1 and absurd at `d` = 4, forcing every shape to pay for the worst one.
The substrate is fixed (§3.2); each dimensionality gets the index computation and
table shape that suit it.

**v1 scope is `d` ≤ 2**, where the motivating entries live. `d` = 3–4 has a plan
(§6.4) but no code in v1.

The current integration corpus contains **no 2-dim entry and no `ejit_free_dim`
use at all** — `ejit_test` exercises 0-dim entries almost exclusively — so every
shape below is untested end to end today.

### 6.1 `d` = 0 — excluded

A 0-dim entry has one identity, so the per-identity model already produces one
function for it and switch-case would add only a dispatch. **0-dim entries do not
use the mode**; the AOT flag is a no-op on them.

### 6.2 `d` = 1

`N = r₀`, bounded by `D` = 16 at the default, so the table is at most 128 bytes
and a direct-indexed table is unambiguously right:

```
  if dim0 >= r0: goto default
  musttail br armTable[dim0]
```

At two or three arms a predictable compare chain may beat the indirect branch;
that is a lowering choice inside this emitter, decided by measurement and pinned
by a lit test.

### 6.3 `d` = 2 — the v1 target

`N = r₀·r₁`. Two table shapes, and the choice is about occupancy, not speed:

**Dense, row-major** — `idx = dim0 * r₁ + dim1`, one `madd`, the same
linearization `icacheLinearize` uses (`EJitSharedTaskPool.cpp:287`) so the two
agree by inspection. Costs `r₀·r₁` pointers: 320 bytes at 8×5, 2 KiB at the 16×16
worst case — exactly the size of the icache table it replaces, so no BSS win but
no loss either.

**Two-level** — dispatch on `dim0` into a per-`dim0` sub-table on `dim1`,
allocating sub-tables only for `dim0` values that occur. One extra dependent load,
in exchange for not materialising rows that never happen. The right shape when
`dim0` is sparse and `dim1` churns, which is the expected `(cellIdx, slotNo)`
profile.

**v1 emits dense**: simpler, worst case bounded at 2 KiB, and the occupancy data
to justify two-level does not exist yet (§9.2).

### 6.4 `d` = 3–4 — the plan, no v1 code

Dense tables are untenable here — `16³` = 4096 and `16⁴` = 65 536 pointers, before
a single arm body — and the body count binds long before the table does. The plan
is **reduction, not a third and fourth dispatch shape**:

> Select the dimensions that actually carry constants, take the product over
> **those**, and leave the rest generic — not substituted, their `may_const` loads
> emitted as ordinary loads inside every arm. A `d` = 4 entry whose constants are
> carried by two dimensions *is* a `d` = 2 entry for dispatch, and reuses §6.3
> unchanged.

A dimension earns its multiplication only to the extent that fixing its value
resolves `may_const` loads, which existing machinery can measure: substitute one
dimension at a time and count the sites that resolve, via
`EJitStructFieldPass::collectMayConstLoadSites` and the branch audit's pre/post
counting (`EJitOptimizer.cpp:204-209`). Select greedily by yield per arm until the
body budget is spent. An unselected dimension is simply a value the arm does not
know, so correctness is unaffected either way.

**The limitation**, and why this is a plan rather than a promise: if three or four
dimensions *all* carry constants, reduction drops specialization the per-identity
model delivers today. Switch-case is then a **downgrade** for that entry and it
should stay on the per-identity path (§1.1). The mode suits entries whose identity
count is large relative to the number of dimensions actually carrying constants.

### 6.5 Arm-set selection: eager over the declared range

`slotNo` varies several times per TTI and its `may_const` fields genuinely differ
per slot — so it is **not** an `ejit_free_dim`, whose contract is value-invariance
across the dim (`EJIT_FREE_DIM.md` §2), and cannot be made one. Each
`(cellIdx, slotNo)` pair is a real, distinct specialization identity.

That rules out **incremental** arm-set construction, where each newly observed
identity triggers a recompile of the whole set: with `slotNo` churning, new
identities arrive continuously, so total optimization work would be
`O(N²·body/2)` and `NO_RECLAIM` residency `O(N²/2)` — not a tail risk but the
steady state.

**Take the arm set from the declared range instead.** `PeriodArrayRegistry` holds
`PeriodArrayInfo::arraySize` for every period array, registered at `ejit_init` and
read-only thereafter (`EJitRuntimeState.h:23-28`), so the instance count per
dimension is known **before any call happens**. The arm set is the declared
product space over the selected dims, fully known at first compile: **one compile
per entry**, `O(N·body)` work, `O(N)` residency, and a new identity at runtime
already has an arm.

The arm cap then becomes a static admission check at registration — compute
`∏ r_i` over the selected dims, and if the body budget cannot hold it even at one
selected dimension, the entry stays on the per-identity path. No LRU, no eviction,
and no identity is ever left unspecialized by a policy decision made under load.

This is load-bearing, not an optimization: abandon declared-range selection and
both quadratic terms return in full.

Three costs, all to be measured rather than argued away. Arms for identities that
never occur are still emitted and resident — `O(N)`, paid once, but pure waste
where the product space is sparsely occupied, and the term §4 exists to shrink.
First-compile latency rises, since one compile now optimizes N arms.

And **"one compile per entry" describes initial population only**: §5.5 forces a
recompile after every deactivate → modify → reactivate cycle, so over `K` update
cycles the cost is `O(K·N)` work and, under `NO_RECLAIM`, `O(K·N)` retained code.
Eager selection removes the `O(N²)` term incremental growth would add; it does
not remove this one, and here `K` grows with uptime while `N` does not. Two
answers are required before the mode is enabled anywhere with frequent updates:

* **Selective rebuild** (§3.1), which turns `O(K·N)` into `O(K · N/r_i)` by
  recompiling only the arms an update invalidated.
* **A residency policy, decided before production enablement.** Otherwise
  retained code grows without bound in `K`. This is a **deployment gate**, not an
  open question, and the three candidates are not equal:

  | Policy | Verdict |
  |---|---|
  | Enable reclamation | **Unavailable.** Wiring a releaser makes `icacheFill` decline every fill, disabling the inline cache the board runs (§5.4). |
  | Replace arms in place | **Future work.** Pages are sealed RX in 4K-seal mode, so rewriting a published arm needs a re-seal cycle this design does not have. |
  | **Cap residency per entry** | **Chosen for v1**, with the accounting below. |

  **The cap must charge retained bytes, not published bytes.**
  `EJitCodePoolMemoryManager::deallocate` deliberately does not recycle sealed
  pages (`EJitCodePoolMemoryManager.cpp:384-401`), so pool space is consumed by
  every allocation the entry has ever made — including arms §5.3 **rejects at
  publication** after generating their code, the batch's helpers and outlined
  regions, and page rounding. A counter over published bytes would sit still
  while repeated update cycles, each losing its publication race, exhausted the
  pool.

  So: charge the allocation, not the publication, and **reserve budget before
  allocating** rather than accounting after. A batch that cannot be reserved is
  not compiled at all; past the cap, invalidated arms stop being rebuilt and
  their table entries stay at `<name>_aot`, which is already a valid state
  (§3.2). The cost is explicit — a long-running entry under frequent updates
  converges on AOT performance — but residency is bounded by the cap rather than
  by `K`.

  **Test repeated publication rejection**, not only successful replacement: N
  update cycles that all lose the race must leave pool usage bounded and the
  entry still serving correct code.

Measurement must therefore cover **sustained update cycles**, not first
compilation alone.

---

## 7. Soundness rules

1. **Bound-pointer entries are excluded from the mode in v1** (§3.3): one
   invocation's `rawPtr` cannot serve another identity's arm, and the descriptor
   does not outlive the compile callback that borrowed it.
   Should they later be admitted, a second rule applies on top of per-identity
   descriptors: `applyBoundPointerFacts` (`EJitOptimizer.cpp:807`) attaches
   `nonnull` / `dereferenceable(N)` / `align` **to the entry function's
   parameters**, which on a merged function become signature-level facts that
   must hold for every identity, including ones never observed —
   `dereferenceable(64)` valid for cell 3 but not cell 7 is a miscompile. Those
   facts belong on the *arm clone's* parameters only.

2. **The arm key is the bound dims, exactly.** `ejit_free_dim` parameters are not
   part of the specialization identity and must never enter the dispatch
   (`EJIT_FREE_DIM.md` §3: the witness never enters the IR). Cloning on a free dim
   would redirect stores. PASS6 keeps consuming `TAG_EJIT_FREE_DIM` unchanged,
   inside each arm.

3. **`<name>_aot` is mandatory and must never resolve.** No merged function may
   be emitted without it, and nothing reachable from it may call
   `ejit_taskpool_compile_or_get` — that is what makes dispatch terminate
   (§3.2). Enqueueing belongs solely to `<name>_request`.

4. **No arm may be entered except through the arm table**, and the per-dim bounds
   check may not be elided. The table is both the deactivation contract (§5.5) and
   the out-of-range guard; hoisting the load above the check, or caching a loaded
   target across calls, is a miscompile.

5. **Arms are `internal` and address-taken only by the dispatcher**, so
   `runInterproceduralPropagation`'s internalize-then-IPSCCP step keeps working
   per arm.

---

## 8. Staging

| Stage | Deliverable | Gate |
|---|---|---|
| 0 | `EJIT_SWITCH_CASE` CMake option + AOT flag; the regression gate itself (§1.2) | at OFF, **byte-identical `ejit.o`**; at ON, unchanged non-switch-case output, blob ABI and behaviour, within the stated branch budget |
| 1 | A **2-dim** `(cellIdx, slotNo)` integration test; restore `MachineOutliner` under the trim guard; measure sharing **across update cycles**, not one compile (§3.1, §4) | go/no-go on **net** footprint — code saved, minus cross-batch duplication, minus the runtime-library cost of restoring the pass |
| 2 | Per-arm module clone + merge with per-arm renaming (§3.1, §3.3); dispatcher, **`d` = 1 only** (§6.2); `<name>_aot` / `<name>_request` split (§3.2) | stage 0 gate still green; a **non-inlined helper whose result differs per identity** is correct in every arm; a call for an absent arm reaches the AOT body without re-entering the resolver |
| 3 | Bound-pointer exclusion in the admission check (§7.1), free-dim exclusion (§7.2) | `ejit_bound_ptr` entry is declined, not miscompiled; free-dim tests |
| 4 | Declared-range arm set + static admission check (§6.5) | one compile per entry, verified in the compile record |
| 5 | **`d` = 2, dense row-major table (§6.3)** | the stage-1 test, specialized end to end |
| 6 | `@__ejit_sc_state_<name>` + mode bit; **arm lock**, snapshot-then-specialize, `armGen`, atomic table contract (§5.1, §5.3, §5.5) | gtest asserting the exclusions hold: deactivation **blocks** against publication, no handler write overlaps a snapshot, an arm **disabled before the compile** is never specialized, and an update cycle completing before publication causes its arms to be dropped |
| 7 | **Deactivation via arm-table stores (§5.5)**; selective rebuild, reserve-before-allocate residency budget (§6.5) | deactivate → modify → reactivate test proving a call with the deactivated cell takes the AOT path *while the scalar cell is filled*; **repeated publication rejection** leaves pool usage bounded; sustained cycles bounded by the cap, not by `K` |
| 8 | Scalar icache cell; admission declines the mode where cross-core execution is unavailable (§5.4) | `ejit_icache_multiverify_test` extended to dimensioned entries registered 0-dim, exercised on cores that never resolved them |
| 9 | `EJIT_SWITCH_CASE_OUTLINE` tuning (§4) | `size -A` before/after |
| — | *Out of v1*: two-level 2-dim table (§6.3), `d` = 3–4 by reduction (§6.4) | gated on occupancy and per-dim yield data stage 5 produces |

**Stage 1 comes first on purpose**: the shape the mode exists to serve is untested
today, and the sharing measurement decides whether the rest is worth executing.
**`d` = 1 before `d` = 2 is implementation order, not a scope cut** — both are v1;
stage 2 gets the substrate and soundness rules right where the table is 128 bytes
and the fan-out is one entry, and stage 5 then changes only the index computation
and table shape.

Stages 2–4 are pure AOT/optimizer work, testable without touching the taskpool
ABI; stage 6 needs an ABI review. **Stage 7 is the correctness gate** — stage 8
must not land before it, because the scalar cell is what removes the structural
enforcement of deactivation (§5.5).

---

## 9. Open questions

1. **What is the net footprint of the sharing mechanism?** Code saved by the
   outliner, minus cross-batch duplication under the real update model, minus the
   runtime-library cost of restoring the pass to the trimmed build. A negative
   net is the stop signal §4 describes. Stage 1 answers it.
2. What is the declared range per dimension for the real `(cellIdx, slotNo)`
   entries, and how sparsely is the product actually occupied? Sets the arm count
   and the admission check (§6.5), and decides dense vs two-level (§6.3).
3. What is the per-dimension `may_const` yield on real 3- and 4-dim entries?
   Decides whether §6.4's reduction leaves them worth specializing at all.
4. What does the §6.3 dispatch cost on the target's hit path, against the current
   branchless probe? The board runs the inline cache, so this lands on the measured
   fast path.
5. Does the shared-mapping assumption under `icacheCrossCoreExecutable()` hold
   for *every* entry, not just the 0-dim subset that rests on it today (§5.4)?
   The gate holds in 4K-seal mode and v1 declines the mode where it does not;
   what switch-case changes is how much rests on it. A stage-8 gate.
6. What is `K`, the sustained period-update rate per entry (§6.5)? With the
   residency cap chosen, `K` no longer decides *whether* the mode is bounded — it
   decides **where to set the cap**, and how quickly a hot entry degrades to the
   AOT path once it is reached.

Only (1) gates the plan as a whole, and stage 1 exists to answer it.
