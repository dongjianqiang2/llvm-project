# PLAN: 跨函数全局基本块重排 — 设计文档 (XBBR Design)

> **设计文档** —— 描述 XBBR 的实现架构、数据结构、算法、代码组织与落地步骤。
> 需求与契约（做什么、为什么、验收标准）见同目录 [`SPEC.md`](SPEC.md)。本文件回答"怎么做"。
> 与 SPEC 冲突时，以 SPEC 的契约（ABI 不变量、用户接口、验收门槛）为准，并应回写修正 SPEC。

- **代号**：XBBR
- **状态**：设计草案 v0.1
- **日期**：2026-06-17
- **对应 SPEC**：[`SPEC.md`](SPEC.md) v0.1

---

## 目录

1. [整体架构](#1-整体架构)
2. [数据流与文件交互](#2-数据流与文件交互)
3. [编译器侧设计](#3-编译器侧设计)
4. [链接器侧设计](#4-链接器侧设计)
5. [调试与可观测性实现](#5-调试与可观测性实现)
6. [确定性构建](#6-确定性构建)
7. [代码组织与目录结构](#7-代码组织与目录结构)
8. [关键数据结构](#8-关键数据结构)
9. [新增 ELF Section 二进制格式](#9-新增-elf-section-二进制格式)
10. [里程碑实现分解](#10-里程碑实现分解)
11. [风险与缓解](#11-风险与缓解)

---

## 1. 整体架构

```
              ┌───────────────────────┐
              │  Source + IRPGO data  │  (-fprofile-instr-generate / -use)
              └──────────┬────────────┘
                         │
            ┌────────────▼─────────────┐
            │  clang + LLVM middle-end │
            │  ─ MD_prof on IR         │
            │  ─ MBFI / MBPI on MIR    │
            └────────────┬─────────────┘
                         │
       ┌─────────────────▼─────────────────────┐
       │  LLVM back-end (per-function)         │
       │  ─ MachineBlockPlacement (复用)       │
       │  ─ MachineFunctionSplitter (复用，    │
       │    扩展 "可跨函数迁移" 标记)          │
       │  ─ XBBRMetadataEmitter (新增)         │
       │       生成:                           │
       │       · .text.<fn>.<bbid>             │
       │       · SHT_LLVM_BB_ADDR_MAP (复用,   │
       │         启用 FuncEntryCount/BBFreq/   │
       │         BrProb PGO feature)           │
       │       · .llvm.call_graph_profile      │
       │         (复用, 含间接调用边)          │
       │       · .llvm_xbbr_attr  (新, 黑名单) │
       └─────────────────┬─────────────────────┘
                         │  (.o files)
              ┌──────────▼──────────┐
              │  lld --bb-cross-    │
              │  reorder pipeline   │
              ├─────────────────────┤
              │ Stage 0: 收集元数据 │   全局 BB graph 构建
              ├─────────────────────┤
              │ Stage 1: hfsort+/C³ │   函数簇粗排，使用 CGProfile
              ├─────────────────────┤
              │ Stage 2: ExtTSP +   │   簇内 BB 精排 (跨同簇函数)
              │   PH chain merge   │
              ├─────────────────────┤
              │ Stage 3: 多目标代价 │   微调，权重 CLI 可调
              │   函数 (I$/iTLB/   │
              │   BTB/size)         │
              ├─────────────────────┤
              │ Stage 4: 单 BB 约束 │   超分支距离 / EH 冲突 / 黑名单
              │   求解与回退        │   触发自动回退到原函数位置
              ├─────────────────────┤
              │ Stage 5: section    │   写出 .text.hot / .text.warm /
              │   emission +        │   .text.unlikely + thunk 生成 +
              │   DWARF/CFI 重写    │   .debug_xbbr_decision (决策 map)
              └─────────────────────┘
```

### 1.1 设计原则

1. **契约优先**：SPEC §5 的 ABI 不变量是不可违反的硬约束，所有算法遇到约束冲突时必须回退而非绕过。
2. **复用优先**：能复用 LLVM/lld 既有设施就不重写（见 §3.1、§4.1 的复用清单）。
3. **可插拔算法**：粗排/精排/代价函数三层解耦，通过 CLI 切换实现，便于 A/B 与未来 ML 替换。
4. **元数据自包含**：所有布局输入随 `.o` 文件流转，链接器不依赖外部 profile 文件即可决策（决策权由 `--bb-cross-reorder` 显式触发）。
5. **链接后零运行时开销**：除决策 map（可剥离）外，新增 section 不进入可加载段。

---

## 2. 数据流与文件交互

```
┌──────────┐   IRPGO      ┌──────────────┐   .o (含 XBBR 元数据)   ┌──────────┐
│  源码    ├─────────────▶│  clang/llc   ├──────────────────────▶│   lld    │
└──────────┘  .profdata   └──────────────┘                         └────┬─────┘
                                                                          │
                          ┌───────────────────────────────────────────────┘
                          ▼
                  ┌───────────────┐   .debug_xbbr_decision    ┌──────────────────┐
                  │  最终 ELF     ├───────────────────────▶│ llvm-bbreorder-  │
                  │ (.text.hot/   │                         │ dump / perf /    │
                  │  unlikely)    │                         │ addr2line / gdb  │
                  └───────────────┘                         └──────────────────┘
```

关键边界：
- **编译器 → 链接器**：仅通过 `.o` 文件内的 XBBR sections（BB_ADDR_MAP PGO feature + CGProfile + `.llvm_xbbr_attr`）传递，不依赖外部文件。
- **链接器 → 工具**：仅通过 `.debug_xbbr_decision` 暴露决策，便于事后审计与 BOLT 二次消费。
- **频率归一化**（见 §3.2）：`FuncEntryCount` 与 `BBFreq` 均由 BB_ADDR_MAP 携带，链接器 Stage 0 组合得 `global_freq`——归一化在链接器侧完成，编译器只需启用 BB_ADDR_MAP 的 PGO feature。
- **Full LTO**（SPEC §8.3）：`-flto=full` 下无独立 `.o`，`XBBRMetadataEmitter` 在 LTO CodeGen 按 partition 运行，元数据随 partition 输出流入 lld，数据流与非 LTO 等价；ThinLTO 不支持。

---

## 3. 编译器侧设计

### 3.1 复用的现有设施

| 组件 | 复用方式 | 扩展点 |
|---|---|---|
| `MachineBlockPlacement` | 函数内 baseline ordering 不变 | 输出 BB 频率给 emitter |
| `MachineFunctionSplitter` (`-fsplit-machine-functions`) | 冷热分离 | 标记每 BB "可跨函数" 位 |
| `BasicBlockSections` (`-fbasic-block-sections`) | 自动启用 `=labels` 模式 | 新增 `=cross-reorder` mode |
| `SHT_LLVM_BB_ADDR_MAP` | 携带 BB 偏移；**XBBR 启用其 PGO-analysis feature**（`-pgo-analysis-map=func-entry-count,bb-freq,br-probe`）以复用 `FuncEntryCount` / `BBFreq` / `BrProb` | `BBEntry::Metadata` 已含 `IsEHPad`/`HasIndirectBranch`/`HasTailCall`/`HasReturn`/`CanFallThrough`；XBBR 仅需补 `is_musttail`/`has_setjmp`/`has_inline_asm_label`/`user_blacklisted`/`cold` 等少量新位（见 §3.4、§9.3） |
| `.llvm.call_graph_profile` (CGProfile) | 函数级粗排输入；**跨函数调用边**（含 IRPGO 值画像派生的间接调用边，见 §3.3）由此承载 | 不变（边以符号名编码，见 §9.2） |

> **复用优先（落实 §1.1）**：`BBAddrMap` 既有 `FuncEntryCount`+`BBFreq`+`BrProb` 已覆盖"全局频率"与"函数内 CFG 边权重"的全部需求。因此 **`.llvm_bb_freq` / `.llvm_cfg_edge` 不再作为独立新 section**——频率与边权重直接取自 BB_ADDR_MAP（函数内）+ CGProfile（跨函数调用边）。仅保留 `.llvm_xbbr_attr` 承载 XBBR 专属黑名单位（§9.3）。下文 §9.1/§9.2 保留独立格式仅作"无法扩展 BB_ADDR_MAP 时的回退方案"。

### 3.2 全局频率归一化

IRPGO 的 BB 频率是函数局部的。XBBR 将每个 BB 频率归一到全局尺度以跨函数比较：

```
global_freq(BB) = local_freq(BB) × entry_count(F)
local_freq(BB)  = P(BB | entry_F)   // MBFI 的相对频率，以入口为 1.0 归一化
entry_count(F)  = F 的绝对入口执行次数
```

- **数据来源（均已存在于 `SHT_LLVM_BB_ADDR_MAP`，无需新 section）**：
  - `BBFreq(BB)` ← `BBAddrMap::BBEntry` 的 `BBFreq`，来自 `MachineBlockFrequencyInfo`。**注意**：BBAddrMap 存的是 BFI **原始**定标频率（入口块取实现最大值，**非** 1.0），故 `local_freq = P(BB|entry)` 需用入口块原始频率归一化，链接器实际计算为
    `global_freq(BB) = BBFreq(BB) × FuncEntryCount / BBFreq(entry_block)`（`lld/ELF/XBBR/XBBRGraph.cpp::computeGlobalFreq`，128-bit 防溢出）。早先公式 `local_freq × entry_count` 把 `local_freq` 当作已归一的相对频率，与 BBAddrMap 实际存储的原始定标不符——回写 2026-06-21 修正。
  - `entry_count(F)` ← `BBAddrMap` 的 `FuncEntryCount`（来自 `MF.getFunction().getEntryCount()`，由 `PGOInstrumentationUse` 填充，**非**直接读取 `__llvm_prf_cnts`）。
  - `FuncEntryCount` 为绝对计数、`BBFreq` 为 BFI 原始定标；归一化后的乘积即得绝对执行次数。链接器在 Stage 0 直接组合二者，无需编译器侧再发 `.llvm_bb_freq`。
- **语义正确性（回应评审：公式非缺陷）**：`local_freq` 是条件概率 `P(BB|entry)`，已天然聚合所有 callsite。冷调用路径的 BB 其 `local_freq` 已反映低条件概率，乘积给出正确的绝对计数，不存在"高估"。提前返回函数的出口块 `local_freq < 1.0`，乘积同样正确。递归函数的 `entry_count` 是**实测的含递归总调用次数**（非会复利的乘子），`local_freq × entry_count` 正确给出绝对执行次数，无"几何放大"。上下文敏感（按 callsite 拆分）是 flat PGO 的已知限制，列入 SPEC §11 未来工作。
- **递归函数在聚类中的去重（真正需注意处）**：Stage 1 计算函数级密度应使用 `entry_count(F)`（调用次数），**而非** `Σ_BD global_freq(BB)`——后者会随递归深度膨胀，使递归函数被过度优先。函数内 BB 级布局（Stage 2）才使用 per-BB `global_freq`。递归自调用边 `F→F` 复用 `CallGraphSort` 既有处理（自环跳过/截断）。
- **缺失 profile 的回退**：若 `getEntryCount()` 返回 `None`（函数未被插桩/profile 不含该函数），则 `global_freq` 不可比较，该函数按 SPEC §3.1 视为冷、不参与跨函数迁移。
- 跨函数**调用边**权重取自 CGProfile（含间接调用边，见 §3.3）。

### 3.3 新增 Pass：`XBBRMetadataEmitter`

- **位置**：`MachineBlockPlacement` 之后、`AsmPrinter` 之前。
- **触发条件**：`-fbb-cross-reorder={partial,full}` 启用时（`function` 模式不需要 BB 元数据，仅需 CGProfile）。
- **substrate 副作用（回写 2026-06-21，对应 SPEC §6.3）**：`partial`/`full` 在 `clang/lib/CodeGen/BackendUtil.cpp` 同时置 `Options.BBSections = All` + `FunctionSections = true` + `UniqueBasicBlockSectionNames = true`，使每个 BB 成为独立 InputSection——这是链接期物理重排的 substrate（§4.3 Stage 5）。AArch64 额外经 `cl::opt` 桥禁用 `aarch64-enable-branch-relax`，否则 `addPostBBSections` 在 BB-sections 成形后跑 BranchRelaxation 会插入未跟踪 MBB 破坏 BBAddrMap；用户手传 `-fbasic-block-sections=all` 的 AArch64 驱动拒绝保留，XBBR 经 BackendUtil 绕过。`function` 模式不触发上述副作用。
- **输入**：MBFI、MBPI、`MachineFunctionSplitter` 的冷热划分、黑名单分析结果。
- **输出**（遵循 §3.1 复用原则，避免与既有 section 重复）：
  - **频率 + 函数内 CFG 边权重**：通过启用 BB_ADDR_MAP 的 `FuncEntryCount`/`BBFreq`/`BrProb` feature 承载（`-pgo-analysis-map=...`），不再单独发 `.llvm_bb_freq`/`.llvm_cfg_edge`。
  - **跨函数调用边（含间接调用）**：由 CGProfile 承载。XBBRMetadataEmitter 确保 **IRPGO 间接调用值画像（`IPVK_IndirectCallTarget` VP 数据）**被纳入 CGProfile，使 C++ 虚调用 / 函数指针调用的热边不缺失（回应评审 #11：仅靠直接调用 CGProfile 会漏掉 vtable dispatch 热路径）。VP 不可用时该类边缺失，Stage 1/2 据实降级。
  - **`.llvm_xbbr_attr`**：per-BB XBBR 专属黑名单位掩码（§3.4、§9.3）。`BBEntry::Metadata` 已有的 `IsEHPad`/`HasIndirectBranch`/`HasTailCall` 等位优先复用，不重复。

### 3.4 黑名单 BB 判定

实现为 `XBBRMetadataEmitter` 内的静态分析，对每个 BB 检查 SPEC §5.3 的条件，结果写入 `.llvm_xbbr_attr` 位掩码（格式见 §9.3）。判定逻辑：

| 位 | 条件 | 判定方式 | 来源 |
|---|---|---|---|
| `is_entry` | 函数入口 BB | `MBB.getNumber() == 0`（等价于 BB_ADDR_MAP 首个 range 的首 BB） | 派生 |
| `is_landing_pad` | EH landing pad | `MBB.isEHPad()` | **复用 `BBEntry::Metadata::IsEHPad`** |
| `is_indirectbr_target` | indirectbr/callbr 目标 / `blockaddress` 取址 / inline asm goto 间接目标 | `MBB.hasAddressTaken()` 或 `MBB.isInlineAsmBrIndirectTarget()`；可由 `BBEntry::Metadata::HasIndirectBranch` 辅助 | 部分复用 |
| `has_setjmp` | 含 setjmp/longjmp | (a) `CallBase::hasFnAttr(ReturnsTwice)` 命中 setjmp 系列；(b) callee 名匹配 `longjmp` / `_longjmp` / `siglongjmp` 等命中 longjmp 系列（glibc longjmp 无 IR 级标记，只有 `noreturn`，但 `noreturn` 会误命中 `abort/exit/__cxa_throw` 等，故必须用名匹配） | 新位 |
| `has_inline_asm_label` | inline asm 内含 `.section`/`.pushsection`/`.popsection` 指令 | 扫描 `InlineAsm::getAsmString()` | 新位 |
| `is_musttail` | musttail call | **`MF.getFunction()` 上遍历 BB，调用 IR `BasicBlock::getTerminatingMustTailCall()` 或 `CallBase::isMustTailCall()`** | 新位 |
| `user_blacklisted` | 用户黑名单（`-fbb-cross-reorder-blacklist=<file>` 列出的函数） | 读黑名单文件，命中函数的非入口 BB 全部置位（入口 BB 本来就锚定，不重复置位） | 新位 |
| `is_no_return_tail` | `noreturn` callsite 后无后继尾块（SPEC §5.3 第 7 项） | MBB 满足 `succ_empty()` 且其父 IR BB 终止指令为 `unreachable`、其前驱指令为 `noreturn` callsite | 新位 |
| `cold` | 冷 BB | 由 `MachineFunctionSplitter` 同步；冷判据见 SPEC §4/§6.1（`global_freq < threshold×entry_count`） | 新位（M3 落地，lld 消费 cold-threshold 时启用） |

> **musttail 检测修正（回应评审 #7）**：musttail 是**调用**指令（`IS_CALL`），不是返回指令。`MachineInstr::isReturn()` 过宽——会误命中普通 `RET` 与所有 `tail call`（后者 lowering 为 `TCRETURN`，亦带 `isReturn`），无法专一识别 musttail。正确做法是回到 IR 级：musttail 调用必为其所在 BB 的终止指令，`BasicBlock::getTerminatingMustTailCall()`（见 `SjLjEHPrepare.cpp`、`GlobalMergeFunctions.cpp` 用法）可精确判定，经 `MF.getFunction()` 在 CodeGen 层可达。`BBEntry::Metadata::HasTailCall` 仅标识任意尾调用，不足以替代。

> **`is_no_return_tail` 的窄检测（SPEC §5.3 第 7 项）**：仅当 BB **同时**满足"无后继(succ_empty)" 与 "终止指令前是 noreturn callsite" 两个条件时才置位。若仅靠 `noreturn` 属性,会把所有调用 `abort/exit/__cxa_throw` 的 BB 都标黑名单——这些 BB 恰恰是冷代码、应被允许迁出，与 SPEC §2 优化目标冲突。两条件 AND 后只命中真正的"函数尾巴",符合 SPEC 第 7 项"归属原函数有助于回溯"的本意。

> **`has_setjmp` 中 longjmp 的命名匹配**：如表中所注，glibc longjmp 的 IR 表达没有 `returns_twice`，只有 `noreturn`。LLVM 也没有 TLI 库函数枚举或 builtin 对应它（只有 SjLj EH 内建 `eh_sjlj_longjmp`，仅在部分 ARM 平台触发）。唯一可靠的检测是按 callee 函数名匹配 `longjmp` / `_longjmp` / `siglongjmp` / `__longjmp` / `_siglongjmp`。同名但非标准库实现（如自定义模拟 longjmp）也会被命中——这是过保守的安全方向，符合 SPEC §5.3 的强约束。

### 3.5 函数内 baseline ordering

`MachineBlockPlacement` 产出的函数内 BB 序列作为"局部 baseline"保留，但仅用于：
1. 生成 `.llvm_cfg_edge` 的 fall-through 边（标记哪些是当前 fall-through）。
2. 作为链接器 ExtTSP 的初始解（加速收敛）。

链接器的最终布局可以完全覆盖此 baseline。

---

## 4. 链接器侧设计

### 4.1 复用的 LLD 设施

| 组件 | 复用方式 | 扩展 |
|---|---|---|
| Propeller pipeline (`SymbolOrderer`, `BPSectionOrderer`) | 框架不变 | 增加 cross-section BB 拼接逻辑 |
| `--call-graph-profile-sort` (`CallGraphSort.cpp`) | hfsort+ 函数级粗排 | 接入 XBBR Stage 1 |
| `Thunk` 机制（`AArch64Thunks.cpp` / `ARMSectCreate.cpp` / x86 `Relocations.cpp`） | long-branch veneer | BB 跨段时按需注入 |
| `OutputSection` 划分 | 仍写入 `.text.hot` / `.text.unlikely` | 增加 `.text.warm` (可选) |

### 4.2 新增管线阶段（`lld/ELF/XBBR/`）

| Stage | 职责 | 输入 | 输出 |
|---|---|---|---|
| Stage 0 | 收集所有 .o 的 XBBR section | `.llvm_bb_freq/_cfg_edge/_attr` | 全局 BB graph |
| Stage 1 | 函数簇粗排（hfsort+ / C³） | CGProfile + 函数总频率 | 函数簇序列 |
| Stage 2 | 簇内 BB 精排（ExtTSP）+ PH chain merge | BB graph + 黑名单 | BB 全序 |
| Stage 3 | 多目标代价函数微调 | Stage 2 序列 + 用户权重 | 最终序 |
| Stage 4 | 约束求解 / 单 BB 回退 | 最终序 + ISA 分支限制 | 可行序 + 回退集 |
| Stage 5 | Section emission + DWARF/CFI/EH 重写 | 可行序 | 输出 ELF + 决策 map |

### 4.3 算法细节

#### Stage 0 — 全局 BB graph 构建

- 遍历所有输入 `.o`，解析 `SHT_LLVM_BB_ADDR_MAP`（PGO feature）+ `.llvm_xbbr_attr` + CGProfile。（§9.1/§9.2 的独立 `.llvm_bb_freq`/`.llvm_cfg_edge` 仅作回退方案，首期不复用。）
- **per-BB InputSection 关联（回写 2026-06-21）**：在 `-fbasic-block-sections=all` substrate（§3.3）下，一个 BBAddrMap section（含 N 个 `BBRangeEntry`）描述一整个函数，每个 range 的 `BaseAddress` 是一条 `R_AARCH64_ABS64` 重定位指向该 BB 的 section 符号。`EF.decodeBBAddrMap` 解析时丢弃符号只留 addend，故 Stage 0 自行重读 RELA 段、按 `r_offset` 排序与 range 位置配对，把每个 range 解析到其 per-BB `InputSectionBase*`，挂在 `XBBRNode::BBSection`（§8.1）。**一个 BBAddrMap = 一个函数 = 一个 `FuncId`**（不是每个 text section 一个 FuncId）；`SectionToFuncId` 把该函数每个 per-BB section 都映射到同一 FuncId。读 `BBEntry::Offset`/`Size` 并与 section 大小做一致性断言。EH gate（`FuncInfo::IsEHGated`，§5.3）在此分类。
- 构建统一的 `XBBRGraph`（见 §8），节点 = BB（携带 `BBSection` 作为物理放置单元），边 = CFG 边（含跨函数调用边）。
- 用 `.llvm_xbbr_attr` 标记黑名单节点为 anchor；`markRangeAnchors` 在 AArch64 上额外把 `R_AARCH64_CONDBR19`/`TSTBR14` 的源与目标标 `CondInvolved`（§4.3 Stage 4）。
- 函数入口块天然为 anchor（SPEC §5.1）。

#### Stage 1 — 函数簇粗排（hfsort+）

- 复用 `CallGraphSort.cpp` 的 hfsort+ 实现，输入为 CGProfile + 函数总频率（来自 `.llvm_bb_freq` 各函数 BB 频率之和）。
- density 优先队列合并函数为簇，密度 = 总执行次数 / 字节。
- 阈值与 `--call-graph-profile-sort` 一致，避免引入新调参面。
- 输出：函数簇序列（簇是有序的，簇内函数无序，留给 Stage 2）。

#### Stage 2 — 簇内 BB 精排（ExtTSP + PH chain merge）

- `ExtTSP`：在簇内所有可迁移 BB 上最大化加权 fall-through 收益。收益函数：
  ```
  benefit(e) = weight(e) · f(distance, is_fallthrough)
  ```
  其中 `distance` 为两 BB 在最终布局中的距离，`is_fallthrough` 表示是否相邻。
- 黑名单 BB 视为 anchor，位置固定，其他 BB 围绕其重排。
- `Pettis-Hansen chain merge`：作为 ExtTSP 后处理，处理悬挂边（被多个簇共享的 BB），合并短链。
- 跨函数 BB 在此阶段发生：同簇不同函数的 BB 可被穿插排列。

**跨函数规模适配（回应评审 #3）**：原始 ExtTSP 文献面向单函数（数百节点）。XBBR 跨函数后单簇可达数万 BB，必须显式处理可扩展性：
- **复杂度澄清**：LLVM 既有 ExtTSP（`llvm/lib/Transforms/Utils/CodeLayout.cpp` 的 `computeExtTspLayout`）是**贪心边驱动合并**（初始每 BB 一簇，按收益用优先队列合并，配合 union-find），约 `O(E log E)`，**非**最优 TSP 的 `O(n³)`。故数万节点在计算上可行，但内存与质量退化仍是风险。
- **簇大小上限**：Stage 1 hfsort+ 合并时设 `max_bbs_per_cluster` / `max_bytes_per_cluster` 阈值（沿用 hfsort+ 的 hot-section 体积阈值机制）。超限簇在 Stage 2 **按热子路径切分为子簇**，ExtTSP 先在各子簇内排序，再按子簇间最大收益边串接。子簇划分用稳定 tie-breaker（§6.2）保证确定性。
- **PH chain merge 跨函数语义**：跨函数边（来自 CGProfile，含间接调用边）的权重直接作为 chain 合并的边权；不同函数的 BB chain 沿调用边合并时，函数入口块（anchor）作为不可移动的分界，chain 不得跨越 anchor 拼接。被多簇共享的悬挂 BB 按其最重归属簇归并，避免重复放置。
- **降级**：簇仍超限或 ExtTSP 未收敛时，该簇回退到"函数入口顺序 + 冷尾"的 baseline，仅 Stage 1 的簇间顺序生效。

#### Stage 3 — 多目标代价函数微调

```
Cost(layout) = Σ w_icache · ICacheLineCrossings(e)
             + Σ w_itlb   · TLBPageCrossings(e)
             + Σ w_btb    · BTBPressure(branch)
             + Σ w_size   · SizeOverhead(thunk_bytes + align_padding + fde_split + eh_frame_hdr)
```

- 权重通过 `--bb-cross-reorder-weights` 传入。
- 嵌入式默认 `size=4, icache=2, itlb=2, btb=1`；服务端默认 `icache=4, itlb=2, btb=1, size=1`。
- 微调采用局部搜索（swap / move 单个 BB），接受降低总 Cost 的移动，迭代至收敛或步数上限。

**地址投影模型（回应评审 #4：代价↔地址循环依赖）**：`ICacheLineCrossings`/`TLBPageCrossings`/`BTBPressure` 依赖具体地址，而最终地址在 Stage 5 才确定。Stage 3 不等待真实地址，而是基于**投影布局**计算代价：
- 给定 Stage 2 的 BB 全序，按各 BB 的（对齐上限截断后的）对齐要求与预估 size 顺序累加偏移，得到每个 BB 的**投影偏移**（`projected_offset(BB) = Σ_{前置} aligned_size`）。`ICacheLineCrossings`/`TLBPageCrossings` 在投影偏移上按 cache-line（64B）/ page（4KB）边界判定，`BTBPressure` 按投影距离判定。
- `SizeOverhead` 中的 `thunk_bytes` 此阶段为**估计值**（按超距分支数 × ISA thunk 单字节宽估算），`align_padding` 为投影布局中的对齐 NOP，`fde_split`/`eh_frame_hdr` 按漂移函数的段数估算。
- **Stage 5 终态复核**：emit 时 thunk 字节数与对齐填充定型，可能与估计值有偏差。Stage 5 末尾用真实地址重算 Cost 增量；若超出 §9.2 体积预算或新引入超距分支，触发**末梢回退**（交还 Stage 4 处理）。即"投影逼近 → emit 定型 → 末梢复核"三步，打破循环而非消除依赖。
- **对齐上限（回应评审 #10）**：迁移 BB 的对齐要求取 `min(原对齐, --bb-cross-reorder-max-align)`（默认 16B，热 BB 可至 cache-line 64B），避免高对齐入口块的对齐要求污染普通 BB 造成 NOP 膨胀。`align_padding` 计入 `SizeOverhead`，超 §9.2 预算则触发回退。

#### Stage 4 — 单 BB 约束求解与回退

> **落地现状（回写 2026-06-21；P0-1 更新 2026-06-21）**：下文伪码描述设计目标态。当前 AArch64 实现分两层：
> - **CondInvolved 守卫（保守，已落地）**：`XBBRGraph::markRangeAnchors` 在 Stage 0 即把 `R_AARCH64_CONDBR19`（B.cond ±1MiB）/`R_AARCH64_TSTBR14`（TBZ/TBNZ ±32KiB）的源与目标标 `CondInvolved`，`BBLayout::collectMigratableBBs` 把它们当 anchor（永不迁移）。理由：这两类重定位**不可被 lld thunk**（只有 B/BL `JUMP26`/`CALL26` 可），迁移其端点会在写时 `checkInt` 硬报错；守卫式 pinning 100% 安全，代价是条件分支密集函数的 BB 级迁移受限（函数级聚簇仍生效）。投影-VA 精细化（允许范围内条件分支 BB 迁移）是 P1-3 后续工作。
> - **B/BL 投影-VA 求解 + thunk 预算 + 30% 降级 + fallback=none（P0-1，已落地）**：`ConstraintSolver::runConstraintSolver` 复用 `CostFunction::computeProjectedOffsets` 把 `ClusterBBOrders` 投影到全局字节偏移，遍历每个 BB 的 per-section 重定位找 **thunkable** B/BL（`R_AARCH64_JUMP26`/`CALL26`），若投影 src→dst 距离超 ISA 范围（±128MiB）则计一条 over-range 边；`estThunkBytes = overRange × 16`（`AArch64ABSLongThunk`）。超 `--bb-cross-reorder-max-thunk-bytes` 预算时，贪心 pin（回退原函数位）涉及 over-range 边最多的可迁移 BB，单调收敛（pin 后永不再迁移，≤ `num_migratable+1` 迭代）；超 30% 阈值则整体降级 `function` 模式（SPEC §7），`--bb-cross-reorder-fallback=none` 下直接 fatal error。`XBBRLayoutResult::EstimatedOverRangeEdges`/`EstimatedThunkBytes` 记录估计值供 P0-2 末梢复核比对。B/BL 超距的**真实** thunk 仍由既有 `finalizeAddressDependentContent` thunk 循环插入（Stage 4 只做预算闸，不插 thunk）。
> - **仍为后续工作**：P0-2 末梢体积复核（用真实 `ThunkSection` 大小填 `ThunkBytes`/`ActualCost` 并超预算告警）、松弛复检（§4.5，`relax_recheck`）、P1-3 CondInvolved 投影-VA 精细化。
>
> **测试钩子（hidden）**：`--bb-cross-reorder-branch-range-for-testing=N`（`Flags<[HelpHidden]>`，0=用真 ISA 范围）缩放 Stage 4 投影判距，使预算/降级路径可用极小二进制（而非 128MiB filler）测试；仅影响 XBBR 投影估计，**不**影响 lld 真实 thunk 插入（始终 ISA 范围）亦**不**影响 CondInvolved 守卫。测试：`xbbr-aarch64-jump26-thunk.s`（超距 B/BL 经 lld thunk，XBBR 不 pin）、`xbbr-thunk-budget-revert.s`（超预算单 BB 回退）、`xbbr-fallback-degrade.s`（30% 降级）、`xbbr-fallback-none.s`（fallback=none fatal）。

```python
pinned = {}          # FuncId -> set(BB)，已回退/锚定的 BB，永不再迁移
fallback_count = 0
global_threshold = max(0.30 * num_migratable, fallback_budget)   # 默认 30% 或 CLI 指定
MAX_ITERS = num_migratable + 1   # 单调收敛上界（见下），作为安全网

for iteration in range(MAX_ITERS):
    changed = False
    for each migrated BB B not in pinned:        # 仅检查仍漂移者
        if B in blacklist (assertion/sanity) \
           or B.CondInvolved (AArch64 B.cond/TBZ 端点，已前置 pin) \
           or branch from/to B exceeds ISA range and thunk would exceed budget \
           or B's EH range conflicts with neighbor placement \
           or relax_recheck(B) violates range:    # 链接器松弛后复检（§4.5）
            revert(B)            # 放回原函数入口块之后、下一 anchor 之前
            pinned.add(B)        # ★ 关键：pin，永不再迁移 → 单调
            fallback_count += 1
            changed = True
    if fallback_count > global_threshold:
        degrade_to_function_mode()   # 整体降级（SPEC §7）
        break
    if not changed:
        break                       # 收敛
else:
    # 触达安全网上界：保守降级，绝不无限循环
    degrade_to_function_mode()
```

**收敛性保证（回应评审 #5：震荡风险）**：
- **Pin 即单调**：`revert(B)` 后 B 进入 `pinned`，不再被重新迁移。每次回退使"仍漂移 BB 集合"严格缩小，因此循环至多迭代 `num_migratable` 次必然终止——**无震荡**（震荡需"un-revert"，已禁止）。
- **级联是单向的**：revert B 回原位可能使某跨簇分支 B↔C 超距 → C 亦 revert。这是单调传播（更多 BB 回家），非震荡。
- **安全网**：`MAX_ITERS = num_migratable + 1` 为兜底；正常不触达，触达则整体降级而非挂起。
- **`global_threshold` 取值**：默认 `max(0.30 × num_migratable, fallback_budget)`，即超过 30% 可迁移 BB 被回退即判定 layout 退化、整体降级到 `function` 模式（SPEC §7）。可由 `--bb-cross-reorder-fallback=conservative` 收紧、`none` 关闭回退（CI 严格模式直接报错）。
- 回退是单 BB 粒度，不撤销整个函数的重排；回退动作产生 lld warning（`-Werror` 友好，SPEC §7）。

#### Stage 5 — Section Emission

> **落地现状（回写 2026-06-21，AArch64；P0-2 更新 2026-06-21）**：物理重排**已落地**——`Writer.cpp::buildSectionOrder` 在非降级布局时从 `XBBRLayoutResult::ClusterBBOrders` 产出 per-BB `InputSection` 优先级 map，喂入既有 `sortISDBySectionOrder`：迁移的 per-BB section 按 XBBR 序物理放置，入口/anchor/EH-gated BB 留在 unordered 集合（由符号跟踪其位置 → ABI §5.1 自动成立，ordered 块插 unordered 中点以最小化 B/BL thunk）。`B`/`BL` 超距由既有 `finalizeAddressDependentContent` thunk 循环自动插入 `AArch64Thunks`（无需 XBBR 专用 thunk 代码）。`optimizeBasicBlockJumps` 之后 `backfillDecisionMapVAs` 把决策 map 的 `OrigFuncAddr`/`NewAddress` 从占位/投影值改为真链后 VA（§9.4）。**P0-2 末梢体积复核（2026-06-21）已落地**：`backfillDecisionMapVAs` 遍历 `ctx.outputSections` 的所有 `.text.thunk` ThunkSection 累加真实字节数填 `XBBRLayoutResult::ThunkBytes`、`ActualCost = w_size × ThunkBytes`，与 Stage 4 的 `EstimatedThunkBytes` 比对；超 `--bb-cross-reorder-max-thunk-bytes` 则发 warning（地址已定，无法末梢回退——pre-emit 回退是 P0-1 的职责，末梢复核仅告警暴露投影估计漏计的非-BB gap 距离）。测试：`xbbr-tail-end-thunk-recheck.s`。**P1-1 热冷段分离（2026-06-21）已落地**：`Driver` 在 `processSectionCommands` 前建临时图调 `renameSectionsForHotColdSplit`，按 hot/cold/original 分类把 per-BB section 改名 `.text.hot.*`/`.text.unlikely.*`（partial 下 cold 留 `.text`，SPEC §4），并隐含 `-z keep-text-section-prefix`，使 `getOutputSectionName` 路由到独立 output section；真实布局图仍 post-ICF 在 `buildSectionOrder` 重建。测试：`xbbr-hot-unlikely-split.s`（`.text.hot` 出现、plain 无、partial/full 一致、可重现）。**.text.unlikely 路由**代码与 `.text.hot` 对称（同改名机制、`.text.unlikely.` 前缀），但 lit 未直接覆盖——cold 位经 clang driver flag 不流入 IsCold，而 llc 手工 `-basic-block-sections=all` 与 `-enable-xbbr` 组合产生 `xbbr_attr`/BBAddrMap BB 数不一致（3 vs 5），故 cold BB 构造存在编译器侧工具缺口，留待补 clang driver cold-threshold 透传后补测。**尚未落地**（未来工作）：`$xbbr.<fn>.bb<N>` 本地符号别名、对齐上限放置、DWARF/EH 完整重写（§5）。

输出三段（`.text.warm` 可选）：
- `.text.hot`：被识别为热路径簇的 BB（来自所有原函数）。
- `.text.warm`：温度中等的 BB（避免 hot 段过大，嵌入式可关闭）。
- `.text.unlikely`：所有冷 BB（含原函数留守的冷尾巴）。

每函数原始 `.text.<fn>` 不再使用（除非该函数完全未参与重排）。同时：
- 生成跨段跳转所需的 thunk（ARM/AArch64）。
- 重写 DWARF/CFI/EH（见 §5）。
- 写入 `.debug_xbbr_decision`（见 §9.4）。
- 为每个迁移 BB 生成本地符号别名 `$xbbr.<orig_func>.bb<N>`（见 §5.2）。
- **BB 对齐**：按 §4.3 Stage 3 的对齐上限（`--bb-cross-reorder-max-align`，默认 16B）放置迁移 BB，避免高对齐入口块的对齐要求污染普通 BB。对齐填充 NOP 计入可加载体积预算（§2.2）；Stage 5 末梢复核若超预算，触发末梢回退（§4.3 Stage 3）。

### 4.4 分支距离与 thunk 预算

| 架构 | 直接分支范围 | 超范围处理 |
|---|---|---|
| x86_64 | ±2GB（32-bit rel） | 极少触发，E9 jmp 直接生成 |
| AArch64 | ±128MB（B/BL） | 复用 `AArch64Thunks`，注入 stub |
| ARM (A32) | ±32MB（B/BL） | 复用 `ARMSectCreate`，注入 veneer |
| ARM (Thumb-2) | ±16MB（B/BL） | 同上，注意 ARM↔Thumb 互操作 |

thunk 总字节数受 `--bb-cross-reorder-max-thunk-bytes` 约束，超限触发对应 BB 回退（SPEC §7）。

> **P1-3 条件分支放宽（回写 2026-06-21）**：上表只列 B/BL（thunkable）。AArch64 B.cond（CONDBR19 ±1MiB）/ TBZ（TSTBR14 ±32KiB）**不可 thunk**——超距是硬链接错误。`markRangeAnchors` 标记 `CondInvolved`；P1-3 不再无条件 pin，而是：当某 CondInvolved BB 所在的**条件分支连通分量全 hot**（`CondSafeToMigrate`，固定点传播计算——跨段不安全性沿伙伴图传染，一个非 hot 伙伴 pin 整个分量）时允许迁移到 .text.hot。常见 entry→target 情形（entry 为 anchor 非 hot）由 `collectMigratableBBs` 前置 pin（不计入 Stage 4 回退，避免伪降级）；Stage 4 `collectCondPins` 仅处理罕见的 .text.hot 内超距（投影距离 > range×0.9 margin → pin 双端，级联回退）。ARM `R_ARM_THM_JUMP11`（Thumb-1，lld 不支持）仍无条件 pin。测试旋钮 `--bb-cross-reorder-cond-range-for-testing=N`。溢出是**可捕获的硬链接错误**（非静默），0.9 余量覆盖投影未建模的 B/BL thunk 增量。

### 4.5 链接器松弛（Linker Relaxation）交互

**问题（回应评审 #8）**：lld 在 AArch64 上由 `AArch64Relaxer` 做 `ADRP+ADD→ADR`、`ADRP+LDR` 折叠等代码松弛，会**改变 BB 字节大小**。而松弛是否适用依赖最终相对地址——XBBR 重排改变布局 → 改变可松弛性 → 改变 size → 反过来影响分支距离与 Stage 3 代价。若 Stage 0 用 pre-relaxation size 建图，post-relaxation 偏移会漂移，布局可能溢出。

**处理策略（M5 落地，SPEC §12 开放问题 #6）**：
1. **读 post-relaxation size**：XBBR Stage 0 在 lld 松弛 pass 完成后读取 BB 的最终 size（即 XBBR 布局 pass 排在松弛之后）。Stage 4 的 `relax_recheck(B)` 用 post-XBBR 布局复算可松弛性；若某 BB 因松弛变形超距，按 Stage 4 回退该 BB。
2. **被重排区域冻结松弛**：对 XBBR 决定迁移的 BB 段，禁用会改变 size 的松弛（保留纯重定位写），从源头消除 size 漂移。代价是放弃少量体积收益，换取布局稳定。
3. **默认采用 (1)+(2) 组合**：post-relaxation size 建图 + 迁移段冻结。`--bb-cross-reorder-deterministic` 模式强制 (2) 全量冻结以保证 bitwise 可重现。
4. ARM/x86_64 无 lld 代码 size 松弛（ARM 仅有 TLS 松弛、x86_64 无），本节主要约束 AArch64。

> 与 §6 确定性协同：松弛适用性的判定须基于稳定输入（reloc + 投影地址），不得依赖遍历序。

---

## 5. 调试与可观测性实现

### 5.1 DWARF / CFI 重写

漂移 BB 导致函数代码非连续，需重写调试信息：

- `DW_TAG_subprogram`：由单一 `low_pc/high_pc` 改为 `DW_AT_ranges`，列出该函数所有 BB 段。
- `.debug_line`：在每个 BB 段起始处插入 `DW_LNE_set_address`，重置地址基。
- `.debug_aranges` / `.debug_ranges`：拓展为多段列表。
- `.eh_frame`：FDE 拆分（见 §5.3）。
- ARM `.ARM.exidx`：多段化（见 §5.4）。

实现位置：lld Stage 5 的 `DWARFRewriter`（新增，复用 lld 现有 DWARF 解析基础设施）。

### 5.2 符号别名

为每个跨函数迁移的 BB 生成**本地符号**（不进入动态符号表，不增加运行时开销）：

```
$xbbr.<orig_func>.bb<N>
```

便于 `addr2line` / `perf annotate` / `gdb x/i` 显示具体 BB 归属。本地符号在 lld Stage 5 写入 `.symtab`。

### 5.3 EH 重写（`.eh_frame` / `.gcc_except_table` / `.eh_frame_hdr`）

> **落地现状与关键简化（回写 2026-06-21）**：在 `-fbasic-block-sections=all` substrate 下，编译器对每个 per-BB section 发一个 FDE（实测 3-BB 函数 = 3 个 FDE），FDE 的 PC-begin 是指向该 BB section 的重定位。因此**普通 unwind 的 FDE 随 BB 迁移自动跟随**（PC-begin 重定位解析到迁移后地址）——**无需 EHRewriter 做 FDE 拆分**。唯一破坏是 **LSDA**：`.gcc_except_table.<fn>` 的 call_site 范围是相对入口的字节偏移，非 landing-pad BB 一旦个体漂移就错位。故 mechanism-first 阶段采用 **EH gate**（`FuncInfo::IsEHGated`，Stage 0 分类）：函数有 `.gcc_except_table.<fn>` 或 landing pad 即标 gated，`BBLayout::collectMigratableBBs` 把该函数**所有** BB 当 anchor（个体不漂移）；**整体移动仍允许**（LSDA 偏移是入口相对，整体平移后仍正确）。这保证 SPEC §5.4 栈展开正确，且 FDE 拆分 / `.eh_frame_hdr` 重建 / LSDA 重写可整体推迟到"解除 EH gate"的后续工作。下文 FDE 拆分 + `.eh_frame_hdr` 重建是**未来 EHRewriter** 的设计，当前未实现。

- 每个独立放置的 BB 段生成单独 FDE，CIE 中 personality routine 保持一致。
- 原 FDE 拆分为 N 个，各自 `initial_location` + `address_range` 精确覆盖该 BB 段。
- `.gcc_except_table` 的 LSDA call_site_table 用绝对地址重写，与漂移后地址对齐。
- **`.eh_frame_hdr` 重建（回应评审 #9，correctness-critical）**：`.eh_frame_hdr` 是运行时二分查找 FDE 的表（lld `EhFrameHeader` 合成段，由 `EhFrameSection::getFdeData()` 扫描**存活 FDE 集**构建）。FDE 由 1 拆 N 后，若不重建该表，`_Unwind_RaiseException` / `_Unwind_Backtrace` 的二分查找会落到错误/缺失的 FDE，**异常分发与栈展开失败**。
  - **顺序约束**：`EHRewriter` 必须在 `EhFrameHeader` 生成**之前**完成 FDE 拆分，使拆分后的 N 个 FDE 作为存活 FDE 被 `getFdeData()` 扫到、按 `initial_location` 重新排序建表。即 EHRewriter 产出的 FDE 列表直接喂给既有 `EhFrameHeader` 流程，无需新写建表代码。
  - 漂移函数的表项数从 1 增至 N，`.eh_frame_hdr` 体积增长计入 §2.2 体积预算（每条 ~8B）。
- ARM `.ARM.exidx` 多段化见 §5.4。
- 实现位置：lld Stage 5 的 `EHRewriter`（新增，复用 lld 既有 `EhFrameSection`/`EhFrameHeader` 基础设施）。**当前为 stub/未实现**；EH gate 在其落地前保证正确性。

### 5.4 ARM/AArch64 栈展开（`.ARM.exidx`）

- 每段 BB 生成独立 `.ARM.exidx` 条目，对齐至段起点。
- 不可压缩表（`--exidx-unwind`）情况下段间间隔需填充 `EXIDX_CANTUNWIND`。
- Thumb 函数的段入口需保持对齐与 `[1:0]` 位标志一致。

### 5.5 决策 Map Section：`.debug_xbbr_decision`

默认保留，可被 `strip --strip-debug` 剥离。格式见 §9.4。供后续工具反向查询：

```
(原函数, BB 索引) → (新地址, 新所属簇, 决策标志)
```

### 5.6 可视化工具：`llvm-bbreorder-dump`

新增工具（`llvm/tools/llvm-bbreorder-dump/`），能力：
- 解析 `.debug_xbbr_decision` 与 `SHT_LLVM_BB_ADDR_MAP`。
- 列出每个原函数的 BB 去向。
- 输出 Graphviz 热路径图。
- 与 `perf script` 协作生成"按热簇看 cycles"的报告。
- 与 BOLT data 协作（BOLT 消费决策 map 做边角微调）。

---

## 6. 确定性构建

### 6.1 确定性要求来源

SPEC §9.3 要求同输入产出 bitwise-identical 二进制。这是 CI gate，必须从设计层保证。

### 6.2 实现约束

- **所有内部排序使用稳定 tie-breaker**：基于 `(input_file_index, section_index, offset)` 的字典序，不得依赖指针值、map 迭代序、`SmallPtrSet` 顺序。
- **禁用非确定性数据结构作为排序键**：涉及排序的容器必须显式排序后再遍历。
- **`--bb-cross-reorder-deterministic` 模式**：禁用任何并行重排（如 Stage 2/3 的并行簇处理），强制串行。
- **thunk 地址分配确定性**：thunk 的插入顺序由 BB 全序决定，不接受运行时分配。

### 6.3 验证

CI 对每个 `(arch × output-type × mode)` 三元组构建两次，比对 SHA256。

---

## 7. 代码组织与目录结构

```
llvm/
├── include/llvm/
│   ├── CodeGen/
│   │   └── XBBRMetadata.h            # XBBRMetadataEmitter pass 接口
│   └── Object/
│       └── XBBRSections.h            # 新增 section 类型常量与解析
├── lib/CodeGen/
│   └── XBBRMetadataEmitter.cpp       # 新 pass 实现
└── tools/
    └── llvm-bbreorder-dump/
        └── llvm-bbreorder-dump.cpp   # 可视化工具

lld/
├── include/lld/ELF/
│   └── XBBR/
│       ├── XBBRGraph.h               # 全局 BB graph 数据结构
│       ├── XBBRLayoutStrategies.h    # 算法策略抽象（hfsort+/ExtTSP/custom）
│       └── XBBRTypes.h               # BB/簇/边类型定义
└── ELF/
    ├── XBBR/
    │   ├── XBBRGraph.cpp             # Stage 0
    │   ├── FunctionClustering.cpp    # Stage 1 (hfsort+/C³)
    │   ├── BBLayout.cpp              # Stage 2 (ExtTSP + PH)
    │   ├── CostFunction.cpp          # Stage 3
    │   ├── ConstraintSolver.cpp      # Stage 4 (回退)
    │   ├── SectionEmitter.cpp        # Stage 5 (emit)
    │   ├── DWARFRewriter.cpp         # Stage 5 (debug)
    │   └── EHRewriter.cpp            # Stage 5 (EH)
    └── ... (现有文件，按需小改)

clang/
├── include/clang/Driver/
│   └── Options.td                    # 新增 -fbb-cross-reorder=* 选项
└── lib/Driver/ToolChains/
    └── Clang.cpp                     # 选项处理，互斥检测

llvm/test/CodeGen/{X86,AArch64,ARM}/xbbr/   # lit 测试
lld/test/ELF/xbbr/                           # lit 测试
```

### 7.1 算法策略抽象

`XBBRLayoutStrategies.h` 定义统一接口，便于 A/B 与未来 ML 替换：

```cpp
// 函数级粗排策略
class FunctionClusterStrategy {
public:
  virtual ~FunctionClusterStrategy() = default;
  virtual std::vector<FunctionCluster>
  cluster(const XBBRGraph &G, const CGProfile &CGP) = 0;
};
class HFSortPlusStrategy : public FunctionClusterStrategy { /* ... */ };
class C3Strategy        : public FunctionClusterStrategy { /* ... */ };

// BB 级精排策略
class BBLayoutStrategy {
public:
  virtual ~BBLayoutStrategy() = default;
  virtual std::vector<BBOrder>
  layout(const FunctionCluster &C, const XBBRGraph &G) = 0;
};
class ExtTSPStrategy  : public BBLayoutStrategy { /* ... */ };
class PHChainStrategy : public BBLayoutStrategy { /* ... */ };
```

策略通过两个 CLI 开关分别选择（与 SPEC §6.2 一致，回应评审"单 flag 混淆两层策略"）：
- `--bb-cross-reorder-cluster-algo=hfsort+,c3,custom` → 实例化 `FunctionClusterStrategy`（Stage 1）；
- `--bb-cross-reorder-layout-algo=ext-tsp,ph,custom` → 实例化 `BBLayoutStrategy`（Stage 2）。

工厂方法按枚举值实例化；`custom` 加载用户注册的策略（M5+）。`C3Strategy`（Call-Chain Clustering）作为 hfsort+ 的 A/B 对照项保留。

---

## 8. 关键数据结构

### 8.1 `XBBRGraph`（链接器全局视图）

```cpp
struct XBBRNode {
  uint32_t InputFileIdx;   // 输入 .o 索引
  uint32_t FuncId;         // ★ Stage 0 分配的内部函数 ID（非 .symtab 索引）
  uint32_t BBIndex;        // 函数内 BB 索引
  InputSectionBase *BBSection; // ★ per-BB InputSection（=all substrate 的物理放置单元）
  uint64_t Size;           // BB 字节数（post-relaxation，见 §4.5）
  uint64_t GlobalFreq;     // 全局频率（= BBFreq × FuncEntryCount / EntryBBFreq，见 §3.2）
  uint8_t  Attrs;          // 属性位（来自 .llvm_xbbr_attr + BBEntry::Metadata 复用位）
  bool CondInvolved;       // ★ AArch64: B.cond/TBZ 源/目标 → 不可迁移（§4.3 Stage 4）
  bool isAnchor() const {  // 入口块或黑名单 → 固定位置
    return (Attrs & IS_ENTRY) || isBlacklisted();
  }
  bool isBlacklisted() const {
    return Attrs & (IS_LANDING_PAD | IS_INDIRECTBR_TARGET |
                    HAS_SETJMP | HAS_INLINE_ASM_LABEL | IS_MUSTTAIL |
                    USER_BLACKLIST);
  }
};

struct XBBREdge {
  uint32_t SrcNode;        // XBBRNode 索引
  uint32_t DstNode;
  uint64_t Weight;         // 函数内边←BB_ADDR_MAP BrProb；跨函数调用边←CGProfile
  bool     IsFallthrough;  // 当前是否 fall-through（来自 baseline）
  bool     IsCrossFunc;    // 是否跨函数调用边（含间接调用边）
};

class XBBRGraph {
  std::vector<XBBRNode> Nodes;
  std::vector<XBBREdge> Edges;
  DenseMap<FuncId, std::vector<uint32_t>> FuncToBBs;  // 内部函数 ID → BB 节点列表
  // ... 查询接口
};
```

> **稳定引用（回应评审 #6：.symtab 索引不稳定）**：`.symtab` 索引在链接过程中随符号增删/合并/重排而失效，跨 `.o` 更是无意义，**不可作为跨文件引用键**。Stage 0 改用稳定键建立 `FuncId` 内部空间：
> - **函数关联**：复用 BB_ADDR_MAP 的 **section 依附**机制（元数据 section 附在函数 text section 上，AsmPrinter 已用 `FunctionSymbol` 而非 symtab 索引）。
> - **跨函数调用边**：CGProfile 边以 **MCSymbol（符号名）** 编码（`{From, To, Count}`），链接器按符号表解析为 `FuncId`，与既有 CGProfile 消费路径一致。
> - `FuncId` 由 Stage 0 按 `(input_file_index, function_text_section)` 稳定分配，遍历前显式排序（§6.2），保证确定性。

> **数据结构回写（2026-06-21）**：本节结构为设计草图；实现源真在 `lld/ELF/XBBR/XBBRTypes.h`（`XBBRNode`/`XBBREdge`/`BBPlacement`/`XBBRLayoutResult`）与 `XBBRGraph.h`（`FuncInfo`/`XBBRGraph`）。实际字段名与草图有出入（如 `Func`/`BB`/`XBBRAttrs`(u16) 而非 `FuncId`/`BBIndex`/`Attrs`(u8)；`SectionToFuncId` 而非 `FuncToBBs`；`FuncInfo::IsEHGated` 见 §5.3）。新增的 `BBSection`（per-BB 物理放置单元，§4.3 Stage 0）与 `CondInvolved`（§4.3 Stage 4 守卫）已并入草图。`XBBRLayoutResult` 持久化于 `Ctx::xbbrLayoutResult`（Phase 3 回填真 VA）。

### 8.2 `FunctionCluster` / `BBOrder`（布局输出）

```cpp
struct FunctionCluster {
  uint32_t ClusterId;
  std::vector<uint32_t> FuncIds;       // 簇内函数（内部 FuncId，非 .symtab 索引）
  uint64_t TotalFreq;                  // Σ entry_count(F)（非 Σ global_freq，避免递归放大，§3.2）
  uint64_t TotalSize;
  double density() const { return (double)TotalFreq / TotalSize; }
};

struct BBOrder {
  uint32_t ClusterId;
  std::vector<uint32_t> NodeSeq;       // 簇内 BB 全序
  std::vector<uint32_t> FallbackNodes; // 被回退（pinned）的 BB（§4.3 Stage 4）
};
```

### 8.3 决策记录

```cpp
struct XBBRDecision {
  uint64_t OrigFuncAddr;
  uint32_t BBIndex;
  uint64_t NewAddress;
  uint32_t ClusterId;
  enum Flag : uint32_t {
    MOVED     = 1u << 0,
    ANCHORED  = 1u << 1,
    FALLBACK  = 1u << 2,
    THUNK     = 1u << 3,
  } Flags;
};
```

序列化为 `.debug_xbbr_decision`（格式见 §9.4）。

---

## 9. ELF Section 二进制格式

> **复用优先**：频率（`FuncEntryCount`/`BBFreq`）与函数内 CFG 边权重（`BrProb`）**复用 `SHT_LLVM_BB_ADDR_MAP` 既有 PGO-analysis feature**，跨函数调用边复用 `SHT_LLVM_CALL_GRAPH_PROFILE`。故 §9.1/§9.2 的独立格式仅作"无法扩展 BB_ADDR_MAP 时的回退方案"，首期不分配其 section 类型常量。XBBR 真正新增的 section 仅为 `.llvm_xbbr_attr`（§9.3，黑名单）与 `.debug_xbbr_decision`（§9.4，决策 map）。

所有 section 采用**与所在 ELF 的 `EI_DATA` 一致的字节序**（小端 ELF 用小端、大端 ARM/AArch64 用大端，回应评审"小端假设"）、**无填充对齐**，header 固定 8 字节（决策 map 16 字节）。版本号高位为主版本、低位为次版本。函数关联一律通过 **section 依附**（元数据 section 附在函数 text section 上）或 **符号名**，**不使用 `.symtab` 索引**（回应评审 #6）。

### 9.1 `.llvm_bb_freq`（回退方案；首选复用 `BB_ADDR_MAP` 的 `FuncEntryCount`+`BBFreq`）

> **首选**：直接读 `SHT_LLVM_BB_ADDR_MAP` 的 `FuncEntryCount`（绝对入口计数）与每 BB 的 `BBFreq`（入口相对），Stage 0 组合得 `global_freq`（§3.2）。下列独立格式仅在无法扩展 BB_ADDR_MAP 时使用。

```
Header (8 bytes):
    uint32  version;            // 0x00010000
    uint32  num_funcs;
For each function:
    uint32  num_bbs;
    uint64  bbs[num_bbs];       // global frequencies（已全局化）
    // 函数关联：本 section 附着于函数 text section（与 BB_ADDR_MAP 同机制），
    // 不存放 .symtab 索引。
```

链接后不保留（`SHF_EXCLUDE`）。

### 9.2 `.llvm_cfg_edge`（回退方案；首选复用 `BB_ADDR_MAP` 的 `BrProb` + `CGProfile`）

> **首选**：函数内 CFG 边权重取 `SHT_LLVM_BB_ADDR_MAP` 的 `BrProb`（后继分支概率）；跨函数调用边（含间接调用边）取 `SHT_LLVM_CALL_GRAPH_PROFILE`（边以符号名编码）。下列独立格式仅在无法复用时使用。

```
Header (8 bytes):
    uint32  version;
    uint32  num_edges;
For each edge:
    uint32  src_bb_idx;
    uint32  dst_bb_idx;
    uint64  weight;
    uint8   flags;              // bit0: is_cross_func, bit1: is_indirect_call
    // 跨函数边的 src/dst 函数以符号名（null-terminated string）标识，
    // 与 CGProfile 一致；不使用 .symtab 索引。
```

链接后不保留。

### 9.3 `.llvm_xbbr_attr`（类型 `SHT_LLVM_XBBR_ATTR = 0x6fff4c0e`，新分配）

承载 XBBR 专属黑名单/属性位。**采用 per-function-section 模式**（与 `.llvm_bb_addr_map` 一致），每函数一个独立 section：`SHF_LINK_ORDER` 依附该函数的 text section（`sh_link → .text.<fn>`），加 `SHF_EXCLUDE` 在链接后丢弃。函数关联完全由 section dependency 表达，**不存 `.symtab` 索引、不存符号名**。

每个 `.llvm_xbbr_attr` section 的字节格式：

```
Per-function section (one section per text section):
    uint8   version;            // 0x02 (v0x01 used u8 attrs and lacked
                                //       IsNoReturnTail; v0x02 widens to u16)
    uleb128 num_bbs;            // MachineFunction 中 MBB 个数
    uint16  attrs[num_bbs];     // little-endian 16-bit bitmask（按
                                // MachineFunction 迭代序）：
                                // bit0=is_entry,
                                // bit1=is_landing_pad,
                                // bit2=is_indirectbr_target,
                                // bit3=has_setjmp,                (含 longjmp 名匹配)
                                // bit4=has_inline_asm_label,
                                // bit5=is_musttail,
                                // bit6=user_blacklisted,
                                // bit7=is_cold (M3 落地),
                                // bit8=is_no_return_tail (SPEC §5.3 #7)
```

> **设计选择 1 vs 2（已落地为 1，本节是回写）**：早先草案给出"全模块单 section + `num_funcs` 头"格式（设计选择 2）。M1 实现采用**设计选择 1：per-function-section + SHF_LINK_ORDER**，理由：
> - 与既有 `SHT_LLVM_BB_ADDR_MAP` 一致（同样 per-function、同样 SHF_LINK_ORDER），lld 端读出/索引代码可以完全复用 BB_ADDR_MAP 路径；
> - 函数被 ICF/dead-strip 丢弃时，依赖它的 `.llvm_xbbr_attr` 自动随之消失（SHF_LINK_ORDER 语义），无需额外 GC 逻辑；
> - 不需要 `func_symbol_index` 字段，从根上避免了"`.symtab` 索引跨文件不稳定"问题（评审 #6）。
>
> 代价：单 .o 内若有 N 个函数则有 N 个 `.llvm_xbbr_attr` section，比单一 section 多 N×40B 头开销。`SHF_EXCLUDE` 在链接后全部丢弃,运行时零开销。

> **与 `BBEntry::Metadata` 的关系**：`is_landing_pad`/`is_indirectbr_target` 与 BB_ADDR_MAP `BBEntry::Metadata` 的 `IsEHPad`/`HasIndirectBranch` 重叠；`is_musttail` 比 `BBEntry::Metadata::HasTailCall` 更精确（musttail ⊂ tailcall，PLAN §3.4 评审 #7）。实现优先读 `BBEntry::Metadata`；XBBR 新位（`has_setjmp` / `has_inline_asm_label` / `is_musttail` 精确版 / `user_blacklisted` / `cold`）由本 section 提供。Stage 0 须对重叠位做一致性断言。

链接后不保留（`SHF_EXCLUDE`）。

### 9.4 `.debug_xbbr_decision`（`SHT_PROGBITS` + 非 `SHF_ALLOC`，链接后保留）

自定义二进制格式（**非** `SHT_NOTE`：note 段为 name/desc/type 三元组记录，与此处的定长表不匹配）。

> **section 命名约定（M2-T05 实施时确定）**：用 `.debug_` 前缀使标准 `strip --strip-debug` 自动识别并剥离,与 SPEC §9.4 "决策 map 可被 strip 剥离" 的语义对齐。早先草案给的 `.debug_xbbr_decision` 不带 `.debug_` 前缀,llvm-strip --strip-debug 不识别该名,需要 `--remove-section=` 显式指定才能剥离——属于"运行机制不匹配 SPEC 表述"的事实矛盾。落地为 `.debug_xbbr_decision` 后矛盾消解。

```
Header (16 bytes):
    char    magic[4];           // "XBBR"
    uint32  version;            // 0x00010000
    uint32  num_entries;
    uint32  flags;              // bit 0: degraded (Stage 4 回退到 function 模式)
                                // bits 1-31 reserved
For each entry (32 bytes):
    uint64  orig_func_addr;     // 原函数入口块的链接时绝对地址 (已落地: 真链后 VA)
    uint32  bb_index;           // 函数内 BB 索引
    uint64  new_address;        // 重排后地址 (已落地: 真链后 VA)
    uint32  cluster_id;         // 所属热簇编号
    uint32  decision_flags;     // moved | anchored | fallback | thunk
    uint32  func_id;            // 内部 FuncId，标识所属函数
```

> **VA 回填回写（2026-06-21）**：早先 `orig_func_addr`/`new_address` 标注为 "M3 placeholder 0 / 投影偏移，M5 填真 VA"。`SectionEmitter::backfillDecisionMapVAs`（在 `Writer::finalizeAddressDependentContent` + `optimizeBasicBlockJumps` 之后调用）已把二者改为真实链接后 VA（`orig_func_addr` = 函数入口 section VA，`new_address` = per-BB section VA），`cluster_id`/`func_id` 同步填真值。entry 数量在 `runSectionEmitter`（`buildSectionOrder` 内）已定，回填只 patch VA/flag 字段，**无需二次 finalize**（`getSize` 仅依赖 entry 数）。`decision_flags` 中 `Thunk` 位的填充（检测 BB 重定位目标是否为 thunk 符号）尚未接通，属未来工作。

默认进入二进制但不可加载（`SHF_ALLOC` 不置位，故不进 loadable 段、不计入 §2.2 体积预算），由 `strip --strip-debug` 自动剥离（依赖 `.debug_` 前缀的标准识别行为）。无需新 `SHT_LLVM_*` 类型常量——使用保留段名 `.debug_xbbr_decision` + `SHT_PROGBITS` 即可（与 `.debug_*` 既有"按名识别"段一致）。

### 9.5 新增 section 类型号分配

复用既有 `SHT_LLVM_BB_ADDR_MAP`（频率/边权重）与 `SHT_LLVM_CALL_GRAPH_PROFILE`（跨函数调用边）后，**唯一需新分配的 `SHT_LLVM_*` 类型常量是 `SHT_LLVM_XBBR_ATTR`**（§9.3）。当前 `llvm/include/llvm/BinaryFormat/ELF.h` 的 `SHT_LLVM_*` 区间止于 `SHT_LLVM_JT_SIZES = 0x6fff4c0d`，新常量取 `0x6fff4c0e`，遵循 LLVM 既有 `SHT_LLVM_*` 命名区间。`.debug_xbbr_decision` 用保留段名 + `SHT_PROGBITS`，不占新类型号。M1 阶段优先完成此项。

---

## 10. 里程碑实现分解

对应 SPEC §10 的 M1–M5，每里程碑拆为可独立 review 的子任务。

> **里程碑状态回写（2026-06-21，AArch64）**：物理 BB 级 section emission 机制已落地（Phase 0–3）——`-fbb-cross-reorder=partial|full` 隐含 `=all`、Stage 0 per-BB InputSection 关联、Stage 4 `CondInvolved` 距离守卫、Stage 5 经 `sectionOrder` 物理重排 + 决策 map 真 VA 回填、EH gate 保证栈展开正确。端到端在 AArch64 证明 BB 真实跨函数移动（`lld/test/ELF/xbbr/xbbr-aarch64-physical-migration.s`）。**P0-1（2026-06-21）已落地**：Stage 4 B/BL 投影-VA 范围求解 + `--bb-cross-reorder-max-thunk-bytes` 预算 + 30% 整体降级 + `fallback=none` fatal（§4.3 Stage 4）。**P1-2（2026-06-21）已落地**：`AArch64::deleteFallThruJmpInsn` 删除相邻 BB-section 间可 fall-through 的尾 `B`（`R_AARCH64_JUMP26`，精确 VA 匹配，语义安全）；`relocateAlloc` 跳过 `R_NONE`。`--optimize-bb-jumps` 在 AArch64 不再是 no-op。测试 `xbbr-aarch64-optimize-bb-jumps.s`（.text 体积下降、可重现）。**B.cond+B 翻转 case 暂缓**（需 `jumpInstrMod` 条件码反转 + `applyJumpInstrMod`，未来工作）。**仍未完成（M5 后续）**：`$xbbr.<fn>.bb<N>` 符号别名、对齐上限放置、CondInvolved 投影-VA 精细化（P1-3）、B.cond+B 翻转优化、DWARF/CFI/EH 完整重写（解除 EH gate）、ARM(ELF32LE) 支持、`full` mode 真行为差异量化、§9.2 量化门槛、`experimental-` 前缀。**P2-2（2026-06-21）已落地**：PIE（`-pie`）与动态库（`-shared`）经既有基础设施（PC-rel 由 lld 正常解析、入口块由 `isEntry` 自动锚定、thunk 循环处理超距）即正确工作——导出符号锚定入口、内部函数 BB 可漂移、PLT/GOT 不受影响。测试：`xbbr-pie.s`、`xbbr-shared-export-anchor.s`、`xbbr-shared-internal-drift.s`。M1–M4 的元数据/算法/工具部分此前已落地，本回写不重复。

### M1 — 编译器元数据 + clang 选项

| 子任务 | 位置 | 退出条件 |
|---|---|---|
| 注册 `SHT_LLVM_XBBR_ATTR` 常量 (`0x6fff4c0e`) | `llvm/include/llvm/BinaryFormat/ELF.h` | 编译通过（§9.5） |
| `XBBRMetadataEmitter` pass 骨架 | `llvm/lib/CodeGen/XBBRMetadataEmitter.cpp` | pass 注册成功 |
| 启用 BB_ADDR_MAP PGO feature | 同上（设 `-pgo-analysis-map`） | `FuncEntryCount`/`BBFreq`/`BrProb` 数值正确 |
| 间接调用边纳入 CGProfile | 同上（IRPGO VP） | CGProfile 含 vtable/函数指针调用边 |
| 黑名单 BB 判定 | 同上 | `.llvm_xbbr_attr` 位正确（musttail 用 IR 级判定，§3.4） |
| clang 选项 `-fbb-cross-reorder=` | `Options.td` / `Clang.cpp` | 选项解析、互斥检测 |
| lit 测试 | `llvm/test/CodeGen/X86/xbbr/` | section 生成验证 |

### M2 — lld Stage 0+1 + emit 框架

| 子任务 | 位置 | 退出条件 |
|---|---|---|
| `XBBRGraph` 构建（Stage 0） | `lld/ELF/XBBR/XBBRGraph.cpp` | 全图正确 |
| hfsort+ 接入（Stage 1） | `FunctionClustering.cpp` | 复用 `CallGraphSort` |
| section emission 骨架（Stage 5 部分） | `SectionEmitter.cpp` | x86_64 静态可执行体生成 |
| lld 选项 | `lld/ELF/Options.td` | 选项解析 |
| 互斥检测（Propeller） | `Driver.cpp` | 同时启用报错 |
| lit 测试 | `lld/test/ELF/xbbr/` | 功能等价 CGProfile-only |

### M3 — Stage 2+3+4 + 回退

| 子任务 | 位置 | 退出条件 |
|---|---|---|
| ExtTSP 实现（Stage 2） | `BBLayout.cpp` | 簇内 BB 重排正确 |
| PH chain merge（Stage 2） | `BBLayout.cpp` | 悬挂边处理 |
| 多目标代价函数（Stage 3） | `CostFunction.cpp` | 权重 CLI 生效 |
| 单 BB 回退（Stage 4） | `ConstraintSolver.cpp` | x86_64 分支约束回退 |
| 验收测试 | test-suite | 达到 SPEC §9.2 partial 门槛 |

### M4 — 调试设施 + 决策 map + 工具

| 子任务 | 位置 | 退出条件 |
|---|---|---|
| DWARF/CFI 重写 | `DWARFRewriter.cpp` | gdb bt 正确 |
| EH 重写 | `EHRewriter.cpp` | 异常展开正确 |
| ARM `.ARM.exidx` 多段 | `EHRewriter.cpp` | ARM 栈展开正确 |
| `.debug_xbbr_decision` 输出 | `SectionEmitter.cpp` | 反向查询正确 |
| `llvm-bbreorder-dump` | `llvm/tools/llvm-bbreorder-dump/` | 可视化输出 |
| BOLT 消费验证 | 外部 | BOLT 可读决策 map |

### M5 — AArch64 + ARM + PIE/动态库 + full mode

| 子任务 | 位置 | 退出条件 |
|---|---|---|
| AArch64 thunk 集成 | `AArch64Thunks.cpp` 协作 | ±128MB 外跳转正确 |
| ARM/Thumb thunk 集成 | `ARMSectCreate.cpp` 协作 | ±32MB/16MB 正确 |
| PIE 支持 | `SectionEmitter.cpp` | PC-relative 正确 |
| 动态库 + 导出符号锚定 | `Driver.cpp` | 内部链接函数可漂移 |
| `full` mode | `ConstraintSolver.cpp` | 全跨函数正确 |
| 嵌入式 demo | Zephyr / micropython | 通过 |
| 服务端 demo | clang/MySQL | 通过 |

> **落地现状（回写 2026-06-21）**：AArch64 端到端、PIE/动态库（P2-2）、`full` mode 均已落地并通过 lit（34/34）。ARM（P2-1）Stage 0 的 ELF32LE/REL 通路已接通：修复了 `llvm/lib/Object/ELF.cpp::decodeBBAddrMapImpl`（原仅处理 SHT_RELA/CREL，ARM 的 SHT_REL 被"expected 12, got 8"拒绝）与 `XBBRGraph::collectFromObjFiles`（原用未设防 `dyn_cast<ObjFile<ELF64LE>>`，因 `ELFFileBase::classof` 对任意 ELF 返回 true 而误匹配 ELF32 ARM 文件、按 64 位误解析头）两处缺陷；`markRangeAnchors` 扩到 EM_ARM（pin 不可 thunk 的 `R_ARM_THM_JUMP11`），`isThunkableBranchReloc` 移除 JUMP11。ARM 测试：`xbbr-arm-physical-migration.s`、`xbbr-thumb-thunk.s`、`xbbr-arm-thumb-interop.s`。
>
> **ARM EH gate 限制**：每个 ARM 函数强制带 `.ARM.exidx.text.<fn>`，故 Stage 0 EH gate 对**所有** ARM 函数生效 → 非 entry BB 整体随函数移动、不个体漂移（正确，PLAN §5.3/§5.4）。即 ARM 当前为**函数级**重排，跨函数 BB 个体漂移收益需 `.ARM.exidx` 多段化（P2-3）解除 gate 后才有。`.text.hot` split 仍出现（rename 基于 `isAnchor()/isCold()`，不查 IsEHGated）。


---

## 11. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 跨函数 BB 导致 backtrace 错误 | 调试体验破坏 | 强制 DWARF/CFI 重写（§5.1）+ M4 验证 gdb/perf |
| **FDE 拆分后 `.eh_frame_hdr` 未重建** | 异常分发/栈展开失败（correctness） | EHRewriter 在 `EhFrameHeader` 生成前拆 FDE，喂给既有建表流程（§5.3） |
| ARM Thumb thunk 频繁插入致体积膨胀 | 违反 SPEC 体积约束 | `--bb-cross-reorder-max-thunk-bytes` + 单 BB 回退（§4.4） |
| **对齐填充 NOP 膨胀** | 超 §2.2/§9.2 体积预算 | `--bb-cross-reorder-max-align` 对齐上限 + 计入 `SizeOverhead` + 末梢回退（§4.3 Stage 3/5） |
| **链接器松弛改变 BB size** | 布局偏移漂移、分支超距 | post-relaxation size 建图 + 迁移段冻结松弛（§4.5） |
| **profile 与源码失配** | 频率错误、布局退化 | SPEC §3.1 分级降级 + warning（`-Werror` 友好） |
| **ExtTSP 跨函数规模退化** | 大簇质量下降/内存压力 | 簇大小上限 + 子簇切分 + 降级 baseline（§4.3 Stage 2） |
| 非确定性排序 | 违反 SPEC §9.3 | 稳定 tie-breaker + `--deterministic` 模式（§6） |
| `.symtab` 索引跨文件失效 | 链接器引用错函数 | 弃用 symtab 索引，改 section 依附 + 符号名 + 内部 `FuncId`（§8.1、§9） |
| 与 Propeller/IBT/BTI 冲突 | 编译/运行错误 | CLI 互斥检测 + SPEC §12 未决问题追踪 |
| 链接时间超 SPEC 门槛 | 开发体验差 | 增量重排（未来扩展）+ Stage 并行化（确定性模式禁用） |
| 大型二进制（GB 级）graph 内存 | OOM | Stage 0 流式构建，按函数簇分区处理 |

---

**PLAN 文档结束** —— 需求契约见 [`SPEC.md`](SPEC.md)。
