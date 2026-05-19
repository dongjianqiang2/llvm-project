# AIMV — AI Multi-Level Vectorization

AI-driven compiler optimization feedback loop. Automatically identifies vectorization
failures across **LoopVectorize**, **SLP Vectorizer**, and **Loop Unrolling** passes,
queries an LLM for source-level fixes, applies patches with atomic rollback, and
verifies correctness — all in a closed iteration loop.

```
compile → diagnose (LV+SLP+Unroll) → AI analysis → source patch → recompile + verify → repeat
```

## Architecture

```
┌──────────────────┐   NDJSON    ┌──────────────┐    HTTP     ┌────────────────┐
│  LLVM Passes      │───────────▶│  Driver       │───────────▶│  MCP Server     │
│  LV + SLP + Unroll│            │  (Python CLI) │            │  (FastAPI)      │
│  → !aimv.diag     │            │  orchestrate  │            │  → LLM backend  │
│  → aimv.json      │            │  patch+verify │            │  → AnalyzeResp  │
└──────────────────┘            └──────────────┘            └────────────────┘
```

Three subsystems:

| Component | Language | Role |
|-----------|----------|------|
| **LLVM Passes** | C++ | LoopVectorize, SLPVectorize, LoopUnroll emit diagnostics via `emitAIMVDiagnostic()` into `!aimv.diag` Named Metadata. `AIMVFeedbackPass` parses and serializes to NDJSON. |
| **Driver** (`aimv-driver`) | Python | Orchestrates compile → diagnose → MCP → patch → recompile → verify loop. Supports multi-function sequential processing with rollback. |
| **MCP Server** (`aimv-server`) | Python/FastAPI | Receives `AnalyzeRequest`, builds prompt with source context + IR + cost model, calls LLM, returns `AnalyzeResponse` with unified diffs. |

## Quick Start

### Prerequisites

- Python 3.10+
- `patch` command (for applying diffs)

### 1. Build & Install

```bash
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="ARM;AArch64"
ninja -C build clang opt

# Install clang, opt, aimv-driver, aimv-server to prefix
DESTDIR=/path/to/install ninja -C build install
```

After install:

```
<prefix>/bin/
├── clang-21
├── opt
├── aimv-driver       # AI-driven vectorization CLI
└── aimv-server       # MCP REST API server
```

### 2. Install Python dependencies

```bash
pip install fastapi uvicorn httpx pyyaml jinja2 openai anthropic
```

### 3. Start MCP Server

```bash
# Anthropic backend (e.g. glm-5.1 via BigModel)
ANTHROPIC_API_KEY="your-key" \
ANTHROPIC_BASE_URL="https://open.bigmodel.cn/api/anthropic" \
ANTHROPIC_MODEL="glm-5.1" \
AIMV_LLM_BACKEND="anthropic" \
aimv-server

# OpenAI backend
OPENAI_API_KEY="sk-..." \
OPENAI_BASE_URL="https://api.openai.com/v1" \
OPENAI_MODEL="gpt-4o" \
AIMV_LLM_BACKEND="openai" \
aimv-server

# Mock backend (offline testing)
AIMV_LLM_BACKEND="mock" aimv-server
```

### 4. Run analysis

```bash
# Generate aimv.json with all diagnostic passes
clang -O2 -g --target=armv7-unknown-linux-gnueabi -S -emit-llvm src.c -o src.ll
opt -passes="loop-vectorize,slp-vectorizer,loop-unroll,aimv-feedback" -S src.ll \
    -aimv-output=aimv.json -aimv-enable

# Run driver (installed)
aimv-driver --from-json aimv.json --source src.c \
    --mcp-url http://localhost:8080 --max-rounds 3

# Target a specific function
aimv-driver --from-json aimv.json --source src.c \
    --function process_task --mcp-url http://localhost:8080

# Quick start from source tree (without install)
cd aimv
python3 -m driver.aimv_driver --from-json aimv.json --source src.c \
    --mcp-url http://localhost:8080
```

### 5. One-command automation

```bash
# Full auto: compile + diagnose + MCP + patch + verify
clang -O2 -faimv -faimv-mcp-url=http://mcp:8080 -c file.c
```

The `-faimv` flag triggers the full pipeline automatically via fork+exec of `aimv-driver` after compilation. No separate `opt` or `aimv-driver` invocation needed.

## Configuration

### MCP URL Priority

The MCP server address is resolved in this order:

