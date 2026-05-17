# CLAUDE.md

## Project Context

This is the **LLVM monorepo** (llvm-project) on branch `aimv_dev`.
We are implementing AIMV (AI Multi-Level Vectorization) — an AI-driven compiler
optimization feedback system. Design documents in `aimv_design_doc/` are the
authoritative source for architecture, data models, and implementation plans.

**LLVM baseline: 21**. All API calls must be verified against this version.

## Repo Layout

```
aimv_design_doc/          # Design docs (authoritative for AIMV architecture)
  SPEC.md                 #   Requirements spec
  PLAN.md                 #   Technical plan + data models
  LLVM_DESIGN.md          #   LLVM Pass side design
  DRIVER_DESIGN.md        #   Python driver design
  MCP_DESIGN.md           #   MCP REST API server design
  CI_DESIGN.md            #   CI/CD integration design
aimv/                     # AIMV implementation root (to be created)
  driver/                 #   Python driver scripts
  mcp-server/             #   MCP REST API server (FastAPI)
  ci/                     #   CI integration tools
  test/                   #   Tests (lit/, unit/, integration/)
  benchmarks/             #   C benchmark files
  config/                 #   YAML config templates
```

AIMV LLVM-side code goes into:
- `llvm/lib/Transforms/AIMV/` — new pass implementation
- `llvm/include/llvm/Transforms/AIMV/` — public headers
- `llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h` — shared internal header for `emitAIMVDiagnostic()`

Key existing files AIMV touches:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` — add `emitAIMVDiagnostic()` calls
- `llvm/lib/Transforms/Vectorize/LoopAccessAnalysis.cpp` — UnsafeDep insertion point
- `llvm/lib/Passes/PassRegistry.def` — `FUNCTION_PASS("aimv-feedback", ...)`
- `llvm/lib/Passes/PassBuilderPipelines.cpp` — wire into `addVectorPasses()`
- `llvm/lib/Transforms/CMakeLists.txt` — `add_subdirectory(AIMV)`
- `llvm/lib/Passes/CMakeLists.txt` — link `AIMV` component

## Architecture Summary

Three sub-projects, communicating via JSON files and HTTP:

1. **LLVM Pass** (`AIMVFeedbackPass`): Function Pass. LoopVectorize writes structured
   diagnostics into `!aimv.diag` Named Metadata (parallel to ORE remarks).
   AIMVFeedbackPass reads it, enriches with IR context, serializes to JSON.
   Pass accesses Module via `F.getParent()`.

2. **Driver** (`aimv-driver`): Python CLI orchestrating compile→diagnose→MCP→patch→recompile.
   Iteration state machine with rollback and session persistence.

3. **MCP Server** (`aimv-server`): FastAPI REST service. Receives diagnostic JSON,
   constructs LLM prompt, returns `AnalyzeResponse` with unified diffs.

## Key LLVM 21 API Notes

Things that bit us during design review — don't repeat these mistakes:

```cpp
// InstructionCost: NO implicit conversion. Must check isValid() first.
InstructionCost IC = CM->expectedCost(VF);
int cost = IC.isValid() ? (int)IC.getValue() : -1;  // getValue() asserts if Invalid

// Remark Streamer: -fsave-optimization-record sets BOTH getLLVMRemarkStreamer()
// AND getMainRemarkStreamer(). Checking the former is sufficient.
// NOTE: emitAIMVDiagnostic() does NOT check the streamer — it writes
// unconditionally to !aimv.diag. The consumer (AIMVFeedbackPass) controls
// activation via EnabledFlag + OutputPath. This avoids streamer inconsistency
// across opt/clang pipelines. See LLVM_DESIGN.md §1.2.

// MemoryDepChecker::getDependences() returns const SmallVectorImpl<Dependence>*
// — must null-check before iterating.

// Dependence has 8 DepType values. Only 3 bool accessors exist:
//   isForward(), isBackward(), isPossiblyBackward().
// The following methods do NOT exist: isBackwardVectorizable(),
//   isForwardButPreventsForwarding(), isIndirectUnsafe(), isNoDep().
// To get the exact type, access Dep.Type directly or use Dependence::DepName[Dep.Type].
// Use Dependence::isSafeForVectorization(Dep.Type) for safety classification.

// RuntimePointerChecking: use getNumberOfChecks() (method), getPointerInfo(idx)
// (returns const PointerInfo&). PointerInfo is a struct, not a pair.
// LoopAccessInfo has NO hasRuntimePointerChecks() method.
// Use getNumRuntimePointerChecks() instead. PtrRtChecking is always initialized.

// LoopAccessInfo::emitUnsafeDependenceRemark() has NO access to VF, IC, CM, or Checks.
// VF/IC are only determined in LoopVectorize's cost model phase.
// Inside emitUnsafeDependenceRemark, use: TheLoop, PSE.getSE(), Info (local string),
// getNumRuntimePointerChecks(). Get Function/Module via pointer chain:
//   TheLoop->getHeader()->getParent()->getParent()

// Loop::getTripCount() does NOT exist. Use SE.getSmallConstantTripCount(&L).

// GeneratedRTChecks::getCost() returns InstructionCost, may be Invalid.
```

## Build System

Standard LLVM CMake build:

```bash
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="ARM;AArch64"
ninja -C build clang opt
```

New LLVM component libraries use `add_llvm_component_library` in CMakeLists.txt.
Do NOT duplicate `LINK_COMPONENTS` and `LINK_LIBS` — use only `LINK_COMPONENTS`.

Python dependencies (driver + mcp-server):
- `httpx`, `pyyaml`, `jinja2`, `fastapi`, `uvicorn`
- LLM SDKs: `openai`, `anthropic` (optional, backend-dependent)

## Coding Conventions

- **Comment prefix**: `// [AIMV` on all AIMV-added C++ code, `# [AIMV` for Python
- **C++ style**: LLVM conventions (`.clang-format` at repo root)
- **Naming**: `AIMV*` prefix for C++ classes, `aimv_*` for Python modules, `aimv-*` for CLI tools
- **Tests**: `llvm-lit` for C++ pass tests, `pytest` for Python, C files in `benchmarks/`
- **Target platforms**: ARM embedded (armv7/cortex-a9 with NEON as primary target)
- **Independent from EmbeddedJIT**: no shared components

## Design Document Hierarchy

```
SPEC.md  ─── requirements
  └── PLAN.md  ─── architecture, data models (API contract), phases
        ├── LLVM_DESIGN.md  ─── LLVM Pass + Named Metadata
        ├── DRIVER_DESIGN.md  ─── Python iteration engine
        ├── MCP_DESIGN.md  ─── MCP REST server (Pydantic models are authoritative)
        └── CI_DESIGN.md  ─── CI/CD integration
```

When in doubt between documents, the precedence order for data models is:
**MCP_DESIGN.md > PLAN.md > SPEC.md**.
