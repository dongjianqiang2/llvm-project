# AIMV 实施任务分解

> 基于 `aimv_design_doc/` 设计文档（v2.0 系列），按 8 阶段拆分为原子任务。
> 每个任务可独立实现、独立测试，任务间通过依赖关系串联。
> 标注 [MVP] 的为阶段 0-4 必须完成的任务。
> 阶段 7-8 为 Phase 2b/2c 扩展（SLP + Unrolling），复用 !aimv.diag 通道。

---

## 阶段 0: 基础设施搭建

### T0.1 [MVP] 项目目录结构与 CMake 骨架

**目标**: 创建 `aimv/` 目录树、Python 包结构、LLVM AIMV 组件 CMake 配置

**实现**:
- 创建 `aimv/` 下 `driver/`、`mcp_server/`、`ci/`、`test/`、`benchmarks/`、`config/` 目录
- 创建 `aimv/driver/__init__.py`、`aimv/mcp_server/__init__.py`、`aimv/ci/__init__.py`
- 创建 `aimv/driver/requirements.txt`（httpx, pyyaml, jinja2）
- 创建 `aimv/mcp_server/requirements.txt`（fastapi, uvicorn, httpx, pyyaml, jinja2, openai, anthropic）
- 创建 `llvm/lib/Transforms/AIMV/CMakeLists.txt`（`add_llvm_component_library(LLVMAIMV ...)`）
- 修改 `llvm/lib/Transforms/CMakeLists.txt` 添加 `add_subdirectory(AIMV)`
- 修改 `llvm/lib/Passes/CMakeLists.txt` 添加 `AIMV` 到 LINK_COMPONENTS

**测试**:
- `cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_ENABLE_PROJECTS=clang -DLLVM_TARGETS_TO_BUILD=ARM;AArch64` 成功
- `ninja -C build LLVMAIMV` 成功（产出空组件库）
- `python -c "import aimv.driver; import aimv.mcp_server; import aimv.ci"` 成功

---

### T0.2 [MVP] 配置系统 (`aimv/driver/config.py`)

**前置**: T0.1

**目标**: 实现 `DriverConfig` 加载——环境变量 > `~/.aimv/config` > 默认值

**实现**:
- `DriverConfig` dataclass: `mcp_url`, `mcp_api_key`, `aimv_level`, `max_rounds`, `test_cmd`, `cc`, `cflags`, `output_dir`, `aimv_mode`
- `load_config()` 函数: 读取 YAML → 环境变量覆盖 → 校验（`raise ValueError` 非 `assert`）
- 环境变量: `AIMV_MCP_URL`, `AIMV_MCP_API_KEY`, `AIMV_LEVEL`, `AIMV_MAX_ROUNDS`, `AIMV_TEST_CMD`, `AIMV_MODE`
- YAML 结构: 顶层 `mcp:` + `driver:` 键（与 DRIVER_DESIGN §8.3 对齐）

**测试**:
- `test_config_defaults`: 不设环境变量、无配置文件 → 全部为默认值
- `test_config_yaml`: 创建 `~/.aimv/config` → `mcp.url` 和 `driver.max_rounds` 被读取
- `test_config_env_override`: 设置 `AIMV_MCP_URL` → 覆盖 YAML 中的值
- `test_config_invalid_level`: `aimv_level="invalid"` → `raise ValueError`
- `test_config_invalid_max_rounds`: `max_rounds=0` → `raise ValueError`

---

### T0.3 [MVP] 内部数据模型

**前置**: T0.2

**目标**: 实现 Driver 侧的枚举和数据类

**实现**:
- `IterationStatus` 枚举: PENDING → COMPILING → ANALYZING → QUERYING → PATCHING → VERIFYING → SUCCESS/FAILED/ROLLED_BACK
- `TerminationReason` 枚举: VECTORIZED, ROUND_LIMIT, NO_IMPROVEMENT, NO_SUGGESTION, COMPILE_ERROR, TEST_FAILURE, LOCK_TIMEOUT, MCP_ERROR, INTERRUPTED
- `NextAction` 枚举: CONTINUE, RETRY_SAME, ESCALATE_LEVEL, ROLLBACK, STOP
- `BuildResult` dataclass: returncode, stdout, stderr, opt_record_path, aimv_json_path, elapsed_ms
- `TestResult` dataclass: returncode, stdout, stderr, passed, failed, elapsed_ms
- `VectorizationStatus` dataclass: total_loops, vectorized_loops, missed_loops, passed_remark_count
- `RoundRecord` dataclass: round_number, status, diagnostics_json, mcp_request, mcp_response, patch, verify_build, test_result, vectorized, started_at, finished_at
- `PerFunctionResult` dataclass: function_name, termination_reason, rounds_used, final_level, patches_applied, cross_function_regression
- `SessionRecord` dataclass: session_id, source_file, started_at, functions, aimv_level
- `PatchRecord` dataclass: source_file, backup_path, diff_text, original_hash, applied_at
- 序列化: `_serialize()` 递归 dataclass → JSON dict，Enum → value

**测试**:
- `test_iteration_status_transitions`: 验证合法状态转换路径
- `test_termination_reason_values`: 枚举值与 SPEC 定义一致
- `test_round_record_serialize`: RoundRecord → JSON → 反序列化回 RoundRecord 一致
- `test_session_record_multi_function`: 多 PerFunctionResult 的序列化/反序列化

---

### T0.4 [MVP] 日志与测试框架

**前置**: T0.1

**目标**: 统一日志格式 + pytest 基础设施

**实现**:
- `aimv/driver/logger.py`: 基于 `logging` 模块，`[AIMV]` 前缀格式化器
- `aimv/test/conftest.py`: pytest fixtures（临时配置文件、mock clang 路径、临时源文件）
- `aimv/test/__init__.py`
- `aimv/pytest.ini` 或 `pyproject.toml` 中的 pytest 配置

**测试**:
- `test_logger_format`: 日志输出包含 `[AIMV]` 前缀
- `test_conftest_fixtures`: 临时文件 fixture 创建/清理正常