| Priority | Source | Example |
|----------|--------|---------|
| **1 (highest)** | `-faimv-mcp-url=` flag | `clang -faimv -faimv-mcp-url=http://mcp:8080 -c file.c` |
| **2** | `AIMV_MCP_URL` env var | `export AIMV_MCP_URL=http://mcp:8080` |
| **3** | `~/.aimv/config` YAML | `mcp: { url: http://mcp:8080 }` |
| **4 (lowest)** | built-in default | `http://localhost:8080` |

## Environment Variables

### MCP Server

| Variable | Required | Description |
|----------|----------|-------------|
| `AIMV_LLM_BACKEND` | Yes | `openai` / `anthropic` / `mock` |
| `OPENAI_API_KEY` | openai only | API key |
| `OPENAI_BASE_URL` | openai only | Base URL (e.g. `https://api.openai.com/v1`) |
| `OPENAI_MODEL` | openai only | Model name (e.g. `gpt-4o`) |
| `ANTHROPIC_API_KEY` | anthropic only | API key |
| `ANTHROPIC_BASE_URL` | anthropic only | Base URL (e.g. `https://open.bigmodel.cn/api/anthropic`) |
| `ANTHROPIC_MODEL` | anthropic only | Model name (e.g. `glm-5.1`) |
| `AIMV_API_KEY` | No | Enable Bearer token auth on all endpoints |
| `AIMV_CACHE_TTL` | No | Diagnostic cache TTL in seconds (default: 86400) |
| `AIMV_MCP_URL` | No | MCP server URL for driver (default: `http://localhost:8080`) |
| `AIMV_MAX_ROUNDS` | No | Max iteration rounds per function (default: 5) |
| `AIMV_LEVEL` | No | Modification level: `conservative` / `moderate` / `aggressive` |

All three backend-specific variables (API_KEY, BASE_URL, MODEL) are **required**.
The server refuses to start if any is missing.

### LLM Providers

| Provider | Backend | Protocol | Verified |
|----------|---------|----------|----------|
| **Zhipu GLM** | `anthropic` | Anthropic Messages | Live tested |
| **OpenAI** | `openai` | Chat Completions | SDK verified |
| **DeepSeek** | `openai` | OpenAI-compatible | Compatible |
| **vLLM / Ollama** | `openai` | OpenAI-compatible | Compatible |
| Any OpenAI-compatible | `openai` | Chat Completions | Compatible |
| Any Anthropic-compatible | `anthropic` | Messages API | Compatible |

## Diagnostic Passes

AIMV collects diagnostics from **three LLVM passes**, all sharing the `!aimv.diag` channel:

| Pass | Remark IDs | Has Loop | Has LAI | Has Cost |
|------|-----------|----------|---------|----------|
| **LoopVectorize** | `CantReorderMemOps`, `UnsafeDep`, `VectorizationNotBeneficial`, `InterleavingNotBeneficial`, `LoopVectorized` | Yes | Yes | Yes |
| **SLP Vectorizer** | `UnsupportedType`, `SmallVF`, `NotBeneficial`, `NotPossible` | No | No | No |
| **Loop Unrolling** | `CantUnrollTripCount`, `UnrollNotBeneficial`, `UnrollTooExpensive` | Yes | No | No |

When LAI or CostModel is unavailable (SLP/Unroll), sentinel values (`-1`) are used.
The JSON output uses newline-delimited JSON (NDJSON) — one JSON object per function.

## Driver CLI

```
aimv-driver [OPTIONS] [source_file]

Options:
  --from-json PATH       Read diagnostics from aimv.json (produced by opt)
  --source PATH          Source file (required with --from-json)
  --function NAME        Process only this function
  --mcp-url URL          MCP server URL (default: http://localhost:8080)
  --max-rounds N         Max iteration rounds per function (default: 5)
  --aimv-level LEVEL     conservative | moderate | aggressive (default: conservative)
  --output-dir DIR       Output directory (default: ./aimv-output)
  --dry-run              Collect diagnostics only, skip MCP
  --verbose              Verbose output

# From source tree (without install):
#   cd aimv && python3 -m driver.aimv_driver [OPTIONS]
```

### Server

```bash
aimv-server                          # mock backend (default)
AIMV_LLM_BACKEND=anthropic aimv-server  # with env vars

# Custom host/port:
AIMV_HOST=0.0.0.0 AIMV_PORT=9000 aimv-server

# From source tree:
#   cd aimv && AIMV_LLM_BACKEND=mock python3 -m uvicorn mcp_server.aimv_server:app --host 127.0.0.1 --port 8080
```

