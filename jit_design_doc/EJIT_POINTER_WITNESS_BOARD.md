# PR256 Unknown-Bounds Pointer Witness Board Demo

Single C: `ejit_test/ejit_pointer_witness_sre_test.c`.
Use this dedicated demo for PR256, not the older pointer-period demo, which
installs an exact allocation-size callback and tests the strict-bounds path.

## Build

Use the PR256 runtime (including its PR252 dependency), its matching
`EJitRuntime.h`, and the merged EJIT Clang with PR249 pointer-layout emission
and PR247 local free-dim boundaries. Update **both Clang and runtime**, then
recompile the demo C/embedded bitcode. There is no new Clang flag, SDK symbol, adapter
header, task-ID hook, allocation callback, or mandatory demo macro.

Keep the existing SRE shared-taskpool, fixed worker6, code-pool and permission
configuration. Keep `EJIT_SRE_PGO_BRANCH_AUDIT=ON`, `EJIT_DIAG_ENABLE=ON`, and
enable **runtime** counters with `EJIT_STATS_ENABLE=ON` / `build.sh --stats`.
A macro on this C alone does not enable counters in a prebuilt library.
The demo calls the existing `ejit_init_pgo` with Async and opt level 2.

Example (retain your SDK target, include paths and ABI flags):

```sh
clang -target aarch64_be-none-elf -O2 -ffreestanding -mno-outline-atomics \
  -ffunction-sections -fdata-sections -I/path/to/llvm/include \
  -c ejit_pointer_witness_sre_test.c -o ejit_pointer_witness_sre_test.o
```

There is **no `main`**. Link only one demo exporting `test_ejit_period`.
Retain `test_ejit_period`, `test_ejit_pointer_witness_print`, normal EJIT
registry/bitcode/init-array sections, and the existing `.mc_shared` mapping.
For example, the existing relocatable lipo retention check is:

```sh
ld.lld -r --gc-sections -u test_ejit_period -u test_ejit_pointer_witness_print \
  --script=ejit_test/lipo/merge.ld ejit_pointer_witness_sre_test.o -o retained.o
```

This is not the final SDK link script. Both cores must see the same coherent
shared storage and permissions, as in the existing board demos. All five
pointer roots and their backing objects are in `.mc_shared`; no allocation
extent is registered or discovered by a demo callback. Run in a fresh,
standalone image without other EJIT producers or allocation lookup adapters.

## Run: 6 -> 16 -> 6

```text
core[6]-> test_ejit_period
core[6]-> core 16
core[16]-> test_ejit_period
core[16]-> core 6
core[6]-> test_ejit_pointer_witness_print
```

Wait for `WORKER_READY` before core16, and `OWNER_DONE` or `FAIL` before the
final core6 print. All four shell arguments are ignored. Printing is read-only;
it never initializes, registers, reactivates, or converts a failed run to PASS.
Do not shut down between commands. Reset before rerunning.

**Default waits are unlimited** (`PR256_WAIT_ROUNDS=0`). Real compile,
publication, queue, SDK-delay and result errors still fail immediately.
The optional finite limit is for automated testing, not required on board.

By default the demo runs init-array once on each participating core. Only if
the SDK already runs it on **both** cores, build with `-DPR256_RUN_INIT_ARRAY=0`.
Do not run constructors twice or only on core6.

## What Must Fold

All entries use `cell=1` and an `ejit_free_dim` slot. The original four use a
parameter; the fifth reads a local slot from an unbound input structure.
Cell0 deliberately has
different marked values. Within a cell and generation, the marked fields are
uniform across all slots. Complete backing objects through cell1 are alive,
initialized and compile-readable, satisfying the PR256 witness contract.
The 64-bit field is invariant in its entirety, not merely bit 17.

| Entry | Address / field coverage | Marked loads | Final T2 constant after renewal |
| --- | --- | --- | --- |
| `pr256_witness_nested` | Pointer root, fixed cell1, `%5` field `+9` and `%10` field `+344` | 2 | `201900` |
| `pr256_witness_wide` | Pointer root, cell1, `%5`, stride64, full `i64` plus bit17 test | 1 | `0x1234567800000013` (`1311768464867721235`) |
| `pr256_witness_flat128` | Pointer root, cell1, `%5`, stride128, fields `+9/+31` | 2 | `211900` |
| `pr256_witness_mul1240` | Pointer root, cell1, `urem i16 -> mul i16 1240 -> zext -> GEP -> +9` | 1 | `1900` |
| `pr256_witness_local184` | `load i16(input+20) -> local free_dim -> %5 -> *184 -> zext -> cell1 +14` | 1 | `1900` |

