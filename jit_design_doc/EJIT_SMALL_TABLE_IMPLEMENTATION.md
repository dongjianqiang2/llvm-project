# EJIT Small Table — Implementation Map, Status and Evidence

Base commit: `027a151b7d8b5b44446dc055c963b1a782754fbb` (documentation-only PR231 on
spec5 `3f0e190dd54752f9c1dcb4bcd1fcb5d6349ae59c`), branch
`codex/ejit-small-table-deepseek`. Milestone A (compiler side) of
`EJIT_SMALL_TABLE_SPEC.md` / `../EJIT_SMALL_TABLE_SPEC.latest.md`.

Current gate: **29/29 focused gtest tests pass on the local Windows host**,
including real ORC-engine compiles whose IR transform runs the small-table pass
and executes the emitted table loads against the AOT reference. Two
configurations are built from this checkout against the read-only local LLVM
archives: NDEBUG (product flags) and `-DEJIT_DIAG_ENABLE`. Logs (runner dir,
outside git): `build/pr231-local/test-run-20260914.log` (NDEBUG, 29/29, exit 0)
and `build/pr231-local-diag/test-full-20260914.log` (diag, 29/29, exit 0);
compile logs `build/pr231-local/compile-resume-20260914.log` and
`build/pr231-local-diag/compile-resume-20260914.log`. The earlier
`build/pr231-local/test-run-baseline-20260914.log` (17/24) is preserved as the
pre-fix host/accommodation baseline.

Provenance of this gate: the 26 production EJIT translation units and the focused
gtest TU are compiled from this checkout with one consistent macro set
(`build-local.ps1`, flags recorded from `build_batch4/compile_commands.json`:
clang-cl `/O2 /Ob2 /DNDEBUG -std:c++17 /MD /EHs-c- /GR-`, `EJIT_SRE_CODE_POOL`,
`EJIT_SRE_SHARED_TASKPOOL`, `EJIT_SRE_SHARED_CODE_POINTERS`,
`EJIT_CODE_POOL_4K_SEAL`, `EJIT_ICACHE_*`, `EJIT_SRE_TASKPOOL_*`) and linked with
`lld-link` against `build_batch4`/`build_batch2` non-EJIT archives read-only.
No batch4/ws6 `LLVMEJIT` object is used, because those EJIT objects are a
different revision with different class layouts.

Changed/added source hashes verified in this run (sha256):

```
c1e030361bbf8b3cab0c8a4a92654753f9e3c66947aeb8d79d5b437b8ddf4549  EJitSmallTable.h
920ac17ca8e8f7e3cfa94e241e233099f782d3a189e505106c41fce62fbeb3dd  EJitSmallTable.cpp
e08349dfdba7ad9655e59fa77339c3768436e6f7c0b72d40db1c2103519a29a9  EJitSmallTableTest.cpp
b6bef6fa97ece623e56602255bf3426a0e147c2c235f96539f01c37f53e8675f  EJitStructFieldPass.h
2fbf27f230e73a0bb8963851bf3db07aeaffdac5cf09cfb254467383bb0171a0  EJitStructFieldPass.cpp
b629eeb9a5accd9c5ad27ae3ac76129ad3e9d1a38607921e0727511ca5686218  EJitOptimizer.h
e513dfae7cac3dd058f902054ddcbfedbea3367efe1c0d5dbce4fe92d41d2a47  EJitOptimizer.cpp
e2be8012bc8de20359c8ad0ce04c4bcf5c31e2ef4ca4073f309275264919878b  EJitOrcEngine.cpp
```

Focused test binaries (sha256): NDEBUG
`ffa7a445d43e13e2f7a0f7cb647c057848ebdc8019003764b75ef5317df6b5f2`,
diag `50d4698e48fd10a1268d8298e6e50689cb084db37023fd91008dd3fb1bd9d9fb`.

Only the coordinator can mark a row `independently-verified`; worker state stops
at `implemented`.

Legend: `planned` / `partial` / `implemented` / `independently-verified` /
`blocked`.

## 1. Actual pipeline map (read from this checkout, not invented)

