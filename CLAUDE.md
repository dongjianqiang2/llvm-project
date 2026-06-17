# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Context

This is a fork of [llvm-project](https://github.com/llvm/llvm-project) on the `bbr_dev_spec0` branch, used to develop **XBBR (eXtended cross-function Basic Block Reordering)** — a new optimization that combines IRPGO with Propeller-style techniques to perform link-time, cross-function basic-block reordering.

**Before starting any XBBR-related work, read the design docs.** The repository's code is the upstream LLVM tree; XBBR has not yet been implemented. The design is split into three documents with a clear contract / design / execution hierarchy:

### Design documents (read these first)

- [`xbbr_design_doc/SPEC.md`](xbbr_design_doc/SPEC.md) — **需求文档 (requirements/contract).** What & why, not how. The source of truth for:
  - Goals, target scenarios, non-goals
  - ABI invariants (function entry block anchoring, blacklist BBs) — **hard constraints, never violate**
  - User-facing CLI (clang/lld options), supported targets & output types
  - Safety & fallback requirements
  - Acceptance metrics (L1i/iTLB/branch-miss thresholds, binary-size budgets)
  - Roadmap (M1–M5), future work, open questions

- [`xbbr_design_doc/PLAN.md`](xbbr_design_doc/PLAN.md) — **设计文档 (implementation design).** How. The source of truth for:
  - Overall architecture & data flow
  - Compiler-side: `XBBRMetadataEmitter` pass, global frequency normalization, blacklist detection
  - Linker-side: 5-stage lld pipeline (`lld/ELF/XBBR/`) — hfsort+ → ExtTSP → multi-objective cost → single-BB fallback → section emission
  - Algorithms (ExtTSP, PH chain merge, cost function, constraint solver pseudocode)
  - DWARF/CFI/EH rewriting implementation, decision map, `llvm-bbreorder-dump`
  - Code organization & directory layout, key C++ data structures (`XBBRGraph`, etc.)
  - New ELF section binary formats (byte-level)
  - Per-milestone overview & risks

- [`xbbr_design_doc/TASK.md`](xbbr_design_doc/TASK.md) — **执行分解文档 (task breakdown).** Who does what, when, how it's verified. Decomposes SPEC/PLAN into independently reviewable tasks (M1–M5 + cross-cutting), each with exit conditions and lit test cases. **TASK is derived from SPEC/PLAN, not a source of truth** — when TASK conflicts with them, SPEC wins over PLAN, PLAN wins over TASK; fix TASK (or the underlying SPEC/PLAN if the contract itself moved).

**Doc discipline:** SPEC is the contract; PLAN is the design. When implementing, PLAN answers "how"; if the implementation diverges from PLAN, update PLAN. If a change touches goals, ABI invariants, user-facing interfaces, or acceptance criteria, it crosses into SPEC territory and SPEC must be updated in the same change. When SPEC and PLAN conflict, **SPEC wins** (it's the contract) — fix PLAN.

## XBBR-Relevant Areas of the LLVM Tree

Implementation will touch (planned):

| Area | Path | Role |
|---|---|---|
| Profile metadata emission | `llvm/lib/CodeGen/` | New `XBBRMetadataEmitter` pass after `MachineBlockPlacement` |
| Existing reuse — block placement | `llvm/lib/CodeGen/MachineBlockPlacement.cpp` | Read-only; provides intra-function baseline |
| Existing reuse — function splitter | `llvm/lib/CodeGen/MachineFunctionSplitter.cpp` | Extend to mark "cross-function migratable" bits |
| Existing reuse — BB sections | `llvm/lib/CodeGen/BasicBlockSections.cpp` | Add `=cross-reorder` mode |
| BB address map | `llvm/include/llvm/Object/ELFTypes.h` (`SHT_LLVM_BB_ADDR_MAP`) | Reused; XBBR enables its PGO-analysis features (`FuncEntryCount`/`BBFreq`/`BrProb`) instead of inventing `.llvm_bb_freq`/`.llvm_cfg_edge`. Only `.llvm_xbbr_attr` + `.llvm_cross_bb_map` are new (PLAN §9) |
| LLD pipeline | `lld/ELF/` — particularly `CallGraphSort.cpp`, `BPSectionOrderer.cpp`, `Thunks.cpp`, `OutputSections.cpp` | Extended for cross-section BB stitching |
| Clang driver flags | `clang/include/clang/Driver/Options.td`, `clang/lib/Driver/ToolChains/Clang.cpp` | New `-fbb-cross-reorder=` option |

Mutual-exclusion checks must be added against existing `--symbol-ordering-file` (Propeller) usage in lld.

## Building LLVM

This is a standard llvm-project monorepo. Common configurations:

```bash
# Configure (from repo root). Adjust LLVM_ENABLE_PROJECTS to scope work.
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_TARGETS_TO_BUILD="X86;AArch64;ARM" \
  -DLLVM_ENABLE_ASSERTIONS=ON \
  -DLLVM_USE_LINKER=lld

# Build
cmake --build build -j

# Build a single target (faster iteration)
cmake --build build -j --target clang
cmake --build build -j --target lld
cmake --build build -j --target llc
```

XBBR targets x86_64, AArch64, and ARM (see SPEC §10). Always include all three in `LLVM_TARGETS_TO_BUILD` when validating.

For full instructions, see https://llvm.org/docs/GettingStarted.html.

## Running Tests

```bash
# All check targets
cmake --build build --target check-all

# Per-component
cmake --build build --target check-llvm
cmake --build build --target check-clang
cmake --build build --target check-lld

# Single lit test (path is relative to the component test root)
build/bin/llvm-lit -v llvm/test/CodeGen/X86/some-test.ll
build/bin/llvm-lit -v lld/test/ELF/some-test.s

# Filter within a directory
build/bin/llvm-lit -v --filter=xbbr llvm/test/CodeGen/X86/
```

Per the SPEC's testing pyramid, XBBR work must add tests at:

- `llvm/test/CodeGen/{X86,AArch64,ARM}/xbbr/` — metadata section emission, option parsing
- `lld/test/ELF/xbbr/` — pipeline stages, fallback paths, decision-map output
- `llvm-test-suite` (separate repo) — SPEC CPU 2017 + MicroBenchmarks integration

Reproducibility is a CI gate: identical `(source + profile + flags)` must produce bitwise-identical binaries. Use stable tie-breakers (file/section index + offset), never pointer or hash-map iteration order.

## Code Review Discipline

From `.github/copilot-instructions.md` (applies to all reviews in this repo, especially XBBR work which manipulates control flow):

> When performing a code review, pay close attention to code modifying a function's control flow. Could the change result in the corruption of performance profile data? Could the change result in invalid debug information, in particular for branches and calls?

XBBR is exactly such a change. Every patch must explicitly account for:

- Profile data integrity (BB frequency / CFG edge weights survive transformations)
- Debug info correctness — especially `.debug_line`, `.debug_ranges`, `DW_AT_ranges` on `DW_TAG_subprogram`, `.eh_frame` FDE splitting, ARM `.ARM.exidx` multi-segment entries (SPEC §8.1)
- Branch range constraints on AArch64 (±128MB) and ARM (±32MB / ±16MB Thumb) — thunk budget enforcement (SPEC §9)

## Branch & Workflow

- Working branch: `bbr_dev_spec0`
- Upstream / PR target: `main`
- Upstream strategy: XBBR enters tree as `experimental-` prefixed feature; prefix removed after M5 (SPEC §13).
