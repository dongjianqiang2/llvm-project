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
用户视角:
  clang -O2 -faimv -c task.c -o task.o
       │
       ▼
  ┌──────────────────────────────────────────────────────────┐
  │                   clang Driver (C++)                     │
  │                                                          │
  │  ┌──────────┐   ┌──────────────┐                        │
  │  │1.编译    │──▶│2.AIMV Pass   │                        │
  │  │ +AIMV    │   │  !aimv.diag  │   aimv.json 产出       │
  │  │ flags    │   │  → JSON      │                        │
  │  └──────────┘   └──────┬───────┘                        │
  │                        │                                │
  │                        ▼ fork+exec                      │
  │               ┌──────────────────┐                      │
  │               │ 3. aimv-driver   │                      │
  │               │   (Python 编排)   │                      │
  │               │                  │                      │
  │               │ MCP → AI → patch │                      │
  │               │ → 重编译 → 测试   │                      │
  │               └──────────────────┘                      │
  └──────────────────────────────────────────────────────────┘

  最终返回:  成功 → clang exit 0 + 源码已修改 + task.c.aimv.patch
             放弃 → 回滚源码 + 报告路径输出到 stderr
```

**迭代终止条件（组合策略）**:

1. 目标函数内**所有循环**成功向量化即停止（`aimv-driver` 检测到该函数在最新 aimv.json 中无 `severity=="missed"` 的诊断记录）
2. 到达最大迭代轮次上限（默认 5 轮，可配置）
3. 收益退化时回滚（编译期判定：新 patch 后 `LoopVectorize` passed remark 数量**减少**则回滚。持平不触发回滚。运行期性能验证（`aimv-bench`）为 Phase 2 预留功能，不在 MVP 闭环内）

---

## 3. 系统形态

### 3.1 主入口：clang Driver 原生集成

用户零感知。`-faimv` 由 clang Driver 原生解析，编译后自动调用 `aimv-driver` 完成闭环：

```bash
clang -O2 -faimv -c task.c -o task.o
#                    ^^^^^^ 唯一新增的 flag
```

```
clang Driver (C++)                     aimv-driver (Python)
     │                                       │
     ├─ 解析 -faimv                           │
     ├─ 转发 -mllvm -aimv-enable              │
     ├─ 常规编译 + AIMVFeedbackPass           │
     │  └─ 产出 aimv.json                     │
     │                                       │
     ├─ fork + exec aimv-driver ─────────────▶│
     │   --from-json=aimv.json                ├─ 读 aimv.json → 调 MCP
     │   --source=task.c                      ├─ AI 建议 → patch 源码
     │                                       ├─ 重编译 + 测试验证
     │                                       └─ 返回 exit code
     │◀────────────────────────────────────── │
     │                                       │
     └─ driver exit 0 → 源码已修改，exit 0   │
        driver exit ≠0 → 回滚，报告路径到 stderr
```

**改动范围**：

| 层 | 改动 | 量 |
|----|------|-----|
| clang Driver (`Clang.cpp`) | 解析 `-faimv`，编译后 `fork+exec aimv-driver` | ~20 行 |
| clang Options (`Options.td`) | 注册 `-faimv` / `-fno-aimv` flag | 3 行 |
| LLVM Pass Pipeline | `PassRegistry.def` + `PassBuilderPipelines.cpp` 新增注册 | ~5 行，设计已完成 |
| aimv-driver (Python) | 编排逻辑，无修改；新增 `--from-json` 入口 | ~10 行 |

**空诊断时的行为**：`aimv.json` 中所有 diagnostics 的 `severity == "passed"`（即无向量化失败）时，`aimv-driver` 直接 exit 0，输出：`[AIMV] all loops already vectorized, nothing to do` 到 stderr。clang Driver 将该 exit 0 视为成功。

**迭代重编译与防无限 fork**：

```
Round 1 (用户触发):
  clang -O2 -faimv -c task.c           ← 用户命令，含 -faimv
    ├─ Driver 编译                           启动 AIMVFeedbackPass
    └─ Driver fork aimv-driver --from-json=aimv.json

