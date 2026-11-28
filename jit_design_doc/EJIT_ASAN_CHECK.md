# EJIT-ASAN: live substitution and annotation checking

EJIT-ASAN checks, at runtime, that the constants the JIT baked into
specialized code still hold — and that the fields whose values it was
told to trust really are trustworthy. It is a read-only diagnostic:
nothing gates on it and no specialized code is diverted — evidence is
recorded and reported, the call always continues with its baked values
(folded checks) or its real values (drift observations).

It has three complementary parts:

- **The worker probe** re-reads, on the shared worker's idle rounds, the
  shared-RAM locations whose values the fold actually baked, comparing
  them against the constants in the JIT code. It watches the
  compile-time snapshot of every fold — including the exemplar pointee
  of a bound_ptr fold — but it can only ever see the addresses captured
  at compile time.
- **The entry stub** answers the question the probe structurally cannot:
  whether the pointer a call passes *now* still honors the bound_ptr
  promise. A `bound_ptr` parameter is a promise that certain
  period_const members are CONSISTENT across every pointer the product
  may pass for the specialization's lifetime — different pointers, same
  folded fields, no recompile on pointer change. Only the entry of the
  specialized function can see the current pointer, so the stub loads
  the folded fields through the ACTUAL argument and compares them with
  the baked constants on a rotating sample of calls.
- **The drift observer** (the gen-2 extension) watches the unfolded
  half: annotated fields that SURVIVED the specialization — their
  annotation claims "constant within the window", yet nothing ever
  verified the claim. The same merged entry stub observes the ones
  reached through a bound_ptr argument; the worker's full-scope scan
  samples every annotated field of every registered object,
  compile-independent and version-independent. A value that moves is
  not a fault — it is evidence the annotation does not hold.

All parts report through the same dump; rows carry their source
(`kind=`, `src=`).

## Configure and enable

```text
-DEJIT_ASAN=ON            (requires EJIT_DIAG_ENABLE; defaults OFF)
```

| CMake knob | default | meaning |
|---|---|---|
| `EJIT_ASAN_IDLE_TICKS` | 5000 | worker-probe: one probe slice every Nth worker idle round (~5 s at 1 tick ~ 1 ms) |
| `EJIT_ASAN_STEP_BUDGET` | 32 | worker-probe: max probe/full-scope steps per slice |
| `EJIT_ASAN_YIELD_EVERY` | 8 | worker-probe: in-slice yield cadence |
| `EJIT_ASAN_MAX_WATCHES` | 81920 | worker-probe: global watch budget; overrun trims the NEWEST capture |
| `EJIT_ASAN_MAX_VERSIONS` | 1024 | worker-probe: simultaneous tracked-version budget |
| `EJIT_ASAN_ROTATION_WINDOW` | 128 | entry stub: calls per bound-argument sub-window before the gate advances |
| `EJIT_ASAN_MAX_SLOTS` | 64 | entry stub: rotation function-slot table capacity |
| `EJIT_ASAN_MAX_CORES` | 64 | entry stub: per-core rotation cell capacity (in [1, 256]; cores above it never check) |
| `EJIT_ASAN_MAX_SITES` | 512 | entry stub: emitted-site budget (folded + drift share it); monotonic per boot, overrun drops the NEWEST capture with a log |
| `EJIT_ASAN_MAX_FULLSITES` | 4096 | full-scope drift watch: registered field-instance budget, merged per boot, dedup by address; overrun drops the capture's remaining sites with a log |

The aarch64_be preset pins all of the above. The build knob compiles the
feature in; USE is a single runtime opt-in — all three parts (worker
probe, full-scope drift scan, entry stubs) activate together:

```c
ejit_init_asan(0);         // instead of ejit_init: probe + full-scope
                           // scan + entry stubs, all for the boot
```

`ejit_init_asan()` takes the same config as `ejit_init` and forces the
feature on for the boot (emitted stubs stay in their versions); PGO stays
off — the ASAN test mode runs the plain pipeline. The shared-pool parts
(worker probe, full-scope scan) need the Async compile mode; with
`EJIT_COMPILE_OFF` nothing activates. A call on an already-initialized
runtime is a silent no-op that enables nothing. Under plain
`ejit_init()` the feature stays compiled-in but inert: no probe slices,
no scan, no stubs, and `ejit_asan_dump()` returns `EJIT_ERR_NOT_ACTIVE`.
A build without the feature returns `EJIT_ERR_DISABLED` without
initializing.

