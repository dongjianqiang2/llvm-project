# PR248 Global Layout Board Smoke

Use only `ejit_global_layout_sre_test.c` and the matching
`llvm/ExecutionEngine/EJIT/EJitRuntime.h`. No test helper header is needed.
This is a cold-image smoke, not a board acceptance result.

A second smoke covers the 2026-10-15 unlisted-functions contract:
`ejit_test/baremetal/ejit_global_layout_unlisted_test.c` (same three-step
core6/core16 flow, PGO64, 8 manifest members A..H plus registered-but-unlisted
X/Y/Z). Its authoritative spec is the file's header comment. Run it as a
separate image; its PASS criteria are the append order (every unlisted body
above the manifest region), `unlistedAppended == 3` and the park evidence
during collection.

A third smoke covers the 2026-10-16 name-only contract:
`ejit_test/baremetal/ejit_global_layout_nameonly_test.c` — five 2-dim
(cell x trp) roots listed as NAME-ONLY entries (entryName only, no dims, no
bounds), one of them dispatching two identities. Its authoritative spec is
the file's header comment; it needs four shell steps (worker boot, producer
phase-1 drive, producer refresh drive, worker print). PASS criteria: init
accepts a name-only manifest for dimensional functions, the five
first-identity member bodies are strictly rank ordered, the function's second
identity appends above them WITHOUT failing the layout
(`unlistedAppended == 1`, park evidence during collection), and both
properties survive a lifecycle refresh into a second generation.

Use this revision's `lipo.py gc-merge`, which retains the defined runtime exports
`ejit_init_global_layout`, `ejit_get_global_layout_status`,
`ejit_refresh_global_layout` and `ejit_shutdown_checked`. Older scripts discard
these application-only entry points before the demo object is linked. Rebuild
the runtime package and confirm all four exports survive in the final SDK
artifact. Demo-object GC retention alone does not verify runtime-package
retention or final SDK linking. Older runtime archives without these APIs do
not acquire undefined roots from the optional retention list.

## Build The Image

Replace the old `test_ejit_period` source. PR247, PR230 and PR248 demos define
the same shell entry: use separate images, never link them together. Since the
2026-10-15 unlisted-functions contract the manifest no longer has to cover every
registered entry, but this smoke deliberately registers and lists exactly these
26 `ejit_entry` roots, A through Z, so the strict A..Z address-order assertion
stays exact and `unlistedReady`/`unlistedAppended` must both stay zero.
Do not add extra entry roots to this image: they would compile as unlisted
functions and break the exact 52-count checks below. Internal helpers are not
additional entry roots. Verify the final
package's static registration/root list as well as this C object's 26 roots.

A FAILED layout no longer switches misses to a fallback-only mode: every
function keeps compiling through the ordinary paths and the system keeps
running. `ejit_get_global_layout_status` is therefore the health check that
makes a lost layout visible — archive its output (including the new
`unlistedReady`/`unlistedAppended` fields) with the worker log.

Use this branch's EJIT Clang, not stock Clang with ignored attributes. The source
fails compilation without `ejit_entry` support. Preserve generated AOT wrappers,
embedded bitcode, static registration sections/constructors and their linker GC
roots in the existing spec5 compile/lipo/package/merge.ld flow. Use the matching
64-bit shared ABI20/VP ABI3 runtime and `-mno-outline-atomics` for SRE. Rebuild BOTH this
single C object and the runtime package, then link a fresh SDK image; replacing
only one leaves stale ABI, wrapper or runtime code. Do not reuse a PR247 package.

Runtime requirements: normal online PGO quota64, four active admissions,
async shared taskpool, worker core6, fixed near/far code pools, 4K seal/batched
publish, shared code pointers, audit + diagnostics + VP ON. Enable the existing
package build `--stats` option / CMake `-DEJIT_STATS_ENABLE=ON` and INFO diagnostics
for worker evidence. Without runtime stats the52-count check intentionally fails;
do not fabricate counters or drop it. No SDK interface, task-ID provider, mapping
callback or runtime API is added by this example. Keep existing product startup,
code-pool/linker provisioning and coherent same-VA `.mc_shared` mapping on both
cores. Capacity must cover 26 final bodies, temporary T1 bodies, staged objects,
helpers and retained generations; NO_RECLAIM is not unlimited reuse.

Both cores run init-array once before their local runtime initialization.
Define `LAYOUT_RUN_INIT_ARRAY=0` ONLY if existing startup already ran it on BOTH
cores. `LAYOUT_SHARED` may name the existing product shared section. Run in an
otherwise idle reset image, from schedulable shell tasks; no other callers or
configuration writers. All four shell arguments are ignored: no arguments means
normal PGO64, not Baseline or audit-only. Keep the default runtime quota/cap;
the C ABI cannot query their configured values, so verify build flags and worker
diagnostics rather than treating the demo banner as measurement.

## Run And Capture

```text
core6:  test_ejit_period
        WORKER_READY
core16: test_ejit_period
        OWNER_DONE (not yet board acceptance)
core6:  test_ejit_global_layout_print
        RUNTIME_CHECKS_PASS ... NOT BOARD_ACCEPTED until worker evidence checked
```

Wrong core, owner-before-worker, repeated init, workload rerun and print-before-
done are rejected without replaying constructors. Failures/timeouts require a
reset; core6 may print partial state after FAILED. TaskDelay errors are fatal;
the bounded workload defaults to 30000 one-tick rounds, not a timing guarantee.
The print command is read-only: no init, activation, lookup, refresh or shutdown.
There is deliberately no refresh phase/control command in this simple smoke.

Before any manual lookup, a cold H invocation uses a volatile call boundary to
the real generated wrapper. Subsequent manual sweeps also start at H.
Every successful direct lookup executes its returned body and releases exactly
once after return, including the final address checks. Pending/admission deferral
uses the wrapper/AOT path and is not counted as a successful direct sample.
Early AOT business correctness is not PASS: COMPLETE, stable nonzero generation,
26 required/ready/published, asyncCompiles52 (26 T1 + 26 final publications),
readyEntries26, no active profiles/in-flight work or failures, and
strictly increasing actual final-body addresses A..Z are all required.
COMPLETE may precede draining obsolete queued-T1/dedup tail requests: the same
bounded loop waits/yields until idle, failing on errors/count mismatch or timeout.
The selected final PGOUse/T2 generation is ordered; temporary T1 is not relocated.

Archive both workload and worker logs. Require worker `active=0/4`, completed26,
tier1=26, tier2=26, mergeFailed=0, execPrepFailed=0, 26 final near-pool T2 entries,
and genuine per-function quota64/saturation evidence, including branch-audit
`root_entries=64` for each of the 26 names (26 x 64 returned T1 samples).
No new T1 calls after freeze; real-return/release is essential. The C ABI exposes
neither the threshold nor a returned-sample counter, so automated PASS alone
does not prove these fields. Unexpected quota/cap/tier counts invalidate board
acceptance even if business/order checks pass. Confirm errors and pending counts
are zero, with all recorded addresses ordered in the same generation.

H-only IR/ASM capture is enabled by default. Core6 arms capture before READY;
the read-only print command prints the saved function and module after DONE/FAILED.
Capture may be absent on failure before
compilation; preserve that failure, do not claim a dump was produced. Keep capture
off explicitly with `-DLAYOUT_DUMP_IR=0` for a separate timing run. Host mocks,
native tests, genuine BE compilation and partial links do not establish SDK final
link, cross-core permissions/cache
coherence, board execution, latency or memory acceptance.
