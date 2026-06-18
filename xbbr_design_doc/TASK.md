# TASK: XBBR 实现任务分解与测试用例 (XBBR Task Breakdown)

> **执行分解文档** —— 把 SPEC（契约）与 PLAN（设计）落到可独立认领、可独立 review、可独立验证的任务单元。
> - 需求契约见 [`SPEC.md`](SPEC.md)；实现设计见 [`PLAN.md`](PLAN.md)。
> - 本文件回答"谁做什么、何时做、怎么验"。**与 SPEC 冲突时 SPEC 赢；与 PLAN 冲突时 PLAN 赢。** 若实现偏离，回写 PLAN；触碰契约则同步 SPEC。
> - **状态**：v0.1 / 2026-06-17 / 对应 SPEC v0.1 + PLAN v0.1

---

## 0. 编号与约定

### 0.1 任务编号

- `M{里程碑}-T{两位序号}`：里程碑内任务，如 `M1-T03`。
- `M{里程碑}-T{nn}-C{m}`：任务下的第 m 个测试用例，如 `M1-T03-C2`。
- `X-T{nn}`：横切任务（跨里程碑），如 `X-T01`（确定性 CI）。

### 0.2 任务字段

每个任务含：**描述 / 涉及文件 / 依赖(blocked by) / 退出条件 / 测试用例**。
退出条件须是可机器或可人工判定的客观标准，禁止"基本完成"之类措辞。

### 0.3 测试目录与命名约定（遵循既有 LLVM 风格）

| 层级 | 目录 | 命名 |
|---|---|---|
| CodeGen lit | `llvm/test/CodeGen/{X86,AArch64,ARM}/xbbr/` | `xbbr-<topic>.ll`（IR 驱动）/ `xbbr-<topic>.s` |
| LLD lit | `lld/test/ELF/xbbr/` | `xbbr-<topic>.s` |
| 工具 lit（readobj/dump） | `llvm/test/tools/llvm-readobj/ELF/`、`llvm/test/tools/llvm-bbreorder-dump/` | `xbbr-<topic>.test` |
| test-suite | `llvm-test-suite/`（独立仓库） | SPEC CPU 2017 + MicroBenchmarks |
| CI gate 脚本 | `.github/workflows/xbbr-*.yml` | — |

命名一律连字符小写（与既有 `cgprofile-orderfile.s`、`bb-addr-map-pgo-analysis-map.test` 一致）。多文件用 `split-file %s %t` 组织，首行加 `# REQUIRES: <arch>`。

### 0.4 全局测试策略（SPEC §9.1 测试金字塔）

```
        ┌──────────────┐
        │  CI gate     │  可重现性(SHA256) + self-host + 实战项目(clang/MySQL/Redis/Nginx/嵌入式)
        └──────┬───────┘
       ┌───────┴───────┐
       │  test-suite   │  SPEC CPU2017 + MicroBenchmarks（量化验收门槛 §9.2）
       └───────┬───────┘
      ┌────────┴────────┐
      │  lit 集成测试    │  端到端：源码→profile→链接→断言布局/符号/体积
      └────────┬────────┘
     ┌─────────┴─────────┐
     │  lit 单元测试      │  选项解析 / section 生成 / 单阶段算法 / 回退路径
     └───────────────────┘
```

每个新任务必须**自下而上**补测试：先单元 lit，再集成 lit；里程碑末加 test-suite/CI gate。

### 0.5 确定性测试硬要求（SPEC §9.3 / PLAN §6）

- 任何"排序/遍历"类任务，其 lit 测试**必须**包含一个"双链接比对 SHA256"用例：同输入链接两次，`cmp` 二进制必须一致。该用例是该任务的退出条件之一。
- 测试中禁止依赖 `DenseMap`/`SmallPtrSet` 遍历序的断言；断言前须显式排序或用数值集合匹配。

---

## 1. 横切任务（跨里程碑）

### X-T01 — 确定性构建 CI gate

- **描述**：搭建 CI 作业，对每个 `(arch × output-type × mode)` 三元组构建两次并比对 SHA256（SPEC §9.3、PLAN §6.3）。
- **涉及文件**：`.github/workflows/xbbr-repro.yml`、`xbbr_design_doc/scripts/xbbr-repro-check.sh`
- **依赖**：无（可先建空壳，随里程碑填充矩阵）
- **退出条件**：CI 在 M2 起对 `x86_64×static×partial` 跑通双构建比对；M5 起覆盖全部三元组。
- **测试用例**：
  - `X-T01-C1`：脚本 `xbbr-repro-check.sh x86_64 static partial` 退出码 0 且打印 `SHA256 MATCH`。
  - `X-T01-C2`（回归守卫）：人为注入一个依赖 `DenseMap` 遍历序的 patch，CI 须报 `SHA256 MISMATCH` 失败（用于证明 gate 有效）。

### X-T02 — 文档与契约一致性 lint

- **描述**：维护一个脚本，检查 SPEC↔PLAN↔TASK 三者交叉引用一致（CLI 选项名、section 名、常量值、里程碑退出条件）。
- **涉及文件**：`xbbr_design_doc/scripts/doc-consistency-check.sh`
- **依赖**：无
- **退出条件**：脚本在每次 PR 检查；发现"PLAN 用了 SPEC 未定义的选项名"等即报错。
- **测试用例**：`X-T02-C1`：故意在 PLAN 写 `--bb-cross-reorder-algo=`（已被 SPEC 拆为 cluster/layout 两个），脚本报错。

### X-T03 — 互斥检测矩阵（CLI 互斥，SPEC §6.3 / §7）

- **描述**：实现 clang/lld 侧 CLI 互斥检测：Propeller (`--symbol-ordering-file`) 与 XBBR、`-fbb-cross-reorder=none` 与子选项、`fallback=none` 与降级行为等。属横切（每个里程碑新增选项都要补）。
- **涉及文件**：`clang/lib/Driver/ToolChains/Clang.cpp`、`lld/ELF/Driver.cpp`
- **依赖**：随 M1/M2 选项落地
- **退出条件**：见各里程碑对应任务；本任务维护一张互斥表（SPEC §6.3）。
- **测试用例**：见 `M1-T06`、`M2-T04`。

---

## 2. M1 — 编译器元数据 + clang 选项（SPEC §10 M1）

**里程碑目标**：x86_64 上 `.o` 文件 XBBR 元数据正确生成；lld 暂不消费。
**退出条件**（全部 AND）：M1-T01…M1-T07 全部通过；`clang -fbb-cross-reorder=partial` 在 x86_64 上产出含正确 PGO feature 的 BB_ADDR_MAP + `.llvm_xbbr_attr`，链接器未改动仍能正常链接（功能等价于关闭）。

### M1-T01 — 注册 `SHT_LLVM_XBBR_ATTR` 类型常量