---

## 阶段 1: LLVM AIMVFeedback Pass

### T1.1 [MVP] AIMVDiagnostic.h 头文件 + AIMVCostSnapshot

**前置**: T0.1

**目标**: 创建 `llvm/include/llvm/Analysis/AIMVDiagnostic.h`，声明 `emitAIMVDiagnostic()` 和 `AIMVCostSnapshot`

**实现**:
- `AIMVCostSnapshot` struct: `ScalarCost`, `VectorCost`, `VF`, `IC` + `unknown()` static 方法
- `emitAIMVDiagnostic()` 声明: 参数 `(Module&, Function&, Loop&, const LoopAccessInfo*, const AIMVCostSnapshot&, StringRef, StringRef, ScalarEvolution* = nullptr, int = -1, int = -1)`
- `#include` 守卫、LLVM 命名空间
- 注释: `[AIMV]` 前缀

**测试**:
- 头文件可被 `LoopVectorize.cpp` 和 `LoopAccessAnalysis.cpp` 同时 `#include`（编译通过）
- `AIMVCostSnapshot::unknown()` 产出 `{ScalarCost: -1, VectorCost: -1, VF: 0, IC: 0}`

---

### T1.2 [MVP] emitAIMVDiagnostic() 实现

**前置**: T1.1

**目标**: 创建 `llvm/lib/Analysis/AIMVDiagnostic.cpp`，实现 `emitAIMVDiagnostic()`

**实现**:
- 修改 `llvm/lib/Analysis/CMakeLists.txt` 添加 `AIMVDiagnostic.cpp`
- `source_location`: DebugLoc → DILocation → DISubprogram 回退链，使用 `raw_string_ostream`
- `cost_data`: 从 `AIMVCostSnapshot` 直接读取（调用方已预计算）
- `dep_data`: `LAI->getDepChecker().getDependences()` null-check，`DepName[Dep.Type]`，`isSafeForVectorization()`
- `memory_data`: num_stores/num_loads 从 LAI，num_pred_stores 填 None(-1)，max_alignment 遍历循环体 Load/Store，stride 推断
- `loop_info`: `SE->getSmallConstantTripCount()` (SE 返回 0 时映射为 -1)
- 组装 9-operand MDNode 追加到 `!aimv.diag`

**测试** (llvm-lit):
- `test_emit_aimv_diagnostic_alias.ll`: 含 alias 的循环 → `!aimv.diag` 包含 CantReorderMemOps 条目，dep_data 非空
- `test_emit_aimv_diagnostic_unsafe_dep.ll`: 含 UnsafeDep → `!aimv.diag` 包含 UnsafeDep 条目，cost_data 全 -1
- `test_emit_aimv_diagnostic_cost_reject.ll`: 代价模型拒绝 → cost_data 含 scalar_cost > vector_cost
- `test_emit_aimv_diagnostic_vectorized.ll`: 向量化成功 → `!aimv.diag` 包含 LoopVectorized 条目，severity="passed"
- `test_emit_aimv_diagnostic_no_dbg.ll`: 无 debug info → source_location 不崩溃（回退到 "unknown"）

---

### T1.3 [MVP] LoopVectorize 5 个插入点

**前置**: T1.1, T1.2

**目标**: 在 `LoopVectorize.cpp` 的 5 个位置插入 `emitAIMVDiagnostic()` 调用

**实现**:
- 插入点 CantReorderMemOps: `isOutsideLoopWorkProfitable()` 返回 false 后，构造 `AIMVCostSnapshot` 并调用（`LoopVectorize.cpp`）
- 插入点 VectorizationNotBeneficial: `VF.Width.isScalar()` 时（`LoopVectorize.cpp`）
- 插入点 UnsafeDep: `Analysis/LoopAccessAnalysis.cpp` 的 `emitUnsafeDependenceRemark()` 末尾，传 `AIMVCostSnapshot::unknown()`（调用 `Analysis/AIMVDiagnostic.cpp` 中的 `emitAIMVDiagnostic()`）
- 插入点 InterleavingNotBeneficial: IC==1 且未强制交错（`LoopVectorize.cpp`）
- 插入点 LoopVectorized: `processLoop()` 非 epilogue 路径（`LoopVectorize.cpp`）
- 每个插入点: `#include "llvm/Analysis/AIMVDiagnostic.h"`，预计算 `AIMVCostSnapshot`

**测试** (llvm-lit):
- 对 `dep_fail_alias.c` 编译 → `!aimv.diag` 含 CantReorderMemOps 记录
- 对 `dep_fail_stride.c` 编译 → `!aimv.diag` 含 UnsafeDep 记录
- 对简单可向量化循环编译 → `!aimv.diag` 含 LoopVectorized 记录
- 无向量化失败的函数 → `!aimv.diag` 仅含 passed 记录

---

### T1.4 [MVP] AIMVDiagnosticParser (Metadata → RawDiagnostic)

**前置**: T1.3

**目标**: 创建 `llvm/lib/Transforms/AIMV/AIMVDiagnosticParser.cpp`，解析 `!aimv.diag`

**实现**:
- `parseDiagnostics(Module &M)` → `std::vector<RawDiagnostic>`
- 遍历 `M.getNamedMetadata("aimv.diag")` 的 operands
- 解析 9-operand MDNode 到 `RawDiagnostic` 结构
- `source_accuracy` 标注: source_location 含 "approximate" → 标记
- trip_count: 0 表示零次迭代，-1 表示不可用
- 跳过 loop_id_str operand（MVP parser 忽略，保留扩展）

**测试** (llvm-lit):
- `test_parser_single_diag.ll`: 单条诊断 → RawDiagnostic 字段正确
- `test_parser_multi_diag.ll`: 多条诊断（含 passed + missed）→ 数量正确，字段不混
- `test_parser_empty_module.ll`: Module 无 `!aimv.diag` → 返回空 vector
- `test_parser_approximate_source.ll`: source_location 回退 → `source_accuracy == "approximate"`

