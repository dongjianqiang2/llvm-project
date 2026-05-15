# AIMV (AI Multi-Level Vectorization) — 需求规格说明书

## 1. 项目概述

**项目名称**: AIMV (AI Multi-Level Vectorization)
**项目类型**: AI 驱动的编译优化反馈系统
**核心功能**: 通过编译器 opt-info 诊断信息 + 远程 MCP 大模型服务，形成 "编译→失败诊断→AI分析→源码修改→重编译" 闭环迭代，实现函数级代码的精准自动向量化
**目标用户**: 嵌入式/HPC 开发者，需要将遗留 C/C++ 代码自动向量化加速

核心思想：将 LLVM 的 optimization remarks（opt-info）作为大模型的 "眼睛"，让 AI 理解向量化失败的根因，反向映射到源码层给出可验证的修改建议，并通过编译+测试闭环确保正确性。

---

## 2. 核心工作流

```
用户视角 (透明模式):
  clang -O2 -faimv -c task.c -o task.o
       │
       ▼
  ┌──────────────────────────────────────────────────────────┐
  │               aimv-clang (clang 包装器)                   │
  │                                                          │
  │  ┌──────────┐   ┌──────────────┐   ┌──────────────┐      │
  │  │1.真clang │──▶│2.AIMV Pass   │──▶│3.MCP Server  │      │
  │  │  编译    │   │  !aimv.diag  │   │  AI 分析      │      │
  │  └──────────┘   │  → JSON      │   └──────────────┘      │
  │                 └──────────────┘         │               │
  │       ▲                                  │               │
  │       │      ┌──────────────┐           │               │
  │       └──────│5.重编译+测试  │◀──4.源码  │               │
  │   (最多N轮)  │  验证         │   patch   │               │
  │              └──────────────┘           │               │
  └──────────────────────────────────────────────────────────┘
  
  最终返回:  成功 → clang exit 0 + 源码已修改 + task.c.aimv.patch
             放弃 → clang 原始 exit code + 回滚 + 诊断报告
```

**迭代终止条件（组合策略）**:

1. 函数成功向量化即停止（检测到 LoopVectorize pass 成功生成向量指令）
2. 到达最大迭代轮次上限（默认 5 轮，可配置）
3. 收益不增时回滚（新策略导致已向量化循环退化则回退该变更）

---

## 3. 系统形态

### 3.1 主入口：透明 clang 包装器（推荐）

用户无需改变构建习惯。将 `aimv-clang` 置于 PATH 中的 `clang` 位置：

```bash
clang -O2 -faimv -c task.c -o task.o
#                    ^^^^^^ 唯一新增的 flag
```

| 产物 | 说明 |
|------|------|
| 源文件（原地修改） | 编译通过后写入 restrict/alignas 等 hint |
| `task.c.aimv.patch` | 每轮迭代的 unified diff 日志 |
| `aimv-output/session.json` | 完整迭代记录（诊断→AI→验证） |

`-faimv` 被 clang driver 识别，转发给 LLVM 后端激活 `!aimv.diag` 写入和 `AIMVFeedbackPass`。

### 3.2 开发者入口（需要精细控制时）

| 形态 | 说明 | 适用场景 |
|------|------|---------|
| `aimv-driver` | Python 脚本：编译→MCP→patch→重编译，支持 `--require-review` / `--dry-run` | 手动审查、CI 集成 |
| `opt -passes=loop-vectorize,aimv-feedback` | LLVM Pass 独立运行，输出 JSON 诊断 | 调试、离线分析 |
| `aimv-analyze` | 解析 YAML/JSON opt-records 离线批量分析 | 团队共享报告 |

---

## 4. 用户交互模式

### 4.1 修改层级：C/C++ 源码层

大模型分析 IR 层向量化失败原因后，**反向映射到源码**，生成源码级修改建议供开发者 review。

**反向映射要求**:

- 利用 LLVM debug metadata（`!dbg`）将 IR 指令精确映射回源码行号
- 诊断包携带足够上下文（源码片段 + 对应 IR），让 AI 在源码层面理解问题
- 无法精确映射时标注 "approximate match"

### 4.2 修改激进度（三级开关可配置）

```
保守 (--aimv-level=conservative)
├── 添加 restrict / const 限定符
├── 添加 alignas / __builtin_assume_aligned
├── 添加 #pragma clang loop vectorize(enable)
└── 使用 __builtin_assume 提示取值范围

适中 (--aimv-level=moderate)
├── 以上所有 +
├── Loop fission / distribution（拆分依赖循环）
├── Loop interchange（交换嵌套循环顺序）
├── 局部变量提升（将全局 load 提升到循环外）
└── 标量扩展 / 规约变量调整

激进 (--aimv-level=aggressive)
├── 以上所有 +
├── 数据结构调整（AoS → SoA 转换建议）
├── 算法替换（查表替代 switch、条件化简）
└── 需开发者逐项确认
```