- **描述**：在 `ELF.h` 注册新常量 `0x6fff4c0e`（紧接既有 `SHT_LLVM_JT_SIZES = 0x6fff4c0d`，PLAN §9.5）。同步注册到 yaml2obj/obj2yaml/readobj 的 section 类型识别。
- **涉及文件**：`llvm/include/llvm/BinaryFormat/ELF.h`、`llvm/lib/ObjectYAML/ELFYAML.cpp`、`llvm/lib/Object/ELF.cpp`、`llvm/tools/llvm-readobj/ELFDumper.cpp`
- **依赖**：无
- **退出条件**：`yaml2obj` 能生成含 `.llvm_xbbr_attr` 的 ELF；`llvm-readobj -S` 能识别类型名。
- **测试用例**：
  - `M1-T01-C1`：`llvm/test/tools/yaml2obj/ELF/xbbr-attr.yaml` — yaml2obj 生成含 `SHT_LLVM_XBBR_ATTR` 的对象，`llvm-readobj -S` 打印 `SHT_LLVM_XBBR_ATTR`。
  - `M1-T01-C2`：`llvm/test/tools/obj2yaml/ELF/xbbr-attr.yaml` — 往返（obj2yaml→yaml2obj）类型不变。
  - `M1-T01-C3`：常量值断言：`llvm/test/Object/ELF/xbbr-section-type.test` — `readobj` 对 `0x6fff4c0e` 输出 `SHT_LLVM_XBBR_ATTR` 而非 `Unknown`。

### M1-T02 — `XBBRMetadataEmitter` pass 骨架

- **描述**：新增 CodeGen pass，挂在 `MachineBlockPlacement` 之后、`AsmPrinter` 之前；`-fbb-cross-reorder={partial,full}` 触发，`function` 模式仅依赖 CGProfile（不发 BB 元数据）。骨架先只做 pass 注册 + 空跑（PLAN §3.3）。
- **涉及文件**：`llvm/include/llvm/CodeGen/XBBRMetadata.h`、`llvm/lib/CodeGen/XBBRMetadataEmitter.cpp`、`llvm/lib/CodeGen/TargetPassConfig.cpp`（注册）、`llvm/lib/CodeGen/CMakeLists.txt`
- **依赖**：M1-T01
- **退出条件**：`llc -fbb-cross-reorder=partial -run-pass=xbbr-metadata-emitter` 不崩溃；pass 在 `-debug-pass=Structure` 中可见；`none` 模式下 pass 不运行。
- **测试用例**：
  - `M1-T02-C1`：`llvm/test/CodeGen/X86/xbbr/xbbr-pass-enabled.ll` — `-fbb-cross-reorder=partial` 下 `opt -debug-pass` 含 `XBBRMetadataEmitter`；`none` 下不含。
  - `M1-T02-C2`：`llvm/test/CodeGen/X86/xbbr/xbbr-pass-placement.ll` — pass 顺序断言：在 `MachineBlockPlacement` 之后、`AsmPrinter` 之前（用 `-print-after-all` 捕获 pass 序列 FileCheck）。
  - `M1-T02-C3`：`llvm/test/CodeGen/X86/xbbr/xbbr-pass-no-crash.ll` — 含 EH、间接跳转、递归的复合函数，pass 空跑不崩溃（baseline 保护）。

### M1-T03 — 启用 BB_ADDR_MAP PGO feature（全局频率归一化的数据源）

- **描述**：XBBR 启用 BB_ADDR_MAP 的 `FuncEntryCount`/`BBFreq`/`BrProb` feature（`-pgo-analysis-map`），作为 `global_freq = BBFreq × FuncEntryCount` 的数据源（PLAN §3.2）。复用既有设施，**不**新增 `.llvm_bb_freq`（PLAN §9.1）。验证频率数值正确，含递归/缺 profile 的处理。
- **涉及文件**：`llvm/lib/CodeGen/XBBRMetadataEmitter.cpp`（设置 feature）、`llvm/lib/CodeGen/AsmPrinter/AsmPrinter.cpp`（确认 feature 透传）
- **依赖**：M1-T02
- **退出条件**：`-fbb-cross-reorder=partial` + IRPGO profile 产出的 `.o`，`llvm-readobj --bb-addr-map --pretty-pgo-analysis-map` 同时显示 `FuncEntryCount`、每 BB 的 `BBFreq`、`BrProb`；乘积 `global_freq` 与手工核算一致。
- **测试用例**：
  - `M1-T03-C1`：`llvm/test/CodeGen/X86/xbbr/xbbr-bb-addr-map-pgo.ll` — 构造一个入口计数=100、BB2 条件频率=0.5 的函数，断言 `FuncEntryCount: 100`、`BBFreq` 比例正确、`global_freq(BB2)=50`（在 dump 工具或 readobj 中可见）。
  - `M1-T03-C2`：`llvm/test/CodeGen/X86/xbbr/xbbr-recursive-func.ll` — 递归函数 `fib`：断言 `entry_count` 为实测含递归次数，且**不**几何放大（BB 频率合理）；为 Stage 1 聚类用 `entry_count` 而非 `Σ global_freq` 留 TODO 注释。
  - `M1-T03-C3`：`llvm/test/CodeGen/X86/xbbr/xbbr-no-profile.ll` — 未传 profile：`getEntryCount()` 为空时该函数发 `FuncEntryCount=0`、标记为冷，不崩。
  - `M1-T03-C4`：`llvm/test/CodeGen/X86/xbbr/xbbr-pgo-mismatch.ll` — profile 与 CFG 边不符：复用 `-Wprofile-instr-mismatch` 诊断，该函数降级（不发漂移标记）。对应 SPEC §3.1。
  - `M1-T03-C5`（确定性）：`llvm/test/CodeGen/X86/xbbr/xbbr-bb-addr-map-deterministic.ll` — 同输入编译两次，BB_ADDR_MAP 字节级一致。

### M1-T04 — 间接调用边纳入 CGProfile

- **描述**：确保 IRPGO 间接调用值画像（`IPVK_IndirectCallTarget` VP）派生的间接调用热边进入 `.llvm.call_graph_profile`，补 C++ vtable/函数指针调用热路径（PLAN §3.3，回应检视 #11）。
- **涉及文件**：`llvm/lib/CodeGen/CGProfile.cpp`（VP→CGProfile 边）、`llvm/lib/ProfileData/InstrProf.cpp`
- **依赖**：M1-T02
- **退出条件**：含虚调用的 C++ 代码经 IRPGO 后，CGProfile 含 `(caller, virtual_callee)` 边且 `Count` 与 VP 一致。
- **测试用例**：
  - `M1-T04-C1`：`llvm/test/CodeGen/X86/xbbr/xbbr-indirect-call-edge.ll` — 构造 vtable dispatch，断言 CGProfile 含间接调用目标边（用 `llvm-readobj --cg-profile`）。
  - `M1-T04-C2`：`llvm/test/CodeGen/X86/xbbr/xbbr-indirect-call-no-vp.ll` — 无 VP 数据时优雅缺边，不崩，发统计 warning。

### M1-T05 — 黑名单 BB 判定 + `.llvm_xbbr_attr`

