# AIMV — AI Multi-Level Vectorization

AI-driven compiler optimization feedback system. Automatically identifies vectorization
failures in C/C++ loops, queries an LLM for source-level fixes, applies patches, and
verifies correctness — all in a closed iteration loop.

```
compile → diagnose failures → AI analysis → source patch → recompile + test → repeat
```

## Architecture

```
┌──────────────┐     JSON      ┌─────────────┐     HTTP      ┌──────────────┐
│  LLVM Pass   │──────────────▶│  Driver      │─────────────▶│  MCP Server  │
│  !aimv.diag  │               │  (Python)    │              │  (FastAPI)   │
│  → JSON      │               │  orchestrate │              │  → LLM       │
└──────────────┘               └─────────────┘              └──────────────┘
                                                              │
                                              ┌───────────────┼───────────────┐
                                              │               │               │
                                              ▼               ▼               ▼
                                          OpenAI          DeepSeek        Anthropic
                                         (GPT-4o)        (V4-Pro)        (Claude)
```

Three subsystems:

| Component | Language | Role |
|-----------|----------|------|
| **LLVM Pass** (`AIMVFeedbackPass`) | C++ | Collects structured diagnostics from LoopVectorize into `!aimv.diag` Named Metadata, serializes to JSON |
| **Driver** (`aimv-driver`) | Python | Orchestrates compile→diagnose→MCP→patch→recompile iteration loop |
| **MCP Server** (`aimv-server`) | Python/FastAPI | Receives diagnostic JSON, constructs LLM prompt, returns structured `AnalyzeResponse` with unified diffs |

## Quick Start

### Prerequisites

- LLVM 21+ (build from source with `-DLLVM_ENABLE_PROJECTS="clang"`)
- Python 3.10+

### 1. Build LLVM with AIMV Pass

```bash
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_ENABLE_PROJECTS="clang" \
  -DLLVM_TARGETS_TO_BUILD="ARM;AArch64"
ninja -C build clang opt
```

### 2. Install Python dependencies

```bash
pip install -e aimv/driver
pip install -e aimv/mcp_server
```

### 3. Start MCP Server

Choose your LLM backend:

```bash
# DeepSeek (已验证通过)
AIMV_LLM_BACKEND=openai \
  AIMV_LLM_BASE_URL=https://api.deepseek.com/v1 \
  OPENAI_API_KEY=sk-your-deepseek-key \
  AIMV_LLM_MODEL=deepseek-v4-pro \
  uvicorn aimv.mcp_server.aimv_server:app --host 0.0.0.0 --port 8080

# OpenAI
AIMV_LLM_BACKEND=openai \
  OPENAI_API_KEY=sk-... \
  uvicorn aimv.mcp_server.aimv_server:app --host 0.0.0.0 --port 8080

# Anthropic
AIMV_LLM_BACKEND=anthropic \
  ANTHROPIC_API_KEY=sk-ant-... \
  uvicorn aimv.mcp_server.aimv_server:app --host 0.0.0.0 --port 8080

# Local OpenAI-compatible server (vLLM / Ollama / etc.)
AIMV_LLM_BACKEND=openai \
  AIMV_LLM_BASE_URL=http://localhost:8000/v1 \
  OPENAI_API_KEY=not-needed \
  uvicorn aimv.mcp_server.aimv_server:app --host 0.0.0.0 --port 8080

# Mock mode (no API key needed, returns empty suggestions)
AIMV_LLM_BACKEND=mock \
  uvicorn aimv.mcp_server.aimv_server:app --host 0.0.0.0 --port 8080
```

### 4. Run analysis

```bash
# Pass mode (full diagnostics: cost model + dependency analysis)
aimv-driver --function=process_task --mcp-url=http://localhost:8080 \
  aimv/benchmarks/dep_fail_alias.c

# YAML mode (zero LLVM modification, fewer diagnostics)
aimv-driver --function=process_task --mode=yaml --mcp-url=http://localhost:8080 \
  aimv/benchmarks/dep_fail_alias.c

# Dry run (diagnostics only, no MCP call)
aimv-driver --function=process_task --dry-run \
  aimv/benchmarks/dep_fail_alias.c
```

## LLM Backend Configuration

