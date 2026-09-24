# EJIT Switch-Case Mode — runtime-keyed arms inside a per-identity specialization

**Status**: design, not implemented
**Related**: `EJIT_FREE_DIM.md`, `EJIT_ICACHE_MULTIVERSION.md`,
`EJIT_ICACHE_SHARED_TABLE.md`, `EJIT_VALUE_PROFILE.md`, `EJIT_ONLINE_PGO.md`,
`PASS3_EJitWrapperGen.md`, `PASS6_EJitStructFieldPass.md`

---

## 1. Goal and scope

### 1.1 Problem

Some entries select their `may_const` data with a value that is only known at
runtime and that EJIT cannot fold today. The running example uses a cell index
and a slot number. The mechanism is generic: it applies to any parameter carrying
the attribute (§3), whatever it is called.

```c
typedef struct {
  ejit_may_const uint32_t shift;
  ejit_may_const uint32_t scale;
  ejit_may_const uint32_t mode;
} SlotCfg;

typedef struct {
  ejit_may_const uint32_t len;
  ejit_may_const int32_t  coef[4];
  ejit_may_const int32_t  clip;
  SlotCfg slot[3];
} CellCfg;

ejit_period_arr("cell") CellCfg g_cellCfg[8];

ejit_entry
int32_t process(uint8_t  ejit_dim("cell") cellIndex,
                uint32_t                  slotNo,
                const int16_t *in) {
  const CellCfg *c = &g_cellCfg[cellIndex];

  int32_t acc = 0;                               /* part A: cell only */
  for (uint32_t i = 0; i < c->len; ++i)
    acc += in[i] * c->coef[i];
  if (acc > c->clip)
    acc = c->clip;

  const SlotCfg *s = &c->slot[slotNo % 3];       /* part B: slot */
  int32_t r = acc >> s->shift;
  if (s->mode)
    r *= s->scale;
  return r + slotNo;
}
```

`cellIndex` is a specialization dimension and folds. `slotNo` does not, so every
load through `s` is declined with `non-const-offset`
(`EJitStructFieldPass.cpp:1919`). The specialization for cell 3 folds part A and
none of part B.

None of the existing parameter attributes fits `slotNo`:

| Attribute | Why not |
|---|---|
| `ejit_dim` | No period array describes the slot, so there is no declared range; values run up to about 1024, against `kEJitSharedInstances = 256` (`EJitSharedTaskPoolState.h:109`); and the slot has no activate/deactivate lifecycle of its own. The data it selects belongs to the cell's period. |
| `ejit_free_dim` | Its contract is that the data is the same for every value (`EJIT_FREE_DIM.md` §2). Here it is not. |

### 1.2 Objective

**Fold slot-dependent data inside each cell's specialization, without a full
specialization per slot.** Each cell keeps one specialization, exactly as today.
Inside it, a small number of **arms** are keyed on the slot, plus a **default
arm**, and only the code that needs the key is cloned (§4). Cell 3 with
`len = 4`, `coef = {1, -2, 3, 1}`, `clip = 1000`, and
`slot[0..2] = {2,3,1}, {0,1,0}, {4,5,1}` becomes:

```c
int32_t process_cell3(uint8_t, uint32_t slotNo, const int16_t *in) {
  int32_t acc = in[0] - 2*in[1] + 3*in[2] + in[3];   /* part A, once */
  if (acc > 1000) acc = 1000;

  int32_t r;
  switch (slotNo % 3) {                              /* the key, §4.1 */
    case 0:  r = (acc >> 2) * 3; break;              /* arm k=0 */
    case 1:  r = acc;            break;              /* arm k=1 */
    case 2:  r = (acc >> 4) * 5; break;              /* arm k=2 */
    default: {                                       /* default arm */
      const SlotCfg *s = &g_cellCfg[3].slot[slotNo % 3];
      r = acc >> s->shift;
      if (s->mode) r *= s->scale;
    }
  }
  return r + slotNo;                                 /* slotNo stays live */
}
```

The gain is speed, bought with bounded extra code. Whether arms cost or save
memory depends on the baseline, so all three are measured (§8):

| Baseline | Arms against it |
|---|---|
| **Today**: one specialization per cell, slot generic | **More code.** Must be faster to justify it |
| One full specialization per `(cell, slot)` pair | **Much less code.** Not available today for `slotNo`, but it is what arms replace where it would be |
| One specialization per cell, with the slot's data copied into a small constant table in the JIT code, indexed by the key | **Usually more code.** The table cannot fold branches such as `if (s->mode)`; arms must beat it on speed |

**Two kinds of slot churn behave differently:**
* **selection churn**: calls pick slot 0, then 2, then 1. This only selects a
  different arm; nothing is compiled. The design is cheap here;
* **configuration churn**: a period handler rewrites a slot's data. That
  invalidates the whole cell specialization, and every arm is recompiled with it.
  The feature budget (§5.3) bounds what this adds on top of today's recompiles.

### 1.3 What does not change

The runtime dim never enters the specialization identity. So the following are
all unchanged:

* the cache identity `(funcIndex, dims[])`, `EJitCompileRequest`, dedup;
* versions, publication, the generation gate;
* **deactivation.** PASS4's deactivate → modify → reactivate cycle
  (`EJitPeriodHandler.cpp`) is today's. A specialization, arms included, is one
  function, so it is invalidated and rebuilt as a whole. The runtime dim has no
  lifecycle and is never deactivated;