- **描述**：在 `XBBRMetadataEmitter` 内做静态分析，按 SPEC §5.3 判定黑名单 BB，写 `.llvm_xbbr_attr` 位掩码（PLAN §3.4、§9.3）。`musttail` 用 IR 级 `getTerminatingMustTailCall()`/`isMustTailCall()`，**不**用 `isReturn()`（PLAN §3.4 修正）。复用 `BBEntry::Metadata` 已有位，仅补新位；Stage 0 暂不消费（M2）。
- **涉及文件**：`llvm/lib/CodeGen/XBBRMetadataEmitter.cpp`
- **依赖**：M1-T01、M1-T02
- **退出条件**：每类黑名单 BB 各有 lit 覆盖，`llvm-readobj --bb-addr-map`（或自定义 dump）显示正确位。
- **测试用例**（每类一条 + 负例）：
  - `M1-T05-C1`：`.../xbbr-attr-entry-block.ll` — 入口块 `is_entry=1` 且不可漂移。
  - `M1-T05-C2`：`.../xbbr-attr-landing-pad.ll` — EH landing pad `is_landing_pad=1`（复用 `BBEntry::Metadata::IsEHPad`，一致性断言）。
  - `M1-T05-C3`：`.../xbbr-attr-indirectbr-target.ll` — `indirectbr`/`callbr` 目标、`blockaddress` 取址块 `is_indirectbr_target=1`。
  - `M1-T05-C4`：`.../xbbr-attr-setjmp.ll` — 含 `setjmp`/`longjmp` BB `has_setjmp=1`。
  - `M1-T05-C5`：`.../xbbr-attr-inline-asm-label.ll` — inline asm + `section`/label 引用 `has_inline_asm_label=1`。
  - `M1-T05-C6`：`.../xbbr-attr-musttail.ll` — **musttail call 所在 BB `is_musttail=1`**；负例：普通 tail call（`TCRETURN`）**不**置位（证明未误用 `isReturn`，回应检视 #7）。
  - `M1-T05-C7`：`.../xbbr-attr-user-blacklist.ll` — `-fbb-cross-reorder-blacklist=file` 命中函数的 BB `user_blacklisted=1`。
  - `M1-T05-C8`：`.../xbbr-attr-cold.ll` — 零频/低于阈值 BB `cold=1`，阈值默认 1%（SPEC §4/§6.1）。
  - `M1-T05-C9`（负例）：`.../xbbr-attr-clean-bb.ll` — 普通可漂移 BB 所有黑名单位为 0。
  - `M1-T05-C10`：`.../xbbr-attr-consistency-with-bbmetadata.ll` — `is_landing_pad`/`is_indirectbr_target` 与 `BBEntry::Metadata` 一致（Stage 0 一致性断言的前置）。

### M1-T06 — clang 选项 `-fbb-cross-reorder=*` 与子选项

- **描述**：实现 SPEC §6.1 全部 clang 选项：主开关 `=none|function|partial|full`、`-fbb-cross-reorder-cold-threshold=<frac>`（默认 0.01）、`-fbb-cross-reorder-blacklist=<file>`、`-fbb-cross-reorder-stats`。互斥检测（X-T03）。
- **涉及文件**：`clang/include/clang/Driver/Options.td`、`clang/lib/Driver/ToolChains/Clang.cpp`、`clang/lib/Frontend/CompilerInvocation.cpp`
- **依赖**：M1-T02
- **退出条件**：所有选项可解析、传到 CodeGen、默认值正确；非法值报错；`partial` 默认阈值 0.01 生效（回应检视 #2）。
- **测试用例**：
  - `M1-T06-C1`：`clang/test/Driver/xbbr-options.c` — 各 mode 解析为对应 cc1 flag；`-###` 可见。
  - `M1-T06-C2`：`clang/test/Driver/xbbr-options-invalid.c` — `=bogus` 报 `error: invalid value`。
  - `M1-T06-C3`：`clang/test/Driver/xbbr-cold-threshold-default.c` — 不传时默认 `0.01`（断言 `-cc1` 参数）；传 `0.05` 覆盖。
  - `M1-T06-C4`：`clang/test/Driver/xbbr-mutex.c` — `partial`/`full` 与 `-fno-bb-cross-reorder` 互斥关系；与 Propeller 无冲突（Propeller 在 lld 侧）。
  - `M1-T06-C5`：`clang/test/Driver/xbbr-blacklist.c` — 黑名单文件路径透传。

### M1-T07 — M1 集成测试

- **描述**：端到端验证 M1 产出物：`clang -fprofile-instr-generate → run → -fprofile-instr-use -fbb-cross-reorder=partial` 产出的 `.o`，元数据完整且 lld（未改）仍可正常链接。
- **涉及文件**：`llvm/test/CodeGen/X86/xbbr/xbbr-m1-integration.ll`
- **依赖**：M1-T03、M1-T04、M1-T05、M1-T06
- **退出条件**：集成用例通过；`ld.lld`（当前主线版本）链接该 `.o` 不报错、行为等价于关闭 XBBR。
- **测试用例**：
  - `M1-T07-C1`：多函数 + 虚调用 + EH 的 C++ 小程序，跑 IRPGO 后编译，断言 `.o` 同时含 PGO-enabled BB_ADDR_MAP、CGProfile（含间接边）、`.llvm_xbbr_attr`。
  - `M1-T07-C2`：上述 `.o` 用 `ld.lld` 链接为可执行，运行结果与关闭 XBBR 一致（功能等价）。

---

## 3. M2 — lld Stage 0+1 + emit 框架（SPEC §10 M2）

**里程碑目标**：x86_64 静态可执行可生成；功能等价于 CGProfile-only。
**退出条件**：M2-T01…M2-T06 通过；`lld --bb-cross-reorder=partial` 产出 x86_64 静态可执行，函数级布局等价于 `--call-graph-profile-sort=hfsort`（既有行为），且不引入新运行时错误。

### M2-T01 — Stage 0：`XBBRGraph` 构建

- **描述**：遍历输入 `.o`，解析 BB_ADDR_MAP（PGO feature）+ CGProfile + `.llvm_xbbr_attr`，构建全局 `XBBRGraph`（PLAN §8.1）。**稳定引用**：函数用内部 `FuncId`（按 `(input_file_index, function_text_section)` 稳定分配，遍历前排序），跨函数调用边由 CGProfile 符号名解析为 `FuncId`（回应检视 #6）。`global_freq = BBFreq × FuncEntryCount` 在此组合。
- **涉及文件**：`lld/ELF/XBBR/XBBRGraph.cpp`、`XBBRGraph.h`、`XBBRTypes.h`
- **依赖**：M1-T03、M1-T04、M1-T05
- **退出条件**：多 `.o` 输入下图节点/边数与元数据一致；黑名单节点标为 anchor；入口块为 anchor。
- **测试用例**：
  - `M2-T01-C1`：`lld/test/ELF/xbbr/xbbr-graph-build.s` — 2 个 `.o` 各 2 函数，`--bb-cross-reorder-stats` 打印节点数/边数/anchor 数正确。
  - `M2-T01-C2`：`lld/test/ELF/xbbr/xbbr-graph-stable-funcid.s` — 同输入不同 `.o` 传入顺序，`FuncId` 分配一致（确定性，回应检视 #6）。
  - `M2-T01-C3`：`lld/test/ELF/xbbr/xbbr-graph-cross-func-edge.s` — CGProfile 含跨函数调用边，图中 `IsCrossFunc=1`。
  - `M2-T01-C4`：`lld/test/ELF/xbbr/xbbr-graph-anchor.s` — 入口块与黑名单 BB 标 `isAnchor()`。

