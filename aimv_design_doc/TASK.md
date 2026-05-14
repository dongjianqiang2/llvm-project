# AIMV 原子任务列表

**版本**: 1.0
**日期**: 2026-05-14
**关联文档**: SPEC.md, PLAN.md, LLVM_DESIGN.md, DRIVER_DESIGN.md, MCP_DESIGN.md, CI_DESIGN.md

---

## 约定

- 每个任务为可独立开发、测试、合入的**原子单元**
- 每个任务必须包含：**任务描述**、**涉及文件**、**验收标准**、**测试用例**
- 任务间依赖用 `前置:` 标注
- 阶段编号对应 PLAN.md 的 Phase 0-6

---

## Phase 0: 基础设施搭建

### T0.1 创建 AIMV 目录结构与 Python 包骨架

**描述**: 创建 `aimv/` 下所有子目录，初始化 Python 包结构（`__init__.py`），创建 `requirements.txt`。

**涉及文件**:
- `aimv/driver/__init__.py` (新建)
- `aimv/driver/requirements.txt` (新建)
- `aimv/mcp-server/__init__.py` (新建)
- `aimv/mcp-server/requirements.txt` (新建)
- `aimv/test/__init__.py` (新建)
- `aimv/benchmarks/` (新建目录)
- `aimv/config/aimv_config.yaml` (新建，从 PLAN §9 配置模板复制)

**验收标准**:
- 目录结构符合 PLAN §1 定义
- `pip install -r aimv/driver/requirements.txt` 成功
- `pip install -r aimv/mcp-server/requirements.txt` 成功

**测试用例**:
- `test_structure.py`: 断言所有必需目录和 `__init__.py` 存在
- `test_requirements.py`: 断言 `pip install -r` 返回 0

---

### T0.2 LLVM 侧目录结构与 CMake 集成

**描述**: 创建 `llvm/lib/Transforms/AIMV/` 目录、CMakeLists.txt、空源文件骨架；修改上层 CMakeLists.txt 注册子目录；在 `Passes/CMakeLists.txt` 中添加 `LLVMAIMV` 链接组件。

**涉及文件**:
- `llvm/lib/Transforms/AIMV/CMakeLists.txt` (新建)
- `llvm/lib/Transforms/AIMV/AIMVFeedbackPass.cpp` (新建，空骨架)
- `llvm/lib/Transforms/AIMV/AIMVDiagnosticParser.cpp` (新建，空骨架)
- `llvm/lib/Transforms/CMakeLists.txt` (修改，+1 行 `add_subdirectory(AIMV)`)
- `llvm/lib/Passes/CMakeLists.txt` (修改，`LINK_COMPONENTS` 添加 `AIMV`)

**验收标准**:
- `ninja -C build LLVMAIMV` 编译成功（空库）
- `ninja -C build clang opt` 编译成功

**测试用例**:
- 构建 smoke test: `ninja -C build LLVMAIMV && echo "OK"`
- 验证 `libLLVMAIMV.a` 或 `LLVMAIMV.so` 产物存在

---

### T0.3 AIMVFeedbackPass 公共头文件

**描述**: 创建 `AIMVFeedback.h` 头文件，定义 `AIMVFeedbackPass` 类（含 `RawDiagnostic` 内部结构体和 `parseDiagnostics()` 静态方法声明）。Pass 实现暂为 stub（直接返回 `PreservedAnalyses::all()`）。

**前置**: T0.2

**涉及文件**:
- `llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h` (新建)

**验收标准**:
- 头文件可被其他编译单元正常 `#include`
- `AIMVFeedbackPass` 类可通过 `PassInfoMixin` 注册

**测试用例**:
- 编译测试：一个 `.cpp` 文件 `#include "llvm/Transforms/AIMV/AIMVFeedback.h"` 编译通过
- 链接测试：`opt -passes=aimv-feedback` 加载不崩溃

---

### T0.4 Pass 注册到 PassRegistry 和 Pipeline

**描述**: 在 `PassRegistry.def` 中注册 `FUNCTION_PASS("aimv-feedback", AIMVFeedbackPass())`；在 `PassBuilderPipelines.cpp` 的 `addVectorPasses()` 中，SLPVectorizer 之后插入 `FPM.addPass(AIMVFeedbackPass())`。

**前置**: T0.3

**涉及文件**:
- `llvm/lib/Passes/PassRegistry.def` (修改，+2 行)
- `llvm/lib/Passes/PassBuilderPipelines.cpp` (修改，+3 行)

**验收标准**:
- `opt -passes='loop-vectorize,aimv-feedback' -S < /dev/null` 不报错
- `opt -passes=aimv-feedback -S < /dev/null` 可运行（stub pass 直接返回）

**测试用例**:
- Lit 测试 `aimv_stub_pass.ll`:
  ```llvm
  ; RUN: opt -passes=aimv-feedback -S < %s | FileCheck %s
  ; CHECK: @main
  define i32 @main() { ret i32 0 }
  ```
- 验证 pass 名称注册：`opt -passes=list | grep aimv-feedback` 输出含 `aimv-feedback`

---

### T0.5 配置加载模块与 CLI 骨架

**描述**: 实现 `aimv/driver/config.py`（YAML 配置加载+校验）和 `aimv/driver/aimv_driver.py` 的 CLI 入口（`argparse` 定义所有选项，`--help` 可输出）。暂无实际逻辑。

**前置**: T0.1

**涉及文件**:
- `aimv/driver/config.py` (新建)
- `aimv/driver/aimv_driver.py` (新建)

**验收标准**:
- `python -m aimv.driver.aimv_driver --help` 输出完整帮助信息
- 配置模板加载并校验通过

**测试用例**:
- `test_config.py`: 加载 `aimv_config.yaml`，断言 `max_rounds` 默认值 = 5
- `test_config.py`: 断言无效配置（如 `max_rounds=-1`）抛出校验异常
- `test_cli.py`: 断言 `--help` 退出码 = 0 且输出含 `--function`

---

### T0.6 MCP Server FastAPI 骨架

**描述**: 创建 `aimv_server.py`，注册三个端点路由（`/api/v1/analyze-vectorization`, `/api/v1/health`, `/api/v1/cache/stats`），暂用 mock 响应。创建 `models.py`，定义所有 Pydantic 模型（与 MCP_DESIGN §3 完全一致）。

**前置**: T0.1

**涉及文件**:
- `aimv/mcp-server/aimv_server.py` (新建)
- `aimv/mcp-server/models.py` (新建)