* **the inline cache** (`-mllvm -ejit-inline-cache`), which the design assumes
  is on, as on the board. The `[D]^numDims` cell table stays keyed on the
  `ejit_dim`s. A hit tail-calls the specialization with the original arguments,
  so the runtime dim arrives as an ordinary argument, and `EJIT_ICACHE_DIM_SIZE`
  does not bound it. The only new interaction is on the lazy path: a promotion
  must invalidate one cell (§6.6).

The default arm is the specialization an entry gets today. Arms are an addition
on top of it, and **no state of this design runs code less specialized than
today's**.

### 1.4 Gating

A CMake option `EJIT_SWITCH_CASE` builds the support.

* **OFF**: `ejit.o` is byte-identical to today's. Clang accepts the attribute,
  warns that it is ignored, and ignores it (safe, §3).
* **ON**: entries without the attribute get unchanged AOT output, an unchanged
  shared-state ABI and unchanged behaviour.

There is no separate AOT flag. The attribute is the per-entry opt-in.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **runtime dim** | the parameter carrying `ejit_runtime_dim`; `slotNo` in the examples |
| **projection `P`** | the function of the runtime dim through which the `may_const` addresses depend on it (§4.1) |
| **key** | `P(runtime dim)`; the value an arm is specialized for |
| **`M`** | the number of distinct keys `P` can produce; unbounded for the identity projection |
| **switch point** | where the key is computed and dispatched on; code before it is shared by all arms (§4.3) |
| **arm** | a clone of the code after the switch point, with `P` replaced by one key |
| **default arm** | the original code after the switch point, with the runtime dim left generic; today's code |
| **eager path** | all useful arms of a finite domain built in the first compile (§5.2) |
| **lazy path** | arms chosen from observed keys, then added by one promotion (§6) |
| **promotion** | the recompile of an identity that adds the lazy path's arms |
| **feature budget** | the pool bytes switch-case may consume beyond today (§5.3) |
| **epoch** | the lifecycle stamp of a lazy-path record; never reused (§6.3) |
| **identity** | `(funcIndex, dims[])`, unchanged from today |

---

## 3. The attribute and its limits

The spelling is a placeholder:

```c
#define ejit_runtime_dim       __attribute__((ejit_runtime_dim))
#define ejit_runtime_dim_n(n)  __attribute__((ejit_runtime_dim(n)))

ejit_entry
int32_t process(uint8_t  ejit_dim("cell") cellIndex,
                uint32_t ejit_runtime_dim slotNo, const int16_t *in);
```

**Applies to** an integer parameter, at most 32 bits wide, of an `ejit_entry`
function. There may be at most one per function in v1. It cannot share a
parameter with `ejit_dim`, `ejit_free_dim` or `ejit_bound_ptr`. A 0-dim entry
may carry it too; its single identity then gets the arms.

**Asserts nothing.** Unlike `ejit_free_dim`, soundness does not depend on a
claim the programmer makes about the data. Every arm is guarded by its key, and
any other value runs the default arm. The attribute is an opt-in to spending code
size on this parameter.

**CodeGen** emits the tag on the function's `!ejit.metadata`, the same way
`ejit_free_dim` emits `TAG_EJIT_FREE_DIM` (`CGEJIT.cpp:121`,
`EJitCommon.h:72`). No range, modulus or period name is declared; the JIT
derives them (§4.1).

**Limits**, as CMake options passed to both PASS3 and the runtime. Arm count
alone is not a cost bound: eight tiny arms and eight cloned loops differ by
orders of magnitude.

| Option | Bounds | Default |
|---|---|---|
| `EJIT_SWITCH_CASE_MAX_ARMS` (`A_max`) | arms per specialization; overridable per entry with `ejit_runtime_dim(n)` | 8 |
| `EJIT_SWITCH_CASE_MAX_REGION` | IR instructions in one arm's region | set from the stage 4 sweep (§8.2) |
| `EJIT_SWITCH_CASE_MAX_CLONED` | IR instructions added by cloning in one compile; bounds compile work | set from the stage 4 sweep |
| `EJIT_SWITCH_CASE_MAX_POOL_BYTES` | feature budget per entry per generation (§5.3) | set from stage 1 pool data |
| `EJIT_SWITCH_CASE_MAX_IDENTITY_BYTES` | feature budget per identity per generation (§5.3) | set from stage 1 pool data |
| `EJIT_SWITCH_CASE_MAX_PROMOTIONS` | successful promotions per identity per generation (§6.4) | 2 |

---

## 4. The specialization

### 4.1 Finding the key: projection, not the parameter

**The trap.** Under `slotNo % 3`, the arm for key 2 serves `slotNo` = 2, 5, 8, ….
So an arm must **not** substitute the parameter. Substituting it would turn
`return r + slotNo` into `return r + 2` for a call with `slotNo = 5`, which is a
miscompile. Only `P(slotNo)` is replaced, and every other use of the parameter
stays live. This is why `preReplacePeriodIndices`'s whole-parameter RAUW
(`EJitOptimizer.cpp:846`) cannot be reused.

**Detection.** Consider the `may_const` sites the first PASS6 run declined with
`non-const-offset`. For each one whose address depends on the runtime dim, walk
the variable part of the address back to the parameter. `P` is the common value
on every one of those walks, and its only non-constant leaf is the parameter.
v1 recognizes the modulus, the form real entries use:

| Form | `P` | `M` | Path |
|---|---|---|---|
| `urem v, C`, with `C` constant after cell specialization | `v % C` | `C` | eager if within limits, else lazy |
| anything else | `v` (identity) | unbounded | lazy |

In the modulus form, `v` is the parameter or a `zext` of it, and the operation is
unsigned. `srem` and `sdiv`, which can produce negative keys, any `trunc` or
`sext` on the path, and other projections such as a mask, fall to the identity
row (§10).

**The projection descriptor** is `{op:8, width:8, pad:16, constant:32}`, 64 bits,
compared field by field, never by hash. It is lossless because of one
normalization: the parameter is at most 32 bits wide, so a `urem` whose constant
is `2³²` or more cannot change its value, and is recorded as the identity. `% 3`
and `% 5` therefore always differ.

The identity row is always valid, because fixing the parameter fixes everything
derived from it, and an identity-keyed arm is only entered for that exact value.
Valid is not the same as useful, though: `table[slotNo + runtimeOffset]` keeps a
dynamic offset whatever `slotNo` is. Keys are therefore selected by what they
actually fold (§4.3).

**Detection runs after the cell has been specialized**, not at AOT time. That
covers a modulus that is itself configuration: in `v % c->numSlots`, `numSlots`
is a `may_const` field, which phase 1c turns into a constant before detection
looks.

### 4.2 Supported form

v1 transforms only a deliberately narrow form, and declines everything else,
logging the reason (§7):

1. **Entry-local sites.** The key-dependent sites are in the entry function
   itself. Sites in non-inlined helpers have no common dominator with the entry
   and stay generic. If every site is in a helper, the entry gets no arms.
2. **A recognized projection** from the §4.1 table.
3. **A rematerializable address chain.** Every instruction between `P` and the
   key-dependent loads (the `urem`, the GEPs, any casts) must be speculatable and
   side-effect free. It is **rematerialized inside each region**, so that an
   address computed before the switch point does not stay on the original,
   variable `P`, which would block folding in the arms.
4. **Supported control flow.** Loops are in LoopSimplify form, with a preheader.
   The region has no `invoke`, `callbr` or `indirectbr`, and no edges other than
   ordinary branches, switches and returns.
5. **Within limits.** Each region is at most `EJIT_SWITCH_CASE_MAX_REGION`, and
   the total cloned is at most `EJIT_SWITCH_CASE_MAX_CLONED` (§3).

### 4.3 Region-level arms

Arms clone only the code that needs the key, not the whole entry. Whole-entry
clones would copy the cell-only work (part A in §1.1, the fully unrolled loop)
into every arm.

**The switch point** is the latest program point that:
1. dominates every key-dependent site; and
2. is not inside a loop. A dominator inside a loop moves to the preheader of the
   outermost loop containing it, which is ordinary loop unswitching.

Hoisting is legal because §4.2 rule 3 makes `P` and the address chain
speculatable. It can run `P`, and the lazy path's observation, on paths that
never reach a key-dependent load, including zero-trip loops. That costs cycles
and skews the lazy path's counts, but it is never incorrect.

**Cloning.** The blocks dominated by the switch point are cloned once per
selected key. In each clone, `P` and the rematerialized address chain use the
key; the originals become the default arm. Blocks where control rejoins and which
the switch point does not dominate stay shared, with SSA repaired by
`SSAUpdater`, as loop unswitching and jump threading already do. When the switch
point is the entry block, the region is the whole body. Whole-entry cloning is
the degenerate case, not a separate mechanism.

Key-independent code *after* the switch point is still cloned with its region:
work interleaved with key-dependent loads, or a whole loop unswitched on the key.
The region limits bound it, and nothing at the IR level recovers it (§10).

**Helpers.** The key substitution is local to each region clone, never a
module-wide RAUW:
* a helper called from two arms with different constant arguments is left
  generic by IPSCCP, which only folds arguments that agree at every call site;
* a helper reached from one arm only is specialized for that arm alone.

Neither case puts one arm's constants into another arm's code.

**Where it runs**: a new step in `EJitOptimizer::runPipeline`, between phases 1c
and 1d (`EJitOptimizer.cpp:187-194`):

1. Phases 1a–1c run as today. The cell is substituted and its constants folded;
   key-dependent loads are declined.
2. Detect `P` (§4.1) and check the supported form (§4.2).
3. **Select keys by analysis, before any cloning.** For each candidate key `k`
   (every key of a finite domain on the eager path, the frozen keys on the lazy
   path), fold the address chain with `P := k` and ask PASS6's resolver whether
   each key-dependent site's address becomes a resolvable `may_const` location.
   Sites are identified in the uncloned function, and no IR is changed. The
   default arm resolves none of these sites (that is why they were declined), so
   `k` is kept exactly when it resolves at least one. This sees first-order sites
   only, so it may drop a key whose benefit would appear later; that is
   conservative, never incorrect.
4. Choose the path (§5.1) and clone regions for the kept keys only. On the lazy
   path before promotion there are no keys, and only the observation code (§6.2)
   is inserted. A promotion with no kept key stops here, before compiling
   (§6.4).
5. Phases 1d–1f and the rest of the pipeline run unchanged. Nothing is pruned
   afterwards, so they may merge, reshape or delete regions freely. That includes
   a default region made unreachable because the arms cover a finite domain
   entirely, which is then correctly removed.