### M2-T02 — Stage 1：hfsort+ 函数簇粗排

- **描述**：复用 `CallGraphSort.cpp` 的 hfsort+，输入 CGProfile + 函数总频率（用 `entry_count`，**不**用 `Σ global_freq`，避免递归放大，PLAN §3.2）。密度优先队列合并为簇。簇大小上限（PLAN §4.3 Stage 2 的前置）。
- **涉及文件**：`lld/ELF/XBBR/FunctionClustering.cpp`、复用 `lld/ELF/CallGraphSort.cpp`
- **依赖**：M2-T01
- **退出条件**：簇序列等价于既有 `--call-graph-profile-sort=hfsort`；递归函数不被过度优先。
- **测试用例**：
  - `M2-T02-C1`：`lld/test/ELF/xbbr/xbbr-hfsort-equiv.s` — 同输入下 `--bb-cross-reorder=function` 与 `--call-graph-profile-sort=hfsort` 产出的函数序**等价**（`llvm-nm --numeric-sort` 一致）。
  - `M2-T02-C2`：`lld/test/ELF/xbbr/xbbr-hfsort-recursive.s` — 递归函数密度按 `entry_count` 算，不被 `Σ global_freq` 放大。
  - `M2-T02-C3`：`lld/test/ELF/xbbr/xbbr-cluster-cap.s` — 超大簇被切分，`--bb-cross-reorder-stats` 报告切分数。

### M2-T03 — Stage 5（部分）：section emission 骨架

- **描述**：最小 emit：把 hfsort+ 簇序写为 `.text.hot`/`.text.unlikely`（M2 阶段仍按函数粒度，BB 未漂移），生成静态可执行。thunk/DWARF/EH 重写留 M3/M4。确保 M2 产出"功能等价 CGProfile-only"。
- **涉及文件**：`lld/ELF/XBBR/SectionEmitter.cpp`、`lld/ELF/OutputSections.cpp`
- **依赖**：M2-T02
- **退出条件**：x86_64 静态可执行可运行且行为正确。
- **测试用例**：
  - `M2-T03-C1`：`lld/test/ELF/xbbr/xbbr-emit-static.s` — 链接 x86_64 静态可执行，运行通过，`llvm-objdump -d` 热函数在 `.text.hot`。
  - `M2-T03-C2`：`lld/test/ELF/xbbr/xbbr-emit-equiv.s` — 与 CGProfile-only 链接产出的 `.text` 布局等价（函数序）。
  - `M2-T03-C3`（确定性）：`lld/test/ELF/xbbr/xbbr-emit-deterministic.s` — 双链接 SHA256 一致。

### M2-T04 — lld 选项 + 互斥检测

- **描述**：SPEC §6.2 全部 lld 选项：`--bb-cross-reorder=<profdata-or-none>`、`--bb-cross-reorder-mode=partial|full`、`--bb-cross-reorder-cluster-algo`、`--bb-cross-reorder-layout-algo`（拆分两个，回应检视表）、`--bb-cross-reorder-weights`、`--bb-cross-reorder-max-thunk-bytes`、`--bb-cross-reorder-fallback`、`--bb-cross-reorder-emit-decision-map`、`--bb-cross-reorder-deterministic`、`--bb-cross-reorder-max-align`。互斥：与 `--symbol-ordering-file`（Propeller）同时启用报错（SPEC §6.3）。
- **涉及文件**：`lld/ELF/Options.td`、`lld/ELF/Driver.cpp`、`lld/include/lld/Common/Args.h`
- **依赖**：M2-T01
- **退出条件**：所有选项解析生效；互斥检测正确。
- **测试用例**：
  - `M2-T04-C1`：`lld/test/ELF/xbbr/xbbr-options.s` — 各选项解析、默认值（max-thunk、max-align、weights）。
  - `M2-T04-C2`：`lld/test/ELF/xbbr/xbbr-mutex-propeller.s` — `--bb-cross-reorder` + `--symbol-ordering-file` 报错退出。
  - `M2-T04-C3`：`lld/test/ELF/xbbr/xbbr-options-split-algo.s` — `cluster-algo=hfsort+,c3` 与 `layout-algo=ext-tsp,ph` 分别生效（回应检视表 C³）。

### M2-T05 — 决策 map 输出框架（`.debug_xbbr_decision`）

- **描述**：实现 `.debug_xbbr_decision`（`SHT_PROGBITS` + 非 `SHF_ALLOC`，PLAN §9.4）输出。M2 阶段记录函数级决策（BB 未漂移，多数 `anchored`）；BB 级 entries 在 M3 填充。含 magic `XBBR` + version。
- **涉及文件**：`lld/ELF/XBBR/SectionEmitter.cpp`
- **依赖**：M2-T03
- **退出条件**：输出 section 含正确 header；`strip --strip-debug` 可剥离；不进 loadable 段。
- **测试用例**：
  - `M2-T05-C1`：`lld/test/ELF/xbbr/xbbr-decision-map.s` — `--bb-cross-reorder-emit-decision-map` 产出 `.debug_xbbr_decision`，`readobj -S` 显示非 `SHF_ALLOC`。
  - `M2-T05-C2`：`lld/test/ELF/xbbr/xbbr-decision-map-strip.s` — `strip --strip-debug` 后 section 消失；`--strip-all` 行为记录为 SPEC §12 开放问题。

### M2-T06 — M2 集成测试

- **描述**：端到端：编译器 M1 产出 → lld M2 链接 → 等价 CGProfile-only。
- **涉及文件**：`lld/test/ELF/xbbr/xbbr-m2-integration.s`
- **依赖**：M1-T07、M2-T05
- **退出条件**：x86_64 静态可执行从源码到运行全链路通过，行为等价 CGProfile-only，双链接可重现。

---

## 4. M3 — Stage 2/3/4 + 回退（SPEC §10 M3）

**里程碑目标**：x86_64 上达到 SPEC §9.2 `partial` 量化门槛。
**退出条件**：M3-T01…M3-T06 通过；test-suite 在 `partial` 模式下 L1i miss↓≥10%、iTLB↓≥15%、branch-miss↓≥8%、hot-text↑≤10%、总二进制↑≤1.5%、链接时间≤1.5× baseline。

### M3-T01 — Stage 2：ExtTSP 簇内 BB 精排

