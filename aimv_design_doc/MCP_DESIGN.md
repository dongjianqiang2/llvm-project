# AIMV -- MCP 分析服务详细设计方案

**版本**: 1.1
**日期**: 2026-05-17
**关联文档**: SPEC.md v1.4, PLAN.md v1.1

---

## 0. 设计目标

MCP 分析服务是 AIMV 闭环中的"大脑"——接收 LLVM AIMVFeedbackPass 产出的结构化诊断 JSON，调用远程大模型分析向量化失败根因，返回源码级修改建议（unified diff 格式），供 `aimv-driver` 应用和验证。

本文档为 API 层数据模型的权威来源（API Contract），其他文档的模型定义应以本文为准。

---

## 1. 服务架构

### 1.1 整体拓扑

```
┌──────────────┐     HTTPS      ┌────────────────────────────────┐
│  aimv-driver │ ──────────────▶│  MCP Server (FastAPI)          │
│  (客户端)     │ ◀──────────────│  :8080                         │
└──────────────┘                │                                │
                                │  ┌──────────────────────────┐  │
                                │  │  Request Pipeline         │  │
                                │  │  1. 参数校验 (Pydantic)    │  │
                                │  │  2. 缓存查询 (Redis/内存)  │  │
                                │  │  3. Prompt 构建            │  │
                                │  │  4. LLM 调用               │  │
                                │  │  5. 响应解析 + 验证        │  │
                                │  └──────────────────────────┘  │
                                │              │                 │
                                │              ▼                 │
                                │  ┌──────────────────────────┐  │
                                │  │  LLM Backend              │  │
                                │  │  - OpenAI (GPT-4o)         │  │
                                │  │  - Anthropic (Claude)      │  │
                                │  │  - DeepSeek (V4)           │  │
                                │  └──────────────────────────┘  │
                                │              │                 │
                                │              ▼                 │
                                │  ┌──────────────────────────┐  │
                                │  │  Cache Layer              │  │
                                │  │  - 诊断指纹哈希 → 建议缓存 │  │
                                │  │  - TTL 24h                │  │
                                │  └──────────────────────────┘  │
                                └────────────────────────────────┘
```

### 1.2 目录结构

```
aimv/mcp_server/                         # 注意: 下划线（非连字符）
├── aimv_server.py              # FastAPI 入口，路由注册
├── models.py                   # Pydantic 请求/响应模型（权威数据源）
├── prompt_builder.py           # 诊断信息 → LLM prompt
├── templates/
│   ├── system_prompt.txt       # System prompt 模板
│   ├── diagnostic_block.j2     # 单条诊断的 Jinja2 模板
│   └── history_block.j2        # 历史轮次摘要模板
├── suggestion_parser.py        # LLM 响应 → 结构化 Suggestion
├── llm/
│   ├── __init__.py
│   ├── base.py                 # AbstractLLMBackend 接口
│   ├── openai_backend.py       # OpenAI SDK 实现
│   ├── anthropic_backend.py    # Anthropic SDK 实现
│   └── deepseek_backend.py     # DeepSeek SDK 实现
├── cache.py                    # 诊断指纹缓存
├── config.py                   # 配置管理（环境变量 + YAML）
├── middleware.py                # 日志、限流、错误处理中间件
└── requirements.txt
```

---

## 2. API 端点定义

### 2.1 `POST /api/v1/analyze-vectorization`

#### Request

```json
{
  "request_id": "aimv-a1b2c3d4-1715000000",
  "target": {
    "triple": "armv7-unknown-linux-gnueabi",
    "cpu": "cortex-a9",
    "features": ["neon", "vfp4"],
    "vector_width": 128
  },
  "function": {
    "name": "process_task",
    "signature": "void process_task(int *a, int *b, int n)",
    "source_code": "void process_task(int *a, int *b, int n) {\n  for (int i = 0; i < n; i++) {\n    a[i] = b[i+1] + b[i];\n  }\n}",
    "source_file": "../src/task.c",
    "loop_line": 2
  },
  "diagnostics": [
    {
      "pass_name": "LoopVectorize",
      "remark_id": "CantReorderMemOps",
      "remark_text": "loop not vectorized: unsafe dependent memory operations in loop",
      "severity": "missed",
      "function_name": "process_task",
      "loop_location": "../src/task.c:2:3",
      "source_context": "for (int i = 0; i < n; i++) {\n    a[i] = b[i+1] + b[i];\n}",
      "ir_snippet": "...",
      "cost_model": {
        "scalar_cost": 24,
        "vector_cost": 38,
        "vf": 4,
        "interleave_count": 1
      },
      "dependencies": [
        {
          "dep_type": "Backward",
          "source_ptr": "ptr %b + i + 1",
          "sink_ptr": "ptr %a + i",
          "alias_result": "unsafe: prevents vectorization"
        }
      ],
      "memory_info": {
        "num_stores": 1,
        "num_loads": 2,
        "num_pred_stores": 0,
        "max_alignment": 4,
        "stride": "stride=1",
        "memory_check_count": 2,
        "memory_check_cost": 8
      },
      "loop_info": {
        "num_blocks": 3,
        "num_instructions": 18,
        "trip_count": -1,
        "num_branches": 1,
        "num_calls": 0
      }
    }
  ],
  "history": [
    {
      "round": 1,
      "diagnosis_summary": "CantReorderMemOps: unsafe dependent memory operations",
      "suggestion_applied": "added restrict to parameter 'a'",
      "outcome": "compile_passed, vectorization_still_failed"
    }
  ],
  "aimv_level": "conservative"
}
```

