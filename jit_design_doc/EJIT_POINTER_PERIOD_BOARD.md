# PR251 Pointer/Bound-Context Board Demo

Single C: `ejit_test/ejit_pointer_period_sre_test.c`. No `main`, helper header,
new SDK adapter, task-ID hook or shared-memory callback is required.

## Build

Use this implementation branch's runtime and public `EJitRuntime.h`. Use the
matching EJIT Clang including PR249's `ejit_pointer_layout` emission; rebuild
the demo bitcode, not just the runtime library. The compiler attribute check
does not by itself establish that a compiler includes PR249.

Keep the existing SRE shared-taskpool/code-pool/permission configuration.
Keep `EJIT_SRE_PGO_BRANCH_AUDIT=ON`, `EJIT_DIAG_ENABLE=ON` and enable the existing
runtime counters with `build.sh --stats` or `EJIT_STATS_ENABLE=ON`. A macro only
on the C file cannot enable counters in a prebuilt runtime library.

Example C compilation (retain your SDK's existing target/include/ABI flags):

```sh
clang -target aarch64_be-none-elf -O2 -ffreestanding -mno-outline-atomics \
  -ffunction-sections -fdata-sections -I/path/to/llvm/include \
  -c ejit_pointer_period_sre_test.c -o ejit_pointer_period_sre_test.o
```

There is no new mandatory demo `-D` option. Do not disable EJIT embedding/AOT
wrapper passes. Do not link another demo exporting `test_ejit_period` into
the same image. In the existing lipo/final-link retention flow, retain both
`test_ejit_period` and `test_ejit_pointer_period_print`, plus the normal EJIT
registry/bitcode sections. A relocatable retention check can use:

```sh
ld.lld -r --gc-sections -u test_ejit_period -u test_ejit_pointer_period_print \
  --script=ejit_test/lipo/merge.ld ejit_pointer_period_sre_test.o -o retained.o
```

The standalone image must provide the usual shared `.mc_shared` mapping:
same virtual/physical storage, coherence and existing permissions on both
cores. This is the existing product contract, not something an option proves.
This demo's callback reports the exact size of its own statically allocated
object. It does not claim to discover arbitrary product heap allocations.

## Run: 6 -> 16 -> 6

After a fresh reset, with no other EJIT producers or configuration writers:

```text
core[6]-> test_ejit_period
core[6]-> core 16
core[16]-> test_ejit_period
core[16]-> core 6
core[6]-> test_ejit_pointer_period_print
```

All four shell arguments are ignored. Wait for `WORKER_READY` before core16,
and `OWNER_DONE` or `FAIL` before the final core6 print. Print is read-only and
does not initialize, register, compile, reactivate or turn a failure into PASS.
Do not call `ejit_shutdown` between these commands. Reset before rerunning.

The demo runs init-array once on each participating core, before registration
freezes. Only when the SDK already does this on **both** cores, compile with
`-DPR251_RUN_INIT_ARRAY=0`. Do not run constructors twice or only on core6.

Waits are unlimited by default (`PR251_WAIT_ROUNDS=0`); actual compile,
publication, queue, SDK-delay and functional errors still fail immediately.
An optional finite wait is for automated host/protocol testing, not required
on the board. A missing stats build cannot become acceptance.

## Cases And Expected IR

All three entries use cell1 and ordinary online PGO through `ejit_init_pgo`.
Each first runs through its real cold wrapper, then executes/releases real
JIT grants until T2 publication is established. Pending polls are not samples.
The demo never uses differing T1/T2 pointer values as its acceptance criterion.

| Entry | Source operation | Final IR after renewal |
| --- | --- | --- |
| `pr251_pointer_slots` | Marked mode through pointer-period, slot `%5` and `%10`; free_dim slot | `3800 + payload[slot % 10]` |
| `pr251_pointer_constant` | Marked mode at nonzero constant index 3 | `1900 + payload[slot % 10]` |
| `pr251_bound_loop` | Bound object; marked count reveals four-iteration loop, then marked weights fold | `8800 + payload[slot % 10]` |

Initial mode is 7 (constants 1400/700/4000). After all three T2 publications,
all calls stop and tokens are released; the workload drains, cell1 is
deactivated, data changes to mode 19, then cell1 is reactivated and all three
entries are rebuilt. This is an exclusive standalone update test, **not** a
general concurrent writer/compiler-borrow fence. A failed drain never permits
a configuration write. Live payload changes are checked in both generations.

Expected runtime result: six `PHASE` lines, each with at least 65 executed
manual grants; `OWNER_DONE`; then `RUNTIME_CHECKS_PASS` on core6. Final shared
stats: 12 successful compiles, 3 Ready entries, 3 T2 entries, no pending work
or errors, all three `post_publish_seen=yes`.

Final acceptance also requires worker evidence: initialized `pgo=1`, six
real `root_entries=64` audits, published IR with `function_entry_count=64`
and PGO profile metadata. The final three entries should have no remaining
marked mode/weight loads; slot-dependent payload loads must remain. In the
bound-loop audit, expect `mayconst=2/1/0`: one load is removed specifically by
the later optimization pipeline. Functional success alone can still be AOT.

The compiler can warn that the dynamic AOT loop could not fully unroll, and
that the bound-only entry does not index a period global directly. The first
is why the JIT test exists; the second does not justify removing the `cell`
dimension that governs the bound object's lifetime.

## Verification Boundary

This demo is validated separately from the six undisclosed product sites.
Host execution and LE/BE object/embedded-IR checks do not establish SRE final
SDK linking, shared permissions or actual core6/core16 board correctness.
Keep the complete worker and owner logs, including failed/partial captures.
