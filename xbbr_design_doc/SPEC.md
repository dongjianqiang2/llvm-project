# SPEC: 跨函数全局基本块重排 (Cross-Function Basic Block Reordering, XBBR)

> **需求文档** —— 描述 XBBR 要解决的问题、约束、外部接口与验收标准。
> 实现层面的设计（架构、算法、数据格式、代码组织）见同目录 [`PLAN.md`](PLAN.md)。

- **代号**：XBBR (eXtended cross-function Basic Block Reordering)
- **驱动选项**（拟）：`clang -fbb-cross-reorder=<mode>` / `lld --bb-cross-reorder=<file>`
- **状态**：需求草案 v0.1
- **日期**：2026-06-17

---

## 1. 背景与动机 (Background)

现有 LLVM 工具链中存在三个互不联动的代码布局优化：

- **IRPGO** 提供精确的 BB 频率计数，但消费者仅限于 LLVM 中端的 `MachineBlockPlacement`，**只能在单函数内部重排**。
- **Propeller** 在链接阶段做 BB 级重排，但默认采用 AutoFDO（采样不精确，且嵌入式场景常无 LBR），且**仍不跨函数**。
- **BOLT** 在 post-link 阶段做布局优化，需要二次部署流程，且与链接器决策互相覆盖。

**XBBR 的定位**：在链接阶段，以 IRPGO 提供的精确 BB 计数为输入，**跨越函数边界**重新排布 BB，将所有可执行二进制的热路径拼接为最少的连续段，同时把冷代码隔离到独立段。

---

## 2. 目标 (Goals)

### 2.1 核心优化目标

XBBR 必须**同时**改善以下四个维度，单一维度回归即视为失败：

| 维度 | 关键指标 | 目标 |
|---|---|---|
| I-Cache | L1i miss rate | 比仅 IRPGO 基线降 ≥ 15% |
| iTLB | iTLB miss / kInstrs | 降 ≥ 20% |
| 分支预测 / BTB | branch-misses / kInstrs | 降 ≥ 10% |
| 代码体积 / 冷代码隔离 | hot-text 增量 ≤ 20%，总体积增量 ≤ 3% |

### 2.2 目标场景

首期面向**嵌入式 / 受限环境**，意味着：

- 二进制总体积**严格受控**（与服务端 Propeller 不同，不可任意膨胀 hot 段）。
- 元数据 section 仅存在于 `.o` 文件，链接后**不进入**可执行体（除调试信息和决策 map 外）。
- 链接器需提供"低体积模式"开关，在性能与体积之间可调。

同时保留对服务端二进制的可用性（关闭体积约束即可）。

**体积预算口径（重要）**：SPEC §9.2 的体积增量门槛针对**可加载段（loadable image）**，不计入以下非加载内容：
- `.llvm_bb_freq` / `.llvm_cfg_edge` / `.llvm_xbbr_attr` —— `SHF_EXCLUDE`，链接后丢弃；
- `.debug_*` / `DW_AT_ranges` 膨胀 —— 非分配段，`strip --strip-debug` 可剥离；
- `.debug_xbbr_decision` —— 非 `SHF_ALLOC`，可剥离。

真正计入可加载体积的增量来源（嵌入式须在 §9.2 1.5% 预算内核算）：
1. **thunk / veneer** 字节（ARM/Thumb 尤甚），受 `--bb-cross-reorder-max-thunk-bytes` 约束；
2. **FDE 拆分**开销（漂移函数 1 个 FDE → N 个，每个含 ~8–16B 头）；
3. **`.eh_frame_hdr`** 二分查找表增长（每个新 FDE 增加一条 8B 表项）；
4. **对齐填充 NOP**（跨函数混排不同对齐要求的 BB 时引入，见 PLAN §4.3 Stage 3/5）。

PLAN Stage 3/4 须将上述四项纳入 `SizeOverhead` 与回退判定；超预算触发单 BB 回退或整体降级。

### 2.3 非目标 (Non-Goals)

- 不取代 IRPGO；XBBR 是 IRPGO 的**消费者**而非替代。
- 不取代 BOLT；XBBR 在链接期完成，BOLT 在 post-link 完成，两者可叠加。
- 不做跨函数 BB ICF / outlining（列入未来扩展）。
- 首期不支持 PowerPC / MIPS / s390x。
- 首期不支持 COFF / Mach-O（仅 ELF）。
- 首期不支持 ThinLTO 下的跨模块 BB 重排。