### 4.4 Dispatch

At the switch point:

```llvm
  %k = <P(%slotNo)>
  switch i32 %k, label %default.region [ i32 0, label %arm0.region
                                         i32 1, label %arm1.region ... ]
```

No table lives outside the function, nothing is written at runtime, and the
dispatch has no concurrency surface. The key needs no bounds check: the switch
default *is* the out-of-set path.

**Lowering is chosen by measurement, not fixed.** Left to the backend, a switch
with around four or more cases may become a jump table: a dependent load and an
indirect branch. A compare chain avoids those, but its cost depends on key
distribution, branch prediction, arm size and layout, and a rapidly varying key
can favour either. The levers are limited. `"no-jump-tables"` forbids jump tables
but does not fix the shape of what replaces them, and it applies to every switch
in the function. So v1 uses the backend's choice, and stage 4 (§9) measures it
against the alternatives across `K` and key distributions before any lever is
set.

---

## 5. Choosing arms and paying for them

### 5.1 Policy

At every compile of an identity:

1. **Mode off** while online PGO is enabled (§10). The attribute is ignored and
   the decline is logged.
2. **No arms** if §4.1–§4.2 decline, or the feature budget has nothing left for
   this identity (§5.3).
3. **Eager** when `M` is finite, `M <= A_max`, and `M` arms fit the §4.2 limits.
4. **Lazy** otherwise, in async compile mode (§6.4), unless §10 excludes the
   entry or identity from the lazy path.
5. Otherwise, no arms.

The choice is remade at every compile, so a configuration update that changes
`P` (a new `numSlots`) simply moves the identity to whichever path now applies.

**A cell specialization has one set of arms**, at most `A_max`. Keys without an
arm take the default arm; there is no second batch. A re-promotion replaces the
set rather than adding to it.

### 5.2 Eager path

The first compile builds the arms of every kept key. There is no observation, no
record, no promotion, and nothing new at the inline cache. The identity's
lifecycle is exactly today's, with arms in the code from the start. For `% 3`,
this is the whole feature. Entries with bound pointers may use it: the compile
carries the call's descriptors as today.

### 5.3 Compile pipeline, feature budget and admission

**The feature budget** bounds every pool byte switch-case adds. It charges the
full reserved size of any allocation that contains arms **or** observation code,
whether that allocation is later published, rejected with `VersionMismatch`, or
discarded. Compiles with neither are today's and are not charged. There are two
caps per generation: one per entry (`EJIT_SWITCH_CASE_MAX_POOL_BYTES`), so the
feature has a hard total; and one per identity
(`EJIT_SWITCH_CASE_MAX_IDENTITY_BYTES`), so one frequently reconfigured cell
cannot spend the allowance of every other cell. Identities outside the inline
cache's range share one overflow counter.

PASS3 emits one global per switch-case entry, `@__ejit_rtdim_<name>`, in
`.mc_shared`, registered by name alongside the icache slot. Its header holds the
entry's charge and the per-identity charges, indexed by `icacheLinearize`
(`EJitSharedTaskPool.cpp:287`), plus the overflow counter. Lazy-path records
follow it (§6.3). None of this is placed in `EJitSharedTaskPoolState`, whose
layout and budget are fixed for every build.

**The pipeline**, with one admission point:

1. **IR**: specialize, select keys, clone, optimize (§4.3).
2. **Object and link graph**: code generation, then JITLink builds the graph.
3. **Layout**: `EJitCodePoolMemoryManager::allocate` computes the page- or
   compact-aligned layout over all segments, and its destination pool
   (`EJitCodePoolMemoryManager.cpp:251-263`). An allocation cannot span pools.
4. **Admission**, in that same call, before `Pool.allocateCode`, and only for
   charged allocations. The charge is the layout's reserved size plus the bytes a
   forced page seal would strand, which the pool reports. The allocation is
   admitted only if the charge fits what is left of both the entry and the
   identity cap, and the charge is recorded together with the allocation, under
   the header's lock. The admitted allocation *is* the reservation; nothing is
   reserved twice.
5. **Link and seal.**
6. **Publish**: as today, or through §6.5 for a promotion.

A charged allocation stays charged on every later failure (link, seal,
`VersionMismatch`, discard), because pool memory is never released (§8.1).

**If admission is refused**, the allocation fails with a distinct error before
any pool memory is taken:
* on an **initial compile**, the worker compiles again without arms or
  observation. That compile is today's, so it is not charged;
* on a **promotion**, the worker keeps the published code, and the identity
  moves to `KEEP_BASELINE` (§6.4), with no second compile.

**Budget order is first come, first served** within each cap. Every charge and
every refusal is logged per identity (§7), and stage 3 tests arrival order, so
which cells win is visible before deployment.

---

## 6. Lazy path

### 6.1 Lifecycle

| State | What calls run |
|---|---|
| Not yet compiled | AOT body, as today |
| `COLLECTING` | The specialization without arms, with observation (§6.2) |
| `VERIFYING` | Same; candidate keys chosen, their coverage being measured |
| `FROZEN` | Same; keys verified, promotion being requested |
| `QUEUED` | Same; promotion compile in flight |
| `PROMOTED` | Arms for the frozen keys, the default arm for others. Every later compile, including after a data update, builds the arms immediately |
| `KEEP_BASELINE` | The specialization without arms, and without observation from its next compile on. Terminal for the epoch |

