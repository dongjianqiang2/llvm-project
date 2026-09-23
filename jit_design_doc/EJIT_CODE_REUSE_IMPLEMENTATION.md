# EJIT version code reuse implementation map

This table tracks implementation evidence for PR230. `independently-verified` is
reserved for coordinator review. The experimental feature remains default off.

## Latest Closeout - 2026-09-16

PR230 is now rebased onto PR233 `812f6474b706e0bb8f0cd1b93e9b0413bd5457cf`.
The optimizer conflict preserves both bound-pointer facts and load-only
preserved-dimension evaluation. The board-shaped round-robin/update regression
and the [single-C board example](EJIT_CODE_REUSE_BOARD.md) are added; neither
this rebase nor the example claims board acceptance. See the rebase addendum
in the host closeout for current-source test results.

The production-lock repair has independent read-only review. Final shared-pool
suites pass NRC205/205 and token187/187; corrected concurrent stress passes
1000/1000 NRC and100/100 token independent processes without relaxing quota64.
See [host closeout](EJIT_CODE_REUSE_HOST_CLOSEOUT.md) for actual captured
status-5 fallback evidence, source-matching runtime results, and remaining gates.
Everything below is historical milestone evidence. The feature remains OFF,
the PR remains Draft, and board/product acceptance is not claimed.

## Current local host evidence — 2026-09-15

The following status supersedes the historical milestone table below. These are
local author results on Windows x86-64, not independent review or board acceptance.
Exact source/configuration/binary/log hashes are recorded in the task control
root's `REVIEW_READY.json`; raw failures are retained beside the passing logs.

| Requirement | Production path | Current evidence | Remaining gate |
| --- | --- | --- | --- |
| Candidate grouping before sampling | Worker candidate tier, `classifyRepresentativeRequest`, `EJitOrcEngine::classifyCandidate` | Same entry, different may_const gains classified before PGO admission; full prefix/schema/effective bindings compared; no candidate code emit | Bound-pointer requests currently use independent compilation |
| Real representative execution | Runtime C entry, production worker, dispatch observer | One representative supplies 64 generated T1 calls with checked returns/stores; five members wait on AOT; final call paused in flight before immutable freeze | Board execution |
| Full immutable profile | Driver bundle and VP collector | Edge counts, IC target, memop sizes and scalar side table checked from actual generated executions; first bundle bytes unchanged while second group samples | More profile shapes and failure timings |
| Physical T2 reuse | Prepared final IR + effective bindings, shared physical emitter | Six same-group members execute common T2; same-entry unequal group emits its own T2; raw cell/TRP arguments remain live | Board executable permissions / BE target |
| Independent group identities | Candidate directory, group handles, pool-to-session binding | Same-entry distributions 48/16 and 16/48 each have quota64, distinct session/attempt/timestamps and independent frozen profiles | Concurrent cold-group election/timeout |
| Cancel and re-election | Real `cancelRequestAttempt`, driver refresh outside group lock | Third group cancelled after one execution, re-elected at generation2; replacement token/session differ; frozen profile contains exactly64 new samples; group member shares T2 | Cancellation during execution/compile and source changes |
| Member lifecycle | Reclassification resets waiter, wake uses pool generation | Deactivate/reactivate old group member, compare and reuse existing physical T2 | Stale callback and queue/retry matrix |
| NO_RECLAIM | Sampling-only read tokens and worker drain | VP+NO_RECLAIM runtime 3/3, including in-flight final sample, group isolation, cancellation and version update | Broader lifecycle timings |
| Ordinary pool regression | Mode-specific token assertions | NO_RECLAIM suite192/192 single run; 7 old token-contract assertions corrected, sentinel release/readers verified | Old legacy concurrent quota test still fails under100-repeat stress; no claim it is fixed |