---

## 3. Profile 输入要求 (Profile Source Requirement)

**仅支持 IRPGO（Instrumentation PGO）**，工作流为 `-fprofile-instr-generate` → 运行 → `-fprofile-instr-use`。

理由：
- BB 频率信息精确（计数，非采样估计）。
- 现有 `MD_prof` metadata 直达后端，无需新增 IR pass。
- 嵌入式场景常无 LBR，AutoFDO 路径不可用。

未来可扩展支持 AutoFDO 或与之融合（见 §11），但首期不实现。

### 3.1 Profile 与源码失配处理 (Profile Mismatch)

当 IRPGO profile 与当前源码不完全匹配（函数增删、BB 结构变化、profile 过期）时，XBBR 必须定义确定性行为，禁止静默错误：

| 失配情形 | 行为 |
|---|---|
| 函数在源码中存在但 profile 无入口计数 | 视为**冷函数**：不参与跨函数迁移，留在原位；`entry_count` 按未知处理（`global_freq` 不可比较） |
| 函数 `MD_prof` 分支权重与当前 CFG 边不符（BB 增删/重组） | 复用 LLVM 既有 `-Wprofile-instr-mismatch` 诊断；该函数**降级为 function 级**（仅 hfsort+，不漂移其 BB） |
| profile 引用了已删除的函数 | 忽略，不报错 |
| profile 缺失（未传 `-fprofile-instr-use`） | `-fbb-cross-reorder` 主开关降级为 `none`，emit warning |

所有降级动作产生 lld/clang diagnostic（默认 warning，`-Werror` 友好，与 §7 一致）。`--bb-cross-reorder-fallback=none`（CI 严格模式）下，失配直接报错而非降级。

---

## 4. 重排粒度 (Granularity Modes)

通过 `-fbb-cross-reorder=<mode>` 选择运行模式：

| mode | 行为 | 适用场景 |
|---|---|---|
| `none` | 关闭 XBBR（默认） | 调试 / 无 profile |
| `function` | 仅函数级重排（hfsort+），等同 LLD CGProfile 现状 | 体积受限且无 BB 数据 |
| `partial` | **仅热 BB 允许跨函数提升**，冷 BB 留在原函数位置 | 嵌入式默认推荐 |
| `full` | 完全跨函数 BB 重排，函数边界仅作为符号锚点 | 服务端追求性能 |

`partial` 与 `full` 共享同一套元数据格式与链接器管线，差异仅在于哪些 BB 被允许迁移。

**冷热阈值（partial/full 区分的关键）**：`partial` 下的"冷 BB"由 `-fbb-cross-reorder-cold-threshold` 定义为 `global_freq(BB) < threshold × entry_count(F)`（即执行次数低于入口的 `threshold` 比例）。**默认 `threshold = 0.01`（1%）**，使 partial 仅迁移真正频繁执行的 BB，与 full 形成有意义的区分；若设为 `0` 则退化为"仅零频 BB 视为冷"，此时 partial 行为趋近 full（仅作激进调试用）。详见 §6.1。

---

## 5. ABI 与函数语义保持 (ABI Invariants)

以下是 XBBR **必须保持**的语义契约，任何实现都不得违反。

### 5.1 函数入口块锚定（强约束）

**函数符号 = 函数入口 BB 的地址**，永远不变：

- `call F` 永远跳到 F 的入口块。
- 入口块**不可**被跨函数迁移，**不可**被消除，**不可**被改写为指向其他位置的跳转。
- 调试器中 `info func F` 永远定位到合法地址。
- 动态库导出符号天然兼容（PLT/GOT 不受影响）。

### 5.2 非入口 BB 漂移规则

- 入口块之外的 BB 可以"漂"到任意位置（包括其他函数内部空隙、单独的热簇）。
- 漂移后的 BB 仍**逻辑上属于原函数**：DWARF `DW_TAG_subprogram` 的 ranges 必须列出所有归属 BB。
- 漂移后的 BB 在 backtrace、`addr2line`、`gdb` 中必须能被正确归因。

