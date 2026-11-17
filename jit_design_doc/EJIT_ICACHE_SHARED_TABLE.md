# EJIT Inline Cache: Shared Partitioned Cell Table

> Moves the `@__ejit_icache_fn_<name>` cell table from per-core `.bss` into the
> inter-core shared section, so one index-partitioned table backs every core. A
> period toggle then invalidates by zeroing the cells in place, which reaches
> every core directly — no state on the probe, no per-core drain rendezvous, no
> sync entry point for the application.
>
> Builds on `-ejit-inline-cache` (`EJIT_ICACHE_MULTIVERSION.md`), which owns the
> `[D]^numDims` direct-indexed shape and the frame-less wrapper.

---

## 1. Problem

The probe is four instructions inside `ejit_entry`, so it **cannot ask whether
the cell is still valid** without ceasing to be four instructions. Invalidation
has to happen *to the cell*, from outside.

With the table in per-core `.bss`, a deactivate could only reach the table of
the core it ran on. A permanently hot peer — one that never misses, so never
calls the runtime again — kept executing a specialization baked from period
values that had already been replaced, and nothing in the system could reach it.

## 2. Hit path

Unchanged by this patch except that the load is now `atomic monotonic` (the
writer can be another core, so the concurrent drain must be a defined race
rather than UB; `monotonic` is the same `LDR` on AArch64).

```llvm
jit_entry:                                          ; frame-less
  %ejit_ic_slot = getelementptr inbounds [16 x ptr], ptr @__ejit_icache_fn_one_dim_entry, i32 0, i32 %idx1
  %ejit_ic_fn   = load atomic ptr, ptr %ejit_ic_slot monotonic, align 8
  %hit          = icmp ne ptr %ejit_ic_fn, null
  br i1 %hit, label %jit_icache_dispatch, label %jit_miss

jit_icache_dispatch:
  %r = musttail call i32 %ejit_ic_fn(i32 %idx1)     ; lowers to br
  ret i32 %r
```

```asm
one_dim_entry:
  adrp  x8, __ejit_icache_fn_one_dim_entry
  add   x8, x8, #:lo12:__ejit_icache_fn_one_dim_entry
  ldr   x1, [x8, w0, sxtw #3]      ; the cell
  cbz   x1, .Lmiss
  br    x1                          ; tail-call the specialization
.Lmiss:
  b     one_dim_entry_miss
```

No funcIndex guard, no version check, no read token, no call. Everything else —
L0, the bucket lookup, the compile scheduler — is out of line in `MissFn`.

## 3. Design

### 3.1 Storage

`EJitWrapperGen` emits `@__ejit_icache_fn_<name>` into `EJIT_ICACHE_SECTION`
(default `.mc_shared`), the region the taskpool blob and period data already use.

```
              .mc_shared  (ONE object, every core sees it)
   ┌──────────────────────────────────────────────────────┐
   │ @__ejit_icache_fn_foo[D]                             │
   │  [0] [1] [2] [3]  │  [4] [5] [6] [7]  │  ...         │
   └─── core A writes ─┴─── core B writes ─┴──────────────┘
   Both cores READ all of it. A drain on ANY core clears all of it.
```

The index is `icacheLinearize(dims)` — row-major Horner over the `instanceId`s,
which must match the AOT GEP's dimension order (`dim0` = leftmost `ejit_dim`).

**D-cache clustering (`-ejit-icache-cluster`).** In the embedded shape each TU
contributes ONE cell table, and the shared section also carries that TU's
toggle-written period arrays plus the MB-scale taskpool blob — so the
probe-hot cell lines sit next to frequently-written neighbors that keep
evicting them, and cells from different TUs scatter across the shared window.
With the flag ON and a non-empty section, the pass instead emits every cell
table into the DERIVED subsection `<EJIT_ICACHE_SECTION>.ejit_icache` (default
`.mc_shared.ejit_icache`): the linker concatenates same-named input sections
in link order — the only mechanism that can group cells across TUs (a
per-module array would degenerate to size 1 under one-entry-per-TU). The
grouping works with or without `-fdata-sections`; an explicit section name is
the grouping key either way. Per-cell alignment stays 8 (a cell never
straddles a 64B line; eight scalar cells pack one line), and line-granularity
isolation is the deployment script's `ALIGN(64)` — the same split as
`.text.ejit_dispatch`. The cells are read-mostly (probes only load; a fill is
once per specialization and a drain writes every cell whatever the layout),
so packing them on shared lines creates no false sharing — it moves them AWAY
from the high-frequency neighbor writes that do. Optional hardening, NOT
built: per-cell 64B alignment on top of clustering would give each cell its
own line (N*64B memory, 8x resident lines) to remove an effect that only
occurs at fill frequency — see ejit_registry.ld for the script entry.

