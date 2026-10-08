# EJIT Small Table — Implementation Map, Status and Evidence

Base commit: `027a151b7d8b5b44446dc055c963b1a782754fbb` (documentation-only PR231 on
spec5 `3f0e190dd54752f9c1dcb4bcd1fcb5d6349ae59c`), branch
`codex/ejit-small-table-deepseek`. Milestone A (compiler side) of
`EJIT_SMALL_TABLE_SPEC.md` / `../EJIT_SMALL_TABLE_SPEC.latest.md`.

Current gate: **52/52 focused gtest tests pass on the local Windows host**,
including real ORC-engine compiles whose IR transform runs the small-table pass
and executes the emitted table loads against the AOT reference. Two
configurations are built from this checkout against the read-only local LLVM
archives: NDEBUG (product flags) and `-DEJIT_DIAG_ENABLE` (an
assertions-independent diagnostics build, **not** an assertions-on substitute).
Logs (runner dir, outside git; final full 27-TU rebuild + test TU from the
current tree): `build/pr231-local/test-full-conflict-1.out` (NDEBUG, 52/52, exit 0,
empty stderr) and `build/pr231-local-diag/test-full-provenance-1.out` (diag, 52/52,
exit 0); compile logs `build/pr231-local/compile-provenance-1.log` and
`build/pr231-local-diag/compile-provenance-1.log`. Earlier logs
(`test-run-a1-final.log` 42/42, `test-run-a1-3.log` 39/39,
`test-run-final-20260914.log` 29/29, `test-run-baseline-20260914.log` 17/24,
`test-run-intermediate-20260914.log` 23/24) are preserved as the previous
checkpoints' evidence and pre-fix baseline.

Milestone A1 (compiler core: automatic per-field uniform detection, per-field
retained axes, complete joint-projection validation, exported admission contract)
is the change on top of the A0 repair checkpoint `33d16632`. A0 itself is
inspected and regressed here as a prerequisite, not declared independently
accepted.

