# AI × 传统编译器：性能优化与小型化方向探索

> 目标：用 AI 改造传统编译器本身（而非用编译器服务 AI 模型），围绕两个落点 —— 让编译器**产出代码更优**（性能）、让编译器**自身/产物更小更快**（小型化）。

---

## 性能方向（让编译器产出更优代码）

### 1. 学习型 Inliner / Unroller 的"可解释化"

**问题本质**
MLGO 把 LLVM 的 inliner 换成了强化学习训出来的神经网络（`-mllvm -enable-ml-inliner=release`），在 size 上比手写启发式好 3-7%。但它是黑盒：编译器开发者无法 review、调试时无法解释"为什么这个函数被 inline 了"、移植到新架构要重新训练几周。这违背了编译器"可审计"的传统。

**技术路径**
- 用训好的 MLGO 模型在大规模 IR 上跑推理，收集 (feature → decision) 数据
- 用决策树 / 规则学习算法（CART、RIPPER，或更现代的 Neural Decision Forests）拟合
- 把决策树反向翻译成 C++ 启发式代码，插回 `Inliner.cpp`
- 对比"原始 MLGO vs 蒸馏后规则"在 SPEC / LLVM test suite 上的差距

**核心难点**
- **特征工程**：MLGO 用了 ~30 维特征，决策树要在不爆炸节点数的前提下表达非线性组合
- **精度墙**：决策树通常比 NN 差 1-2%，要找到能拉回的技巧（boosting、规则后处理）
- **稳定性**：神经网络在 OOD 样本上随机退化，规则系统的退化模式可预测——要量化这个优势

**切入点**
先做 size inliner（reward 简单：最终 binary size），跑通蒸馏 → 回灌 → 对比的闭环；再扩展到 perf inliner。LLVM 有 `MLInlineAdvisor` 抽象，注入新 advisor 很干净。

---

### 2. 基于 LLM 的 Superoptimizer

**问题本质**
传统 superoptimizer（Souper、STOKE、Souper+Alive2）枚举或 SMT 求解，规模卡在 ~5 条指令。但很多真实的优化机会是 **10-30 条指令的局部重写**（比如 SIMD 化、bit-trick 重构）。LLM 有"猜出非平凡候选"的能力，但不可信——必须配 verifier。

**技术路径**
- 从真实代码（kernel、ffmpeg、加密库）切出小段 IR 作为输入
- LLM 生成 N 个候选重写（few-shot + IR 语法约束 decoding）
- Alive2 形式化验证语义等价 + cost model 估算收益
- 通过验证的 pattern 自动归纳成 InstCombine matcher 或 `.td` pattern

**核心难点**
- **输入表示**：直接喂 IR token 化效果差。需要 **SSA-aware encoding**——把 def-use 边作为 attention bias，或用 GNN 预处理后再喂 LLM
- **生成有效率**：朴素采样 99% 是语法错误或语义不等价。要做 grammar-constrained decoding（用 LLVM 的 IR grammar 约束）
- **新颖性 vs 平凡性**：模型容易输出"重命名+重排"这种平凡变换。需要在 reward 里惩罚低编辑距离的候选
- **回灌自动化**：验证通过的 pattern 怎么自动变成 C++ matcher？这步现在还得人肉。可以用模板化的 `m_*` matcher DSL

**切入点**
选一个窄场景，比如 **ARM Neon 的 saturating arithmetic 模式识别**——已知有大量手写汇编比 llc 优、Alive2 支持完整、pattern 写起来规整。跑通后再泛化。

---

### 3. 神经代价模型替代 MachineScheduler / RegAlloc 启发式

**问题本质**
`RegAllocGreedy` 的 spill weight 公式有十几个魔法常数；`MachineScheduler` 的 latency heuristic 在乱序+SMT+大小核场景下早就不准。这些启发式是 2000 年代针对 in-order 简单流水线设计的，现代 CPU 上经常做出反直觉的坏决策。