---

### T1.5 [MVP] AIMVFeedbackPass + AIMVDiagnosticAnalysis

**前置**: T1.4, T1.6

**目标**: 创建 Function Pass + Module Analysis 缓存

**实现**:
- `AIMVDiagnosticAnalysis` (Module Analysis): `run()` 调用 `parseDiagnostics()`，`Result` 持有 `vector<RawDiagnostic>`
- `AIMVFeedbackPass` (Function Pass):
  - `cl::opt` 全局定义: `AIMVOutputPath`, `AIMVEnable`, `AIMVTargetFunction`, `AIMVDryRun`
  - `run()`: 读 cl::opt → 激活检查 → `AM.getResult<AIMVDiagnosticAnalysis>()` 获取缓存 → 按 FunctionName 筛选 → 获取 target info (TTI) → 源码反向映射 → IR 片段提取 → 构建 JSON → 追加写入文件
- 注册: `PassRegistry.def` 中 `MODULE_ANALYSIS("aimv-diagnostic", ...)` + `FUNCTION_PASS("aimv-feedback", ...)`
- Pipeline: `PassBuilderPipelines.cpp` 中 `RequireAnalysisPass<AIMVDiagnosticAnalysis, Function>()` + `AIMVFeedbackPass()`

**测试** (llvm-lit):
- `test_aimv_feedback_pass_single_func.ll`: 单函数 → aimv.json 含该函数的 diagnostics
- `test_aimv_feedback_pass_multi_func.ll`: 多函数 → aimv.json 按 function_name 分组，每函数独立
- `test_aimv_feedback_pass_target_function.ll`: `-aimv-target-function=foo` → 仅含 foo 的诊断
- `test_aimv_feedback_pass_disabled.ll`: 无 `-aimv-enable` → 无 aimv.json 输出
- `test_aimv_feedback_pass_cache.ll`: 多函数 Module → parseDiagnostics 仅执行一次（ModuleAnalysis 缓存）
- `test_aimv_feedback_pass_json_schema.ll`: 输出 JSON 结构与 MCP_DESIGN AnalyzeRequest 的 diagnostics 部分兼容（依赖 T1.6 extractSourceContext 完整功能）

---

### T1.6 [MVP] 源码反向映射

**前置**: T1.4

**目标**: IR → 源码位置 + 上下文提取

**实现**:
- `extractSourceContext()`: `!dbg` → DILocation → `getFile()` + `getLine()` + `getColumn()`
- 回退链: `!dbg` → DISubprogram → 循环 header DebugLoc → 函数名匹配 + "approximate" 标记
- 源码上下文: 读取源文件，提取目标行 ±3 行
- IR 片段: 打印循环体 IR（`print()` 到 string stream）

**测试** (llvm-lit):
- `test_source_mapping_precise.ll`: 有 `-g` debug info → source_location 格式 "file.c:42:5"，source_accuracy 为空
- `test_source_mapping_approximate.ll`: 优化后 `!dbg` 漂移 → source_accuracy = "approximate"
- `test_source_mapping_no_debug.ll`: 无 `-g` → source_location = "unknown"，不崩溃
- `test_ir_snippet.ll`: ir_snippet 非空，包含循环体 IR

---

### T1.7 [MVP] clang Driver

**前置**: T1.5 `-faimv` flag 注册 + LLVM flags 转发

**目标**: `Options.td` 注册 + `Clang.cpp` 转发

**实现**:
- `clang/include/clang/Driver/Options.td`: `def faimv` / `def fno_aimv` (Flag, Group<f_Group>)
- `clang/lib/Driver/ToolChains/Clang.cpp`: 检测 `-faimv` → 转发 `-mllvm -aimv-enable` + `-mllvm -aimv-output=<tmp_path>`
- `clang/lib/CodeGen/BackendUtil.cpp`: `extern cl::opt<...> AIMVOutputPath; AIMVEnable;` 引用（不重复定义）
- 暂不实现 fork+exec（阶段 4 集成）

**测试**:
- `clang -O2 -faimv -c test.c` → 编译成功，aimv.json 产出
- `clang -O2 -faimv -mllvm -aimv-dry-run -c test.c` → 产出 aimv.json 但不调用 driver
- `clang -O2 -fno-aimv -c test.c` → 无 AIMV 行为
- `clang -O2 -c test.c`（无 -faimv）→ 无 AIMV 行为

---

## 阶段 2: MCP Server

### T2.1 [MVP] Pydantic

**前置**: T0.1 数据模型 (`aimv/mcp_server/models.py`)

**目标**: 实现 MCP_DESIGN.md §3 中定义的所有 Pydantic 模型

**实现**:
- `RemarkSeverity`, `AimvLevel` 枚举
- `CostModelDetail`: scalar_cost/vector_cost/vf/interleave_count (ge=-1 或 ge=0)
- `DependencyInfo`: dep_type (pattern 约束 LLVM 8 种 DepType), source_ptr, sink_ptr, alias_result
- `MemoryInfo`: num_stores/num_loads (ge=0), num_pred_stores (Optional[int]=None, MVP 阶段 LAI 不提供此数据，None 表示不可用), max_alignment (ge=0), stride, memory_check_count (Optional[int]=None, None=legality 阶段不可用), memory_check_cost (Optional[int]=None, None=不可用, -1=Invalid InstructionCost) — 与 MCP_DESIGN.md §3 修改后的定义一致
- `LoopInfo`: trip_count (description: -1=不可用, 0=空循环, >0=具体值)
- `SingleDiagnostic`: 含 source_accuracy (Optional[str]=None)
- `TargetInfo`, `FunctionInfo` (signature 为 str，非 Optional), `HistoryRecord`
- `AnalyzeRequest`: request_id, target, function, diagnostics (1-20), history, aimv_level
- `Suggestion`: description, reasoning, diff, estimated_impact, safety_concern 等
- `AnalyzeResponse`: request_id, suggestions, overall_analysis, confidence, no_action_possible