### Environment Variables

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `AIMV_LLM_BACKEND` | Yes | `mock` | `openai` / `anthropic` / `mock` |
| `AIMV_LLM_MODEL` | No | `gpt-4o` | Model name |
| `AIMV_LLM_BASE_URL` | No | SDK default | Custom API endpoint (proxy / local / compatible service) |
| `OPENAI_API_KEY` | OpenAI only | — | OpenAI/DeepSeek API key |
| `ANTHROPIC_API_KEY` | Anthropic only | — | Anthropic API key |
| `DEEPSEEK_API_KEY` | DeepSeek only | — | DeepSeek API key (convenience alias) |
| `AIMV_API_KEY` | No | — | MCP Server auth key (set to enable Bearer auth on all endpoints) |
| `AIMV_CACHE_TTL` | No | `86400` | Diagnostic cache TTL in seconds |

### Supported LLM Providers

| Provider | Backend | Protocol | Verified |
|----------|---------|----------|----------|
| **DeepSeek** | `openai` | OpenAI-compatible | ✅ Live tested |
| **OpenAI** | `openai` | Native | SDK verified |
| **Anthropic** | `anthropic` | Anthropic Messages | SDK verified |
| **vLLM** | `openai` | OpenAI-compatible | Compatible |
| **Ollama** | `openai` | OpenAI-compatible | Compatible |
| **阿里百炼** | `openai` | OpenAI-compatible | Compatible |
| Any OpenAI-compatible | `openai` | OpenAI-compatible | Compatible |

### Protocol

The MCP Server uses the standard **OpenAI Chat Completions API** (`POST /v1/chat/completions`)
for `openai` backend, and the **Anthropic Messages API** (`POST /v1/messages`) for `anthropic`.
Any service compatible with either protocol can be used by setting `AIMV_LLM_BASE_URL`.

## Usage

### Driver CLI

```
aimv-driver [OPTIONS] <source_file>

OPTIONS:
  --function FUNC         Target function name (required)
  --aimv-level LEVEL      Modification aggressiveness:
                            conservative | moderate | aggressive (default: moderate)
  --max-rounds N          Max iteration rounds (default: 5)
  --mcp-url URL           MCP server URL (default: http://localhost:8080)
  --output-dir DIR        Output directory (default: ./aimv-output)
  --mode MODE             Diagnostic source: pass (AIMVFeedbackPass) | yaml (opt-records)
  --dry-run               Collect diagnostics only, skip MCP + source modification
  --test-cmd CMD          Test command for verification
  --measure-perf          Enable performance measurement
  --resume SESSION_ID     Resume from saved session
  --list-sessions         List all saved sessions
  --verbose               Verbose output
```

### LLVM Pass (standalone with opt)

```bash
# Compile C to IR, then run AIMV pass
clang -O2 -S -emit-llvm -g src.c -o src.ll
opt -passes="loop-vectorize,aimv-feedback" \
  -pass-remarks-output=opt.yaml \
  -pass-remarks-missed=loop-vectorize \
  -aimv-output=aimv.json \
  -S src.ll -o /dev/null
```

### Clang Integrated

```bash
clang -O2 \
  -fsave-optimization-record=opt.yaml \
  -aimv-output=aimv.json \
  -Rpass-missed=loop-vectorize \
  -g src.c -o task
```

### MCP Server API

```
POST /api/v1/analyze-vectorization   — Analyze vectorization failure
GET  /api/v1/health                  — Health check
GET  /api/v1/cache/stats             — Cache statistics
POST /api/v1/feedback                — Record suggestion result for prompt optimization
```

Authentication (optional): `Authorization: Bearer <AIMV_API_KEY>`.

## Configuration

Default config at `aimv/config/aimv_config.yaml`:

```yaml
aimv:
  max_rounds: 5
  aimv_level: moderate
  mcp:
    url: http://localhost:8080
    timeout_seconds: 60
  build:
    cc: clang
    cflags: -O2 -fsave-optimization-record -g
  verify:
    test_cmd: make test
    measure_perf: false
  output:
    dir: ./aimv-output
```

## Modification Levels

