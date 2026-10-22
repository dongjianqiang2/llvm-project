# EJIT Tier-2 MFS on spec5

This port adapts spec4 PR224 (ca8fab949115ac870fd417b051cb1e6f6dd61879)
to spec5's existing near/far code pools. It does **not** import PR201's
16 cell pools plus public pool, fnSize accounting, or PR230/231/237 patches.

`EJIT_T2_MFS` defaults OFF. When enabled, only genuine Instrumentation/CS
Instrumentation PGO with a known basic-block count of exactly zero authorizes
cold splitting. Missing, mismatched, synthetic or nonzero profiles stay hot.
This is a placement policy, not a promise that a zero-count path never executes.

## Build contract

Required options when enabling MFS:

```sh
-DEJIT_SRE_CODE_POOL=ON
-DEJIT_CODE_POOL_4K_SEAL=ON
-DEJIT_FIXED_CODE_POOL=ON
-DEJIT_SRE_SHARED_TASKPOOL=ON
-DEJIT_SRE_SHARED_CODE_POINTERS=ON
-DEJIT_CODE_POOL_BATCHED_PUBLISH=ON
-DEJIT_SRE_PGO_BRANCH_AUDIT=ON
-DEJIT_T2_MFS=ON
```

For the OFF control change only `EJIT_T2_MFS=OFF`. Do not enable the spec4
`EJIT_CODE_POOL_FIXED_NEAR_HOT` option: this port does not provide it.
Use normal online PGO; audit diagnostics can remain enabled, but audit-only
profiling does not authorize splitting.

Rebuild LLVMCodeGen, LLVMEJIT, the product archive/lipo package and final image.
The shared ABI is **26**, distinct from spec5 base18, spec4 MFS23 and PR230's
24/25 layouts. Every participating core must use the same image configuration.
This standalone PR does not certify combination with PR230; combining layouts
requires integration and another deliberate ABI decision.

## Cold reservation, costs and publication

Keep the existing spec5 near reservation unchanged (the repository example
uses 16MiB with alignment slack). Add an independent executable cold section in
the product linker script, as shown in the existing `ejit_registry.ld` example:

```ld
.text.ejit_cold ALIGN(4K) :
{
  __ejit_cold_start = .;
  . += 8M; /* 6MiB usable when the loaded base is not 2MiB-aligned */
  __ejit_cold_end = .;
} > CODE
```

The configurable example uses `--defsym=__ejit_cold_bytes=0x800000` instead.
The script is an integration example: preserve the board's MEMORY/SECTIONS
layout rather than replacing it blindly. DLIB needs only 4KiB section alignment;
the runtime rounds the loaded start up and end down to 2MiB boundaries, using
only complete large pages inside the reservation. An 8MiB reservation provides
6MiB usable unless already 2MiB-aligned; reserve 10MiB to guarantee 8MiB usable.
At least one complete 2MiB page must remain after alignment. The cold region must
be separate from near and placed within appropriate AArch64 branch reach.
Missing/invalid/overlapping or too-small reservations fail closed, with actual
bounds and a specific reason logged. Successful setup logs usable bounds and
head/tail alignment slack. No dynamic allocation fallback is used for rejection.
OFF does not require cold memory; omit its reservation for an ordinary OFF image.

Cold memory is additional capacity, not free space: it does not shrink near
or Tier-1 far capacity. Each cold companion starts on a separate page. There
are extra allocations, branches, permission/cache operations and shared ABI
fields. No performance benefit or absence of adverse effects is claimed without
product measurements.

The owner first seals/synchronizes cold companions, then performs spec5's
existing **global near batch** flush. This is not per-cell independent commit:
a cold failure can delay the hot batch. Neither cache nor inline-cache receives
a callable pointer before both ranges are ready. A peer separately prepares
hot and cold before setting its executable-core bit. Failures remain fail-closed
and retryable. Bump allocations retain their existing engine-lifetime policy.
The cold pool has kind `Cold` and local ID0; near/far retain spec5's local IDs.
Pool IDs are interpreted together with kind/base, not as global cell identities.

## Diagnostics and board sequence

Board source: `ejit_test/ejit_mfs_zero_count_sre_multicore_test.c`.
This is the source PR's 16-cell acceptance workload, not the PR230 light demo.
Like the PR230 reuse demo, `MFS_RUN_INIT_ARRAY=1` is the default: the first
accepted setup on EACH core runs `call_init_array_functions()` before EJIT
initialization. Repeated commands do not rerun constructors. If product startup
already runs the relevant constructors, compile the demo with
`MFS_RUN_INIT_ARRAY=0`; never initialize through both paths. This switch is a
demo compile definition, not an LLVM CMake option. Compile with the custom EJIT
Clang, not stock Clang.