- **描述**：复用 `llvm/lib/Transforms/Utils/CodeLayout.cpp` 的 `computeExtTspLayout`（贪心 `O(E log E)`，非 O(n³)，PLAN §4.3 修正）。簇内可迁移 BB 最大化加权 fall-through 收益；黑名单/入口为 anchor 固定；簇大小上限 + 子簇切分 + 降级 baseline（回应检视 #3）。
- **涉及文件**：`lld/ELF/XBBR/BBLayout.cpp`
- **依赖**：M2-T02
- **退出条件**：簇内 BB 序最大化 fall-through；跨函数同簇 BB 可穿插；超限簇降级不崩。
- **测试用例**：
  - `M3-T01-C1`：`lld/test/ELF/xbbr/xbbr-exttsp-within-cluster.s` — 单簇多 BB，断言热边相邻（fall-through），`llvm-objdump` 验证相邻地址。
  - `M3-T01-C2`：`lld/test/ELF/xbbr/xbbr-exttsp-cross-func.s` — 同簇不同函数的 BB 穿插排列，入口块仍为函数符号地址（ABI 不变量 SPEC §5.1）。
  - `M3-T01-C3`：`lld/test/ELF/xbbr/xbbr-exttsp-anchor.s` — 黑名单 BB 位置固定，其余围绕重排。
  - `M3-T01-C4`：`lld/test/ELF/xbbr/xbbr-exttsp-subcluster.s` — 超大簇切分为子簇，`--stats` 报告切分；降级 baseline 路径覆盖。
  - `M3-T01-C5`（确定性）：双链接 BB 序一致。

### M3-T02 — Stage 2：Pettis-Hansen chain merge

- **描述**：ExtTSP 后处理，处理悬挂边（多簇共享 BB），沿调用边合并短链；anchor 不可跨越拼接（PLAN §4.3）。
- **涉及文件**：`lld/ELF/XBBR/BBLayout.cpp`
- **依赖**：M3-T01
- **退出条件**：悬挂 BB 按最重归属簇归并，无重复放置；跨函数 chain 合并正确。
- **测试用例**：
  - `M3-T02-C1`：`lld/test/ELF/xbbr/xbbr-ph-dangling.s` — 被多簇引用的 BB 归并到最重簇，`--stats` 报告合并数。
  - `M3-T02-C2`：`lld/test/ELF/xbbr/xbbr-ph-anchor-boundary.s` — chain 不跨越函数入口块拼接。

### M3-T03 — Stage 3：多目标代价函数微调

- **描述**：实现 `Cost = w_icache·IcacheCrossings + w_itlb·TLBCrossings + w_btb·BTB + w_size·SizeOverhead`（PLAN §4.3）。**投影地址模型**破循环依赖：按 BB 全序 + 对齐上限累加投影偏移，在投影偏移上算 cache-line/page/BTB 代价；`SizeOverhead` 含 thunk 估计 + align_padding + fde_split + eh_frame_hdr（回应检视 #4、#10）。局部搜索至收敛。Stage 5 末梢复核。
- **涉及文件**：`lld/ELF/XBBR/CostFunction.cpp`
- **依赖**：M3-T02
- **退出条件**：权重 CLI 生效；投影代价随布局单调评估；对齐上限 `--bb-cross-reorder-max-align` 生效。
- **测试用例**：
  - `M3-T03-C1`：`lld/test/ELF/xbbr/xbbr-cost-weights.s` — 改 `--bb-cross-reorder-weights=size=4,...` 产出更小布局（`--stats` 报告 SizeOverhead 降低）。
  - `M3-T03-C2`：`lld/test/ELF/xbbr/xbbr-cost-projected-offset.s` — 构造跨 cache-line/page 边界场景，断言代价正确计入（`--stats` 报告 IcacheCrossings/TLBCrossings）。
  - `M3-T03-C3`：`lld/test/ELF/xbbr/xbbr-cost-max-align.s` — `--bb-cross-reorder-max-align=16` 下对齐填充受控，`--stats` 报告 align_padding。
  - `M3-T03-C4`：`lld/test/ELF/xbbr/xbbr-cost-monotone.s` — 局部搜索后总 Cost 不升（回归守卫）。

### M3-T04 — Stage 4：单 BB 约束求解与回退

- **描述**：实现 PLAN §4.3 Stage 4 伪码：超分支距离/EH 冲突/黑名单/松弛复检触发单 BB 回退。**pin 被回退 BB**（单调收敛，无震荡），安全网上限 `num_migratable+1`，`global_threshold=30%` 超阈整体降级（回应检视 #5）。
- **涉及文件**：`lld/ELF/XBBR/ConstraintSolver.cpp`
- **依赖**：M3-T03
- **退出条件**：回退单调收敛；超阈降级；`fallback=none` 报错；warning 友好。
- **测试用例**：
  - `M3-T04-C1`：`lld/test/ELF/xbbr/xbbr-fallback-branch-range.s` — x86_64 构造超距分支（用大偏移），该 BB 回退，`--stats` 报告 fallback 数；产物可运行。
  - `M3-T04-C2`：`lld/test/ELF/xbbr/xbbr-fallback-eh-conflict.s` — EH range 冲突触发回退。
  - `M3-T04-C3`：`lld/test/ELF/xbbr/xbbr-fallback-converges.s` — 构造级联回退场景，断言**不**无限循环（pin 单调），迭代数 ≤ `num_migratable+1`（回应检视 #5 震荡）。
  - `M3-T04-C4`：`lld/test/ELF/xbbr/xbbr-fallback-degrade.s` — 回退超 30% 触发整体降级到 `function` 模式，warning 输出。
  - `M3-T04-C5`：`lld/test/ELF/xbbr/xbbr-fallback-none.s` — `--bb-cross-reorder-fallback=none` 遇约束直接 error 退出。
  - `M3-T04-C6`：`lld/test/ELF/xbbr/xbbr-fallback-warning-werror.s` — 回退 warning 在 `-Werror` 下升级为 error（SPEC §7）。

### M3-T05 — Stage 5：决策 map BB 级条目 + Fragment 准备（x86_64）

> **M3/M5 拆分（回写 2026-06-18）**：物理 BB 级 .text.hot/.text.unlikely 输出推迟到 M5。M3 范围：决策 map BB 级条目持久化 + BBFragment 生成。

- **描述**：`SectionEmitter::run()` 根据管线输出构建 per-BB 决策条目，通过 `XBBRDecisionMapSection::setEntries()` 持久化为 32B/条 ELF section 数据，并生成 `BBFragment`/`BBPlacement` 供 M5 物理 emit 消费。
- **涉及文件**：`lld/ELF/XBBR/SectionEmitter.cpp`、`lld/ELF/SyntheticSections.{h,cpp}`
- **依赖**：M3-T04、M2-T05
- **退出条件**：决策 map `writeTo()` 输出完整 per-BB entry；`llvm-readelf -x .debug_xbbr_decision` 可见 magic "XBBR" + version + 非零 entries。
- **测试用例**：
  - `M3-T05-C2`：`lld/test/ELF/xbbr/xbbr-m3-integration.s` — 端到端 M3，`--bb-cross-reorder-emit-decision-map` 产出 `.debug_xbbr_decision`。
  - `M3-T05-C1`（thunk）、`M3-T05-C3`（runnable EH）：推迟到 M5。

