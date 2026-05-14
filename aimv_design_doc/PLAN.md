# AIMV 技术方案 v1.0

**版本**: 1.0
**日期**: 2026-04-29
**关联文档**: SPEC.md

---

## 0. 背景

AIMV (AI Multi-Level Vectorization) 通过编译器 opt-info 诊断信息 + 远程 MCP 大模型服务，形成 "编译→失败诊断→AI分析→源码修改→重编译" 闭环迭代，实现函数级代码的精准自动向量化。

与 EmbeddedJIT 完全独立，不使用其任何组件。AIMV 是编译期优化辅助工具，不引入运行时依赖。

---

## 1. 目录结构

```
aimv/                                    # 项目根目录
├── driver/                              # Python Driver 脚本
│   ├── aimv_driver.py                   # 主入口：编排编译-诊断-MCP-patch 流程
│   ├── opt_info_parser.py               # 解析 YAML/JSON opt-records
│   ├── mcp_client.py                    # MCP REST API 客户端
│   ├── source_manager.py               # 源码 patch + 回滚 + 工作目录管理
│   ├── build_orchestrator.py            # 编译/测试 编排（迭代控制）
│   ├── iteration_tracker.py             # 迭代状态追踪与日志
│   └── requirements.txt
│
├── mcp-server/                          # MCP REST API 服务端
│   ├── aimv_server.py                   # FastAPI/Flask 入口
│   ├── models.py                        # Pydantic 请求/响应模型
│   ├── prompt_builder.py                # 诊断信息 → LLM prompt 构造
│   ├── suggestion_parser.py             # LLM 响应 → 结构化 patch 解析
│   ├── cache.py                         # 诊断模式缓存（避免重复请求）
│   └── requirements.txt
│
├── llvm/                                # LLVM 侧组件（直接在 llvm/ 目录下）
│   ├── lib/Transforms/AIMV/
│   │   ├── AIMVFeedbackPass.cpp         # Function Pass 主实现
│   │   ├── AIMVDiagnosticParser.cpp     # !aimv.diag Metadata → RawDiagnostic 解析
│   │   └── CMakeLists.txt
│   │
│   └── include/llvm/Transforms/AIMV/
│       └── AIMVFeedback.h               # Pass 公共头文件
│
├── tools/
│   └── aimv-analyze/
│       ├── aimv-analyze.cpp             # 独立 CLI：批量离线分析
│       └── CMakeLists.txt
│
├── test/
│   ├── lit/                             # LLVM Lit 测试
│   │   ├── AIMV/
│   │   │   ├── feedback_collect.ll      # Pass 诊断收集测试
│   │   │   └── cost_export.ll           # 代价模型导出测试
│   │   └── lit.cfg.py
│   ├── unit/                            # Python 单元测试
│   │   ├── test_opt_info_parser.py
│   │   ├── test_prompt_builder.py
│   │   ├── test_suggestion_parser.py
│   │   └── test_source_patcher.py
│   └── integration/                     # 端到端集成测试
│       ├── test_e2e_loop.py
│       └── test_dep_fail.c              # 已知向量化失败的 benchmark
│
├── benchmarks/                          # 验证用的循环 benchmark
│   ├── dep_fail_alias.c                 # 别名分析失败场景
│   ├── dep_fail_stride.c                # 跨迭代依赖场景
│   ├── cost_reject.c                    # 代价模型拒绝场景
│   └── align_unknown.c                  # 对齐未知场景
│
├── config/
│   └── aimv_config.yaml                 # 默认配置模板
│
├── SPEC.md                              # 需求规格说明书
├── PLAN.md                              # 本文档
└── CMakeLists.txt                       # 顶层构建（可选）
```

---

## 2. 整体架构

### 2.1 端到端流程