### 5.3 黑名单 BB（强制不可跨函数）

下列 BB 必须保留在原函数内（partial / full 模式都受约束）：

1. 函数入口块（§5.1）。
2. `indirectbr` 的目标块、`callbr` 的目标块、地址被 `blockaddress` 取走的 BB。
3. 异常处理 landing pad（绑定 `.gcc_except_table` FDE range）。
4. 含 `setjmp` / `longjmp` 调用的 BB。
5. 含 inline asm 且使用 `__attribute__((section))` 或显式 label 引用的 BB。
6. `musttail call` 所在 BB（防止破坏 tail-call 收尾）。
7. `noreturn` 之后无后继的尾块（归属原函数有助于回溯）。

### 5.4 异常处理与栈展开 (EH & Unwinding)

无论 BB 如何漂移，下列要求必须满足：

- 抛出异常时 personality routine 仍能定位正确的 LSDA 与 landing pad。
- `_Unwind_Backtrace` / libunwind / libgcc_s 在任意指令地址都能正确展开栈帧。
- ARM/AArch64 上 `.ARM.exidx` 表项与运行时地址完全一致。
- 漂移后的 BB 序列在 `gdb bt`、`perf record --call-graph dwarf` 下产生正确调用栈。

具体实现策略（FDE 拆分、CFI 重写）见 PLAN §5。

---

## 6. 用户接口 (User-Facing Interface)

### 6.1 Clang 选项

```
-fbb-cross-reorder=none|function|partial|full       # 主开关
-fbb-cross-reorder-cold-threshold=<frac>             # 冷 BB 判定阈值，默认 0.01
                                                      # 冷定义为 global_freq(BB) < frac × entry_count(F)
                                                      # 即 BB 执行次数低于入口的 frac 比例
                                                      # frac=0 退化为"仅零频为冷"（≈full，仅调试用）
-fbb-cross-reorder-blacklist=<file>                  # 用户级函数黑名单
-fbb-cross-reorder-stats                             # 输出统计
```

### 6.2 LLD 选项

```
--bb-cross-reorder=<profdata-or-none>
--bb-cross-reorder-mode=partial|full
--bb-cross-reorder-cluster-algo=hfsort+,c3,custom   # Stage 1 函数簇粗排算法
--bb-cross-reorder-layout-algo=ext-tsp,ph,custom     # Stage 2 簇内 BB 精排算法
--bb-cross-reorder-weights=icache=4,itlb=2,btb=1,size=2
--bb-cross-reorder-max-thunk-bytes=<n>               # trampoline 体积上限
--bb-cross-reorder-fallback=auto|conservative|none
--bb-cross-reorder-emit-decision-map                 # 写 .debug_xbbr_decision
--bb-cross-reorder-deterministic                     # 强制 bitwise reproducible
```

### 6.3 与既有特性的关系

| 既有特性 | 关系 |
|---|---|
| `-fbasic-block-sections=labels` | XBBR `partial` 自动启用 `labels`，`full` 启用 `all` |
| `-fsplit-machine-functions` | 提供冷热划分基线 |
| `--call-graph-profile-sort` | 作为函数级 baseline，被 XBBR Stage 1 接管 |
| `BOLT` | 可叠加（BOLT 消费 XBBR 决策 map）；亦可单独使用 |
| `Propeller` (`--symbol-ordering-file`) | **互斥**，链接器侧检测：同时启用报错 |

---

## 7. 安全回退要求 (Safety & Fallback Requirements)

XBBR 必须提供分级回退能力，避免单点失败导致整体编译/链接失败：

| 触发条件 | 期望行为 |
|---|---|
| BB 超分支距离且 thunk 超预算 | 单 BB 回退到原函数位置 |
| BB 是黑名单（assertion 兜底） | 单 BB 回退 |
| EH range 与邻 BB 冲突 | 单 BB 回退 |
| 多次回退导致 layout 退化超阈值 | 整体降级到 `--bb-cross-reorder-mode=function` |
| 用户函数级黑名单（`-fbb-cross-reorder-blacklist`） | 该函数所有非入口 BB 都不漂移 |
| profile 与源码失配（函数增删 / BB 结构变化，见 §3.1） | 失配函数降级为 function 级；profile 缺失则整体降级 `none` |
| 对齐填充/thunk 使可加载体积超 §9.2 预算 | 单 BB 回退；累计超阈则整体降级 |
| `--bb-cross-reorder-fallback=none` | 关闭所有回退，遇约束直接报错（CI 严格模式） |