**Nothing falls back to the AOT body because collection is incomplete.** Waiting
for the keys would give up today's cell specialization for the whole collection
window. The cost is one pre-promotion allocation per identity (§8.1). §10 lists
running AOT until promotion as a future alternative.

### 6.2 Observation

The lazy path chooses keys by **verified frequency**, not first-seen order. The
domain is large or unknown there, and the keys seen at startup need not be the
keys that dominate later.

Before promotion the JIT emits this at the switch point, with the record's
address and the current epoch baked in as constants:

```c
uint32_t n = atomic_fetch_add_relaxed(&rec->calls, 1) + 1;
if ((int32_t)(n - load_relaxed(&rec->nextSample)) >= 0 &&
    load_acquire(&rec->state) <= FROZEN)
  ejit_rtdim_observe(rec, EPOCH, n, P(slotNo));
```

**`calls` is monotonic.** It is a relaxed atomic add, so concurrent callers never
move it backwards, and it is never reset. Comparisons against it are wrap-safe.
The add needs `outline-atomics` disabled on the function, as PGO Tier-1 already
does for its counters (`EJitOptimizer.cpp:211-236`); otherwise the backend emits
a libcall that does not exist on SRE. Old-epoch code still running after a reset
only advances the counter, which is harmless.

**Sampling is jittered.** Each sample sets `nextSample` to `n` plus a random
offset in `[S/2, 3S/2)`, from a per-record xorshift. A fixed stride would lock
onto periodic traffic: with slots cycling 0…15 and a stride of 16, it would see
one slot only.

**Only the runtime changes the rest of the record**, inside
`ejit_rtdim_observe`, under the record lock. It is a runtime function made
visible to JIT modules the way `ejit_vp_record_scalar` already is
(`EJit.cpp:292`). **Every field read outside the lock (`calls`, `nextSample`,
`state`) is accessed only atomically**, with release stores from the runtime.
So the design does not rely on one core per identity. That covers 0-dim entries,
which several cores execute by design (`EJIT_ICACHE_SHARED_TABLE.md` §P1a), as
well as overlapping tasks and migration.

`ejit_rtdim_observe`:
1. `tryWrite` on the record lock. If it is busy, drop the sample. The call path
   never waits, so interrupt reentry on the same core cannot deadlock.
2. If the passed epoch differs from `rec->epoch`, or `n` is before
   `nextSample` (another caller already took this sample), return. Otherwise set
   the next `nextSample`.
3. Act on `state`, rechecked under the lock:
   * **`COLLECTING`**: count the key in a space-saving table of `2·A_max`
     entries. After `W` samples, take the top `A_max` keys as **candidates**,
     zero their hit counters and move to `VERIFYING`. Space-saving counts are
     estimates, so they only nominate keys;
   * **`VERIFYING`**: count exact hits on the candidates over `W` samples. If the
     candidates cover at least `C` of them, freeze them, move to `FROZEN`, and
     try to enqueue. Otherwise clear the table and return to `COLLECTING`. After
     `R` failed verifications, go to `KEEP_BASELINE`;
   * **`FROZEN`**: retry the enqueue with the frozen snapshot;
   * **any other state**: return.

`S`, `W`, `C` and `R` are runtime-configurable, with defaults set from stage 5
data. Open question 1 asks what the real key distributions are.

### 6.3 The record and its epoch

After the budget header (§5.3), `@__ejit_rtdim_<name>` holds a byte index over
the identities, by `icacheLinearize`, where `0xFF` means "no record", and a pool
of at most 255 records. The worker assigns a record when it first compiles an
identity on the lazy path, and bakes its address into the code. A record belongs
to one identity for the whole generation. Eager and declined identities never
take one. A full pool puts the identity in `KEEP_BASELINE`, logged.

```c
struct EJitRtDimRecord {
  EJitRwLock lock;                 // runtime only; tryWrite on the call path
  uint64_t epoch;                  // increases only (below)
  uint64_t proj;                   // projection descriptor (§4.1)
  uint32_t state;                  // §6.1; atomic
  uint32_t calls, nextSample;      // atomic (§6.2)
  uint32_t rng, samples, verifications, attempts, promotions;
  struct { uint32_t key, count; } table[2 * A_max];
  uint32_t hits[A_max];            // VERIFYING
  uint32_t frozenCount;
  uint32_t frozen[A_max];
};
```

At `A_max = 8` a record is about 256 bytes.

**The epoch identifies a collection and is never reused.** It is 64 bits and only
increases. It is bumped, under the lock, when the record is reset for a new
projection, cleared for a new generation, or assigned to a new identity at a
generation change. At one bump per compile it cannot wrap. So code that is still
running from any earlier collection, generation or owner carries an older epoch,
and step 2 of §6.2 rejects its samples. Under the lock:
* **reset** also clears the collection fields, sets `proj`, and sets
  `nextSample` from the current `calls`. Only the worker resets, at the start of
  a compile, when the detected projection differs from `proj`;
* **a compile snapshot** reads `epoch`, `state` and the frozen keys together;
* **a state transition** caused by a compile applies only if `rec->epoch` still
  equals the snapshot's epoch. Otherwise the record is left to the newer
  collection. Stale keys in published code are harmless: dispatch always
  recomputes `P` with that code's own projection.

### 6.4 Promotion state machine

Every exit has an explicit transition:

| From | Event | To |
|---|---|---|
| `COLLECTING` | window of `W` samples closes | `VERIFYING` |
| `VERIFYING` | candidates reach coverage `C` | `FROZEN` |
| `VERIFYING` | below `C`; fewer than `R` failures | `COLLECTING` |
| `VERIFYING` | below `C` for the `R`-th time | `KEEP_BASELINE` |
| `FROZEN` | enqueue succeeds | `QUEUED` |
| `FROZEN` | queue full, dedup declines, or not async | stay; retry on the next sample; `attempts++` |
| `FROZEN` | `attempts` exceeds its limit | `KEEP_BASELINE` |
| `QUEUED` | published | `PROMOTED`; `promotions++`; clear the cell (§6.6) |
| `QUEUED` | `VersionMismatch` or generation mismatch | `FROZEN` (the allocation stays charged, §5.3) |
| `QUEUED` | no kept key (§4.3), admission refused (§5.3), or compile or seal failure | `KEEP_BASELINE` |
| any | projection changes, `promotions < EJIT_SWITCH_CASE_MAX_PROMOTIONS` | reset → `COLLECTING` |
| any | projection changes, promotion limit reached | `KEEP_BASELINE` |
| any | generation change | cleared, epoch bumped |

"Keys frozen" and "request queued" are separate states, so a failed enqueue
never strands an identity. Rejected promotions are bounded by the feature budget
(§5.3), not by `EJIT_SWITCH_CASE_MAX_PROMOTIONS`, which counts only successes.

Promotion is **async-only**: `ejit_rtdim_observe` never compiles, because
compiling inline from inside JIT code would re-enter the compiler.

### 6.5 Publication

The promotion request is a baseline request for the identity, carrying a
**promotion marker**. The worker must not send it through batch staging.
`cacheStagePending` takes the slot matching the identity even when it is
`Ready`, stores the new pointer as `Pending`, and releases the old function
(`EJitSharedTaskPool.cpp:2932-3001`). Callers would lose today's specialization
until the batch is flushed, and nothing makes a flush prompt.

So a marked request takes the direct path of §5.3. Once the code is sealed and
executable, the worker replaces the `Ready` slot's pointer through `cachePublish`
(`EJitSharedTaskPool.cpp:3178-3185`). The old specialization keeps serving until
that store. The inline cache is on, so no code releaser is wired, and the old
code stays executable for callers still inside it.

How the marker is carried is open question 3.

### 6.6 Inline cache: scoped cell clear

A hit jumps straight into the specialization, so after promotion the identity's
cell keeps the pre-promotion pointer until it is cleared. The global
`icacheDrainAll` would do that, but it also clears every unrelated hot cell on
every core. So v1 clears **one cell**, with the full drain protocol of
`icacheDrainAll` (`EJitSharedTaskPool.cpp:415`):

1. capture the generation, and announce: `icacheDrainsInFlight.fetchAdd(1)`;
2. store the table's **empty value** into the cell at `icacheLinearize(dims)`:
   `&MissFn` for sentinel-form tables, whose probe branches through the cell
   unconditionally, and 0 for guarded tables;
3. bump `icacheDrainSeq`, and retire through `ejitIcacheRetireDrain` with the
   captured generation. A plain decrement would underflow the in-flight count
   if the owner re-initialized in between, and block every later fill.

A fill that resolved concurrently sees the drain and retracts itself, so the old
pointer cannot be written back after the clear. `icacheArmed` is left alone.
Promotions published in one worker step share one bracket. Lazy identities are
always within the cell range (§10), so the cell always exists.

**Isolation is partial.** Other entries' cells are not cleared, but the sequence
bump makes any fill resolving concurrently anywhere retract once. That is far
cheaper than a full drain, but not free.

For 0-dim entries the fill stays gated by `icacheCrossCoreExecutable()`, as
today.

---

## 7. Diagnostics

Every step is logged, so that on SRE the logs alone show:
* which entries use the mode, and which path each identity takes;
* when an identity was promoted, and what each arm folded;
* what it cost, and which identities the budget refused.

All lines use the existing `EJIT_DIAG` macros (`EJitDiag.h`) with a common
`rtdim` prefix. None comes from JIT code or a hit path. They come only from
compiles, from `ejit_rtdim_observe` (once per sample at most), and from
publication.

| Event | Level | Content |
|---|---|---|
| Entry registered | `EJIT_DIAG_VERBOSE` | function, limits, record pool size |
| Compile of an identity | `EJIT_DIAG` | function, dims, path (`eager` / `lazy` / `none`), projection descriptor, `M`, candidate and kept keys, switch-point block, region and cloned sizes, whether the body has another switch |
| Per key | `EJIT_DIAG_VERBOSE` | key, sites it resolves; `not kept` if none |
| Declined | `EJIT_DIAG`, once per function | reason: which §4.2 rule failed, a limit, PGO, bound pointer on the lazy path, dims out of cell range, sync mode, attribute ignored because the option is off |
| Admission | `EJIT_DIAG` | function, identity, charge (size + seal waste), admitted or refused, entry and identity budget left |
| Window or verification closed | `EJIT_DIAG` | function, identity, epoch, samples, candidates, coverage, outcome |
| State transition | `EJIT_DIAG` | function, identity, epoch, from → to, cause (every row of §6.4) |
| Promotion published | `EJIT_DIAG` | function, identity, arm count, code size before and after |
| Reset | `EJIT_DIAG` | function, identity, old and new epoch, old and new projection |
| Cell clear | `EJIT_DIAG_VERBOSE` | function, identity, whether the cell held a pointer |