**测试**:
- `test_analyze_request_valid`: 合法 JSON → 解析成功
- `test_analyze_request_missing_diagnostics`: diagnostics 为空 → ValidationError
- `test_analyze_request_too_many_diagnostics`: diagnostics > 20 → ValidationError
- `test_memory_info_pred_stores_none`: num_pred_stores=None → 验证通过
- `test_dependency_info_invalid_dep_type`: dep_type="InvalidType" → ValidationError
- `test_loop_info_trip_count_semantics`: -1, 0, 42 均合法
- `test_analyze_response_no_action_possible`: no_action_possible=True, suggestions=[] → 合法

---

### T2.2 [MVP] FastAPI

**前置**: T2.1 应用骨架 + API 路由 (`aimv/mcp_server/aimv_server.py`)

**目标**: FastAPI 入口 + 4 个端点注册

**实现**:
- `POST /api/v1/analyze-vectorization`: 接收 AnalyzeRequest → 返回 AnalyzeResponse
- `GET /api/v1/health`: 返回 status/backend/cache_hits/uptime
- `GET /api/v1/cache/stats`: 返回 DiagnosticCache 统计
- `POST /api/v1/feedback`: 接收 pattern/strategy/result → 异步写入
- `APIKeyMiddleware`: Bearer token 认证，返回 JSONResponse（非 HTTPException）

**测试**:
- `test_health_no_auth`: GET /health 无需认证 → 200
- `test_analyze_no_auth`: POST /analyze-vectorization 无 Bearer → 401
- `test_analyze_invalid_api_key`: Bearer wrong-key → 401
- `test_analyze_valid_request`: 有效 Bearer + 合法请求 → 200 + AnalyzeResponse 结构
- `test_feedback_endpoint`: POST /feedback → 200

---

### T2.3 [MVP] LLM 后端

**前置**: T2.1抽象层 (`aimv/mcp_server/llm/`)

**目标**: 实现 `AbstractLLMBackend` + OpenAI/Anthropic/DeepSeek 三个实现

**实现**:
- `base.py`: `AbstractLLMBackend` ABC，`analyze(request) -> AnalyzeResponse` + `health_check() -> bool`
- `openai_backend.py`: `OpenAIBackend`，SDK 调用，`response_format={"type": "json_object"}`
- `anthropic_backend.py`: `AnthropicBackend`，System Prompt 注入 JSON Schema
- `deepseek_backend.py`: 继承 OpenAI，`base_url="https://api.deepseek.com/v1"`，不调用 `super().__init__()`

**测试**:
- `test_openai_backend_json_format`: mock OpenAI SDK → 返回合法 JSON
- `test_anthropic_backend_schema_injection`: system prompt 末尾包含 JSON schema
- `test_deepseek_base_url`: 实例的 base_url == "https://api.deepseek.com/v1"
- `test_backend_health_check`: mock 200 响应 → True；mock timeout → False

---

### T2.4 [MVP] Prompt 构建器

**前置**: T2.1 (`aimv/mcp_server/prompt_builder.py` + templates)

**目标**: Jinja2 模板渲染诊断信息到 LLM prompt

**实现**:
- `system_prompt.txt`: AI 角色（编译器工程师）、规则（不改变语义、每轮一个变更、aimv_level 限制范围）、JSON schema 约束
- `diagnostic_block.j2`: 函数信息 + 源码 + 逐条诊断（含 cost model 表格、dep_type 语义说明、memory 表格、loop 结构表）
- `history_block.j2`: 最近 3 轮历史 + "Do NOT repeat" 指示
- `vf=0` 条件渲染: `VF=0` → "not determined, legality rejection"
- `source_accuracy` 条件渲染: "approximate" → 警告行号可能偏差

**测试**:
- `test_system_prompt_contains_rules`: 包含 "conservative"/"restrict"/"do not modify headers"
- `test_diagnostic_block_alias_case`: 渲染 CantReorderMemOps → 包含 dependency 表格
- `test_diagnostic_block_vf_zero`: vf=0 → 显示 "not determined" 而非 "VF=0"
- `test_diagnostic_block_source_accuracy`: source_accuracy="approximate" → 包含 WARNING
- `test_history_block_not_repeat`: 包含 "Do NOT repeat"
- `test_prompt_level_escalation`: aimv_level="aggressive" → 包含 "data structure changes"

---

### T2.5 [MVP] 响应解析器

**前置**: T2.1 (`aimv/mcp_server/suggestion_parser.py`)

**目标**: LLM 输出 → `AnalyzeResponse` Pydantic 对象

**实现**:
- `_extract_json()`: 大括号深度计数（非正则），跟踪字符串内/外状态和转义字符
- `parse_structured_response()`: `_extract_json()` → `AnalyzeResponse.model_validate()` → 失败则 `SuggestionParseError`
- 重试注入: parse 失败时将错误信息注入下一轮 prompt（最多追加 1 次）