Current passing VP logs: `lifecycle-runtime-vp-test-20260915-02.log` and
`lifecycle-runtime-vp-nrc-test-20260915-01.log`. Reclamation may reuse a released
T1 address; replacement identity is verified by token/session and fresh profile,
not by requiring a different address. The initial address-inequality failure is
retained as `lifecycle-runtime-vp-test-20260915-01.log`.

The feature remains OFF by default. Runtime policy tests still include pure
admission-policy checks; they do not establish the complete process-init/mode
change matrix. T2 retry, cold timeout, source borrow completion, queue saturation
and stale callbacks through the runtime require further work.

### Failure/lifecycle follow-up (current source)

The production runtime test now injects one queuePush rejection for a real T2
request and one subsequent real worker compile failure before profile capture.
Ordinary rollback/retry retains the closed64-dispatch boundary and produces the
same frozen counters and shared T2; diagnostics assert exactly one queueFull and
one compileFailed increment. This is fault-injected queue-full handling, not a
physical capacity saturation claim.

Cancellation now happens while generated old T1 is paused under its sampling
read token. NO_RECLAIM retracts the slot and accepts a replacement request, but
replacement compilation waits for the old root token before replacing code and
profile storage. Reclaiming cancellation itself drains the token. The replacement
still records exactly64 fresh samples. VP+NO_RECLAIM passed20 independent-process
repetitions before the subsequent initialization-policy additions.

`EJit` rejects unsupported representative configuration before registration
consumption or driver/worker creation. Tests call the real C Sync-init rejection,
construct real PGO-off/audit-only rejected instances, and verify real ordinary
initialization leaves sharing inactive. A live Sync switch is rejected before
controller/cache-epoch changes. All features remain opt-in.

Audit diagnostics (`EJIT_SRE_PGO_BRANCH_AUDIT` / `enableProfileAudit`) may
accompany normal online PGO. Both the early initialization check and driver
group admission use `enablePgo`, matching `ctx.profileAuditOnly = !enablePgo`;
enabling diagnostics must not reject representative sharing or disable PGOUse.

### Cold representative timeout

Owner maintenance checks unfinished representative sessions before consuming new
queue work, so a continuously replenished queue cannot starve the timeout. The
internal Config sets a no-progress bound in platform trace-clock ticks and a
maximum re-election count. Defaults are5,000,000,000 ticks and2 re-elections;
host ticks are nanoseconds, while SRE deployments must set the bound for their
cycle-counter scale. Zero timeout is rejected at initialization.

Cancellation goes through the real pool outside group locks. A retiring flag
blocks new elections during cancellation. Each permitted new round uses fresh
request/session identity; exhausted groups stay on explicit AOT fallback without
reacquiring sampling admission. Runtime tests exercise a one-call cold
representative, a different member completing64 new samples (edge48/64/1312),
shared T2 consumption by the former representative, and a zero-re-election
budget releasing admission then retaining AOT fallback. The full matrix is
recorded under the control root's cold-final logs.

Source-borrow lifetime across classified waiters is still a separate open gate;
these timeout results do not establish caller-visible permission to mutate or
release registered source data.

### Classified waiter borrow fence and member final retries

Candidate classification now leaves the request's `BorrowPending` event live
after the no-code prefix pass. The owner schedules a waiter PGOUse request when
the group's bundle is ready, and the driver clears the borrow only after the
final transform has consumed the source identity. `completeRequestBorrow()` is
idempotent and clears copied bound descriptors before the attempt can retire.
`ejit_representative_deactivate_begin()` returns a generation/dimension/
instance/version fence; `ejit_representative_borrow_status()` is bounded over
the fixed attempt table and reports `EJIT_PENDING` until compiler reads end.
Stale or re-enabled fences are rejected. These APIs confirm compiler-source
completion only; they do not extend AOT or generated-code object lifetimes.

