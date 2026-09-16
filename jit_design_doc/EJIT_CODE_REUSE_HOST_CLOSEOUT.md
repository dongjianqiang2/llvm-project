# PR230 Host Closeout - 2026-09-16

Status: host implementation and the latest legacy concurrency follow-up are
published for review. The feature remains OFF by default and the PR
remains Draft. This is not product, board, or whole-specification acceptance.

## Audit-Enabled Initialization Repair

The board's `representative sharing requires Async + normal online PGO`
initialization failure was reproduced on the published source with
`EJIT_SRE_PGO_BRANCH_AUDIT` and `EJIT_DIAG_ENABLE` enabled. Both the early
constructor guard and driver group admission incorrectly treated the audit
diagnostic flag as audit-only mode, even though the compile pipeline defines
audit-only as `!enablePgo`.

Both gates now accept Async online PGO with audit diagnostics still enabled.
PGO-off (with or without audit), Sync/Off and zero-timeout configurations remain
rejected before registration consumption or worker creation. No public ABI,
build-option default, board example, sampling quota or product sharing default
changes. Keep the board's audit option ON and keep using
`ejit_init_representative`; do not substitute ordinary `ejit_init_pgo`.

Fresh audit-enabled generated-code runtime suites pass 6/6 for NRC and 6/6 for
VP+NRC; a separate audit-disabled NRC compatibility control also passes 6/6.
These cover explicit initialization/group activation, real 64-dispatch
T1 sampling, frozen profiles and shared T2, 120-identity equal/unequal cases,
borrow-fenced renewal, failure controls, and ordinary-init sharing remaining
off. Logs retain both branch-audit and mayconst-audit output with 64 observed
entries/hits. The original failing executable/logs are retained separately.
Evidence: `reports/audit-init-fix-20260916/` under the PR230 control root.
This is Windows x86-64 execution of generated code with fresh EJIT translation
units and retained non-EJIT LLVM libraries, not SRE board acceptance.

## Rebase Onto PR233

On user request, all eleven published PR230 commits were replayed onto PR233
`812f6474b706e0bb8f0cd1b93e9b0413bd5457cf`. The former remote head
`25b36dfee5a138832013e5542cdc813cbb792f79` is retained on
`codex/ejit-spec5-code-reuse-backup-before-233-20260916` in the head repository.
The previous local development branch is also retained. No spec5/PR233 history
is rewritten, and PR231 is not imported.

The only explicit conflict was in the common optimizer prefix: retain PR233's
`applyBoundPointerFacts` call alongside PR230's load-only preserved-dimension
policy. The remaining ten commits replayed without semantic patch changes.

Fresh current-source checks: shared pool NRC 205/205, token 187/187, and the
folding/optimizer/preserved-dimension/prepared-code selection 129 passed with
two ELF-only native cases skipped on Windows. Real runtime NRC and VP+NRC
each pass 5/5,
including a new single-producer round-robin of all 120 identities followed by
cell deactivation, compiler-borrow completion, mutation and reactivation. The
20 renewed entries reuse the original T2 pointers without new physical code or
new representative samples. This complements the earlier concurrent sampling
pressure test; it does not replace it.

The header-free board source and startup/acceptance instructions are in
[EJIT_CODE_REUSE_BOARD.md](EJIT_CODE_REUSE_BOARD.md). Mock tests passed with
both startup modes (including a real-public-header build); the attribute-disabled
AArch64 BE syntax/object check also passed. That object check does NOT test EJIT
attributes, generated wrappers, bitcode registration, JIT codegen or SRE execution.

Current evidence: `reports/rebase-233-20260916/` under the PR230 control root.
The initial pipeline harness failures are retained: the wider EJitPgoTest TU
requires code-pool APIs absent from this host configuration, and the first link
lacked the same host libcall/AsmParser adapters already used by the runtime
suite. The final focused build excludes that unrelated TU and uses those
host-only adapters; production code was not changed to satisfy the harness.
The older stress counts below describe the pre-rebase closeout, not new runs.

## Implemented Host Path