**验收标准**:
- `uvicorn aimv.mcp-server.aimv_server:app &` 启动不报错
- `curl localhost:8080/api/v1/health` 返回 `{"status":"ok"}`
- Pydantic 模型可通过 `model_validate()` 校验有效/无效 JSON

**测试用例**:
- `test_models.py`: 构造有效 `AnalyzeRequest` JSON → `model_validate()` 成功
- `test_models.py`: 构造缺少 `diagnostics` 的 JSON → 抛出 `ValidationError`
- `test_models.py`: 构造 `dep_type="RAW"` → 抛出 `ValidationError`（不再接受旧值）
- `test_models.py`: 构造 `diagnostics` 含 21 条 → 抛出 `ValidationError`（上限 20）
- `test_server.py`: `GET /api/v1/health` 返回 200
- `test_server.py`: `POST /api/v1/analyze-vectorization` 无 body → 返回 422

---

### T0.7 Benchmark 文件集

**描述**: 创建 SPEC §8.2 定义的 5 个 benchmark C 文件，每个附 `Makefile` 或编译脚本。

**前置**: T0.1

**涉及文件**:
- `aimv/benchmarks/dep_fail_alias.c` (新建)
- `aimv/benchmarks/dep_fail_stride.c` (新建)
- `aimv/benchmarks/cost_reject.c` (新建)
- `aimv/benchmarks/align_unknown.c` (新建)
- `aimv/benchmarks/multi_fail.c` (新建)
- `aimv/benchmarks/Makefile` (新建)

**验收标准**:
- 每个 `.c` 文件 `clang -O2 -c` 编译通过
- `make -C aimv/benchmarks` 编译全部 5 个
- 每个 benchmark 在 `-O2 -Rpass-missed=loop-vectorize` 下产出 missed remark

**测试用例**:
- `test_benchmarks.py`: 断言每个 `.c` 文件编译成功且 stderr 含 `loop not vectorized`
- `test_benchmarks.py`: 断言 `dep_fail_alias.c` 的 missed remark 含 `CantReorderMemOps` 或类似别名分析失败文本

---

## Phase 1: LLVM AIMVFeedback Pass

### T1.1 AIMVDiagnostic.h 共享头文件

**描述**: 创建 `llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h`，声明 `emitAIMVDiagnostic()` 函数。该头文件由 LoopVectorize.cpp 和 LoopAccessAnalysis.cpp 共同包含。

**前置**: T0.4

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h` (新建, ~20 行)

**验收标准**:
- 声明匹配 LLVM_DESIGN §2.2 中的函数签名
- LoopVectorize.cpp 包含此头文件编译通过

**测试用例**:
- 编译测试：包含此头文件的 `.cpp` 编译通过
- 链接测试：LoopVectorize.cpp 引用该声明后 ninja 编译链接成功

---

### T1.2 实现 emitAIMVDiagnostic() 函数体

**描述**: 在 `LoopVectorize.cpp` 中实现 `emitAIMVDiagnostic()` 完整函数体，包括 `!aimv.diag` Named Metadata 构建逻辑（cost_data、dep_data、memory_data、loop_info 四个子结构）。

**前置**: T1.1

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` (修改，+~100 行)

**验收标准**:
- 函数通过编译，正确构建 MDNode 并追加到 `!aimv.diag`
- 当 `getLLVMRemarkStreamer()` 为空时立即返回（不写入）
- 当 LAI/CM 为 nullptr 时使用默认值（不崩溃）

**测试用例**:
- `aimv_emit_diag_basic.ll`:
  ```llvm
  ; RUN: opt -passes=loop-vectorize -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize -S < %s
  ; RUN: FileCheck %s --check-prefix=MD < %t.ll
  ; MD: !aimv.diag
  ```
  验证 IR 输出中出现 `!aimv.diag` Named Metadata。
  注：`opt` 使用 `-pass-remarks-output`（非 clang 的 `-fsave-optimization-record`）。
- `aimv_emit_diag_no_streamer.ll`:
  ```llvm
  ; RUN: opt -passes=loop-vectorize -S < %s | FileCheck %s --check-prefix=NO_MD
  ; NO_MD-NOT: aimv.diag
  ```
  验证无 remark streamer 时不生成 `!aimv.diag`
- `aimv_emit_diag_null_lai.ll`: 传入 `LAI=nullptr`，验证不崩溃且 dep_data 计数为 0

---

### T1.3 插入点 1: CantReorderMemOps

**描述**: 在 `LoopVectorize.cpp` 的 `CantReorderMemOps` 拒绝点（`isOutsideLoopWorkProfitable` 返回 false 处）插入 `emitAIMVDiagnostic()` 调用。

**前置**: T1.2

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` (修改)

**验收标准**:
- 当循环因 runtime check 代价过高被拒绝时，`!aimv.diag` 包含对应的诊断记录
- RemarkID = `"CantReorderMemOps"`
- cost_data 包含有效标量/向量代价和 VF

**测试用例**:
- `aimv_cant_reorder.ll`: 使用有指针别名的循环 IR，验证:
  - `!aimv.diag` 存在
  - 诊断的 remark_id = `"CantReorderMemOps"`
  - cost_data 的 scalar_cost > 0, vector_cost > 0
  - dep_data 包含至少一条 `Backward` 类型依赖

---

### T1.4 插入点 2: VectorizationNotBeneficial

**描述**: 在代价模型拒绝点插入 `emitAIMVDiagnostic()` 调用。

**前置**: T1.2

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` (修改)

**验收标准**:
- 当循环因代价模型判定向量化不划算被拒绝时，`!aimv.diag` 包含诊断记录
- RemarkID = `"VectorizationNotBeneficial"`

**测试用例**:
- `aimv_cost_reject.ll`: 构造标量代价低于向量代价的循环 IR，验证:
  - 诊断的 remark_id = `"VectorizationNotBeneficial"`
  - scalar_cost < vector_cost

---

### T1.5 插入点 3: UnsafeDep (LoopAccessAnalysis.cpp)

**描述**: 在 `LoopAccessAnalysis.cpp` 的 `emitUnsafeDependenceRemark()` 中插入 `emitAIMVDiagnostic()` 调用。此处无 VF/IC/CM，使用指针链获取 Function/Module。