Waiter PGOUse attempts use their own request token without a sampling admission.
Their final transform has a bounded retry count (`representativeMaxFinalRetries`,
default 2). A failed member attempt is cancelled and retried while budget
remains; exhaustion marks that logical member failed, ends its source borrow,
and leaves subsequent calls on AOT. This avoids retaining a representative's
function-level PGO admission for a member that never owned one. Fault-injected
member failure and borrow-fence tests are present in the latest runtime source;
their final rebuild is pending the host RAM gate.

## Historical milestone evidence

The entries below describe earlier source states and are not current acceptance.

| Requirement | Production file / symbol | State | Exact test or artifact | Remaining risk / next gate |
| --- | --- | --- | --- | --- |
| Request-attempt identity and three completion events | `EJitSharedTaskPool::{beginRequestAttempt,cancelRequestAttempt,runCompile,cachePublish}` | implemented; coordinator independently verified milestone | `SharedTaskPoolTest` 173/173 at `30157004912f`; `/home/ruanchen/ejit-dev/agents/pr230-web-review/COORDINATOR_REREVIEW_3015700.md` | Public runtime borrow-completion adapter is still absent. |
| Exact candidate grouping before PGO | `EJitOptimizer::runPipeline`, `EJitCandidateCapture`, `EJitCandidateDirectory` | classifier implemented after accepted `7d26b051a13d`; coordinator review pending; runtime scheduling remains off | exact-source `EJitPreparedCodeTest`: 20/20; real optimizer two-phase prefix/schema capture; preserved-cell 200/200 group and 300 split; binding completeness/kind, schema, forced-collision, group+byte exhaustion; preserved-dims OFF/ON x VP OFF/ON optimizer TU 4/4 | Prefix is serialized before instrumentation and completed only with schema extracted from that same module after IR instrumentation. Hash is bucket-only and full prefix/bindings/schema are compared; the prefix entry and every referenced external function/data binding are validated. No ORC emission, representative scheduling, waiter lifetime, or final-code eligibility gate is connected here. |
| One representative T1 per group | `EJitRepresentativeGroupRegistry::{openGroup,electRepresentative,recordRepresentativeDispatch,currentRepresentative}`, `EJitCompileDriver::{admitSamplingRequest,bindRepresentativeTier1,dispatchObserverThunk}`, `EJitSharedTaskPool::{setSamplingAdmissionCallback,setDispatchObserver}` | **wired into the REAL runtime path** (local HEAD `807adab7`, author evidence; independent review pending) | local `build/bin/rep-runtime.exe` (`EJitRepresentativeRuntimeTest`) 3/3: the runtime is brought up through the production entry `ejit_init_representative`, every request enters through the business entry `ejit_taskpool_compile_or_get`, and the real `EJitCompileDriver::compileNow/compileCold` compiles Tier-1/Tier-2. Observed: `elected attempt 1 session 1 logicalKey=0x101 quota=64` (first legal arrival = cell 1, never hardcoded cell 0); `member cell=0/2/3/4/5 joins as waiter 1..5` with no personal T1 and no personal sampling admission; `representativeDispatches == 64` owned by the pool's committed-dispatch hook; `bundle publish outcome=0 (dispatch=64/64 quotaEnd=<real trace clock>)` | Product activation stays OFF (`Config::enableRepresentativeSharing` default false). Two real lifecycle bugs found by this test and fixed: `funcIndex == 0` was wrongly treated as illegal (it is a valid dense index; the sentinel is `kEJitInvalidFuncIndex`), and the representative's own Tier-2 was rejected for having no published bundle although that compile is what publishes it. Cold-representative/timeout re-election through the real pool is still not exercised. |
| Unique group generation, request token, and sampling session | `EJitRepresentativeGroupRegistry` (`attemptToken`, `samplingSessionId`, generation stamping), `SpecializationContext::samplingSessionId`, `EJitProfileBundle`, `EJitCompileDriver::repSessions_/repTier1Bindings_` | group-generation/attempt/session ownership implemented and tested; the real driver now binds the pool's Tier-1 request attempt to the group session | two groups of the same registry get distinct generation/session/attempt identities and never mix (`TwoGroupsNeverMixSamplesQuotaOrSessions`); 120 sequential sessions and 30 waves of four active sessions (VP macro builds, earlier milestone); the runtime-path test shows the real request attempt bound to the elected session (`Tier-1 request attempt 1 bound to sampling session 1`) | **Gap:** the SAME-entry/two-group production case is still NOT run (the existing two-group regression is cross-entry: entryA/200 vs entryB/201). Sampling IDs remain monotonic across owner-driver reconstruction because the allocator is shared. |
| Frozen bundle consumed by different members | `EJitRepresentativeGroupRegistry::{publishBundle,bundleFor,schemaCompatible}`, `EJitCompileDriver::compileCold` (group branch + `representativeWakeThunk`), `SpecializationContext::profileBundle` (+`profileData` compatibility view) | **implemented on the real driver path** (author evidence; independent review pending) | one bundle published once by the representative's real Tier-2 request, then consumed by every member's PGOUse compile through the production wake path: `representative-wake ... group Tier-2 enqueued (observed 64/64)` followed by `member key=0x…100/102/103/104/105 consumes the published bundle (count=64 limit=64)`; a member without a bundle stays on AOT; a schema mismatch is rejected instead of dropping value data | The driver still copies `indexedProfile` into `profileData` as the compatibility view the PGOUse transform reads; the registry owns the immutable carrier. The member compiles are per-function serialized by the pool's dedup. |
| Final IR exact compare and one physical code object | `EJitRepresentativeGroupRegistry::{decideMember,completeMember,noteRepresentativeCode}`, `EJitPreparedCodeEmitter::linkedIdentityEquals` | group-level logic implemented and unit-integrated only; **NOT wired into the real driver** | six cells -> one `codeId`/`fn`, emitter `codeObjects=1`, `reused=5`, real object count 2 in the focused test (`rep-group-pgo` 3/3) | **INCOMPLETE GATE:** on the runtime path each member still emits its OWN Tier-2 object (`sharedPhysicalReuses == 0` asserted as the honest current behaviour). Wiring requires exposing the engine's `orc::LLJIT`, capturing the post-pipeline module, `EJitPreparedCode::create` + `Emitter::link` for the representative, and `decideMember` reuse for validated members. |
| Representative cancel/re-elect, waiter cancel, same-key retry, exhaustion, queue-full | `EJitRepresentativeGroupRegistry::{invalidateRepresentative,cancelRepresentative,cancelWaiter}`, existing attempt layer | group-level logic implemented and unit-tested; queue-full/exhaustion remain on the existing attempt layer; **not** exercised through the real pool/driver | re-election bumps the generation; retired-session dispatch/publish settle `Stale`; the replacement starts at count 0 with new tokens; waiter cancel and duplicate `completeMember` settle exactly once (`AdmissionGatesAndLifecycleSettleExactlyOnce`) | **Gap:** queue-full/T2 retry, cold representative, late member, cancel, stale callback and source-borrow release are covered only by helper tests that bypass production admission/capture/publication. |
| Edge/IC/memop/scalar session isolation | `EJitVpCollector::{ejitVpCreateSession,ejitVpBeginSession,ejitVpBindProfileData,ejitVpTakeSessionSnapshot,ejitVpEndSession}`, session-aware scalar instrumentation; representative-private edge arrays | implemented for bounded active representative sessions; coordinator follow-up pending | collector and original probes 28/28; deterministic cancellation/retirement probes 35/35; shared cancellation/lifecycle integration 173/173; focused scalar/schema 12/12; production VP TUs compile | The ABI-v7 gate uses an exclusive allocation/retirement transition and references for producers, cancellation, and close; terminal discard cannot be changed back to retry preservation. An exact shared request token closes a published T1 session, failed drains are serviced under pressure, owner cold paths erase cancelled partial accumulators, tokenless queued-T2 lifecycle/generation drops notify the owner using the exact generation/version identity, replacement T1s retire any predecessor, and owner release retires sessions before private profd state is cleared. Group-level representative re-election remains to connect. |
| Immutable complete ProfileBundle | `EJitProfileBundle`, `readProfileSchema`, `EJitCompileDriver::compileCold`, `captureTier1ProfileAttemptIdentity`, `applyT1DispatchObservation`, `SpecializationContext::profileBundle` | implemented for the existing single-key T1/T2 chain; repaired above `b91cf59e`; pending independent verification | profile/schema probe exit 0; merge/scalar focused tests 12/12; VP macro production TUs compile; exact-source `EJitObservedT1DispatchTest` 192/192 token (16 observed/bundle tests) and 18/18 observed in NO_RECLAIM (198-test suite: 7 pre-existing base failures + 1 pre-existing flaky); real granted-T1 -> real queued T2 request -> bundle join test | `actualDispatchCount`/`dispatchLimit`/`quotaEnd`/`dispatchQuality` come only from the frozen Tier-1 dispatch observation carried by the Tier-2 request (never the configured threshold or the Tier-2 compile time); `freezeCompletedAt` stays the snapshot-completion instant. The engine-independent driver join is executed end-to-end with real pool objects; the ORC-engine part of `compileCold` remains compile-verified only. In the token build a concurrent closed-quota retry may still enqueue before the frozen timestamp becomes visible and is then reported as unknown (0), never fabricated. |
| 64 real T1 dispatch boundary | `EJitSharedTaskPool::{resolveMatchedSlot,peerPrepareSlot,admitObservedT1Dispatch,admitObservedT1DispatchLocked,classifyHit,enqueueTier2FromLookup,cachePublish}`, slot `t1DispatchLimit/t1DispatchCount/t1QuotaEnd`, bucket `observationLock` | implemented for the per-function experimental request-attempt path (shared ABI v22; the lock word reuses bucket header padding so every offset and `sizeof` are unchanged); repaired above `b91cf59e`; pending independent verification | exact-source `EJitSharedTaskPoolTest` + `EJitObservedT1DispatchTest` 192/192 token and 198 total / 190 pass NO_RECLAIM (7 pre-existing base failures + the pre-existing flaky `ConcurrentPeersCapTier1AtConfiguredSampleCount`, 5/10 head vs 3/10 e245); observed filter 16/16 token and 18/18 NO_RECLAIM, `ConcurrentReplacementCannotInterleaveTheCommit` 30/30 in NO_RECLAIM; reviewer barrier probe K2 (8 threads x 32 calls) 10/10 quota closed at count=hits=64, `missOpenPost=0`, vs e245 1/10; e245 implementation objects fail exactly the two new regressions (seqlock stability, legacy wrong Tier-2); local taskpool layout suite 91/91; compile matrix 14/14; AArch64 BE objects rebuilt with the v22 layout static asserts | Quota is the configured Tier-2 threshold (64 by default); only a committed pointer return consumes one entry; the final allowed dispatch freezes count/quotaEnd once and later calls fall back. A cold non-owner peer preparation carries the exact validated publish coordinates, so the final real dispatch arranges its own Tier-2 request. NO_RECLAIM serializes identity re-check + admission CAS + quotaEnd freeze with every publish/cancel/reset through the separate leaf `observationLock` (v22) instead of the bucket writer lock, so a granted dispatch no longer sets `writeFlag`/bumps `publishSeq` and cannot invalidate a concurrent load-only lookup (R1R-1); the clock is called under that exclusion, so it must stay a non-blocking timestamp source. A failed legacy cold-peer preparation no longer propagates the deferred arm with zeroed bucket0/slot0 coordinates (R2R-01). The replacement regression drains the FIFO ring with a bounded poll loop until its own attempt publishes (R2R-02). Legacy `hitCount` stays an identity-hit/hotness counter; legacy/tokenless mode reports Unavailable. Representative/group ownership, waiter lifetime and the borrow-completion adapter remain open. |
| Host / AArch64 BE / board evidence | affected host TUs and ABI probes | partial | host and AArch64 BE header evidence in `SOL_MILESTONE_STATUS.md` | Full BE TU lacks target libc/sysroot; no board evidence. |