The production runtime classifies candidates before representative sampling,
executes generated Tier-1 code, freezes the group's profile, and compares final
IR plus effective bindings before sharing physical Tier-2 code. Host coverage
includes 20 entries across six cells, unequal-member separation, profile/session
isolation, cancellation, re-election, source-borrow completion, queue rejection,
compile failure, stale callbacks, and token/NO_RECLAIM variants.

Bound-pointer requests still use conservative independent compilation. The
IR01 may_const board sample is a separate feature test, not PR230 acceptance.

## Failed-Preparation Lock Repair

The NO_RECLAIM legacy cold path used Tier-2 eligibility coordinates to select
the settlement lock. Failed peer preparation cleared those coordinates while
preserving the real cache slot, allowing a nonzero-bucket slot to be charged
under bucket zero's lock.

The fix carries independent legacy bucket/slot coordinates, validates bounds
and exact slot ownership, and then checks the captured publication identity
under the real bucket's observation lock. Failed preparation still grants no
Tier-1 execution and must not arm Tier-2. Token behavior and shared-state ABI
are unchanged. Five permanent tests cover successful/failed generic lookup
and failed fixed 0D/1D/2D lookup with the actual nonzero-bucket lock held.

An independent, read-only Luna xhigh review found no actionable issue in this
narrow production patch. It did not independently run tests, review the later
stress-classification change, or accept the entire feature.

## Concurrent Stress Classification

Before changing any acceptance condition, the original concurrent test was
instrumented with per-thread records printed after join. The original strict
assertions passed 977/1000 independent processes. The 23 failures contained
24 captured status-5 (`PgoAdmissionDeferred`) returns, all with the expected
AOT pointer, no read token, and exactly 64 total Tier-1 grants. Their original
logs, executable, source snapshot, and hashes are retained.

The bounded seqlock lookup can exhaust its retries while the function already
owns a PGO admission. Its slow path explicitly returns an AOT admission
deferral; it must not be reported as a new queued compilation or a T1 grant.
The test previously accepted only `AlreadyPending` as AOT.

The corrected test accepts only that exact documented deferred result shape,
not arbitrary non-hits. It additionally validates the Tier-1 pointer, absence
of fallback read tokens, deferred-miss accounting, and all 256 calls. The
exactly-64 grant and hit-count assertions, single queued T2, and single T2
compile remain. No production status or quota was weakened to pass this test.
These are mock-pointer pool tests, not 64 generated-code executions.

Fresh capture establishes the current failure mechanism; it does not recover
the unrecorded statuses of historical failures or erase earlier quota defects.

## Verification

| Check | Result |
| --- | --- |
| Focused original probes and lock/publication controls, before test-only closeout | 18/18 |
| Final-source shared pool, NO_RECLAIM, all target TUs rebuilt | 205/205 |
| Final-source shared pool, token, all target TUs rebuilt | 187/187 |
| Corrected concurrent NO_RECLAIM test, independent processes | 1000/1000 |
| Corrected concurrent token test, independent processes | 100/100 |
| Real representative runtime, NO_RECLAIM | 4/4 |
| Real representative runtime, VP + NO_RECLAIM | 4/4 |

Runtime results are the September16 fresh 29-TU builds. Their production/test
inputs and EJIT headers were rehashed against the closeout source; the later
edit affects only the standalone shared-pool test, not either runtime target.
Non-EJIT LLVM libraries were retained. Evidence is Windows x86-64 only.

Local evidence directories under the PR230 control root:
`reports/failed-prep-repair-20260916/` and `reports/closeout-20260916/`.
The latter retains strict-capture and corrected results separately.

## Remaining Gates

- Current-source AArch64 big-endian artifacts, ELF/dependency inspection,
  stable-wrapper comparison, cross-core permissions and actual board execution.
- Execute the supplied PR230 single-C six-cell/twenty-entry board scenario;
  product correctness/performance/resource acceptance from sections 9/11.
- Bound-pointer representative source borrowing/shared-code support; current
  fallback is independent compilation, not a claim of shared support.
- Broader whole-feature review and final commit organization before merge.

Publishing this implementation does not enable the feature or authorize merge.