**测试**:
- `test_extract_json_plain`: `{"key": "value"}` → 正确提取
- `test_extract_json_markdown_wrapped`: ```json\n{...}\n``` → 正确提取
- `test_extract_json_with_trailing_comma`: `{...,}` → 提取后需容错（宽松解析）
- `test_extract_json_with_diff_backticks`: diff 字段含 ``` → 不被截断
- `test_parse_valid_response`: 合法 JSON → AnalyzeResponse 对象
- `test_parse_invalid_response`: 非法 JSON → SuggestionParseError
- `test_parse_with_apology_prefix`: "I'm sorry, here is..." + JSON → 正确提取 JSON

---

### T2.6 [MVP] 诊断缓存

**前置**: T2.1 (`aimv/mcp_server/cache.py`)

**目标**: 双模式缓存（strict/relaxed）

**实现**:
- `compute_diagnostic_fingerprint()`: strict 模式（target + function_name + source_code SHA256 + diagnostics + history），relaxed 模式（target + diagnostics pass+remark_id+remark_prefix）
- `DiagnosticCache`: L1 内存 dict (10K cap)，L2 Redis (TTL 24h，可选)
- 线程安全: `threading.Lock`
- 统计: hits, misses, hit_rate, cost_saved

**测试**:
- `test_cache_strict_hit`: 相同诊断 → 命中
- `test_cache_strict_miss_different_source`: source_code 变更 → 未命中
- `test_cache_relaxed_hit`: 不同函数但相同诊断模式 → 命中
- `test_cache_ttl_expiry`: 模拟 TTL 过期 → 未命中
- `test_cache_stats`: 多次查询后 hit_rate 正确

---

### T2.7 [MVP] 错误处理中间件

**前置**: T2.2 (`aimv/mcp_server/middleware.py`)

**目标**: `AIMVErrorHandler` + `APIKeyMiddleware`

**实现**:
- `AIMVErrorHandler.handle_llm_call()`: 可重试错误（rate_limit/server_error/timeout/overloaded）→ 指数退避最多 2 次 → 仍失败返回 `AnalyzeResponse(no_action_possible=True)`
- `APIKeyMiddleware`: 校验 Bearer token，返回 `JSONResponse(status_code=401)`（非 HTTPException）
- 注释标注: `backend.analyze()` 是同步阻塞调用，MVP 可接受，生产环境应改用 `run_in_executor`

**测试**:
- `test_error_handler_success`: LLM 首次成功 → 返回结果
- `test_error_handler_retry_then_success`: 第 1 次 rate_limit → 重试 → 成功
- `test_error_handler_exhausted`: 3 次 server_error → `no_action_possible=True`
- `test_api_key_valid`: 正确 Bearer → 200
- `test_api_key_invalid`: 错误 Bearer → 401 JSONResponse
- `test_api_key_missing`: 无 Authorization 头 → 401

---

## 阶段 3: Driver 脚本

### T3.1 [MVP] BuildOrchestrator

**前置**: T0.2, T0.3 (`aimv/driver/build_orchestrator.py`)

**目标**: clang 子进程管理 + AIMV flags 注入

**实现**:
- `compile_with_aimv()`: 注入 `-g -mllvm -aimv-enable -mllvm -aimv-output=<path>`
- 影子文件 `-x c/c++` 自动注入: 扩展名非 `.c/.cpp/.cc/.cxx` 时，从原始文件名推断语言
- `run_tests()`: 执行 test_cmd，解析 CTest/GoogleTest 输出，空命令则跳过
- `check_vectorization()`: 解析 aimv.json，统计 `VectorizationStatus`
- 超时处理: `timeout_seconds` 参数（默认 120s）

**测试**:
- `test_compile_with_aimv_basic`: `compile_with_aimv("test.c", "test.o")` → clang 命令含 `-mllvm -aimv-enable`
- `test_compile_shadow_file_x_c`: `compile_with_aimv("test.c.aimv-tmp", ...)` → 命令含 `-x c`
- `test_compile_shadow_file_x_cpp`: `compile_with_aimv("test.cpp.aimv-tmp", ...)` → 命令含 `-x c++`
- `test_run_tests_empty_cmd`: test_cmd="" → TestResult(returncode=0, passed=0, failed=0)
- `test_run_tests_pass`: mock 测试通过 → passed > 0, failed == 0
- `test_check_vectorization_missed`: aimv.json 含 missed 诊断 → missed_loops > 0
- `test_check_vectorization_vectorized`: aimv.json 仅含 passed → vectorized_loops > 0, missed_loops == 0
- `test_compile_timeout`: 超时 → BuildResult.returncode != 0

---

### T3.2 [MVP] MCPClient

**前置**: T0.2 (`aimv/driver/mcp_client.py`)

**目标**: MCP REST 客户端 + 重试 + 认证错误处理

**实现**:
- `analyze()`: POST /api/v1/analyze-vectorization
- 重试: 最多 3 次，指数退避 (2s, 4s)
- 总超时预算: 180s（含重试等待）
- 401/403 → `raise PermissionError("check AIMV_MCP_API_KEY")`
- 422 → `raise ValueError`
- 其他非 2xx → `raise RuntimeError`
- `health()`: GET /api/v1/health

**测试**:
- `test_analyze_success`: mock 200 → 返回 dict
- `test_analyze_401_auth_error`: mock 401 → PermissionError，消息含 "AIMV_MCP_API_KEY"
- `test_analyze_429_retry_success`: mock 429 → 429 → 200 → 成功
- `test_analyze_total_timeout`: mock 每次超时 → 总 180s 后返回 None
- `test_health_ok`: mock 200 → True
- `test_health_fail`: mock 连接错误 → False

---

### T3.3 [MVP] SourceManager

**前置**: T0.3 (`aimv/driver/source_manager.py`)

**目标**: 影子文件协议 + 原子替换 + FileLock + 回滚 + 竞态检测

**实现**:
- `file_hash()`: SHA256 文件哈希
- `snapshot_hash()` / `check_stale()`: MCP 查询前记录 hash，获取锁后校验
- `acquire_lock()` / `release_lock()`: fcntl.flock，per-source-file 粒度，30s 超时
- `apply_shadow_patch()`: cp → patch → 返回 PatchRecord
- `commit_shadow()`: `os.replace()` 原子替换
- `discard_shadow()`: rm 影子文件
- `rollback()`: 从 backup 恢复（含 SHA256 校验）
- `rollback_all()`: 反序回滚所有 patch
- `rollback_last()`: 仅回滚最后一个 patch
- `check_stale_shadow()`: 检测 kill -9 残留 `.aimv-tmp` 文件

**测试**:
- `test_apply_and_commit_shadow`: cp → patch → commit → 原文件已更新
- `test_apply_and_discard_shadow`: cp → patch → discard → 原文件不变
- `test_rollback_restores_original`: 2 次 patch → rollback_all → 文件恢复原始内容
- `test_file_lock_serialization`: 两个线程竞争同一文件 → 串行执行
- `test_file_lock_timeout`: 获取锁超时 → 返回 False
- `test_stale_shadow_detection`: 残留 .aimv-tmp → check_stale_shadow() 返回 True
- `test_race_detection_stale`: MCP 查询后文件被修改 → check_stale() 返回 True
- `test_race_detection_fresh`: MCP 查询后文件未变 → check_stale() 返回 False
- `test_hash_integrity`: patch → commit → 文件 hash 与预期一致

---

### T3.4 [MVP] IterationEngine

**前置**: T0.3 (`aimv/driver/iteration_engine.py`)

**目标**: 迭代决策矩阵 + level 升级 + 安全约束

**实现**:
- `decide()`: 接受 `compile_phase` 参数区分 source/patch 编译失败
- 独立计数器: `_consecutive_compile_failures` (patch) / `_consecutive_source_compile_failures` (source)
- `_try_escalate()`: conservative → moderate → aggressive；升至 aggressive 时自动启用 review 模式
- 决策: vectorized→STOP, max_rounds→STOP, compile_fail(patch)→ROLLBACK/STOP, compile_fail(source)→STOP, test_fail→ROLLBACK+STOP, MCP no_action→ESCALATE_LEVEL/STOP, regression→ROLLBACK+STOP

**测试**:
- `test_decide_vectorized_success`: vectorized=True → (STOP, "vectorization succeeded")
- `test_decide_round_limit`: current_round >= max_rounds → (STOP, ...)
- `test_decide_patch_compile_fail_rollback`: compile_phase="patch", 第 1 次 → (ROLLBACK, ...)
- `test_decide_patch_compile_fail_stop`: compile_phase="patch", 第 2 次 → (STOP, ...)
- `test_decide_source_compile_fail_stop`: compile_phase="source" → 直接 STOP
- `test_decide_test_failure_rollback`: test_result_ok=False → (ROLLBACK, ...)
- `test_decide_no_suggestion_escalate`: no_action_possible → (ESCALATE_LEVEL, ...)，level 从 conservative → moderate
- `test_decide_escalate_to_aggressive_enables_review`: level 升至 aggressive → _review_mode = True
- `test_decide_regression_rollback`: passed_remark_delta < 0 → (ROLLBACK, ...)
- `test_counters_independent`: patch 编译失败不影响 source 计数器

---

### T3.5 [MVP] SessionStore

**前置**: T0.3 (`aimv/driver/session_store.py`)

**目标**: Session 持久化与恢复

**实现**:
- `save()`: 序列化 SessionRecord → 写 tmp + rename 原子写入
- `load()`: 读取 session JSON → 反序列化
- `list_sessions()`: 列出 output_dir/sessions/ 下所有 session
- `session_id` 格式: `f"aimv-{uuid.uuid4().hex[:12]}"`

**测试**:
- `test_save_and_load`: save → load → 内容一致
- `test_save_atomic`: 中断写入（模拟）→ load 不崩溃，返回 None 或旧版本
- `test_list_sessions`: 3 个 session → 列表长度 3
- `test_session_id_format`: session_id 匹配 `aimv-[a-f0-9]{12}`

---

### T3.6 [MVP] MCP 请求构建

**前置**: T0.2, T3.1 (`build_mcp_request()`)

**目标**: 组装 `AnalyzeRequest` JSON

**实现**:
- `request_id`: `f"aimv-{session.session_id}-{hashlib.sha256(func_name.encode()).hexdigest()[:8]}-r{round_rec.round_number}"`（session 从参数传入，func_name 为当前目标函数，round_rec 为当前轮次记录）
- `target`: 从 aimv.json 获取，回退到 config（含 TODO: -mcpu/-mfpu 补充）
- `function`: name, signature（MVP 空字符串）, source_code（当前文件完整内容）, source_file, loop_line（正则 `r":(\d+):\d+$"` 解析）
- `diagnostics`: 筛选当前函数的 missed 诊断
- `history`: 最近 3 轮，含 round/diagnosis_summary/suggestion_applied/outcome
- `aimv_level`: 当前级别

**测试**:
- `test_build_request_id_unique`: 同函数不同轮次 → request_id 不同
- `test_build_request_loop_line_parse`: loop_location="task.c:42:5" → loop_line=42
- `test_build_request_loop_line_windows_path`: "C:\\src\\file.c:42:5" → loop_line=42（不崩溃）
- `test_build_request_history_last_3`: 5 轮历史 → 仅发送最近 3 轮
- `test_build_request_source_code_current`: source_code 为当前文件内容（非原始版本）

---

### T3.7 [MVP] 主循环

**前置**: T3.1, T3.2, T3.3, T3.4, T3.5, T3.6 (`process_single_function()`)

**目标**: 单函数的完整迭代循环

**实现**:
- Step 0: 初始编译 + 诊断 → 检查是否已向量化
- Step 1: 编译获取诊断（`compile_phase="source"`）
- Step 2: MCP 查询（锁外，记录 `pre_query_hash`）
- Step 3: 获取锁 → 竞态检测 → 影子文件 patch
- Step 4: 影子文件编译验证（`compile_phase="patch"`）→ 向量化状态检查 → 收益退化检测
- Step 5: 测试 → 成功则 commit_shadow
- 循环直到 IterationEngine 决定 STOP

**测试**:
- `test_single_function_already_vectorized`: 无 missed 诊断 → 直接 VECTORIZED，不调 MCP
- `test_single_function_success_one_round`: 1 轮 MCP → patch → 编译通过 → 向量化 → VECTORIZED
- `test_single_function_compile_error_rollback`: patch 编译失败 → discard_shadow → 继续
- `test_single_function_test_failure_stop`: 测试失败 → ROLLBACK + STOP
- `test_single_function_regression_rollback`: passed_remark_count 减少 → 回滚 + STOP
- `test_single_function_max_rounds`: 达到 max_rounds → ROUND_LIMIT
- `test_single_function_no_suggestion_escalate`: MCP no_action → escalate level → 再尝试
- `test_single_function_interrupted`: KeyboardInterrupt → 回滚 + INTERRUPTED

---

### T3.8 [MVP] 顶层编排

**前置**: T3.7 (`aimv_driver.py`)

**目标**: CLI 入口 + 多函数顺序处理 + 跨函数回归检测 + stderr 输出

**实现**:
- `--from-json` 模式: 读取 aimv.json → 解析 diagnostics → 按 function_name 分组
- 独立模式: 自行编译获取诊断
- 多函数: `for func_name in failed_functions: process_single_function()`
- 跨函数回归检测: 每函数成功后 `_check_cross_function_regression()`
- `emit_summary()`: 输出格式遵循 SPEC §3.1（每函数一行，含轮次/级别/结果）
- Exit code: 0/2/3/4/5/130/143

**测试**:
- `test_from_json_mode`: 传入 aimv.json + source file → 正常执行
- `test_independent_mode`: 传入 source file → 自行编译获取诊断
- `test_multi_function_sequential`: 2 个失败函数 → 顺序处理
- `test_cross_function_regression`: 函数 B 的 patch 破坏函数 A → 回滚 B
- `test_exit_code_success`: 全部向量化 → exit 0
- `test_exit_code_no_source`: 源文件不存在 → exit 2
- `test_stderr_format`: 输出含 `[AIMV]` 前缀

---

## 阶段 4: MVP 集成验证

### T4.1 [MVP] clang Driver

**前置**: T1.7, T3.8 fork+exec 集成

**目标**: `Clang.cpp` 编译后 fork+exec `aimv-driver --from-json`

**实现**:
- `clang/lib/Driver/ToolChains/Clang.cpp`: 编译完成后 `llvm::sys::ExecuteAndWait("aimv-driver", ["--from-json", aimv_json_path, "--source", source_file])`
- 防无限 fork: `-faimv` 仅在 Driver 层，不传递给子进程
- 异常处理: driver 未安装 → 警告 + 退化；崩溃/超时 → 回滚源码

**测试**:
- `test_faimv_end_to_end`: `clang -O2 -faimv -c dep_fail_alias.c` → 自动调用 aimv-driver → 产出 patch
- `test_no_fork_chain`: driver 内部重编译 → 不含 `-faimv` flag
- `test_driver_not_installed`: 无 aimv-driver → 警告 + 编译仍成功
- `test_driver_crash`: mock driver 崩溃 → 编译不崩溃，源码未修改

---

### T4.2 [MVP] Benchmark

**前置**: T4.1 端到端验证

**目标**: MVP benchmark 通过 AIMV 成功向量化

**实现**:
- `dep_fail_alias.c`: 添加 restrict → 向量化成功
- `dep_fail_stride.c`: 添加 restrict 或 loop fission → 向量化成功
- 验证: 编译 + 测试通过 + 中位数执行时间缩短 >= 5%

**测试**:
- `test_benchmark_dep_fail_alias`: AIMV 全流程 → 函数向量化 + 执行时间改善
- `test_benchmark_dep_fail_stride`: AIMV 全流程 → 函数向量化
- `test_benchmark_no_regression`: AIMV 修改后其他函数性能不退化

---

### T4.3 [MVP] MCP Server Mock

**前置**: T2.3 模式

**目标**: 无真实 LLM 的离线测试

**实现**:
- `MockLLMBackend`: 继承 `AbstractLLMBackend`，对已知诊断模式返回固定建议
- dep_fail_alias → "add restrict to parameter a"
- dep_fail_stride → "add restrict to parameter b"
- 可通过环境变量 `AIMV_LLM_BACKEND=mock` 激活

**测试**:
- `test_mock_backend_alias`: CantReorderMemOps → 建议添加 restrict
- `test_mock_backend_unknown`: 未知诊断 → no_action_possible=True

---

## 阶段 5: 诊断维度扩展

### T5.1 代价模型拒绝 prompt 模板

**目标**: 覆盖 cost_reject 场景

**实现**:
- cost model 表格增强: 含 cost ratio 计算和 "NOT profitable" 判断
- 建议策略: `#pragma clang loop vectorize(enable)`、循环结构调整