Candidate-classifier verification uses the exact files in this worktree. Under
`/home/ruanchen/ejit-dev/build.lock`, `/tmp/compile-candidate.py` compiled
`EJitPreparedCode.cpp`, `EJitOptimizer.cpp`, and `EJitStructFieldPass.cpp`;
`/tmp/compile_candidate_matrix.py` compiled `EJitOptimizer.cpp` with preserved
dimensions OFF/ON crossed with value profiling OFF/ON (4/4); and the linked
`/tmp/pr230-candidate-tests` passed all 20 candidate, final-identity, and native
prepared-code tests. The candidate tests call the production optimizer, capture
the canonical prefix before instrumentation, then use the PGO schema extracted
from that same module after IR instrumentation. No candidate test invokes ORC
emission for a non-representative.

The configured `ninja -C build/ws6/host -j8 EJITTests` target is not evidence for
this milestone: that build directory names a different workstation source tree
and its link also fails on pre-existing missing SRE platform symbols. The
exact-source compile/link above avoids claiming that unrelated failure as either
a product regression or a pass. A first combined locked validation command was
also stopped by its 60-second outer timeout during the compile matrix; splitting
the same locked checks produced the passing results above.

Observed Tier-1 dispatch metadata verification (2026-09-11) uses the exact files
in this worktree. Under `/home/ruanchen/ejit-dev/build.lock`,
`/tmp/pr230-obs/build-shared-tests.sh` compiled the current
`EJitSharedTaskPool.cpp`, `EJitSharedPlatform.cpp`, `EJitLogger.cpp`,
`EJitProfileMerge.cpp`, `EJitSharedTaskPoolTest.cpp` and the new
`EJitObservedT1DispatchTest.cpp` with the exact `EJITSharedTaskPoolTests` target
flags and linked `/tmp/pr230-obs/out/tests`: 185/185 PASS (176 existing + 9 new).
The same script with `-DEJIT_SRE_TASKPOOL_NO_RECLAIM` produced
`/tmp/pr230-obs/out-noreclaim/tests`: all 10 new observed tests PASS; the 7
remaining failures in the pre-existing suite are byte-identical to the HEAD
baseline binary `/tmp/pr230-obs/out-base-noreclaim/tests` built from `git show`
HEAD sources (one further pre-existing threaded test is flaky in both).
`/tmp/pr230-obs/build-taskpool-tests.sh` linked the local (non-shared) taskpool
suite with the v21 request layout: 91/91 PASS. `/tmp/pr230-obs/matrix.py`
compiled 11/11 cells: driver local/shared x VP OFF/ON, production pool
(with/without NO_RECLAIM), profile merge VP OFF/ON, local taskpool VP OFF/ON and
the updated request-layout test TU.