**技术路径**
- 收集 (MI 序列, 调度决策, 实测 cycle) 三元组——可以用 `llvm-mca` 当 oracle，或真机 PMU 采样
- 训一个轻量模型（GBDT 或 2 层 MLP）预测 "swap 这两条指令的收益"
- 把模型嵌入 `MachineScheduler::pickNode()` 作为 tiebreaker（不是完全替换）
- 用 fallback：模型置信度低时退回手写启发式

**核心难点**
- **推理延迟预算**：编译大型项目时 scheduler 可能被调用百万次。模型必须 <10μs/次推理——基本只能用决策树或极小 MLP
- **训练数据获取**：真机 PMU 噪声大、`llvm-mca` 不准。需要混合数据源 + 噪声鲁棒训练
- **泛化**：在 SPEC 上训的模型到 game engine 上还准吗？需要 online adaptation 或 per-workload fine-tune
- **最坏情况保证**：编译器不能容忍"偶尔生成慢 10x 的代码"。需要形式化的 safeguard

**切入点**
先做 RegAlloc 的 **spill weight 修正**——影响小、可回退、收益可测。跑通后扩展到调度。Google 的 MLGO RegAlloc 已经开了路。

---

### 4. Profile-Guided Optimization 的"无 profile 化"

**问题本质**
PGO 平均能再提 5-15% 性能，但要用户跑 profile，工程上极其麻烦（要构造代表性 workload、要重新部署一遍）。实际只有 Google / Meta 这种规模公司在用。如果能从静态 IR 预测出"近似 profile"，就能让 PGO 民主化。

**技术路径**
- 训练目标：给定 IR，预测每个 basic block 的相对频次（log scale）和分支概率
- 输入：函数的 CFG + 指令序列，用 GNN 编码
- 训练数据：在大规模开源项目（LLVM test suite、SPEC、Chromium）上跑真实 PGO，得到 ground truth
- 把预测结果灌进 `BlockFrequencyInfo`，让现有的 PGO-aware pass（inliner、layout、register allocator）直接用

**核心难点**
- **跨项目泛化**：分支频次和业务逻辑强相关。SPEC 上训的模型在数据库代码上可能完全错
- **错误代价不对称**：把冷代码预测成热的（过度优化），可能比反过来更糟（增加 i-cache 压力）
- **与现有 PGO 共存**：用户既给了 profile 又跑了预测时怎么融合？
- **解释性**：开发者会问"为什么这个 block 被认为是热的"，黑盒不可接受

**切入点**
从最简单的 **branch probability prediction** 开始（二分类，evaluation 直观）——替代 `BranchProbabilityInfo` 的启发式。这个子问题学术界做过（如 Calder 1997），但用现代 GNN 重做还没人系统跑过。

---

### 5. 自动发掘缺失的 Combine 规则

**问题本质**
GitHub、Linux kernel、ffmpeg、liboqs 里有大量"手写汇编 vs `gcc -O3` 输出"的 diff。每个 diff 背后都对应编译器漏掉的一个优化机会。这些机会现在靠 bugzilla 上偶尔有人 report——极低效。

**技术路径**
- 爬虫：扫 GitHub 上 `__asm__` 块、inline asm、加密库的手写 routine
- 对每段手写汇编：用同等 C 实现喂给 clang，生成对比汇编
- LLM 分析 diff：定位是哪一步丢的（DAGCombine？ISel？MachineScheduler？）
- 生成最小复现 IR + 候选 fix patch
- 用 Alive2 验证 fix 的正确性

**核心难点**
- **diff 归因**：同一段差异可能由多个 pass 共同造成。需要用 `-print-after-all` 跑出每个 pass 的输出，二分定位
- **最小化**：手写汇编通常是大函数，要 reduce 到最小可复现 IR。可以用 `llvm-reduce` 自动化
- **noise 过滤**：很多手写汇编只是用了不同寄存器调用约定，不是真的更优。要先做 cost 对比筛掉
- **patch 质量**：LLM 生成的 C++ matcher 经常错，需要 sandbox 编译 + 跑回归测试自动 gate