**Coverage needs counters, not logs**, because it is a hot-path property. A
measurement-only build flag, `EJIT_SWITCH_CASE_STATS`, makes the switch point
count arm and default-arm executions per identity, in a side table beside the
record. The runtime prints them at shutdown or on demand. The flag is off in
production and **off when cycles are measured** (§8.2).

---

## 8. Costs and measurement

### 8.1 Costs

This mode spends code to buy speed. Two facts about the code pool decide the
memory side:
* **pool memory is never released in v1.** `EJitCodePoolMemoryManager::deallocate`
  runs the dealloc actions but does not return the memory, because sealed pages
  must not be recycled (`EJitCodePoolMemoryManager.cpp:384-401`);
* **with immediate 4K sealing, every allocation takes at least one page**
  (`EJitCodePool.cpp:243-246`). With batched page sealing, allocations share
  pages until the flush. A promotion that must be executable before it publishes
  can force an early seal, and the unused remainder of those pages is lost too.

| Per identity | Today | Eager | Lazy |
|---|---|---|---|
| Key-dependent region | 1 generic copy | kept keys specialized + 1 default | `K` specialized + 1 default, after promotion |
| Compiles | 1 | 1 | 2 (initial + promotion) |
| Dead allocations | — | — | the pre-promotion code, and any rejected promotion |
| Each recompile after a data update | 1 allocation | 1, larger by the arm regions | 1, larger by the arm regions; before promotion, larger by the observation code |
| Hit path | icache probe → body | probe → body to the switch point → `P` → dispatch → arm | same as eager, after promotion; before it, plus the sampling check |

Everything in the eager and lazy columns beyond today is charged to the feature
budget (§5.3).

`1 + K·r` times today's size, with `r` the region's share of the body, is a
**rough structural estimate only**. Cloning changes what the rest of the pipeline
does: unrolling, inlining, register allocation and layout all respond to function
size. So neither compile time nor allocation size is predicted from it, and "the
default arm is today's code" does not mean it performs like today's compiled
specialization. Both are measured.

### 8.2 Measurement

All measurements are taken on SRE, with the inline cache on. The gate checks
that `-mllvm -ejit-inline-cache` is in the AOT compile command itself; a runtime
cache entry alone does not emit the probe. Cycles are measured with
`EJIT_SWITCH_CASE_STATS` off, and coverage separately with it on.

**Region-size sweep (stage 4).** Vary the switch point's position and the region
size in a synthetic entry: early, late, inside the main loop. Record emitted code
and cycles per path. The results set `EJIT_SWITCH_CASE_MAX_REGION` and
`EJIT_SWITCH_CASE_MAX_CLONED`, and decide the dispatch lowering (§4.4).

**Gates (stage 8).** For each entry, against the three baselines of §1.2:

| Metric | Gate |
|---|---|
| Cycles/call, keys that reach an arm | improvement of at least `G_arm` over today |
| Cycles/call, keys that reach the default arm | regression of at most `G_default` over today |
| Tail latency: worst-case call over the run, including compile, promotion and cell-clear windows | at most `G_tail` over today |
| Bytes and pool pages per identity | reported against each baseline |
| Coverage (lazy path) | reported: share of calls served by arms |

`G_arm`, `G_default` and `G_tail` are fixed from the stage 1 baseline **before**
stage 8 runs, so they are not tuned to the result. An entry that misses a gate
should not use the attribute.

---

## 9. Staging

| Stage | Deliverable | Gate |
|---|---|---|
| 0 | `EJIT_SWITCH_CASE` and limit options; attribute in `Attr.td`, Sema rules and CodeGen tag (§3) | OFF: byte-identical `ejit.o`, attribute warns and is ignored. ON: unchanged output for entries without it. Sema tests for every rule in §3 |
| 1 | The §1.1 integration test and a constant-table variant, run with today's code, inline cache on | Baselines for §8.2; gate values `G_*` and budget defaults fixed |
| 2 | **Transformation prototype**: projection detection, supported-form checks, key selection, region cloning, eager path, compile-time logs (§4, §5.1–§5.2, §7). Unbudgeted; not for deployment | `slotNo = 5` reaches arm 2 and returns `r + 5`; part A appears once; each §4.1 form, including `srem` and a mask falling to identity; descriptors distinguish `% 3` from `% 5`; config-field modulus; address chain above the switch point rematerialized; key-dependent load behind a condition in a loop (hoisted, still correct on zero-trip); helper-only sites declined; a key resolving no site is never cloned; a fully covered finite domain loses its default region correctly; bound-pointer entry gets eager arms; every §4.2 decline logged |
| 3 | Admission in `allocate`, feature budget, per-identity caps (§5.3). **The eager path is complete here** | Admission happens before `Pool.allocateCode`, and the admitted allocation is the one charged; an allocation larger than the remaining budget is refused; seal waste is included; a refused initial compile recompiles uncharged; charges survive every later failure; arrival order across identities is logged and tested |
| 4 | Region-size sweep; dispatch lowering comparison (§4.4, §8.2) | Limits set; lowering chosen |
| 5 | Lazy path: record pool, sampling, verification, epoch, state machine (§6.2–§6.4) | gtest: concurrent observers on one record, including a 0-dim entry, lose no correctness; a caller entering after a freeze does not reopen collection; a paused caller cannot move `calls` backwards, under heavy traffic or across a reset; old-epoch samples are rejected after a reset, a generation change and a reassignment; every §6.4 transition; repeated rejected promotions stop at the budget; periodic, bursty and phase-changing traffic freeze the dominant keys, not an aliased one |
| 6 | Promotion publication and scoped cell clear (§6.5, §6.6) | Marked requests never reach `cacheStagePending`; the old specialization serves until the replacement is executable; a refused promotion keeps the published code with no second compile; a fill racing the clear retracts; the clear retires through `ejitIcacheRetireDrain` |
| 7 | PGO and bound-pointer handling (§5.1, §10) | With PGO enabled the attribute is ignored and logged; bound-pointer entries never enter the lazy path |
| 8 | Measurement on SRE (§8.2) | The gates |