**前置**: T1.1, T1.2

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/LoopAccessAnalysis.cpp` (修改，+~10 行)

**验收标准**:
- 真正的 UnsafeDep（依赖分析失败）写入 `!aimv.diag`
- VF=0, IC=0, CM=nullptr，cost_data 全部为 -1
- dep_data 包含正确的 DepType 名称（`Backward`/`IndirectUnsafe` 等）

**测试用例**:
- `aimv_unsafe_dep.ll`: 使用有跨迭代 RAW 依赖的循环，验证:
  - remark_id = `"UnsafeDep"`
  - cost_data: scalar_cost = -1, vector_cost = -1（legality 阶段拒绝，无 CM）
  - dep_data 至少一条，dep_type 为 `Backward` 或 `IndirectUnsafe`

---

### T1.6 插入点 4: InterleavingNotBeneficial

**描述**: 在交错不利拒绝点插入 `emitAIMVDiagnostic()` 调用。

**前置**: T1.2

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` (修改)

**验收标准**:
- 当交错化被判定为不划算时，`!aimv.diag` 包含对应记录

**测试用例**:
- `aimv_interleave_not_beneficial.ll`: 验证 remark_id = `"InterleavingNotBeneficial"`

---

### T1.7 插入点 5: LoopVectorized (成功记录)

**描述**: 在 `processLoop()` 成功向量化路径插入 `emitAIMVDiagnostic()` 调用，RemarkID = `"LoopVectorized"`。Driver 依靠此正向记录确认向量化成功。

**前置**: T1.2

**涉及文件**:
- `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` (修改)

**验收标准**:
- 成功向量化的循环在 `!aimv.diag` 中有 `"LoopVectorized"` 记录
- remark_msg 包含 VF 信息

**测试用例**:
- `aimv_vectorized.ll`: 使用简单可向量化的循环（无别名、连续访问），验证:
  - `!aimv.diag` 包含 remark_id = `"LoopVectorized"` 的记录
  - remark_msg 包含 `VF=` 字符串
- `aimv_partial_vectorized.ll`: 一个函数含 2 个循环（1 个可向量化、1 个不可），验证 `!aimv.diag` 同时含 `"LoopVectorized"` 和 `"CantReorderMemOps"` 记录

---

### T1.8 实现 AIMVDiagnosticParser（Metadata → RawDiagnostic）

**描述**: 实现 `AIMVDiagnosticParser.cpp`，将 `!aimv.diag` Named Metadata 解析为 `std::vector<RawDiagnostic>`。

**前置**: T0.3, T1.3

**涉及文件**:
- `llvm/lib/Transforms/AIMV/AIMVDiagnosticParser.cpp` (新建, ~150 行)

**验收标准**:
- 正确解析所有子结构（cost_data, dep_data, memory_data, loop_info）
- 空 `!aimv.diag` 返回空 vector
- 格式错误的 metadata 不崩溃（跳过无效条目）

**测试用例**:
- `aimv_parse_diag.ll`: 构造含已知 `!aimv.diag` 的 IR，调用 `parseDiagnostics()`:
  - 断言返回 1 条 RawDiagnostic
  - 断言 `PassName == "LoopVectorize"`
  - 断言 `Dependencies` 非空，`DepEntry.Type == "Backward"`
- `aimv_parse_empty.ll`: 无 `!aimv.diag` → 返回空 vector
- `aimv_parse_malformed.ll`: 手动构造格式错误的 MDNode → 不崩溃，返回可解析的条目

---

### T1.9 实现 AIMVFeedbackPass::run()

**描述**: 实现 Pass 主逻辑：读取 `!aimv.diag`、筛选当前函数、补充 IR 上下文和源码映射、推断 severity、序列化为 JSON 并写入 `-aimv-output` 指定文件。

**前置**: T1.8, T0.4

**涉及文件**:
- `llvm/lib/Transforms/AIMV/AIMVFeedbackPass.cpp` (新建, ~350 行)

**验收标准**:
- 对有 `!aimv.diag` 的函数，输出 JSON 包含完整的 diagnostics 数组
- JSON 格式与 MCP_DESIGN §3 AnalyzeRequest 的 diagnostics 部分兼容
- severity 推断规则正确（`CantReorderMemOps` → `"missed"`, `LoopVectorized` → `"passed"`）
- 无 `!aimv.diag` 时直接返回，不输出文件
- 同一 Module 多函数时，缓存 `parseDiagnostics()` 结果避免重复解析

**测试用例**:
- `aimv_feedback_json.ll`:
  ```llvm
  ; RUN: opt -passes=loop-vectorize,aimv-feedback -aimv-output=%t.json -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize -S < %s
  ; RUN: FileCheck %s --check-prefix=JSON < %t.json
  ; JSON: "pass_name": "LoopVectorize"
  ; JSON: "severity": "missed"
  ; JSON: "cost_model"
  ; JSON: "dependencies"
  ```
- `aimv_feedback_no_diag.ll`: 无 `!aimv.diag` → 不生成 JSON 文件
- `aimv_feedback_severity.ll`: 验证 severity 推断:
  - `"CantReorderMemOps"` → `"missed"`
  - `"LoopVectorized"` → `"passed"`
  - 未知 RemarkID → `"analysis"`
- `aimv_feedback_multi_func.ll`: 两个函数，验证 JSON 中各自只含对应的诊断
- `aimv_feedback_source_context.ll`: 验证 `source_context` 和 `loop_location` 字段非空

---

### T1.10 命令行参数: -aimv-output, -aimv-enable, -aimv-target-function

**描述**: 在 `clang/lib/CodeGen/BackendUtil.cpp` 中解析新命令行选项，通过 `setOutputPath()`/`setEnabled()`/`setTargetFunction()` 注入 Pass 实例。

**前置**: T1.9, T0.4

**涉及文件**:
- `clang/lib/CodeGen/BackendUtil.cpp` (修改, +~15 行)

**验收标准**:
- `clang -aimv-output=diag.json -fsave-optimization-record=opt.yaml -O2 src.c` 生成 `diag.json`
- `clang -aimv-target-function=foo` 只输出 foo 函数的诊断
- 无 `-aimv-output` 时不生成文件

**测试用例**:
- `test_aimv_flags.sh`:
  ```bash
  # 验证 -aimv-output 生成 JSON
  clang -O2 -fsave-optimization-record=opt.yaml -aimv-output=aimv.json dep_fail_alias.c
  test -f aimv.json && echo "PASS: aimv.json exists"
  ```
