# EJIT Small Table — Implementation Map, Status and Evidence

Base commit: `027a151b7d8b5b44446dc055c963b1a782754fbb` (documentation-only PR231 on
spec5 `3f0e190dd54752f9c1dcb4bcd1fcb5d6349ae59c`), branch
`codex/ejit-small-table-deepseek`. Milestone A (compiler side) of
`EJIT_SMALL_TABLE_SPEC.md` / `../EJIT_SMALL_TABLE_SPEC.latest.md`.

Current gate: **24/24 focused gtest tests pass**, including real ORC-engine
compiles whose IR transform runs the small-table pass and executes the emitted
table loads against the AOT reference. Log (runner dir, outside git):
`build-obj/test-run-2026-09-11T06-58-21Z.log` (exit 0, full rebuild
`build-obj/build-2026-09-11T06-57-58Z.log`). Two earlier 21/23 and 23/23
iteration logs are preserved next to it.

Changed/added source hashes verified in this run (sha256):

```
1ba57dc589ddce76638ee46360d49d328465594a84eaea092f35e8c52508496a  EJitSmallTable.h
db8027d025b502c7843019099a9b67b1d2a6052ac45dfd01b37c00ef156e9aad  EJitSmallTable.cpp
9630e0e8da31855ade1fbd6ba594759c4d47583cd5d5c60d1384da995016a4a1  EJitSmallTableTest.cpp
9e7954727d3c0fdc866fc71b81bf37054746ddab9fb68770a9b4d45cd02844f2  EJitOptimizer.h
bbc522eead05f5863021b56cc3dc082890d7c325b243c8c59724e0fbdd5a0c72  EJitOptimizer.cpp
53615f7de1846e76f508a77458abe2801c7207840731608c22855dcf17f5aa0f  EJitOrcEngine.h
628914c3703aacb39459a80a932d278f4b9734ba36fe07f6e3da9db3e5c09cc7  EJitOrcEngine.cpp
```

Focused build artifacts (sha256): `build-obj/obj/EJitSmallTable.o`
`ea56c697…`, `EJitOptimizer.o` `1265ce0a…`, `EJitOrcEngine.o` `1e6c5e76…`,
`EJitSmallTableTest.o` `54bb00ab…`, `EJitSmallTableTests` `3ebb4c72…`.