```
┌─────────────────────────────────────────────────────────────────────┐
│  aimv-driver (迭代控制器)                                            │
│                                                                     │
│  ┌──────────┐   ┌──────────────┐   ┌──────────────┐   ┌──────────┐ │
│  │  clang   │──▶│ AIMVFeedback │──▶│  MCP Server  │──▶│ 源码     │ │
│  │ -fsave-  │   │ Pass (JSON)  │   │  POST /api   │   │ patch    │ │
│  │ opt-recrd│   │ 收集诊断信息   │   │  /v1/analyze │   │ 生成     │ │
│  └──────────┘   └──────────────┘   └──────────────┘   └──────────┘ │
│       ▲                                                      │     │
│       │         ┌──────────────┐                             │     │
│       └─────────│  重编译+测试  │◀────────────────────────────┘     │
│       (N轮迭代) │  验证        │                                    │
│                 └──────────────┘                                    │
│                                                                     │
│  终止: 向量化成功 / 达到N轮上限 / 收益不增回滚                        │
└─────────────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件交互

```
┌────────────────┐     ┌─────────────────┐     ┌──────────────────┐
│ AIMVFeedback   │────▶│  aimv-driver    │────▶│  MCP Server      │
│ (LLVM Pass)    │     │  (Python 编排)   │     │  (REST API)      │
│                │     │                 │     │                  │
│ · 收集 missed  │     │ · 迭代循环控制   │     │ · prompt 构造    │
│   + passed     │     │ · 源码 patch     │     │ · LLM 调用       │
│   remarks      │     │ · 编译/测试编排  │     │ · 响应解析       │
│ · VPlan 代价   │     │ · 回滚管理       │     │ · 模式缓存       │
│   模型导出     │     │ · 日志/追溯      │     │                  │
│ · IR 上下文    │     │                 │     │                  │
└────────────────┘     └─────────────────┘     └──────────────────┘
        ↑                       │                       │
        │ Pass 模式              │ HTTP POST             │ HTTP
   ┌────┴─────┐          ┌──────┴──────┐         ┌──────┴──────┐
   │aimv.json │          │  source.c   │         │ GPT/Claude  │
   └──────────┘          └─────────────┘         │ API         │
        ↑                                        └─────────────┘
   ┌────┴─────┐
   │ opt.yaml │  ← YAML 模式（零侵入，诊断信息较少）
   └──────────┘    直接解析 -fsave-optimization-record 输出
```

### 2.3 Driver 迭代状态机

```
          ┌──────────────┐
          │  IDLE        │
          └──────┬───────┘
                 │ 开始分析函数 F
                 ▼
          ┌──────────────┐
    ┌────▶│  COMPILING   │  clang -fsave-optimization-record
    │     └──────┬───────┘
    │            │
    │            ▼
    │     ┌──────────────┐
    │     │  ANALYZING   │  解析 opt-records, 构造 JSON 诊断包
    │     └──────┬───────┘
    │            │
    │            ▼
    │     ┌──────────────┐
    │     │  QUERYING    │  POST → MCP Server → 解析 suggestion
    │     └──────┬───────┘
    │            │
    │            ▼
    │     ┌──────────────┐
    │     │  PATCHING    │  应用 diff, 记录回滚点
    │     └──────┬───────┘
    │            │
    │            ▼
    │     ┌──────────────┐
    │     │  VERIFYING   │  重编译 + 运行测试
    │     └──────┬───────┘
    │            │
    │            ├── 向量化成功 ──▶ SUCCESS
    │            ├── 轮次上限   ──▶ GIVE_UP
    │            ├── 收益退化   ──▶ ROLLBACK
    │            └── 继续       ──▶ (round++) ──┘
```

---

## 3. 核心数据模型

### 3.1 诊断信息模型

```python
# [BiSheng] mcp-server/models.py
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

class CostModelDetail(BaseModel):
    """VPlan 代价模型拆解（MVP 不包含 instruction_costs 明细）"""
    scalar_cost: int = Field(..., ge=-1)
    vector_cost: int = Field(..., ge=-1)
    vf: int = Field(..., ge=0)
    interleave_count: int = Field(..., ge=0)

class DependencyInfo(BaseModel):
    """内存依赖分析结果 — dep_type 与 LLVM 21 Dependence::DepType 对应"""
    dep_type: str = Field(..., pattern=r"^(RAW|WAR|IndirectUnsafe|BackwardVectorizable|BackwardVectorizableButPreventsForwarding|ForwardButPreventsForwarding|Unknown)$")
    source_ptr: str
    sink_ptr: str
    alias_result: str

class MemoryInfo(BaseModel):
    """内存/对齐信息"""
    num_stores: int = Field(..., ge=0)
    num_loads: int = Field(..., ge=0)
    num_pred_stores: int = Field(0, ge=0)
    max_alignment: int = Field(..., ge=0)     # bytes
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

