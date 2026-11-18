# EJIT LLVM Backend Trimming — `EJIT_TRIM_LLVM_BACKEND`

**Version**: 1.0
**Date**: 2026-06-10
**Related**: EJIT_LIBRARY_TRIMMING.md, EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL_STUBS.md
**Goal**: Document the `EJIT_TRIM_LLVM_BACKEND` CMake flag: what it removes, how to build with it, current size data, and the roadmap for further trimming.

---

## 1. Purpose

`EJIT_TRIM_LLVM_BACKEND` is a **stable** CMake option that removes LLVM backend passes and source files that EJIT's JIT compilation workloads do not exercise.

The flag has two effects:

1. **CMakeLists-level**: source files for the excluded passes are never compiled, and the corresponding TableGen steps are skipped.  This happens at build time, not link time.
2. **Preprocessor-level**: `#ifndef EJIT_TRIM_LLVM_BACKEND` guards inside several LLVM source files prevent the excluded passes from being registered or added to the pipeline at all (belt-and-suspenders for cases where an `.o` is still pulled in by another dependency).

The flag is declared `OFF` by default so that ordinary LLVM builds are unaffected:

```cmake
# llvm/CMakeLists.txt
option(EJIT_TRIM_LLVM_BACKEND
  "Trim LLVM backend pieces used by EJIT: disable heavyweight debug info,
   GlobalISel, and optional AArch64 SME/SVE passes" OFF)
if(EJIT_TRIM_LLVM_BACKEND)
  add_definitions(-DEJIT_TRIM_LLVM_BACKEND)
endif()
```

---

## 2. What Is Removed

### 2.1 AArch64 Pass Registration (`AArch64TargetMachine.cpp`)

When `EJIT_TRIM_LLVM_BACKEND` is defined, the following passes are **not registered**:

| Pass | Purpose | Why removed |
|---|---|---|
| `SMEABIPass` | SME calling-convention lazy-saving | EJIT never lowers SME-attributed functions |
| `SMEPeepholeOptPass` | SME instruction peepholes | Dead without SME ABI |
| `SVEIntrinsicOptsPass` | SVE intrinsic expansion | EJIT workloads don't use SVE intrinsics |
| `AArch64AdvSIMDScalarPass` | Convert AdvSIMD vector ops to scalar equivalents | Not beneficial for EJIT's scalar/general-purpose codegen |
| `AArch64SIMDInstrOptPass` | AArch64 SIMD instruction substitutions | Same as above |

### 2.2 Source Files Excluded from Build (`AArch64/CMakeLists.txt`)

The CMakeLists guard `if(NOT EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL AND NOT EJIT_TRIM_LLVM_BACKEND)` removes:

**SME / SVE / AdvSIMD passes:**

| File | Removes |
|---|---|
| `SMEABIPass.cpp` | SME ABI calling-convention pass |
| `SMEPeepholeOpt.cpp` | SME peephole optimizer |
| `SVEIntrinsicOpts.cpp` | SVE intrinsic opt pass |
| `AArch64AdvSIMDScalarPass.cpp` | AdvSIMD → scalar conversion pass |
| `AArch64SIMDInstrOpt.cpp` | SIMD instruction substitution pass |

**GlobalISel infrastructure:**

| File | Removes |
|---|---|
| `GISel/AArch64CallLowering.cpp` | GlobalISel call lowering |
| `GISel/AArch64GlobalISelUtils.cpp` | GlobalISel utilities |
| `GISel/AArch64InstructionSelector.cpp` | GISel instruction selector |
| `GISel/AArch64LegalizerInfo.cpp` | GISel legalizer |
| `GISel/AArch64O0PreLegalizerCombiner.cpp` | Pre-legalizer combiner (O0) |
| `GISel/AArch64PreLegalizerCombiner.cpp` | Pre-legalizer combiner |
| `GISel/AArch64PostLegalizerCombiner.cpp` | Post-legalizer combiner |
| `GISel/AArch64PostLegalizerLowering.cpp` | Post-legalizer lowering |
| `GISel/AArch64PostSelectOptimize.cpp` | Post-select optimizer |
| `GISel/AArch64RegisterBankInfo.cpp` | Register bank info for GISel |
| `AArch64Arm64ECCallLowering.cpp` | Arm64EC calling convention |

**TableGen steps skipped:**