### M3-T06 — 量化验收（test-suite + perf）

> **M3/M5 拆分（回写 2026-06-18）**：量化验收依赖物理 BB 级布局生效，M3 仍走函数级路径故推迟到 M5。M3 已产出完整 XBBRLayoutResult 数据。

- **状态**：**blocked by M5**（M5-T05 物理 section emission 完成后解锁）。
- **依赖**：M5-T05

---

## 5. M4 — 调试设施 + 决策 map + 工具（SPEC §10 M4）

**里程碑目标**：gdb/perf annotate 可读；BOLT 可消费决策 map。
**退出条件**：M4-T01…M4-T06 通过；漂移函数在 gdb `bt`、`perf annotate`、`addr2line` 下正确归因；BOLT 读决策 map 成功。

### M4-T01 — DWARF/CFI 重写

- **描述**：漂移函数 `DW_TAG_subprogram` 改用 `DW_AT_ranges` 列出所有 BB 段；`.debug_line` 每 BB 段插入 `DW_LNE_set_address`；`.debug_aranges`/`.debug_ranges` 多段化（PLAN §5.1）。
- **涉及文件**：`lld/ELF/XBBR/DWARFRewriter.cpp`
- **依赖**：M3-T05
- **退出条件**：gdb `bt`/`info func`/`addr2line` 对漂移 BB 正确归因。
- **测试用例**：
  - `M4-T01-C1`：`lld/test/ELF/xbbr/xbbr-dwarf-ranges.s` — 漂移函数 `readobj --dwarf=info` 显示 `DW_AT_ranges`，`--dwarf=ranges` 列出多段。
  - `M4-T01-C2`：`lld/test/ELF/xbbr/xbbr-dwarf-line.s` — `.debug_line` 含每段 `set_address`。
  - `M4-T01-C3`：`lld/test/ELF/xbbr/xbbr-gdb-bt.test`（REQUIRES: gdb）— 用 gdb 在漂移 BB 设断点、`bt` 正确。
  - `M4-T01-C4`：`lld/test/ELF/xbbr/xbbr-addr2line.test` — `addr2line` 对漂移 BB 地址返回正确函数+行。

### M4-T02 — EH 重写（`.eh_frame` + `.gcc_except_table` + `.eh_frame_hdr`）

- **描述**：漂移函数 FDE 拆 N 个（各自 `initial_location`+`address_range`）；LSDA call_site_table 用绝对地址重写；**`.eh_frame_hdr` 重建**（EHRewriter 须在 `EhFrameHeader` 生成前完成拆分，喂给既有建表流程，否则异常分发失败，PLAN §5.3，回应检视 #9）。
- **涉及文件**：`lld/ELF/XBBR/EHRewriter.cpp`、复用 `lld/ELF/SyntheticSections.cpp`（`EhFrameHeader`/`EhFrameSection`）
- **依赖**：M3-T05
- **退出条件**：抛异常时 personality routine 正确定位 LSDA/landing pad；`_Unwind_Backtrace` 正确展开。
- **测试用例**：
  - `M4-T02-C1`：`lld/test/ELF/xbbr/xbbr-eh-fde-split.s` — 漂移函数 1→N FDE，`readobj --unwind` 列出全部。
  - `M4-T02-C2`：`lld/test/ELF/xbbr/xbbr-eh-frame-hdr-rebuild.s` — `.eh_frame_hdr` 二分查找表含全部 N 个 FDE 条目（**correctness gate**，回应检视 #9）。
  - `M4-T02-C3`：`lld/test/ELF/xbbr/xbbr-eh-throw.test`（REQUIRES: libstdc++）— 抛捕获异常的程序正确运行。
  - `M4-T02-C4`：`lld/test/ELF/xbbr/xbbr-eh-backtrace.test` — `_Unwind_Backtrace` 在漂移 BB 地址展开栈帧正确。

### M4-T03 — ARM/AArch64 `.ARM.exidx` 多段

- **描述**：每段 BB 生成独立 `.ARM.exidx` 条目对齐段起点；不可压缩表段间填 `EXIDX_CANTUNWIND`；Thumb 段入口保持对齐与 `[1:0]` 位标志（PLAN §5.4）。
- **涉及文件**：`lld/ELF/XBBR/EHRewriter.cpp`、`lld/ELF/Arch/ARM.cpp`
- **依赖**：M4-T02（先在 x86 EH 通，再移植 ARM）
- **退出条件**：ARM/Thumb 漂移函数栈展开正确。
- **测试用例**：
  - `M4-T03-C1`：`lld/test/ELF/xbbr/xbbr-arm-exidx-multi.s` — `readobj --arm-exidx` 列出多段条目。
  - `M4-T03-C2`：`lld/test/ELF/xbbr/xbbr-thumb-exidx-align.s` — Thumb 段入口 `[1:0]` 位与对齐正确。
  - `M4-T03-C3`：`lld/test/ELF/xbbr/xbbr-arm-unwind.test`（REQUIRES: arm）— 漂移函数栈展开正确。

### M4-T04 — `llvm-bbreorder-dump` 工具

- **描述**：新增工具，解析 `.debug_xbbr_decision` + BB_ADDR_MAP，列出每函数 BB 去向、输出 Graphviz 热路径图、与 perf script 协作（PLAN §5.6）。
- **涉及文件**：`llvm/tools/llvm-bbreorder-dump/llvm-bbreorder-dump.cpp`、`llvm/test/tools/llvm-bbreorder-dump/`
- **依赖**：M2-T05、M3-T05
- **退出条件**：工具能 dump 决策 map 并与实际布局一致。
- **测试用例**：
  - `M4-T04-C1`：`llvm/test/tools/llvm-bbreorder-dump/xbbr-dump-map.test` — dump 输出含每 BB `(orig_func, bb) → (new_addr, cluster, flags)`。
  - `M4-T04-C2`：`.../xbbr-dump-graphviz.test` — Graphviz 输出可被 `dot` 解析（REQUIRES: graphviz，可选）。
  - `M4-T04-C3`：`.../xbbr-dump-consistency.test` — dump 的 new_addr 与 `llvm-nm`/`objdump` 实际地址一致。

### M4-T05 — BOLT 消费决策 map

- **描述**：验证 BOLT 可读 `.debug_xbbr_decision` 做边角微调（SPEC §2.3、§11.5）。
- **涉及文件**：外部 `bolt/`（若仓库内含则 `bolt/lib/`）
- **依赖**：M4-T04
- **退出条件**：BOLT 读 map 不报错且微调后可运行。
- **测试用例**：
  - `M4-T05-C1`：`bolt/test/xbbr-consume-map.test`（REQUIRES: bolt）— BOLT 处理含决策 map 的二进制不报错、产物可运行。

### M4-T06 — M4 集成测试

