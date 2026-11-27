# EJIT-ASAN: live substitution checking

EJIT-ASAN checks, at runtime, that the constants the JIT baked into
specialized code still hold. It is a read-only diagnostic: nothing gates on
it and no specialized code is diverted on a mismatch — a mismatch is
recorded and reported, the call always continues with its baked values.

It has two complementary halves:

- **The worker probe** re-reads, on the shared worker's own idle rounds, the
  shared-RAM locations whose values the fold actually baked, comparing them
  against the constants in the JIT code. It watches the compile-time
  snapshot of every fold — including the exemplar pointee of a bound_ptr
  fold — but it can only ever see the addresses captured at compile time.
- **The entry stub** (the bound_ptr half) answers the question the probe
  structurally cannot: whether the pointer a call passes *now* still honors
  the bound_ptr promise. A `bound_ptr` parameter is a promise that certain
  period_const members are CONSISTENT across every pointer the product may
  pass for the specialization's lifetime — different pointers, same folded
  fields, no recompile on pointer change. Only the entry of the specialized
  function can see the current pointer, so the stub loads the folded
  fields through the ACTUAL argument and compares them with the baked
  constants on a rotating sample of calls. The first divergence freezes
  both the seen value and the argument pointer — the family member that
  broke the invariant — and the row names it directly.

Both halves report through the same dump; rows carry their source (`kind=`).

## Configure and enable

```text
-DEJIT_ASAN=ON            (requires EJIT_DIAG_ENABLE; defaults OFF)
```

| CMake knob | default | meaning |
|---|---|---|
| `EJIT_ASAN_IDLE_TICKS` | 5000 | worker-probe: one probe slice every Nth worker idle round (~5 s at 1 tick ~ 1 ms) |
| `EJIT_ASAN_STEP_BUDGET` | 32 | worker-probe: max probe/retirement steps per slice |
| `EJIT_ASAN_YIELD_EVERY` | 8 | worker-probe: in-slice yield cadence |
| `EJIT_ASAN_MAX_WATCHES` | 81920 | worker-probe: global watch budget; overrun trims the NEWEST capture |
| `EJIT_ASAN_MAX_VERSIONS` | 1024 | worker-probe: simultaneous tracked-version budget |
| `EJIT_ASAN_ROTATION_WINDOW` | 128 | entry stub: calls per function window before rotating |
| `EJIT_ASAN_MAX_SLOTS` | 64 | entry stub: rotation function-slot table capacity |
| `EJIT_ASAN_MAX_CORES` | 64 | entry stub: per-core rotation cell capacity (in [1, 256]; cores above it never check) |
| `EJIT_ASAN_MAX_SITES` | 512 | entry stub: emitted-site budget; monotonic per boot, overrun drops the NEWEST capture with a log |

The aarch64_be preset pins all of the above. The worker probe needs no
runtime opt-in; the entry stub does:

```c
ejit_enable_asan_mode();   // BEFORE ejit_init; every later worker compile
                           // of a function with bound folds emits the stub
```

`ejit_enable_asan_mode()` must be called before `ejit_init`: after init it
logs an error and returns `EJIT_ERR_INVALID_PARAM`; without the feature it
returns `EJIT_ERR_DISABLED`. It is irreversible for the boot: emitted
stubs stay in their versions.

Business code needs no new Clang flag — normal spec5 annotations
(`ejit_entry`, `ejit_dim`, `ejit_bound_ptr`, `ejit_period_const`) suffice.

## Worker probe

Pacing and mechanics are unchanged from the original feature: one bounded
probe slice every `EJIT_ASAN_IDLE_TICKS` idle rounds; a mismatching probe
only bumps the location's sticky record (the first mismatch freezes the
seen value; later mismatches only grow saturating counters); normal
scanning never prints. Every fold the capture could not watch counts into
`uncovered`; dropped or trimmed captures log at compile time
(`capture DROPPED` / `capture TRIMMED`, plus `capture DROPPED: site
budget full` for the entry-stub budget).