| TableGen output | Purpose |
|---|---|
| `AArch64GenGlobalISel.inc` | GISel DAG patterns |
| `AArch64GenO0PreLegalizeGICombiner.inc` | O0 combiner rules |
| `AArch64GenPreLegalizeGICombiner.inc` | Pre-legalizer rules |
| `AArch64GenPostLegalizeGICombiner.inc` | Post-legalizer combiner rules |
| `AArch64GenPostLegalizeGILowering.inc` | Post-legalizer lowering rules |
| `AArch64GenRegisterBank.inc` | Register bank definitions |

**Link component removed:**

| Library | Effect |
|---|---|
| `GlobalISel` | `libLLVMGlobalISel.a` not linked into AArch64CodeGen |

### 2.3 AArch64CodeGen Library Membership

With `EJIT_TRIM_LLVM_BACKEND=ON`, `libLLVMAArch64CodeGen.a` contains **43 `.o` files** (down from ~56 when all SME/SVE/GISel files are included):

```
AArch64A57FPLoadBalancing  AArch64AsmPrinter  AArch64BranchTargets
AArch64CallingConvention   AArch64CollectLOH  AArch64CondBrTuning
AArch64ConditionalCompares AArch64DeadRegisterDefinitionsPass
AArch64ExpandImm           AArch64ExpandPseudoInsts
AArch64FalkorHWPFFix       AArch64FastISel    AArch64A53Fix835769
AArch64FrameLowering       AArch64CompressJumpTables
AArch64ConditionOptimizer  AArch64RedundantCopyElimination
AArch64ISelDAGToDAG        AArch64ISelLowering AArch64InstrInfo
AArch64LoadStoreOptimizer  AArch64LowerHomogeneousPrologEpilog
AArch64MachineFunctionInfo AArch64MachineScheduler AArch64MacroFusion
AArch64MIPeepholeOpt       AArch64MCInstLower  AArch64PointerAuth
AArch64PostCoalescerPass   AArch64PromoteConstant AArch64PBQPRegAlloc
AArch64RegisterInfo        AArch64SLSHardening AArch64SelectionDAGInfo
AArch64SpeculationHardening AArch64StackTagging AArch64StackTaggingPreRA
AArch64StorePairSuppress   AArch64Subtarget   AArch64TargetMachine
AArch64TargetObjectFile    AArch64TargetTransformInfo
```

### 2.4 Register Allocators

Only the greedy register allocator (`RAGreedy`) is retained. EJIT codegens at a
fixed optimization level and only ever selects greedy, so the alternatives and
their advisory infrastructure are compiled out:

| Removed | Notes |
|---|---|
| `RegAllocBasic` | basic allocator |
| `RegAllocFast` | O0 / fast allocator |
| `RegAllocPBQP` / `AArch64PBQPRegAlloc` | PBQP allocator (unused at `-Os`) |
| `MLRegAllocEvictAdvisor`, `RegAllocPriorityAdvisor`, ML/priority eviction paths | the default heuristic eviction is kept |

Guarded in `llvm/lib/CodeGen/RegAlloc*.cpp` and `TargetPassConfig::createRegAllocPass`.

### 2.5 CodeGen and MC Trims

The following are also removed under the flag:

| Removed | Why |
|---|---|
| `MachineVerifier` | debug-only machine-IR correctness checker |
| Textual assembly printer + per-instruction name tables | EJIT emits object code, not `.s` — opt back in with `EJIT_DUMP_ASM` (below) |
| `MachinePipeliner` / software pipelining | not used by EJIT codegen |
| Non-ELF object writers (MachO / COFF / Wasm / XCOFF) + x86 bitcode auto-upgrade path | EJIT targets ELF AArch64 only |
| X86 backend dependencies | AArch64-only build |
| Residual CodeView symbols, `.note.*`, and `.comment` sections | cleaned out of the final merged `ejit.o` during the lipo merge step |

**`EJIT_DUMP_ASM` (opt-in).** Re-adds the textual assembly printer and its
instruction-name tables so `ejit_dump_*()` can emit human-readable AArch64
assembly under a trimmed build. Off by default because those tables have a size
cost. The dump path takes `snprintf` from either libc or the SRE format stubs —
never both.

### 2.6 Opt-In and Non-ELF Paths

These guards remove code that only runs behind a non-default option, for a
non-ELF or Windows target, or for debugging. Default codegen is unchanged:
`llc -O2` objects and `opt default<O2>/<O3>` output are byte-identical with and
without the guards.