| Stage | Real symbol / file | Small-table relevance |
| --- | --- | --- |
| AOT metadata | `EJitRegisterBitcode.cpp`, `EJitRegisterPeriod.cpp` (`MD_EJIT_METADATA`, `MD_EJIT_MAY_CONST`, `TAG_EJIT_MAY_CONST_FIELD`, `TAG_EJIT_PERIOD_ARR_IND`) | Candidate authorization; a plan field must also be authorized at the load (`EJitSmallTable.cpp:isAuthorizedMayConstLoad`) |
| Compile context | `SpecializationContext` (`EJitOrcEngine.h`) | `fnName`, dimensions, bound pointers, tier |
| Engine seam | `EJitOrcEngine::Create` -> `EJitOptimizer` (EJitOrcEngine.cpp:824); `EJitOrcEngine::setSmallTablePlans` (EJitOrcEngine.cpp:1289) | Where a plan set reaches the optimizer; default = empty = feature OFF |
| Round 1c | `EJitOptimizer::runPipeline` -> `preReplacePeriodIndices` -> `runInstCombine` -> `runStructFieldPass` | Plan dims stay dynamic here; small-table pass runs before the field pass |
| Propagation | `runInterproceduralPropagation` (IPSCCP, internalize non-entry defs) | Entry stays external, so dims stay real; callees keep baseline |
| Round 1f | `runInstCombine` -> `runStructFieldPass` | Fresh pass instances per round; plans are immutable value snapshots (no `Value*` retained) |
| Core opts | `runOptimizationPipeline` (LowerExpect + O1/O2/O3 `buildFunctionSimplificationPipeline` + `pgoUseFPM_`) incl. the inliner | InstCombine rewrites `bitcast (load float)` into `load i32` (handled); inlined fields at covered offsets become table reads |
| Round 3 (phase 4) | `runOptimizationPipeline` -> `runSmallTablePass` -> `runStructFieldPass` -> `cleanupFPM_` | Last replace round; table globals must survive, unready plans are refused before this point |
| Emission | `EJitOrcEngine` + `orc::LLJIT` + JITLink, code memory via `EJitCodePoolMemoryManager`; transform-created column globals are claimed through the same `symFlags` + `defineMaterializing` protocol as the PGO counters (EJitOrcEngine.cpp:983-1005) | Table is a plain mutable module global (data section); direct `adrp`+`add` on AArch64 BE (child gate); `lookup` resolves the table address via `getLastSmallTableColumnNames()` for runtime row publication |
| Code execution gate | `EJitCodePoolManager::flushPendingRanges` via `EJitOrcEngine::flushPendingCode` (product: shared-taskpool batch-flush thunk) | Batched 4K seals are deferred; code must be flushed after lookup before the first call |
| Analysis lifetime | `EJitOptimizer::clearAnalyses` after `runPipeline` in the engine transform | Persistent managers must drop analyses while the module is alive (pre-existing teardown crash in the real compile path) |
| Source values | `PeriodArrayRegistry`, `EJitBoundPointerView` | Borrowed source region; the planner copies scalars and never retains the pointer |

Replace rounds are exactly three (`runStructFieldPass` at 1c, 1f, and phase 4);
the small-table pass runs immediately before the existing field pass in each, so
no round can regress a table read back to a source load. Plans are value
snapshots and the source shape is re-derived from the current IR in every round,
so no `Value*` survives inline/cleanup.

## 2. Requirement table