#### Response (200)

```json
{
  "request_id": "aimv-a1b2c3d4-1715000000",
  "suggestions": [
    {
      "description": "Add restrict qualifier to pointer parameter `a`",
      "reasoning": "The loop has a RAW dependency: `a[i] = b[i+1] + b[i]`. The compiler cannot prove that `a` and `b` do not overlap. Marking `a` with `restrict` tells the compiler that `a` is the only pointer through which the memory it points to is accessed, breaking the false dependency.",
      "source_file": "../src/task.c",
      "line_start": 1,
      "line_end": 1,
      "original": "void process_task(int *a, int *b, int n) {",
      "modified": "void process_task(int * restrict a, int *b, int n) {",
      "diff": "--- a/../src/task.c\n+++ b/../src/task.c\n@@ -1 +1 @@\n-void process_task(int *a, int *b, int n) {\n+void process_task(int * restrict a, int *b, int n) {",
      "estimated_impact": "high",
      "safety_concern": null
    }
  ],
  "overall_analysis": "The vectorization failure is caused by aliasing uncertainty between pointer parameters `a` and `b`. Adding `restrict` to `a` will resolve this. If `a` and `b` may genuinely alias, consider loop distribution with a temporary buffer instead.",
  "confidence": 0.92,
  "no_action_possible": false
}
```

#### Response (200, no action)

```json
{
  "request_id": "...",
  "suggestions": [],
  "overall_analysis": "The loop contains a `div` instruction inside the loop body which is not supported by NEON vectorization. No source-level fix is available.",
  "confidence": 1.0,
  "no_action_possible": true
}
```

#### Response (422, validation error)

```json
{
  "detail": [
    {
      "loc": ["body", "diagnostics", 0, "pass_name"],
      "msg": "field required",
      "type": "value_error.missing"
    }
  ]
}
```

### 2.2 认证

所有 `/api/v1/` 端点需要在请求头中携带 API Key：

```
Authorization: Bearer <AIMV_API_KEY>
```

FastAPI 中间件在请求进入时校验，不合法请求返回 401。

```python
# [AIMV] mcp_server/middleware.py

from fastapi import Request, HTTPException
from starlette.middleware.base import BaseHTTPMiddleware

class APIKeyMiddleware(BaseHTTPMiddleware):
    async def dispatch(self, request: Request, call_next):
        if request.url.path.startswith("/api/v1/"):
            auth = request.headers.get("Authorization", "")
            if not auth.startswith("Bearer ") or auth.split(" ", 1)[1] != API_KEY:
                raise HTTPException(status_code=401, detail="Invalid API key")
        return await call_next(request)
```

### 2.3 `GET /api/v1/health`

```json
{
  "status": "ok",
  "backend": "openai",
  "model": "gpt-4o",
  "cache_hits": 1247,
  "cache_misses": 389,
  "uptime_seconds": 86400
}
```

### 2.4 `GET /api/v1/cache/stats`

```json
{
  "total_entries": 320,
  "hit_rate": 0.76,
  "total_requests": 1636,
  "cache_hits": 1247,
  "cache_misses": 389,
  "estimated_cost_saved_usd": 42.18
}
```

---

## 3. Pydantic 模型