| Removed | Guard site | Effect of the guard |
|---|---|---|
| GVNHoist, GVNSink, NewGVN, DFAJumpThreading | `PassBuilderPipelines.cpp` | `-enable-gvn-hoist`, `-enable-gvn-sink`, `-enable-newgvn`, `-enable-dfa-jump-thread` are ignored |
| Flow-sensitive AutoFDO (MIRSampleProfile, MIRFSDiscriminator, sample-profile reader) | `TargetPassConfig.cpp` | `-enable-fs-discriminator` / `-fs-profile-file` are ignored |
| Machine debugify, ImplicitNullChecks | `TargetPassConfig.cpp` | `-debugify-and-strip-all-safe` / `-enable-implicit-null-checks` are ignored |
| Unused legacy codegen registrations (EH prepare for SjLj/WinEH/Wasm, BB sections, MFS, static-data splitter, MachineRegionInfo, ModuloSchedule test, machine uniformity, CFG/cycle printers, HardwareLoops, CFGuardLongjmp) | `CodeGen.cpp` | only reachable through non-default options or non-ELF targets |
| Windows CFGuard longjmp / EHCont targets | `AArch64TargetMachine.cpp` | Windows triples only |
| WinEH state numbering | `FunctionLoweringInfo.cpp` | funclet (MSVC / SEH / CoreCLR) personalities hit a fatal error |
| MIR serialization (MIRPrinter, YAML) | `CodeGenTargetMachineImpl.cpp` | `-stop-before` / `-stop-after` hit a fatal error |
| ML-release and replay inline advisors | `InlineAdvisor.cpp`, `Inliner.cpp` | `-enable-ml-inliner=release` fails; inline replay files are ignored |
| DXIL, DDG, IR2Vec and function-properties analyses and printers | `PassRegistry.def` | consumed only by DirectX, DDG printing and the ML inliner |
| Itanium profile remapper (ItaniumManglingCanonicalizer) | `InstrProfReader.cpp` | a profile remapping file is rejected |
| CodeGenData file reader (YAML) | `CodeGenData.cpp` | `-codegen-data-use-path` is ignored |
| RuntimeDyld fallback layer | `LLJIT.cpp` | EJIT targets always select JITLink; other triples get an error |
| Legacy `-debugify` registrations | `Debugify.cpp` | new-PM `debugify` is unaffected |
| MS / Rust / D demanglers, Arm64EC mangling, non-ELF asm-parser extensions | `Demangle.cpp`, `Mangler.cpp`, `MCParser/AsmParser.cpp` | these files now map the stable flag to their existing `EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL` guards |

`CtxProfAnalysis` stays registered because the AOT `buildModuleInlinerPipeline`
requests it, and `RegionInfoAnalysis` stays because ControlHeightReduction uses
it under PGO.

On native AArch64 this removes about 1.0 MB of `.text`/`.rodata`/`.data` from
`ejit.o` (19.57 MB → 18.51 MB loadable; file 38.1 MB → 35.6 MB).

---

## 3. Relationship with `EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL`

### 3.1 CMake-Level Implication

`EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL` implies `EJIT_TRIM_LLVM_BACKEND` at the CMake level:

```cmake
option(EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL
  "Trim backend for bare-metal: exclude non-ELF formats, non-AArch64 targets,
   DWARF/CFI" OFF)
if(EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL)
  add_definitions(-DEJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL)
  add_definitions(-DEJIT_TRIM_LLVM_BACKEND)    ← also sets the stable flag
endif()
```

So enabling the experimental flag activates **both** layers of trimming.

### 3.2 Source-Level Implication (Reverse Direction)

Inside every source file that carries both flag styles, the stable flag **implies** the experimental flag locally:

```cpp
// Top of AArch64TargetMachine.cpp, AsmPrinter.cpp, EHStreamer.cpp,
//         AArch64AsmBackend.cpp, AArch64MCAsmInfo.cpp,
//         AArch64MCTargetDesc.cpp, AArch64TargetStreamer.cpp,
//         ObjectFile.cpp …
#if defined(EJIT_TRIM_LLVM_BACKEND) && !defined(EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL)
#define EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL
#endif
```