Business code needs no new Clang flag — normal spec5 annotations
(`ejit_entry`, `ejit_dim`, `ejit_bound_ptr`, `ejit_period_const`) suffice.

## Worker probe

Unchanged from gen-1: one bounded probe slice every
`EJIT_ASAN_IDLE_TICKS` idle rounds; a mismatching probe only bumps the
location's sticky record (the first mismatch freezes the seen value;
later mismatches only grow saturating counters); normal scanning never
prints. Every fold the capture could not watch counts into `uncovered`;
dropped or trimmed captures log at compile time (`capture DROPPED` /
`capture TRIMMED`, plus `capture DROPPED: site budget full` /
`full-scope site budget full` for the two site budgets).

The gen-1 capture bug is fixed here: the domain walk now propagates
`AllowArgs` through GEP chains, so constant-offset bound folds capture
(was: every nonzero-offset bound fold landed `uncovered:
unsupported-index-domain`).

## Entry stub and drift observer

**Emission.** At worker compile, bound folds with a fully constant offset
(`modulus == 1`) are recorded — folded ones from the substitution, drift
ones from the final-module walk below. When the mode is enabled, the very
END of the optimization pipeline — after the last StructFieldPass run
and after the audit snapshot — emits at the specialized function's entry
one merged stub:

```text
head:   %sub = call i32 @__ejit_asan_gate(i32 %slot)
        switch %sub, label %cont [ i32 0, label %check0 ... ]
checkK: the K-th bound argument (ascending rank):
        folded fields: load through the CURRENT bound argument, compare
                       with the baked constant; mismatch -> hook -> rejoin
        drift fields:  load (Acquire when the source load was atomic),
                       report the value through the hook, rejoin
cont:   <original body>
```

The stub resolves `__ejit_asan_gate`/`__ejit_asan_mismatch` through the
same absolute-symbol path as the value-profiling hooks (registered by
`ejit_init`; same-image/same-VA cross-core precondition).

**Sub-rotation.** One gate call per instrumented entry; each activated
call checks exactly ONE bound argument of the active function (product
rule: a function may mark several bound_ptrs, but no call pays for more
than one). The gate returns the sub-window index or -1; the window spans
`bound x EJIT_ASAN_ROTATION_WINDOW` matching calls, where `bound` is the
slot's registered sub-window count (the distinct bound arguments its
stubs ever instrumented — a monotonic max written by the emitter). The
armed window SNAPSHOTS the count: a version registering more arguments
affects the next arming on any core; a stale stub whose rank set shrank
falls through the switch default and checks nothing. At most one
function slot is active per core; each core rotates independently; v1
has no dead-slot skip and no tick backstop (a resting rotation is
visible via `ejit_asan_rotation_status()`); cores beyond
`EJIT_ASAN_MAX_CORES` are safely degraded.

**The drift walk.** At both final-code exits, before stub emission (the
stub's own check loads match the metadata-free bound-field fallback and
must not be re-recorded), a fresh pass instance walks the final module:

- A **bound-anchored surviving load** with a constant offset inside a
  listed bound field becomes a drift site: the stub observes the field
  through the current argument on every activated call, and the value
  the compile saw is published as its baseline. Clang cannot attach
  `!ejit.period_const` to an atomic load — an annotated field read with
  atomic semantics (`__atomic_load_n` in source) therefore reaches the
  final module unmarked; the walk recognizes it through the bound
  metadata's field ranges, the same authority the substitution fallback
  trusts. Residue-driven survivors (`modulus > 1`) get no site in v1.
- **Global-anchored survivors** cannot be observed at an entry (only
  bound arguments are visible there); they count into `drift_uncovered`
  with a reason — the replacement-failure taxonomy (`no-root-gv`,
  `gv-not-in-map`, `base-unresolved`, `non-const-offset`,
  `witness-out-of-bounds`, `unsupported-type`, `atomic`) plus
  `offset-not-in-field-metadata`, the annotation-consistency smell of a
  marker on a field the GV's own metadata does not list. Volatile
  annotated loads stay out entirely (an observation load of our own
  would change the access pattern the program promised).
- **Full-scope enumeration**: every registered object's annotated fields
  (GV metadata, including nested struct members and expanded array
  slots; unions and bitfields are excluded at the frontend) are
  enumerated per walk — element-relative offset, DL-derived width
  (1/8/16/32/64-bit scalars only), registered instance count
  cross-checked against the IR array length — with the registered base
  resolved so instance addresses are `registryBase + instance * stride
  + offset`.

**Hook dispatch.** The mismatch hook keeps its historical name and
signature; the site's kind dispatches the protocol:

- A **folded** site records a MISMATCH: the first divergence freezes the
  seen value and the argument pointer (the family member that broke the
  promise); later mismatches only grow a saturating count.
- A **drift** site records an OBSERVATION: every activated call counts a
  sample; a value that differs from the last-seen baseline transfers
  `lastSeen` (a bounded 4-round CAS — under extreme multi-core flutter
  the retry gives up and under-counts rather than spins, a documented
  approximation) and counts one change; the first changed value and the
  diverging call's pointer freeze through the same CAS-elected single
  writer. An A→B→A flip entirely between two observations is physically
  invisible at any sampling rate.

**The full-scope scan.** The worker merges every compile's enumeration
into one boot-lifetime table, deduplicated by instance address — it is
independent of versions on purpose: retiring a version and resetting
these counters would destroy exactly the long-run evidence the tier
exists to collect. Each scan slice runs a second cursor over the table
with its own step budget: folded-marked sites (an address some
version's folded watch already covered — back-marked at merge, sticky
through retirement, a documented blind spot) are skipped; the rest are
sampled with value transfers recorded. At the default 32 steps per ~5 s
slice a full 4096-site round takes ~11 minutes — the cadence scales
inversely with the annotated-field count.

## Output model

`ejit_asan_dump()` prints the report NOW, synchronously in the caller's
context (owner/worker core only): one scan slice, one row per recorded
location, then the stats line, each paced with fixed 20-tick yields; a
second call while printing returns `EJIT_ERR_NOT_ACTIVE`.

**MISMATCH rows** (folded checks — faults; all fields at INFO):

```text
[EJIT-ASAN] MISMATCH tier=0 gen=1 dims=... period=asan_cell fn=ejit_asan_probe
  object=gEJitAsanCells@0x82f40000 offset=0x14 src=arg1 residue=4/5 kind=value
  expected=0x7 first=0x11 count=7/7 rate=100.0%          <- worker probe

[EJIT-ASAN] MISMATCH tier=0 gen=3 period=asan_cell fn=ejit_asan_bound_probe
  object=bound@0x82f50020 offset=0x0 src=bound1 kind=entry
  expected=0x7 first=0x9 count=12                        <- entry stub
```

**DRIFT rows** (evidence, not faults). A changed site prints its core
row at INFO plus a forensic detail line at VERBOSE; a never-changed
site prints at VERBOSE only (the stats line carries the aggregate):

```text
[EJIT-ASAN] DRIFT period=asan_cell fn=ejit_asan_bound_probe
  object=bound@0x82f50020 offset=0x4 src=bound1 kind=drift initial=0x0
  last=0x3 changes=1 samples=512                       <- entry stub
[EJIT-ASAN] DRIFT-DETAIL fn=ejit_asan_bound_probe offset=0x4 first=0x3
  tier=0 gen=3

[EJIT-ASAN] DRIFT period=asan_cell object=gEJitAsanCells@0x82f40000
  offset=0x8 instance=0/2 src=metadata kind=drift initial=0x7 last=0x17
  changes=1 samples=40                                   <- full-scope scan
[EJIT-ASAN] DRIFT-DETAIL object=gEJitAsanCells@0x82f40000 offset=0x8
  instance=0/2 first=0x17
```

`object=bound@<arg>` on a stub row names the family member that moved
(or diverged); `instance=i/n` on a full-scope row pins the registered
array instance. Offsets map to source members offline via DWARF on a
`-g` image (`llvm-dwarfdump --name=<symbol> board.elf`).

Stats line: `versions`, `staged`, `watches=<n>/<cap>` (FULL marker),
`checked_active`, `checks`, `mismatch_samples`, `rate`, `recorded`,
`uncovered`, `dropped`, `blocked_root`, `retired`, `entry_checks`
(rotation volume served by the per-core cells), `entry_folded_sites`,
`entry_drift_sites` (sites with a frozen record, by kind),
`entry_drift_checks` (drift observations), `fullscope_sites=<n>/<cap>`
(FULL marker), `fullscope_folded`, `fullscope_changed`,
`fullscope_samples`, `drift_uncovered`, `drift_dropped`.
`checks=0` is not a pass.

`ejit_asan_rotation_status()`: read-only, any core, any time. The header
and per-core lines (INFO) answer "whose window is it and which argument
is next"; the per-slot lines are audit depth and print at VERBOSE only:

```text
[EJIT-ASAN] rotation slots=2/64 sites=5 drift_sites=1
[EJIT-ASAN] rotation slot=1(ejit_asan_bound_probe) bound=1 sites=2    <- VERBOSE
[EJIT-ASAN] rotation core=16 slot=1(ejit_asan_bound_probe) sub=0 bound=1
  window=37/128 cycles=12 checks=1543
```

## Coverage and contract

Folded-check coverage: scalar integer/pointer folds with a registered
source, constant offsets, one fully enumerable unsigned remainder
(residues <= 16), pointer roots double-checked, bound exemplars probed
at their frozen snapshot address; bound folds with a constant offset
additionally checked at the entry against the current argument.

Drift coverage: bound-anchored unfolded annotated loads with a constant
offset (widths 1/8/16/32/64) are observed by the stub; every annotated
field of every registered object is sampled by the full-scope scan.
Not covered: volatile annotated loads; residue-driven unfolded bound
addresses (v1); annotated fields wider than 64 bits or non-scalar;
registered objects whose base cannot be resolved at walk time;
bound-anchored annotated loads owned by a non-entry function (a callee
formal — no stub observes them and no worker watch exists; they surface
only as VERBOSE drift lines); and the retired-folded blind spot (a
full-scope site marked folded stays skipped after its version retires).
Clang's frontend excludes unions, bitfields and volatile fields from the
annotation, and nested struct members ARE enumerated (element-relative
offsets) and resolved (recursive layout descent).

The product contract for bound pointers is unchanged (heap memory,
lifetime spanning the version, updates coherent across cores) plus the
family rule the stub enforces empirically. Report-only means a family
break is OBSERVED, not corrected — a persistent `kind=entry` row is a
live wrong-answer bug in the business results until the product
restores consistency (or recompiles); a `kind=drift` row is the
annotation-truthfulness signal, and the observed value itself is what
the business computes with.

## Board smoke

Link only `ejit_test/ejit_asan_sre_test.c` with normal spec5 build flags
(board-only demo; not enrolled in `build.sh ALL_TESTS`). The EJIT-ASAN
runtime entry symbols are lipo GC roots (`ejit_asan_dump`,
`ejit_init_asan`, `ejit_asan_rotation_status`,
`__ejit_asan_gate`, `__ejit_asan_mismatch`) — verify the trimmed image
still defines them (`nm -g board.elf | grep ejit_asan`). No new external
symbols were added by the drift observer.

1. Core 6: `test_ejit_asan_start` — initializes with the entry mode
   (`ejit_init_asan`; PGO off).
2. Core 16: `test_ejit_asan_sample` — worker-probe demo (waits for the
   global probe's compile; full coverage of the demo folds).
3. Core 6: `ejit_asan_dump` — stats line shows full probe coverage, zero
   recorded rows, `entry_checks=0`, and the full-scope table seeded
   (`fullscope_sites=30/4096`, `fullscope_folded=15`: the probe's folded
   watch covers cell 1's 15 annotated field slots; cell 0's 15 are
   scanned).
4. Core 16: `test_ejit_asan_bound_sample` — compiles the bound probe
   from the family exemplar (FamilyA, scale=7, probeBias=0).
5. Core 16: `test_ejit_asan_bound_inject` — 512 calls with the divergent
   member FamilyB. Every call returns the BAKED computation (7·input+1),
   the wrong-answer symptom itself.
6. Core 16: `test_ejit_asan_drift_inject` — moves FamilyB's probeBias
   (read with atomic semantics, never folded) to 3 and calls 512 times:
   every call now returns the REAL value (7i+1+3).
7. Core 16: `test_ejit_asan_drift_fullscope_inject` — changes cell 0's
   `slot5[2].value` (covered by no folded watch) to 23.
8. Core 6: `ejit_asan_dump` (after ~10 s, two scan slices) — exactly one
   `kind=entry` row (FamilyB, `expected=0x7 first=0x9`), one stub
   `kind=drift` row (`object=bound@<FamilyB> offset=0x4 initial=0x0
   last=0x3 changes=1` — every earlier bound-sample call already counted
   its samples), one `src=metadata` row
   (`object=gEJitAsanCells@... offset=0x8 instance=0/2 initial=0x0
   last=0x17 changes=1` — cell 0 is zero at merge time), and the stats
   reflecting
   `entry_drift_sites=1 entry_drift_checks>0 fullscope_changed=1`.
9. Any core: `ejit_asan_rotation_status` — header with `drift_sites=1`,
   per-core `sub=/bound=/window=` lines at INFO; per-slot lines at
   VERBOSE.

Reboot before repeating; the injection commands are diagnostic-only.