Round 1-N (aimv-driver 内部，BuildOrchestrator 调用 clang):
  clang -O2 -mllvm -aimv-output=<path> -c task.c    ← 不含 -faimv！
    ├─ 不会触发 clang Driver 再 fork aimv-driver
    ├─ 只运行 LLVM 层的 AIMVFeedbackPass 收集诊断
    └─ aimv-driver 读新 aimv.json 判断下一轮

关键: -faimv 只存在于 clang Driver 层（C++）。
       aimv-driver 调用的是 LLVM 后端 flag（-mllvm -aimv-enable），
       永远不会传递 -faimv，因此不会产生 fork 链。
```

**多函数文件处理策略**：

一个 `.c` 文件可能包含多个函数，其中只有部分存在向量化失败。

- `aimv.json` 按**函数**粒度组织诊断，每个函数独立记录
- `aimv-driver` 按**函数**粒度迭代：对文件内所有失败函数**顺序处理**，每次只修改一个函数
- 影子文件协议**每函数独立应用**：函数 A 验证通过后立即 `mv` 到原文件，不等待 B 完成。这样 B 编译时看到的源码已包含 A 的变更，且 B 失败时不需要回滚 A
- 轮次计数**每函数独立**：函数 A 成功向量化不影响函数 B 继续迭代
- 编译结束时，stderr 输出按函数分别汇总

**MCP 请求的源码版本**：每轮发送给 MCP 的 `source_code` 为**当前文件的完整内容**（即已包含前面函数所有已通过变更的版本）。AI 生成的 diff 基于该版本，行号与当前文件一致，不会出现偏移。

**作用范围**：AIMV 仅修改 `.c` / `.cpp` 源文件，不修改头文件（`.h` / `.hpp`）。头文件中的 `static inline` 函数或宏展开导致的向量化失败不在 MVP 处理范围内。

**配置来源**（与 DRIVER_DESIGN §8.1 一致）：

`aimv-driver --from-json` 模式下，运行参数按以下优先级加载：

1. 环境变量（最高优先级）：`AIMV_MCP_URL`、`AIMV_MCP_API_KEY`、`AIMV_LEVEL`、`AIMV_MAX_ROUNDS`、`AIMV_TEST_CMD`、`AIMV_MODE`（`review`/`dry-run`/`off`）
2. `~/.aimv/config`（YAML，用户全局配置）
3. 默认值

aimv.json 内无配置字段，仅包含诊断数据。

默认值：`aimv_level=conservative`（Driver 自动模式和 `aimv-driver` 独立模式统一。全自动场景下保守修改是安全基线，用户可通过环境变量或配置提升）、`max_rounds=5`、`mcp_url=http://localhost:8080`、`test_cmd=""`（空则跳过测试，仅做编译验证）。

**并发安全与数据竞争防护**：

| 场景 | 冲突？ | 处理 |
|------|--------|------|
| `make -j4` 编译不同 `.c` 文件 | 无 | 各自独立，不同锁 |
| 同一 `.c` 被并行编译两次（如 `foo.o` + `foo.pic.o`）| 排队 | FileLock（`source_manager.py`）串行化 |
| 排队等锁的进程获得锁后，文件已被前一个进程修改 | **有** | 影子基于获取锁时的快照，而非旧版本 |
| driver 改文件时另一个 make 进程在读 | **有** | 影子文件 + 原子替换 |

**影子文件 + 原子替换协议**：