class TargetInfo(BaseModel):
    """目标平台信息"""
    triple: str
    cpu: str
    features: List[str] = []
    vector_width: int = Field(..., gt=0)

class FunctionInfo(BaseModel):
    """被分析函数信息"""
    name: str
    signature: str
    source_code: str
    source_file: str
    loop_line: int = Field(..., gt=0)

class AnalyzeRequest(BaseModel):
    """POST /api/v1/analyze-vectorization 请求体"""
    request_id: str
    target: TargetInfo
    function: FunctionInfo
    diagnostics: List[SingleDiagnostic] = Field(..., min_length=1)
    history: List[dict] = []
    aimv_level: AimvLevel = AimvLevel.MODERATE

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

### 3.2 迭代状态模型

```python
# [BiSheng] driver/iteration_tracker.py

from dataclasses import dataclass, field
from enum import Enum
from typing import Optional
import time

class IterationStatus(Enum):
    PENDING = "pending"         # 等待开始
    COMPILING = "compiling"     # 正在编译
    ANALYZING = "analyzing"     # 正在分析 opt-info
    QUERYING = "querying"       # 正在请求 MCP
    PATCHING = "patching"       # 正在应用 patch
    VERIFYING = "verifying"     # 正在验证
    SUCCESS = "success"         # 向量化成功
    FAILED = "failed"           # 本轮失败
    ROLLED_BACK = "rolled_back" # 已回滚

class TerminationReason(Enum):
    VECTORIZED = "vectorized"           # 函数成功向量化
    ROUND_LIMIT = "round_limit"         # 达到最大轮次
    NO_IMPROVEMENT = "no_improvement"   # 收益不增
    NO_SUGGESTION = "no_suggestion"     # AI 无可用建议
    COMPILE_ERROR = "compile_error"     # patch 导致编译失败
    TEST_FAILURE = "test_failure"       # patch 导致测试失败
    INTERRUPTED = "interrupted"         # 用户中断 (Ctrl+C)

@dataclass
class RoundRecord:
    """单轮迭代记录（简化版；详细字段见 DRIVER_DESIGN.md）"""
    round_number: int
    status: IterationStatus = IterationStatus.PENDING
    diagnostics: list = field(default_factory=list)
    request_json: dict = field(default_factory=dict)
    response_json: dict = field(default_factory=dict)
    applied_patch: Optional[str] = None
    compile_success: Optional[bool] = None
    test_success: Optional[bool] = None
    vectorized: Optional[bool] = None   # 向量化是否成功
    perf_delta: Optional[float] = None  # 性能变化（正=改善）
    start_time: float = field(default_factory=time.time)
    end_time: Optional[float] = None

@dataclass
class SessionRecord:
    """完整分析会话记录（简化版；详细字段见 DRIVER_DESIGN.md）"""
    function_name: str
    source_file: str
    aimv_level: str
    max_rounds: int
    target_loop_line: Optional[str] = None  # 目标循环源码位置（首轮自动选定）
    rounds: list = field(default_factory=list)
    termination_reason: Optional[TerminationReason] = None
    final_patch_path: Optional[str] = None
    overall_perf_improvement_pct: Optional[float] = None
    session_id: str = ""
```

### 3.3 源码 Patch 模型

```python
# [BiSheng] driver/source_manager.py

@dataclass
class PatchRecord:
    """一次源码修改的完整记录"""
    source_file: str
    backup_path: str           # 回滚用的备份文件路径
    diff_text: str
    original_hash: str         # sha256 of original file
    applied_at: float

class SourceManager:
    """源码 patch 管理器（详细实现见 DRIVER_DESIGN.md）"""

    def apply_patch(self, source_file: str, diff_text: str) -> PatchRecord:
        """应用 unified diff，创建回滚备份。失败时抛异常并从备份恢复。"""
        ...

    def rollback(self, patch: PatchRecord) -> bool:
        """从备份恢复原始文件"""
        ...

    def rollback_all(self):
        """按反序回滚所有 patch，单条失败不中断后续回滚"""
        ...
```

---

## 4. 接口定义

### 4.1 MCP REST API

```
Base URL: http://<mcp-host>:<port>/api/v1
```