回退动作均产生 lld diagnostic（默认 warning，`-Werror` 友好）。

具体回退算法见 PLAN §4.4。

---

## 8. 架构与产出支持 (Targets & Output Types)

### 8.1 首期支持架构

| Arch | 备注 |
|---|---|
| **x86_64** | 首发 |
| **AArch64** | 首发 |
| **ARM (A32 + Thumb-2)** | 首发 |

### 8.2 首期支持产出

- **静态可执行** (`-static`)
- **PIE 可执行** (`-fPIE -pie`)
- **动态库** (`-shared`)：仅允许跨函数 BB 在内部链接函数中漂移（导出符号严格锚定入口块）
- **Bare-metal 镜像**（嵌入式 ARM/AArch64）

### 8.3 暂不支持

- COFF / Mach-O（仅 ELF）。
- **ThinLTO**（`-flto=thin`）下的跨模块 BB 重排：ThinLTO 的并行 per-module CodeGen 与分布式后端使跨模块元数据序列化复杂，首期不实现（见 §12）。
- BTI/PAC 与 XBBR 的交互需在 M5 阶段额外验证。

**Full LTO 支持**：`-flto=full` 下，`XBBRMetadataEmitter` 在 LTO 的 CodeGen 流水线中按 **partition** 运行（与非 LTO 的 CodeGen 路径一致），元数据 section 随各 partition 输出的 `.o` 流入 lld，Stage 0 跨 partition 聚合为全局 BB graph。因此 full LTO 数据流与非 LTO 等价，无需特殊设计；内联发生在 CodeGen 之前，不影响 BB 粒度元数据。ThinLTO 是唯一需要特殊处理的 LTO 形态，故首期排除。

---

## 9. 测试与验收 (Testing & Acceptance)

### 9.1 测试覆盖要求

| 层级 | 内容 | 位置 |
|---|---|---|
| lit 单元 | 选项解析 / section 生成 / 回退路径 | `llvm/test/CodeGen/{X86,AArch64,ARM}/xbbr/`、`lld/test/ELF/xbbr/` |
| test-suite | SPEC CPU 2017 + MicroBenchmarks | `llvm-test-suite/External/SPEC/` |
| 实战项目 | clang/lld self-host、MySQL、Redis、Nginx；嵌入式样例 (Zephyr / micropython) | CI gate |
| 可重现性 | 同 source + profile + flags 必须 bitwise-identical | CI gate |

### 9.2 量化验收门槛

| 指标 | 嵌入式默认 (`partial`) | 服务端 (`full`) |
|---|---|---|
| L1i miss 下降 | ≥ 10% | ≥ 15% |
| iTLB miss 下降 | ≥ 15% | ≥ 20% |
| branch-miss 下降 | ≥ 8% | ≥ 10% |
| End-to-end perf | ≥ Propeller-equivalent | ≥ Propeller-equivalent |
| hot-text 增量 | ≤ 10% | ≤ 20% |
| 总二进制增量 | ≤ 1.5% | ≤ 3% |
| 链接时间增量 | ≤ 1.5× IRPGO baseline | ≤ 2.0× IRPGO baseline |

### 9.3 可重现性要求

- 同 source + profile + flags 输入两次，二进制 SHA256 必须完全一致。
- 必须在每个支持的 (arch × output-type × mode) 三元组上验证。
- 实现细节（稳定 tie-breaker、确定性数据结构）见 PLAN §6.2。

---

## 10. 路线图 (Roadmap)

5 阶段递进交付，每阶段独立可验收：