1. After a fresh coordinated reset, core6: `test_ejit_mfs`.
2. Core16: `test_ejit_mfs`.
3. `test_ejit_mfs_print` prints diagnostics without reinitializing.
4. A failed/timed-out run requires coordinated reset. A completed repeat
   validates already-published code.

First setup rejects any pre-existing runtime with `-21`, even if its worker
is core6: `ejit_init_pgo()` does not upgrade an already initialized non-PGO
runtime. Do not run reuse and MFS demos sequentially in the same live image,
or clear the demo's shared state while old constructors/runtime remain live.
The startup-owned mode still requires externally verified constructors on both
cores; worker readiness and a static registry are not proof of LLVM C++ init.

Use the same board source with `MFS_EXPECT_SPLIT=0` for the OFF control.
It trains only the hot path, waits for actual T2 execution, then executes the
previously zero-count path and checks results and cold placement. The retained
16-cell/128-round workload can be slow; it is not a latency benchmark.

`ejit_taskpool_classify_tier2_pc(pc)` distinguishes hot1/cold2/unrecognized0.
`ejit_get_cold_code_pool_stats` reports the independent pool, and compiled
diagnostics show the companion start/size. These APIs are retained by lipo's
GC-root list. Repackage with the updated script.

## Validation boundary

The lifecycle harness
`ejit_test/ejit_mfs_zero_count_command_sequence_test.c` uses mocks and validates
startup/repeat/wrong-core/failure handling only.

`EJITMfsIntegrationTests` uses real LLVM optimization, instrumentation profiles,
JITLink and host RW/NX/RX transitions, with simulated core IDs. The zero-profile
ON/OFF control uses actual64:0 counters; controls cover missing/mismatched64:0
and observed64:1 (not split). ON-only tests cover owner/peer failure retry and
emitted AArch64 big-endian objects. `EJIT_MFS_ARTIFACT_DIR` retains those objects;
emitting BE objects is not executing BE code.

Actual SRE page-table/TLBI/cache/coherence, final linked branch reach, product
SDK symbols, custom frontend output and end-to-end board execution still need
board acceptance. Consult the PR's validation results for what was actually
compiled/run on this port; spec4 results are historical, not spec5 certification.

### Spec5 port host validation (2026-09-24)

Fresh Linux ARM64 GCC13 Release/assertions build, AArch64 backend, using the
options above plus diagnostics/stats ON; builds serialized with at most8 jobs.
The same build directory was reconfigured and rebuilt for each product switch.

| Suite | MFS ON | MFS OFF |
| --- | --- | --- |
| Real JIT MFS integration | 5 passed | 2 passed, 3 ON-only skipped |
| Code pool/memory manager | 64 passed | 64 passed |
| Shared taskpool | 168 passed | 168 passed |
| Branch-profile audit | 4 passed | 4 passed |

The shared suite retains2 pre-existing disabled tests, not counted as passes.
All test invocations were bounded by120s. The board-command lifecycle mocks
passed in both modes (30s bound); lipo GC-root regression passed (60s bound),
including an archive lacking the optional new diagnostics.

Actual AArch64 BE objects were emitted with/without splitting, then statically
linked using LLD17 at hot0x40000000/cold0x42000000. ON has a20-byte hot section
and772-byte cold section; OFF has a788-byte hot section and no cold section.
Both final fixtures have no remaining relocations. This tests an artificial
32MiB layout, not the product image or BE execution. The host runtime tests
execute both paths in native ARM64 JIT code with real RW/NX/RX transitions.

### Board initialization follow-up

The one-shot init-array adaptation was checked in all four combinations of
`MFS_RUN_INIT_ARRAY=0/1` and `MFS_EXPECT_SPLIT=0/1`. Mocks verify constructors
precede runtime initialization, one call per participating core in shell mode,
no calls in startup-owned mode, repeat/print/concurrent-setup protection, and
rejection of a pre-existing worker or producer runtime before constructors.
These mocks do not validate the product's actual init-array contents.

A separate host allocation probe repeated the six real-JIT integration tests
20 times (120 passes), wrapping C allocations, ordinary C++ new and mmap.
It recorded4,999,836 requests with a maximum request of4,194,304 bytes; no
0xffffffff request was reproduced. Requests at or above256MiB would terminate
the diagnostic probe, not clamp production allocations. This hosted run has
normal C++ initialization and is not a reproduction of the old zero-dimension
const_after_init board workload. The historical4GiB-1 failure remains unresolved.