**测试**:
- `test_prompt_cost_reject`: VF=0/cost_not_profitable → prompt 含代价分析
- `test_benchmark_cost_reject`: cost_reject.c → AIMV 建议 pragma → 向量化

### T5.2 内存/对齐 prompt 模板

**目标**: 覆盖 align_unknown 场景

**实现**:
- memory_info 表格增强: 含对齐信息解读
- 建议策略: `alignas(16)`、`__builtin_assume_aligned`

**测试**:
- `test_prompt_align_unknown`: alignment=0 → prompt 含对齐建议
- `test_benchmark_align_unknown`: align_unknown.c → AIMV 建议 alignas → 向量化

### T5.3 多维 benchmark 验证

**目标**: multi_fail.c 端到端

**测试**:
- `test_benchmark_multi_fail`: 混合失败 → 多轮迭代 → 至少 1 个维度修复

---

## 阶段 6: 循环变换组合

### T6.1 循环变换 API 端点

**目标**: `POST /analyze-loop-transform`

**测试**:
- 端点可达，Pydantic 模型验证
- 返回含 loop interchange/fission/unroll 建议

### T6.2 循环变换 prompt + 建议策略

**测试**:
- 嵌套循环 → interchange 建议
- 大循环 → unroll 建议

### T6.3 多 pass 协同集成