| Method | Path | 说明 |
|--------|------|------|
| POST | `/analyze-vectorization` | 分析向量化失败，返回源码修改建议 |
| GET | `/health` | 健康检查 |
| GET | `/cache/stats` | 诊断模式缓存统计 |

#### POST /analyze-vectorization

**Request**: `AnalyzeRequest`（详见 §3.1）

**Response**: `AnalyzeResponse`（详见 §3.1）

**行为**:
- 接收诊断包，构造 LLM prompt
- 调用后端 LLM（GPT/Claude 等）
- 解析 LLM 输出为结构化 Suggestion
- 相同诊断模式命中缓存则直接返回缓存结果
- 超时 60s，超时返回 `no_action_possible=true`

### 4.2 LLVM Pass 接口

```cpp
// [BiSheng] llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h

namespace llvm {

/// [BiSheng] AIMVFeedbackPass — 收集向量化诊断信息并导出 JSON
///
/// 类型: Function Pass。在 LoopVectorize / SLPVectorize 之后运行，
/// 从 Module 的 !aimv.diag Named Metadata 中读取诊断（通过 F.getParent()），
/// 筛选当前函数的诊断，补充 IR 上下文后序列化为 JSON。
///
/// 输出：追加写入 -aimv-output=<path> 指定的 JSON 文件
class AIMVFeedbackPass : public PassInfoMixin<AIMVFeedbackPass> {
public:
    /// [BiSheng] 配置 JSON 输出路径
    void setOutputPath(const std::string& path);

    /// [BiSheng] 配置只收集指定函数的诊断
    void setTargetFunction(const std::string& funcName);

    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
    static StringRef name() { return "aimv-feedback"; }
};

} // namespace llvm
```

**Pass 工作流程**:

```
输入: Function (LoopVectorize/SLPVectorize 已在当前函数上运行)

1. 通过 F.getParent() 获取 Module，读取 !aimv.diag Named Metadata
   - 无 !aimv.diag → 直接返回

2. 解析 !aimv.diag 为 RawDiagnostic 向量
   - 筛选: 仅保留 FunctionName 匹配当前函数的诊断
   - 为空 → 直接返回

3. 对每条匹配的诊断:
   a. 获取函数级分析: AAResults, ScalarEvolution, LoopInfo
   b. 按 !dbg metadata 映射源码行号（含回退链）
   c. 提取 IR 片段和源码上下文
   d. 推断 severity（按 RemarkID 匹配规则）

4. 收集目标平台信息: Module 的 target triple, TargetIRAnalysis

5. 序列化: 追加写入 --aimv-output 指定的 JSON 文件

输出: JSON 文件（diagnostics 部分），供 aimv-driver 补充 function/target 后发送 MCP
```

### 4.3 Driver CLI 接口

```
aimv-driver [OPTIONS] <source_file> -- <compiler_flags>

OPTIONS:
  --function FUNC       只分析指定函数（默认：所有函数）
  --aimv-level LEVEL    修改激进度 (conservative | moderate | aggressive)
  --max-rounds N        最大迭代轮次（默认 5）
  --mcp-url URL         MCP 服务地址（默认 http://localhost:8080）
  --output-dir DIR      输出目录（日志、patch、诊断 JSON）
  --build-cmd CMD       编译命令模板（默认 "clang -fsave-optimization-record {src}"）
  --test-cmd CMD        测试命令模板
  --dry-run             仅收集诊断，不执行修改
  --verbose             详细日志
```

### 4.4 Offline Tool CLI 接口

```
aimv-analyze [OPTIONS] <opt_record_file.yaml>

OPTIONS:
  --output-dir DIR      输出分析报告
  --format FORMAT       输出格式 (json | markdown | html)
  --sort-by PRIORITY    排序依据 (severity | frequency | estimated_gain)
  --top-n N             仅输出前 N 项
```

---

## 5. MCP Prompt 工程

### 5.1 System Prompt 模板