- `test_aimv_target_function.sh`:
  ```bash
  # 验证 -aimv-target-function 过滤
  clang -O2 -fsave-optimization-record=opt.yaml -aimv-output=aimv.json -aimv-target-function=foo multi_func.c
  # JSON 中只有 foo 的诊断
  python3 -c "import json; d=json.load(open('aimv.json')); assert all(diag['function_name']=='foo' for diag in d['diagnostics'])"
  ```
- `test_aimv_no_output.sh`: 无 `-aimv-output` → `aimv.json` 不存在

---

## Phase 2: MCP Server

### T2.1 LLM Backend 抽象接口与 OpenAI 实现

**描述**: 实现 `llm/base.py`（`AbstractLLMBackend` ABC）和 `llm/openai_backend.py`（`OpenAIBackend`）。Mock 模式下可跳过真实 LLM 调用。

**前置**: T0.6

**涉及文件**:
- `aimv/mcp-server/llm/base.py` (新建)
- `aimv/mcp-server/llm/openai_backend.py` (新建)

**验收标准**:
- `AbstractLLMBackend` 定义 `analyze()` 和 `health_check()` 抽象方法
- `OpenAIBackend` 可实例化（mock API key），调用 `analyze()` 不崩溃

**测试用例**:
- `test_openai_backend.py`: 使用 `unittest.mock.patch` mock `openai.OpenAI`，验证:
  - `analyze()` 调用 `client.chat.completions.create()` 一次
  - 传入 `response_format={"type":"json_object"}`
  - 返回值可被 `AnalyzeResponse.model_validate()` 校验
- `test_openai_backend.py`: mock 返回非 JSON 文本 → 抛出 `SuggestionParseError`

---

### T2.2 Anthropic 和 DeepSeek Backend 实现

**描述**: 实现 `llm/anthropic_backend.py` 和 `llm/deepseek_backend.py`。DeepSeek 复用 OpenAI SDK（兼容 API），覆盖 base_url。

**前置**: T2.1

**涉及文件**:
- `aimv/mcp-server/llm/anthropic_backend.py` (新建)
- `aimv/mcp-server/llm/deepseek_backend.py` (新建)

**验收标准**:
- 两个 backend 都实现 `analyze()` 和 `health_check()`
- DeepSeek 的 `base_url` = `"https://api.deepseek.com/v1"`

**测试用例**:
- `test_anthropic_backend.py`: mock `anthropic.Anthropic`，验证 `messages.create()` 调用参数
- `test_deepseek_backend.py`: 验证 `base_url` 正确设置
- `test_deepseek_backend.py`: mock 调用，验证与 OpenAI 相同的 JSON 解析流程

---

### T2.3 Prompt 构建器（prompt_builder.py + Jinja2 模板）

**描述**: 实现 `prompt_builder.py`，将 `AnalyzeRequest` 转换为 system prompt + user prompt（Jinja2 模板渲染）。创建 `templates/system_prompt.txt`、`templates/diagnostic_block.j2`、`templates/history_block.j2`。

**前置**: T0.6

**涉及文件**:
- `aimv/mcp-server/prompt_builder.py` (新建)
- `aimv/mcp-server/templates/system_prompt.txt` (新建)
- `aimv/mcp-server/templates/diagnostic_block.j2` (新建)
- `aimv/mcp-server/templates/history_block.j2` (新建)

**验收标准**:
- `build_system_prompt()` 输出包含目标平台信息和规则
- `build_user_prompt()` 输出包含源码、IR、代价模型、依赖信息
- history 模板正确渲染历史轮次

**测试用例**:
- `test_prompt_builder.py`: 用标准 `AnalyzeRequest` fixture 调用:
  - 断言 system prompt 包含 `{target.triple}` 替换后的实际 triple
  - 断言 user prompt 包含 `void process_task` (函数签名)
  - 断言 user prompt 包含 `Backward` (dep_type)
- `test_prompt_builder.py`: 空 history → user prompt 中无 "Previous Attempts" 段
- `test_prompt_builder.py`: 非空 history → user prompt 包含 "Do NOT repeat" 提示
- `test_prompt_builder.py`: `cost_model` 为 None → 输出 "Cost model data not available"

---

### T2.4 建议解析器（suggestion_parser.py）

**描述**: 实现 `parse_structured_response()` 和 `_extract_json()`，处理 LLM 输出的常见格式问题（markdown 代码块包裹、尾部逗号、前缀噪音）。

**前置**: T0.6

**涉及文件**:
- `aimv/mcp-server/suggestion_parser.py` (新建)

**验收标准**:
- 标准 JSON 输入 → 正确解析为 `AnalyzeResponse`
- `` ```json ... ``` `` 包裹 → 正确提取 JSON
- LLM 前缀噪音（"Here is the response: ..."） → 正确跳过
- 无效 JSON → 抛出 `SuggestionParseError`

**测试用例**:
- `test_suggestion_parser.py`: 标准 JSON → 解析成功，`confidence` 在 [0,1]
- `test_suggestion_parser.py`: `` ```json\n{...}\n``` `` → 解析成功
- `test_suggestion_parser.py`: `"Here is the analysis:\n{...}"` → 解析成功
- `test_suggestion_parser.py`: 无效 JSON `"not json"` → 抛出 `SuggestionParseError`
- `test_suggestion_parser.py`: `no_action_possible=true` 且 `suggestions=[]` → 解析成功
- `test_suggestion_parser.py`: `estimated_impact="critical"` → 校验失败（只允许 high/medium/low）

---

### T2.5 诊断指纹缓存（cache.py）

**描述**: 实现 `DiagnosticCache`（内存缓存）和 `compute_diagnostic_fingerprint()`。

**前置**: T0.6

**涉及文件**:
- `aimv/mcp-server/cache.py` (新建)

**验收标准**:
- 相同请求 fingerprint 一致
- 不同源码的请求 fingerprint 不同
- 缓存命中返回缓存的 `AnalyzeResponse`
- TTL 过期后缓存不命中
- 本地缓存超过 10000 条时淘汰最旧条目

**测试用例**:
- `test_cache.py`: 相同请求两次 → `fingerprint` 一致
- `test_cache.py`: `source_code` 不同 → `fingerprint` 不同
- `test_cache.py`: `set()` + `get()` → 返回缓存值
- `test_cache.py`: 过期 TTL（设 0s）→ `get()` 返回 None
- `test_cache.py`: `get_stats()` 返回 `hit_rate` 正确计算

---

### T2.6 集成 analyze-vectorization 端点（完整管线）

**描述**: 将 prompt_builder、LLM backend、suggestion_parser、cache 串联到 `/api/v1/analyze-vectorization` 端点。实现超时处理和错误中间件。

