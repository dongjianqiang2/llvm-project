# AI × 编译器探索方向总评榜

> 综合 `exploration-directions.md`（9 个渐进方向）与 `radical-directions.md`（9 个激进方向），共 **18 个方向**。
> 评级维度：**技术深度 × 颠覆性 × 真正做出来的价值**。
>
> 等级从高到低：**夯 → 顶级 → 人上人 → NPC → 拉完了**

---

## 🔥 夯（天花板级，做出来改写行业）

### 1. 自演化编译器（Self-Improving Compiler）
*来源：radical #2*
编译器在编译过程中持续从用户代码学习新优化规则，自动验证后固化进下一个版本。**一旦做成，传统编译器 5 年内被淘汰**。技术上 LLM + Alive2 已经具备基础组件，社会接受度（可复现编译、责任归属）是更大瓶颈。这是真正的范式革命，不是改良。

### 2. 编译器作为 Proof Assistant（Verified-by-Default Compilation）
*来源：radical #6*
AI 自动生成程序性质的证明，让所有代码默认带形式化证书（无 UB、无 data race、无 panic）。补上了 loop invariant inference 这个 30 年开放难题。**飞控、医疗、加密、AI 模型供应链全部受益**。需求清晰、技术路径清晰，缺的是工程级 LLM+SMT 闭环。

---

## ⭐ 顶级（强落地价值 + 强学术深度，能立大功）

### 3. 基于 LLM 的 Superoptimizer
*来源：exploration #2*
LLM propose + Alive2 verify 的闭环，把 superoptimizer 从 5 条指令规模拉到 10-30 条。**最干净的 AI4Compiler 范式**，正确性由形式化方法兜底，结果可直接回灌成 InstCombine / TableGen pattern。DeepMind AlphaEvolve、Meta LLM Compiler 已经验证范式可行，LLVM 主线还没人系统做。

### 4. 端到端可微编译器（Differentiable Compiler）
*来源：radical #1*
把整条 pipeline 的离散决策连续松弛，端到端梯度下降优化运行时间。**博士论文级别的硬核命题**，挑战编译器界"pass 必须离散、可调试"的根本共识。如果突破"约束可微化"这个核心技术问题，会开辟一个全新的研究领域。

### 5. 程序综合替代部分编译（Synthesis-Augmented Compilation）
*来源：radical #5*
编译器不只翻译，还会重写算法本身（O(n²) → O(n log n)），靠 I/O 等价 + profile 验证。**编译器变协同程序员**。GitHub Copilot 是弱版本，真正闭环（带形式化等价证明）的还没人做出来。

### 6. 神经符号混合 IR（Neuro-Symbolic IR）
*来源：radical #3*
让 IR 节点可以是神经嵌入，允许编译器在中间阶段"模糊地思考"，最后塌缩成符号实现。**质疑了"IR 必须有精确语义"这个最底层假设**。偏理论但一旦做出来，整个 MLIR/LLVM 生态都要重写。

---

## 💪 人上人（技术含量很高，能做出真东西）

### 7. 神经代价模型替代 MachineScheduler / RegAlloc 启发式
*来源：exploration #3*
RegAllocGreedy 的 spill weight、MachineScheduler 的 latency heuristic 在异构硬件（大小核、AMX、SME）上撑不住了。难点是 <10μs 推理延迟 + 最坏情况不退化。**时间窗口正好**，Google MLGO RegAlloc 已经开路。

### 8. 跨抽象层联合优化（Cross-Stack Joint Optimization）
*来源：radical #4*
打破编译器 / OS / 硬件的分层抽象，AI 学习跨层隐性接口。工程量巨大但渐进可做，方向正确——未来 10 年异构硬件爆炸，分层抽象一定会被突破。

### 9. 多模态编译器（Multi-Modal Compiler）
*来源：radical #7*
让编译器同时读代码 + 注释 + commit + issue + design doc，理解"程序意图"。**质疑了"代码是程序的完整规范"**这个假设。2-3 年能出 demo，但社会接受度（"编译器读我 Slack？"）是大问题。