## MCP Server API

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/api/v1/analyze-vectorization` | POST | Analyze vectorization failures, return suggestions |
| `/api/v1/health` | GET | Server health + backend info |
| `/api/v1/cache/stats` | GET | Diagnostic cache statistics |
| `/api/v1/feedback` | POST | Record suggestion outcome for prompt optimization |

Authentication (optional): `Authorization: Bearer <AIMV_API_KEY>`

### Request Format

```json
{
  "request_id": "aimv-...",
  "target": {"triple": "armv7-...", "cpu": "cortex-a9", "features": ["+neon"], "vector_width": 128},
  "function": {"name": "process_task", "source_code": "void f(...) {...}", "source_file": "src.c", "loop_line": 5},
  "diagnostics": [
    {
      "pass_name": "LoopVectorize", "remark_id": "CantReorderMemOps",
      "severity": "missed", "loop_location": "src.c:5:3",
      "source_context": "2: void f(...) {\n3:   for ...\n...",
      "ir_snippet": "define void @f(...) { ...",
      "cost_model": {"scalar_cost": 5, "vector_cost": 8, "vf": 4},
      "dependencies": [{"dep_type": "Backward", "source_ptr": "store", ...}],
      "memory_info": {"num_stores": 1, "max_alignment": 4, "stride": "non-constant", ...},
      "loop_info": {"num_blocks": 1, "trip_count": -1, ...}
    }
  ],
  "history": [],
  "aimv_level": "conservative"
}
```

### Response Format

```json
{
  "request_id": "aimv-...",
  "suggestions": [{
    "description": "Add restrict qualifier to pointer parameters",
    "reasoning": "Alias analysis failed because...",
    "diff": "--- a/src.c\n+++ b/src.c\n@@ -1,5 +1,5 @@\n ...",
    "estimated_impact": "high",
    "safety_concern": "Restrict requires that pointers do not alias at runtime."
  }],
  "overall_analysis": "summary paragraph",
  "confidence": 0.85,
  "no_action_possible": false
}
```

## Iteration Flow

```
Function: process_task
  Round 1:
    1. COMPILE   → clang -c -g -mllvm -aimv-enable -mllvm -aimv-output=aimv.json src.c
    2. DIAGNOSE  → parse NDJSON, filter by function_name
    3. MCP       → POST /api/v1/analyze-vectorization
    4. PATCH     → apply_shadow_patch (diff → .aimv-tmp shadow file)
    5. VERIFY    → compile shadow file, check vectorization status
    6. COMMIT    → commit_shadow (atomic replace original with shadow)
    → Continue if still missed loops

  Round N (failure):
    4. PATCH     → apply_shadow_patch FAILS (malformed diff)
    → Rollback all previous rounds' patches for this function
    → BREAK — terminate function processing

  Termination:
    → VECTORIZED  — all loops vectorized
    → ROUND_LIMIT — max rounds reached
    → ROLLBACK    — patch/compile/test failure, source restored