**前置**: T2.3, T2.4, T2.5

**涉及文件**:
- `aimv/mcp-server/aimv_server.py` (修改)
- `aimv/mcp-server/middleware.py` (新建)

**验收标准**:
- 端到端：POST 有效 AnalyzeRequest → 返回 AnalyzeResponse（mock LLM 或真实 LLM）
- 缓存命中时跳过 LLM 调用
- LLM 超时 60s → 返回 `no_action_possible=true`
- API Key 无效 → 返回 401

**测试用例**:
- `test_analyze_endpoint.py`: mock LLM 返回有效 JSON → 200 + AnalyzeResponse
- `test_analyze_endpoint.py`: 二次相同请求 → 缓存命中（检查 LLM 调用次数 = 1）
- `test_analyze_endpoint.py`: mock LLM 超时 → 200 + `no_action_possible=true`
- `test_analyze_endpoint.py`: 无 Authorization header → 401
- `test_analyze_endpoint.py`: 发送 `dep_type="RAW"` → 422（旧格式不再接受）

---

## Phase 3: Driver 脚本

### T3.1 BuildOrchestrator: 编译执行

**描述**: 实现 `build_orchestrator.py` 的 `compile_with_aimv()` 方法，管理 clang 子进程，注入 AIMV flags。

**前置**: T0.5, T1.10

**涉及文件**:
- `aimv/driver/build_orchestrator.py` (新建)

**验收标准**:
- `compile_with_aimv()` 正确组装 clang 命令行
- 编译成功返回 `BuildResult(returncode=0)`
- 编译超时抛出 `subprocess.TimeoutExpired`

**测试用例**:
- `test_build_orchestrator.py`: 用 `dep_fail_alias.c` 编译 → returncode=0
- `test_build_orchestrator.py`: 验证命令行含 `-fsave-optimization-record` 和 `-aimv-output`
- `test_build_orchestrator.py`: 编译不存在的文件 → returncode!=0
- `test_build_orchestrator.py`: mock `subprocess.run` 超时 → 抛出 `TimeoutExpired`

---

### T3.2 BuildOrchestrator: 向量化状态检查

**描述**: 实现 `check_vectorization_from_json()` 和 `check_vectorization_from_yaml()` 双模式检查。

**前置**: T3.1

**涉及文件**:
- `aimv/driver/build_orchestrator.py` (修改)

**验收标准**:
- JSON 模式：正确统计 passed/missed 循环
- YAML 模式：从 opt-record YAML 解析 remark
- 无诊断数据时保守报告

**测试用例**:
- `test_vectorization_check.py`: 构造含 1 passed + 1 missed 的 JSON → `vectorized_loops=1, missed_loops=1`
- `test_vectorization_check.py`: 空 diagnostics → `total_loops=0`
- `test_vectorization_check.py`: 构造含 3 条 loop-vectorize remark 的 YAML → 正确计数
- `test_vectorization_check.py`: `_check_target_loop_passed(target_loop="task.c:42")` → 当 missed_details 不含该位置时返回 True

---

### T3.3 BuildOrchestrator: 测试执行

**描述**: 实现 `run_tests()` 和 `_parse_test_output()`。

**前置**: T3.1

**涉及文件**:
- `aimv/driver/build_orchestrator.py` (修改)

**验收标准**:
- 解析 CTest 输出格式
- 解析 GoogleTest 输出格式
- 未知格式时根据 returncode 判断

**测试用例**:
- `test_parse_output.py`: CTest 格式 `"100% tests passed, 0 tests failed out of 5"` → `(5, 0)`
- `test_parse_output.py`: GoogleTest 格式 `"[  PASSED  ] 3 tests." "[  FAILED  ] 1 test."` → `(3, 1)`
- `test_parse_output.py`: 未知格式 + returncode=0 → `(1, 1)` (保守)

---

### T3.4 SourceManager: 原子 patch + 回滚

**描述**: 实现 `source_manager.py` 的 `apply_patch()`, `rollback()`, `rollback_all()`, `get_current_diff()`。

**前置**: T0.5

**涉及文件**:
- `aimv/driver/source_manager.py` (新建)

**验收标准**:
- `apply_patch()` 成功应用 unified diff 并创建备份
- `rollback()` 从备份恢复原始文件（sha256 校验通过）
- patch 失败时自动恢复，源文件不变
- `rollback_all()` 按反序回滚所有 patch

**测试用例**:
- `test_source_manager.py`: 创建临时文件，`apply_patch()` 添加一行 → 验证文件内容变更
- `test_source_manager.py`: `rollback()` → 验证文件恢复原状且 sha256 匹配
- `test_source_manager.py`: 格式错误的 diff → `apply_patch()` 抛异常且文件未变
- `test_source_manager.py`: 连续 3 次 `apply_patch()` + `rollback_all()` → 文件恢复最初状态
- `test_source_manager.py`: `get_current_diff()` 返回最近一次 diff

---

### T3.5 MCPClient

**描述**: 实现 `mcp_client.py`，含重试逻辑、超时处理、健康检查。

**前置**: T0.5

**涉及文件**:
- `aimv/driver/mcp_client.py` (新建)

**验收标准**:
- 200 响应返回解析后的 JSON
- 422 响应抛出 ValueError（不可重试）
- 429/500/502/503 指数退避重试最多 3 次
- 超时重试

**测试用例**:
- `test_mcp_client.py`: mock 200 → 返回 JSON dict
- `test_mcp_client.py`: mock 连续 2 次 503 + 第 3 次 200 → 重试后成功
- `test_mcp_client.py`: mock 连续 3 次 503 → 返回 None
- `test_mcp_client.py`: mock 422 → 抛出 ValueError
- `test_mcp_client.py`: mock timeout → 重试 3 次后返回 None
- `test_mcp_client.py`: `health()` mock 200 → True

---

### T3.6 IterationEngine 决策引擎

**描述**: 实现 `iteration_engine.py`，含完整决策矩阵（向量化成功/轮次上限/编译失败/测试失败/MCP 无响应/MCP 无建议/性能退化）。

**前置**: T0.5

**涉及文件**:
- `aimv/driver/iteration_engine.py` (新建)

**验收标准**:
- 每种场景返回正确的 `(NextAction, reason)` 元组
- 编译失败计数器正确重置
- 激进度升级顺序 conservative→moderate→aggressive

