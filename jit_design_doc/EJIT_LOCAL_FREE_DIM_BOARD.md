# PR247 Local Free-Dimension Board Demo

Single C: [ejit_local_free_dim_sre_test.c](../ejit_test/ejit_local_free_dim_sre_test.c).
Replace the old file defining `test_ejit_period`; do not link both demos.
No test-helper header, SDK adapter, new task ID, private generated-global access,
or runtime change is required. Use the matching public `EJitRuntime.h`.

## Build

Use the patched PR247 EJIT Clang and matching runtime. Stock Clang and
`EJIT_DISABLE` are rejected, not silently treated as an AOT-only success.
Example object command, with paths supplied by the existing SDK build:

```sh
$EJIT_CLANG -target aarch64_be-none-elf -O2 -ffreestanding \
  -mno-outline-atomics -I$LLVM_SOURCE/llvm/include \
  -DEJIT_SRE_TASKPOOL -DEJIT_SRE_SHARED_TASKPOOL \
  -DEJIT_SRE_PGO_BRANCH_AUDIT -DEJIT_DIAG_ENABLE \
  -c ejit_local_free_dim_sre_test.c -o ejit_local_free_dim_sre_test.o
```

Use the established AOT embedding/static-registry/lipo/SDK final link flow.
Do not disable EJIT embedding or wrapper passes. Retain generated wrappers,
init-array constructors, embedded bitcode, period/fixup records, shell entry
points and their relocations; the existing lipo `merge.ld` has the registry
and init-array KEEP rules. Audit final SDK imports, especially `wmemchr`;
a demo-object audit does not certify the complete runtime package.

Runtime requirements: async shared taskpool, fixed worker core6, shared code
pointers, normal online PGO, stats/dumps/diagnostics, branch audit ON.
Build the RUNTIME library with the existing `build.sh --stats` option or
`-DEJIT_STATS_ENABLE=ON` in its CMake configuration. Stats default OFF.
Defining `EJIT_STATS_ENABLE` only while compiling this demo C does NOT enable
counters in the linked runtime library. Retain the actual runtime build flags
as evidence; zero counters despite working JIT calls are not acceptance.
Keep `EJIT_SRE_PGO_BRANCH_AUDIT=ON` and `EJIT_DIAG_ENABLE=ON`.
This demo
always calls `ejit_init_pgo`, not plain init or representative-sharing init.
The matching runtime's `EJit.cpp` sets its normal PGO quota to64 on this path;
there is no public demo quota setter. Do not change that runtime quota or
inject fake samples. A public C tier/quota-query API is absent in this baseline,
so the final worker log/audit check below is mandatory.

Map `.mc_shared` coherently at the same VA on cores6 and16, including both
cells, volatile runtime offset/initializer counter, stage and result data.
The generated wrapper cache and registry also need the established SDK mapping.
This is a fresh, otherwise idle, standalone image with no other business
callers, configuration writers or unrelated EJIT compilations. It is not a
concurrent-update or compiler-borrow-fence demonstration.

Both cores run the existing `call_init_array_functions()` once before their
own local public registration fixups and EJIT init. Default
`LFD_RUN_INIT_ARRAY=1` matches PR230. Set it to0 only when SDK startup already
ran constructors on BOTH cores. Never replay constructors after a partial run.
All errors/timeouts require an image reset, not a partial retry.

## Run

Run from schedulable shell tasks; all four shell arguments are ignored.

```text
core[6]  test_ejit_period
         [LFD247] WORKER_READY
core[16] test_ejit_period
         [LFD247] PHASE=0 cell=0 mode=7 ...
         [LFD247] PHASE=1 cell=1 mode=11 ...
         [LFD247] PHASE=2 cell=1 mode=19 ...
         [LFD247] OWNER_DONE ... NOT final acceptance
core[6]  test_ejit_local_free_dim_print
         [LFD247] RUNTIME_CHECKS_PASS; FINAL_ACCEPTANCE_PENDING ...
```