The added entry uses the existing spelling on an initialized, read-only local:

```c
const uint16_t slot ejit_free_dim = input->slot;
```

The input is not a bound pointer, and its real value need not be constant.
Only the authorized field load uses the zero witness; the initializer, live
slot-dependent payload and visit store retain runtime behavior. The application
must guarantee that the marked field is invariant across the selected slots.
Do not use this annotation to erase a real identity whose field values differ.
The local cannot be assigned again or have its address taken.

Each return is the table constant **plus a live slot-dependent payload**.
The actual selected slot's `visits` must still increment. Every call checks
the full return value and all visit counters for that case: only the selected
cell1/slot may change; every cell0 counter must remain zero. Thus folding the
value cannot accidentally redirect the actual memory accesses to slot0.

Initial mode7 constants are `80700`, `0x1234567801020007`, `90700`, `700`, `700`.
After all five T2 publications and completion of the sole caller, the workload
drains, deactivates cell1, changes marked data to mode19, and reactivates it.
All five entries then undergo another real T1-to-T2 compilation. This is an
exclusive standalone update test, not a general concurrent borrow fence.
No configuration write occurs after a failed drain/deactivation.

In both generations, the test changes only unmarked payloads by `+123` while
active and checks real wrapper calls over every slot, plus inputs 10 and 65535.
The slot remainders and live stores must remain correct. Each phase starts
with the real cold wrapper, not manual prewarming; pending polls are not
counted as executed samples. T1/T2 addresses are allowed to be equal.

## Acceptance

Runtime evidence:

- Ten `PHASE` lines, each with at least 65 actually executed manual grants.
- `OWNER_DONE`, then `RUNTIME_CHECKS_PASS` on core6.
- 20 successful compilations, five Ready/T2 entries, all five
  `post_publish_seen=yes`, no pending work or compile/publication/queue errors.
- Ten real `root_entries=64` branch audits across the two generations.
- In each generation, mayconst audits for the five entries report
  `2/0/0`, `1/0/0`, `2/0/0`, `1/0/0`, `1/0/0`, with removed counts 2/1/2/1/1.

**Functional success alone does not prove folding.** Final core6 printing
includes the mayconst ranking and captured IR/modules. Require the final five
T2 definitions to have `function_entry_count=64`, PGO profile metadata, the
constants above, and **zero remaining marked loads in those definitions**.
Slot-dependent payload loads and visit stores must remain. Remaining `urem`
or address arithmetic for these live accesses is expected, not a failed fold.
In `pr256_witness_local184`, the `input+20` slot load must remain, but the
marked field load and `__ejit_local_free_dim_v1_*` calls must not.
Do not judge unrelated module declarations or T1 captures as the final T2.

The final banner intentionally says `FINAL_ACCEPTANCE_PENDING` until these IR
checks are complete. Keep worker and owner logs, including partial failures.

## Validation Boundary

The original four-entry version was exercised on the user's board; that
evidence does not automatically accept the added local-variable case.

The integrated five-entry C has been exercised with real generated wrappers
and native Linux ORC/online PGO, two generations, full-width returns and live
reads/writes. Its embedded IR has seven marked field loads; its final five
T2 definitions have none. Normal and
batched-publish configurations each passed five fresh-process runs: ten phases,
780 checked calls, ten 64-entry audits and five checked final T2 modules per
run. All local markers are gone from final executable IR; the input slot load
remains. LE/BE compilation, pointer-layout metadata and relocatable lipo symbol
retention passed. These checks do **not** establish final SRE SDK linking or
real-board acceptance of the newly integrated fifth case.

This demo does not cover arbitrary pointer chains/data-dependent UE indices,
an unanchored absolute base already beyond its first element, or nonuniform
marked values across slots. Those are not newly authorized by this example.
