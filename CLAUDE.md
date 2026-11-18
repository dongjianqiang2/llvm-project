# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Configuration

### Host Environment Check (run before any build)

Before invoking `build.sh`, verify the host environment to avoid misconfiguration:

```bash
# 1. Host architecture — determines native vs cross-compile
uname -m
#   x86_64  → native for "x86" arch; "aarch64" requires cross-compiler
#   aarch64 → native for "aarch64" arch; "x86" requires cross-compiler

# 2. CPU cores — this machine is limited (8 cores); don't pass -j higher than $(nproc)
nproc

# 3. Memory — avoid -j > 8 or multiple concurrent builds
free -h
```

Use the top-level `build.sh` script for all builds:

```bash
./build.sh debug x86              # → build_debug_x86/ (dev daily driver)
./build.sh release x86            # → build_release_x86/ (for EJIT tests)
./build.sh debug aarch64          # → build_debug_aarch64/
./build.sh release aarch64        # → build_release_aarch64/
```

### Incremental Build (use these daily)

This machine has **limited CPU and memory**. Use targeted incremental builds:

```bash
# Dev build (debug, shared libs)
ninja -C build_debug_x86 clang opt lld

# Release build (static libs, for ejit_test)
ninja -C build_release_x86 LLVMEJIT lld

# Just the AOT passes library (runs inside clang)
ninja -C build_debug_x86 clang

# Just the runtime library (linked into test binaries)
ninja -C build_release_x86 LLVMEJIT
```

### Build directory layout

| Directory | Type | Arch | Purpose |
|-----------|------|------|---------|
| `build_debug_x86/` | Debug | x86 | Dev: clang, opt, lld (shared libs) |
| `build_release_x86/` | Release | x86 | EJIT tests: LLVMEJIT.a, lld |
| `build_debug_aarch64/` | Debug | AArch64 | Cross-compile dev |
| `build_release_aarch64/` | Release | AArch64 | Cross-compile runtime |

## Running Tests

EJIT tests are spread across four directories. Commands use `$BUILD` as a placeholder for the build directory (e.g. `build_release_x86`, `build_release_aarch64`).

| Test Suite | Directory | Type | How to Run |
|------------|-----------|------|-------------|
| AOT Pass lit | `llvm/test/Transforms/EmbeddedJIT/` | lit (opt + FileCheck) | `cd $BUILD && ./bin/llvm-lit -v ../llvm/test/Transforms/EmbeddedJIT/` |
| Clang lit | `clang/test/CodeGen/ejit_*.c` `clang/test/Sema/ejit_*.cpp` `clang/test/Sema/ext_attr_ejit.cpp` | lit (%clang_cc1 + FileCheck) | `cd $BUILD && ./bin/llvm-lit -v ../clang/test/CodeGen/ejit_* ../clang/test/Sema/ejit_*` |
| Runtime gtest | `llvm/unittests/ExecutionEngine/EJIT/` | gtest (C++ unit tests) | `ninja -C $BUILD EJITTests EJITCodePoolTests EJITSharedTaskPoolTests` |
| Integration | `ejit_test/` | C programs + runtime | `EJIT_CLANG=$BUILD/bin/clang cd ejit_test && ./build.sh` |

**Build prerequisites**:

```bash
ninja -C $BUILD opt clang                  # lit
ninja -C $BUILD EJITTests ...              # gtest
ninja -C $BUILD clang LLVMEJIT lld         # integration
```

> **Note**: Prefer release builds for running EJIT lit tests — debug builds may skip optimizations that affect test behavior.

## Repository Architecture

This is the **LLVM monorepo**. Key top-level directories:

| Directory | Purpose |
|-----------|---------|
| `llvm/` | Core LLVM: IR, passes, codegen, execution engines |
| `clang/` | C/C++/ObjC frontend |
| `lld/` | LLVM linker |
| `compiler-rt/` | Compiler runtime (sanitizers, builtins) |
| `libcxx/`, `libcxxabi/`, `libunwind/` | C++ standard library |
| `mlir/` | MLIR framework |