### 4.3 精准性保障（多层保险）

```
AI 建议 ──▶ 开发者 review ──▶ 编译验证 ──▶ 测试套件 ──▶ [可选] Alive2 语义验证
 (增量 diff)   (逐个确认)      (无报错)       (全部通过)      (IR等价)
```

- 每轮只建议**一个局部变更**，生成带解释的 diff
- 编译通过 + 原测试套件全绿
- **Alive2 语义验证**（推荐启用，MVP 阶段至少对 restrict/alias 类变更验证）:
  - 编译优化前后 IR → `alive-tv <old.ll> <new.ll>`
  - 仅验证变更函数，不跑全量 IR
  - Alive2 超时（>30s per function）则跳过 + 标记 "unverified"
  - 验证失败 → 拒绝 patch + 回滚，记录反例供 prompt 优化

---

## 5. MCP AI 分析服务

### 5.1 部署形态

**远程云端服务**（如 GPT/DeepSeek 等强模型），编译客户端通过网络调用：

- 中心化部署，团队共享分析历史
- 可选缓存层：相同诊断模式不重复请求

### 5.2 通信协议

**JSON/OpenAPI REST**：

```
POST /api/v1/analyze-vectorization
Content-Type: application/json

{
  "request_id": "uuid",
  "target": {
    "triple": "armv7-unknown-linux-gnueabi",
    "cpu": "cortex-a9",
    "vector_width": 128
  },
  "function": {
    "name": "process_task",
    "signature": "void process_task(int * restrict a, int n)",
    "source_code": "...",
    "source_file": "task.c",
    "loop_line": 42
  },
  "diagnostics": [
    {
      "pass_name": "LoopVectorize",
      "remark_id": "CantReorderMemOps",
      "remark_text": "loop not vectorized: unsafe dependent memory operations in loop",
      "severity": "missed",
      "cost_model": { "scalar_cost": 24, "vector_cost": 38, "vf": 4, "interleave_count": 1 },
      "dependencies": [...],
      "source_context": "...",
      "ir_snippet": "..."
    }
  ],
  "history": [...]
}
```

### 5.3 诊断信息包（每轮发送给大模型）

| 必须字段              | 说明                                                                                         |
| ----------------- | ------------------------------------------------------------------------------------------ |
| **opt-info 失败文本** | LLVM optimization remark 完整文本（如 `loop not vectorized: unsafe dependent memory operations`） |
| **VPlan 代价模型拆解**  | 总标量代价 vs 向量代价、VF、interleave count（MVP 不含逐指令明细，延后到 Phase 2）                      |
| **失败循环源码 + 行号**   | 原始 C/C++ 源码片段及精确行号映射                                                                       |
| **对应的 LLVM IR**   | 优化前后的 IR 片段（含 `!dbg` 调试行号）                                                                 |

---

## 6. 目标诊断维度（首期聚焦）

| 维度          | 关键诊断项                             | 典型 AI 建议                                      |
| ----------- | --------------------------------- | --------------------------------------------- |
| **依赖分析失败**  | 指针别名不可判、数组下标 SCEV 不可推断、跨迭代读后写/写后读 | 加 `restrict`、`__builtin_assume`、循环 fission 改写 |
| **代价模型拒绝**  | VF*向量代价 > 标量代价、向量指令代价估计过高         | 调整循环结构、减少控制流、添加 `#pragma clang loop`          |
| **内存/对齐问题** | 非连续访存、对齐未知、stride≠1、TLI 不支持       | 加 `alignas`、`__builtin_assume_aligned`、数据重排   |

---

## 7. MVP 范围

**目标**: 自动化单 Pass 闭环 —— 针对**依赖分析失败**场景，实现：

```
编译 → 解析 opt-info 发现 "unsafe dependent memory operations"
     → 构造 REST 请求发 MCP 大模型
     → AI 建议：添加 restrict 限定符
     → 自动 patch 源码
     → 重编译验证
     → 向量化成功 OR 回退 + 报告
```

**MVP 交付物**:

1. `aimv-driver` Python 脚本（编排编译-诊断-MCP-patch 流程）
2. `AIVectorizeFeedback` LLVM Pass（收集 opt-info 诊断信息）
3. MCP REST API mock/实现（接收诊断、返回建议）
4. 1 个已知失败 benchmark 的端到端 demo

---

## 8. 成功度量

### 8.1 测量方法