| 里程碑 | 交付内容 | 关键退出条件 |
|---|---|---|
| **M1** | 编译器侧元数据 sections + `XBBRMetadataEmitter` pass + clang 选项打通 | x86_64 上 `.o` 文件元数据正确，lld 暂不消费 |
| **M2** | lld Stage 0+1：读元数据 + 函数级 hfsort+ 粗排 + section emission 框架 | x86_64 静态可执行体可生成；功能等价于 CGProfile-only |
| **M3** | lld Stage 2+3+4：BB 级 ExtTSP + 多目标代价 + 单 BB 回退 + 决策 map BB 级条目 | x86_64 上 BB 级布局管线产出完整 `XBBRLayoutResult` + 决策 map（含 per-BB 32B entry）；物理 `.text.hot/.text.unlikely` BB 级 emit 推迟到 M5 |
| **M4** | DWARF/CFI/EH 完整重写 + `llvm-bbreorder-dump` | gdb / perf annotate 可读；BOLT 可消费决策 map |
| **M5** | 物理 BB 级 section emission + AArch64 + ARM 完整支持（含 thunk）+ PIE / 动态库 + `full` mode + §9.2 量化门槛 | 嵌入式 demo（Zephyr）通过；服务端 (clang/MySQL) 通过；L1i↓≥10–15%、iTLB↓≥15–20% 等指标达标 |

**Upstream 策略**：5 阶段以"实验性 feature"形式逐步进入 upstream LLVM (前缀 `experimental-`)，M5 完成后申请去前缀。

---

## 11. 未来扩展 (Future Work)

1. **生产环境 PMU 采样接入**：生产二进制部署后通过 BPF/perf 采集 PMU 事件 → 聚合为新版本 profile → 触发增量 re-link。
2. **增量重排** (`--bb-cross-reorder-incremental`)：仅重排 profile 上变化显著的 BB 簇，复用 90%+ 旧布局。
3. **与 AutoFDO 双源融合**：IRPGO 提供精确计数，AutoFDO 提供生产真实分布，二者加权融合。
4. **跨函数 BB ICF**：识别跨函数语义等价 BB 后去重。
5. **与 BOLT 协同**：XBBR 决策 map 直接喂给 BOLT，BOLT 仅做边角微调。
6. **ML 驱动代价函数权重**：用 RL/进化算法搜索最优权重组合。
7. **上下文敏感（CS）PGO 融合**：当前 `global_freq = local_freq × entry_count` 是 flat PGO 的聚合估计（PLAN §3.2）。未来接入 CSIRPGO 后，可按 callsite 拆分热度，使漂移决策更贴近真实调用上下文。

---

## 12. 未决问题 (Open Questions)

1. ARM/AArch64 BTI 与跨函数 BB 入口的 landing pad 兼容性需 M5 验证。
2. `-fbb-cross-reorder=full` 与 `-fcf-protection=branch` (IBT) 的交互。
3. 是否需要支持 `--gc-sections` 与跨函数簇的活性分析联动。
4. ThinLTO 下跨模块 BB 重排的元数据序列化（暂不支持，列入扩展）。
5. 决策 map section 是否应在 `--strip-all` 时仍保留（取决于事故诊断需求）。
6. **链接器松弛（linker relaxation）与 XBBR 的交互**：lld 的 `AArch64Relaxer`（ADRP+ADD→ADR、ADRP+LDR 折叠等）会改变 BB 字节大小，而松弛适用性又依赖最终布局（XBBR 改变布局 → 改变可松弛性 → 改变 size → 影响分支距离/代价）。需在 M5 确定排序：XBBR 读 post-relaxation size 还是禁用被重排区域的松弛，见 PLAN §4.5。
7. **FDE 拆分对 `.eh_frame_hdr` 与异常分发的影响**：漂移函数的 1→N FDE 必须同步重建 `.eh_frame_hdr` 二分查找表，否则 `_Unwind_*` 失败（见 PLAN §5.3）。

---

## 13. 词汇表 (Glossary)

| 术语 | 含义 |
|---|---|
| **BB** | Basic Block，基本块 |
| **XBBR** | 本特性代号 |
| **入口块** | 函数的第一个 BB，与函数符号同地址 |
| **漂移 (drift)** | 非入口 BB 被放置到原函数代码段之外 |
| **热簇 (hot cluster)** | 由跨函数热 BB 拼接成的连续段 |
| **回退 (fallback)** | 取消某 BB 的漂移，将其放回原函数位置 |
| **决策 map** | 记录每个 BB 最终归宿的链接后 section |
| **Thunk / Veneer** | 长跳跃中转代码，跨段跳转超分支范围时插入 |

---

**SPEC 文档结束** —— 实现细节请见 [`PLAN.md`](PLAN.md)。