### 3.2 Invalidation

`setInstanceEnabled` calls `icacheDrainAll()` at both ends of the period mutation
window; it zeroes every cell of every registered slot, so a peer's next probe
loads 0. Also driven from `retireDispatchCache()` (`ejit_clear_cache`,
`ejit_invalidate`, compile-mode change, `setReleaser`), `ownerShutdown()`, and
owner init.

It runs on **every** `setInstanceEnabled` call, not only the one that moves the
shared `enabled` bit: N cores bracket their own core-private period writes over
one shared bit, so a caller that lost the CAS may still have rewritten its copy.
`version[]` still moves only on a real transition — its consumer discards an
in-flight compile, and nothing re-enqueues a dropped one.

**Cost, and prefer the bulk entry points.** A drain is O(registered slots ×
`D^numDims`), and only `ejit_activate_all` / `ejit_deactivate_all` batch it —
`setAllInstancesEnabled` does the CAS loop, then one `dispatchEpoch` bump and one
drain. `ejit_activate` / `ejit_deactivate` / `ejit_set_instance_enabled` each go
straight to `setInstanceEnabled`, so bringing 256 instances up one at a time at
startup costs 256 full table walks and 256 epoch bumps, with the in-flight
counter raised throughout (which also blocks concurrent fills). Use the bulk
calls for bring-up; the per-instance ones are for steady-state toggles, where one
drain per toggle is the intended behaviour.

### 3.3 Fill guard

The one non-benign interleaving is a **fill landing after a drain**: the pointer
was resolved pre-toggle, so the cell would come back holding a stale
specialization and nothing would clear it again.

```
taskpool entry   token = icacheBeginResolve()   // drainsInFlight==0 ? seq : invalid
   ... resolve ...
on success       icacheFill(..., token)         // drop unless still 0 / unchanged
```

`icacheDrainAll()` raises `icacheDrainsInFlight` before the first cell store and
lowers it after the `icacheDrainSeq` bump, so the pair brackets the whole drain.
A fill is accepted only if in-flight was 0 at both ends and the sequence never
moved. The counter (not just the sequence) is what stops a fill slipping into a
cell a drain has passed but not yet accounted for, and makes concurrent drains
safe. The token is a **stack value**, not runtime state: on an RTOS a
higher-priority task can preempt a resolve and run its own.

Those checks can only *precede* the store, so they cannot cover it: a drain
beginning after they pass can zero the cell and finish before the store lands,
stranding a pre-toggle specialization nothing clears again — and a preempted
filler makes that window unbounded. So the fill publishes optimistically, then
re-reads `icacheDrainsInFlight` and `icacheDrainSeq` and writes 0 back on
conflict. An overlapping drain has either bumped the sequence or is still in
flight, so both are visible. Retracting discards only this core's own fill —
under P1 no other core writes that cell — and a null cell is always safe.

The probe reads none of this.

### 3.4 Drain accounting across reinitialization

`setInstanceEnabled` does not require Ready, so a peer drain can still be walking
when a new owner claims the blob. A drain therefore stamps itself with the
generation it announced under and retires its increment (via
`ejitIcacheRetireDrain`) only if that generation still stands; the owner clears
the counter only **after** publishing the new generation. Without both, the
straggler's `fetchSub` takes `0 - 1` to `UINT32_MAX`, after which
`icacheBeginResolve()` refuses every token forever and no later drain repairs it.
The retire also saturates at zero.

## 4. Preconditions

**P1 — cores drive disjoint dim identities.** Assumed, not enforced, and the real
cost of this design. The cell index is the dim identity, not the core id, so
"partitioned by core" holds only insofar as the application gives each core its
own `ejit_period_lc` instance indices.

If it holds, no core ever *reads* a cell another core filled: every pointer it
loads is one it put there after resolving through the taskpool, which is where it
did whatever per-core execute preparation the platform needs. That is the
implication a per-core `.bss` table gave structurally, now carried by the
deployment model. If it does not hold, the colliding cores store the same value
so the *result* is right, but under 4K seal or a wired `prepareCodeFn_` the
second core branches into a page it never sealed.

