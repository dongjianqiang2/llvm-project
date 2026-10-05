# EJIT Switch-Case Mode — runtime-keyed arms inside a per-identity specialization

**Status**: stages 0 and 2 implemented (attribute, gating, eager path;
`EJitSwitchCase.cpp`). Budget and admission (stage 3), the lazy path (stages
5-6) and PGO handling beyond declining non-Baseline tiers (stage 7) are not.
The region and clone limits are provisional until the stage 4 sweep.
**Related**: `EJIT_FREE_DIM.md`, `EJIT_ICACHE_MULTIVERSION.md`,
`EJIT_ICACHE_SHARED_TABLE.md`, `PASS3_EJitWrapperGen.md`,
`PASS6_EJitStructFieldPass.md`

---

## 1. Goal and scope

### 1.1 Problem

Some entries select their `may_const` data with a runtime value that EJIT
cannot fold. The running example uses a cell index and a slot number, but the
mechanism applies to any parameter carrying the attribute (§3).

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

ejit_period_arr(cell) CellCfg g_cellCfg[8];

ejit_entry
int32_t process(ejit_period_arr_ind(cell) uint8_t cellIndex,
                uint32_t slotNo, const int16_t *in) {
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

`cellIndex` folds; `slotNo` does not, so PASS6 declines every load through `s`
with `non-const-offset`. Cell 3's specialization folds part A and none of part B.

No existing attribute fits `slotNo`. `ejit_dim` needs a period array with a
declared range and its own activate/deactivate lifecycle; the slot has neither,
and its values (up to about 1024) exceed `kEJitSharedInstances`. `ejit_free_dim`
requires the data to be the same for every value, which it is not.

### 1.2 Objective

**Fold slot-dependent data inside each cell's specialization, without a full
specialization per slot.** Each cell keeps one specialization. Inside it, a few
**arms** are keyed on the slot, plus a **default arm**, and only the code that
needs the key is cloned (§4). For cell 3 with `len = 4`,
`coef = {1, -2, 3, 1}`, `clip = 1000` and
`slot[0..2] = {2,3,1}, {0,1,0}, {4,5,1}`:

```c
int32_t process_cell3(uint8_t, uint32_t slotNo, const int16_t *in) {
  int32_t acc = in[0] - 2*in[1] + 3*in[2] + in[3];   /* part A, once */
  if (acc > 1000) acc = 1000;

  int32_t r;
  switch (slotNo % 3) {                              /* the key, §4.1 */
    case 0:  r = (acc >> 2) * 3; break;
    case 1:  r = acc;            break;
    case 2:  r = (acc >> 4) * 5; break;
    default: {                                       /* default arm */
      const SlotCfg *s = &g_cellCfg[3].slot[slotNo % 3];
      r = acc >> s->shift;
      if (s->mode) r *= s->scale;
    }
  }
  return r + slotNo;                                 /* slotNo stays live */
}
```

The gain is speed, bought with bounded extra code. Against today's single
specialization the arms cost code and must be faster; against a full
specialization per `(cell, slot)` they cost much less; against a constant table
of slot data indexed by the key they usually cost more, and must win by folding
branches like `if (s->mode)`. All three are measured (§8).

**Selection churn** (calls pick slot 0, then 2) only selects a different arm.
**Configuration churn** (a period handler rewrites slot data) invalidates the
cell's specialization and recompiles every arm with it, bounded by the feature
budget (§5.3).

### 1.3 What does not change

The runtime dim never enters the specialization identity, so the cache identity
`(funcIndex, dims[])`, `EJitCompileRequest`, dedup, versions, publication, the
generation gate and deactivation are all unchanged. A specialization, arms
included, is one function, invalidated and rebuilt as a whole.

**The inline cache** (`-mllvm -ejit-inline-cache`), which the design assumes is
on, stays keyed on the `ejit_dim`s. A hit tail-calls the specialization with the
original arguments, so the runtime dim is an ordinary argument and
`EJIT_ICACHE_DIM_SIZE` does not bound it. Only the lazy path's promotion
interacts with it (§6.6).

The default arm is today's specialization, so **no state of this design runs
code less specialized than today's**.

### 1.4 Gating

The CMake option `EJIT_SWITCH_CASE` builds the support. **OFF**: `ejit.o` is
byte-identical to today's, and Clang warns that the attribute is ignored.
**ON**: entries without the attribute are unchanged. The attribute is the only
per-entry opt-in.

---

## 2. Terminology

| Term | Meaning |
|---|---|
| **runtime dim** | the parameter carrying `ejit_runtime_dim` |
| **projection `P`** | the function of the runtime dim through which the `may_const` addresses depend on it (§4.1) |
| **key** | `P(runtime dim)`; the value an arm is specialized for |
| **`M`** | the number of keys `P` can produce; unbounded for the identity |
| **switch point** | where the key is computed and dispatched on (§4.3) |
| **arm** / **default arm** | a clone of the code after the switch point with `P` replaced by one key / the original code |
| **eager path** | all useful arms of a finite domain, built in the first compile (§5.2) |
| **lazy path** | arms chosen from observed keys, added by one **promotion** (§6) |
| **feature budget** | the pool bytes switch-case may consume beyond today (§5.3) |
| **epoch** | the lifecycle stamp of a lazy-path record; never reused (§6.3) |

---

## 3. The attribute and its limits

```c
#define ejit_runtime_dim       __attribute__((ejit_runtime_dim))
#define ejit_runtime_dim_n(n)  __attribute__((__ejit_runtime_dim__(n)))

ejit_entry
int32_t process(ejit_period_arr_ind(cell) uint8_t cellIndex,
                ejit_runtime_dim uint32_t slotNo, const int16_t *in);
```

It applies to an integer parameter of at most 32 bits of an `ejit_entry`, at
most one per function in v1, not combined with `ejit_period_arr_ind` or
`ejit_free_dim`. A 0-dim entry may carry it; its single identity gets the arms.

**It asserts nothing.** Every arm is guarded by its key, and any other value
runs the default arm, so it is only an opt-in to spending code size. No range,
modulus or period name is declared; the JIT derives them (§4.1). CodeGen emits
`{"ejit_runtime_dim", "", argIdx, maxArms}` on the entry's `!ejit.metadata`.

**Limits** are CMake options compiled into the runtime. Arm count alone does
not bound cost, since eight tiny arms and eight cloned loops differ by orders
of magnitude.

| Option | Bounds | Default |
|---|---|---|
| `EJIT_SWITCH_CASE_MAX_ARMS` (`A_max`) | arms per specialization; overridable per entry with `ejit_runtime_dim_n(n)` | 8 |
| `EJIT_SWITCH_CASE_MAX_REGION` | IR instructions in one arm's region | 2000, provisional (§8.2) |
| `EJIT_SWITCH_CASE_MAX_CLONED` | IR instructions added by cloning in one compile | 8000, provisional |
| `EJIT_SWITCH_CASE_MAX_POOL_BYTES` | feature budget per entry per generation (§5.3) | not implemented |
| `EJIT_SWITCH_CASE_MAX_IDENTITY_BYTES` | feature budget per identity per generation | not implemented |
| `EJIT_SWITCH_CASE_MAX_PROMOTIONS` | successful promotions per identity per generation (§6.4) | not implemented |

---

## 4. The specialization

### 4.1 Finding the key: projection, not the parameter

Under `slotNo % 3`, the arm for key 2 serves `slotNo` = 2, 5, 8, …, so an arm
must **not** substitute the parameter: `return r + slotNo` would become
`return r + 2`. Only `P(slotNo)` is replaced; every other use stays live. This
is why `preReplacePeriodIndices`'s whole-parameter RAUW cannot be reused.

**Detection.** The sites are the `may_const` loads PASS6 declined whose address
depends on the runtime dim. A load counts if it is `may_const` at any key of its
own projection, not only at key 0, because a bound-pointer field is recognized
by the offset the key resolves to. Each site's address is walked back to the
parameter, and `P` is the projection every site agrees on. Key-dependent loads
that are not sites, such as `input[slotNo % 5]`, take no part.

| Form | `P` | `M` | Path |
|---|---|---|---|
| `urem v, C`, `C` constant after cell specialization | `v % C` | `C` | eager if within limits, else lazy |
| `and v, 2ⁿ − 1` | `v % 2ⁿ` | `2ⁿ` | as `urem` |
| anything else | `v` (identity) | unbounded | lazy |

`v` is the parameter or its `zext`. `srem`, `sdiv`, `trunc`, `sext` and other
masks fall to the identity row. InstCombine (already run at AOT `-O2`) turns
`v % 2ⁿ` into the low-bit mask, which is recorded as that `urem`: `% 4` and
`& 3` share a descriptor, and a power-of-two domain dispatches on the mask. A
signed `v % 2ⁿ` stays an `srem`.

**The projection descriptor** is `{op:8, width:8, pad:16, constant:32}`,
compared field by field. It is lossless because a `urem` whose constant is at
least `2^w` for a `w`-bit parameter cannot change its value and is recorded as
the identity.

The identity row is always valid, but not always useful: `table[slotNo +
runtimeOffset]` keeps a dynamic offset for every `slotNo`. Keys are therefore
selected by what they actually fold (§4.3).

Detection runs **after** the cell is specialized, so a modulus that is itself
configuration (`v % c->numSlots`) is already a constant.

### 4.2 Supported form

v1 transforms only this form and declines everything else with a logged reason:

1. **Entry-local sites.** Sites in non-inlined helpers stay generic; if every
   site is in a helper, the entry gets no arms.
2. **A recognized projection** from the §4.1 table.
3. **A rematerializable address chain.** Every instruction between `P` and the
   sites must be speculatable and side-effect free. It is recomputed inside the
   region, so that an address computed before the switch point does not stay
   on the variable `P`.
4. **Supported control flow.** Loops have a preheader. The region has no
   `invoke`, `callbr`, `indirectbr`, EH pad, address-taken block, `noduplicate`
   call or token value.
5. **Within limits** (§3).

### 4.3 Region-level arms

Arms clone only the code that needs the key, so cell-only work (part A) is not
copied into every arm.

**The switch point** is the latest point that dominates every site and is not
inside a loop; a dominator inside a loop moves to the outermost loop's
preheader, as in loop unswitching.

The switch therefore also runs on paths that reach no site: past a branch below
the switch point, or ahead of a zero-trip loop. Computing `P` there is legal
(§4.2 rule 3), but branching on an undef or poison value is UB, so the key is
computed from `freeze` of the parameter unless it is `noundef` (as clang emits
for C parameters). The arms still replace the unfrozen instances of `P`:
entering arm `k` means the frozen value has key `k`, one of the values an undef
`P` could take.

**Cloning.** The region is the switch point's block from the switch point on,
plus every block it dominates. It is cloned once per kept key; in each clone,
`P` and the rematerialized chain use the key, and the originals become the
default arm. Unreachable blocks are removed first, so every predecessor of a
region block is in the region and only phis at the region's exits need new
incoming values. A switch point in the entry block makes the region the whole
body.

Key-independent code after the switch point is cloned with its region; the
region limits bound it (§10).

**Helpers.** The key substitution is local to each clone, never a module-wide
RAUW. IPSCCP leaves a helper called with different constants from two arms
generic, and specializes a helper reached from one arm for that arm alone.

**Where it runs**: in `EJitOptimizer::runPipeline`, between phases 1c and 1d:

1. Phases 1a–1c run as today: the cell is substituted, its constants folded,
   key-dependent loads declined.
2. Detect `P` (§4.1) and check the supported form (§4.2).
3. **Select keys by analysis, before cloning.** For each candidate key `k`
   (every key of a finite domain on the eager path, the frozen keys on the lazy
   path), ask PASS6's resolver whether each site resolves with `P := k`. `k` is
   kept if it resolves at least one. This sees first-order sites only, which
   is conservative, never incorrect.
4. Choose the path (§5.1) and clone regions for the kept keys.
5. Phases 1d–1f and the rest of the pipeline run unchanged, and may merge or
   delete regions freely, including a default arm made unreachable because the
   arms cover a finite domain entirely.

### 4.4 Dispatch

```llvm
  %k = <P(freeze %slotNo)>
  switch i32 %k, label %default.region [ i32 0, label %arm0.region
                                         i32 1, label %arm1.region ... ]
```

Nothing lives outside the function and nothing is written at runtime. The switch
default is the out-of-set path, so no bounds check is needed.

**Lowering is left to the backend** in v1. A switch may become a jump table (a
dependent load and an indirect branch) or a compare chain, and which is better
depends on key distribution and arm size. Stage 4 (§9) measures it before any
lever, such as `"no-jump-tables"`, is set.

---

## 5. Choosing arms and paying for them

### 5.1 Policy

At every compile of an identity:

1. **Off** while online PGO is enabled (§10), logged.
2. **No arms** if §4.1–§4.2 decline, or the feature budget is exhausted (§5.3).
3. **Eager** when `M` is finite, `M <= A_max`, and the arms fit the limits.
4. **Lazy** otherwise, in async compile mode (§6.4), unless §10 excludes it.

The choice is remade at every compile, so a configuration change that changes
`P` moves the identity to whichever path now applies. A cell specialization has
one set of at most `A_max` arms; a re-promotion replaces it.

### 5.2 Eager path

The first compile builds the arms of every kept key. There is no observation,
no record and no promotion, and the identity's lifecycle is today's. Entries
with bound pointers may use it. For `% 3`, this is the whole feature.

### 5.3 Compile pipeline, feature budget and admission

**The feature budget** charges the full reserved size of every allocation that
contains arms or observation code, whether it is published, rejected or
discarded. There are two caps per generation: per entry
(`EJIT_SWITCH_CASE_MAX_POOL_BYTES`), and per identity
(`EJIT_SWITCH_CASE_MAX_IDENTITY_BYTES`) so one reconfigured cell cannot spend
every other cell's allowance. Identities outside the inline cache's range share
one overflow counter.

PASS3 emits one global per switch-case entry, `@__ejit_rtdim_<name>`, in
`.mc_shared`, registered by name like the icache slot. Its header holds the
entry and per-identity charges (indexed by `icacheLinearize`) and the overflow
counter; lazy-path records follow (§6.3). Nothing is added to
`EJitSharedTaskPoolState`.

**Admission** happens in `EJitCodePoolMemoryManager::allocate`, after layout and
before `Pool.allocateCode`, for charged allocations only. The charge is the
layout's reserved size plus any bytes a forced page seal would strand. It is
admitted if it fits both caps, and recorded under the header's lock; the
admitted allocation is the reservation. Pool memory is never released (§8.1),
so a charge stays on every later failure.

If admission is refused, no pool memory is taken. An **initial compile** is
redone without arms or observation, uncharged; a **promotion** keeps the
published code and moves to `KEEP_BASELINE` (§6.4). Budget is first come,
first served within each cap, and every charge and refusal is logged (§7).

---

## 6. Lazy path

### 6.1 Lifecycle

| State | What calls run |
|---|---|
| Not yet compiled | AOT body |
| `COLLECTING` | the specialization without arms, with observation (§6.2) |
| `VERIFYING` | same; candidate keys chosen, coverage being measured |
| `FROZEN` | same; keys verified, promotion being requested |
| `QUEUED` | same; promotion compile in flight |
| `PROMOTED` | arms for the frozen keys. Every later compile builds them immediately |
| `KEEP_BASELINE` | the specialization without arms or observation. Terminal for the epoch |

Nothing falls back to the AOT body while collecting, which would give up
today's specialization for the whole window. The cost is one pre-promotion
allocation per identity (§8.1).

### 6.2 Observation

The lazy path chooses keys by **verified frequency**, not first-seen order.
Before promotion the JIT emits at the switch point, with the record's address
and the epoch as constants:

```c
uint32_t n = atomic_fetch_add_relaxed(&rec->calls, 1) + 1;
if ((int32_t)(n - load_relaxed(&rec->nextSample)) >= 0 &&
    load_acquire(&rec->state) <= FROZEN)
  ejit_rtdim_observe(rec, EPOCH, n, P(slotNo));
```

* **`calls` is monotonic**: a relaxed atomic add, never reset, compared
  wrap-safely. It needs `outline-atomics` disabled, as PGO Tier-1 does, since
  the libcall does not exist on SRE.
* **Sampling is jittered**: `nextSample` advances by a random offset in
  `[S/2, 3S/2)` from a per-record xorshift, so periodic traffic cannot alias
  with the stride.
* **Only the runtime changes the rest of the record**, in `ejit_rtdim_observe`,
  under the record lock, made visible to JIT modules like
  `ejit_vp_record_scalar`. Fields read outside the lock (`calls`, `nextSample`,
  `state`) are only accessed atomically, so several cores may run one identity.

`ejit_rtdim_observe`:
1. `tryWrite` the record lock; if busy, drop the sample, so interrupt reentry
   cannot deadlock.
2. Return if the epoch differs or another caller took this sample; otherwise
   set the next `nextSample`.
3. By `state`, rechecked under the lock:
   * **`COLLECTING`**: count the key in a space-saving table of `2·A_max`
     entries. After `W` samples, nominate the top `A_max` as candidates and move
     to `VERIFYING`;
   * **`VERIFYING`**: count exact hits on the candidates over `W` samples. If
     they cover at least `C`, freeze them, move to `FROZEN` and try to enqueue;
     otherwise return to `COLLECTING`, or after `R` failures to
     `KEEP_BASELINE`;
   * **`FROZEN`**: retry the enqueue.

`S`, `W`, `C` and `R` are runtime-configurable, with defaults from stage 5 data.

### 6.3 The record and its epoch

After the budget header (§5.3), `@__ejit_rtdim_<name>` holds a byte index over
the identities (`0xFF` = no record) and a pool of at most 255 records. The
worker assigns a record when it first compiles an identity on the lazy path; it
belongs to that identity for the generation. A full pool sends the identity to
`KEEP_BASELINE`.

```c
struct EJitRtDimRecord {
  EJitRwLock lock;                 // runtime only; tryWrite on the call path
  uint64_t epoch;                  // increases only
  uint64_t proj;                   // projection descriptor (§4.1)
  uint32_t state;                  // atomic
  uint32_t calls, nextSample;      // atomic
  uint32_t rng, samples, verifications, attempts, promotions;
  struct { uint32_t key, count; } table[2 * A_max];
  uint32_t hits[A_max];
  uint32_t frozenCount;
  uint32_t frozen[A_max];
};                                 // about 256 bytes at A_max = 8
```

**The epoch is 64 bits and never reused.** It is bumped under the lock when the
record is reset for a new projection, cleared for a new generation, or
reassigned, so code from any earlier collection carries an older epoch and its
samples are rejected. A reset (only by the worker, when the detected projection
differs from `proj`) also clears the collection fields. A compile reads epoch,
state and frozen keys together, and its state transition applies only if the
epoch is unchanged. Stale keys in published code are harmless, since dispatch
recomputes `P` with that code's own projection.

### 6.4 Promotion state machine

| From | Event | To |
|---|---|---|
| `COLLECTING` | `W` samples | `VERIFYING` |
| `VERIFYING` | coverage `C` reached | `FROZEN` |
| `VERIFYING` | below `C`, fewer than `R` failures | `COLLECTING` |
| `VERIFYING` | below `C` for the `R`-th time | `KEEP_BASELINE` |
| `FROZEN` | enqueue succeeds | `QUEUED` |
| `FROZEN` | queue full, dedup declines, or not async | stay; `attempts++` |
| `FROZEN` | `attempts` over its limit | `KEEP_BASELINE` |
| `QUEUED` | published | `PROMOTED`; `promotions++`; clear the cell (§6.6) |
| `QUEUED` | `VersionMismatch` or generation mismatch | `FROZEN` (still charged) |
| `QUEUED` | no kept key, admission refused, or compile/seal failure | `KEEP_BASELINE` |
| any | projection changes, below the promotion limit | reset → `COLLECTING` |
| any | projection changes, promotion limit reached | `KEEP_BASELINE` |
| any | generation change | cleared, epoch bumped |

Freezing and queueing are separate states, so a failed enqueue never strands an
identity. Promotion is **async-only**: compiling from inside JIT code would
re-enter the compiler.

### 6.5 Publication

A promotion is a baseline request for the identity carrying a **promotion
marker** (open question 3). It must not go through batch staging:
`cacheStagePending` would replace the identity's `Ready` slot with a `Pending`
one, and callers would lose today's specialization until a flush. Instead, once
the code is sealed and executable, the worker replaces the `Ready` slot's
pointer through `cachePublish`. The old code keeps serving until that store and
stays executable for callers inside it.

### 6.6 Inline cache: scoped cell clear

After promotion the identity's icache cell still holds the old pointer.
`icacheDrainAll` would clear every hot cell on every core, so v1 clears one cell
with the same protocol:

1. capture the generation and `icacheDrainsInFlight.fetchAdd(1)`;
2. store the table's empty value at `icacheLinearize(dims)`: `&MissFn` for
   sentinel-form tables, 0 for guarded ones;
3. bump `icacheDrainSeq` and retire through `ejitIcacheRetireDrain` with the
   captured generation.

A concurrent fill sees the drain and retracts, so the old pointer cannot return.
Other cells are not cleared, though any fill resolving concurrently retracts
once. Promotions published in one worker step share one bracket. 0-dim entries
keep the `icacheCrossCoreExecutable()` gate.

---

## 7. Diagnostics

All lines use the `EJIT_DIAG` macros with an `rtdim` prefix, and come only from
compiles, `ejit_rtdim_observe` and publication, never from a hit path.
Implemented today: the per-compile line (eager or declined) and the per-key
line.

| Event | Level | Content |
|---|---|---|
| Compile | `EJIT_DIAG` | path, projection, `M`, sites, kept keys, switch point, region and cloned sizes |
| Per key | `EJIT_DIAG_VERBOSE` | key, sites it resolves, `not-kept` if none |
| Declined | `EJIT_DIAG` | reason (§4.2 rule, limit, PGO, lazy path not implemented) |
| Admission | `EJIT_DIAG` | identity, charge, admitted or refused, budget left |
| Window or verification closed | `EJIT_DIAG` | identity, epoch, candidates, coverage, outcome |
| State transition | `EJIT_DIAG` | identity, epoch, from → to, cause |
| Promotion published | `EJIT_DIAG` | identity, arm count, code size before and after |
| Reset, cell clear | `EJIT_DIAG` / `VERBOSE` | identity, epochs and projections / whether the cell held a pointer |

Coverage is a hot-path property, so it needs counters: a measurement-only flag,
`EJIT_SWITCH_CASE_STATS`, counts arm and default-arm executions per identity.
It is off in production and when cycles are measured.

---

## 8. Costs and measurement

### 8.1 Costs

Pool memory is never released in v1 (sealed pages are not recycled), and with
immediate 4K sealing every allocation takes at least one page.

| Per identity | Today | Eager | Lazy |
|---|---|---|---|
| Key-dependent region | 1 generic copy | kept keys + default | `K` + default, after promotion |
| Compiles | 1 | 1 | 2 |
| Dead allocations | — | — | pre-promotion code, rejected promotions |
| Hit path | probe → body | probe → body → `P` → dispatch → arm | as eager; before promotion, plus the sampling check |

`1 + K·r` times today's size, with `r` the region's share of the body, is only a
rough estimate: cloning changes unrolling, inlining and register allocation. So
size and compile time are measured, not predicted.

### 8.2 Measurement

On SRE, with `-mllvm -ejit-inline-cache` in the AOT compile command itself, and
cycles measured with `EJIT_SWITCH_CASE_STATS` off.

**Region-size sweep (stage 4).** Vary the switch point (early, late, inside the
main loop) and region size in a synthetic entry; record code size and cycles.
This sets `EJIT_SWITCH_CASE_MAX_REGION`, `EJIT_SWITCH_CASE_MAX_CLONED` and the
dispatch lowering.

**Gates (stage 8)**, per entry against the §1.2 baselines: cycles per call on
arm keys improve by at least `G_arm`; on default-arm keys regress by at most
`G_default`; worst-case call latency, including compile and promotion windows,
within `G_tail`; bytes, pages and coverage reported. The `G_*` values are fixed
from stage 1 before stage 8 runs. An entry that misses a gate should not use
the attribute.

---

## 9. Staging

| Stage | Deliverable | Status |
|---|---|---|
| 0 | Options, attribute, Sema rules, CodeGen tag (§3) | done |
| 1 | The §1.1 integration test and a constant-table variant on today's code; baselines and `G_*` | |
| 2 | Eager path: detection, supported-form checks, key selection, cloning, logs (§4, §5.2, §7) | done |
| 3 | Admission and feature budget (§5.3); completes the eager path | |
| 4 | Region-size sweep and dispatch lowering (§8.2) | |
| 5 | Lazy path: records, sampling, verification, epochs, state machine (§6.2–§6.4) | |
| 6 | Promotion publication and scoped cell clear (§6.5, §6.6) | |
| 7 | PGO and bound-pointer handling (§10) | partial: PGO tiers declined |
| 8 | Measurement on SRE (§8.2) | |

---

## 10. Exclusions and future work

**Excluded in v1:**
* **Online PGO.** Tier-1 and Tier-2 number sites on the same CFG, and arms
  change it. Leaving the entry at baseline is no better: with PGO on,
  `icacheFill` refuses non-Tier-2 pointers, so it would never be cached.
* **The lazy path for `ejit_bound_ptr` entries**, whose promotion request would
  be raised from JIT code with no descriptor to attach.
* **The lazy path outside the inline cache's range** (`icacheDimsInRange`):
  there is no cell to clear and no record index entry.

`ejit_free_dim` is a different parameter and is treated as today in every arm.

**Future work:** running AOT until promotion (saves a compile and an allocation,
costs collection-window speed); `MachineOutliner` for identical code left in
arms (compiled out under `EJIT_TRIM_LLVM_BACKEND`); more than one batch of arms
per cell; adapting frozen keys after promotion; more projection forms, such as
general masks; more than one runtime dim per entry; sites in non-inlined
helpers.

---

## 11. Soundness rules

1. **Only `P(runtime dim)` and its address chain are substituted in an arm,
   never the parameter**, unless `P` is the identity (§4.1).
2. **An arm is entered only through its `case`**, on a frozen key (§4.3, §4.4).
3. **The key substitution is local to its region clone** (§4.3).
4. **Keys are selected before cloning**; no later step depends on a region
   surviving optimization (§4.3).
5. **The runtime dim never enters the identity** (§1.3).
6. **Fields read outside the record lock are only accessed atomically; other
   changes are made under the lock, for a matching epoch; epochs are never
   reused** (§6.2, §6.3).
7. **Promotion never compiles from JIT code, and never goes through batch
   staging** (§6.4, §6.5).

---

## 12. Open questions

1. **Real key behaviour.** Is the divisor a constant or a configuration field,
   and how are keys distributed per cell per TTI? This decides how often the
   eager path applies, and sets `S`, `W`, `C` and `R`.
2. **Record pool size.** How many lazy identities does one real entry have?
3. **The promotion marker**, without an ABI change: a high bit of `numDims`
   (every consumer must mask it), or the free tier value (`isPublishedTier2`
   tests `>= kEJitTierPgoUse`, so it would read as Tier-2 unless audited).
4. **How often is the switch point late enough?** If real entries load
   key-dependent data early or inside their main loop, regions approach the
   whole body and the limits decline them. The compile log answers this.