```python
# [AIMV] mcp_server/models.py
#
# 注意: 此模型为 API 层的权威数据源（API Contract）。
# MCP_DESIGN.md 中的模型定义是最终参考，其他文档应与之保持一致。

from pydantic import BaseModel, Field, field_validator
from typing import Optional, List
from enum import Enum


class RemarkSeverity(str, Enum):
    PASSED = "passed"
    MISSED = "missed"
    ANALYSIS = "analysis"


class AimvLevel(str, Enum):
    CONSERVATIVE = "conservative"
    MODERATE = "moderate"
    AGGRESSIVE = "aggressive"


# --- 诊断子结构 ---

class CostModelDetail(BaseModel):
    """VPlan 代价模型拆解（MVP 不包含 instruction_costs 明细）"""
    scalar_cost: int = Field(..., ge=-1)
    vector_cost: int = Field(..., ge=-1)
    vf: int = Field(..., ge=0)
    interleave_count: int = Field(..., ge=0)


class DependencyInfo(BaseModel):
    """内存依赖分析结果 -- dep_type 直接使用 LLVM Dependence::DepName[Dep.Type]"""
    dep_type: str = Field(..., pattern=r"^(NoDep|Unknown|IndirectUnsafe|Forward|ForwardButPreventsForwarding|Backward|BackwardVectorizable|BackwardVectorizableButPreventsForwarding)$")
    source_ptr: str
    sink_ptr: str
    alias_result: str


class MemoryInfo(BaseModel):
    """内存/对齐信息"""
    num_stores: int = Field(..., ge=0)
    num_loads: int = Field(..., ge=0)
    num_pred_stores: int = Field(0, ge=0)
    max_alignment: int = Field(..., ge=0)       # bytes
    stride: str
    memory_check_count: int = Field(..., ge=0)
    memory_check_cost: int = Field(..., ge=-1)


class LoopInfo(BaseModel):
    """循环结构信息"""
    num_blocks: int
    num_instructions: int
    trip_count: int
    num_branches: int
    num_calls: int


class SingleDiagnostic(BaseModel):
    """单条 opt-info 诊断记录"""
    pass_name: str
    remark_id: str
    remark_text: str
    severity: RemarkSeverity
    function_name: str
    loop_location: str
    source_context: str
    ir_snippet: str
    cost_model: Optional[CostModelDetail] = None
    dependencies: List[DependencyInfo] = []
    memory_info: Optional[MemoryInfo] = None
    loop_info: Optional[LoopInfo] = None


# --- 请求/响应顶层模型 ---

class TargetInfo(BaseModel):
    """目标平台信息"""
    triple: str
    cpu: str
    features: List[str] = []
    vector_width: int = Field(..., gt=0)


class FunctionInfo(BaseModel):
    """被分析函数信息

    source_code 为**当前文件的完整内容**（已包含前面函数所有已通过变更的版本），
    而非原始源码。AI 生成的 diff 基于该版本，行号与当前文件一致。

    版本说明:
      - 多函数文件中，函数 A 的修改验证通过后立即原子替换源文件
      - 处理函数 B 时，source_code 已包含 A 的变更
      - 因此 AI 对 B 生成的 diff 行号基于"含 A 变更"的版本，不会出现偏移
      - Driver 每轮发送前必须读取当前最新文件内容填充此字段
    """
    name: str
    signature: str
    source_code: str
    source_file: str
    loop_line: int = Field(..., gt=0)


class HistoryRecord(BaseModel):
    """历史轮次记录

    传递策略: 发送最近 3 轮，不足 3 轮时发送全部。
    每条记录约 100-200 字节，3 轮合计 < 1 KB，不会显著增加请求体积。
    目的: 让 AI 了解之前的尝试，避免重复失败的策略。
    """
    round: int
    diagnosis_summary: str
    suggestion_applied: str
    outcome: str      # e.g. "compile_passed, vectorization_still_failed"


class AnalyzeRequest(BaseModel):
    """POST /api/v1/analyze-vectorization 请求体"""
    request_id: str
    target: TargetInfo
    function: FunctionInfo
    diagnostics: List[SingleDiagnostic] = Field(..., min_length=1)
    history: List[HistoryRecord] = []     # 最近 3 轮历史，让 AI 避免重复建议
    aimv_level: AimvLevel = AimvLevel.CONSERVATIVE  # 默认保守，对应 SPEC 配置默认值

    @field_validator("diagnostics")
    @classmethod
    def check_diag_count(cls, v):
        if len(v) > 20:
            raise ValueError("too many diagnostics (max 20 per request)")
        return v


class Suggestion(BaseModel):
    """AI 返回的单条修改建议"""
    description: str
    reasoning: str
    source_file: str
    line_start: int
    line_end: int
    original: str
    modified: str
    diff: str
    estimated_impact: str = Field(pattern="^(high|medium|low)$")
    safety_concern: Optional[str] = None


class AnalyzeResponse(BaseModel):
    """POST /api/v1/analyze-vectorization 响应体"""
    request_id: str
    suggestions: List[Suggestion] = []
    overall_analysis: str
    confidence: float = Field(..., ge=0.0, le=1.0)
    no_action_possible: bool = False
```

---

## 4. Prompt 工程

### 4.1 System Prompt 模板