**切入点**
窄定一个领域，比如 **cryptography primitives**（OpenSSL、BoringSSL、liboqs）——这些库的手写汇编质量极高、和 C 实现一一对应、性能差距大。先跑出 50 个真实优化机会，再考虑泛化。

---

## 小型化方向（编译器本身 / 产物）

### 6. 学习型 Size Optimization（-Oz 的继承者）

**问题本质**
`-Oz` 现在的策略：关 inline、关 unroll、用短指令编码。但这是粗粒度的——同一个程序里，有些函数 inline 反而省 size（消掉了 prologue/epilogue + spill），有些 unroll 后能 fold 出更短编码。每个决策都是 size 上的微优化机会。嵌入式、Wasm、移动 App 极度需要。

**技术路径**
- 训模型预测 "inline 这个 callsite 的 size delta"、"unroll factor K 的 size delta"
- 输入：caller / callee IR + 调用上下文
- 在 size budget 约束下做全局 ILP 或贪心
- 评估：clang 自举、Linux kernel `allyesconfig` 的最终 size

**核心难点**
- **二阶效应**：inline A 可能让后续 pass 把 A+B fuse 成更短的代码——单点预测抓不到这个
- **架构敏感**：Thumb2 vs ARM 编码差异巨大，模型要 per-arch 训练
- **size 和 perf 的多目标**：用户其实想要 "size 不超 X 的前提下最快"，需要帕累托
- **训练数据稀缺**：要 ground truth size，得真编译——每个样本几秒，规模上不去。需要 surrogate（如 IR 指令数加权）

**切入点**
单独做 **size-aware inliner**，对标 MLGO 但 reward 函数纯 size。MLGO 框架已经有，直接换 reward 就能跑起来，快速验证想法。

---

### 7. 基于学习的 Link-Time 死代码消除

**问题本质**
LTO 的 DCE 基于可达性：只要可能被调用就保留。这个"可能"非常保守——比如 C++ 虚函数表里所有 entry、所有 `__attribute__((used))`、所有可能被 dlsym 找到的 symbol。实际运行时 90%+ 永远不会被调用。Chromium、Android system_server 这种巨型二进制特别受损。

**技术路径**
- 训练模型：给定 symbol 的特征（名字、签名、所在文件、是否在 vtable、是否 exported），预测"运行时是否会被调用"
- 训练数据：在大量真实程序上跑 instrumentation + workload，得到 ground truth
- 在 LTO 阶段，把低置信度 symbol 标记为"可删除"，但留 fallback：运行时 trap 后能 lazy load 回来（类似 demand-paging）

**核心难点**
- **误删代价极高**：删掉一个真的会被调用的 symbol 等于引入 crash bug。需要极保守的阈值 + 运行时安全网
- **反射 / dlopen 不可见**：Java JNI、Python C extension、插件系统都用 dlsym——静态完全看不到
- **训练数据获取**：要 instrument 大型程序跑真实 workload，工程量大
- **跨用户泛化**：不同用户用同一个 binary 触发不同 code path，怎么取并集

**切入点**
不要直接删，而是做 **profile-guided code layout 的辅助信号**——预测"冷 symbol"放到 binary 末尾，减少 i-cache 和启动时 paging。误判代价从"crash"降到"轻微变慢"，门槛低很多。

---

### 8. AI 辅助的 Pass 删除 / 编译器自身瘦身

**问题本质**
LLVM 主线有 ~200 个 IR pass、~150 个 MI pass。`-O3` 默认全跑。但对特定 workload（比如游戏引擎几乎没有 sparse computation、ML 推理几乎没有异常处理），很多 pass 跑了等于没跑——浪费编译时间，且增加编译器 binary 大小。