### LLVM Passes

- **Pass implementations**: `llvm/lib/Transforms/<PassCategory>/`
- **Pass registration** (new PM): `llvm/lib/Passes/PassRegistry.def` — each pass listed as `MODULE_PASS("name", ClassName())` or `FUNCTION_PASS("name", ClassName())`
- **PassBuilder**: `llvm/lib/Passes/PassBuilder.cpp` — pipeline construction and analysis registration
- Passes use `PassInfoMixin<ClassName>` pattern (new pass manager); no manual registration needed beyond `PassRegistry.def`

### LLVM Execution Engine / OrcJIT

- `llvm/lib/ExecutionEngine/` — JIT infrastructure: OrcJIT, JITLink, Interpreter, MCJIT
- `llvm/lib/ExecutionEngine/EJIT/` — EmbeddedJIT runtime library (core engine, cache, compiler, optimizer)
- `llvm/include/llvm/ExecutionEngine/EJIT/` — Runtime headers

### Clang Architecture

- **Attribute definitions**: `clang/include/clang/Basic/Attr.td` (TableGen, ~5200 lines)
- **Semantic analysis**: `clang/lib/Sema/`
- **CodeGen (AST → LLVM IR)**: `clang/lib/CodeGen/`
- **Backend integration** (adding LLVM passes to Clang pipeline): `clang/lib/CodeGen/BackendUtil.cpp`

## This Branch: `ejit_dev_spec4`

This branch is developing **EmbeddedJIT** — an embedded-scenario JIT compilation system based on time-window constants with runtime specialization. Design documents are in `/workspaces/jit_design_doc/`.

### File locations:

- **AOT Passes**: `llvm/lib/Transforms/EmbeddedJIT/` — 5 passes (EJitRegisterBitcode, EJitRegisterPeriod, EJitWrapperGen, EJitPeriodHandler, EJitAotModulePass)
- **Runtime library**: `llvm/lib/ExecutionEngine/EJIT/` — core engine, cache, compiler, optimizer, PASS6
- **Runtime headers**: `llvm/include/llvm/ExecutionEngine/EJIT/`
- **AOT Pass lit tests**: `llvm/test/Transforms/EmbeddedJIT/`
- **Runtime unit tests**: `llvm/unittests/ExecutionEngine/EJIT/`
- **Clang attributes**: `clang/include/clang/Basic/Attr.td`, `clang/lib/Sema/`, `clang/lib/CodeGen/`

### Key design decisions (from SPEC4.md / PLAN4.md):

- **Single-function wrapper**: wrapper logic is inserted directly into the original `ejit_entry` function entry, no separate wrapper function
- **Metadata-driven**: `!ejit.may_const` on load instructions (soft annotation, safe to drop), `!ejit.metadata` on functions/globals
- **Two-phase AOT**: early pass (before O2/O3) extracts raw bitcode with metadata intact; late passes (after O2/O3) add wrapper + period registration
- **OrcJIT + JITLink**: runtime uses LLJIT with custom embedded memory manager (slab allocator, 512KB default)
- **JIT pipeline order**: param substitution → InstCombine → Inline → EJitStructFieldPass → standard LLVM opts (L1/L2/L3)

## Code Review

After completing a non-trivial change, spawn a sub-agent to review the diff before committing:

```
Agent("code-review", prompt: "Review the current git diff for correctness, simplification, and test coverage.")
```

Review focus areas:

- **Correctness**: metadata round-trips, pass ordering, memory/ownership, thread safety, error handling.
- **Simplicity**: reuse existing helpers, avoid duplication, prefer LLVM idioms.
- **Tests**: cover new paths with lit tests (passes) or gtest (runtime); integration tests for end-to-end behavior.
- **Control flow**: changes to branches/calls must not corrupt profile data or debug info. (From `.github/copilot-instructions.md`)