**测试**:
- 嵌套循环 benchmark → interchange + vectorize 联合成功

---

## CI 工具集（跨阶段）

### T-CI.1 `aimv-detect-changes`

**测试**:
- `test_detect_changed_functions`: git diff → 变更函数列表
- `test_filter_loopy_functions`: 过滤不含循环的函数

### T-CI.2 `aimv-run-batch`

**测试**:
- `test_batch_parallel`: 多文件并行执行
- `test_batch_intra_file_serial`: 同文件函数串行

### T-CI.3 `aimv-report`

**测试**:
- `test_report_markdown`: session JSON → Markdown 表格
- `test_report_gitlab_api`: GitLab 兼容 JSON 格式

### T-CI.4 `aimv-gate`

**测试**:
- `test_gate_level0_report`: 默认模式，不阻止合入
- `test_gate_level1_regression`: 向量化退化 → 阻止
- `test_gate_level2_enforce`: 向量化覆盖率不足 → 阻止

### T-CI.5 `aimv-baseline`

**测试**:
- `test_baseline_save_and_compare`: 保存 + 比较基线

---

## 阶段 7: SLP Vectorizer 诊断集成 (Phase 2b)

### T7.1 SLP 4 个插入点

**前置**: T1.2 (`emitAIMVDiagnostic` 可用，`AIMVCostSnapshot` 已定义)

