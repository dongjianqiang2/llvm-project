# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

AIMV (AI Multi-Level Vectorization) is a design documentation repository for an AI-driven compiler optimization feedback system. The system creates a closed-loop iteration: compile → diagnose vectorization failures → AI analysis → source patching → recompile. All documents are written in Chinese (Simplified) and target embedded/HPC developers auto-vectorizing legacy C/C++ code.

## Repository Structure

This is a **documentation-only** repository (no source code yet). The documents form a layered design hierarchy:

- **SPEC.md** — Requirements specification: core workflow, system forms (Driver/MCP/LLVM Pass), user interaction modes, modification aggressiveness levels, success metrics
- **PLAN.md** — Technical plan: directory layout, architecture diagrams, data models (Pydantic schemas), interface definitions, prompt engineering templates, implementation phases (6 phases over ~23 weeks)
- **DRIVER_DESIGN.md** — Driver orchestration script detailed design: iteration state machine, subprocess management, source patching with rollback, MCP client, iteration decision engine, session persistence
- **LLVM_DESIGN.md** — LLVM side design: `!aimv.diag` Named Metadata format, `emitAIMVDiagnostic()` hook points in LoopVectorize.cpp, `AIMVFeedbackPass` Module pass, PassBuilder pipeline integration
- **MCP_DESIGN.md** — MCP REST API server design: FastAPI endpoints, Pydantic models, LLM backend abstraction (OpenAI/Anthropic/DeepSeek), prompt templates (Jinja2), diagnostic fingerprint caching, suggestion parser
- **CI_DESIGN.md** — CI/CD integration: GitHub Actions and GitLab CI pipelines, incremental change detection (git diff + clang AST), batch analysis tool, MR report templates, gating strategies

## Key Architectural Concepts

**Three-system form**: Driver (Python orchestration), LLVM Pass (`AIMVFeedbackPass`), MCP REST API (AI analysis server). These communicate via JSON files and HTTP.

**Closed-loop iteration**: Compile → `!aimv.diag` metadata → AIMVFeedbackPass JSON → MCP REST API → LLM suggestion → source patch → recompile+test. Terminates on success, max rounds (default 5), or regression.

**Diagnostic channel**: LoopVectorize emits structured diagnostics into `!aimv.diag` Named Metadata (parallel to existing ORE remarks), consumed by AIMVFeedbackPass which enriches with IR context and serializes to JSON.

**Three modification levels**: conservative (qualifiers only), moderate (+ loop transformations), aggressive (+ data structure changes). Escalates automatically when AI has no suggestions at current level.

**MCP Server**: Receives `AnalyzeRequest` JSON, constructs LLM prompt from Jinja2 templates, calls LLM backend, parses structured response into `AnalyzeResponse` with unified diff suggestions. Uses diagnostic fingerprint hashing for cache hits.

## Document Relationships

```
SPEC.md → defines requirements
  └── PLAN.md → architecture and implementation plan
        ├── DRIVER_DESIGN.md → driver component detail
        ├── LLVM_DESIGN.md → LLVM pass component detail
        ├── MCP_DESIGN.md → MCP server component detail
        └── CI_DESIGN.md → CI integration detail
```

## Conventions

- All documents use version 1.0, dated 2026-04-29
- Code examples in docs are prefixed with `// [BiSheng]` comments
- Data models use Python dataclasses/Pydantic (driver/server) and C++ (LLVM)
- The project is **independent from EmbeddedJIT** — no shared components
- Target platforms are ARM embedded (e.g., armv7, cortex-a9 with NEON)
