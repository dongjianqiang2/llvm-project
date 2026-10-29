# Local ejit_free_dim value boundary (issue 246, v1)

Baseline: e09df842062735ba9f6279ae7d1c5b43fd723bec.

## Encoding frozen before implementation

Use a compiler-private, typed identity operation encoded as a direct call:
`%local = call i64 @__ejit_local_free_dim_v1_i64(i64 %initializer)`.
Its declaration has string attribute `"ejit.local_free_dim"="v1"`, memory(none),
nounwind, willreturn, nocallback and nosync. The consumer checks exact reserved
name, integer scalar signature, declaration-only shape and version attribute.
This is a pseudo-intrinsic, not a runtime function or SDK API. It deliberately
has no LLVM-known identity simplification: SROA/mem2reg promote its RESULT,
CSE can merge identical marked operations but cannot equate an unmarked input
to a marked result. No global facts about the initializer input are introduced.
Integer width is encoded in the name; no target register constraint is needed.

Insertion is after normal initializer evaluation and before the initializing
store, in CodeGenFunction::EmitScalarInit. Constant-expression emission of
marked locals is disabled so it cannot bypass this boundary. Effects of calls,
volatile/atomic source reads and cleanup expressions are unchanged.

## Extraction and cleanup

EJitRegisterBitcodePass runs before host optimization. Its extracted clone keeps
the operation through its actual SROA, AlwaysInline/cost-based O2 inliner,
mem2reg, EarlyCSE and InstCombine sequence (and lighter PGO extraction mode).
After extraction, an unconditional EJitLowerFreeDimPass in every PassBuilder
AOT pipeline lowers remaining calls to their actual input, even when EJIT
extraction/wrappers are disabled. No marker declaration or symbol registration
may remain in the AOT module. EmitAssemblyHelper::RunCodegenPipeline also lowers
remaining boundaries for actual machine output, covering -disable-llvm-passes
without erasing the marker from explicitly requested raw IR/bitcode.

EJitStructFieldPass::evalWithAssumed recognizes only the operation RESULT and
evaluates it as integer zero without walking/evaluating its initializer.
The original IR, initializer, GEP and ordinary uses are never rewritten to zero.
Existing witness bounds, poison/overflow, alignment, field eligibility, object
extent and registry/lifecycle checks remain authoritative. Unknown PHI/call
chains safely miss folding. Marker inputs cannot authorize sibling SSA uses.

EJitOptimizer keeps the boundary until its last may_const consumption:
Baseline/PGOUse after runOptimizationPipeline; Instrumented after counter
capture and all may_const handling. Each return lowers calls to actual inputs
and clears analyses. The Tier1/Tier2 common profile prefix retains matching
marker shape; cleanup after counter capture must not alter profile CFG.

An old consumer cannot understand the private operation: the new AOT does not
register it as a host symbol, so an old JIT fails unresolved-symbol compilation
and retains AOT fallback. New bitcode is not promised compatible with old
packages. No cache key, ABI, SDK/CMake feature requirement or sampling axis is
added.

## Declaration validation

Retain parameter diagnostics/merge behavior. Extend subject to Var but reject
global/static/thread storage, noninteger, volatile/atomic and non-entry locals.
After the full body is available, require initialization and a managed dimension,
and reject every use other than an lvalue-to-rvalue read (parentheses allowed),
including assignment/increment/address/reference escape and closure capture.
Const and unmodified nonconst integer locals use the same boundary.

## Expected IR

Before specialization:
```llvm
%source = load i64, ptr @runtime_slot
%offset = call i64 @__ejit_local_free_dim_v1_i64(i64 %source)
%marked = getelementptr i8, ptr %cell_base, i64 %offset
%mode = load i32, ptr %marked, !ejit.may_const !0
%raw = getelementptr i8, ptr %cell_base, i64 %source
%unmarked = load i32, ptr %raw, !ejit.may_const !0
```

After successful witness consumption and mandatory cleanup:
```llvm
; mode is replaced by the generation's baked value, e.g. 7.
; raw/unmarked and non-may_const loads still use %source.
; all surviving offset computations also use %source, never zero.
```

Zero must designate the same semantic field and preserve cell/TRP/bound-pointer
identity. All legal offsets must yield the same field value for the whole
managed generation. A numerically in-bounds different field is still an invalid
business promise, not something this compiler can fully prove.

## Object and lifecycle safety