```

## Rollback Mechanism

Every patch application creates a backup before modifying the source.
When any round fails, all patches applied in previous rounds for the
**same function** are rolled back in reverse order, restoring the file
to its original state.

| Failure | Action |
|---------|--------|
| Patch apply fails (malformed diff) | Rollback all → BREAK |
| Shadow compile fails | Discard shadow → Rollback all → engine.decide |
| Vectorization regression | Discard shadow → Rollback all → BREAK |
| Test failure | Discard shadow → Rollback all → BREAK |

## Project Structure

```
aimv/
├── README.md
├── CMakeLists.txt                   # Install rules (bundled with LLVM build)
├── bin/                             # Entry-point scripts
│   ├── aimv-driver                  #   Driver CLI wrapper
│   └── aimv-server                  #   Server wrapper
├── setup.sh                         # One-click server setup
├── driver/                          # Python CLI driver
│   ├── aimv_driver.py               #   Entry point + main loop
│   ├── build_orchestrator.py        #   clang subprocess management
│   ├── source_manager.py            #   Shadow file + rollback + FileLock
│   ├── iteration_engine.py          #   Decision matrix + level escalation
│   ├── mcp_client.py                #   HTTP client for MCP server
│   ├── config.py                    #   DriverConfig: env > YAML > defaults
│   ├── models.py                    #   Data models + enums
│   ├── session_store.py             #   Session persistence
│   ├── logger.py                    #   [AIMV]-prefixed logging
│   └── requirements.txt
├── mcp_server/                      # FastAPI REST server
│   ├── aimv_server.py               #   Entry point + endpoints
│   ├── models.py                    #   Pydantic request/response models
│   ├── prompt_builder.py            #   Prompt construction
│   ├── suggestion_parser.py         #   LLM output → AnalyzeResponse
│   ├── cache.py                     #   Diagnostic fingerprint cache
│   ├── middleware.py                #   API key auth + error handler
│   ├── llm/                         #   LLM backends
│   │   ├── base.py                  #     AbstractLLMBackend
│   │   ├── openai_backend.py        #     OpenAI Chat Completions
│   │   ├── anthropic_backend.py     #     Anthropic Messages
│   │   └── mock_backend.py          #     Pattern-based offline testing
│   ├── templates/                   #   Prompt templates
│   │   ├── cost_reject_prompt.txt
│   │   ├── align_prompt.txt
│   │   ├── loop_transform_prompt.txt
│   │   ├── slp_prompt.txt
│   │   └── unroll_prompt.txt
│   └── requirements.txt
├── ci/                              # CI integration
│   ├── aimv_detect_changes.py
│   ├── aimv_run_batch.py
│   ├── aimv_report.py
│   └── aimv_gate.py
├── benchmarks/                      # C benchmark files
│   ├── dep_fail_alias.c             #   Alias analysis failure
│   ├── dep_fail_stride.c            #   Stride failure
│   ├── cost_reject.c                #   Cost model rejection
│   ├── align_unknown.c              #   Alignment unknown
│   ├── multi_fail.c                 #   Multi-dimension failure
│   └── tsvc_aimv_suite.c            #   25-function TSVC-style suite
├── test/                            # Python test suite (330+ cases)
│   ├── test_aimv_driver.py
│   ├── test_source_manager.py
│   ├── test_iteration_engine.py
│   ├── test_mcp_server.py
│   ├── test_prompt_builder.py
│   ├── test_e2e_loop.py
│   └── ... (28 test files)
└── config/                          # YAML config templates
    └── aimv_config.yaml
```

LLVM-side code (in the monorepo):

```
llvm/
├── include/llvm/Analysis/
│   └── AIMVDiagnostic.h             #   emitAIMVDiagnostic() + AIMVCostSnapshot
├── lib/Analysis/
│   ├── AIMVDiagnostic.cpp           #   emitAIMVDiagnostic() implementation
│   └── LoopAccessAnalysis.cpp       #   UnsafeDep insertion point
├── lib/Transforms/AIMV/
│   ├── AIMVFeedbackPass.cpp         #   !aimv.diag → NDJSON
│   ├── AIMVDiagnosticParser.cpp     #   Metadata parser
│   └── CMakeLists.txt
├── lib/Transforms/Vectorize/
│   ├── LoopVectorize.cpp            #   4 insertion points (LV)
│   └── SLPVectorizer.cpp            #   4 insertion points (SLP)
├── lib/Transforms/Scalar/
│   └── LoopUnrollPass.cpp           #   3 insertion points (Unroll)
├── include/llvm/Transforms/AIMV/
│   └── AIMVFeedback.h              #   AIMVFeedbackPass + AIMVDiagnostic
└── test/Transforms/AIMV/            #   19 llvm-lit tests
```

## Running Tests

```bash
# Python tests (330+ cases)
cd aimv && python3 -m pytest test/ -q

# LLVM lit tests (19 cases, requires built opt)
build/bin/llvm-lit -v llvm/test/Transforms/AIMV/

# E2E with installed tools + mock backend
AIMV_LLM_BACKEND=mock aimv-server &
aimv-driver --from-json aimv.json --source src.c \
    --mcp-url http://localhost:8080 --max-rounds 2

# E2E from source tree
cd aimv
AIMV_LLM_BACKEND=mock python3 -m uvicorn mcp_server.aimv_server:app --host 127.0.0.1 --port 8080 &
python3 -m driver.aimv_driver --from-json aimv.json --source src.c \
    --mcp-url http://localhost:8080 --max-rounds 2
```

## Design Decisions

- **Source-level modification**: LLM suggests C source changes (not IR), developer-reviewable
- **Three-pass diagnostics**: LV + SLP + Unroll share the same `!aimv.diag` / NDJSON channel
- **One change per iteration**: minimizes risk, easy to verify
- **Atomic shadow file protocol**: cp → patch shadow → atomic replace, with backup for rollback
- **Per-function rollback**: on failure, only the current function's patches are reversed
- **NDJSON output**: one JSON line per function, parsed by `_load_aimv_json()` merging all
- **LLVM baseline: 21**: all API calls verified against this version