One capture bug is fixed in this version: the domain walk previously
dropped its AllowArgs flag while walking GEP chains, so every bound fold
at a nonzero offset was reported `uncovered: unsupported-index-domain`
instead of being watched. Only bare offset-0 bound loads ever captured;
all constant-offset bound folds now capture.

## Entry stub

**Emission.** At worker compile, a bound fold whose byte offset is fully
constant (`modulus == 1`) is recorded in the compile's capture regardless
of the mode. When the mode is enabled, the very END of the optimization
pipeline — after the last StructFieldPass run and after the audit
snapshot, so no later fold pass can recognize the check loads as
period_const and re-fold them into the constants they compare against —
emits at the specialized function's entry:

```text
head:   %active = call i1 @__ejit_asan_gate(i32 %slot)
        br %active, check, cont
check:  per folded field: load <field> through the CURRENT bound argument;
        compare with the baked constant
fail:   call @__ejit_asan_mismatch(i32 %site, ptr %arg, i64 %value)
        br cont                        -- report-only: the call continues
cont:   <original body>
```

The stub resolves `__ejit_asan_gate`/`__ejit_asan_mismatch` through the
same absolute-symbol path as the value-profiling hooks (registered by
`ejit_init`; same-image/same-VA cross-core precondition). Residue-driven
bound folds (`modulus > 1` — a free-dim index in the address) get NO
entry stub in v1: the stub would have to recompute the index per call;
those folds keep their full worker-probe coverage instead. Functions with
no bound folds emit nothing; the dormant cost of an instrumented entry is
one gate call.

**Rotation.** One shared rotation state (`gEJitAsanState`, inter-core
shared memory; an independent blob — the taskpool state and its ABI are
untouched): a function-slot table claimed by the emitting worker (a slot
is never freed; a dead slot — no published sites — may only be renamed
once the table is full), and one cell per core. At most ONE function slot
is active per core at a time: `EJIT_ASAN_ROTATION_WINDOW` consecutive
calls of the active function are checked, then the next gate call
advances to the next slot. Each core rotates independently (a cold core
stalls only its own rotation). By product decision v1 has NO dead-slot
skip and NO tick backstop: if a slot with no live versions (or a
function nobody calls) is reached, that core's rotation rests on it
until some instrumented call arrives; `ejit_asan_rotation_status()` makes
the stall visible. The gate reads only its own core's cell — plain loads
and stores, no atomics on the hot path — and cores beyond
`EJIT_ASAN_MAX_CORES` are safely degraded (never check).

**Records.** The mismatch hook freezes, on the FIRST divergence per site,
the seen value and the actual argument pointer (a CAS elects the single
freezing writer; losers only grow a saturating count). Entry rows carry
no rate: counting every executed check would put an atomic in the hot
stub; the per-core `checks` counter covers the volume and is reported in
the stats. Sites are monotonic per boot (version retirement does not
reclaim them); when the site budget is full the newest capture is dropped
with a discoverable log and its folds keep the worker-probe coverage.

## Output model

`ejit_asan_dump()` prints the report NOW, synchronously in the caller's
context (owner/worker core only): one probe slice, one row per recorded
location, then the stats line, each paced with fixed 20-tick yields; a
second call while printing returns `EJIT_ERR_NOT_ACTIVE`.

Worker-probe row (unchanged semantics; `kind` is `value`, `pointer-root`,
or `bound`):

```text
[EJIT-ASAN] MISMATCH tier=0 gen=1 dims=... period=asan_cell
  fn=ejit_asan_probe object=gEJitAsanCells@0x82f40000 offset=0x14 src=arg1
  residue=4/5 kind=value expected=0x7 first=0x11 count=7/7 rate=100.0%
```

Entry row (new; `kind=entry`; `object=bound@<arg>` names the diverging
family member; `src=boundN` names the bound-pointer argument; no
residue/rate by design):

```text
[EJIT-ASAN] MISMATCH tier=0 gen=3 period=asan_cell fn=ejit_asan_bound_probe
  object=bound@0x82f50020 offset=0x0 src=bound1 kind=entry
  expected=0x7 first=0x9 count=N
```