Existing SpecializationContext dimensions carry the selected cell into the
final field pass. They are not new dimensions or runtime assumptions: they
constrain the witness to the already-selected cell when O2 has flattened the
typed cell GEP. Without these facts the consumer requires an independently
recognizable typed cell selection. Marking the cell selector itself does not
authorize eliminating identity. The consuming function must track the actual
period, not merely some unrelated managed period.

Nested constant arrays of records contribute existing may_const field offsets
for both global and bound objects. Traversal has a 4096-node budget; unions,
volatile fields and oversized/unrepresentable offsets remain conservative
misses. Bound reads require an eligible field inventory and valid object range,
including when per-load metadata survives. Checked pointer-index arithmetic,
intermediate inbounds steps, ABI/load alignment and final selected-cell bounds
are checked before reading memory.

The source integration exposed several baseline production-path defects:
binary byte arrays were incorrectly required to have a trailing NUL; imported
mutable globals retained dso_local (out-of-range AArch64 ADRP relocations);
audit cleanup left dead ConstantExpr counter addresses; and persistent
MemorySSA outlived materialized modules. The narrowly scoped fixes are covered
by the new integration/full EJIT regressions. Native compile failures consume
their LLVM Error and preserve fallback rather than aborting.

## Source and observed IR

Complete executable sources:
- `ejit_test/ejit_local_free_dim_test.c`: real frontend wrapper, embedded O2
  bitcode, online shared-worker PGO, two cells, ten changing offsets, and refresh.
- `ejit_test/ejit_local_free_dim_sre_test.c`: single-C core6/core16 board demo.

Both use the existing runtime header/macros and initialization. The native
ELF test uses constructor registration; the board demo uses existing static
registry linker bounds. No additional SDK API or initialization adapter exists.

The native source offset is initialized by an always-inline helper that
increments a volatile counter and reads `local_host_runtime.slot`. Its extracted
O2 IR retained the boundary after that helper was inlined and locals promoted:

```llvm
%slot = load i32, ptr @local_host_runtime, align 4
%wide = zext i32 %slot to i64
%bytes = shl nuw nsw i64 %wide, 3
%offset = tail call i64 @__ejit_local_free_dim_v1_i64(i64 %bytes)
%selected = getelementptr inbounds [2 x %struct.HostCell],
    ptr @local_host_cells, i64 0, i64 %cell_index
%p = getelementptr inbounds i8, ptr %selected, i64 %offset
%mode = load i32, ptr %p, align 4, !ejit.may_const !0
%payload_ptr = getelementptr inbounds i8, ptr %p, i64 4
%payload = load i32, ptr %payload_ptr, align 4
```

For cell0's PGOUse, the saved optimized IR contains the original volatile
load/store counter effect, the real slot read and these remaining computations
(names shortened; no marker declaration remains):

```llvm
%bytes = shl nuw nsw i64 %wide, 3
%p = getelementptr inbounds i8, ptr @local_host_cells, i64 %bytes
%payload_ptr = getelementptr inbounds i8, ptr %p, i64 4
%payload = load i32, ptr %payload_ptr, align 4
%result = add i32 %payload, 700
ret i32 %result
```

Cell1's refreshed generation similarly bakes 1900, not cell0's 700 or its old
1100. The source test checks the first cold call before waiting for compilation.
Its observed run had six compilations (T1/T2 for each of two cells and one
refreshed cell), 223 cache hits, and 514 initializer calls. Each of the three
profile rounds reported 64 real executed root entries. Hit/call totals are
scheduler-dependent; six compilations and correctness are assertions.

## Verification and acceptance limits

Verification ran on Linux AArch64 through the global build-limited.sh lock,
at most eight jobs. Native Release configuration keeps LLVM assertions,
EJIT_SRE_PGO_BRANCH_AUDIT and EJIT_DIAG_ENABLE ON. A verification-only source
override adds NDEBUG to EJitRegisterBitcode.cpp, exercising its actual Release
preoptimization, while other LLVM assertions remain enabled. The existing
optional EJIT_STATS_ENABLE is applied to LLVMEJIT/tests for count assertions;
it is not a new product or CMake requirement.

Focused CTest uses 120-second local, 90-second parameter, 180-second frontend
and 300-second full EJIT limits. Source integration is bounded at 120 seconds
with a separate 10-second LLVM-parser check of the saved optimized IR.