| ID | Spec | Implementation file / symbol | State | Test + command | Provenance | Evidence | Risks / next gate |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ST-A1 | §9, §15 | `EJitSmallTablePlan{,Set}`; `EJitOptimizer::setSmallTablePlans`, `EJitOrcEngine::setSmallTablePlans` (default empty) | implemented | `PipelineFeatureOffKeepsBaseline`, `JitFeatureOffKeepsBaselineCorrect` | base `027a151b`; flags from ws6 `compile_commands.json` | `build-obj/test-run-2026-09-11T06-51-28Z.log` | OFF path must stay byte-identical; both OFF tests pass |
| ST-A2 | §1.2, §8 | `EJitSmallTablePlanner::planShape` / `plan` per-entry field+schema plan | implemented | `PlannerBuildsColumnsAndRowsFromMemory` | same | same | Fields discovered pre-inline only (ST-A15); 3x4B columns over 120 rows vs 120x1KiB source asserted |
| ST-A3 | §5.2, §5.3 | borrow region extent, per-ready-row bounds, `MaxRows`/`MaxTableBytes`, overflow-checked row count | implemented | `PlannerRefusesOutOfRegionAndUnknownShape`, `PlannerRefusesOversizedAndDuplicateDomains` (incl. declared-extent mismatch) | same | same | No borrow/lifetime protocol (ST-B2) |
| ST-A4 | §4, §6.5, §9 | `EJitSmallTablePass::materialize`/`run`: indexed load from `@__ejit_stab_*`, mutable `dso_local` data | implemented | `PassReplacesWithDynamicIndexAndKeepsArguments`, `PipelineKeepsTableLoadsThroughAllReplaceRounds` | same | `build-obj/test-run-2026-09-11T06-58-21Z.log`; `children/aarch64be2/out/README.md` | Non-`inbounds`, unknown shapes and whole-element constant hops stay original |
| ST-A5 | §9 | `runSmallTablePass` before `EJitStructFieldPass` in rounds 1c/1f/phase 4; idempotent `materialize` | implemented | `PipelineKeepsTableLoadsThroughAllReplaceRounds` (3 loads after all rounds) | same | same | `materialize` validates all columns before creating any |
| ST-A6 | §4, §5.6 | dims matched against the real `Argument` / `urem(arg,mod)`; `preReplacePeriodIndices` skips only the planned entry's dim args | implemented | `PlanKeepsDeclaredDimensionDynamic`, `PassReplacesWithDynamicIndexAndKeepsArguments` | same | same | Skip keyed on arg index within the entry only |
| ST-A7 | §5.6 | ordinary loads, store addresses, helper args unchanged | implemented | `JitLiveLoadStoreAndHelperStayDynamic`, `UniformContractFoldsOnFullyReadyPlan` (live store) | same | same | Only the former load's users see the replacement |
| ST-A8 | §6.5 | pure-table: late different row values observed without recompilation | implemented (single module) | `JitLateDifferentRowValuesAreObserved`, `UniformContractFoldsOnFullyReadyPlan` (varying column) | same | same | Cross-tier table identity is ST-B3 |
| ST-A9 | §4, §14.3 | real `slotNo` 0..1023, `%10` and `%5`, two full wraps, `cell`/`trp` dynamic | implemented | `JitDynamicRowsMatchAotAcrossTwoSlotWraps`, `JitSlotModuloFiveTwoFullWrapsMatchAot`, `UniformContractFoldsOnFullyReadyPlan` | same | same | No clamp for out-of-domain values (documented) |
| ST-A10 | §6.6 | explicit uniform admission contract: caller-provided, checked against every ready row, **at least one ready row must confirm it**, never inferred | implemented (compiler half) | `UniformContractIsExplicitAndFolds`, `UniformContractIsNeverInferredFromEqualVisibleRows`, `UniformContractIsCheckedAgainstEveryReadyRow`, `UniformContractFoldsOnFullyReadyPlan` | same | same | Runtime cold-path validation of a later member is ST-B2 |
| ST-A11 | §5 | i1/i8/i16/i32/i64, float/double bit patterns incl. -0.0 and NaN payload; BE decoding; bitcast-punned views | implemented (host) / verified artifact (BE) | `PassCoversScalarWidthsAndFloatBits`, `PassMatchesBitcastPunnedScalarView`; BE bytes in child gate | same | `build/pr231-local/test-run-20260914.log`; `children/aarch64be2/out/verify_data.txt` | No host unit test with a BE `DataLayout` (child gate covers it) |
| ST-A11b | §5 "整数保留位宽" | **R1 F1**: a sub-byte integer load's typed value is the low `bitWidth` bits of the `accessSize` bytes it reads. `readScalarBits` masks to `bitWidth`, so a non-canonical byte (0xFE over an i1 field) never reaches `APInt(bitWidth, …)`: under assertions that construction aborted and under NDEBUG it truncated silently | fixed | `PlannerMasksSubByteFieldsToTheirWidth` (row bits masked; i1/i8/i16/i9 columns keep separate, correct values), `UniformContractIsRefusedOutsideTheFieldWidth` | same | `build/pr231-local/test-run-20260914.log` | canonical i1 (0/1 byte) is unaffected; the mask is a refinement in both domains. Poison/undef cannot appear: a table row is a byte copy of a stable, authorized memory location (spec §5.2), and IR-level `undef`/`poison` is a compile-time value with no storage for the copy to observe |
| ST-A11c | §5 | **R1 F2**: site identity is `(offset, accessSize, bitWidth)`. Deduplicating by `(offset, accessSize)` alone let a wider load share a narrower column and be coerced with `zext` (order-dependent silent wrong code); `fieldMatchesType` is now width-exact, so the only coercion left is identity or a same-width `bitcast` (int ↔ float/double) | fixed | `JitMixedWidthSitesKeepTheirOwnColumns` (real ORC execution of i1+i8+i16 at one offset, i9 at another, against the source values), `PlannerMasksSubByteFieldsToTheirWidth` | same | `build/pr231-local/test-run-20260914.log` | An integer column still serves a same-width integer view and a same-width float/double view; no cross-width fold is attempted |
| ST-A11d | §6.6 | **R1 F3**: a uniform admission contract must be a value of exactly the field's width. A raw storage byte (0xFE) is refused before it can be truncated; an in-range value is still only admitted when every ready row's masked bits confirm it, and the folded constant is the typed i1 value | fixed | `UniformContractIsRefusedOutsideTheFieldWidth` (0xFE refused, 0 admitted and folded to i1 `false`, raw 0xFE never appears as the folded value) | same | `build/pr231-local/test-run-20260914.log` | Contract vector size is validated against the field count before any indexing (pre-existing check, exercised by `PlannerRefusesOversizedAndDuplicateDomains`) |
| ST-A12 | §5.5 | volatile/atomic/AS!=0/pointer/aggregate/vector/>64-bit, non-inbounds, `inttoptr`-rooted, non-`urem(arg,mod)`, foreign global, whole-element constant hop, unsupported element types refused | implemented | `PassRefusesVolatileAtomicAndUnsupportedShapes` (AS1 + `inttoptr` included), `PassRefusesWholeElementConstantHop`, `PlannerRefusesOversizedAndDuplicateDomains`, `MaterializeFillsDeclaredSlotsAndRefusesForeignOnes` | same | same | Constant-index overflow and out-of-element hops refused, not aliased |
| ST-A13 | §14.8 | AArch64 **BE** freestanding object: table in `.data`, entry in `.text`, `ADR_PREL_PG_HI21`+`ADD_ABS_LO12_NC` for every table symbol, no GOT for them, no libc/host deps, BE bytes verified | implemented (artifact gate, re-run on post-change snapshot) | `children/aarch64be2/out/checks_report.txt` (checks a-e PASS, exit 0); llc ws6 21.1.8 + GNU readelf 2.46 | pass object compiled by the child from snapshot `1ba57dc5…`/`db8027d0…` (`EJitSmallTable.snapshot.o` `f29c2479…`) | `children/aarch64be2/out/pass_emitted.o` (`5fda0c37…`, byte-identical codegen to the pre-change artifact; only the new load metadata differs), `readelf_*.txt`, `verify_data.txt`, `README.md` | Object is **not** GOT-free overall: `g_cfg`/`g_out` keep `ADR_GOT_PAGE`+`LD64_GOT_LO12_NC`; only the `__ejit_stab_*` symbols are direct. No board execution, no SRE sysroot link |
| ST-A14 | §13 | strategy counters: may_const/table/uniform/kept/refused-shape + `refusedNotReady`; per-entry ready/rows summary | partial | counters asserted in the pass tests | same | same | Per-load refusal reasons and table byte reporting not logged into the product diagnostics yet |
| ST-A15 | §9 (inline) | inline-exposed fields: covered columns are replaced post-inline, uncovered stay original; schema frozen at plan time | partial | `NestedCalleeLoadsStayOutsideThePlan` | same | same | New columns discovered only after inlining need a re-plan (milestone A gap) |
| ST-A16 | §6.5 readiness | partial plans are **refused for executable lowering**: no 0/undef/placeholder row is ever emitted, and no contract is folded from an incomplete domain | implemented (compiler half) | `PartiallyReadyPlanIsRefusedUntilRuntimeAdmission`, `JitPartiallyReadyPlanKeepsAotBehavior`, `UniformContractIsExplicitAndFolds` | same | `build/pr231-local/test-run-20260914.log` | **Online partial-row execution is blocked on ST-B2**: incremental `tableReady(row)` admission + a caller attestation are required before this refusal can be relaxed |
| ST-A18 | §4.1, §5, §6.6 | **whole-entry readiness**: when a plan is lowered for an entry, that entry's legacy compile-time `may_const` fold is blocked for that function (`EJitStructFieldPass::blockLegacyConstantFolds`, applied by `runSmallTablePass`'s lowered result in all three replace rounds). Code shared across the plan's declared domain then contains only plan-backed reads (`!ejit.smalltable.column` / `!ejit.smalltable.contract`) and real dynamic source reads; an unmatched legacy fold can no longer hide behind a partial plan. Entries without a lowered plan keep the baseline fold | implemented (compiler half) | `LoweredPlanBlocksTheLegacyFoldItDoesNotDescribe` (registered period array with a declared may_const field: folded without a plan, kept as a real load with one), `PipelineFeatureOffKeepsBaseline` | same | `build/pr231-local/test-run-20260914.log` | Fold blocking is per-function (the planned entry); callees keep the baseline fold and are only reachable through the entry. Blocked loads are counted (`getLegacyFoldsBlocked`) and keep their original dynamic form; they are **not** claimed by the plan |
| ST-A19 | §6.5, §11 | **per-compile table identity and handoff**: each compile has its own JITDylib (`spec_base_<cacheKey>`) and materializes its own table objects, so the same column spelling is a different object/address in another compile. The compiler exposes the exact names it lowered through `EJitOrcEngine::getLastSmallTableColumnNames()` (also used for the ORC symbol claim); a name resolves inside its own compile and not through a foreign cache key | implemented (compiler half) | `TableIdentityIsPerCompileAndHandedOffByName` (names handed off, resolvable in their own compile, not process-wide, address stable across repeated lookups within the compile) | same | `build/pr231-local/test-run-20260914.log` | **The compiler cannot and does not claim a stable table address across compiles**: B must publish a member's rows into the table instance of the compile it is admitting to, or the row lands in a different object. One shared per-entry/epoch table for T1/T2 remains ST-B3 |
| ST-A17 | §9 provenance | synthesized table load carries `!ejit.smalltable.load` (entry + source) and never `!ejit.may_const`, so a later round cannot re-extract it | implemented | `PassReplacesWithDynamicIndexAndKeepsArguments` | same | same | Tag is informational; the pass already refuses foreign roots |
| ST-B1 | §6.2, §6.5 | runtime plan construction from the product config-completion/activate point | planned (milestone B) | — | — | — | Needs a real product readiness signal, not activate alone |
| ST-B2 | §6.5, §6.6 | row readiness/publication + **runtime entry gate** + cold-path admission validation before binding a logical slot | planned (milestone B) | — | — | — | The runtime entry gate ST-A18 leaves to B, all four parts required: (1) dispatch a logical slot to this code only when every row the code can index is committed and published into **this compile's** table instance (ST-A19); (2) for each recorded `!ejit.smalltable.contract` entry, validate the incoming member's field value against the recorded bit pattern before the call — this is the only thing standing behind a folded uniform constant, since the compiler verified the contract only against the rows it could read at plan time; (3) when the plan is not fully ready or the contract fails, do not enter the specialized body at all (the AOT body, which has its own frozen values, is per-member and safe); (4) never present a plan as protecting a load the plan does not describe — ST-A18 makes the compiler side of that true by keeping such loads dynamic. An unauthorized member must never reach this code (ST-A16 currently refuses instead) |
| ST-B3 | §6.5, §11 | one stable table address per entry/epoch shared by T1/T2 and later members; publication ordering | planned (milestone B) | — | — | — | Today each compile materializes its own table from the plan snapshot; a row published into a T1 table is a hole in a freshly compiled T2 table |
| ST-B4 | §10 | common T1/T2 PGO, 64 real dispatches per entry/epoch, session/schema isolation, bounded cold-entry expiry | planned (milestone B) | — | — | — | No per-cell quota, no representative-only policy |
| ST-B5 | §7 | update path: old code/tables survive in-flight calls, NO_RECLAIM retention accounting, no RW reopen of published pages | planned (milestone B) | — | — | — | Table is plain data (never in the RX code pool); writability depends on the 4K preset |
| ST-B6 | §10, §13 | unsupported mode combinations rejected before side effects; default OFF preserved | planned (milestone B) | — | — | — | Default OFF is already the compiler behavior |
| ST-B7 | — | diagnostics transport (counters/reasons to the product log) | planned (milestone B) | — | — | — | ST-A14 is the local half |