**测试用例**:
- `test_iteration_engine.py`: `vectorized=True` → `(STOP, "vectorization succeeded")`
- `test_iteration_engine.py`: `current_round >= max_rounds` → `(STOP, "reached max rounds")`
- `test_iteration_engine.py`: `build_result_ok=False` → `(ROLLBACK, ...)`；连续 2 次 → `(STOP, ...)`
- `test_iteration_engine.py`: `test_result_ok=False` → `(ROLLBACK, ...)`（立即停止）
- `test_iteration_engine.py`: `mcp_responded=False` → `(STOP, "MCP server unresponsive")`
- `test_iteration_engine.py`: `mcp_had_suggestions=False` → `(ESCALATE_LEVEL, ...)`；已是 aggressive → `(STOP, ...)`
- `test_iteration_engine.py`: `perf_delta_pct=-10` (退化超阈值) → `(ROLLBACK, ...)`
- `test_iteration_engine.py`: `perf_delta_pct=-2` (退化未超阈值) → `(CONTINUE, ...)`

---

### T3.7 SessionStore 持久化

**描述**: 实现 `session_store.py`，含原子写入、加载、列出 sessions、崩溃恢复支持。

**前置**: T0.5

**涉及文件**:
- `aimv/driver/session_store.py` (新建)

**验收标准**:
- `save()` 原子写入（tmp + rename）
- `load()` 正确反序列化嵌套 dataclass
- `list_sessions()` 返回按时间倒序的 session 摘要列表

**测试用例**:
- `test_session_store.py`: `save()` + `load()` → 数据一致
- `test_session_store.py`: `save()` 中途崩溃（删除 .tmp）→ `load()` 返回 None（旧文件保留）
- `test_session_store.py`: 多次 `save()` → 文件内容始终为最新
- `test_session_store.py`: `list_sessions()` 返回正确数量和排序
- `test_session_store.py`: 验证 Enum 类型正确序列化为字符串值

---

### T3.8 源码提取辅助函数

**描述**: 实现 `extract_function_source()`, `extract_function_signature()`, `extract_loop_line()`, `extract_lines_around()` 四个 helper 函数。Main loop 在构造 MCP 请求的 `function` 字段时需要这些函数（仅发送目标函数的源码片段，而非整个文件）。

**前置**: T0.5

**涉及文件**:
- `aimv/driver/aimv_driver.py` (修改，+~60 行)

**验收标准**:
- `extract_function_source()` 正确提取指定函数的源码文本
- 提取失败时 `extract_lines_around()` 回退截取 loop 前后 N 行
- `extract_loop_line()` 正确解析 `"file.c:42:5"` 格式的行号

**测试用例**:
- `test_function_extraction.py`: 构造含 2 个函数的 C 文件:
  ```c
  void foo(int n) { for(int i=0;i<n;i++) a[i]=b[i]; }
  int bar(int x) { return x+1; }
  ```
  `extract_function_source("test.c", "foo")` → 返回 `foo` 的函数体
- `test_function_extraction.py`: 不存在的函数名 → 返回 None
- `test_function_extraction.py`: `extract_loop_line([{"loop_location":"task.c:42:5"}], ...)` → 返回 42
- `test_function_extraction.py`: 空 diagnostics → 返回 0
- `test_function_extraction.py`: `extract_lines_around("test.c", 42, context=10)` → 返回第 32-52 行
- `test_function_extraction.py`: 行号超出文件范围 → 返回文件末尾附近行

---

### T3.9 Driver 主循环实现

**描述**: 实现 `aimv_driver.py` 的 `main_loop()`，串联所有模块（BuildOrchestrator → MCPClient → SourceManager → IterationEngine → SessionStore），实现完整的编译→分析→patch→验证迭代循环。

**前置**: T3.1-T3.8

**涉及文件**:
- `aimv/driver/aimv_driver.py` (修改，补全主循环)

**验收标准**:
- 单轮闭环可运行：编译→诊断→MCP→patch→重编译
- 向量化成功时终止，session 记录 `termination_reason=VECTORIZED`
- 编译失败时回滚
- 测试失败时回滚并终止
- Ctrl+C 时回滚所有 patch

**测试用例**:
- `test_main_loop.py`: mock 所有子模块，模拟 1 轮向量化成功 → 返回 0
- `test_main_loop.py`: mock MCP 返回 `no_action_possible=true` → 返回 1
- `test_main_loop.py`: mock 编译失败 → SourceManager.rollback 被调用
- `test_main_loop.py`: mock 测试失败 → SourceManager.rollback 被调用 + 终止
- `test_main_loop.py`: mock 连续 5 轮未成功 → 返回 1（ROUND_LIMIT）
- `test_main_loop.py`: 模拟 KeyboardInterrupt → rollback_all 被调用

---

### T3.10 OptInfoParser (YAML 模式支持)

**描述**: 实现 `opt_info_parser.py`，解析 `-fsave-optimization-record` 产出的 YAML/JSON opt-records，转换为 `AnalyzeRequest` 兼容格式。

**前置**: T0.5

**涉及文件**:
- `aimv/driver/opt_info_parser.py` (新建)

**验收标准**:
- 正确解析 YAML remark 格式
- 转换为 `AnalyzeRequest` 的 diagnostics 部分
- 无 loop-vectorize remark 时返回空 diagnostics

**测试用例**:
- `test_opt_info_parser.py`: 构造含 1 条 missed remark 的 YAML → 返回 1 条 diagnostic
- `test_opt_info_parser.py`: 构造无 loop-vectorize remark 的 YAML → 空 diagnostics
- `test_opt_info_parser.py`: 验证 `severity` 从 remark `type` 字段正确映射（`"Missed"` → `"missed"`）
- `test_opt_info_parser.py`: 验证 `loop_location` 从 `DebugLoc` 字段提取

---

### T3.11 FileLock 并发控制

**描述**: 实现 `source_manager.py` 中的 `FileLock` 类（跨进程文件锁，基于 `fcntl.flock`）。

**前置**: T3.4

**涉及文件**:
- `aimv/driver/source_manager.py` (修改)

**验收标准**:
- 同一文件只能被一个进程锁定
- 超时未获取返回 False
- `release()` 释放锁

**测试用例**:
- `test_file_lock.py`: 同进程 acquire + release → 成功
- `test_file_lock.py`: 子进程持有锁 → 父进程 acquire 超时 → False
- `test_file_lock.py`: acquire 后 release → 再次 acquire 成功

---

## Phase 4: MVP 集成验证

### T4.1 端到端联调: Pass + Driver + MCP Server