```
You are an expert compiler engineer specializing in automatic vectorization
of C/C++ code for embedded systems. You analyze LLVM optimization remarks
and suggest source-level fixes to enable loop vectorization.

## Your Task
Given:
1. The current version of the C source code of a function containing a loop
   (note: this source may already contain changes from prior iterations for
   other functions in the same file; line numbers match this version exactly)
2. LLVM diagnostic data explaining WHY vectorization failed
3. LLVM IR snippets showing how the compiler sees the loop
4. Cost model details and memory dependency analysis

You must determine a source-level modification to fix the failure.

## Target Platform
- Triple: {target.triple}
- CPU: {target.cpu}
- SIMD: {target.features} ({target.vector_width}-bit vectors)

## Rules
1. DO NOT change program semantics. The fix must be behavior-preserving.
2. Prefer minimal changes: add qualifiers (restrict, const, alignas) before
   rewriting loop structure.
3. Suggest ONE change per iteration. The driver will re-run the compiler and
   send you updated diagnostics if the first fix isn't sufficient.
4. If you cannot determine a safe fix, set `no_action_possible: true`.
5. Provide a valid unified diff in the `diff` field. Line numbers in the diff
   must match the current source_code version (not the original file).
6. Only suggest modifications to .c/.cpp source files, never to headers
   (.h/.hpp). Header file changes (static inline functions, macros) are out
   of scope.
7. For {aimv_level} level, you may:
   - conservative: only add qualifiers and attributes
   - moderate: also suggest loop fission, interchange, scalar promotion
   - aggressive: also suggest data structure changes (AoS->SoA)

## Output Format
Respond with a single JSON object matching this exact schema:
{response_schema}

Do not include any text outside the JSON.
```

### 4.2 诊断信息注入模板（Jinja2）

```jinja2
{# mcp_server/templates/diagnostic_block.j2 #}
## Function Under Analysis

**Name:** `{{ function.name }}`
**Signature:** `{{ function.signature }}`
**File:** `{{ function.source_file }}`, line {{ function.loop_line }}

### Source Code (current version with prior changes applied)

```c
{{ function.source_code }}
```

---

{% for diag in diagnostics %}
### Vectorization Failure #{{ loop.index }}

**Pass:** {{ diag.pass_name }}
**Remark ID:** {{ diag.remark_id }}
**Message:** {{ diag.remark_text }}
**Severity:** {{ diag.severity }}

#### LLVM IR (optimized, surrounding the loop)

```llvm
{{ diag.ir_snippet }}
```

#### Cost Model Analysis

{% if diag.cost_model %}
| Metric | Value |
|--------|-------|
| Scalar cost | {{ diag.cost_model.scalar_cost }} |
| Vector cost (VF={{ diag.cost_model.vf }}) | {{ diag.cost_model.vector_cost }} |
| Interleave count | {{ diag.cost_model.interleave_count }} |
| Cost ratio | {{ "%.1f"|format(diag.cost_model.vector_cost / max(diag.cost_model.scalar_cost, 1)) }}x |

{% if diag.cost_model.scalar_cost < diag.cost_model.vector_cost %}
**Key insight:** Vector cost ({{ diag.cost_model.vector_cost }}) > scalar cost ({{ diag.cost_model.scalar_cost }}). The cost model estimates vectorization is NOT profitable.
{% endif %}
{% else %}
Cost model data not available (legality stage rejection).
{% endif %}

#### Memory Dependencies

{% if diag.dependencies %}
| # | Type | Source | Sink | Alias Result |
|---|------|--------|------|-------------|
{% for dep in diag.dependencies %}
| {{ loop.index }} | `{{ dep.dep_type }}` | `{{ dep.source_ptr }}` | `{{ dep.sink_ptr }}` | {{ dep.alias_result }} |
{% endfor %}
{% else %}
No dependency information available.
{% endif %}

#### Memory Access Pattern

{% if diag.memory_info %}
| Attribute | Value |
|-----------|-------|
| Stores / Loads | {{ diag.memory_info.num_stores }} / {{ diag.memory_info.num_loads }} |
| Max alignment | {{ diag.memory_info.max_alignment }} bytes |
| Stride | {{ diag.memory_info.stride }} |
| Memory checks needed | {{ diag.memory_info.memory_check_count }} (cost: {{ diag.memory_info.memory_check_cost }}) |
{% else %}
No memory access pattern information available.
{% endif %}

#### Loop Structure

{% if diag.loop_info %}
| Blocks | Instructions | Trip count | Branches | Calls |
|--------|-------------|------------|----------|-------|
| {{ diag.loop_info.num_blocks }} | {{ diag.loop_info.num_instructions }} | {{ diag.loop_info.trip_count if diag.loop_info.trip_count >= 0 else "unknown" }} | {{ diag.loop_info.num_branches }} | {{ diag.loop_info.num_calls }} |
{% else %}
No loop structure information available.
{% endif %}

---

{% endfor %}

{% if history %}
### Previous Attempts (last {{ history | length }} round(s))

{% for item in history %}
Round {{ item.round }}:
- Diagnosis: {{ item.diagnosis_summary }}
- Applied: {{ item.suggestion_applied }}
- Outcome: {{ item.outcome }}
{% endfor %}

**Important:** Do NOT repeat any of the above suggestions. They have been tried and failed or did not fully resolve the issue.
{% endif %}
```