Keep the core6 worker alive. Wrong cores are rejected before initialization or
writes. The atomic RESET/STARTING/READY/RUNNING/DONE/FAILED gate rejects
owner-before-worker, repeated initialization, reruns and partial retries.
The print entry is read-only and can print partial FAILED diagnostics; it
never initializes, registers, activates, requests compilation or resets state.

Each phase checks the first cold generated-wrapper call, then at least65
successful fresh lookup/execute/release grants. Every acquired pointer executes
exactly once before release, including on a wrong-result failure. Pending calls
are not samples. The owner yields even after successful calls and checks every
`SRE_TaskDelay` result. A baseline-only/no-recompile runtime or a missing final
publication cannot satisfy the two-publications-per-generation requirement.
Audit-only mode can still have two publications and even tier2-labelled cache
slots: that is why initialization pgo=1 AND profile metadata in the published
module must be confirmed for final acceptance. Pointer addresses are printed
only for diagnosis: a valid allocator may reuse the T1 address for T2.

All ten actual offsets and exactly-once initializer effects are checked.
Mode is slot-uniform within each cell/generation; cells differ7/11. Ordinary
payload is changed after publication and read through the real offset/wrapper.
After both cells finish, all grants are released and business calls stop.
The owner drains work, deactivates cell1, drains again, then changes every
slot's mode to19 and reactivates. Cell0 remains7. A drain failure authorizes
no mode write. Safety relies on the completed publications and exclusive
standalone workload: `pending_count()==0` alone is NOT a borrow fence.
The synchronous compile callback returns before publication in this runtime;
no compiler-source borrower remains for these completed identities.

Board waits have no round limit by default (`LFD_WAIT_ROUNDS=0`), for both
PGO publication and queue draining. No extra compiler option is needed.
The worker-ready banner prints `waitRounds=0 (0=unlimited)`. Each wait yields
one SRE tick and progress logs appear every1000 rounds, including drain waits.
Compile/publish/queue errors, failed delays and business mismatches still fail
immediately. A stalled runtime can therefore wait indefinitely without PASS;
reset the image to abandon that run. Configuration is never changed before
the drain succeeds. Runtime safety/no-progress limits are unchanged.

Host regression tests can explicitly set a positive `LFD_WAIT_ROUNDS` for a
finite budget and retain an external process timeout. A round is not a fixed
number of milliseconds: SDK ticks, compilation, handshakes and logging affect
wall time. Do not enable a finite override for the normal board run.

## Final Acceptance

`OWNER_DONE`, aggregate counters, a changed pointer, or a zero print return
alone do not establish final board acceptance. Retain the complete worker log
and confirm all of the following manually after the read-only core6 print:

- Worker initialization explicitly reports `pgo=1`, not0/audit-only.
  Three actual normal-PGO generations: T1=3, T2=3, no merge/compile/publish/queue
  failure; all3 branch-audit rounds have `root_entries=64`.
- Final compiled entries: baseline=0, tier1_collecting=0, tier2=2, both
  `post_publish_seen=yes`; no pending/queued work.
- All3 phase checks passed, both cells/all offsets and live payload succeeded,
  initializer total equals checked calls, and cell1 renewal did not alter cell0.
- Worker IR/ASM removes the slot-uniform mode load and bakes the right value,
  keeps the real offset/payload load and volatile initializer effects, and
  contains no private free-dimension marker call/import. The published function
  carries `!prof` with `function_entry_count`64 and the module has the PGO
  profile summary. An audit clone's log or a tier2 cache label alone is not
  proof of profile use in the published code. The saved dump is the
  latest capture, not an archive of all generations; retain earlier audit logs.
  The final refreshed mode contributes1900; earlier cell0 contributes700.
- SDK final link/imports and real shared mapping/cache/execute permissions are
  valid. No Linux execution or native/BE object compile substitutes for this.

The current deliverable has local source/host/BE validation recorded separately
in the task support `BOARD_DEMO_RESULT.md`. No real board run is claimed.