**描述**: 用 T0.7 的 `dep_fail_alias.c` benchmark，启动 MCP Server（mock LLM 或真实 LLM），运行 `aimv-driver`，验证完整闭环。

**前置**: T1.10, T2.6, T3.9

**涉及文件**:
- 无新文件，修改已有代码中的集成问题

**验收标准**:
- `aimv-driver --function=process_task dep_fail_alias.c` 成功运行
- Session JSON 包含完整的 RoundRecord
- MCP Server 日志显示收到请求并返回建议

**测试用例**:
- `test_e2e_dep_fail_alias.py`: 自动化脚本:
  1. 启动 MCP Server（mock LLM 固定返回 `restrict` 建议）
  2. 运行 `aimv-driver --function=... dep_fail_alias.c`
  3. 断言 session termination_reason = VECTORIZED
  4. 断言修改后的源码包含 `restrict`
  5. 断言重编译后无 missed remark

---

### T4.2 端到端: dep_fail_stride.c

**描述**: 用 `dep_fail_stride.c` benchmark 验证跨迭代依赖场景。

**前置**: T4.1

**验收标准**: 同 T4.1

**测试用例**:
- `test_e2e_dep_fail_stride.py`: 类似 T4.1 的自动化测试

---

### T4.3 端到端: YAML 模式验证

**描述**: 使用 `--mode=yaml`（不依赖 AIMVFeedbackPass）运行 driver，验证零侵入模式可用。

**前置**: T3.10, T4.1

**验收标准**:
- YAML 模式下能完成至少 1 轮迭代
- 诊断信息少于 Pass 模式（无代价模型和依赖分析数据）

**测试用例**:
- `test_e2e_yaml_mode.py`: 使用 YAML 模式运行，断言:
  - 成功读取 opt-record YAML
  - MCP 收到的 diagnostics 无 `cost_model` 和 `dependencies` 字段
  - 至少完成 1 轮迭代

---

### T4.4 Prompt 精度调优

**描述**: 根据实际 LLM 响应质量，调整 system prompt 模板和诊断上下文格式。目标是提高 `restrict` 建议的准确率和 diff 格式的正确率。

**前置**: T4.1

**涉及文件**:
- `aimv/mcp-server/templates/system_prompt.txt` (修改)
- `aimv/mcp-server/templates/diagnostic_block.j2` (修改)

**验收标准**:
- Mock 测试中 LLM 返回的建议 diff 格式正确率 ≥ 80%
- LLM 返回的建议与诊断信息一致

**测试用例**:
- `test_prompt_quality.py`: 收集 5 个 benchmark 的诊断 JSON，手动检查 LLM 输出:
  - diff 格式是否为有效 unified diff
  - 建议是否与失败原因一致（别名分析失败 → restrict 建议）
  - JSON schema 校验是否通过

---

### T4.5 性能测量模块

**描述**: 实现 `perf_measurer.py`，使用 `perf stat` 或 `clock_gettime()` 测量执行时间，计算中位数和加速比。

**前置**: T3.9

**涉及文件**:
- `aimv/driver/perf_measurer.py` (新建)

**验收标准**:
- 测量结果稳定（3 次运行标准差 < 5%）
- 正确计算中位数

**测试用例**:
- `test_perf_measurer.py`: 测量简单循环程序的执行时间 → 结果 > 0
- `test_perf_measurer.py`: 连续测量 10 次 → 标准差 < 5%

---

### T4.6 MVP Benchmark 验证与测试报告

**描述**: 使用全部 5 个 benchmark 运行完整 AIMV 流程，生成测试报告。验证 SPEC §8 成功度量（中位数执行时间缩短 ≥ 5%）。

**前置**: T4.1-T4.5

**涉及文件**:
- `aimv/benchmarks/run_bench.sh` (新建)
- `aimv/benchmarks/analyze_results.py` (新建)

**验收标准**:
- 至少 3 个 benchmark 通过 AIMV 成功向量化
- 成功向量化的 benchmark 性能有可测量提升
- 测试报告包含每个 benchmark 的基线和优化后性能数据

**测试用例**:
- `test_mvp_benchmarks.py`: 对每个 benchmark:
  1. 编译原始代码，记录 missed remark
  2. 运行 AIMV
  3. 编译修改后代码，验证无 missed remark 或 remark 变为 passed
  4. 断言性能提升 ≥ 5%

---

## Phase 5: 诊断维度扩展

### T5.1 代价模型拒绝维度的 Prompt 模板

**描述**: 创建针对 `VectorizationNotBeneficial` 的专用 prompt 模板，引导 LLM 建议 `#pragma clang loop` 指令、循环结构调整等。

**前置**: T4.1

**涉及文件**:
- `aimv/mcp-server/templates/cost_reject_prompt.txt` (新建)
- `aimv/mcp-server/prompt_builder.py` (修改，增加代价模型专用分支)

**验收标准**:
- `VectorizationNotBeneficial` 诊断使用专用 prompt
- LLM 返回 `#pragma` 或循环结构调整建议

**测试用例**:
- `test_cost_reject_prompt.py`: 构造 `cost_reject.c` 的诊断 JSON，调用 prompt_builder:
  - 断言 prompt 包含 "cost model" 和 "pragma" 关键词
- `test_e2e_cost_reject.py`: 用 `cost_reject.c` 端到端运行，断言建议包含 pragma 或结构调整

---

### T5.2 内存/对齐维度的 Prompt 模板

**描述**: 创建针对对齐未知场景的专用 prompt 模板，引导 LLM 建议 `alignas`/`__builtin_assume_aligned` 等。

**前置**: T4.1

**涉及文件**:
- `aimv/mcp-server/templates/align_prompt.txt` (新建)
- `aimv/mcp-server/prompt_builder.py` (修改)

**验收标准**:
- 对齐相关诊断使用专用 prompt
- LLM 返回 `alignas` 或 `__builtin_assume_aligned` 建议

**测试用例**:
- `test_align_prompt.py`: 构造 `align_unknown.c` 诊断 JSON:
  - 断言 prompt 包含 "alignment" 和 "alignas" 关键词
- `test_e2e_align_unknown.py`: 用 `align_unknown.c` 端到端运行，断言建议包含对齐修饰符

---

### T5.3 三维度综合验证

**描述**: 用 `multi_fail.c`（混合依赖+代价失败）验证多维度诊断和多轮迭代能力。

**前置**: T5.1, T5.2

**验收标准**:
- `multi_fail.c` 经过多轮 AIMV 迭代后至少部分循环被向量化
- 每轮建议与当前诊断维度一致