**目标**: 在 `SLPVectorizer.cpp` 的 4 个位置插入 `emitAIMVDiagnostic()` 调用

**实现**:
- 插入点 UnsupportedType: `tryToVectorize()` 中类型检查失败时，传 `nullptr` Loop/LAI + `AIMVCostSnapshot::unknown()`
- 插入点 SmallVF: 可向量化指令数 < 2 时
- 插入点 NotBeneficial: 代价分析确定打包/解包开销超过收益时
- 插入点 NotPossible: reduction 模式不被识别时
- `pass_name = "SLPVectorize"`
- 所有插入点: `#include "llvm/Analysis/AIMVDiagnostic.h"`
- SLP 无 Loop*/LAI/CM → 全部传 `nullptr` + `AIMVCostSnapshot::unknown()`

**测试** (llvm-lit):
- `test_slp_unsupported_type.ll`: 非 SIMD 类型 → `!aimv.diag` 含 UnsupportedType
- `test_slp_small_vf.ll`: 可向量化指令太少 → SmallVF
- `test_slp_not_beneficial.ll`: 代价不划算 → NotBeneficial
- `test_slp_not_possible.ll`: reduction 失败 → NotPossible

---

### T7.2 SLP 诊断 prompt 模板

**前置**: T7.1

**目标**: 新增 SLP 专用 prompt 模板 `aimv/mcp_server/templates/slp_prompt.txt`

**实现**:
- SLP 4 种诊断的语义说明（与 LoopVectorize 的区别）
- 建议策略: 拆分复合表达式、合并相邻计算、调整数据布局、重写归约
- 空 cost_data / dep_data 的处理（注明 "SLP 不提供代价/依赖信息"）

**测试**:
- `test_slp_prompt_unsupported_type`: 渲染 UnsupportedType → 含类型建议
- `test_slp_prompt_small_vf`: 渲染 SmallVF → 含合并计算建议
- `test_slp_prompt_null_fields`: cost_data/dep_data 为空 → prompt 注明不可用

---

### T7.3 SLP benchmark 端到端验证

**前置**: T7.1, T7.2

**目标**: SLP benchmark 通过 AIMV 成功向量化

**实现**:
- `aimv/benchmarks/slp_unsupported.c`: 复合类型表达式 → 拆分后 SLP 成功
- `aimv/benchmarks/slp_reduction.c`: 非标准归约模式 → 重写后 SLP 成功
- 验证: 编译 + 测试通过 + SLP 向量化统计改善

**测试**:
- `test_benchmark_slp_unsupported`: AIMV 全流程 → SLP 向量化成功
- `test_benchmark_slp_reduction`: AIMV 全流程 → reduction 识别成功

---

## 阶段 8: Loop Unrolling 诊断集成 (Phase 2c)

### T8.1 Unroll 3 个插入点

**前置**: T1.2

**目标**: 在 `LoopUnrollPass.cpp` 的 3 个位置插入 `emitAIMVDiagnostic()` 调用

**实现**:
- 插入点 CantUnrollTripCount: trip count 不可知时（`OptimizationRemarkMissed` 旁）
- 插入点 UnrollNotBeneficial: 展开后代码膨胀超过阈值时
- 插入点 UnrollTooExpensive: 展开代价太高时
- `pass_name = "LoopUnroll"`
- `#include "llvm/Analysis/AIMVDiagnostic.h"`
- Unroll 有 Loop* → 可传递完整 loop_info；无 LAI/CM → 传 `nullptr` + `AIMVCostSnapshot::unknown()`

**测试** (llvm-lit):
- `test_unroll_trip_count.ll`: 可变 trip count → `!aimv.diag` 含 CantUnrollTripCount
- `test_unroll_not_beneficial.ll`: 大循环体 → UnrollNotBeneficial
- `test_unroll_too_expensive.ll`: 高代价 → UnrollTooExpensive

---

### T8.2 Unroll 诊断 prompt 模板

**前置**: T8.1

**目标**: 新增 Unroll 专用 prompt 模板 `aimv/mcp_server/templates/unroll_prompt.txt`

**实现**:
- Unroll 3 种诊断的语义说明
- 建议策略: `__builtin_assume(n >= N)`、循环 fission 拆分、`#pragma clang loop unroll`
- trip_count 信息利用: 已有 trip count 时优先建议 pragma 调整因子

**测试**:
- `test_unroll_prompt_trip_count`: 渲染 CantUnrollTripCount → 含 assume 建议
- `test_unroll_prompt_not_beneficial`: 渲染 UnrollNotBeneficial → 含 fission 建议

---

### T8.3 Unroll benchmark 端到端验证

**前置**: T8.1, T8.2

**目标**: Unroll benchmark 通过 AIMV 改善展开决策

**实现**:
- `aimv/benchmarks/unroll_trip_unknown.c`: trip count 不可知 → assume 后 unroll 成功
- `aimv/benchmarks/unroll_too_large.c`: 循环体过大 → fission + unroll 成功
- 验证: 编译 + 测试通过 + 执行时间改善

**测试**:
- `test_benchmark_unroll_trip_unknown`: AIMV 全流程 → unroll 成功
- `test_benchmark_unroll_too_large`: AIMV 全流程 → fission + unroll 成功