This means:
- Setting `-DEJIT_TRIM_LLVM_BACKEND=ON` in CMake **activates all experimental guards** inside these files automatically, even without `-DEJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL=ON`.
- The CMakeLists guards that key on `NOT EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL AND NOT EJIT_TRIM_LLVM_BACKEND` are satisfied by either flag.

### 3.3 Guard Map

| Guard | Activated by stable flag | Activated by experimental flag |
|---|---|---|
| SME/SVE/AdvSIMD pass removal (AArch64TM.cpp) | ✅ directly | ✅ via stable implication |
| GlobalISel CMakeLists removal | ✅ (joint guard) | ✅ (joint guard) |
| DWARF debug info removal (AsmPrinter) | ✅ via local `#define` | ✅ directly |
| CodeView / WinEH / WasmEH removal | ✅ via local `#define` | ✅ directly |
| ObjCARC / CFGuard pass removal | ✅ via local `#define` | ✅ directly |

In practice, both flags converge to the same set of trimmed source files.  The stable flag is the recommended choice for **all EJIT builds**; the experimental flag is the CMake option that was historically used first.

---

## 4. Current Trimming Progress and Size Data

All measurements are `build_release_aarch64` built with `clang`/`clang++`, `-Os -ffunction-sections -fdata-sections`, `LLVM_TARGETS_TO_BUILD=AArch64`.

> The tables below reflect the SME/SVE/GISel scope only; they predate the
> register-allocator (§2.4) and CodeGen/MC (§2.5) trims and the per-CPU
> scheduling-model stripping (§5.3.1), and should be re-measured.

### 4.1 Lipo Pipeline Results (`EJIT_TRIM_LLVM_BACKEND=ON`, no `--exclude`)

| Stage | Size | Object count |
|---|---|---|
| `libejit_lipo_aarch64.a` (extract) | **94 MB** | **956 `.o`** |
| `libejit_lipo_aarch64_gc.a` (gc-merge) | **55 MB** | — |
| `ejit.o` (merge) | **34 MB** | — |

### 4.2 Section Breakdown of `ejit.o`

| Section | Size |
|---|---|
| `.text` | 12.9 MB |
| `.rodata` | 4.1 MB |
| `.data` | 376 KB |
| `.bss` | 290 KB |
| Total loadable | ~17.7 MB |

### 4.3 Size Reduction vs. Untrimmed Baseline

The untrimmed baseline (from EJIT_LIBRARY_TRIMMING.md §5.2) with the aarch64-linux-gnu cross-compiler:

| Metric | No trimming (1053 `.o`) | `EJIT_TRIM_LLVM_BACKEND=ON` (956 `.o`) | Reduction |
|---|---|---|---|
| Extract `.a` | 115 MB | ~94 MB | ~21 MB (−18%) |
| gc-merge `.a` | 65 MB | ~55 MB | ~10 MB (−15%) |
| `ejit.o` | 42 MB | ~34 MB | ~8 MB (−19%) |
| `.text` | 14.4 MB | 12.9 MB | ~1.5 MB (−10%) |
| Object count | 1053 | 956 | −97 (−9%) |

> **Note**: The cross-compiler vs. native-clang difference accounts for a few MB of variance; the reduction figures are indicative rather than exact.

---

## 5. Build Instructions

### 5.1 CMake Configuration

Add `-DEJIT_TRIM_LLVM_BACKEND=ON` to the standard AArch64 CMake invocation:

```bash
cmake -S llvm -B build_release_aarch64 -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS="-ffunction-sections -fdata-sections" \
  -DCMAKE_CXX_FLAGS="-ffunction-sections -fdata-sections" \
  -DCMAKE_C_FLAGS_RELEASE="-Os -DNDEBUG" \
  -DCMAKE_CXX_FLAGS_RELEASE="-Os -DNDEBUG" \
  -DLLVM_TARGETS_TO_BUILD="AArch64" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_ZLIB=OFF \
  -DEJIT_DEFAULT_TARGET_TRIPLE=aarch64-none-elf \
  -DEJIT_TRIM_LLVM_BACKEND=ON          # ← enables the stable trimming
```

> `EJIT_TRIM_LLVM_BACKEND=ON` is **not** the same as `EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL=ON` at the CMake level, but it has the same net effect on all participating source files (see §3).  The stable flag is preferred for all builds going forward.

### 5.2 Incremental Build

After changing any guarded source file or CMakeLists.txt:

```bash
# Rebuild the EJIT runtime library and the AArch64 backend
ninja -C build_release_aarch64 LLVMEJIT

# If you also touched lld or clang:
ninja -C build_release_aarch64 LLVMEJIT lld clang
```

### 5.3 Running the Lipo Pipeline

The lipo pipeline packages the build output into a single relocatable `ejit.o` for bare-metal deployment.  Run it from the `llvm-project` root:

```bash
# Full three-step pipeline (extract → gc-merge → merge)
~/ejit/llvm-project/ejit_test/lipo/run_aarch64_pipeline.sh build_release_aarch64 ejit_test/lipo/ejit.o
```

The script wraps:

```bash
# Step 1 — extract: linker-map + nm -u dependency tracing
python3 ejit_test/lipo/lipo.py extract \
  --arch=aarch64 --build-dir=build_release_aarch64

# Step 2 — gc-merge: ld -r --gc-sections dead-code elimination
python3 ejit_test/lipo/lipo.py gc-merge \
  --input=ejit_test/lipo/libejit_lipo_aarch64.a \
  --build-dir=build_release_aarch64

# Step 3 — merge: ld -r -T merge.ld section merging + .group DISCARD
python3 ejit_test/lipo/lipo.py merge \
  --input=ejit_test/lipo/libejit_lipo_aarch64_gc.a \
  --build-dir=build_release_aarch64 \
  --output=ejit_test/lipo/ejit.o
```

To inspect the contents of the extracted archive:

```bash
ar t ejit_test/lipo/libejit_lipo_aarch64.a | wc -l    # total .o count
ar t ejit_test/lipo/libejit_lipo_aarch64.a | grep GISel  # confirm GISel is absent
```

### 5.3.1 Per-CPU Scheduling Model Stripping (`--keep` / `--keep-none`)

`AArch64GenSubtargetInfo.inc` (a TableGen output) defines ~24 per-CPU scheduling
models. Each carries a ~24 KB `MCSchedClassDesc` array plus a smaller
`MCProcResourceDesc` array, and every model is kept alive by the CPU-name lookup
table `AArch64SubTypeKV` — so `gc-sections` cannot remove them through normal
linkage. `ejit_strip_sched_models.py` patches the `.inc`: it nulls the
`ProcResourceTable` / `SchedClassTable` pointers and zeros the matching
`NumProcResourceKinds` / `NumSchedClasses` in each `MCSchedModel` initializer
that should be dropped. Those arrays then become unreferenced and the lipo
`gc-merge` step eliminates them — roughly **24 KB per stripped model**.

This is an **opt-in lipo-pipeline step, independent of the CMake trim flag**,
driven through `run_aarch64_pipeline.sh`:

```bash
# Keep ONE CPU's scheduling model, strip the other ~23. Use this when the
# deployment core is known and accurate machine scheduling matters. CPUNAME must
# be an LLVM CPU name present in AArch64SubTypeKV (e.g. neoverse-n2, cortex-a57):
~/ejit/llvm-project/ejit_test/lipo/run_aarch64_pipeline.sh --keep neoverse-n2

# Strip ALL per-CPU models (smallest object). The target scheduler falls back to
# the generic model — functional, but less precise instruction scheduling:
~/ejit/llvm-project/ejit_test/lipo/run_aarch64_pipeline.sh --keep-none

# Default (no flag): no stripping. If the .inc was previously patched, the script
# regenerates it from TableGen and rebuilds the AArch64 libraries first.
```

Notes:

- **One kept model can cover a whole CPU family** — e.g. `--keep cortex-a57`
  keeps `CortexA57Model`, which also serves a72/a73/a75–a78c.
- The script marks the patched `.inc` with a `// EJIT: stripped` sentinel and
  rebuilds `LLVMAArch64CodeGen` + `LLVMAArch64Desc` before the lipo steps run.
  Switching back to a no-strip build regenerates the `.inc` from `llvm-tblgen`
  automatically (delete the `.inc` and run `ninja AArch64CommonTableGen`, which
  `run_aarch64_pipeline.sh` does for you).
- To run the patch by hand, outside the pipeline:

  ```bash
  python3 ejit_test/lipo/ejit_strip_sched_models.py BUILD_DIR [KEEP_CPU]
  ninja -C BUILD_DIR LLVMAArch64CodeGen LLVMAArch64Desc
  ```