**测试用例**:
- `test_e2e_multi_fail.py`: 端到端运行，断言:
  - Session 至少 2 轮迭代
  - 每轮的建议针对不同的失败维度
  - 最终至少 1 个循环被向量化

---

## Phase 6: CI 集成与循环变换（扩展阶段）

### T6.1 变更检测工具 (aimv-detect-changes)

**描述**: 实现 CI_DESIGN §2 的 `change_detector.py`，通过 `git diff` + AST dump 识别变更函数。

**前置**: T3.9

**涉及文件**:
- `aimv/ci/change_detector.py` (新建)
- `aimv/ci/aimv_detect_changes.py` (新建，CLI 入口)

**验收标准**:
- 给定两个分支，正确识别变更的 C 文件和其中的函数
- 无变更时返回空列表

**测试用例**:
- `test_change_detector.py`: 创建 git 仓库，修改一个函数 → 检测到该函数
- `test_change_detector.py`: 只修改注释 → 不检测到函数变更
- `test_change_detector.py`: 新增文件 → 检测到新文件中的所有函数

---

### T6.2 批量分析工具 (aimv-run-batch)

**描述**: 实现 CI_DESIGN §4 的 `aimv_run_batch.py`，支持跨文件并行、同文件串行。

**前置**: T6.1, T3.11

**涉及文件**:
- `aimv/ci/aimv_run_batch.py` (新建)

**验收标准**:
- 同文件函数串行执行
- 不同文件函数并行执行
- 全局并行度受 `--parallel` 控制

**测试用例**:
- `test_run_batch.py`: 2 个文件各 1 个函数 → 并行执行，总耗时 ≈ 单次耗时
- `test_run_batch.py`: 1 个文件 2 个函数 → 串行执行
- `test_run_batch.py`: `--parallel=1` → 全部串行

---

### T6.3 报告生成工具 (aimv-report)

**描述**: 实现 `aimv_report.py`，从 session JSON 生成 Markdown/GitLab 报告。

**前置**: T4.6

**涉及文件**:
- `aimv/ci/aimv_report.py` (新建)
- `aimv/ci/templates/report_markdown.j2` (新建)

**验收标准**:
- Markdown 报告包含汇总表格和每个函数的详情
- Jinja2 模板渲染不报错

**测试用例**:
- `test_report.py`: 3 个 session JSON → Markdown 包含 3 个函数
- `test_report.py`: 空结果 → Markdown 包含 "0 functions analyzed"

---

### T6.4 门禁决策工具 (aimv-gate)

**描述**: 实现 CI_DESIGN §6 的门禁决策逻辑（report/regression/enforce 三级）。

**前置**: T6.2

**涉及文件**:
- `aimv/ci/aimv_gate.py` (新建)
- `aimv/ci/gate_config.yaml` (新建)

**验收标准**:
- report 模式始终放行
- regression 模式检测性能退化
- enforce 模式检查最低向量化率

**测试用例**:
- `test_gate.py`: report 模式 → `allow=True`
- `test_gate.py`: regression 模式 + 无退化 → `allow=True`
- `test_gate.py`: regression 模式 + 退化 10% → `allow=False`
- `test_gate.py`: enforce 模式 + 覆盖率 40% < 阈值 60% → `allow=False`
- `test_gate.py`: enforce 模式 + 覆盖率 70% ≥ 阈值 60% → `allow=True`

---

### T6.5 GitHub Actions Workflow 配置

**描述**: 创建 `.github/workflows/aimv-analysis.yml`。

**前置**: T6.2, T6.3

**涉及文件**:
- `.github/workflows/aimv-analysis.yml` (新建)

**验收标准**:
- PR 触发时自动运行
- 输出 MR 评论和 artifact

**测试用例**:
- 手动触发验证 workflow 语法正确：`actionlint .github/workflows/aimv-analysis.yml`
- Mock 测试：push 后 workflow 被触发（需实际 GitHub 环境）

---

### T6.6 循环变换 Prompt 模板 (Phase 3 扩展)

**描述**: 创建针对循环变换（unroll, fusion/fission, interchange）的 prompt 模板和 `POST /api/v1/analyze-loop-transform` 端点。

**前置**: T5.3

**涉及文件**:
- `aimv/mcp-server/aimv_server.py` (修改)
- `aimv/mcp-server/templates/loop_transform_prompt.txt` (新建)

**验收标准**:
- 新端点接收循环变换请求
- LLM 返回循环变换建议

**测试用例**:
- `test_loop_transform_endpoint.py`: 构造嵌套循环诊断 → 200 + 建议包含 interchange
- `test_loop_transform_endpoint.py`: 不适合变换的循环 → `no_action_possible=true`

---

## 任务依赖图

```
Phase 0 (基础设施):
  T0.1 ─┬─ T0.5 ─┬─ T3.1, T3.4, T3.5, T3.6, T3.7, T3.8, T3.10
        │         └─ T3.9 (依赖 T3.1-T3.8)
        ├─ T0.6 ─── T2.1-T2.5 ─── T2.6
        └─ T0.7 ─── T4.1
  T0.2 ── T0.3 ── T0.4 ── T1.1 ── T1.2 ─┬─ T1.3, T1.4, T1.5, T1.6, T1.7
                                          └─ T1.8 ── T1.9 ── T1.10

Phase 4 (MVP 集成):
  T1.10 + T2.6 + T3.9 ─── T4.1 ─┬─ T4.2
                                  ├─ T4.3 (YAML 模式)
                                  ├─ T4.4 (Prompt 调优)
                                  └─ T4.5 ── T4.6

Phase 5 (维度扩展):
  T4.1 ── T5.1 ─┐
  T4.1 ── T5.2 ─┴── T5.3

Phase 6 (CI + 循环变换):
  T3.9 ─── T6.1 ─── T6.2 ─── T6.3, T6.4, T6.5
  T5.3 ─── T6.6
```

---

## 统计

| 阶段 | 任务数 | 关键测试用例数 |
|------|--------|---------------|
| Phase 0 | 7 | ~25 |
| Phase 1 | 10 | ~30 |
| Phase 2 | 6 | ~20 |
| Phase 3 | 11 | ~40 |
| Phase 4 | 6 | ~12 |
| Phase 5 | 3 | ~5 |
| Phase 6 | 6 | ~10 |
| **合计** | **49** | **~142** |

---

*文档版本: 1.0*
*创建日期: 2026-05-14*