```
You are an expert compiler engineer specializing in C/C++ loop vectorization
for embedded systems. Your task is to analyze LLVM optimization remarks (opt-info)
and suggest source-level modifications to enable automatic vectorization.

Target platform: {triple}, CPU: {cpu}, Vector width: {vector_width} bits

You must:
1. Analyze the provided diagnostic information to understand WHY vectorization failed
2. Map the problem from LLVM IR back to the original C/C++ source code
3. Generate a specific, minimal source-level modification to fix the issue
4. Respond in a structured JSON format matching the AnalyzeResponse schema

IMPORTANT RULES:
- Do NOT suggest modifications that change program semantics
- Prefer adding qualifiers (restrict, const, alignas) over rewriting loops
- If you cannot determine a safe fix, set no_action_possible=true
- Each suggestion must be a single, local change
- Provide a unified diff in the response
- Be aware this is embedded code: avoid heap allocations, respect stack limits
```

### 5.2 诊断上下文注入格式

```
## Function: {function.name}
Source file: {function.source_file}, line: {function.loop_line}

### Source Code
```c
{function.source_code}
```

### Vectorization Failure
{for diagnostics as diag}
---
Pass: {diag.pass_name}
Remark: {diag.remark_text}

#### LLVM IR (optimized)
```llvm
{diag.ir_snippet}
```

#### Cost Model
Scalar cost: {diag.cost_model.scalar_cost}
Vector cost: {diag.cost_model.vector_cost} (VF={diag.cost_model.vf})

#### Dependencies
{dep info table}

#### Previous Attempts (this session)
{history summary}

{endfor}
```

---

## 6. 实施阶段

### 阶段 0: 基础设施搭建 (2 周)

**目标**: 项目骨架、构建系统、CI/CD

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 1 | 目录结构搭建，CMakeLists.txt，Python venv/requirements | 可构建的空骨架 |
| 2 | 配置系统（YAML schema），日志基础设施，单元测试框架 | 可运行的测试框架 |

**里程碑**: `aimv-driver --help` 可执行，无实际逻辑

---

### 阶段 1: LLVM AIMVFeedback Pass (3 周)

**目标**: 实现诊断信息收集和 JSON 导出

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 3 | `AIMVFeedbackPass` 骨架：remark 遍历框架，`--aimv-output` 参数 | Pass 可加载运行 |
| 4 | `OptInfoCollector`：源码反向映射（`!dbg`），IR 片段提取，remark 结构化 | 完整诊断 JSON 输出 |
| 5 | `CostModelExporter`：VPlan 代价数据提取，依赖分析信息收集 | 含代价模型的完整诊断 |

**里程碑**: 对已知 benchmark 运行 `clang -fpass-plugin=AIMVFeedback -aimv-output=diag.json`，输出完整诊断 JSON

---

### 阶段 2: MCP Server (3 周)

**目标**: REST API 服务 + prompt 工程 + LLM 响应解析

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 6 | FastAPI 骨架，`/analyze-vectorization` 端点，Pydantic 模型 | 可调用的 API |
| 7 | `prompt_builder`：system prompt + 诊断上下文注入，LLM 后端适配（OpenAI/Anthropic SDK） | LLM 可返回分析结果 |
| 8 | `suggestion_parser`：LLM 结构化输出解析为 Suggestion model，模式缓存实现 | 端到端：诊断入 → 建议出 |

**里程碑**: `curl -X POST /analyze-vectorization -d @diag.json` 返回结构化修改建议

---

### 阶段 3: Driver 脚本 (3 周)

**目标**: 闭环迭代编排

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 9 | `build_orchestrator`：编译→解析→请求→重编译 迭代控制，终止条件判断 | 单轮闭环可运行 |
| 10 | `source_patcher`：diff 应用、备份、回滚机制 | 安全的 patch 管理 |
| 11 | `iteration_tracker`：SessionRecord 完整日志，JSON 格式 session 文件 | 完整可追溯日志 |

**里程碑**: `aimv-driver --function=proc dep_fail.c` 完成多轮迭代并输出 session.json

---

### 阶段 4: MVP 集成验证 (2 周)

**目标**: 依赖分析失败 → restrict 单步闭环打通

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 12 | 端到端联调：Pass + Driver + MCP Server，修正 prompt 精度 | 完整链路工作 |
| 13 | 依赖分析 benchmark 集（3-5 个已知失败 case），验证端到端成功率 | benchmark 集 + 测试报告 |

**里程碑**: 至少 3 个 benchmark 通过 AIMV 成功向量化，性能有可测量提升

---