Choose `--keep <deployment-cpu>` when you know the exact target core and want
good scheduling; choose `--keep-none` for the smallest object when scheduling
precision is not critical.

### 5.4 Running the Test Suite

```bash
./ejit_test/build.sh --run --lipo=ejit_test/lipo/ejit.o
```

---

## 6. Plans for Future Trimming

Backend trimming now covers SME/SVE/GISel (§2.1–2.3), the non-greedy register
allocators (§2.4), and the machine verifier / textual ASM / non-ELF writers /
X86 dependencies (§2.5), plus the opt-in per-CPU scheduling-model stripping
(§5.3.1). There is still headroom below `ejit.o`.

### 6.1 Additional AArch64 Pass Exclusions

The following passes are still registered and pulled into `ejit.o`.  They are candidates for a future `#ifndef EJIT_TRIM_LLVM_BACKEND` guard once their impact is measured:

| Pass | Estimated size | Risk |
|---|---|---|
| `AArch64StackTaggingPass` / `StackTaggingPreRA` | Small | Low — no MTE hardware on typical EJIT targets |
| `AArch64PointerAuthPass` | Small | Low — no PAC hardware on typical EJIT targets |
| `AArch64SLSHardeningPass` | Negligible | Medium — security hardening |
| `FalkorHWPFFix` / `FalkorMarkStridedAccesses` | Small | Low — Falkor CPU specific |

### 6.2 TargetTransformInfo and TargetObjectFile

`AArch64TargetTransformInfo.cpp` and `AArch64TargetObjectFile.cpp` are always compiled in.  The TTI is exercised by the optimization pipeline; the target object file class is needed at MC emit time.  Both are small and low-priority.

### 6.3 Selective `--exclude` Integration

`lipo.py extract --exclude` removes entire `.o` files from the extracted archive that are provably unreachable from the EJIT entry points.  Adding `--exclude` to `run_aarch64_pipeline.sh` in combination with `EJIT_TRIM_LLVM_BACKEND=ON` is the next expected step.  Based on the EJIT_LIBRARY_TRIMMING.md data (978 .o → 658 .o with `--exclude`), applying `--exclude` to the already-trimmed 956 .o baseline should bring the count below 700 and `ejit.o` well under 30 MB.

### 6.4 Demangle and Object Library Trimming

`libLLVMDemangle.a` and `libLLVMObject.a` are linked unconditionally but many of their `.o` files are not reachable at runtime.  These are good targets for `--exclude` pass rather than source-level guards because the unused code paths depend on host-side tooling, not AArch64 backend specifics.

### 6.5 RuntimeDyld / OrcTargetProcess

The RuntimeDyld fallback in `LLJIT` is now compiled out (§2.6). OrcTargetProcess (~0.01 MB) is still pulled in by the OrcJIT layer.  Removing it requires a custom `ExecutorProcessControl` and `JITLinkMemoryManager` implementation for the bare-metal target.  This is tracked as a separate workstream (see EJIT_LIBRARY_TRIMMING.md §7.4).

---

## 7. Quick Reference

```bash
# Configure with stable trimming (current recommended setting)
cmake -S llvm -B build_release_aarch64 -G Ninja \
  ... \
  -DEJIT_TRIM_LLVM_BACKEND=ON

# Rebuild EJIT runtime
ninja -C build_release_aarch64 LLVMEJIT

# Run full lipo pipeline → ejit_test/lipo/ejit.o
~/ejit/llvm-project/ejit_test/lipo/run_aarch64_pipeline.sh

# ... optionally also strip per-CPU scheduling models (§5.3.1):
~/ejit/llvm-project/ejit_test/lipo/run_aarch64_pipeline.sh --keep neoverse-n2  # keep one CPU
~/ejit/llvm-project/ejit_test/lipo/run_aarch64_pipeline.sh --keep-none         # strip all

# Run tests against the lipo-produced object
./ejit_test/build.sh --run --lipo=ejit_test/lipo/ejit.o

# Inspect archive membership
ar t ejit_test/lipo/libejit_lipo_aarch64.a | wc -l

# Verify no GISel / SME files were included
ar t ejit_test/lipo/libejit_lipo_aarch64.a | grep -E "GISel|SMEABI|SVEIntrinsic|AdvSIMD"
```

---

*Document version: 1.1*
*Created: 2026-06-10*
*Updated: 2026-07-07*