Milestone B (online runtime: B0 readiness/borrow/provider, B1 shared table
resource with generation identity and incremental publication, B2 ONE common
sampling session per entry/code generation with the confirmed aggregate-64
configurable budget, common T1 and common T2) is implemented in
`EJitSmallTableRuntime.{h,cpp}` and driven end-to-end by the nine
`SmallTableRuntimeTest` cases through the real `EJitOrcEngine`. What is **not**
done and is not claimed: the product configuration-transaction binding (the only
readiness source here is the explicitly labeled host adapter) and the production
`Runtime`/`CompileDriver`/worker consumption that would publish a logical slot
from this runtime (see section 6).

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
9b5523656921e5ca954d874cc0d97b749debd05c1f2aa584e2396343f18d243b  EJitSmallTable.h
db6e24ffb409b20bfabef807ee4c07c2cc0d5439c66fcaac71b573420882e64d  EJitSmallTableRuntime.h
7ae2f89cd0e16a40efdfd30913f3a317938a987a1e6a201363d931ff26aa122a  EJitOrcEngine.h
62e3e0b199a8601c8f6fa623c42c49257775979765ca063586087758dc912ce2  EJitSmallTable.cpp
221c8fd2e015a4b8c969ffe83fc5859414fe83a36203918aec373268f040e951  EJitSmallTableRuntime.cpp
833b633ff79e541400e75b578780130862a0112b4b6627705f7c39ce0a7a6669  EJitOrcEngine.cpp
0ac816bc0568ae64f58d08833b8a16a4e73dda07b8022a6ac322d0c5cf569eb9  EJitOptimizer.cpp
289ba992ec60e4baf7d36107db7cf29984f4f4c6f8f9d1f9e5ecee0f17b4b6c7  EJitSmallTableTest.cpp
```

Focused test binaries (sha256): NDEBUG
`720a2be3124a5809255e19d93ef2f6092785ab07bf8c42b763278de75f2588d1`,
diag `e2e349450cf098692337af4a9d07f8f458a6153db933eaa8ec0b34e2540e0e03`.

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
| Runtime (B) seam | `EJitSmallTableRuntime` (`EJitSmallTableRuntime.{h,cpp}`): `prepare` -> `compileCommonT1` -> `admitMember`/`enterAdmitted`/`leaveAdmitted` -> `freeze` -> `compileCommonT2`, plus `beginNextGeneration`/`retireGenerationsUpTo`/`cancel` | Owns one `EJitOrcEngine`, the plan set, the contract, the shared `EJitSmallTableTableResource` and the sampling session; readiness/borrow come from an `EJitSmallTableReadinessProvider` (host adapter in tests, product binding absent). It reaches the compiler through the same `setSmallTablePlans`/`loadBitcodeModule`/`lookup` seam the product driver uses; runtime-owned columns are declared (not defined) in the module and bound with absolute symbols |

Replace rounds are exactly three (`runStructFieldPass` at 1c, 1f, and phase 4);
the small-table pass runs immediately before the existing field pass in each, so
no round can regress a table read back to a source load. Plans are value
snapshots and the source shape is re-derived from the current IR in every round,
so no `Value*` survives inline/cleanup.

## 2. Requirement table

| ID | Spec | Implementation file / symbol | State | Test + command | Provenance | Evidence | Risks / next gate |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ST-A1 | §9, §15 | `EJitSmallTablePlan{,Set}`; `EJitOptimizer::setSmallTablePlans`, `EJitOrcEngine::setSmallTablePlans` (default empty) | implemented | `PipelineFeatureOffKeepsBaseline`, `JitFeatureOffKeepsBaselineCorrect` | base `027a151b`; flags from ws6 `compile_commands.json` | `build-obj/test-run-2026-09-11T06-51-28Z.log` | OFF path must stay byte-identical; both OFF tests pass |
| ST-A2 | §1.2, §4.1, §8 | `EJitSmallTablePlanner::planShape` / `plan` per-entry field+schema plan | implemented | `PlannerBuildsColumnsAndRowsFromMemory` (per-field retained axes and payload accounting), `AutomaticSolverFoldsUniformAndEliminatesAxes` | A1 HEAD (hashes above) | `build/pr231-local/test-run-a1-3.log` | Fields discovered pre-inline only (ST-A15); declared extents still bound the schema |
| ST-A3 | §5.2, §5.3 | borrow region extent, per-ready-row bounds, `MaxRows`/`MaxTableBytes`, overflow-checked row count | implemented | `PlannerRefusesOutOfRegionAndUnknownShape`, `PlannerRefusesOversizedAndDuplicateDomains` (incl. declared-extent mismatch) | same | same | No borrow/lifetime protocol (ST-B2) |
| ST-A4 | §4, §6.5, §9 | `EJitSmallTablePass::materialize`/`run`: indexed load from `@__ejit_stab_*`, mutable `dso_local` data | implemented | `PassReplacesWithDynamicIndexAndKeepsArguments`, `PipelineKeepsTableLoadsThroughAllReplaceRounds` | same | `build-obj/test-run-2026-09-11T06-58-21Z.log`; `children/aarch64be2/out/README.md` | Non-`inbounds`, unknown shapes and whole-element constant hops stay original |
| ST-A5 | §9 | `runSmallTablePass` before `EJitStructFieldPass` in rounds 1c/1f/phase 4; idempotent `materialize` | implemented | `PipelineKeepsTableLoadsThroughAllReplaceRounds` (3 loads after all rounds) | same | same | `materialize` validates all columns before creating any |
| ST-A6 | §4, §5.6 | dims matched against the real `Argument` / `urem(arg,mod)`; `preReplacePeriodIndices` skips only the planned entry's dim args | implemented | `PlanKeepsDeclaredDimensionDynamic`, `PassReplacesWithDynamicIndexAndKeepsArguments` | same | same | Skip keyed on arg index within the entry only |
| ST-A7 | §5.6 | ordinary loads, store addresses, helper args unchanged | implemented | `JitLiveLoadStoreAndHelperStayDynamic`, `UniformContractFoldsOnFullyReadyPlan` (live store) | same | same | Only the former load's users see the replacement |
| ST-A8 | §6.5 | pure-table: late different row values observed without recompilation | implemented (single module) | `JitLateDifferentRowValuesAreObserved`, `UniformContractFoldsOnFullyReadyPlan` (varying column) | same | same | Cross-tier table identity is ST-B3 |
| ST-A9 | §4, §14.3 | real `slotNo` 0..1023, `%10` and `%5`, two full wraps, `cell`/`trp` dynamic | implemented | `JitDynamicRowsMatchAotAcrossTwoSlotWraps`, `JitSlotModuloFiveTwoFullWrapsMatchAot`, `UniformContractFoldsOnFullyReadyPlan` | same | same | No clamp for out-of-domain values (documented) |
| ST-A10 | §6.6 | explicit uniform admission contract as a **distinctly selected comparison mode** (`EJitSmallTablePlanMode::ExplicitContracts`): caller-provided, checked against every ready row, **at least one ready row must confirm it**, never inferred; every field without a contract keeps all declared axes | implemented (compiler half, comparison mode) | `UniformContractIsExplicitAndFolds`, `UniformContractIsCheckedAgainstEveryReadyRow`, `UniformContractFoldsOnFullyReadyPlan`, `AutomaticModeRefusesContractsAndUnprovenDomains` (mode separation + mode recorded on the plan) | A1 HEAD | `build/pr231-local/test-run-a1-3.log` | Runtime cold-path validation of a later member is ST-B2; this mode no longer defines the default (see ST-A20) |
| ST-A20 | §4.1, §6.6 | **automatic per-field uniform detection** (`EJitSmallTablePlanMode::Automatic`, the required default): for every discovered field the solver groups the complete proven dependency domain by projection and folds a field to a bit-exact constant when it is equal on the whole domain. A folded field gets **no column, no payload and no table load**, and the branch its constant decides folds in the real pipeline. An empty proven domain is refused (no vacuous constant); a caller contract in this mode is refused | implemented (compiler half) | `AutomaticSolverFoldsUniformAndEliminatesAxes`, `AutomaticSolverLowersEachFieldWithItsOwnAxes` (folded load gone, `select`/`icmp` gone after the real pipeline), `JitAutomaticSpecializationMatchesAotEveryRow` (real ORC, every row), `AutomaticWidthsKeepTheirTypedValues` (i1 0xFE → typed 0, i8 → 0xFE, no column for either), `AutomaticEqualityIsProvenOnTheDomainNotOnVisibleRows` | A1 HEAD | `build/pr231-local/test-run-a1-3.log`, `build/pr231-local-diag/test-run-a1-diag2.log` | The constant is an admission obligation, not a claim about unvalidated members; a later conflicting member must stay AOT (ST-A22). Production readiness/row publication is B0/B1 |
| ST-A21 | §4.1 steps 1-4 | **per-field retained axes and complete joint-projection validation**: the deterministic bounded solver enumerates the declared axis subsets, checks every candidate against ALL original proven rows (bit-exact equality per projected coordinate), selects fewest axes → fewest allocated bytes → schema axis order, then re-verifies the winner with a fresh full-domain scan (`verifyProjections`, also run in `isConsistent`). Each field is lowered with only its own axes; no density/neighbour threshold, no independently inferred axis deletion, no candidate survives unverified. Budgets (axis count, scratch bytes, work, rows, table bytes) refuse explicitly | implemented (compiler half) | `AutomaticSolverFoldsUniformAndEliminatesAxes` (uniform/by_cell/by_TRP/joint matrix), `AutomaticSolverLowersEachFieldWithItsOwnAxes` (each column addressed by its own axis), `AutomaticSolverUsesTheWholeDomainDeterministically` (diagonal sparse tie-break, checkerboard keeps the joint axes, TRP-only case), `AutomaticPlanReportsPerFieldAxes` | A1 HEAD | `build/pr231-local/test-run-a1-3.log`; per-field report in `build/pr231-local-diag/test-run-a1-diag2.log` (`payload=76 payload_before=192`) | Scope is declared/enumerable axes on the cell/TRP schema, not a general optimal index compression; out-of-scope cases refuse and keep the original load |
| ST-A22 | §6.6, §6.6.1 step 3 | **exported admission contract**: `buildAdmissionContract` derives a self-contained `EJitSmallTableContract` (source/dims/strides, declared vs proven rows, domain completeness, epoch, provider label, stable-borrow flag, per-field strategy, required uniform value, retained axes, table resource identity, published projected values, stable `identityHash`). `readAdmissionMember` reads a candidate member from a borrowed region using the contract's own offsets/widths; `validateAdmission` classifies it as compatible / extendable / conflicting / unusable, so a conflicting member can never reuse existing specialized code and an already published projected value can never be overwritten by a different one | implemented (compiler contract boundary; test provider clearly labeled) | `AdmissionContractValidatesLaterMembers` (compatible, uniform conflict, projected-value conflict, out-of-schema unusable, unpublished-coordinate extendable, region-too-small refusal), `WideningForAConflictingMemberOnlyExpandsThatField` (only the differing field recovers an axis; unrelated constants stay), `ContractIdentityIsPerCompileNotPerSymbolName`, `AutomaticWidthsKeepTheirTypedValues` (width-exact required value) | A1 HEAD | `build/pr231-local/test-run-a1-3.log` | The **runtime** side (B0 real readiness/borrow source, B1 publication into this compile's table, migration/rebuild budget) is implemented in `EJitSmallTableRuntime` and covered by the nine `SmallTableRuntimeTest` cases, but the readiness source is still the labeled host adapter and nothing in the product consumes it. No product readiness is claimed |
| ST-A11 | §5 | i1/i8/i16/i32/i64, float/double bit patterns incl. -0.0 and NaN payload; BE decoding; bitcast-punned views | implemented (host) / verified artifact (BE) | `PassCoversScalarWidthsAndFloatBits`, `PassMatchesBitcastPunnedScalarView`; BE bytes in child gate | same | `build/pr231-local/test-run-20260914.log`; `children/aarch64be2/out/verify_data.txt` | No host unit test with a BE `DataLayout` (child gate covers it) |
| ST-A11b | §5 "整数保留位宽" | **R1 F1**: a sub-byte integer load's typed value is the low `bitWidth` bits of the `accessSize` bytes it reads. `readScalarBits` masks to `bitWidth`, so a non-canonical byte (0xFE over an i1 field) never reaches `APInt(bitWidth, …)`: under assertions that construction aborted and under NDEBUG it truncated silently | fixed | `PlannerMasksSubByteFieldsToTheirWidth` (row bits masked; i1/i8/i16/i9 columns keep separate, correct values), `UniformContractIsRefusedOutsideTheFieldWidth` | same | `build/pr231-local/test-run-20260914.log` | canonical i1 (0/1 byte) is unaffected; the mask is a refinement in both domains. Poison/undef cannot appear: a table row is a byte copy of a stable, authorized memory location (spec §5.2), and IR-level `undef`/`poison` is a compile-time value with no storage for the copy to observe |
| ST-A11c | §5 | **R1 F2**: site identity is `(offset, accessSize, bitWidth)`. Deduplicating by `(offset, accessSize)` alone let a wider load share a narrower column and be coerced with `zext` (order-dependent silent wrong code); `fieldMatchesType` is now width-exact, so the only coercion left is identity or a same-width `bitcast` (int ↔ float/double) | fixed | `JitMixedWidthSitesKeepTheirOwnColumns` (real ORC execution of i1+i8+i16 at one offset, i9 at another, against the source values), `PlannerMasksSubByteFieldsToTheirWidth` | same | `build/pr231-local/test-run-20260914.log` | An integer column still serves a same-width integer view and a same-width float/double view; no cross-width fold is attempted |
| ST-A11d | §6.6 | **R1 F3**: a uniform admission contract must be a value of exactly the field's width. A raw storage byte (0xFE) is refused before it can be truncated; an in-range value is still only admitted when every ready row's masked bits confirm it, and the folded constant is the typed i1 value | fixed | `UniformContractIsRefusedOutsideTheFieldWidth` (0xFE refused, 0 admitted and folded to i1 `false`, raw 0xFE never appears as the folded value) | same | `build/pr231-local/test-run-20260914.log` | Contract vector size is validated against the field count before any indexing (pre-existing check, exercised by `PlannerRefusesOversizedAndDuplicateDomains`) |
| ST-A12 | §5.5 | volatile/atomic/AS!=0/pointer/aggregate/vector/>64-bit, non-inbounds, `inttoptr`-rooted, non-`urem(arg,mod)`, foreign global, whole-element constant hop, unsupported element types refused | implemented | `PassRefusesVolatileAtomicAndUnsupportedShapes` (AS1 + `inttoptr` included), `PassRefusesWholeElementConstantHop`, `PlannerRefusesOversizedAndDuplicateDomains`, `MaterializeFillsDeclaredSlotsAndRefusesForeignOnes` | same | same | Constant-index overflow and out-of-element hops refused, not aliased |
| ST-A13 | §14.8 | AArch64 **BE** freestanding object: table in `.data`, entry in `.text`, `ADR_PREL_PG_HI21`+`ADD_ABS_LO12_NC` for every table symbol, no GOT for them, no libc/host deps, BE bytes verified | implemented (artifact gate, re-run on post-change snapshot) | `children/aarch64be2/out/checks_report.txt` (checks a-e PASS, exit 0); llc ws6 21.1.8 + GNU readelf 2.46 | pass object compiled by the child from snapshot `1ba57dc5…`/`db8027d0…` (`EJitSmallTable.snapshot.o` `f29c2479…`) | `children/aarch64be2/out/pass_emitted.o` (`5fda0c37…`, byte-identical codegen to the pre-change artifact; only the new load metadata differs), `readelf_*.txt`, `verify_data.txt`, `README.md` | Object is **not** GOT-free overall: `g_cfg`/`g_out` keep `ADR_GOT_PAGE`+`LD64_GOT_LO12_NC`; only the `__ejit_stab_*` symbols are direct. No board execution, no SRE sysroot link |
| ST-A14 | §13 | strategy counters: may_const/table/uniform/kept/refused-shape + `refusedNotReady`; per-entry ready/rows summary; **per-field report** (`strategy`, retained axes, eliminated axes, payload before/after) and plan-level payload/payload-before counters emitted by `EJitOptimizer::runSmallTablePass` | implemented | counters asserted in the pass tests; `AutomaticPlanReportsPerFieldAxes` raises the log level for one run so the report is captured | A1 HEAD | per-field lines in `build/pr231-local-diag/test-run-a1-diag2.log` | Per-load refusal reasons are counted (`keptOriginal`, `refusedShape`, `refusedNotReady`) but not yet attributed per site in the log; no dynamic hit/ranking split |
| ST-A15 | §9 (inline) | inline-exposed fields: covered columns are replaced post-inline, uncovered stay original; schema frozen at plan time | partial | `NestedCalleeLoadsStayOutsideThePlan` | same | same | New columns discovered only after inlining need a re-plan (milestone A gap) |
| ST-A16 | §6.5 readiness | partial plans are **refused for executable lowering**: no 0/undef/placeholder row is ever emitted, and no contract is folded from an incomplete domain | implemented (compiler half) | `PartiallyReadyPlanIsRefusedUntilRuntimeAdmission`, `JitPartiallyReadyPlanKeepsAotBehavior`, `UniformContractIsExplicitAndFolds` | same | `build/pr231-local/test-run-20260914.log` | **Online partial-row execution now exists in the runtime** (ST-B2: declaration-only lowering, per-member admission, immutable publication) but is bound to the labeled host adapter only; the compiler-emitted path still refuses a partial plan. Until the product provider and the worker slot publication exist (section 6), an entry must not be advertised as online-specializable |
| ST-A18 | §4.1, §5, §6.6 | **whole-entry readiness**: when a plan is lowered for an entry, that entry's legacy compile-time `may_const` fold is blocked for that function (`EJitStructFieldPass::blockLegacyConstantFolds`, applied by `runSmallTablePass`'s lowered result in all three replace rounds). Code shared across the plan's declared domain then contains only plan-backed reads (`!ejit.smalltable.column` / `!ejit.smalltable.contract`) and real dynamic source reads; an unmatched legacy fold can no longer hide behind a partial plan. Entries without a lowered plan keep the baseline fold | implemented (compiler half) | `LoweredPlanBlocksTheLegacyFoldItDoesNotDescribe` (registered period array with a declared may_const field: folded without a plan, kept as a real load with one), `PipelineFeatureOffKeepsBaseline` | same | `build/pr231-local/test-run-20260914.log` | Fold blocking is per-function (the planned entry); callees keep the baseline fold and are only reachable through the entry. Blocked loads are counted (`getLegacyFoldsBlocked`) and keep their original dynamic form; they are **not** claimed by the plan |
| ST-A19 | §6.5, §11 | **per-compile table identity and handoff**: each compile has its own JITDylib (`spec_base_<cacheKey>`) and materializes its own table objects, so the same column spelling is a different object/address in another compile. The compiler exposes the exact names it lowered through `EJitOrcEngine::getLastSmallTableColumnNames()` (also used for the ORC symbol claim); a name resolves inside its own compile and not through a foreign cache key | implemented (compiler half) | `TableIdentityIsPerCompileAndHandedOffByName` (names handed off, resolvable in their own compile, not process-wide, address stable across repeated lookups within the compile) | same | `build/pr231-local/test-run-20260914.log` | **The compiler cannot and does not claim a stable table address across compiles**: the runtime now publishes a member's rows into the table instance of the compile it is admitting to (ST-B3, `EJitSmallTableRuntime` allocates one resource per generation, binds every column to it and re-verifies the resolved address after the T1 and T2 compiles). The compiler-emitted form keeps per-compile materialization, which is what this test pins |
| ST-A17 | §9 provenance | synthesized table load carries `!ejit.smalltable.load` (entry + source) and never `!ejit.may_const`, so a later round cannot re-extract it | implemented | `PassReplacesWithDynamicIndexAndKeepsArguments` | same | same | Tag is informational; the pass already refuses foreign roots |
| ST-B1 | §6.2, §6.5, §6.1.1 | runtime plan construction from a readiness fact source: `EJitSmallTableReadinessProvider` (label, `domainEpoch`, `epochCurrent`, `coversDeclaredDomain`, `readyMembers`, protected `borrow`), `EJitSmallTableReadBorrow` (refuses every read once released/invalidated and is released on every path) and the explicitly labeled host adapter `EJitSmallTableHostProvider` (`host-adapter.pr231-not-product`). `EJitSmallTableRuntime::create` without a provider fails closed; `prepare` refuses no provider / refused borrow / epoch-less source / empty or uninitialized member set | implemented (fail-closed interface + labeled host adapter; **product binding absent**) | `RuntimeIsFailClosedWithoutProviderOrBorrow` (no provider, refused borrow, moved generation, epoch 0, member-less source, uninitialized fields), the other six runtime tests drive the host adapter | current HEAD | `build/pr231-local/test-full-conflict-1.out`, `build/pr231-local-diag/test-full-conflict-1.out` | The product configuration-transaction completion point is **not** locally available; `ejit_activate` is explicitly not treated as readiness. The provider interface is the binding point: replacing the host adapter is the remaining product work |
| ST-B2 | §6.5, §6.6 | runtime entry gate: `prepare` plans over the provider's confirmed-ready members under a protected borrow (re-checked after the last read and before publication), `EJitSmallTableTableResource` is one shared fixed-capacity data resource with a real generation identity, `admitMember` validates a member against the exported contract (compatible/extendable/conflict/unusable) before it can dispatch, `enterAdmitted`/`leaveAdmitted` are the only way a call is counted, and a not-admitted member is refused with the AOT path as the fallback. Publication is immutable: a repeated coordinate with a different value is a `Conflict`, never an overwrite | implemented (runtime) | `RuntimePublishesAdmittedRowsAndCommonT1RunsRealCode` (rows published into the runtime resource, columns resolve to `resource()->columnAddress(F)`, real T1 execution over every row, unconfirmed member refused and uncounted), `RuntimeLateCompatibleMemberKeepsTheSameSession` (extendable late member publishes exactly its own new coordinate, then is compatible), `RuntimeConflictPreparesOneGenerationAndMigratesMembers` (conflict stays AOT and publishes nothing), `RuntimeChangedMemberStaysAotOnReValidation` (a member whose own values changed is a conflict on re-validation while untouched members keep dispatching) | current HEAD | `build/pr231-local/test-full-conflict-1.out` (focused run `build/pr231-local/test-rt-c.out`) | `tableReady`/`codeReady` logical-slot publication through the production worker is **not** wired (section 6); the runtime gate itself is real and tested |
| ST-B3 | §6.5, §8, §11 | one stable table address per entry/epoch shared by T1 and T2: the runtime allocates the resource outside the code pool, binds every column to it with absolute symbols before the load, verifies after the compile that each column resolves to `resource()->columnAddress(F)` (a same-named foreign table is refused), and `compileCommonT2` re-verifies the same resource identity | implemented (runtime) | `RuntimePublishesAdmittedRowsAndCommonT1RunsRealCode`, `RuntimeCommonBudgetFreezesBundleAndRunsCommonT2` (T2 runs against the same resource), `RuntimeServesTheProductShapeDomainUnderTheDefaultAggregateBudget` | current HEAD | `build/pr231-local/test-full-conflict-1.out` (focused run `build/pr231-local/test-rt-c.out`) | Symbol-name equality is never used as address identity; the check is the resolved address |
| ST-B4 | §10 | common T1/T2 PGO: ONE session per entry/code generation, aggregate **64** default budget (configurable, `EJitSmallTableSamplingPolicy`), actual admitted executions counted across admitted ready members while the session holds the protected read borrow (taken by the first counted sample, released by freeze/cancel), freeze waits for in-flight admitted executions, ONE immutable `EJitSmallTableProfileBundle` (entry/epoch/session/contract hash/resource address+generation/sample count/participating members/real counters/synthesized profile) and common T2 from that bundle. Stale callbacks after freeze/cancel are rejected and counted; a late member does not restart the quota; a new generation gets its own | implemented (runtime) | `RuntimeCommonBudgetFreezesBundleAndRunsCommonT2` (aggregate 5 budget spread over 3 members, uncounted above-budget dispatch, freeze refuses while in flight, bundle identity, real T2 execution, frozen session), `RuntimeServesTheProductShapeDomainUnderTheDefaultAggregateBudget` (default 64, 64 distinct members, 16x32 schema), `RuntimeCancelRejectsStaleCallbacksAndStopsDispatch`, `RuntimeSamplingRefusesWithoutAProtectedBorrow` | current HEAD | `build/pr231-local/test-full-conflict-1.out` (focused run `build/pr231-local/test-rt-c.out`) | No per-cell 64 and no representative-only quota; the profile is synthesized from real captured Tier-1 counter addresses, not modelled |
| ST-B5 | §7, §8 | update path: `beginNextGeneration` re-plans the union of every member the runtime tracks plus new members on a freshly parsed copy of the pre-declaration source, widens only the affected fields, migrates every still-valid member into a new resource (generation+1), retains the old resource with its bytes accounted (`retentionCapacityLimit`) and refuses to move while a member execution is in flight or a generation is uncompiled; `retireGenerationsUpTo` refuses to free above the live generation | implemented (runtime) | `RuntimeConflictPreparesOneGenerationAndMigratesMembers` (coalesced generation, per-field widening, migration, contract identity change, session closed, retention accounting, retirement below the live generation refused, execution of migrated members) | current HEAD | `build/pr231-local/test-full-conflict-1.out` (focused run `build/pr231-local/test-rt-c.out`) | Retention is bounded and refused when exceeded; no page is freed or reopened |
| ST-B6 | §10, §13 | mode/side-effect rejection: `Options` carry the aggregate budget, resource capacity limit and retention limit; a plan above capacity is refused before allocation; a borrow that cannot be taken refuses specialization; defaults stay OFF (no provider installed means nothing is specialized) | implemented (runtime) | `RuntimeIsFailClosedWithoutProviderOrBorrow`, capacity refusal paths in `EJitSmallTableTableResource::create`, `RuntimeConflictPreparesOneGenerationAndMigratesMembers` (retention limit) | current HEAD | `build/pr231-local/test-full-conflict-1.out` (focused run `build/pr231-local/test-rt-c.out`) | Product switches remain OFF; nothing here enables the feature by default |
| ST-B7 | §13 | diagnostics/accounting: per-field report (`strategy`, retained/eliminated axes, payload before/after), plan totals, runtime `Stats` (planned/ready rows, admissions by class, published rows, AOT refusals, accepted/rejected samples, stale callbacks, generation changes, migrated rows, retired generations) and per-resource `Accounting` (reserved/allocated/payload/published bytes and cells) | implemented (local) | accounting asserted in `RuntimeServesTheProductShapeDomainUnderTheDefaultAggregateBudget`; per-field report captured in the diag log; `AutomaticPlanReportsPerFieldAxes` | current HEAD | `build/pr231-local-diag/test-full-conflict-1.out`, `build/pr231-local/test-full-conflict-1.out` | Transport of these counters into the product log is not wired (host evidence only) |

## 3. Test inventory (all in `llvm/unittests/ExecutionEngine/EJIT/EJitSmallTableTest.cpp`)

39 tests in the previous A1 checkpoint, 42 after A1, **52 tests now**, grouped:
planner (5), pass shape/refusals (6), optimizer pipeline (4), real ORC execution
(7), readiness/contract boundaries (4), scalar-width regressions (2), whole-entry
readiness (1), inline coverage (1), per-compile table identity (1), A1 automatic
solver / axis elimination / admission contract (11), **B runtime (9)**. The
widths the suite exercises at runtime with real ORC materialization are
i1/i8/i16/i32/i64, `i9` (sub-byte, two-byte storage), float and double.

B runtime tests and what each one pins:

| Test | Pins |
| --- | --- |
| `RuntimeIsFailClosedWithoutProviderOrBorrow` | B0 fail-closed: no provider, a provider that refuses the borrow, a moved generation, epoch 0, no confirmed member, a member whose fields are not initialized — none of them specializes anything |
| `RuntimePublishesAdmittedRowsAndCommonT1RunsRealCode` | B1+B2: plan from provider facts, rows published into the runtime resource, each column resolving to `resource()->columnAddress(F)`, one common T1 executed over every row against AOT, a real admitted sample counted/in-flight/completed under the session read borrow, an unconfirmed member refused and uncounted |
| `RuntimeCommonBudgetFreezesBundleAndRunsCommonT2` | B2: aggregate budget shared across members, above-budget dispatch correct but uncounted, freeze refuses while an admitted execution is in flight, one immutable bundle (identity, contract hash, resource address/generation, 5 samples, 3 members, real counters, synthesized profile), freeze ends the sampling window and releases its borrow, common T2 executes against the same resource, frozen session accepts nothing |
| `RuntimeLateCompatibleMemberKeepsTheSameSession` | B1 §6.6 steps 2-3: a late extendable member publishes exactly its own new projected coordinate into the same generation/resource/contract, keeps the same session and quota, then re-admits as compatible with no republication; out-of-schema coordinates are unusable and publish nothing |
| `RuntimeConflictPreparesOneGenerationAndMigratesMembers` | B1 §6.6.1: a conflicting member stays AOT and publishes nothing; a generation change is refused while an execution is in flight; ONE coalesced generation widens only the affected field, migrates the known members, changes contract identity, keeps the old resource until retirement |
| `RuntimeCancelRejectsStaleCallbacksAndStopsDispatch` | B2 cancel/timeout: session closed, reason recorded, in-flight cleared, the session read borrow released, later tickets are stale callbacks, no dispatch and no freeze afterwards |
| `RuntimeSamplingRefusesWithoutAProtectedBorrow` | B2 fail-closed sampling: the sampling window needs the configuration-side protected read borrow; without one the call is refused with that reason, the ticket stays invalid, no budget is consumed, nothing is in flight and no borrow is held; a **delayed** borrow (transient refusal, then granted) does not poison the session, which samples normally afterwards |
| `RuntimeChangedMemberStaysAotOnReValidation` | B1/B2 §6.6.3-4: a member whose own values changed after publication is a `Conflict` on re-validation (nothing republished, the AOT path is taken, no budget consumed) while an untouched member stays compatible and keeps dispatching - samples are never treated as proof that a member stays constant |
| `RuntimeServesTheProductShapeDomainUnderTheDefaultAggregateBudget` | product shape: 16x32 declared schema (per-field rows 16/32/512), 6x20 ready members, per-field payload/capacity/retention accounting, the **default aggregate 64** budget spread over 64 distinct admitted members, 65th dispatch uncounted, one frozen bundle (64 samples, 64 members, real counters) and common T2 execution for all 120 ready members |

A1 tests and what each one pins:

| Test | Pins |
| --- | --- |
| `AutomaticSolverFoldsUniformAndEliminatesAxes` | the §4.1 matrix: uniform (no column/payload), by_cell, by_TRP, joint; per-field payload accounting; recorded readiness identity |
| `AutomaticSolverLowersEachFieldWithItsOwnAxes` | real IR: each column addressed by its own axis, uniform fold, deciding branch folded after the real pipeline, no source load left |
| `JitAutomaticSpecializationMatchesAotEveryRow` | real ORC execution of the automatic plan against the AOT reference for every (cell, TRP) and a range of `x`; exactly 3 published columns |
| `AutomaticSolverUsesTheWholeDomainDeterministically` | diagonal sparse domain, schema-order tie-break, checkerboard keeps the joint axes, TRP-only case, executed checkerboard |
| `AutomaticModeRefusesContractsAndUnprovenDomains` | automatic mode refuses caller contracts and an empty domain; the comparison mode is distinctly selected and recorded; a partial domain is planned but never lowered (all 4 loads kept) |
| `AutomaticWidthsKeepTheirTypedValues` | automatic mode over the width fixture: i1/i8 fold with their typed values and no column, i16/i9 keep one-axis tables; width-exact contract obligation; real ORC execution |
| `AdmissionContractValidatesLaterMembers` | contract export identity/resource shape, cold-path member read, compatible/conflict/uniform-conflict/projected-conflict/unusable/extendable classification, borrow-bounds refusal |
| `WideningForAConflictingMemberOnlyExpandsThatField` | §6.6.1 step 3 at the A1 boundary: only the differing field recovers an axis, unrelated constants stay folded, old contract unchanged, new identity |
| `ContractIdentityIsPerCompileNotPerSymbolName` | same symbol spelling + different domain = different identity; deterministic rebuild; an anonymous contract validates nothing |
| `AutomaticSolverFoldsAClosedSingleMemberDomain` | one-cell/one-TRP closed domain: all four fields fold, no column, no load, no published table, executed through ORC |
| `AutomaticSolverFoldsExactFloatBits` | -0.0 folds to `0x80000000` and a NaN payload to `0x7fc00001` exactly (never canonicalized), the varying integer field keeps its column, real execution matches |
| `AutomaticSolverRefusesBeyondItsAxisBudget` | nine declared axes refuse explicitly (`bounded solver`) instead of degrading; no column created, the original load kept |
| `AutomaticPlanReportsPerFieldAxes` | §13 per-field reporting path and its accounting |

Host build + run commands (runner dir; one compiler and one linker at a time):

```
pwsh -File build-local.ps1 -All -Link                                      # 27 EJIT TUs + gtest
pwsh -File build-local.ps1 -Link -Tu EJitSmallTableTest.cpp                # relink
build/pr231-local/EJitSmallTableTests.exe --gtest_color=no                 # 52/52
pwsh -File build-local.ps1 -All -Diag -Link                                # diagnostics build
build/pr231-local-diag/EJitSmallTableTests.exe --gtest_color=no            # 52/52
```

## 4. Audit triage (children/spec-audit/AUDIT.md)

| Finding | State after this milestone |
| --- | --- |
| B1 unready rows are 0 and replacement is unconditional; no per-row record in the object | **Fixed on the compiler side by refusal**: `materialize` and `run` refuse any plan with unready rows (`refusedNotReady`), no placeholder is emitted, and the JIT test proves the entry keeps AOT behavior. `UniformContract` with zero confirming rows is refused by the planner. The runtime counterpart (ST-B2) now exists: the runtime-owned plan declares columns instead of filling them, and `admitMember`/`enterAdmitted` gate every dispatch on a validated, published member |
| B1 T1/T2 re-materialize the snapshot -> holes in the new table | **Resolved for the runtime path**: the runtime owns ONE resource per generation with a stable address, binds it into both compiles and verifies the resolved address after each compile; T1 and T2 read the same object. The compiler-emitted (test-only) form still materializes per compile, which is what the per-compile identity test pins |
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
3. An emitted **column** is mutable (`isConstant() == false`), `dso_local` on the
   AArch64 product target, plain data, never sealed into the RX code pool, and a
   load from it is never folded to a constant. Only a field the solver (or a
   confirmed caller contract) proved constant over the proven domain is folded,
   and such a field has no column at all.
4. Plans are immutable value snapshots owned by the caller; the pass retains no
   IR pointer across rounds and re-derives the source shape in every round.
5. A plan with unready rows is refused by both lowering entry points; missing
   rows are never emitted as 0/undef/representative cells (spec §5, §6.5).
6a. The **default** plan mode is automatic (§4.1): each field's strategy is
   solved on the complete proven dependency domain, and only an existing,
   enumerable domain may produce a constant (an empty domain refuses). A folded
   field has no column, no payload and no table load.
6b. The solver validates **every** candidate retained-axis set against all
   original legal rows of that domain and re-verifies the chosen set with a fresh
   full-domain scan before the plan is returned (`verifyProjections`, also run by
   `isConsistent`), so no axis set can be inferred independently and no
   unverified candidate reaches code generation. Selection is deterministic:
   fewest axes, then fewest allocated bytes, then schema axis order.
6c. The comparison mode still exists and still refuses a caller contract that a
   ready row contradicts or that no ready row confirms; it never infers equality.
7. A column preserves the load's declared width. Raw memory becomes a typed
   value in exactly one place (`readScalarBits`), which masks to `bitWidth`;
   a uniform value (automatic or contracted) is validated against `bitWidth`
   before folding; and a load is served only by a column of its own width.
8. Site identity is `(offset, accessSize, bitWidth)`, so two integer loads of
   different widths at one address occupy two columns and are never coerced
   into each other.
9. Whole-entry readiness: a lowered plan blocks the legacy compile-time
   `may_const` fold in the planned entry, so no unrecorded constant dependency
   can be presented as protected by the plan. The fold is blocked, not deleted:
   the load keeps its original dynamic form and is counted.
10. The exported admission contract carries the obligations a later member must
    satisfy (uniform values bit-exactly, already-published projected values
    bit-exactly, coordinates inside the declared schema/capacity). A conflicting
    member is classified as a conflict and must stay AOT; the compiler never
    widens or rewrites an existing contract in place — a new domain is a new
    plan with a new identity hash.
11. B0: nothing is specialized without a readiness fact and a protected read
    borrow. A missing provider, a refused borrow, an epoch-less source, an empty
    or uninitialized member set, and a generation that moved during planning or
    publication all refuse; the runtime takes the borrow again for every
    publication path and releases it exactly once on every exit.
12. B1: published projection values are immutable. A repeated coordinate with a
    different value is a `Conflict` (never an overwrite), an out-of-capacity
    coordinate is refused, and a member is dispatchable only after
    `validateAdmission` admits it and its rows are published into **this**
    compile's resource. Column identity is asserted by resolved address, never by
    symbol spelling. Re-validating an already-admitted member whose own values
    changed classifies it as a `Conflict`, so it leaves the specialized path
    without republishing anything.
13. B1: a generation change is coalesced and bounded. One `beginNextGeneration`
    in flight and one pending set; the union keeps every tracked member's
    dependency identity, only the fields whose values changed widen (per-field
    solver), migration happens entirely before the commit, an in-flight admitted
    execution blocks the change, the old resource is retained with its bytes
    accounted, and retiring above the live generation is refused.
14. B2: the sampling budget is ONE aggregate per entry/code generation
    (default 64, configurable). A dispatch that is not admitted — not ready,
    never admitted, conflicting, unprepared, frozen or cancelled — never consumes
    budget; above-budget executions still run but are not counted and are not
    in flight; freeze waits for admitted executions that are still in flight and
    produces one immutable bundle whose counters are the real captured Tier-1
    addresses and whose resource identity is the one both tiers bound.
14b. B2: the sampling window itself runs under the configuration side's
    protected read borrow. The first counted execution takes it, it is held
    across the whole window (not per sample), and freeze/cancel release it
    exactly once. If no borrow can be taken the dispatch is refused with that
    reason — the instrumented entry is never called on an unprotected source and
    no budget is consumed (`RuntimeSamplingRefusesWithoutAProtectedBorrow`).
15. B2: stale callbacks are never merged. A ticket from a frozen, cancelled or
    replaced session is rejected and counted; a configuration generation change
    invalidates the old session instead of reusing its samples; late members
    never restart the quota.
16. B: the whole runtime is single-owner and OFF by default. No provider
    installed means no specialization; no product switch is flipped by this
    change.

## 6. Honest gaps / not verified

- **Milestone B is implemented as a runtime component and driven end-to-end by
  tests, but is not a product feature.** What exists (ST-B1..ST-B7): the
  fail-closed readiness/borrow provider interface, the labeled host adapter, the
  shared table resource with generation identity and immutable incremental
  publication, contract-based admission, one common T1 per entry/code generation
  with the confirmed aggregate-64 configurable budget, in-flight-aware freeze,
  one immutable profile bundle and common T2 — all through the real
  `EJitOrcEngine`, with real ORC/JITLink materialization and execution.
- **Missing product binding (not claimed, deliberately fail-closed).** (1) The
  readiness source is the explicitly labeled host adapter
  `host-adapter.pr231-not-product`; the product configuration-transaction
  completion point is not in this repository, and `ejit_activate` /
  `ejit_deactivate` are explicitly **not** treated as readiness evidence (spec
  §6.1.1/§6.2: an instance having been activated or executed, or a caller's
  current cell, does not prove that other rows are initialized and stable).
  Replacing `EJitSmallTableHostProvider` with an implementation over the
  product's configuration commit is the exact remaining binding; the interface
  does not change. (2) No production `Runtime`/`CompileDriver`/worker caller
  consumes this runtime yet: nothing in the product publishes a logical slot from
  `admitMember`/`enterAdmitted`, and the AOT wrapper does not call the
  enter/leave hooks, so the runtime is exercised by the test TU only. (3) The
  per-call `tableReady(row)`/`codeReady(code_id)` slot publication of §6.5 is
  therefore also absent in production. Until (1)-(3) exist, an entry must not be
  advertised as online-specializable: defaults stay OFF and no product entry is
  enabled by this change.
- The runtime evidence is host-only: the common T1 on this COFF host needs the
  registered `__llvm_profile_runtime` hook stand-in (the freestanding product
  image provides that symbol; ELF/Linux does not need it because
  `InstrProfilingLowering::emitRuntimeHook` returns early). The engine change
  that makes such a registered symbol resolvable for transform-created
  references is product-relevant and covered by the full suite, but the hook
  itself is harness support, labeled as such in the test source.
- The A1 solver's projection scope is the declared, enumerable axis set on the
  cell/TRP schema (plus the historical modulo-argument axis). It is not a general
  optimal index-compression search: out-of-range candidates and budget
  exhaustion are refused so the affected loads keep their original form. A
  per-axis "no comparable neighbour" heuristic is deliberately absent.
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
  HEAD. The local 52/52 above is the current evidence. No BE object was
  regenerated for the A1 change (the A1 code changes the compiler's IR/plan
  emission path; the AArch64 object gate was **not** re-run and is therefore a
  missing gate for this HEAD, not a passing one).
- Windows-native limits of this evidence: assertions-on execution of the EJIT
  library is not available locally (the read-only candidate archives are all
  Release/assertions-OFF and lack ORC in the assertions-on tree), so the
  sub-byte `APInt` abort is characterized by source reading plus the NDEBUG
  symptom and covered by the masking fix and its tests, not by a reproduced
  assertions-on abort. The anti-optimization (`NDEBUG`) configuration is the
  product configuration.