## 3. Test inventory (all in `llvm/unittests/ExecutionEngine/EJIT/EJitSmallTableTest.cpp`)

29 tests, grouped: planner (5), pass shape/refusals (6), optimizer pipeline (4),
real ORC execution (7), readiness/contract boundaries (4), scalar-width
regressions (2), whole-entry readiness (1), inline coverage (1), per-compile
table identity (1). The widths the suite exercises at runtime with real ORC
materialization are i1/i8/i16/i32/i64, `i9` (sub-byte, two-byte storage), float
and double.

Host build + run commands (runner dir; one compiler and one linker at a time):

```
pwsh -File build-local.ps1 -All                                            # 26 EJIT TUs + gtest
pwsh -File build-local.ps1 -Link -Tu EJitSmallTableTest.cpp                # relink
build/pr231-local/EJitSmallTableTests.exe --gtest_color=no                 # 29/29
pwsh -File build-local.ps1 -All -Diag                                      # assertions-independent
build/pr231-local-diag/EJitSmallTableTests.exe --gtest_color=no            # 29/29
```

## 4. Audit triage (children/spec-audit/AUDIT.md)

| Finding | State after this milestone |
| --- | --- |
| B1 unready rows are 0 and replacement is unconditional; no per-row record in the object | **Fixed on the compiler side by refusal**: `materialize` and `run` refuse any plan with unready rows (`refusedNotReady`), no placeholder is emitted, and the JIT test proves the entry keeps AOT behavior. `UniformContract` with zero confirming rows is refused by the planner. Incremental row dispatch remains ST-B2 |
| B1 T1/T2 re-materialize the snapshot -> holes in the new table | No holes can be materialized now (plans must be complete). The "one shared table per epoch" design is still ST-B3 |
| M1 rows allocated before the bound check / OOB on overflow | Fixed: extents bounded by `MaxRows` before allocation, `Row < rows.size()` guard, `MaxTableBytes` retained |
| M2 partial `materialize`, foreign declaration reused | Fixed: pre-validation pass, declarations/constant/foreign columns refused, atomicity test |
| N1 reversed `urem` accepted | Fixed: only `urem(arg, const)` matches |
| N2 unchecked constant GEP accumulation / modulo aliasing | Fixed and regression-tested: overflow-checked, `ConstOff < elementBytes` required (`PassRefusesWholeElementConstantHop`) |
| N3 `isConsistent` width/size gaps | Fixed: `accessSize<=8` and kind/width/size coherence; malformed-plan test |
| N4 zext vs sign-extend for out-of-domain values | Documented (source GEP is already inbounds-poison; no clamp wanted) |
| N5 uncounted refusals | Partially fixed: `refusedShape` + `refusedNotReady` counters and a per-entry summary; per-load reasons still TODO (ST-A14) |
| N6 inline-exposed new fields | Documented as ST-A15 (covered offsets work; new offsets need re-planning) |
| N7 FAM_ staleness | Fixed for this pass: entry analyses invalidated after a change |
| N8 dynamic-dim skip applied to all functions | Fixed: only the planned entry |
| N9 duplicate plan-set add / entry absent | Plan-set overwrite is last-wins by design; `runSmallTablePass` skips when the entry is absent |
| N10 no provenance tag on the synthesized load | Fixed: `!ejit.smalltable.load` tag with entry + source global, asserted never to carry `!ejit.may_const` (ST-A17) |
| Audit §2 "no test" whole-call items (AS!=0, `inttoptr`, declared-extent mismatch, whole-element constant hop) | Regression-tested in this revision: `PassRefusesVolatileAtomicAndUnsupportedShapes`, `PassRefusesWholeElementConstantHop`, `PlannerRefusesOversizedAndDuplicateDomains` |