### 4.3 诊断指纹（用于缓存 key）

```python
# [AIMV] mcp_server/cache.py

import hashlib
import json

def compute_diagnostic_fingerprint(request: AnalyzeRequest) -> str:
    """为诊断请求计算稳定指纹，用于缓存匹配。

    取诊断的语义信息（不取 request_id、时间戳等），
    确保相同问题命中缓存。

    注意:
    1. 包含 source_code 的 hash 是为了防止迭代场景下的错误缓存命中:
       第 N 轮的源码已变更，即使 remark_text 相同，问题也可能不同。
    2. 包含 history 的 hash 是为了区分相同诊断但不同历史上下文的场景:
       相同诊断 + 不同历史 = 不同缓存 key（AI 可能给出不同建议）。
    """
    canonical = {
        "target_triple": request.target.triple,
        "target_cpu": request.target.cpu,
        "vector_width": request.target.vector_width,
        "level": request.aimv_level.value,
        "function_name": request.function.name,
        # 包含源码 hash: 迭代中源码变更后不会命中旧缓存
        "source_code_sha256": hashlib.sha256(
            request.function.source_code.encode()
        ).hexdigest(),
        "diagnostics": [
            {
                "pass": diag.pass_name,
                "remark_id": diag.remark_id,
                # 取 remark 文本的前 120 字符作为辅助特征
                "remark_prefix": diag.remark_text[:120],
            }
            for diag in request.diagnostics
        ],
        # 包含历史上下文: 相同诊断 + 不同历史 = 不同缓存 key
        "history": [
            {
                "round": h.round,
                "diagnosis_summary": h.diagnosis_summary,
                "suggestion_applied": h.suggestion_applied,
                "outcome": h.outcome,
            }
            for h in request.history
        ],
    }
    payload = json.dumps(canonical, sort_keys=True).encode()
    return hashlib.sha256(payload).hexdigest()[:16]

# 双模式缓存策略:
#
# 严格模式 (strict): fingerprint 含 source_code_sha256 + history
#   - 适用场景: 同一函数的多轮迭代去重
#     （源码未变 + 相同诊断 + 相同历史 → 复用建议）
#   - 缺点: 迭代中源码每轮变更，缓存命中率低
#
# 宽松模式 (relaxed): fingerprint 不含 source_code/history (仅按 target+diagnostics)
#   - 适用场景: 跨函数相似诊断模式复用（不同函数遇相同向量化失败模式）
#   - 配置: cache_relaxed_mode=true（默认启用）
#
# 实现: 两种模式共用同一缓存存储，key 前缀区分:
#   strict:{sha256}  严格匹配（同函数+同源码+同历史）
#   relaxed:{sha256} 宽松匹配（同失败模式）
```

---

## 5. LLM Backend 适配

### 5.1 抽象接口

```python
# [AIMV] mcp_server/llm/base.py

from abc import ABC, abstractmethod
from models import AnalyzeRequest, AnalyzeResponse


class AbstractLLMBackend(ABC):
    """LLM 后端统一接口"""

    @abstractmethod
    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        """发送诊断请求，返回结构化分析结果"""
        ...

    @abstractmethod
    def health_check(self) -> bool:
        """检查后端连通性"""
        ...
```

### 5.2 OpenAI Backend

```python
# [AIMV] mcp_server/llm/openai_backend.py

from openai import OpenAI

class OpenAIBackend(AbstractLLMBackend):
    def __init__(self, config: dict):
        self.client = OpenAI(api_key=config["api_key"])
        self.model = config.get("model", "gpt-4o")
        self.temperature = config.get("temperature", 0.1)  # 低温度保证输出稳定
        self.max_tokens = config.get("max_tokens", 4096)

    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        system_prompt = build_system_prompt(request)
        user_prompt = build_user_prompt(request)

        response = self.client.chat.completions.create(
            model=self.model,
            temperature=self.temperature,
            max_tokens=self.max_tokens,
            response_format={"type": "json_object"},  # 强制 JSON 输出
            messages=[
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
        )

        raw_json = response.choices[0].message.content
        return parse_structured_response(raw_json, request.request_id)
```

### 5.3 Anthropic Backend