### 10. 自动发掘缺失的 Combine 规则
*来源：exploration #5*
扫 GitHub 上"手写汇编 vs gcc -O3"的 diff，LLM 归因 + 生成 patch。**投入产出比最高**，每周能产出真实 LLVM patch，建立学术 + 工程口碑。门槛低、价值高，是切入 AI4Compiler 的最佳入门方向。

### 11. 编译器即操作系统（Compiler-as-OS）
*来源：radical #8*
取消编译时 / 运行时边界，AI 持续重编译热点代码，多版本共存按场景 dispatch。**JIT 的 100 倍极端化**。可行性低（需要硬件 / OS 配合）但方向激进，做出来等于重新定义"程序"。

---

## 🧍 NPC（路径清晰、价值实在，但缺乏惊艳）

### 12. 学习型 Inliner / Unroller 的"可解释化"
*来源：exploration #1*
把 MLGO 的黑盒神经网络蒸馏成决策树 / 规则，解决落地痛点（可调试、可审计、可移植）。工业界会立刻拥抱，但本质是"打补丁"——别人做创新，你做工程化。

### 13. Profile-Guided Optimization 的"无 profile 化"
*来源：exploration #4*
GNN 从静态 IR 预测 branch probability / block frequency，让 PGO 民主化。学术界 Calder 1997 就做过，用现代 GNN 重做有价值，但本质是老问题换新工具。

### 14. 学习型 Size Optimization（-Oz 的继承者）
*来源：exploration #6*
MLGO 框架直接换 reward 为 size，做嵌入式 / Wasm 的极致小型化。可立刻动手，但创新有限——就是把 MLGO 复制到新目标函数。

### 15. AI 辅助的 Pass 删除 / 编译器自身瘦身
*来源：exploration #8*
针对特定 workload 定制 PassManager / 裁剪编译器。工程价值清晰（CI 加速、JIT 启动），但学术上偏 ablation study。

### 16. 跨语言通用 IR + AI 翻译层
*来源：radical #9*
AI 自动学习 Python → Rust、CUDA → SYCL 翻译，带 differential testing 验证。商业公司（Cognition、Replit）已经在做，3 年内会成熟——**等于在卷红海**，留给个人 / 学术的空间不大。

---

## 💩 拉完了（看似有道理，做完一地鸡毛）

### 17. 基于学习的 Link-Time 死代码消除
*来源：exploration #7*
预测哪些 symbol 运行时不会被调用然后激进 DCE。**误删 = crash bug**，dlsym / 反射 / JNI 全是黑洞，跨用户泛化几乎不可能。退化到"冷代码后置 layout"就只是 PGO 的微小改良。risk/reward 严重失衡。

### 18. 中间表示的学习型压缩
*来源：exploration #9*
训神经模型压缩 bitcode / MIR。解压必须比节省传输还快——基本只有 zstd 字典训练这条路有 ROI，神经版本注定亏本。**做出来发不了好论文，工业界也只会用 zstd 那个版本**。属于"听上去 fancy 实际是工程小优化"的典型。

---

## 一句话总览

| 等级 | 方向 |
|------|------|
| 🔥 夯 | 自演化编译器、Verified-by-Default |
| ⭐ 顶级 | LLM Superoptimizer、可微编译器、程序综合、神经符号 IR |
| 💪 人上人 | 神经 RegAlloc/Scheduler、跨层联合优化、多模态编译器、自动挖 Combine 规则、Compiler-as-OS |
| 🧍 NPC | 蒸馏 MLGO、无 profile PGO、学习型 -Oz、Pass 裁剪、AI 翻译层 |
| 💩 拉完了 | 学习型 LTO DCE、IR 学习型压缩 |

---

## 推荐路径

- **想做"夯"级**：方向 1 或 2，但要做好 5-10 年长跑准备 + 跨学科团队（编译器 + 形式化 + ML）
- **想做"顶级"且能毕业**：方向 3（LLM Superoptimizer），范式干净、验证可靠、影响大
- **想快速出成果建立声誉**：方向 10（自动挖 Combine 规则），先 PR 50 个真实 LLVM patch 进主线
- **不推荐入门做**：方向 17、18，性价比低
