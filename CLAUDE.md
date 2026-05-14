# CLAUDE.md

## Project Context

This is the **LLVM monorepo** on branch `aimv_dev`. We are implementing AIMV (AI Multi-Level Vectorization) — an AI-driven compiler optimization feedback system. Design documents live in `aimv_design_doc/`; read them when you need domain context. The design docs are the authoritative source for architecture, data models, and implementation plans — expect them to evolve.

## Repo Layout

```
aimv_design_doc/          # Design documents (authoritative for AIMV architecture)
aimv/                     # AIMV implementation root (to be created)
  driver/                 # Python driver scripts
  mcp-server/             # MCP REST API server (FastAPI)
  ci/                     # CI integration tools
  test/                   # Tests (lit/, unit/, integration/)
  benchmarks/             # C benchmark files for vectorization testing
  config/                 # YAML config templates
llvm/                     # LLVM core (existing monorepo code)
clang/                    # Clang frontend
```

AIMV's LLVM-side code goes into:
- `llvm/lib/Transforms/AIMV/` — new passes
- `llvm/include/llvm/Transforms/AIMV/` — public headers

Key existing files AIMV will touch:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` — add `emitAIMVDiagnostic()` calls
- `llvm/lib/Passes/PassRegistry.def` — register new passes
- `llvm/lib/Passes/PassBuilderPipelines.cpp` — wire into pipeline
- `llvm/lib/Transforms/CMakeLists.txt` — add subdirectory
- `llvm/lib/Passes/CMakeLists.txt` — link new component

## Build System

Standard LLVM CMake build:

```bash
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="X86"
ninja -C build
```

New LLVM component libraries use `add_llvm_component_library` in their CMakeLists.txt.

Python dependencies are lightweight:
- Driver: `httpx`, `pyyaml` (mostly stdlib)
- MCP Server: `fastapi`, `uvicorn`, `openai`/`anthropic` SDKs

## Coding Conventions

- **Comment prefix**: `// [BiSheng]` on all AIMV-added C++ code, `# [BiSheng]` for Python
- **C++ style**: LLVM conventions (`.clang-format` at repo root)
- **Naming**: `AIMV*` prefix for C++ classes, `aimv_*` for Python modules, `aimv-*` for CLI tools
- **Tests**: `llvm-lit` for C++ pass tests, `pytest` for Python, C benchmark files for integration
- **Target platforms**: ARM embedded (armv7/cortex-a9 with NEON as primary target)
- **Independence**: AIMV shares no components with EmbeddedJIT — they are completely separate systems