```python
# [AIMV] mcp_server/llm/anthropic_backend.py

from anthropic import Anthropic

class AnthropicBackend(AbstractLLMBackend):
    def __init__(self, config: dict):
        self.client = Anthropic(api_key=config["api_key"])
        self.model = config.get("model", "claude-opus-4-7")
        self.max_tokens = config.get("max_tokens", 4096)

    def analyze(self, request: AnalyzeRequest) -> AnalyzeResponse:
        system_prompt = build_system_prompt(request)
        user_prompt = build_user_prompt(request)

        # 将 response_schema 注入 system prompt 作为输出约束
        system_prompt += f"\n\nRespond ONLY with valid JSON:\n{RESPONSE_SCHEMA_JSON}"

        message = self.client.messages.create(
            model=self.model,
            max_tokens=self.max_tokens,
            system=system_prompt,
            messages=[
                {"role": "user", "content": user_prompt},
            ],
        )

        raw_json = message.content[0].text
        return parse_structured_response(raw_json, request.request_id)
```

### 5.4 DeepSeek Backend

```python
# [AIMV] mcp_server/llm/deepseek_backend.py

from openai import OpenAI  # DeepSeek API 兼容 OpenAI SDK

class DeepSeekBackend(OpenAIBackend):
    def __init__(self, config: dict):
        # 注意: 不调用 super().__init__() 以避免创建无用的默认 OpenAI client。
        # 直接在构造中设置 DeepSeek 专用 client 和参数。
        self.client = OpenAI(
            api_key=config["api_key"],
            base_url="https://api.deepseek.com/v1"
        )
        self.model = config.get("model", "deepseek-v4-pro")
        self.temperature = config.get("temperature", 0.1)
        self.max_tokens = config.get("max_tokens", 4096)
```

---

## 6. 建议解析器

### 6.1 核心逻辑