Stats line: `versions`, `staged`, `watches=<n>/<cap>` (FULL marker),
`checked_active`, `checks`, `mismatch_samples`, `rate`, `recorded`,
`uncovered`, `dropped`, `blocked_root`, `retired`, plus `entry_checks`
(per-core served checks, summed) and `entry_sites` (sites with a frozen
record). `checks=0` is not a pass.

`ejit_asan_rotation_status()`: read-only, any core, any time. Prints the
claimed-slot count and site count, then one line per core that ever served
a check — slot (with function name), window progress, completed
rotations, checks served:

```text
[EJIT-ASAN] rotation slots=2/64 sites=5
[EJIT-ASAN] rotation core=16 slot=1(ejit_asan_bound_probe) window=37/128 cycles=12 checks=1543
```

### Mapping offsets to source members

Unchanged: embedded bitcode carries IR-level layouts only, so resolve
`object=...@0x... offset=0x14` offline via DWARF on a `-g` image
(`llvm-dwarfdump --name=<symbol> board.elf`). This stays offline tooling.

## Coverage and contract

Worker-probe coverage: scalar integer/pointer folds with a registered
source, constant offsets, one fully enumerable unsigned remainder
(residues <= 16), pointer roots double-checked, bound exemplars probed at
their frozen snapshot address.

Entry-stub coverage: bound folds with a fully constant offset inside the
pointee (width 1/8/16/32/64). The typed IR load makes endianness the
codegen's job — no manual byte-order handling. Not covered by the stub
(v1): residue-driven bound addresses, non-bound folds, and anything whose
pointee is not live shared memory.

The product contract for bound pointers is unchanged (heap memory,
lifetime spanning the version, updates coherent across cores) plus the
family rule the stub enforces empirically: all pointers passed during a
version's lifetime must agree on the folded members. Report-only means a
family break is OBSERVED, not corrected — the call still uses the baked
constant, so a persistent `kind=entry` row is a live wrong-answer bug in
the business results until the product restores consistency (or
recompiles).

## Board smoke

Link only `ejit_test/ejit_asan_sre_test.c` with normal spec5 build flags
(board-only demo; not enrolled in `build.sh ALL_TESTS`). The new EJIT-ASAN
runtime entry symbols are lipo GC roots (`ejit_asan_dump`,
`ejit_enable_asan_mode`, `ejit_asan_rotation_status`,
`__ejit_asan_gate`, `__ejit_asan_mismatch`) — verify the trimmed image
still defines them (`nm -g board.elf | grep ejit_asan`).

1. Core 6: `test_ejit_asan_start` — enables the entry mode (before
   `ejit_init`) and initializes.
2. Core 16: `test_ejit_asan_sample` — worker-probe demo (waits for the
   global probe's compile; full coverage of the demo folds).
3. Core 6: `ejit_asan_dump` — stats line shows full probe coverage, zero
   recorded rows, `entry_checks=0`.
4. Core 16: `test_ejit_asan_bound_sample` — compiles the bound probe from
   the family exemplar (FamilyA, scale=7).
5. Core 16: `test_ejit_asan_bound_inject` — 512 calls with the divergent
   member FamilyB. Every call returns the BAKED computation (7·input+1),
   which is the wrong-answer symptom itself. Only the bound probe is
   instrumented in this demo, so its slot is the only one: the rotation
   serves it a check on the very first call after any re-arm, regardless
   of window state.
6. Core 6: `ejit_asan_dump` again — exactly one `kind=entry` row for
   `ejit_asan_bound_probe`: `object=bound@<address of FamilyB>`,
   `expected=0x7 first=0x9`, count > 0; `entry_checks>0` and
   `entry_sites=1` in the stats. The worker-probe watch for the FamilyA
   exemplar records nothing (FamilyA never changed).
7. Any core: `ejit_asan_rotation_status` — per-core window/cycles/checks.

Reboot before repeating; the injection commands are diagnostic-only.