- **描述**：端到端验证调试可观测性：漂移函数在 gdb/perf/addr2line 全部正确归因。
- **涉及文件**：`lld/test/ELF/xbbr/xbbr-m4-integration.test`
- **依赖**：M4-T01…M4-T05
- **退出条件**：集成用例通过；含 EH 的漂移函数异常分发与回溯全部正确。

---

## 6. M5 — AArch64 + ARM + PIE/动态库 + full mode（SPEC §10 M5）

**里程碑目标**：三架构完整支持；嵌入式/服务端 demo 通过。
**退出条件**：M5-T01…M5-T09 通过；AArch64/ARM 含 thunk 的 ±128MB/±32MB/±16MB 跳转正确；PIE/动态库（内部链接函数漂移）正确；`full` mode 全跨函数正确；Zephyr/clang/MySQL demo 通过；SPEC §9.2 `full` 门槛达标。

### M5-T01 — AArch64 thunk 集成

- **描述**：漂移 BB 跨段超 ±128MB 时复用 `AArch64Thunks` 注入 stub（PLAN §4.4）。
- **涉及文件**：`lld/ELF/Arch/AArch64.cpp`、`lld/ELF/Thunks.cpp`、`lld/ELF/XBBR/SectionEmitter.cpp`
- **依赖**：M3-T05、M4-T03
- **退出条件**：±128MB 外跳转正确；thunk 字节受 `--bb-cross-reorder-max-thunk-bytes` 约束。
- **测试用例**：
  - `M5-T01-C1`：`lld/test/ELF/xbbr/xbbr-aarch64-thunk.s` — 构造超 128MB 跨段，注入 stub，运行正确。
  - `M5-T01-C2`：`lld/test/ELF/xbbr/xbbr-aarch64-thunk-budget.s` — 超 `max-thunk-bytes` 触发 BB 回退。

### M5-T02 — ARM/Thumb thunk 集成

- **描述**：ARM A32 ±32MB、Thumb-2 ±16MB；复用 `ARMSectCreate` 注入 veneer；ARM↔Thumb 互操作（PLAN §4.4）。
- **涉及文件**：`lld/ELF/Arch/ARM.cpp`、`lld/ELF/XBBR/SectionEmitter.cpp`
- **依赖**：M4-T03
- **退出条件**：±32MB/±16MB 跳转正确；ARM↔Thumb 互操作正确。
- **测试用例**：
  - `M5-T02-C1`：`lld/test/ELF/xbbr/xbbr-arm-thunk.s`、`.../xbbr-thumb-thunk.s` — 超距注入 veneer。
  - `M5-T02-C2`：`lld/test/ELF/xbbr/xbbr-arm-thumb-interop.s` — ARM↔Thumb 漂移 BB 跳转正确。

### M5-T03 — 链接器松弛交互（PLAN §4.5）

- **描述**：处理 `AArch64Relaxer`（ADRP+ADD→ADR 等）改变 BB size 的交互（回应检视 #8）。策略：post-relaxation size 建图 + 迁移段冻结松弛；`--bb-cross-reorder-deterministic` 强制全量冻结。Stage 4 `relax_recheck`。
- **涉及文件**：`lld/ELF/Arch/AArch64.cpp`（`AArch64Relaxer`）、`lld/ELF/XBBR/ConstraintSolver.cpp`、`XBBRGraph.cpp`
- **依赖**：M5-T01
- **退出条件**：松弛不导致布局溢出；确定性模式 bitwise 可重现。
- **测试用例**：
  - `M5-T03-C1`：`lld/test/ELF/xbbr/xbbr-aarch64-relax.s` — 含可松弛 ADRP+ADD 的漂移 BB，运行正确、size 不漂移致超距。
  - `M5-T03-C2`：`lld/test/ELF/xbbr/xbbr-relax-deterministic.s` — `--bb-cross-reorder-deterministic` 下双链接 SHA256 一致（SPEC §12 #6）。

### M5-T04 — PIE 支持

- **描述**：`-fPIE -pie` 下 PC-relative 正确；漂移 BB 的 GOT/PLT 引用不变（SPEC §8.2）。
- **涉及文件**：`lld/ELF/XBBR/SectionEmitter.cpp`、`lld/ELF/Relocations.cpp`
- **依赖**：M3-T05
- **退出条件**：PIE 可执行运行正确。
- **测试用例**：
  - `M5-T04-C1`：`lld/test/ELF/xbbr/xbbr-pie.s` — PIE 链接运行正确，`readelf -l` 无异常段。

### M5-T05 — 动态库 + 导出符号锚定

- **描述**：`-shared` 下仅内部链接函数的 BB 可漂移；导出符号严格锚定入口块（SPEC §8.2）。PLT/GOT 不受影响。
- **涉及文件**：`lld/ELF/XBBR/SectionEmitter.cpp`、`lld/ELF/SymbolTable.cpp`
- **依赖**：M5-T04
- **退出条件**：动态库导出符号地址=入口块；内部函数可漂移；dlopen/调用正确。
- **测试用例**：
  - `M5-T05-C1`：`lld/test/ELF/xbbr/xbbr-shared-export-anchor.s` — 导出符号地址=入口块未漂移。
  - `M5-T05-C2`：`lld/test/ELF/xbbr/xbbr-shared-internal-drift.s` — 内部函数 BB 可漂移且库可运行。

### M5-T06 — `full` mode

- **描述**：全跨函数 BB 重排，函数边界仅作符号锚点（SPEC §4）。Stage 4 约束更宽松（更多 BB 可迁移），需保证 ABI 不变量。
- **涉及文件**：`lld/ELF/XBBR/ConstraintSolver.cpp`、`BBLayout.cpp`
- **依赖**：M3-T05
- **退出条件**：`full` 模式产出正确可运行；ABI 不变量（SPEC §5）全部保持。
- **测试用例**：
  - `M5-T06-C1`：`lld/test/ELF/xbbr/xbbr-full-mode.s` — `--bb-cross-reorder-mode=full` 下入口块锚定、黑名单不漂移。
  - `M5-T06-C2`：`lld/test/ELF/xbbr/xbbr-full-vs-partial.s` — `full` 比 `partial` 迁移更多 BB（`--stats` 迁移数更高）。
  - `M5-T06-C3`：ABI 不变量专项（入口块地址=符号、indirectbr 目标不漂移、EH 正确）。

### M5-T07 — full LTO 数据流

- **描述**：`-flto=full` 下 `XBBRMetadataEmitter` 在 LTO CodeGen 按 partition 运行，元数据随 partition 输出流入 lld，Stage 0 跨 partition 聚合（SPEC §8.3，回应检视 #12）。
- **涉及文件**：`llvm/lib/LTO/`、`llvm/lib/CodeGen/XBBRMetadataEmitter.cpp`、`lld/ELF/LTO.cpp`
- **依赖**：M1-T03
- **退出条件**：full LTO 链接产出等价非 LTO 布局（同 profile）。
- **测试用例**：
  - `M5-T07-C1`：`lld/test/ELF/lto/xbbr-full-lto.s` — `-flto=full` 链接正确，布局可重现。
  - `M5-T07-C2`：`lld/test/ELF/lto/xbbr-thinlto-unsupported.s` — `-flto=thin` 报"XBBR 不支持 ThinLTO"（SPEC §8.3）。