| Level | Allowed Changes |
|-------|----------------|
| **conservative** | `restrict`, `const`, `alignas`, `#pragma clang loop vectorize(enable)`, `__builtin_assume` |
| **moderate** | Above + loop fission/distribution, interchange, scalar promotion, reduction adjustment |
| **aggressive** | Above + AoS→SoA conversion, algorithm substitution (requires developer confirmation) |

## Iteration Flow

```
Round N:
  1. COMPILE   → clang -O2 -fsave-optimization-record -aimv-output=aimv.json
  2. CHECK     → parse JSON: any missed loops?
  3. MCP       → POST /api/v1/analyze-vectorization
  4. PATCH     → apply unified diff, save backup
  5. VERIFY    → recompile, run tests
  6. MEASURE   → (optional) perf stat comparison
  ──▶ SUCCESS (all loops vectorized)
  ──▶ CONTINUE (still missed, try next round)
  ──▶ ROLLBACK (test failure / perf regression)
  ──▶ GIVE_UP (max rounds / no suggestion available)
```

## Running Tests

```bash
# Python unit tests (158+ cases)
pytest aimv/test/ -v

# LLVM Lit tests (requires built opt + FileCheck)
build/bin/opt -passes="loop-vectorize,aimv-feedback" \
  -pass-remarks-output=/tmp/t.yaml -pass-remarks-missed=loop-vectorize \
  -aimv-output=/tmp/aimv.json -S \
  llvm/test/Transforms/AIMV/aimv_diag_metadata.ll -o /dev/null

# Benchmark compilation tests (requires clang)
pytest aimv/test/test_benchmarks.py -v
```

## Project Structure

```
├── README.md
├── CLAUDE.md                         # Workspace guidance
├── TASK.md                           # Atomic task list (49 tasks)
├── aimv_design_doc/                  # Design documents
│   ├── SPEC.md                       #   Requirements specification
│   ├── PLAN.md                       #   Technical plan + data models
│   ├── LLVM_DESIGN.md                #   LLVM Pass design
│   ├── DRIVER_DESIGN.md              #   Driver design
│   ├── MCP_DESIGN.md                 #   MCP Server design
│   └── CI_DESIGN.md                  #   CI/CD design
├── aimv/                             # Implementation
│   ├── driver/                       #   Python driver
│   ├── mcp_server/                   #   MCP REST API server
│   │   └── llm/                      #     LLM backends (OpenAI, Anthropic)
│   ├── ci/                           #   CI integration tools
│   ├── test/                         #   Test suite
│   ├── benchmarks/                   #   C benchmark files
│   └── config/                       #   YAML config templates
├── llvm/                             # LLVM source (monorepo)
│   ├── lib/Transforms/AIMV/          #   AIMVFeedbackPass
│   ├── lib/Transforms/Vectorize/     #   AIMVDiagnostic.h + LoopVectorize patches
│   └── include/llvm/Transforms/AIMV/ #   Public headers
└── .github/workflows/                # GitHub Actions CI
```

## CI Integration

GitHub Actions workflow at `.github/workflows/aimv-analysis.yml`:

- Triggered on PRs touching `*.c`/`*.cpp`/`*.h`
- Detects changed functions via `aimv-detect-changes`
- Runs batch AIMV analysis with `aimv-run-batch`
- Posts Markdown report as PR comment

CI tools:

| Tool | Purpose |
|------|---------|
| `aimv-detect-changes` | Git diff → changed function list |
| `aimv-run-batch` | Parallel batch AIMV analysis |
| `aimv-report` | Session JSON → Markdown report |
| `aimv-gate` | Gate decision (report/regression/enforce) |

## Design Decisions

- **Source-level modification**: AI suggests C/C++ source changes (not IR patches) — developer reviewable
- **One change per iteration**: minimizes risk, easy to verify
- **OpenAI-compatible protocol**: universal, works with any LLM provider or local model
- **Multi-provider support**: OpenAI, DeepSeek, Anthropic, any compatible service
- **Remote LLM**: strongest model quality, team-shared analysis history
- **Multi-layer safety**: developer review → compile check → test suite → optional Alive2 verification
- **Independent from EmbeddedJIT**: completely separate system, no shared components
- **LLVM baseline: 21**: all API calls verified against this version