Independent-review triage for this repair milestone (`reports/reviewer1.*`,
`reports/reviewer2.*`), each classified by what was actually reproduced:

| Finding | State |
| --- | --- |
| R1 F1 sub-byte APInt assertion | **Confirmed and fixed** (`readScalarBits` masks to `bitWidth`). The premise is validated against this checkout's LangRef/implementation: an integer load's typed value is the low `bitWidth` bits of its `ceil(bitWidth/8)`-byte storage, so masking is the native semantic and `APInt(bitWidth, raw)` was the defect. Canonical i1 (0/1 byte) is unaffected; non-canonical bytes now produce the typed value. Assertions-on abort not reproduced locally (no assertions-on EJIT library available on this host); NDEBUG silent truncation is the reproduced symptom class. Regression: `PlannerMasksSubByteFieldsToTheirWidth` |
| R1 F2 order-dependent mixed-width dedup/coercion wrong code | **Confirmed and fixed** (width-exact site identity + width-exact `fieldMatchesType`). Order dependence is structural: the first load at an `(offset, accessSize)` claimed the column's width, so which load won decided whether the other was zext-widened. Regression: `JitMixedWidthSitesKeepTheirOwnColumns` (real ORC execution over i1/i8/i16/i9) |
| R1 F3 uniform-fold companion | **Confirmed and fixed** (contract validated against the field width; rows masked before the contract comparison). Regression: `UniformContractIsRefusedOutsideTheFieldWidth` |
| R1 poison/undef variants | **Invalid domain for the table path**: a table row is a byte copy of a stable, authorized memory location; `undef`/`poison` are compile-time IR values with no storage for that copy to observe. No unsupported memory domain was added to the folds |
| R2 power-of-two modulo/AND optimization gap | **Deferred** to the independent slot/TTI-C work, as scoped. Recorded above in §6; no modulo matching was broadened and no phase acceptance matrix was added. Existing modulo tests are historical non-regression coverage only |
| R2 shared-safety residue | No shared safety defect was found in the modulo path by this repair: a refused axis keeps its original loads (conservative), which is the required behavior. The only genuine shared defect found and fixed is the width-model family (F1-F3) |
| Whole-entry readiness: unmatched legacy `may_const` behind a partial plan | **Confirmed and fixed on the compiler side**: a lowered plan blocks the legacy fold in the planned entry (ST-A18). The runtime gate it depends on is explicitly specified as ST-B2 and is not claimed implemented |