```
Round N 完整时序（锁仅覆盖文件 I/O 阶段）:
  1. [无锁] MCP 查询 → 获得 AI 建议
  2. [获取锁]
     a. cp task.c → task.c.aimv-tmp        ← 基于当前版本创建快照
     b. diff 基于快照生成                   ← 确保 diff 上下文正确
     c. patch task.c.aimv-tmp              ← 在影子上修改
     d. clang -c task.c.aimv-tmp           ← 编译验证（其他进程读 task.c 不受影响）
     e. 测试通过 → mv task.c.aimv-tmp task.c ← 原子替换（rename(2) 是原子的）
        失败    → rm task.c.aimv-tmp       ← 丢弃影子
  3. [释放锁]
```

**锁的作用域**：仅覆盖步骤 2a-2e（文件 I/O，秒级）。步骤 1 MCP 查询（网络 I/O，数十秒）不占锁，不阻塞其他 clang 进程。

**多函数文件的原子替换时序**：每个函数的 round 独立应用。函数 A 验证通过 → `mv` 完成 → 释放锁 → 处理函数 B。函数 B 的 MCP 查询和编译基于**已包含 A 变更**的源码，函数 B 失败不回滚函数 A 的变更。

**中止保护**：`aimv-driver` 被 kill -9 或断电时，`task.c.aimv-tmp` 残留在磁盘。下次启动时检测到残留影子文件，输出警告让用户手动检查，不自动覆盖 `task.c`。

**自动化与人工审查**：Driver 模式默认**全自动**（编译→AI→patch→验证，无需人工介入）。对安全性要求高的场景，设置 `AIMV_MODE=review` 环境变量后，每轮 patch 前暂停等待用户输入 `[y/N/r/q]` 确认（与 `aimv-driver --require-review` 行为一致）。默认跳过 review 是因为 §4.3 的"编译验证 + 测试套件"两层自动保险已可拦截语义错误。

**fork+exec 异常处理**：

| 场景 | clang Driver 行为 |
|------|------------------|
| `aimv-driver` 未安装 / 找不到 | 打印警告到 stderr，退化到普通编译（exit code 不变） |
| `aimv-driver` 崩溃（非零退出 / signal） | 影子文件协议保证原文件未被修改（patch 在影子上）。clang Driver 检测到非零退出后，打印错误到 stderr，exit code = aimv-driver 退出码。若崩溃时残留 `*.aimv-tmp` 影子文件，下次 aimv-driver 或 CI 脚本应检测并清理。 |
| `aimv-driver` 超时（默认 120s，可配置） | SIGKILL 终止，回滚源码（同上），打印超时错误 |
| 用户 Ctrl+C（SIGINT） | 传递给 aimv-driver，由其回滚源码后退出；clang Driver waitpid 后退出 |
| 用户 kill（SIGTERM） | 同 SIGINT |

**编译结束后的用户可见输出**（aimv-driver 输出到 stderr，不干扰编译 stdout）：

```
# 单函数成功：
[AIMV] process_task: vectorized (2 rounds, conservative)
[AIMV]   Round 1: added restrict to parameter 'a' (line 1)
[AIMV]   Patch: /home/user/task.c.aimv.patch
[AIMV]   Report: /home/user/aimv-output/aimv-a1b2c3d4e5f6.json

# 多函数混合结果：
[AIMV] task.c: 3 functions analyzed, 2 optimized, 1 skipped
[AIMV]   process_task: vectorized (2 rounds, conservative)
[AIMV]   filter_data:  vectorized (1 round, moderate)
[AIMV]   init_buf:     already vectorized (skipped)
[AIMV]   Patch: /home/user/task.c.aimv.patch
[AIMV]   Report: /home/user/aimv-output/aimv-a1b2c3d4e5f6.json

# 放弃场景：
[AIMV] process_task: unable to vectorize (3 rounds exhausted)
[AIMV]   Source rolled back to original
[AIMV]   Report: /home/user/aimv-output/aimv-a1b2c3d4e5f6.json
```

**追溯产物**（均由 aimv-driver 生成）：