### M5-T08 — 嵌入式 demo（Zephyr / micropython）

- **描述**：在嵌入式样例上验证 `partial` 模式 + 体积预算（SPEC §2.2、§9.2 嵌入式门槛）。
- **涉及文件**：外部仓库 + `xbbr_design_doc/scripts/xbbr-embedded-demo.sh`
- **依赖**：M5-T02、M5-T03
- **退出条件**：Zephyr/micropython 镜像构建运行通过；总二进制↑≤1.5%、L1i↓≥10%、iTLB↓≥15%。
- **测试用例**：`M5-T08-C1` demo 脚本 pass + 6 项指标达标。

### M5-T09 — 服务端 demo（clang / MySQL）

- **描述**：服务端 `full` 模式 + 量化门槛（SPEC §9.2 服务端）。
- **涉及文件**：`xbbr_design_doc/scripts/xbbr-server-demo.sh`
- **依赖**：M5-T06
- **退出条件**：clang/MySQL self-host/构建通过；L1i↓≥15%、iTLB↓≥20%、branch-miss↓≥10%、hot-text↑≤20%、总二进制↑≤3%、链接时间≤2.0×。
- **测试用例**：`M5-T09-C1` demo 脚本 pass + 6 项指标达标。

---

## 7. 关键路径与依赖（任务图）

```
M1-T01 ──┬─> M1-T02 ──┬─> M1-T03 ─┐
         │            ├─> M1-T04 ─┤
         └─> M1-T05 ──┘           ├─> M1-T06 ─┐
                                   └───────────┴─> M1-T07
                                                       │
                          M2-T01 <─────────────────────┤
                            │
                       M2-T02 ──> M2-T03 ──> M2-T05 ──> M2-T06
                         │           │
                    M3-T01           └─(决策map框架)
                      │
                  M3-T02 ──> M3-T03 ──> M3-T04 ──> M3-T05 ──> M3-T06 (量化验收, partial)
                                                          │
                              M4-T01 ──┐                  │
                              M4-T02 ──┼─> M4-T03 ──> M4-T06
                                       │      │
                                       │   M5-T01 ──> M5-T03
                                       │      │
                                       │   M5-T02
                                       │
                              M4-T04 ──> M4-T05 (BOLT)
                                       │
                              M5-T04 ──> M5-T05
                              M5-T06 (full)
                              M5-T07 (full LTO, 依赖 M1-T03)
                              M5-T08 (嵌入式, 依赖 M5-T02/T03)
                              M5-T09 (服务端, 依赖 M5-T06)

横切: X-T01 (确定性 CI, 随 M2 起填充) | X-T02 (文档 lint) | X-T03 (互斥, 随 M1/M2)
```

**关键路径**：M1-T01→M1-T02→M1-T03→M2-T01→M2-T02→M3-T01→M3-T02→M3-T03→M3-T04→M3-T05→M3-T06（partial 量化验收）。这是 M3 退出的最长依赖链。

---

## 8. 验收矩阵（量化指标 → 任务/测试）

对应 SPEC §9.2，每项指标在哪个里程碑/任务首次可测、由哪个测试 gate。

| 指标 | 嵌入式(partial) | 服务端(full) | 首测里程碑 | gate 任务 |
|---|---|---|---|---|
| L1i miss ↓ | ≥10% | ≥15% | M3 / M5 | M3-T06 / M5-T08 / M5-T09 |
| iTLB miss ↓ | ≥15% | ≥20% | M3 / M5 | M3-T06 / M5-T08 / M5-T09 |
| branch-miss ↓ | ≥8% | ≥10% | M3 / M5 | M3-T06 / M5-T09 |
| End-to-end perf | ≥Propeller-equiv | ≥Propeller-equiv | M3 / M5 | M3-T06 / M5-T09 |
| hot-text 增量 | ≤10% | ≤20% | M3 / M5 | M3-T06 / M5-T09 |
| 总二进制增量 | ≤1.5% | ≤3% | M3 / M5 | M3-T06 / M5-T08/T09 |
| 链接时间增量 | ≤1.5× | ≤2.0× | M3 / M5 | M3-T06 / M5-T09 |
| 可重现性 (SHA256) | bitwise-identical | bitwise-identical | M2 起 | X-T01 + 各 T 确定性子用例 |
| ABI 不变量 (SPEC §5) | 必须保持 | 必须保持 | M3 起 | M3-T01-C2 / M5-T06-C3 |
| EH/栈展开正确 | 必须保持 | 必须保持 | M4 | M4-T02 / M4-T03 |
| 调试归因 (gdb/perf/addr2line) | 必须正确 | 必须正确 | M4 | M4-T01 / M4-T04 |

---

## 9. 风险 → 任务映射（PLAN §11）

| 风险 | 缓解任务 | 关键测试 |
|---|---|---|
| 跨函数 BB 致 backtrace 错误 | M4-T01 | M4-T01-C3/C4 |
| FDE 拆分后 `.eh_frame_hdr` 未重建 | M4-T02 | M4-T02-C2（correctness gate） |
| ARM Thumb thunk 体积膨胀 | M5-T02 + M3-T04 | M5-T02-C1、M3-T04-C1 |
| 对齐填充 NOP 膨胀 | M3-T03 | M3-T03-C3 |
| 链接器松弛改 BB size | M5-T03 | M5-T03-C1/C2 |
| profile 与源码失配 | M1-T03 + M3-T04 | M1-T03-C4 |
| ExtTSP 跨函数规模退化 | M3-T01 | M3-T01-C4 |
| `.symtab` 索引跨文件失效 | M2-T01 | M2-T01-C2 |
| 非确定性排序 | X-T01 + 各确定性子用例 | X-T01-C1/C2 |
| 与 Propeller/IBT/BTI 冲突 | X-T03 + M5（IBT/BTI 验证，SPEC §12 #1/#2） | M2-T04-C2 |

---

## 10. 任务认领与状态约定

- 每个任务在开工前标 `in_progress`、完工且测试全绿后标 `completed`（用项目任务追踪工具或 PR 标签）。
- **"完成"定义**：退出条件全部满足 + 全部测试用例（含确定性子用例）通过 + 涉及的 SPEC/PLAN 已同步更新（doc-lint X-T02 通过）。
- **跨里程碑依赖**：下游任务不得在上游未 `completed` 时声称完成；若需并行，须以 mock/stub 隔离并在 PR 说明。
- **实验性前缀**（SPEC §13）：M1–M5 全部选项/section 以 `experimental-` 前缀进入；M5 完成且 §8 验收矩阵全绿后，统一去前缀（独立收尾任务，记为 `M5-T10`）。

---

**TASK 文档结束** —— 需求契约见 [`SPEC.md`](SPEC.md)，实现设计见 [`PLAN.md`](PLAN.md)。