## 5. Safety invariants implemented

1. Only may_const-authorized fields are replaced; volatile, atomic, AS!=0,
   non-inbounds, foreign-root and unsupported-typed loads keep original form.
2. The row index is built from the real dynamic dim SSA values; no argument is
   replaced by a representative value and no pointer base is frozen.
3. The emitted column is mutable (`isConstant() == false`), `dso_local` on the
   AArch64 product target, plain data, never sealed into the RX code pool; loads
   are never folded to constants.
4. Plans are immutable value snapshots owned by the caller; the pass retains no
   IR pointer across rounds and re-derives the source shape in every round.
5. A plan with unready rows is refused by both lowering entry points; missing
   rows are never emitted as 0/undef/representative cells (spec §5, §6.5).
6. Uniform folding never infers equality from visible rows: `uniformValue` is
   only ever copied from an explicit caller contract, checked against every
   ready row with at least one required confirmation, and recorded on the entry
   for the runtime cold path.
7. A column preserves the load's declared width. Raw memory becomes a typed
   value in exactly one place (`readScalarBits`), which masks to `bitWidth`;
   a uniform contract is validated against `bitWidth` before folding; and a
   load is served only by a column of its own width.
8. Site identity is `(offset, accessSize, bitWidth)`, so two integer loads of
   different widths at one address occupy two columns and are never coerced
   into each other.