| Requirement | Evidence and remaining limits |
| --- | --- |
| LFD01-LFD02 | Real annotated global-struct source, ten slots, two cells, payload changes; parsed PGOUse IR bakes 700/1900 with dynamic payload. |
| LFD03-LFD05 | Baseline/ORC mixed-same-SSA test, ordinary store/call/branch IR, initializer call counts, volatile/atomic source-read IR checks, no initializer execution by compilation. |
| LFD06 | Clang actual O2 extraction and inlining, promoted marker boundary, integer conversion/GEP, mixed select safely retained. Complex PHI/loops/calls remain conservative misses. |
| LFD07 | Unknown extent, overflow, previous-cell witness, misalignment/unmarked-field negatives; unrelated lifecycle refuses folding. Semantic wrong-field promises cannot all be proved statically. |
| LFD08 | Cell independence, joint local/parameter zero witnesses with existing cell/TRP bound objects; no new identity axis. |
| LFD09 | Real online deactivate/update/reactivate and regenerated cell constant. Existing taskpool generation/publication protections unchanged; no new local-specific forced concurrent board race claimed. |
| LFD10 | C/C++/Blocks declaration diagnostics and existing parameter regressions; const/nonconst accepted, mutation/address/reference/closure escapes rejected. |
| LFD11 | Baseline, executed ORC Gen/Use, normal queued online PGO/refresh; unknown private-marker version fails cleanly to fallback. |
| LFD12 | Unavailable: no may_const value verifier exists in frozen baseline; PR237 is independent. No witness-vs-itself comparison or fabricated verifier result. |
| LFD13 | Native C source link/execution; native and genuine big-endian demo objects; BE consumer component compile/relocatable link; BE AOT-only static ELF with zero undefined symbols. Actual SDK final-package link pending. |
| LFD14 | Demo source and compile evidence only. Real worker6/owner16 board execution/IR/ASM inspection pending. |

Native results: 15 local unit tests, 16 focused existing parameter regressions,
six frontend tests; full EJIT suite 216 total, 215 passed and one explicit skip
for an existing code-pool-only stress fixture in the code-pool-OFF configuration.
The focused counts overlap the full suite and must not be summed as unique tests.
Two source integration/IR checks and three native AOT execution variants
(C, C++, and disabled LLVM passes) also pass. There are 91 passing taskpool
tests and nine passing focused shared lifecycle/binding tests, including
deactivation/generation change during compilation.

The supplemental full shared-taskpool suite is NOT passing: token mode runs
162 tests with 140 passed/22 failed and two additional disabled; the existing
no-reclaim/code-sharing test opt-in runs 162 with 155 passed/seven failed and
two disabled. Both failure sets reproduce from the frozen base's four unchanged
source files in a task-owned detached tree using the same compiler flags and
unchanged Support/gtest libraries. The no-reclaim XML failure-name sets match
exactly. These baseline failures are retained as residual risk, not fixed by
silently changing unrelated taskpool behavior or disabling audit.

BE evidence uses target aarch64_be-none-linux-gnu and the retained read-only
GNU 13.3.1 sysroot for two affected C++ consumer translation units, with the
existing SRE/code-pool/taskpool/shared/no-reclaim macros and audit ON. These
GNU-header components use -fhosted plus EJIT_FREESTANDING, not an SDK-identical
build. The single-C demo and AOT-only object use -ffreestanding and target
aarch64_be-none-elf. The consumer link is relocatable, not a final SDK package.

## Board procedure (not executed)

The hardened PR247 single-C artifact and current build/startup/acceptance
instructions are in [EJIT_LOCAL_FREE_DIM_BOARD.md](EJIT_LOCAL_FREE_DIM_BOARD.md).
That guide supersedes the abbreviated procedure below, including shared startup
gating, bounded real PGO dispatches, quiescent renewal and read-only printing.

1. Build the single-C SRE demo into the established EJIT package without new
   SDK macros or initialization APIs; retain branch audit/required diagnostics.
2. Reset. Run `test_ejit_period` on core6 first; it initializes the shared
   cells, calls the established init-array flow and ejit_init_pgo, verifies
   worker6, and requests the entry dump.
3. Run `test_ejit_period` on core16. Observe first cold slot9 calls for both
   cells before bounded compilation waits; verify all ten offsets, initializer
   counts, payload values, then cell1 deactivate/update/reactivate and cell0
   independence. Failure/timeout is a failure, not prewarm acceptance.
4. Keep worker6 alive. Run `test_ejit_local_free_dim_print` on core6 and retain
   IR/ASM/statistics: mode baked for the appropriate generation, payload
   addressed by the real slot, no marker call in generated code, real PGO counts.
5. Record SDK final link and actual cold-start/refresh outputs before claiming
   LFD13 platform acceptance or LFD14. No board acceptance follows from host
   tests or BE component compilation.