Nothing in the runtime can check it: a cell carries no record of which core wrote
it, and adding one puts a per-core gate back on the probe.

> **Contract:** each core drives its own `ejit_period_lc` instance indices, and
> no two cores call one `ejit_entry` with the same `ejit_dim` values.

**P1a — the 0-dim exception, which IS enforced.** An entry with no `ejit_dim`
params has one cell and no identity to partition it by, so P1 cannot cover it:
core B necessarily reads what core A wrote. For `numDims == 0` `icacheFill`
consults `icacheCrossCoreExecutable()` and declines unless a resolved pointer is
callable everywhere the instant it exists; the cell then stays empty and the
taskpool serves every call. The AOT probe is still emitted — the code is platform
independent, only the fill is not.

What counts as per-core preparation is deliberately narrow: **only a wired
`prepareCodeFn_`** (the legacy whole-2MiB path). 4K-seal mode does not, because
the seal acts on an address space every core translates through — a page sealed
by one core is executable on all of them, and the per-core split the taskpool
performs is bookkeeping over a shared mapping rather than a precondition for the
jump. Counting the seal here left every 0-dim entry permanently unfillable on the
only platform the inline cache ships on, paying the full taskpool path on every
call for a shape that is safe there.

> If a 0-dim entry ever faults on a peer core, this is the assumption to
> re-check first. `EJitSharedPoolSplit` tracks `splitDoneMask` per core, which is
> the runtime modelling the split as per-core state — consistent with per-core
> bookkeeping over a shared mapping, but also with the mapping not being shared
> at all. The board-side check is `ejit_icache_multiverify_test`'s `f_0`
> executing on cores that never resolved it themselves.

For a *dimensioned* entry `icacheFill` does **not** consult that gate. An earlier
revision did, and it made the feature inert on every real target:
`EJitCompileDriver` sets `fourKSeal_` under `EJIT_CODE_POOL_4K_SEAL` and wires
`prepareCodeFn_` otherwise, both inside `EJIT_SRE_SHARED_CODE_POINTERS` which the
probe requires, so the gate was closed in every build the cache was allowed in.

**P2 — placement must match the deployment.** `EJIT_ICACHE_SECTION` must name an
inter-core shared section. This is a link-time contract with no runtime check: the
symbol resolves to the same address either way, so the mistake is undetectable
from code. CMake warns when `EJIT_SRE_SHARED_TASKPOOL + EJIT_FREESTANDING` is
configured with an empty section. See §8.

With `-ejit-icache-cluster` P2 gains a second, script-shape-dependent clause:
the derived `<section>.ejit_icache` name must ALSO be mapped into the shared
region. An exact-match script (`*(.mc_shared)`) leaves the derived fragments as
orphans — lld's longest-name-prefix heuristic usually lands them adjacent to
`.mc_shared`, but the region assignment is not contractual; a globbing script
(`*(.mc_shared*)`) swallows the name back into the shared section INTERLEAVED,
and the feature silently no-ops. Both shapes degrade without any diagnostic;
the only detector is the link-time check in §8.

**P3 — `ejit_dim` values stay in `[0, D)`.** The accepted ranges never agreed: the
taskpool takes instance ids up to 256 and a period array may declare up to 100
entries, against a `D` of 16. The probe indexes with the raw argument, so an id of
16 would read past the wrapper's global into its neighbour in `.mc_shared`.

`EJitWrapperGen` emits the probe only when the declared period-array size proves
the bound, and `icacheFill` re-checks. A period may own several arrays activated
as a group, so the proof uses the **maximum** over them — keeping whichever global
the module listed last made the answer depend on declaration order, and could
approve a 16-cell probe for a lifecycle producing identity 31. A declared count of
0 means "not stated", which is unprovable rather than small.

## 5. Reclamation

In production JIT code is never freed (`EJIT_SRE_TASKPOOL_NO_RECLAIM`, bump
allocator over sealed RX pools, no `setReleaser` on the shared pool), so a cached
pointer cannot dangle — including one a probe loaded an instruction before a drain
zeroed its cell.

If a releaser *is* wired, the cache disables outright: `icacheFill` no-ops,
`icacheTry` misses, and the table is drained so already-armed cells stop serving.