**技术路径**
- 离线分析：对目标 workload，逐个 pass 做 ablation——禁用后看生成代码质量变化
- 训模型预测"这个 pass 对这个 workload 是否有用"，输入是 workload 的 IR 统计特征
- 生成定制 PassManager 配置 + 裁剪过的编译器 binary
- 评估：编译时间下降 X%、binary 缩小 Y%、生成代码性能损失 < Z%

**核心难点**
- **pass 之间有依赖**：删 A 可能让 B 失效。需要建依赖图（LLVM 有 `AnalysisUsage` 但 transform pass 之间的隐式依赖没标）
- **"无用"的边界模糊**：某个 pass 99% 时间无效，1% 时间救命——删不得
- **如何裁 binary**：删 pass 容易（不注册就行），但要真正缩小 binary 需要让 linker DCE 掉对应代码——涉及大量符号依赖
- **评估成本高**：每个候选配置都要跑完整 benchmark

**切入点**
不做 binary 裁剪，先做 **PassManager 定制化**——针对 LLM 推理引擎（如 llama.cpp、MLC）的编译路径，生成最小 pass 序列。即使只省 30% 编译时间也有价值（CI、JIT 场景）。

---

### 9. 中间表示的学习型压缩

**问题本质**
Bitcode / MIR 在三个场景大量传输：分布式编译（distcc、icecc、Bazel remote build）、ThinLTO 缓存、编译产物分发（如 PNaCl、Wasm）。现在用 gzip / zstd 通用压缩，没利用 IR 的结构性。一个大型 C++ 项目 ThinLTO summary 几 GB，传输是瓶颈。

**技术路径**
- 训练 IR 专用的字典 / 语言模型——SSA name、opcode 序列、type 用法有强模式
- 两条路线：(a) zstd 训练字典（轻量、立刻可用）；(b) 神经语言模型 + arithmetic coding（压缩率更高但解压慢）
- 评估：压缩率、解压延迟、整体 build time 改善

**核心难点**
- **解压必须快**：编译器是延迟敏感的，神经模型解压可能比节省的传输时间还多。要 GPU 解压或极小模型
- **正确性绝对**：一 bit 错就崩，不能用 lossy
- **训练数据**：要大量代表性 IR——LLVM test suite 太小，可能要 instrument 真实 build farm
- **与现有工具链集成**：要无缝替换 ThinLTO 的序列化层，工程量不小

**切入点**
先做 **zstd 字典训练版**——一个晚上能跑出来，估计能省 20-40%（zstd 字典对结构化数据效果好）。验证有 ROI 后再考虑神经版本。

---

## 总评与推荐

按"做了能发论文 + 工业界会用"的优先级：

| 优先级 | 方向 | 理由 |
|--------|------|------|
| ★★★ | **方向 2（LLM Superoptimizer）** | propose-verify 范式 + Alive2 是目前最干净的 AI4Compiler 闭环，可信度高 |
| ★★★ | **方向 5（自动挖掘 Combine 规则）** | 投入产出比最高，每周都能产出真实 LLVM patch，建立学术 + 工程口碑 |
| ★★ | **方向 1（蒸馏 MLGO）** | 解决落地痛点，工业界会立刻拥抱 |
| ★★ | **方向 3（神经 RegAlloc / Scheduler）** | 硬件越来越异构（大小核、AMX、SME），手写启发式撑不住，时间窗口对 |
| ★★ | **方向 6（学习型 Size Opt）** | 嵌入式 / Wasm 刚需，MLGO 框架可复用 |
| ★ | 方向 4 / 7 / 8 / 9 | 价值清晰但工程量或落地风险较高 |

---

## 备注

- 文档来源：与 Claude 的设计讨论
- 范围限定：AI 改造传统编译器（不含"编译器服务 AI 模型"的方向，如 kernel 生成、量化感知 schedule 等）
- 下一步可挑选一个方向，进入"第一周做什么、需要哪些 dataset、最小验证实验"的层面