| 产物 | 路径 | 格式 | 生成时机 |
|------|------|------|---------|
| 累积 patch | `<source>.aimv.patch` | unified diff（仅包含最终成功的变更，相对于原始源码；失败/回滚的中间尝试不记录） | 每轮成功后更新 |
| Session 记录 | `aimv-output/sessions/<id>.json` | JSON（完整诊断→AI→patch→验证链） | 每轮更新 |
| 备份 | `aimv-output/backups/<name>.r<N>.bak` | 原始源码副本 | 每次 patch 前 |

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
- 编译通过 + 原测试套件全绿（测试命令通过 `AIMV_TEST_CMD` 环境变量或 `~/.aimv/config` 指定；未配置时仅做编译验证，跳过测试步骤）
- **Alive2 语义验证**（Phase 2 预留，MVP 阶段不集成。MVP 仅依赖编译验证+测试套件两层保障）:
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
  "request_id": "aimv-a1b2c3d4e5f6",
  "target": {
    "triple": "armv7-unknown-linux-gnueabi",
    "cpu": "cortex-a9",
    "features": ["neon", "vfp4"],
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
      "function_name": "process_task",
      "loop_location": "task.c:42:5",
      "source_context": "...",
      "ir_snippet": "...",
      "cost_model": { "scalar_cost": 24, "vector_cost": 38, "vf": 4, "interleave_count": 1 },
      "dependencies": [...]
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
}
```

history 传递策略：发送最近 3 轮的历史（含 outcome），让 AI 避免重复建议。不足 3 轮时发送全部。每条 history 约 100-200 字节，不会显著增加请求体积。

### 5.3 诊断信息包（每轮发送给大模型）

| 必须字段              | 说明                                                                                         |
| ----------------- | ------------------------------------------------------------------------------------------ |
| **opt-info 失败文本** | LLVM optimization remark 完整文本（如 `loop not vectorized: unsafe dependent memory operations`） |
| **VPlan 代价模型拆解**  | 总标量代价 vs 向量代价、VF、interleave count（MVP 不含逐指令明细，延后到 Phase 2）                      |
| **失败循环源码 + 行号**   | 原始 C/C++ 源码片段及精确行号映射                                                                       |
| **对应的 LLVM IR**   | 优化前后的 IR 片段（含 `!dbg` 调试行号）                                                                 |

---

## 6. 目标诊断维度

### 6.1 LoopVectorize（首期聚焦 — MVP）

| 维度          | 关键诊断项                             | 典型 AI 建议                                      |
| ----------- | --------------------------------- | --------------------------------------------- |
| **依赖分析失败**  | 指针别名不可判、数组下标 SCEV 不可推断、跨迭代读后写/写后读 | 加 `restrict`、`__builtin_assume`、循环 fission 改写 |
| **代价模型拒绝**  | VF*向量代价 > 标量代价、向量指令代价估计过高         | 调整循环结构、减少控制流、添加 `#pragma clang loop`          |
| **内存/对齐问题** | 非连续访存、对齐未知、stride≠1、TLI 不支持       | 加 `alignas`、`__builtin_assume_aligned`、数据重排   |

### 6.2 SLP Vectorizer（Phase 2）

SLP 处理**基本块内**的标量指令链（水平向量化），与 LoopVectorize（循环级）互补。

| 维度          | 关键诊断项                             | 典型 AI 建议                                      |
| ----------- | --------------------------------- | --------------------------------------------- |
| **不支持指令类型**  | `UnsupportedType`: 指令操作数类型不被 SIMD 支持 | 拆分复合表达式、使用更小的类型、对齐内存访问 |
| **向量宽度太小**  | `SmallVF`: 可向量化的指令太少，代价不划算 | 合并相邻计算、手动 unroll 展开更多指令 |
| **代价不划算**  | `NotBeneficial`: 打包/解包开销超过 SIMD 收益 | 调整数据布局（连续存储）、减少 type cast |
| **水平归约失败**  | `NotPossible`: reduction 模式不被识别 | 重写归约代码为标准形式（`a += b[i]`） |

