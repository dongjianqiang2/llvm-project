# EJIT I-Cache 命中率分析

**日期**: 2026-08-04
**状态**: 讨论稿，待实测验证

---

## 1. 背景

JIT 特化函数体通过 `FIXED_CODE_POOL` 部署在单独的 `.text.ejit` 段，与 AOT 的 `.text` 段物理分离。虽然 `EJitLinkOptimizationPlugin` 已将长跳转 stub 优化为直接 `BL`（±128 MiB 直跳），但 JIT 代码和 AOT 代码位于不同的内存区域，理论上可能引发 I-cache 换入换出。

本文分析这种 cache 竞争在 EJIT 实际场景下的影响程度。

---

## 2. 执行模型：替换关系而非交替关系

JIT 特化后，`ejit_entry` 函数的 AOT 版本不再被执行：

```
无 JIT：  caller → AOT(entry) → AOT(helper1) → AOT(helper2) → return
                     ↑ AOT .text 段
有 JIT：  caller → wrapper/dispatcher → JIT(entry_specialized) → AOT(helper1) → AOT(helper2) → return
                            ↑ AOT .text 段    ↑ .text.ejit 段       ↑ AOT .text 段
```

关键洞察：**JIT 代码替换了 entry 的 AOT 执行，而非与之交替**。真正和 JIT 代码共享 I-cache 的是那些被特化函数调用的 AOT helper 函数——它们本来就因调用关系已在 cache 中，不构成新的竞争源。

---

## 3. I-Cache 容量估算

以 ARM Cortex-A 嵌入式典型配置为例：

| 层级 | 大小 | 关联度 | 行大小 |
|------|------|--------|--------|
| L1 I-cache | 32-64 KB | 2-4 way | 64 B |
| L2 unified | 128 KB-1 MB | 8-16 way | 64 B |

### 3.1 特化代码体量

经过分支折叠 + 常量传播 + Dead-Code Elimination 后：

- **特化 entry 函数**：通常 50-500 字节（取决于原始复杂度）
- **AOT helper 函数**（被 entry 调用）：通常 100-1000 字节
- **热点路径总代码量**：通常 2-5 KB

### 3.2 结论

单个 EJIT 热点路径的代码量（2-5 KB）远小于 L1 I-cache 容量（32-64 KB）。**容量本身不是瓶颈**。

---

## 4. Cache Set 冲突分析

### 4.1 冲突条件

Cache set 冲突发生在两个代码地址映射到同一 cache set 时：

```
32KB L1, 4-way, 64B line → 128 sets (index bits [12:6])

两个地址落在同一 set 的条件：
  (addr1 >> 6) % 128 == (addr2 >> 6) % 128
即:
  (addr1 >> 6) & 0x7F == (addr2 >> 6) & 0x7F
```

对于 way-associative cache，只有同一 set 内的 concurrent line 超过 associativity（4 way）时才会触发逐出。

### 4.2 EJIT 场景评估

- `.text` 和 `.text.ejit` 物理地址由链接脚本确定，布局是可控的
- `FIXED_CODE_POOL=ON` 使 JIT 区域地址在链接时已知
- 热点函数数量少（通常 <10 个），associativity（8-16 way for L2）足以容纳

**Set 冲突在 EJIT 场景下概率低**，且可通过调整链接脚本偏移规避。

---

## 5. 真正需要关注的方面

### 5.1 测量优先

在假设瓶颈之前，应先用 PMU 计数器实测：

```bash
# AArch64 PMU 事件
perf stat -e L1I_CACHE_REFILL,L1I_CACHE,L2D_CACHE_REFILL \
    -e br_mis_pred,branch_prediction \
    -e cpu_cycles,instructions \
    ./ejit_benchmark
```

关键指标：
- `L1I_CACHE_REFILL / L1I_CACHE` = L1 I-cache miss 率
- `L1I_CACHE_REFILL / cpu_cycles` = 每周期 I-cache miss 次数

典型健康值：I-cache miss 率 < 1%。如果 > 5% 才需关注。

### 5.2 D-Cache 更可能是瓶颈

JIT 特化对 D-cache 的影响更值得关注：

- **收益**：`ejit_period_const` load 被替换为常量，减少全局变量 D-cache 访问
- **风险**：循环展开等优化可能导致栈帧变大、寄存器溢出增多

### 5.3 分支预测

特化后的代码消除了条件分支（常量折叠），**分支预测准确率应该提升**而非下降。

---

## 6. 可行的缓解手段（按投入产出比排序）

### Tier 1：几乎零成本

- ~~**代码池分配 padding**~~（已评估，不实施）：4K seal 模式下每次分配已对齐到 4 KiB 页，远超 cache line 粒度，额外 padding 无效果
- **链接脚本偏移验证**：检查 `FIXED_CODE_POOL` 基址与 `.text` 热点函数地址之间的 offset，确保不是 2 的幂次倍数

### Tier 2：低实现成本