AArch64 big-endian target evidence:
`/tmp/pr230-obs/be/probe_be.cpp` (v21 request/slot layout static_asserts),
`EJitSharedTaskPool.cpp`, `EJitProfileMerge.cpp` and `EJitCompileDriver.cpp` all
compile to genuine `ELF 64-bit MSB, ARM aarch64` objects
(`/tmp/pr230-obs/be/*-be.o`). The BE sysroot is the host aarch64-linux-gnu (LE)
cross-gcc 15 header set plus a one-line `gnu/stubs-lp64_be.h` alias shim; there
is no real BE product sysroot, no BE link, and no board run. The BE pool object
has no `wmemchr` relocation (only `memcpy`/`memmove`/`memset` plus hosted
operator new/delete). Host tests do not substitute for SRE runtime behavior.

Reviewed-defect repair above `b91cf59e` (2026-09-11, pending independent
verification). Under `/home/ruanchen/ejit-dev/build.lock`,
`/tmp/pr230-obs-fix/build-shared-tests.sh` compiled the current
`EJitSharedTaskPool.cpp`, `EJitSharedPlatform.cpp`, `EJitLogger.cpp`,
`EJitProfileMerge.cpp`, `EJitSharedTaskPoolTest.cpp` and the extended
`EJitObservedT1DispatchTest.cpp` with the exact `EJITSharedTaskPoolTests` target
flags: `/tmp/pr230-obs-fix/out/tests` 191/191 PASS (token path, 16
observed/bundle tests) and `/tmp/pr230-obs-fix/out-noreclaim/tests` 189/196 with
only the 7 pre-existing NO_RECLAIM failures of the base binary (the known
threaded `ConcurrentPeersCapTier1AtConfiguredSampleCount` is flaky in both:
6/10 vs 5/10 over ten runs). `/tmp/pr230-obs-fix/build-taskpool-tests.sh`:
local taskpool suite 91/91. `/tmp/pr230-obs-fix/matrix.py`: 14/14 macro cells
(driver local/shared x VP OFF/ON, pool production / NO_RECLAIM /
code-pointers-OFF, profile merge VP OFF/ON, local taskpool VP OFF/ON, taskpool
layout TU, new test TU default / NO_RECLAIM / code-pointers-OFF). Pre-fix
discrimination: `/tmp/pr230-obs-fix/build-base-probe.sh` links the same new test
TU against the `git show b91cf59e` pool + merge implementation; that baseline
binary fails exactly the cold-peer Tier-2 claim tests (3/3) plus, in NO_RECLAIM,
the concurrent-replacement freeze test (observed `publishedInsideCommit == true`
and the predecessor timestamp stamped on the replacement slot) and the
closed-quota freeze test (request `quotaEnd == 0`) — all 5 pass on the repaired
build. AArch64 BE: `/tmp/pr230-obs-fix/be/*-be.o` rebuilt as `ELF 64-bit MSB,
ARM aarch64` with no `wmemchr`/`memchr` undefined symbol; host layout dump
`request=232 slot=240 state=470336 abi=21`, identical to `b91cf59e` (no shared
layout/ABI change in the repair). Honest gaps: the ORC-engine body of
`EJitCompileDriver::compileCold` is still compile-verified only (the
engine-independent granted-T1 -> queued-T2 -> bundle join is executed); no BE
link/board run; R1-3's peer-capture TOCTOU is closed by construction (snapshot
identity + fail-closed commit) but has no deterministic injection point, so it
is not separately reproduced.