### 6.3 Loop Unrolling（Phase 2）

Unrolling 既是独立优化，也是向量化的前置条件。

| 维度          | 关键诊断项                             | 典型 AI 建议                                      |
| ----------- | --------------------------------- | --------------------------------------------- |
| **Trip count 不可知** | 编译期无法确定循环迭代次数 | 加 `__builtin_assume(n >= N)` 提示范围 |
| **循环体过大**  | unroll 后代码膨胀超过阈值 | 循环 fission 拆分、提取冷路径 |
| **代价不划算**  | unroll 因子 > 1 时代价不降反升 | `#pragma clang loop unroll(enable)` + 手动调整因子 |

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
2. `AIMVFeedbackPass` LLVM Pass（收集 opt-info 诊断信息）
3. MCP REST API mock/实现（接收诊断、返回建议）
4. clang Driver `-faimv` 解析 + `fork+exec aimv-driver` 集成
5. 1 个已知失败 benchmark 的端到端 demo（含 clang `-faimv` 入口）

---

## 8. 成功度量

### 8.1 测量方法

**工具**: Linux `perf stat`（PMU 计数器: cycles, instructions, branches, branch-misses）或 `clock_gettime()` 高精度计时。

**策略**: 预热 3 次后取 10 次运行的**中位数**（排除首轮冷启动和末轮噪声）。

**基线**: 优化前原始代码在 `-O2` 下的中位数执行时间。

**合格阈值**: 中位数执行时间缩短 ≥ **5%** 且所有测试无回归。不只看向量化 "覆盖率"，要验证实际加速比。

### 8.2 验证 Benchmark 集

从 `aimv/benchmarks/` 中选取至少 5 个已知有向量化失败场景的 C 文件：

| Benchmark | 失败类型 | 目标 | 阶段 |
|-----------|---------|------|------|
| `dep_fail_alias.c` | 别名分析失败 (CantReorderMemOps) | restrict 修复后向量化 | MVP |
| `dep_fail_stride.c` | 跨迭代 RAW 依赖 | loop fission 或 restrict | MVP |
| `cost_reject.c` | 代价模型拒绝 | pragma 或结构调整 | Phase 2 |
| `align_unknown.c` | 对齐未知 | alignas 修复 | Phase 2 |
| `multi_fail.c` | 混合失败（依赖+代价） | 多轮迭代 | Phase 2 |

### 8.3 可操作性要求

- 每个 benchmark 附带 `Makefile` 或编译脚本，明确 compiler flags
- 附带计时 wrapper（`perf_runner.sh`），输出稳定可比的执行时间
- CI 中可运行: `make -C benchmarks && ./run_bench.sh && python3 analyze_results.py`

---

## 9. 扩展路线图

| 阶段               | 内容                                                      | 依赖             |
| ---------------- | ------------------------------------------------------- | -------------- |
| **Phase 1 (当前)** | 向量化单 Pass 闭环（依赖分析）                                      | MVP 完成         |
| **Phase 2a**     | 覆盖代价模型 + 内存/对齐 两个 LoopVectorize 诊断维度               | MCP 服务稳定       |
| **Phase 2b**     | SLP Vectorizer 诊断集成（复用 !aimv.diag 通道）                   | Phase 1-2a 验证有效 |
| **Phase 2c**     | Loop Unrolling 诊断集成                                          | Phase 1-2a 验证有效 |
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
| 入口方式             | clang Driver 原生集成（非外部 wrapper） | 零安装成本，`-faimv`/`-mllvm` 分层天然防 fork 链 |
| 迭代粒度             | 按函数顺序处理，每函数独立轮次              | 多函数文件中已成功的函数不受其他函数失败影响 |
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

*文档版本: 1.4*
*创建日期: 2026-04-29*
*最后更新: 2026-05-17*