That state lives in the **blob** (`icacheReleasersWired`), not in the facade that
wired the releaser. One table backs every core and the probe consults no gate, so
a core-private flag let a peer facade refill what the owner had just drained,
after which the owner could reclaim code another core was still calling. It is a
count, not a flag, because facades wire and unwire independently: the cache
re-arms only when the last releaser is gone. The same reasoning applies to the
platform property behind P1a, published as `icachePerCorePrepare`.

Draining is not reclamation. A core can load a pointer microseconds before a drain
and still be executing that code when it is freed; erasing the cell says nothing
about callers in flight. Correct reclamation needs quiescence (RCU grace period,
or hazard pointers published by the probe), and the probe cannot publish one
without ceasing to be four instructions. Hence: never free, and if something ever
can, turn the cache off.

## 6. Races

| Race | Outcome |
|---|---|
| Drain stores 0 while a probe loads | Aligned word, both sides atomic: probe reads the old pointer (one more call, misses next) or 0. Never torn. |
| Probe loads, *then* the toggle lands, then it calls | Inherent to lock-free dispatch — the caller had committed. A freshness check just moves the window. |
| Fill lands after a drain | The unsafe one. Closed by the in-flight/sequence token (§3.3). |
| Two cores drain at once | Both store 0; the in-flight counter (not a parity bit) keeps overlapping drains from looking finished. |
| Two cores fill the same cell | Same `fnPtr` — the bucket cache compiles each identity once — so the store is idempotent. Costs a shared line, not correctness. |
| Straggler drain vs. new owner | §3.4. |
| Task preempts a resolve and resolves too | Token is a stack value; the outer resolve keeps its own. |
| Registration racing a drain | Registration is at init, before activation; `registrationFrozen()` rejects it after. |

## 7. Configuration and API

| Flag / define | Default | Effect |
|---|---|---|
| `-ejit-inline-cache` | off | emit the probe |
| `EJIT_ICACHE_SECTION` (`-mllvm -ejit-icache-section=` overrides) | `.mc_shared` | section for the cell table; `""` = per-core `.bss` |
| `-mllvm -ejit-icache-cluster` | off | emit the cell tables into the derived `<section>.ejit_icache` so the linker concatenates them into one D-cache-contiguous region; requires a matching deployment-script entry (P2); no-op with an empty section |
| `EJIT_ICACHE_DIM_SIZE` | 16 | per-dim bound D |
| `EJIT_ICACHE_MAX_DIMS` | 4 | max cached dims |
| `EJIT_ICACHE_FUNC_SLOTS` | 4096 | registration table size (must be ≥ `EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX`) |

Memory: `D^numDims * 8` bytes per cached function, allocated **once** rather than
per core. An explicit section makes the table `PROGBITS`, not `.bss` `NOBITS`, so
it costs those zero bytes in the image.

```cpp
constexpr uint64_t kEJitIcacheNoResolve    = 0;
constexpr uint64_t kEJitIcacheResolveValid = uint64_t{1} << 32;
enum class EJitIcacheRegResult { Ok, CapacityMiss, Invalid };

EJitIcacheRegResult ejitIcacheRegisterSlot(uint32_t funcIndex, void *base,
                                           uint32_t numDims);
void ejitIcacheClearAll();   // UNregister; never dereferences a base
void ejitIcacheRetireDrain(EJitSharedTaskPoolState *st, uint32_t gen);

// EJitSharedTaskPool members
uint64_t icacheBeginResolve();
void     icacheFill(uint32_t funcIndex, void *fnPtr, const EJitDimPair *dims,
                    uint32_t numDims, uint64_t token);
void     icacheDrainAll();
bool     icacheCrossCoreExecutable() const;   // 0-dim fills + taskpool path
bool     icacheReclamationSafeShared() const; // this facade's releaser AND peers'
bool     icacheTry(...);                      // tests/diagnostics only

// C ABI, called by AOT auto-registration on every core
void ejit_register_icache_slot(const char *funcName, void *slot,
                               uint32_t numDims);
```

Registration declines an out-of-range `funcIndex`, a null base, or `numDims`
above the cap (it sizes the array the drain walks); the cell stays null and the
taskpool serves that function. Shared ABI v7 → v8.

## 8. Verifying on a real target

Nothing in the runtime can prove the cells are shared (P2), so check placement at
link time and behaviour at run time.