Provenance of every host gate below: the four changed/added translation units
are compiled from this checkout with the exact `compile_commands.json` flags of
the read-only ws6/host build (`ws6` source == the PR231 base for every unchanged
TU), then linked against the ws6 static archives read-only (commands:
`build-obj/build.sh`, log in `../PROGRESS.md`). Only the coordinator can mark a
row `independently-verified`; worker state stops at `implemented`.

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
| ST-A11 | §5 | i1/i8/i16/i32/i64, float/double bit patterns incl. -0.0 and NaN payload; BE decoding; bitcast-punned views | implemented (host) / verified artifact (BE) | `PassCoversScalarWidthsAndFloatBits`, `PassMatchesBitcastPunnedScalarView`; BE bytes in child gate | same | `build-obj/test-run-2026-09-11T06-51-28Z.log`; `children/aarch64be2/out/verify_data.txt` | No host unit test with a BE `DataLayout` (child gate covers it) |
| ST-A12 | §5.5 | volatile/atomic/AS!=0/pointer/aggregate/vector/>64-bit, non-inbounds, `inttoptr`-rooted, non-`urem(arg,mod)`, foreign global, whole-element constant hop, unsupported element types refused | implemented | `PassRefusesVolatileAtomicAndUnsupportedShapes` (AS1 + `inttoptr` included), `PassRefusesWholeElementConstantHop`, `PlannerRefusesOversizedAndDuplicateDomains`, `MaterializeFillsDeclaredSlotsAndRefusesForeignOnes` | same | same | Constant-index overflow and out-of-element hops refused, not aliased |
| ST-A13 | §14.8 | AArch64 **BE** freestanding object: table in `.data`, entry in `.text`, `ADR_PREL_PG_HI21`+`ADD_ABS_LO12_NC` for every table symbol, no GOT for them, no libc/host deps, BE bytes verified | implemented (artifact gate, re-run on post-change snapshot) | `children/aarch64be2/out/checks_report.txt` (checks a-e PASS, exit 0); llc ws6 21.1.8 + GNU readelf 2.46 | pass object compiled by the child from snapshot `1ba57dc5…`/`db8027d0…` (`EJitSmallTable.snapshot.o` `f29c2479…`) | `children/aarch64be2/out/pass_emitted.o` (`5fda0c37…`, byte-identical codegen to the pre-change artifact; only the new load metadata differs), `readelf_*.txt`, `verify_data.txt`, `README.md` | Object is **not** GOT-free overall: `g_cfg`/`g_out` keep `ADR_GOT_PAGE`+`LD64_GOT_LO12_NC`; only the `__ejit_stab_*` symbols are direct. No board execution, no SRE sysroot link |
| ST-A14 | §13 | strategy counters: may_const/table/uniform/kept/refused-shape + `refusedNotReady`; per-entry ready/rows summary | partial | counters asserted in the pass tests | same | same | Per-load refusal reasons and table byte reporting not logged into the product diagnostics yet |
| ST-A15 | §9 (inline) | inline-exposed fields: covered columns are replaced post-inline, uncovered stay original; schema frozen at plan time | partial | `NestedCalleeLoadsStayOutsideThePlan` | same | same | New columns discovered only after inlining need a re-plan (milestone A gap) |
| ST-A16 | §6.5 readiness | partial plans are **refused for executable lowering**: no 0/undef/placeholder row is ever emitted, and no contract is folded from an incomplete domain | implemented (compiler half) | `PartiallyReadyPlanIsRefusedUntilRuntimeAdmission`, `JitPartiallyReadyPlanKeepsAotBehavior`, `UniformContractIsExplicitAndFolds` | same | `build-obj/test-run-2026-09-11T06-51-28Z.log` | **Online partial-row execution is blocked on ST-B2**: incremental `tableReady(row)` admission + a caller attestation are required before this refusal can be relaxed |
| ST-A17 | §9 provenance | synthesized table load carries `!ejit.smalltable.load` (entry + source) and never `!ejit.may_const`, so a later round cannot re-extract it | implemented | `PassReplacesWithDynamicIndexAndKeepsArguments` | same | same | Tag is informational; the pass already refuses foreign roots |
| ST-B1 | §6.2, §6.5 | runtime plan construction from the product config-completion/activate point | planned (milestone B) | — | — | — | Needs a real product readiness signal, not activate alone |
| ST-B2 | §6.5, §6.6 | row readiness/publication + cold-path admission validation before binding a logical slot | planned (milestone B) | — | — | — | An unauthorized member must never reach this code (ST-A16 currently refuses instead) |
| ST-B3 | §6.5, §11 | one stable table address per entry/epoch shared by T1/T2 and later members; publication ordering | planned (milestone B) | — | — | — | Today each compile materializes its own table from the plan snapshot; a row published into a T1 table is a hole in a freshly compiled T2 table |
| ST-B4 | §10 | common T1/T2 PGO, 64 real dispatches per entry/epoch, session/schema isolation, bounded cold-entry expiry | planned (milestone B) | — | — | — | No per-cell quota, no representative-only policy |
| ST-B5 | §7 | update path: old code/tables survive in-flight calls, NO_RECLAIM retention accounting, no RW reopen of published pages | planned (milestone B) | — | — | — | Table is plain data (never in the RX code pool); writability depends on the 4K preset |
| ST-B6 | §10, §13 | unsupported mode combinations rejected before side effects; default OFF preserved | planned (milestone B) | — | — | — | Default OFF is already the compiler behavior |
| ST-B7 | — | diagnostics transport (counters/reasons to the product log) | planned (milestone B) | — | — | — | ST-A14 is the local half |

## 3. Test inventory (all in `llvm/unittests/ExecutionEngine/EJIT/EJitSmallTableTest.cpp`)

24 tests, grouped: planner (5), pass shape/refusals (6), optimizer pipeline (4),
real ORC execution (6), readiness/contract boundaries (4), inline coverage (1).
Run command (runner dir):

```
build-obj/build.sh                     # compile the 4 focused TUs + link
./build-obj/EJitSmallTableTests --gtest_color=no
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
   for the (future) runtime cold path.

## 6. Honest gaps / not verified

- Runtime plan construction, incremental row publication, admission validation,
  one shared table per epoch, common T1/T2 PGO, mode rejection and NO_RECLAIM
  retention are milestone B (ST-B1..ST-B7) and are not implemented here. Online
  partial-row execution is deliberately **blocked** by the compiler refusal.
- The AArch64 gate is an object-inspection gate with `llc` + GNU readelf on a
  fixed snapshot; the private SRE/sysroot freestanding **link** and AArch64
  execution are not available on this host and are not claimed.
- No performance claim: static load counts are not speedup evidence (product
  intent is DCache/L2D/L3D locality; not measured here).
- The in-tree `EJITTests` host link needs the SRE platform primitives that
  LLVMEJIT deliberately does not define; this milestone's test TU supplies host
  stubs and the batched-seal flush, matching the product compile driver's
  publish sequence. This is a pre-existing host-configuration gap, recorded so a
  reviewer does not mistake it for new product behavior.