**工具**: Linux `perf stat`（PMU 计数器: cycles, instructions, branches, branch-misses）或 `clock_gettime()` 高精度计时。

**策略**: 预热 3 次后取 10 次运行的**中位数**（排除首轮冷启动和末轮噪声）。

**基线**: 优化前原始代码在 `-O2` 下的中位数执行时间。

**合格阈值**: 中位数执行时间缩短 ≥ **5%** 且所有测试无回归。不只看向量化 "覆盖率"，要验证实际加速比。

### 8.2 验证 Benchmark 集（首期）

从 `aimv/benchmarks/` 中选取至少 5 个已知有向量化失败场景的 C 文件：

| Benchmark | 失败类型 | 目标 |
|-----------|---------|------|
| `dep_fail_alias.c` | 别名分析失败 (CantReorderMemOps) | restrict 修复后向量化 |
| `dep_fail_stride.c` | 跨迭代 RAW 依赖 | loop fission 或 restrict |
| `cost_reject.c` | 代价模型拒绝 | pragma 或结构调整 |
| `align_unknown.c` | 对齐未知 | alignas 修复 |
| `multi_fail.c` | 混合失败（依赖+代价） | 多轮迭代 |

### 8.3 可操作性要求

- 每个 benchmark 附带 `Makefile` 或编译脚本，明确 compiler flags
- 附带计时 wrapper（`perf_runner.sh`），输出稳定可比的执行时间
- CI 中可运行: `make -C benchmarks && ./run_bench.sh && python3 analyze_results.py`

---

## 9. 扩展路线图

| 阶段               | 内容                                                      | 依赖             |
| ---------------- | ------------------------------------------------------- | -------------- |
| **Phase 1 (当前)** | 向量化单 Pass 闭环（依赖分析）                                      | MVP 完成         |
| **Phase 2**      | 覆盖代价模型 + 内存/对齐 两个诊断维度                                   | MCP 服务稳定       |
| **Phase 3**      | 循环变换组合（unroll, fusion/fission, interchange），多 pass 协同分析 | Phase 1-2 验证有效 |

---

## 10. 非功能需求

| 类别  | 需求                                   |
| --- | ------------------------------------ |
| 可靠性 | 所有 AI 建议必须编译+测试验证通过才接受，不引入回归         |
| 可追溯 | 每轮迭代记录：诊断输入、AI 回复、应用 patch、验证结果      |
| 可回滚 | 任一修改导致编译失败或性能退化时自动回退                 |
| 延迟  | MCP 分析延迟可接受（编译期异步调用，不阻塞整体构建）         |
| 日志  | 完整记录每轮迭代的诊断信息、AI 请求/响应、patch 内容、验证结果 |

---

## 11. 关键设计决策

| 决策点              | 选择                          | 理由                         |
| ---------------- | --------------------------- | -------------------------- |
| 修改层级             | C/C++ 源码层                   | 开发者可 review，可维护，不引入 IR 层魔数 |
| MCP 协议           | JSON REST API               | 最通用，调试友好，易与多种大模型平台对接       |
| MCP 部署           | 远程云端服务                      | 使用最强模型能力，支持团队共享分析历史        |
| 终止策略             | 成功+轮次+收益三合一                 | 兼顾有效性、安全性、效率               |
| 工具形态             | Driver 脚本 + Pass + 独立工具     | 覆盖不同使用场景和集成深度              |
| 精准性              | 多层保险（review+编译+测试+可选Alive2） | 源代码变更风险高，需多重验证             |
| MVP              | 依赖分析 → restrict 单步闭环        | 最频繁的失败原因，修改最小，风险最低         |
| 成功度量             | 性能提升绝对值                     | 向量化覆盖率是过程指标，执行时间缩短才是目的     |
| 与 EmbeddedJIT 关系 | 完全独立                        | 不同系统，隔离演进，不互相耦合            |

---

## 12. 术语表

| 术语       | 定义                                                             |
| -------- | -------------------------------------------------------------- |
| opt-info | LLVM optimization remarks，编译器输出的优化诊断信息（passed/missed/analysis） |
| MCP      | Model Context Protocol，AI 服务通信协议                               |
| VPlan    | LLVM Loop Vectorizer 的计划表示，包含代价评估                              |
| VF       | Vectorization Factor，向量化因子                                     |
| SCEV     | Scalar Evolution，LLVM 的循环归纳变量分析                                |
| TLI      | Target Library Info，目标平台库信息                                    |
| Alive2   | LLVM IR 翻译验证工具，可证明 IR 变换的语义等价性                                 |

---

*文档版本: 1.0*
*创建日期: 2026-04-29*