```sh
readelf -sW app.o   | grep __ejit_icache_fn_   # which section index
readelf -SW app.o   | grep mc_shared           # is that index .mc_shared?
readelf -SW image.elf | grep mc_shared         # linked address + size
nm image.elf | grep __ejit_icache_fn_          # must fall inside that range
objdump -d image.elf --disassemble=<entry> | head -8   # adrp/add/ldr/cbz/br, no bl
# with -ejit-icache-cluster, additionally:
readelf -SW image.elf | grep ejit_icache      # ONE output section, addr % 64 == 0,
                                              # inside the shared window
nm image.elf | grep __ejit_icache_fn_         # consecutive addresses inside it
```

The linked-address check is the one the code cannot do for you: land outside the
shared window and the cache silently degrades to per-core.

Cluster script fix-up, by current shape — an entry is required either way:

```ld
# exact-match script (*(.mc_shared)) — add:
.mc_shared.ejit_icache : ALIGN(64)
{
  KEEP(*(.mc_shared.ejit_icache))
} > <the same inter-core shared region .mc_shared maps into>

# globbing script (*(.mc_shared*)) — the glob swallows the derived name back
# unclustered, so split it first, then add the same entry:
#   *(.mc_shared)                                  # everything else
#   .mc_shared.ejit_icache : ALIGN(64) { KEEP(*(.mc_shared.ejit_icache)) } > <shared region>
```

At run time, `-mllvm -ejit-wrapper-timing` prints one line per 100000 icache hits
(`status=254` is the hit sentinel); or watch `ejit_print_stats` — once warm,
`cacheHits` stops growing while the application keeps calling.

To prove it is *shared* rather than merely warm (a per-core table hits too):
warm a cell on core A, toggle `ejit_deactivate`/`ejit_activate` on **core B only**,
then re-read core A's cell — it must be 0, and core A must take exactly one extra
miss. Comparing `&__ejit_icache_fn_foo` across cores proves nothing: per-core
`.bss` at the same virtual address gives equal pointers to different storage.

## 9. Tests

`EJitSharedTaskPoolTest.cpp`:

| Test | Pins |
|---|---|
| `InlineCacheFillAndServe` | fill → serve, re-fill, range guard |
| `InlineCacheHighFuncIndex` | slots across the whole dense funcIndex space |
| `InlineCacheDrainsOnPeriodToggle` | activate **and** deactivate empty the cell |
| `InlineCacheDrainReachesEveryCorePartition` | **the core claim**: cores 1 and 2 fill disjoint cells, a toggle on core 3 clears both |
| `InlineCacheDrainsEvenWhenTheEnableBitDoesNotMove` | lost CAS still drains; `version[]` does not move |
| `InlineCacheDropsAFillThatRacedADrain` | stale token dropped, fresh token fills |
| `InlineCacheFillRequiresAValidResolveToken` | `kEJitIcacheNoResolve` declined |
| `InlineCacheDrainsOnRetireDispatchCache` / `...OnOwnerShutdown` | the other drain sites |
| `InlineCacheRegistrationRejectsAnOverCapShape` | `numDims` cap, null base, OOR funcIndex |
| `InlineCacheFillsWhenCoresPrepareIndividually` | **P1**: a dimensioned entry still fills under `prepareCodeFn_` and 4K seal |
| `InlineCacheDeclinesScalarFillWhenCoresPrepareIndividually` | **P1a**: core A cannot arm a 0-dim cell core B has not prepared; still fills where the platform allows it |
| `InlineCacheAutoDisablesWhenReclamationWired` | releaser safety gate |
| `InlineCacheReclamationGateIsSharedAcrossFacades` | **§5**: a peer facade cannot re-arm a table the owner disabled |
| `ReinitCannotUnderflowAStragglerDrainCounter` | **§3.4**: the old-drain/new-owner interleaving |
| `InlineCacheMultiVersion` | per-identity cells; drain clears the whole `[D][D]` table |

`ejit-wrapper-gen-icache.ll` — `SECTION` prefix asserts the tables land in
`.mc_shared` and the probe is unchanged; every RUN pins
`-ejit-icache-section=` so the test does not depend on the CMake default.
`ejit-wrapper-gen-icache-period-arr-max.ll` — **P3**: both declaration orders of a
two-array period decline the probe, while a period whose arrays all fit keeps it.