- **函数排布聚类**：将 inline-cache dispatcher 函数集中放在同一 cache line，利用空间局部性（见 [§7](#7-dispatcher-聚类分析inline-cache-模式)）
- **Cold code 隔离**：将 JIT 编译失败路径（`_miss` 函数体）通过 `__attribute__((cold))` 标记，减少热路径 cache 污染

### Tier 3：需要更多设计

- **PGO 指导的代码排布**：在 AOT 阶段用 profile 数据指导 `.text` 和 `.text.ejit` 的相对偏移
- **Cache 着色**（page coloring）：对于确定性需求的嵌入式场景，可精确控制物理地址位

### 不建议做的

- 复杂的 page coloring 系统——嵌入式代码量小，L2 关联度够高，投入产出比极低
- 动态代码搬迁——破坏 JITLink 的重定位假设，风险太高

---

## 7. Dispatcher 聚类分析（Inline Cache 模式）

### 7.1 Inline Cache Dispatcher 的特点

开启 `-ejit-inline-cache` 后，`ejit_entry` 函数变身为一个极小的分发器：

```asm
; 0-dim dispatcher（约 4 条指令，~16 字节）
jit_entry:
    adrp    x9, __ejit_icache_fn_xxx
    ldr     x9, [x9, :lo12:__ejit_icache_fn_xxx]
    cbz     x9, .L_miss
    br      x9                     ; tail call to JIT specialized
.L_miss:
    b       xxx_miss               ; tail call to miss function (AOT body)
```

| 维度 | 指令数 | 代码量（估算） |
|------|--------|---------------|
| 0-dim | ~4 | ~16 bytes |
| 1-dim | ~6 | ~24 bytes |
| 2-dim | ~8 | ~32 bytes |
| 3-dim | ~10 | ~40 bytes |
| 4-dim | ~12 | ~48 bytes |

- 单个 cache line（64 B）可容纳 1-4 个 dispatcher
- 所有 dispatcher 体积之和（假设 50 个 ejit_entry 函数，平均 2-dim）：50 × 32 B = **1.6 KB**

### 7.2 聚类方案的 Cache 收益分析

**方案**：将所有 dispatcher 放入独立的 `.text.ejit_dispatch` 段，链接时连续排布。

**时序场景分析**：

```
场景 A：频繁切换不同 ejit_entry 函数
  时刻 T1: call funcA → 加载 cache line [funcA, funcB, funcC]
  时刻 T2: call funcB → 已在 L1 I-cache 中！(空间局部性命中)

场景 B：反复调用同一个 ejit_entry 函数（最常见）
  时刻 T1: call funcA → 加载 cache line [funcA, funcB, funcC]
  时刻 T2: call funcA → 已在 L1 I-cache 中 (时间局部性命中，无论是否聚类)
  聚类无额外收益，但无损失

场景 C：调用模式无规律
  聚类不带来收益——相邻 dispatcher 被加载但从未使用（cache 污染）
  但 dispatcher 总体积极小（~1.6 KB），污染的绝对量可忽略
```

**收益量化估算**：

| 指标 | 无聚类（分散在 .text 中） | 聚类（集中在 .text.ejit_dispatch） |
|------|--------------------------|-----------------------------------|
| 首次调用 miss | I-cache miss × 1 | I-cache miss × 1（无差异） |
| 相邻 dispatcher 调用 | 可能 miss（分散在不同页） | 已命中（同 cache line） |
| iTLB 压力 | 每个 dispatcher 占用独立 TLB entry | 全部共享少量 TLB entries |
| .text 段的 cache 污染 | dispatcher 占据 .text cache line 的部分空间 | dispatcher 不再污染 .text 的 cache line |

**结论**：
- **场景 A（频繁切换）**收益最明显——聚类利用空间局部性，使切换调用的 dispatcher 已位于 I-cache
- **场景 B（重复单函数）**收益中性——聚类无帮助也无伤害
- **所有场景的 iTLB 收益**是确定的——降低 TLB 压力
- **对 .text 的净化收益**——将 dispatcher 从 AOT .text 段剥离，使 .text 的 cache 利用率更高

### 7.3 实现要点

1. **段声明**：PASS3 为 dispatcher 函数（`jit_entry` 和 `jit_icache_dispatch` 块）设置 `section(".text.ejit_dispatch")`
2. **链接脚本**：在 linker script 中添加 `.text.ejit_dispatch` 段，位于 `.text` 和 `.text.ejit` 之间
3. **Miss 函数段**：`_miss` 函数体（即原始 AOT 函数体）保留原 `.text` 段或放入 `.text.cold` 段
4. **对齐**：段首 64 字节对齐（cache line 对齐），避免 false sharing

### 7.4 潜在风险

- **额外的段跳转**：dispatcher → JIT 特化函数（`.text.ejit`）和 dispatcher → `_miss`（`.text`）的跳转距离可能增大。需验证 ±128 MiB 约束仍满足
- **链接脚本复杂度**：增加一个段的管理负担
- **收益与场景强相关**：频繁切换 `ejit_entry` 的调用模式下收益才明显；如果大部分时间只调用 1-2 个 entry 函数，聚类收益微乎其微

### 7.5 决策建议

| 条件 | 建议 |
|------|------|
| 只有 1-3 个 `ejit_entry` 函数 | 不值得做——收益可忽略 |
| 有 10+ 个且存在频繁切换场景 | 值得做——实现成本低，收益可测量 |
| 不确定 | **先跑 PMU perf stat**，看 I-cache refill 率的绝对数值，<2% 则优先级低 |

---

## 8. 后续行动计划

1. [ ] **PMU 测量**：在 aarch64_be 实板上测量 I-cache refill 率和 D-cache refill 率
2. [ ] **热点路径 size 测量**：用 `objdump -d` 统计特化后的实际代码量
3. [ ] **Set 冲突验证**：计算 `.text.ejit` 与 `.text` 中热点函数的物理地址 offset，检查是否命中同一 cache set
4. [x] **Dispatcher 聚类实验**：已实施（PR #124），待上板验证
5. [x] **Cold MissFn 隔离**：已实施（PR #124），`Attribute::Cold` → `.text.unlikely`，待上板验证