```python
# [AIMV] mcp_server/suggestion_parser.py

import json
import re
from typing import Optional
from models import AnalyzeResponse, Suggestion


# [AIMV] JSON 响应 schema（注入 system prompt 用于约束 LLM 输出）
RESPONSE_SCHEMA_JSON = """
{
  "suggestions": [
    {
      "description": "<one-line summary>",
      "reasoning": "<detailed analysis of the failure and why this fix works>",
      "source_file": "<path>",
      "line_start": <int>,
      "line_end": <int>,
      "original": "<original source text>",
      "modified": "<modified source text>",
      "diff": "<unified diff>",
      "estimated_impact": "high|medium|low",
      "safety_concern": "<null or string describing potential risk>"
    }
  ],
  "overall_analysis": "<summary paragraph>",
  "confidence": <float 0.0-1.0>,
  "no_action_possible": false
}
"""


class SuggestionParseError(Exception):
    """LLM 响应无法解析为有效建议"""


# LLM 输出格式稳定性保障策略:
#
# 1. 首选: OpenAI response_format={"type": "json_object"} + json_schema (GPT-4o+)
#    - 通过 API 参数直接约束输出为合法 JSON，消除 markdown 包裹和尾部逗号
# 2. 次选: Anthropic/DeepSeek -- System Prompt 中注入完整 JSON Schema
#    - 在 system prompt 末尾追加 "Respond ONLY with valid JSON matching the schema above."
# 3. 兜底: 宽松解析器（见 _extract_json()）处理:
#    - ```json ... ``` 代码块包裹
#    - 尾部逗号（JSON5 容忍）
#    - 注释行（// 或 /* */）
#    - LLM 输出中的道歉/解释前缀
# 4. 重试: parse 失败时，将错误信息注入下一轮 prompt 要求 LLM 修正格式
#    （最多追加 1 次格式修正请求，仍失败则返回 no_action_possible=true）


def parse_structured_response(raw_text: str, request_id: str) -> AnalyzeResponse:
    """解析 LLM 返回的原始文本为 AnalyzeResponse。

    处理常见的 LLM 输出格式问题：
    - JSON 包裹在 ```json ... ``` 代码块中
    - 尾部逗号
    - 转义问题
    """

    # 1. 提取 JSON
    json_text = _extract_json(raw_text)

    # 2. 解析
    try:
        data = json.loads(json_text)
    except json.JSONDecodeError as e:
        raise SuggestionParseError(f"JSON parse failed: {e}")

    # 3. 构建响应（通过 Pydantic model_validate 确保完整校验）
    data["request_id"] = request_id  # 确保 request_id 一致
    try:
        response = AnalyzeResponse.model_validate(data)
    except Exception as e:
        raise SuggestionParseError(f"Response validation failed: {e}")

    return response


def _extract_json(text: str) -> str:
    """从 LLM 输出中提取 JSON，处理 ```json 包裹和噪音。"""
    text = text.strip()

    # 移除 markdown 代码块
    m = re.search(r"```(?:json)?\s*\n?(.*?)\n?```", text, re.DOTALL)
    if m:
        return m.group(1)

    # 找到第一个 { 和最后一个 }
    start = text.find("{")
    end = text.rfind("}")
    if start != -1 and end != -1:
        return text[start:end + 1]

    return text
```

---

## 7. 缓存层

### 7.1 内存 + Redis 双层缓存

```python
# [AIMV] mcp_server/cache.py

import time
import threading
from typing import Optional
from models import AnalyzeResponse


class DiagnosticCache:
    """诊断模式缓存。相同诊断指纹 -> 缓存的 AI 建议。

    双层实现：本地内存（L1，毫秒级）+ Redis（L2，毫秒级，跨进程共享）。

    指纹策略（见 compute_diagnostic_fingerprint）:
    - 严格模式: 含 source_code hash + history，同函数同历史才命中
    - 宽松模式: 仅 target + diagnostics，跨函数相似模式可复用
    """

    def __init__(self, redis_client=None, ttl_seconds: int = 86400):
        self._ttl = ttl_seconds
        self._redis = redis_client
        self._local: dict = {}          # fingerprint -> (expiry, AnalyzeResponse)
        self._lock = threading.Lock()
        self._total_requests = 0
        self._cache_hits = 0
        self._cache_misses = 0

    def get(self, fingerprint: str) -> Optional[AnalyzeResponse]:
        # L1: 本地内存
        with self._lock:
            entry = self._local.get(fingerprint)
            if entry and entry[0] > time.time():
                return entry[1]

        # L2: Redis
        if self._redis:
            raw = self._redis.get(f"aimv:cache:{fingerprint}")
            if raw:
                data = json.loads(raw)
                resp = AnalyzeResponse(**data)
                # 回填 L1
                with self._lock:
                    self._local[fingerprint] = (time.time() + self._ttl, resp)
                return resp

        return None

    def set(self, fingerprint: str, response: AnalyzeResponse):
        expiry = time.time() + self._ttl

        # L1
        with self._lock:
            self._local[fingerprint] = (expiry, response)
            # 本地内存上限 10000 条
            if len(self._local) > 10000:
                oldest = min(self._local, key=lambda k: self._local[k][0])
                del self._local[oldest]

        # L2
        if self._redis:
            self._redis.setex(
                f"aimv:cache:{fingerprint}",
                self._ttl,
                response.model_dump_json()
            )

    def get_stats(self) -> dict:
        return {
            "local_entries": len(self._local),
            "total_requests": self._total_requests,
            "cache_hits": self._cache_hits,
            "cache_misses": self._cache_misses,
            "hit_rate": (self._cache_hits / max(self._total_requests, 1)),
            "estimated_cost_saved_usd": self._cache_hits * 0.01,  # 粗估，实际按 LLM API 单价算
        }
```

---

## 8. 错误处理与重试

### 8.1 中间件

```python
# [AIMV] mcp_server/middleware.py

import time
import logging
from fastapi import Request, HTTPException
from fastapi.responses import JSONResponse

logger = logging.getLogger("aimv.mcp")

# 可重试的错误类型
RETRIABLE_LLM_ERRORS = (
    "rate_limit_exceeded",
    "server_error",
    "timeout",
    "overloaded",
)


class AIMVErrorHandler:
    """统一错误处理：LLM 错误重试、超时回退、限流保护"""

    def __init__(self, max_retries: int = 2, retry_delay: float = 2.0):
        self.max_retries = max_retries
        self.retry_delay = retry_delay

    async def handle_llm_call(self, backend, request):
        import asyncio
        last_error = None

        for attempt in range(self.max_retries + 1):
            try:
                return backend.analyze(request)
            except Exception as e:
                last_error = e
                error_type = _classify_error(e)

                if error_type not in RETRIABLE_LLM_ERRORS:
                    break  # 不可重试，直接失败

                if attempt < self.max_retries:
                    delay = self.retry_delay * (2 ** attempt)  # 指数退避
                    logger.warning(f"LLM error (retry {attempt + 1}/{self.max_retries}): {e}")
                    await asyncio.sleep(delay)

        # 所有重试都失败 -> 返回空建议而非报错
        logger.error(f"LLM call failed after {self.max_retries + 1} attempts: {last_error}")
        return AnalyzeResponse(
            request_id=request.request_id,
            suggestions=[],
            overall_analysis=f"MCP service temporarily unavailable: {str(last_error)[:200]}",
            confidence=0.0,
            no_action_possible=True,
        )
```

---

## 9. Prompt 反馈优化环（消除 LLM 幻觉）

Driver 侧每轮迭代的验证结果（编译成功/失败、测试通过/失败、Alive2 验证结果）
作为 feedback 回传给 MCP Server，用于优化后续 prompt：

```
AIMVFeedbackPass -> MCP -> LLM -> suggestion
   |
   |  driver 验证:
   |    compile+test+Alive2 pass -> 记录为 positive example
   |    compile fail / test fail / Alive2 reject -> 记录为 negative example
   |
   +--> POST /api/v1/feedback  { request_id, result, failure_reason }
                |
                v
         prompt_builder 维护 per-diagnostic-pattern 的:
           - 成功策略列表（优先推荐）
           - 失败策略列表（prompt 中明确排除）
           - 在 history 注入时追加: "The following approaches have been tried
             and FAILED on similar diagnostic patterns. Do NOT suggest them."
```

**实现**: `POST /api/v1/feedback` 端点，轻量级，只记录 pattern->strategy->result 三元组。
不阻塞主流程，异步写入。MVP 阶段可用 JSON 文件替代数据库。

---

## 10. 配置管理

```yaml
# aimv/mcp_server/config.yaml

server:
  host: "0.0.0.0"
  port: 8080
  workers: 4

llm:
  backend: "deepseek"          # openai | anthropic | deepseek
  model: "deepseek-v4-pro"
  temperature: 0.1
  max_tokens: 4096
  timeout_seconds: 60
  max_retries: 2

cache:
  enabled: true
  ttl_hours: 24
  redis_url: "redis://localhost:6379/0"   # 可选，为空则仅内存缓存

rate_limit:
  requests_per_minute: 30
  max_diagnostics_per_request: 20

logging:
  level: "info"                 # debug | info | warn | error
  format: "json"                # json | text
  output: "stdout"
```

---

## 11. 部署

### 11.1 Docker 部署

```dockerfile
FROM python:3.12-slim

WORKDIR /app
COPY mcp_server/requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY mcp_server/ .

EXPOSE 8080
CMD ["uvicorn", "aimv_server:app", "--host", "0.0.0.0", "--port", "8080", "--workers", "4"]
```

### 11.2 docker-compose（含 Redis 缓存）

```yaml
version: "3.9"
services:
  aimv-mcp:
    build:
      context: .
      dockerfile: mcp_server/Dockerfile
    ports: ["8080:8080"]
    environment:
      AIMV_LLM_BACKEND: "deepseek"
      AIMV_LLM_API_KEY: "${AIMV_LLM_API_KEY}"
      AIMV_CACHE_REDIS_URL: "redis://redis:6379/0"
    depends_on: [redis]

  redis:
    image: redis:7-alpine
    ports: ["6379:6379"]
```

---

## 12. 与 Driver 的交互协议

```
aimv-driver                      MCP Server                    LLM Backend
    |                                |                              |
    |  POST /analyze-vectorization   |                              |
    | ------------------------------>|                              |
    |                                |  构建 prompt                  |
    |                                | ---------------------------->|
    |                                |                              |
    |                                |          <-------------------|
    |                                |  解析 structured output      |
    |                                |                              |
    |  <------------------------------|                              |
    |  AnalyzeResponse (JSON)        |                              |
    |                                |                              |
    |  应用 patch -> 重编译 -> 验证    |                              |
    | ------------------------------>|   (下一轮迭代)               |
    |  POST /analyze-vectorization   |                              |
    |  (含 history 字段)             |                              |
```

每轮迭代的 `history` 字段使用结构化的 `HistoryRecord` 模型，包含 `round`、`diagnosis_summary`、`suggestion_applied`、`outcome` 四个字段。Driver 发送最近 3 轮历史（不足 3 轮时发送全部），让 LLM 避免重复失败的策略。

### 12.1 History 传递策略

- **窗口大小**: 最近 3 轮（约 300-600 字节，不显著增加请求体积）
- **不足 3 轮**: 发送全部可用历史
- **无历史**: `history` 字段为空列表 `[]`
- **目的**: 让 AI 了解之前尝试的 diagnosis -> suggestion -> outcome 链，避免重复建议
- **缓存影响**: history 内容参与缓存指纹计算，相同诊断 + 不同历史 = 不同缓存 key

---

## 13. 关键设计决策记录

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 默认 aimv_level | CONSERVATIVE | 对应 SPEC 配置默认值，全自动场景下保守修改是安全基线 |
| History 类型 | 结构化 HistoryRecord（非 dict） | 类型安全，字段明确，便于 prompt 模板直接引用 |
| History 窗口 | 最近 3 轮 | 平衡上下文完整性与请求体积 |
| source_code 版本 | 当前文件完整内容（非原始） | 多函数文件中前序变更已生效，AI diff 行号必须匹配当前版本 |
| 缓存指纹 | 含 source_code hash + history | 防止迭代中错误命中（源码变更或历史不同时不应复用旧建议） |
| 目录命名 | `mcp_server`（下划线） | Python 模块命名规范，与 PLAN.md 一致 |
| 头文件修改 | 不允许 | MVP 不处理 .h/.hpp 文件修改，system prompt 中明确约束 |
| API 数据模型权威性 | MCP_DESIGN.md 为主 | 本文为 API Contract 的最终参考，其他文档以本文为准 |

---

*文档版本: 1.1*
*创建日期: 2026-04-29*
*最后更新: 2026-05-17*