---

## 10. Exclusions and future work

**Excluded in v1:**
* **Online PGO, for the whole mode.** Tier-1 and Tier-2 must number sites on the
  same CFG, and arms change it. Leaving a switch-case entry at baseline is not
  enough either: while PGO is enabled, `icacheFill` refuses any pointer that is
  not Tier-2 (`EJitSharedTaskPool.cpp:836`), so a baseline entry would never be
  cached, which is worse than today. Mixed PGO and switch-case deployments are
  not supported (§5.1).
* **The lazy path for `ejit_bound_ptr` entries.** The promotion request is
  raised from JIT code, which has no bound-pointer descriptor to attach.
* **The lazy path for identities outside the inline cache's range**, whose
  instance ids reach `EJIT_ICACHE_DIM_SIZE` (`icacheDimsInRange`,
  `EJitSharedTaskPool.cpp:276`). They have no cell to clear (§6.6) and no entry
  in the record index, which uses the same linearization (§6.3).

`ejit_free_dim` is independent: it is a different parameter, and PASS6 keeps
treating it as today, inside every arm.

**Future work:**
* **Run AOT until promotion.** Skip the lazy path's pre-promotion compile and
  collect on the AOT path instead. This saves the dead allocation and a compile.
  It costs performance during collection, since the entry runs below today's
  code until promotion. It also has to collect raw values, since a
  configuration-dependent `P` is only known after the cell is specialized.
  Worth considering if pool pages prove scarcer than collection-window cycles.
* **`MachineOutliner`**, for identical key-independent code left inside arms
  (§4.3). It is compiled out under `EJIT_TRIM_LLVM_BACKEND`, so restoring it costs
  runtime-library size. It also turns shared hot-path sequences into calls, and it
  only guarantees a size win. Consider it only if stage 8 shows significant
  duplication, measuring cycles as well as bytes.
* **More than one batch of arms per cell.** Today, keys that do not fit in the
  `A_max` arms fall to the default arm. A later version could compile further
  batches of arms for those keys, so more of them get specialized code. Each
  batch costs a recompile and pool space that is never freed, so this should be
  driven by the measured coverage (§8.2).
* **Adapting frozen keys** when the dominant keys shift after promotion. Like
  extra batches, any re-selection draws on the feature budget, because each one
  leaves unreclaimable code behind.
* **More projection forms**, such as a mask (`v & (2ⁿ − 1)`, `M = 2ⁿ`), if real
  entries start using them. Until then they fall to the identity row: correct,
  but lazy-only.
* Online PGO support; the lazy path for bound-pointer entries; more than one
  runtime dim per entry; sites in non-inlined helpers.

---

## 11. Soundness rules

The checklist for review and tests; each rule is stated where it arises.

1. **Only `P(runtime dim)` and its address chain are substituted in an arm,
   never the parameter**, unless `P` is the identity (§4.1).
2. **An arm is entered only through its `case`** (§4.4).
3. **The key substitution is local to its region clone**, never a module-wide
   RAUW (§4.3).
4. **Keys are selected before cloning**; no later step depends on a region
   surviving optimization (§4.3).
5. **The runtime dim never enters the identity**: not the cache key, the request
   or the icache index (§1.3).
6. **Fields read outside the record lock are accessed only atomically; every
   other change is made under the lock, for a matching epoch; epochs are never
   reused** (§6.2, §6.3).
7. **Promotion never compiles from JIT code, and never goes through batch
   staging** (§6.4, §6.5).

---

## 12. Open questions

1. **Real key behaviour.** `P` is usually a modulus. Is its divisor a constant or
   a configuration field, and how are the keys distributed per cell per TTI? This
   decides how often the eager path applies, and sets `S`, `W`, `C` and `R`.
2. **Record pool size per entry.** Up to 255 records of about 256 bytes. How many
   lazy identities does one real entry have?
3. **The promotion marker.** It must reach the worker without an ABI change.
   Two candidates:
   * a high bit of `numDims`, mirroring the tier in `funcIndex`'s top bits
     (`EJitSreQueue.h:100-107`). Every `numDims` consumer must then mask it;
   * the free tier value. But `isPublishedTier2` tests `>= kEJitTierPgoUse`, so
     that value would read as Tier-2 unless those comparisons are audited.
4. **How often is the switch point late enough?** If real entries load
   key-dependent data early, or inside their main loop, regions approach the
   whole body and the limits decline them. The compile log's switch-point block
   and region size (§7) answer this on real entries.