Narrow cross-check repair above `e245b6404c4e` (2026-09-11, pending independent
verification). The coordinator cross-check confirmed R1R-1: the e245 NO_RECLAIM
admission commit took the bucket writer lock on every granted observed dispatch,
so `writeFlag`/`publishSeq` invalidated concurrent load-only seqlock readers.
Under `/home/ruanchen/ejit-dev/build.lock`,
`/tmp/pr230-obs-fix2/build-shared-tests.sh` compiled the current
`EJitSharedTaskPool.cpp`, `EJitSharedPlatform.cpp`, `EJitLogger.cpp`,
`EJitProfileMerge.cpp`, `EJitSharedTaskPoolTest.cpp` and the extended
`EJitObservedT1DispatchTest.cpp` with the exact `EJITSharedTaskPoolTests` target
flags: `/tmp/pr230-obs-fix2/tok/tests` 192/192 PASS (16 observed/bundle: 12 pool + 4 bundle; the file defines 14 pool tests, two of them NO_RECLAIM-only) and
`/tmp/pr230-obs-fix2/nrc/tests` 198 total / 190 PASS, whose 8 failures are the
7 pre-existing base failures plus the pre-existing flaky
`ConcurrentPeersCapTier1AtConfiguredSampleCount` (5/10 head vs 3/10 e245 over
ten single-test runs, same signature as the base binary). The observed filter is
16/16 token and 18/18 NO_RECLAIM; the previously flaky
`ConcurrentReplacementCannotInterleaveTheCommit` is 30/30 in NO_RECLAIM after the
bounded FIFO drain (R2R-02). `/tmp/pr230-obs-fix2/probe-head/tests` (the
reviewer's barrier probe linked against the fresh NO_RECLAIM objects) closes the
threshold-64 quota in 10/10 barrier 8-thread x 32-call runs with
count=hits=64, `quotaEnd=1000` and a post-call `missOpenPost=0` (no miss while
the quota was still open); the same probe linked against the e245 objects closes
only 1/10 and reaches count 48-61 in the rest (R1R-1 reproduced). Pre-fix
discrimination with the current test TU: the e245 implementation objects fail
exactly the two new regressions (seqlock stability, legacy wrong-Tier-2 claim)
and the b91 objects additionally fail the three R2-PR230-01 cold-peer tests.
Layout `/tmp/pr230-obs-fix2/layout/sizes`: `request=232 slot=240 bucket=3904
state=470336 abi=22 obsLockOff=12 slotsOff=16` (no size/offset change).
AArch64 BE `/tmp/pr230-obs-fix2/be/*-be.o`: `ELF 64-bit MSB, ARM aarch64` with
the v22 layout static asserts and no `memchr`/`wmemchr` undefined symbol.
`/tmp/pr230-obs-fix2/taskpool/tests`: local taskpool suite 91/91;
`/tmp/pr230-obs-fix2/matrix.py`: 14/14 macro cells. Code-pointers-OFF variants
compile (matrix) and run the observed filter with the four pre-existing
cold-peer tests failing exactly as with the e245 nocp objects and the new R2R-01
test skipping itself. Honest gaps: host x86_64 only, no configured CMake suite
(ws6 names another source tree), no BE link/board run, and the ORC-engine body
of `compileCold` remains compile-verified only.

Representative-PGO group integration on this host (2026-09-14, current PR230
sources, local Windows clang-cl 21.1.8 build against read-only LLVM component
archives; new files EJitRepresentativeGroup.{h,cpp} and
EJitRepresentativeGroupPgoTest.cpp). EJitRepresentativeGroupTests 3/3 PASS:
(a) one entry, six legal cells, ONE representative Tier-1 with 64 REAL
pool-granted dispatches whose granted Instrumented JIT pointer is really
executed (live counters: 64 entries, 48/16 branch split), the schema is read
from the live __profd_ and the indexed profile is synthesized from the live
__profc_, the REAL queued Tier-2 request supplies the frozen observation, the
immutable bundle is published once and consumed by all five non-representative
cells' PGOUse compiles (entry_count == 64 and real !prof weights in every
member), and all six cells end on ONE physical Tier-2 object (emitter
codeObjects=1, reused=5, real object count 2 = one Instrumented T1 + one shared
T2) that is really executed with raw cell/TRP arguments; the real pool then
serves that same pointer; (b) two groups in one registry keep distinct
generation/session/attempt identities, distinct synthesized profiles (48 vs 16
taken branches, never mixing), independent quota, and a retired group's late
dispatch/publish settles Stale without touching the other group; (c) the V1
gate rejects PGO-off, sync, audit-only, in-flight mode change and zero quota
before any group side effect, and cancellation/re-election/duplicate completion
settle exactly once. Honest gaps: no value-profile (IC/memop/scalar) variant was
enabled in this production-macro build; the group registry is not yet called
from EJitCompileDriver/EJitRuntime, so product sharing stays off; the
queue-full/T2-retry path is still only covered by the earlier request-attempt
tests; host x86_64 only (no BE link/board), and this is author evidence, not
independent review.