9. Whole-entry readiness: a lowered plan blocks the legacy compile-time
   `may_const` fold in the planned entry, so no unrecorded constant dependency
   can be presented as protected by the plan. The fold is blocked, not deleted:
   the load keeps its original dynamic form and is counted.

## 6. Honest gaps / not verified

- Runtime plan construction, incremental row publication, admission validation,
  one shared table per epoch, common T1/T2 PGO, mode rejection and NO_RECLAIM
  retention are milestone B (ST-B1..ST-B7) and are not implemented here, and no
  runtime publication or common T1/PGO is claimed. Online partial-row execution
  is deliberately **blocked** by the compiler refusal. The runtime entry gate
  (ST-A18's other half, listed in ST-B2) does not exist in this milestone: until
  it does, a folded uniform constant is protected only by the fact that the
  caller supplied the contract and the compiler confirmed it against every row
  it could read.
- **Deferred (not PR231)**: the power-of-two modulo/AND optimization gap. The
  dimension matcher accepts `urem(arg, const)` only, and InstCombine
  canonicalizes a power-of-two `urem` into `and`; in the real post-InstCombine
  pipeline such an axis is therefore refused and the loads stay original
  (conservative, no wrong code). Extending the matcher and adding a phase
  acceptance matrix belongs to the independent slot/TTI-C work; no modulo
  matching was broadened here, and the existing modulo tests
  (`JitSlotModuloFiveTwoFullWrapsMatchAot`, `JitDynamicRowsMatchAotAcrossTwoSlotWraps`)
  run only as historical non-regression coverage, not as PR231 feature
  acceptance.
- **Host accommodations, not product behavior** (each labeled at its source):
  (a) the gtest fixture derives its `target datalayout`/`target triple` from the
  host target, because LLJIT rejects a module whose layout differs from the
  JIT's (ELF `m:e` fixture vs COFF `m:w` host);
  (b) `EJitOrcEngine::Create` uses the Large code model on a COFF host, because
  x86-64 COFF Small lowers external globals to `IMAGE_REL_AMD64_ADDR32`, which
  JITLink COFF does not implement;
  (c) the test TU supplies a Windows SRE shim (real `VirtualAlloc`/
  `VirtualProtect` 2 MiB-aligned low arena) beside the POSIX one, because COFF
  `.pdata` `ADDR32NB` resolves against a zero image base.
  The AArch64 product target keeps the Small model and the real SRE platform
  primitives; none of the three is compiled into the product link.
- The AArch64 gate is an object-inspection gate with `llc` + GNU readelf on a
  fixed snapshot; the private SRE/sysroot freestanding **link** and AArch64
  execution are not available on this host and are not claimed. The Windows
  host COFF build is **not** SRE proof and does not replace that gate.
- No performance claim: static load counts are not speedup evidence (product
  intent is DCache/L2D/L3D locality; not measured here).
- The in-tree `EJITTests` host link needs the SRE platform primitives that
  LLVMEJIT deliberately does not define; this milestone's test TU supplies host
  stubs and the batched-seal flush, matching the product compile driver's
  publish sequence. This is a pre-existing host-configuration gap, recorded so a
  reviewer does not mistake it for new product behavior.
- The 24/24 server result and the previous AArch64 BE artifacts are historical:
  they were produced from an earlier revision and are **not** evidence for this
  repaired HEAD. The local 29/29 above is the current evidence.
- Windows-native limits of this evidence: assertions-on execution of the EJIT
  library is not available locally (the read-only candidate archives are all
  Release/assertions-OFF and lack ORC in the assertions-on tree), so the
  sub-byte `APInt` abort is characterized by source reading plus the NDEBUG
  symptom and covered by the masking fix and its tests, not by a reproduced
  assertions-on abort. The anti-optimization (`NDEBUG`) configuration is the
  product configuration.