### 阶段 5: 诊断维度扩展 (4 周)

**目标**: 覆盖代价模型 + 内存/对齐两个维度

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 14-15 | 代价模型拒绝维度的 prompt 模板 + 建议策略（pragma clang loop、循环结构调整） | cost_reject 场景可用 |
| 16-17 | 内存/对齐维度的 prompt 模板 + 建议策略（alignas、__builtin_assume_aligned、数据重排） | align_unknown 场景可用 |

**里程碑**: 三大诊断维度（依赖/代价/内存）全部覆盖

---

### 阶段 6: 循环变换组合 (6 周) [Phase 3 扩展]

**目标**: unroll/fusion/fission/interchange 多 pass 协同

| 周次 | 任务 | 交付物 |
|------|------|--------|
| 18-19 | `POST /analyze-loop-transform` 新端点，循环变换 prompt 模板 | 循环变换 API |
| 20-21 | unroll factor 选择、fusion/fission 可行性分析 | 变换组合分析 |
| 22-23 | 多 pass 协同：向量化 + 循环变换联合分析，集成验证 | 端到端联合优化 |

**里程碑**: 对包含嵌套循环/多 pass 的 benchmark 实现变换组合 + 向量化

---

## 7. 依赖关系

**外部版本依赖**:
- LLVM 基线版本: **LLVM 21**（所有 API 调用以此版本为准）
- Python ≥ 3.10
- MCP Server LLM 后端: 与具体 LLM SDK 版本解耦，通过抽象接口适配

```
阶段 0 ──────┬──────▶ 阶段 1 (LLVM Pass)
             │
             ├──────▶ 阶段 2 (MCP Server)
             │
             └──────▶ 阶段 3 (Driver)
                          │
                  ┌───────┴───────┐
                  ▼               ▼
             阶段 4 (MVP)    阶段 5 (维度扩展)
                  │               │
                  └───────┬───────┘
                          ▼
                     阶段 6 (循环变换)
```

- 阶段 0 是所有阶段的前置依赖
- 阶段 1 应先行产出真实 JSON 诊断样本（至少 1 个 benchmark 的完整诊断 JSON）
- 阶段 2/3 可用 mock JSON 并行开发骨架，但完整 prompt 工程和 driver 集成需真实 JSON 样本验证
- 阶段 4 依赖 1+2+3 全部完成
- 阶段 5/6 依赖 MVP 验证通过

---

## 8. 关键风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| `!dbg` 反向映射不精确（优化后 IR 行号漂移） | AI 建议无法精确定位源码行 | 多级回退：!dbg → DILocation → 函数级匹配；标注 "approximate" |
| LLM 输出格式不稳定 | 无法自动解析为结构化 patch | 强制 JSON schema 约束 + 重试机制 + 格式验证层 |
| LLM 幻觉导致语义错误 | 引入 bug | 多层验证（编译+测试），回滚机制 |
| VPlan 逐指令代价不易导出 | 诊断信息不完整 | MVP 使用 expectedCost() 总代价，足够判断向量化是否划算；逐指令代价延后到 Phase 2 |
| MCP 服务延迟大 | 编译迭代变慢 | 异步请求 + 模式缓存 + 本地预处理过滤 |
| LLM token 消耗大 | 成本高 | 诊断信息压缩（只发失败循环的 IR，裁剪非必要上下文） |

---

## 9. 配置模板

```yaml
# aimv_config.yaml — 默认配置

aimv:
  # 迭代控制
  max_rounds: 5
  aimv_level: "moderate"           # conservative | moderate | aggressive

  # MCP 服务
  mcp:
    url: "http://localhost:8080"
    timeout_seconds: 60
    retry_count: 2
    cache_enabled: true
    cache_ttl_hours: 24

  # 编译
  build:
    cc: "clang"
    cflags: "-O2 -fsave-optimization-record -g -Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize"
    opt_record_format: "yaml"

  # 验证
  verify:
    test_cmd: "make test"
    check_vectorization: true      # 检查重编译后 remark 是否变为 passed
    measure_perf: false            # 需要硬件 PMU 或计时框架

  # 输出
  output:
    dir: "./aimv-output"
    keep_sessions: true
    log_level: "info"
```

---

*文档版本: 1.0*
*创建日期: 2026-04-29*
