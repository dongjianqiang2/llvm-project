# EmbeddedJIT 在线 PGO 设计方案

**版本**: 0.7
**日期**: 2026-07-11
**关联文档**: SPEC4.md, PLAN4.md, PASS6_EJitStructFieldPass.md, PASS7_EJitRuntime_OrcJITLink.md, EJIT_SRE_TASKPOOL.md, EJIT_SRE_CODE_POOL.md, EJIT_TRIM_LLVM_BACKEND_EXPERIMENTAL_STUBS.md
**目标平台**: SRE 裸核(AArch64, RAM 100KB–2MB, 无文件系统, 单 worker)

> **v0.2 变更**: 经源码核实修正 5 处实质性错误(PGOInstrumentationGen 不产计数器全局、计数器符号 PrivateLinkage、lowering 拖入 runtime 符号、PGO 价值范围、InstrProfWriter 合成方式);新增分阶段 Pass 清单(§12)与分析依赖(§13);补充 Sync 模式 Tier-2 策略、CompileRequest/共享 ABI 约束、publish 字段重置。
>
> **v0.3 变更**: 阶段3 内联策略定为默认激进(PASS1 非预内联 + JIT PGO 内联);补 CFG hash 匹配约束、StructFieldPass 两次、PGO opt-in 隔离 Baseline、Tier-1 默认不 inline。
>
> **v0.4 变更**: P0 验证执行完毕(Gen->Lowering->Writer->Use 闭环跑通,`!prof` 标注成功);修正计数器全局为 InternalLinkage(非 Private)、补 `mergeProfileKind(IRInstrumentation)` 必要步骤、补 `LINK_COMPONENTS` 须加 ProfileData/Instrumentation/IPO。
>
> **v0.5 变更**: P0-6 实测激进内联 Flash 代价(三场景独立测量程序 `/tmp/pgo_p0_6_test.cpp`)。**推翻 v0.3/v0.4 "非内联版常 ≤ 内联版" 假设**:仅中等 callee 多调用点场景 B<A(−5.8%);小/可折叠 callee 场景 B>A(+14%~22%),成因=A 的 GlobalDCE 整删 callee 定义 + InstCombine 跨 callee 折叠。修正 §9 P0-5/P0-6、§12 阶段3 体积断言、新增 §11.11 Flash 代价风险。
>
> **v0.6 变更**: P0-1 运行时体积代价实测(独立 probe `--gc-sections`+`-Os`+strip):PGO 增 `LLVMInstrumentation`+`LLVMProfileData` 后 stripped 增量 **≈ 640 KB**(unstripped ≈ 1.0 MB),sanitizer 被 gc 裁掉,主体为 InstrProf 写/读+OnDisk 索引。更新 §9 P0-1 行、新增 §11.12 运行时体积风险。至此 P0-1(运行时)+P0-6(bitcode Flash)footprint 两维均实测。
>
> **v0.7 变更**:经独立检视核实并修正一批事实错误:(a)§0 价值前提--InstCombine 经 SimplifyLibCalls PGSO(`SimplifyLibCalls.cpp:1402/3457/3763`)**确消费 PSI/BFI**(v0.6 称"无 pass 消费"错),但 Baseline 无 ProfileSummary 时休眠、Tier-2 激活(小额 IR 层收益);(b)计数器全局默认 **PrivateLinkage**(P0 复测 value=8=Private;v0.4 误改 Internal 因 P0 枚举注释写反),撤回 v0.4 的 InternalLinkage 修正;(c)`InstrProfing.cpp` 文件名更正为 `InstrProfiling.cpp`;(d)P0-5 GlobalDCE cite 修正:`PassBuilderPipelines.cpp:1055` 那处条件性(CtxProfile+ThinLTOPostLink),无条件 GlobalDCE 在 :837/:1303;(e)§0 删"L1/L2/L3 固定编排";(f)§0 增"部署前提:Async 多核"(inline 是 stage 3 必做,单核 Sync 下 PGO 净负);(g)§12 阶段3 默认改"按 callee 形态自适应";(h)§13 EphemeralValues 补注(EJIT 自定义 PassBuilder 仍未注册);(i)§10 同步已实现项。

---

## 0. 背景与目标

EmbeddedJIT 已实现"时间窗常量 + 结构体字段特化":AOT 嵌入 bitcode,运行期按 `(funcIndex, dims)` 特化,经 InstCombine/StructFieldPass/标准优化生成机器码,存入 taskpool bucket cache。runOptimizationPipeline 为单一固定管线(optLevel 参数被忽略),但**优化决策不感知真实运行频度**--分支概率、块布局、循环展开均按静态启发式。

本方案在 EJIT 层引入**在线 PGO**:运行期采集边/块频度,反馈到重编译,用真实 profile 引导优化。面向 SRE 裸核,无文件系统、内存受限、单 worker。

**设计原则**:
1. 零文件 I/O--profile 全程内存合成与消费。
2. 最大化复用 LLVM 既有 PGO Pass;自研仅限 EJIT 特有策略与桥接。
3. 不破坏特化正确性与 fallback 语义。
4. 内存有界,可降级。

**部署前提(前置确认)**:PGO 的价值 hinge 在 Async 多核部署--Tier-2 重编译需 worker(stage 3 PGO 内联更是),单核 Sync 下要么 Tier-2 阻塞业务线程重编译、要么永不升级(白付 Tier-1 插桩开销),基本净负。EJIT 已确认要加 inline(stage 3),故**目标部署须为 Async 多核**;单核 Sync 部署应跳过 PGO 或仅做 stage 1 且接受不升级。

**PGO 价值范围(经核实,务必如实理解)**:
- EJIT 现有 IR pipeline 中,**InstCombine 经 SimplifyLibCalls 的 PGSO 消费 PSI/BFI**(`shouldOptimizeForSize`,`SimplifyLibCalls.cpp:1402/3457/3763`),但**仅在存在 ProfileSummary 时激活**(Baseline 无 summary -> 休眠;Tier-2 设 summary -> 激活,小额 IR 层收益)。其余 pass(SCCP/SimplifyCFG/ADCE/LoopFullUnroll/IndVarSimplify/LoopDeletion/Promote)不消费;`LoopFullUnrollPass` 硬编码 `/*BFI*/nullptr,/*PSI*/nullptr`(`LoopUnrollPass.cpp:1528`),且 JIT 禁用 Inline(`EJitOptimizer.cpp:12`)。
- 因此 PGO 的**保底收益主要来自后端 `MachineBlockPlacement` 的块布局**(热路径拉直、mispredict/icache 改善);Tier-2 另激活 InstCombine PGSO(见上)带来小额 IR 层收益,该 pass 在 `CodeGenOptLevel::Default` 下运行(`TargetPassConfig.cpp:1224`,EJIT 未降级)。
- IR 层的 profile 消费(内联/unroll/memop)**需要额外补 Pass**(§12 阶段 2/3),不是现成可得。
- may_const 特化已消除大量分支,PGO 真正发力的是"剩余数据相关分支"--收益真实但非数量级提升,阶段 1 后应实测决定是否推进到阶段 3。

---

## 1. EJIT 层 vs OrcJIT 层

PGO **Pass** 经 Orc 的 `IRTransformLayer` 执行(EJIT 已通过 `getIRTransformLayer().setTransform` 拥有该 hook,`EJitOrcEngine.cpp:699`),但**策略与管线归属 EJIT 层**。纯 Orc 层无法实现在线 PGO:它不感知特化上下文、taskpool、cache 版本、热点判定。本方案是"EJIT 层特性,借用 Orc transform 执行 Pass",与现有 `EJitOptimizer` 在同一 hook 内扩展。

---

## 2. 关键源码约束

| 事实 | 来源 | 对 PGO 的影响 |
|------|------|--------------|
| JIT pipeline 固定 4 阶段,FPM 缓存复用 | `EJitOptimizer.cpp:53-84` | PGO Gen/Use/Lowering 作新阶段插入,须遵守 FAM 复用语义(`clearAnalyses` 清理) |
| `ProfileSummaryAnalysis`/`BFI`/`BPI` 已注册 | `EJitPassBuilder.cpp:50-51,89` | PGO Use Pass 的直接依赖**已就位** |
| `PGOInstrumentationUse` 依赖 FAM proxy/TLI/BPI/BFI/LoopInfo/PSI/DomTree | `PGOInstrumentation.cpp:2377-2391` | 全已注册,不会 assert ✓ |
| `PGOInstrumentationGen` **只插 `llvm.instrprof.*` intrinsic + 名字全局**,不造计数器全局 | `PGOInstrumentation.cpp:960-1035` | **必须再跑 `InstrProfilingLoweringPass`** 把 intrinsic lower 成 `__profc_*`/`__profd_*`,否则 codegen 失败 |
| 计数器/数据全局默认 **PrivateLinkage**(P0 复测 value=8=Private;v0.4 误改 Internal) | `InstrProfiling.cpp:1557,1669`;`Module::getGlobalVariable` 默认 `AllowLocal=false` 跳过 local | ORC 按名 lookup **查不到**;`getGlobalVariable(name, /*AllowLocal=*/true)` 才能取到,transform 须强制 ExternalLinkage |
| `emitInitialization`(`InstrProfiling.cpp:2114`)把 `__llvm_profile_register_functions`(若模块声明)挂进 global_ctors;真正的 runtime 符号是单数 `__llvm_profile_register_function` | `InstrProfiling.cpp:2010,2018,2114` | 裸核无 compiler-rt -> 须提供 no-op stub(ORC 跳 global_ctors,该 ctor 不执行,仅链接需要) |
| code pool 数据段**保持 RW 不封固**,只封 exec 段 | `EJitCodePoolMemoryManager.cpp:29-33,55,174` | `__profc_*`/`__profd_*` 落 RW data 段,运行期可读写 ✓ |
| 后端 `CodeGenOptLevel::Default`(EJIT 未降级) | `JITTargetMachineBuilder.h:154`,`EJitOrcEngine.cpp:608` | `MachineBlockPlacement`/TailDup/BranchFolding/MachineSink 全部运行(`TargetPassConfig.cpp:1140,1186,1197,1212,1224`) |
| `SpecializationContext` 在 `EJitOrcEngine.h:49-58` | 字段:fnName/cacheKey/dimensions/optLevel | 加 `tier`+`profile` 字段;持 std::string,**不能进共享内存/队列** |
| 编译唯一入口 `compileCold`/`compileNow` | `EJitCompileDriver.cpp:292-421,494` | tier 经 `SpecializationContext` 注入;cacheKey = (funcIndex, dims),**不含 tier 也不含 version**(version 是独立 commit/lookup gate,`EJitTaskPool.cpp:113-124` identityMatches 只查 funcIndex+dims);故 Tier-1/Tier-2 同 key 覆盖正确 |
| `cachePublish` 同 identity 原地覆盖 fnPtr,旧指针锁外 `releaseFn_` 释放 | `EJitTaskPool.cpp:209-225`,`EJitSharedTaskPool.cpp:999-1055` | Tier-2 直接覆盖 Tier-1 指针;覆盖时须显式重置 hitCount/profcAddr/profdAddr |
| write-lock 自旋 drain readers==0 | `EJitRwLock.h:51-57`,`EJitSharedTaskPool.cpp:96-106` | 正在执行旧函数的线程安全(NO_RECLAIM 模式靠 seqlock) |
| 三层 version 检查 | `EJitTaskPool.cpp:150-156,576,596,200-206` | instance toggle 后 profile 作废,丢弃 Tier-2 |
| **Sync 模式无 worker** | `EJit.cpp:272,292` | "入队让 worker 编译"仅 Async 可行;Sync 下 Tier-2 须内联(阻塞)或只跑 Tier-1 |
| `EJitCompileRequest` 被 `static_assert(sizeof==72/64)` 锁死 | `EJitSreQueue.h:72-74` | 加 `tier` 须改 assert + 共享队列 ABI,或编码进现有字段高位 |
| 共享 `EJitSharedCacheSlot` 加字段须 bump `kEJitSharedAbiVersion` v6->v7 | `EJitSharedPlatform.h:74`,peer 校验 | 字段用 `EJitAtomicU64/U64` 保 POD/trivially-constructible |
| `SRE_CycleCountGet64`/`SRE_TaskDelay`/`EJitAtomic` | `EJitRuntime.cpp:29-30,863`,`EJitSreTask_sre.cpp:148`,`EJitAtomic.h:42-98` | 热点节流、采样时钟可用 |
| 无 per-function 调用计数 | `EJitTaskPool.h:256-265` | 需自建热点计数 |
| code pool 不回收 | `EJitCodePoolMemoryManager.h:19-21` | 计数器内存长期存活,需总量上限 |

---

## 3. 总体架构:三级分层编译

```
Tier 0  AOT fallback        原函数体;cache miss/编译失败/instance disabled 时执行(已有)
Tier 1  JIT 特化 + 轻优化 + 插桩   首次 JIT 产物;带 __profc/__profd 边计数器;运行期采集
Tier 2  JIT 特化 + PGO Use + 全优化 采样足够后重编译;吃 profile;计数器移除
```

### 3.1 数据流

```
miss ──► compileCold(tier=1)
         [特化 -> 轻fold -> PGOInstrumentationGen -> InstrProfilingLoweringPass]
         [transform: 强制 __profc_*/__profd_* ExternalLinkage,记录 {name,hash,counters} 映射]
         ──► 码池(RX)+ 计数器(RW)
                          │
            运行期: wrapper 命中 Tier1 fnPtr,边执行边累加 __profc
                          │
          hitCount ≥ 阈值 & version 稳定 ──► 入队 Tier-2(Async)或内联(Sync)
                          ▼
        compileCold(tier=2)
        ├─ 读 __profc(计数)+ __profd(FuncHash) ──► InstrProfWriter ──► writeBuffer() ──► InMemoryFS
        └─ [特化 -> PGOInstrumentationUse(InMemoryFS) -> (阶段3 Inline) -> StructFieldPass
              -> mainFPM(阶段2 LoopUnrollPass) -> PGOMemOPSizeOpt -> StructFieldPass -> cleanup]
              ──► 码池(RX)
                          │
        cachePublish 同 (funcIndex,dims,version) 覆盖 Tier1 fnPtr ──► 下次命中走 Tier2
```

### 3.2 与现有特化正确性的关系

Tier-1 与 Tier-2 消费**相同特化常量**(period index、may_const 值),且**起点 IR 形态一致**(同 bitcode + 同 preReplacePeriodIndices + 同 InstCombine),故 CFG hash 一致,PGOUse 能匹配。profile 来自 Tier-1 对同一特化的采样,分支概率对 Tier-2 有效。若 instance 被 toggle(version bump),三层检查丢弃 Tier-2,下次 miss 重 Tier-1。

---

## 4. PGO Pass 挂载点:`EJitOptimizer::runPipeline` 改造

`SpecializationContext`(`EJitOrcEngine.h:49-58`)增字段:

```cpp
enum class CompileTier { Baseline, Instrumented, PGOUse };
struct SpecializationContext {
  // ... 现有 fnName/cacheKey/dimensions/optLevel ...
  CompileTier tier = CompileTier::Baseline;
  struct { const char *data; size_t size; } profile = {nullptr, 0};  // Tier-2 用
};
```

`runPipeline` tier 分支:

```cpp
void EJitOptimizer::runPipeline(Module &M, const SpecializationContext &ctx) {
  // ===== 阶段 1:特化(不变)=====
  preReplacePeriodIndices(M, ctx);
  runInstCombine(M);

  if (ctx.tier == CompileTier::Instrumented) {
    // 特化 pass 1(entry 函数自身 may_const,可不经 inline 追溯到全局)
    runStructFieldPass(M);
    runLightOptPipeline(M);                 // InstCombine + SimplifyCFG(折叠特化暴露的分支)
    // ===== Gen 点 ===== 须与 Tier-2 PGOUse 点完全一致(同 preReplace+InstCombine
    //   +StructFieldPass1+lightOpt,且均 pre-inline),保证 CFG hash 匹配,PGOUse 能 annotate。
    ModulePassManager GenMPM;
    GenMPM.addPass(PGOInstrumentationGen(PGOInstrumentationType::FDO));
    GenMPM.addPass(InstrProfilingLoweringPass());  // intrinsic -> __profc_*/__profd_*
    GenMPM.run(M, MAM_);
    captureCounterGlobals(M);               // 强制 ExternalLinkage + 记录 {name,hash,counters}
    // [阶段3 激进] Tier-1 heuristic inline(默认开,Gen 之后):保留 callee may_const 特化与
    //   运行性能;Gen 仍 pre-inline 保 hash 匹配。可关以减编译时间(代价:callee may_const 不特化、运行变慢)
    // runHeuristicInline(M); runStructFieldPass(M);   // pass2:callee 内 may_const
    return;                                 // 交 codegen,计数器落 data 段(RW)
  }

  if (ctx.tier == CompileTier::PGOUse) {
    // 特化 pass 1 + lightOpt(与 Tier-1 Gen 点对齐 -> CFG hash 匹配)
    runStructFieldPass(M);
    runLightOptPipeline(M);
    // ===== PGOUse 点 ===== 与 Tier-1 Gen 点一致
    auto InMemFS = std::make_unique<vfs::InMemoryFileSystem>();
    InMemFS->addFile("/ejit.prof", 0,
        MemoryBuffer::getMemBufferCopy(StringRef(ctx.profile.data, ctx.profile.size)));
    ModulePassManager UseMPM;
    UseMPM.addPass(PGOInstrumentationUse(/*Filename=*/"/ejit.prof",
                                         /*Remap=*/"", /*IsCS=*/false,
                                         std::move(InMemFS)));
    UseMPM.run(M, MAM_);                    // 标注 !prof + setProfileSummary
    // [阶段3 激进,默认开] PGO-guided 内联(需 CGSCC + EphemeralValuesAnalysis)
    runPgoInline(M);                        // ModuleInlinerWrapperPass,吃 callee profile 决策
    runStructFieldPass(M);                  // 特化 pass 2(内联后 callee 内 may_const GEP 链展开)
    // [阶段2] mainFPM_ 用 LoopUnrollPass 替 LoopFullUnrollPass(profile-aware)
    runOptimizationPipeline(M, ctx.optLevel);
    // [阶段2 可选] PGOMemOPSizeOpt;[阶段4 可选] HotColdSplittingPass
    return;
  }

  // Baseline(默认,保持现有行为)
  runStructFieldPass(M);
  runOptimizationPipeline(M, ctx.optLevel);
}
```

**Pass 顺序要点**:
- **Gen/Use 点对齐(hash 匹配,正确性关键)**:`PGOInstrumentationUse` 按 (函数名, CFG hash) 匹配 profile 记录。Tier-1 Gen 与 Tier-2 PGOUse 前必须执行**完全相同**的前缀(`preReplace+InstCombine+StructFieldPass1+lightOpt`,且均 pre-inline),保证 CFG 一致。内联在 Gen/Use 之后。
- **PGOUse 在内联/优化之前**:标注 `!prof`+`setProfileSummary`(`PGOInstrumentation.cpp:2211`),后续 inline/mainFPM/codegen 经 BFI/BPI/MBFI 消费。
- **StructFieldPass 两次**:pass1(pre-inline,Gen/Use 前)处理 entry 自身 may_const(指针直接来自全局);pass2(内联后)处理 callee 内 may_const(内联展开 GEP 链才可追溯)。
- **PGOGen + Lowering 在 StructFieldPass1+lightOpt 之后**:插桩已特化、已折叠 IR。
- **`captureCounterGlobals` 在 lowering 之后**:`__profc_*`/`__profd_*` 已生成;遍历 `M.globals()` 强制 ExternalLinkage,记录映射。
- FAM 复用语义不变:每次编译前 `clearAnalyses()`(`EJitOrcEngine.cpp:713`)。

---

## 5. 计数器内存与 profile 合成(自研)

### 5.1 计数器布局与符号可见性

`InstrProfilingLoweringPass` 产出:
- `__profc_<PGOFuncName>`:i64 计数器数组。
- `__profd_<PGOFuncName>`:`__llvm_profile_data` 结构(NameRef/FuncHash/Counters 指针/NumCounters/...)。

落在 spec JITDylib 的 data 段,**RW 不封固**(§2)。默认 **PrivateLinkage**--`captureCounterGlobals` 强制 ExternalLinkage 后,ORC 才能 lookup。

### 5.2 取地址(Tier-1 编译后)

`EJitOrcEngine::lookup` 旁补:

```cpp
// captureCounterGlobals 已记录每个函数的 PGOFuncName(= 全局名去掉 __profc_ 前缀)
void *profc = orcEngine->lookup(cacheKey, "__profc_" + pgoName);
void *profd = orcEngine->lookup(cacheKey, "__profd_" + pgoName);
```

存入 `EJitCacheEntry` 新字段 `profcAddr`/`profdAddr`。

### 5.3 profile 合成(Tier-2 编译前,worker/调用线程内)

关键:`InstrProfWriter::addRecord(NamedInstrProfRecord &&I, uint64_t Weight, function_ref<void(Error)> Warn)`(`InstrProfWriter.h:115`),`NamedInstrProfRecord.Name` 是 **StringRef(函数名字符串)**,`Hash` 是 CFG hash。名字字符串从 `__profc_<name>` 符号名取后缀(我们在 `captureCounterGlobals` 已拿到),CFG hash 从 `__profd_*` 的 `__llvm_profile_data` 结构读 `FuncHash` 字段。

```cpp
InstrProfWriter Writer;
// 关键(P0 实测):手动 addRecord 不传播 IR-level 标志,须显式置位,否则
//   PGOInstrumentationUse 报 "Not an IR level instrumentation profile"。
if (auto E = Writer.mergeProfileKind(InstrProfKind::IRInstrumentation))
  consumeError(std::move(E));
for (auto &Item : tier1CounterMap) {          // {pgoName, profcAddr, profdAddr}
  auto *Data = reinterpret_cast<const __llvm_profile_data*>(Item.profdAddr);
  std::vector<uint64_t> Counts(Data->Counters, Data->Counters + Data->NumCounters);
  NamedInstrProfRecord Rec(Item.pgoName, Data->FuncHash, std::move(Counts));
  Writer.addRecord(std::move(Rec), 1, [](Error E){ consumeError(std::move(E)); });
}                                                // addRecord 返回 void,错误经 Warn 回调
auto ProfBuf = Writer.writeBuffer();          // unique_ptr<MemoryBuffer>, indexed 格式
// 挂进 SpecializationContext.profile {data, size} 供 runPipeline 的 InMemoryFS 使用
```

- `__llvm_profile_data` 结构布局来自 `llvm/ProfileData/InstrProfData.inc`(同 LLVM 编译,布局一致),按 compiler-rt 用法 `#define INSTR_PROF_DATA` 引入。
- `writeBuffer()` 返回 `unique_ptr<MemoryBuffer>`(`InstrProfWriter.cpp:705`),可直接给 `IndexedInstrProfReader` 或 `InMemoryFileSystem` 用。
- **不用 `MemoryBuffer::getMemBuffer`**(不持有数据);用 `getMemBufferCopy` 或 `writeBuffer` 的产物。
- 完全绕开 compiler-rt runtime 的 mmap/write/atexit。自研约 80 行。

**顺序约束(正确性关键,实现时钉死)**:Tier-2 读计数器用的是 Tier-1 编译时 `captureCounterGlobals` 捕获的 `profcAddr`/`profdAddr` **原始指针**,不是 ORC lookup。profile 合成(`读 __profc` + `addRecord` + `writeBuffer` -> `ctx.profile`)必须在 `compileCold(tier=2)` 入口、`loadBitcode` **之前**完成--因为 `loadBitcode` 的 `IRTransformLayer` 会跑 `PGOInstrumentationUse` **消费 `ctx.profile`**(数据依赖,非内存安全)。`compileCold` 须显式按序:**设 ctx -> 合成 profile -> loadBitcode(PGOUse 消费 profile)-> lookup -> publish**。

- **指针有效性(v1 实情)**:码池 v1 **永不释放 slab 内存**(`EJitCodePoolMemoryManager.cpp:37,102` "not freed in v1";`deallocate` 只丢簿记),`releaseFn_` 在引擎侧未接线(默认 null,旧码不回收)。故 `removeJITDylib`(`EJitOrcEngine.cpp:887-893`,删同 cacheKey 的 Tier-1 dylib)只丢符号表,**不释放计数器 data 段**--`profcAddr` 跨 dylib 删除仍指向有效内存,不因释放悬空。
- **真正的有效性 gate 是语义,不是释放**:`profcAddr` 仅在 Tier-1 仍是该 cacheKey 的 live 条目时代表有效 profile;version bump / `ejit_clear_cache` / 被别的重编译覆盖后,该内存虽未释放但已语义作废。合成前须由三层 version 检查(§7.2)校验 Tier-1 仍 live,否则丢弃 Tier-2。
- **前向兼容**:若未来码池升级为可回收(`deallocate` 真释放 slab),则"读在 loadBitcode 前"同时成为**内存安全**硬约束(违反即 UAF);当前 v1 仅为数据依赖+语义约束。`cacheKey` 无 tier 位(§2)使 Tier-2 重编译复用同 cacheKey、触发 `removeJITDylib`,是这条顺序的根因。

---

## 6. 热点检测与 Tier-2 触发

### 6.1 热点计数

`EJitCacheEntry`(`EJitTaskPool.h:150-156`)增 `EJitAtomicU64 hitCount`;共享版 `EJitSharedCacheSlot`(`EJitSharedTaskPoolState.h:147-173`)增 `EJitAtomicU64 hitCount` + `EJitAtomicUPtr profcAddr`/`profdAddr`(bump `kEJitSharedAbiVersion` v6->v7)。`classifyHit`/`bucketTryRead` 命中时 `hitCount.fetchAdd(1, Relaxed)`。

### 6.2 触发策略

- **A. 懒触发(推荐,Async)**:命中且 `hitCount >= kTier2Threshold`(默认 64)且 version 稳定 -> `queuePush` 一个 `CompileRequest{tier=PGOUse}`。复用 dedup/checkpoint/publish gate。
- **B. idle hook 采样**:`workerIdle_`(`EJitSharedTaskPool.h:138,242-245`)用 `SRE_CycleCountGet64()` 节流扫描热+稳定条目。作 A 的补充。
- **Sync 模式**:无 worker(`EJit.cpp:272,292`)。Tier-2 两种选择:
  - (a) 内联重编译(走 compileOrGet 的 Sync 分支,在调用线程编译)--阻塞业务线程,但简单;
  - (b) Sync 模式只跑 Tier-1,不升级。
  - 推荐:SRE 单核 Sync 部署选 (b);多核 Async 选 (a)+worker。文档与配置须明示。

### 6.3 `EJitCompileRequest` 携带 tier

`EJitCompileRequest` 被 `static_assert(sizeof==72/64)` 锁死(`EJitSreQueue.h:72-74`),且跨 Vyukov 环按值拷贝。加 `tier` 字段方案:
- 编码进 `generation` 高位或 `funcIndex` 高位(不改布局,首选);
- 或显式加 `uint32_t tier` 并同步改 static_assert(共享队列 ABI 变更,需与 peer 一致)。

---

## 7. 安全换入与版本交互

### 7.1 换入

Tier-2 产物以**相同 `(funcIndex, dims, version)`** 调 `cachePublish` -> 命中 identity -> 原地覆盖 fnPtr(`EJitSharedTaskPool.cpp:1002-1006`)。**覆盖时必须显式重置新字段**:

```cpp
E.fnPtr = newFn;
E.hitCount.storeRelaxed(0);     // 否则旧 hitCount 泄漏,误判热点
E.profcAddr.storeRelaxed(0);    // Tier-2 无计数器
E.profdAddr.storeRelaxed(0);
```

旧 Tier-1 码锁外 `releaseFn_` 释放;计数器内存随码池 slab 生命周期(见 §5.3 v1 实情:码池不回收 slab,`releaseFn_` 引擎侧默认未接线,故 v1 旧码与计数器均**不实际释放**;`releaseFn_` 仅为前向兼容 hook,未来码池可回收时再挂释放回调)。wrapper 无需改--下次 `tryCacheHit` 自动拿 Tier-2 指针。

### 7.2 version 交互

编译期间 instance 被 toggle -> 三层 checkpoint 丢弃(`EJitTaskPool.cpp:576,596`),profile 作废,下次 miss 重 Tier-1。

### 7.3 NO_RECLAIM 构建

fnPtr 永不释放,seqlock 读(`EJitSharedTaskPool.cpp:56-61`),Tier-2 publish 仅 bump `publishSeq`,旧 Tier-1 码不回收。内存预算(§8)须计这部分"僵尸码"。

---

## 8. 内存预算与降级(SRE 硬约束)

| 项 | 估算 | 控制 |
|----|------|------|
| 计数器 | 每特化 ~B 个 i64(小函数 10-50)-> 80–400 B | Tier-1 仅插桩采样子集;总量上限(默认 64KB)达阈后停止新插桩 |
| 瞬时 profile buffer | InstrProfWriter 输出 | `writeBuffer()` 后即释放,常驻一份 |
| Tier-2 码 | 与 Tier-1 同量级 | NO_RECLAIM 模式下双份码;限制同时存在的 Tier-2 数 |
| 码池碎片 | 不回收 | 复用 `getCodePoolStats` 监控 |

**降级链**:计数器内存不足 -> 停新插桩;Tier-2 编译失败 -> publish 失败 -> 继续命中 Tier-1;任何异常 -> fallback AOT。**永不破坏现有 fallback 语义**(SPEC4 §8)。

---

## 9. 复用 / 自研清单

| 组件 | 复用 LLVM 既有 | 自研 |
|------|---------------|------|
| 插桩 intrinsic | ✅ `PGOInstrumentationGen` | - |
| intrinsic -> 计数器全局 lower | ✅ `InstrProfilingLoweringPass` | - |
| profile 标注 | ✅ `PGOInstrumentationUse` + `vfs::InMemoryFileSystem` | - |
| profile 写出 | ✅ `InstrProfWriter::writeBuffer` | - |
| profile 读取 | ✅ `IndexedInstrProfReader`(Use Pass 内部) | - |
| 依赖分析 | ✅ PSI/BFI/BPI/SCEV/DT/LI/TTI/AC 已注册 | - |
| runtime 符号 `__llvm_profile_register_functions` | - | ✅ no-op stub(链接用) |
| 计数器符号强制 external + 映射捕获 | - | ✅ `captureCounterGlobals` |
| 计数器 -> record 合成 | - | ✅ 读 `__profc`/`__profd` + `addRecord`(~80 行) |
| tier 策略 / cache 管线 / 热点 | - | ✅ hitCount/阈值/触发/publish 覆盖 |

### P0 验证结果(已执行,2026-07-11)

在 `build_release_aarch64`(原生 aarch64,Release)用独立测试 `pgo_p0_test.cpp` 跑通 Gen->Lowering->Writer->Use 闭环。

| # | 项 | 结果 |
|---|----|------|
| 1 | trim 保留 PGO 依赖 | ✅ ProfileData/Instrumentation/VirtualFileSystem 源码与 CMake 均无 `EJIT_TRIM` guard,始终编译。**但** EJIT 运行时 `LINK_COMPONENTS`(`EJIT/CMakeLists.txt:114-123`)故意未链 `ProfileData`/`Instrumentation`/`IPO`(注释明说避免拖入 BitWriter/Linker/AsmParser)。→ 须加进 `LINK_COMPONENTS` + `ejit_minimal` 的 `EJIT_LLVM_LIBS`,**与库裁剪目标冲突**,增最小库体积。**P0-1 实测增量 ≈ 640 KB stripped**(见 §11.12)。 |
| 2 | 闭环:Gen->Lowering->Writer->Use | ✅ `PGOInstrumentationGen`+`InstrProfilingLoweringPass` 产 `__profc_*`/`__profd_*`;`writeBuffer` 产 indexed profile;`InMemoryFileSystem`+`PGOInstrumentationUse` 标注 `!prof = !{!"branch_weights", i32 100, i32 1}`(正是所设计数)。零文件 I/O。 |
| 3 | 计数器符号 + 强制 external | ✅ 默认 **PrivateLinkage**(实测 value=8=Private;v0.4 误读为 Internal 因 P0 枚举注释写反);`getGlobalVariable(name, /*AllowLocal=*/true)` 取到;`setLinkage(ExternalLinkage)` 成功。 |
| 4 | hash 匹配 | ✅ `__profd_*` 字段1 读出 FuncHash;Tier-1 Gen 与 Tier-2 PGOUse 在克隆同源 IR 上 hash 一致 -> Use 成功 annotate(annotate 即证明匹配)。 |
| 5 | `buildModuleInlinerPipeline` 含 GlobalDCE | ✅ `buildModuleInlinerPipeline` 含 `GlobalDCEPass`(`PassBuilderPipelines.cpp:1055` 那处条件性 CtxProfile+ThinLTOPostLink,无条件 GlobalDCE 在 `:837`/`:1303`)。→ 当前预优化内联后 **整定义归零**(GlobalDCE 删 callee,非"保留一份")。**故激进版(保 live callee)并非 ≤ 当前**--见 P0-6 实测:小/可折叠 callee 场景 B > A。v0.4 此处"常 ≤ 当前/Flash 赢"的推断已撤销。 |
| 6 | 内联 vs 非内联 bitcode 体积 | ⏳ → ✅ 见下:独立测量程序 `/tmp/pgo_p0_6_test.cpp`(复制 `extractAndSerialize` 两版 preOptimize:A=含 `buildModuleInlinerPipeline`,B=仅 `AlwaysInliner`+同 steps2-5;再 externalize 非常量全局 + `WriteBitcodeToFile` 量字节;delta 隔离内联决策,无需 clang 重建)。三场景(小模块,百分比上界偏大):**S1 可折叠算术+多调用点 +21.6%**(364B)**;S2 不可折叠(读 runtime 全局)结构地板 +14.2%**(236B)**;S3 中等 callee×5 独立调用点 −5.8%**(−108B,文档假设的 B≤A 体制,仅此处成立;A=1864B 甚至 >raw 1748B,5 份内联体 >1 定义+5 call)。成因两块:(a) 结构地板--A 的 GlobalDCE 整删 callee 定义,B 保留一份;(b) 折叠损失--A 的 InstCombine 跨 callee 折叠(如 `helper_small(a)=a+1`×5→`a+5`),B 的不透明 call 挡掉。 |

**P0 新发现(已并入设计)**:
- 计数器全局默认 **PrivateLinkage**(P0 复测 value=8=Private;v0.4 误改 Internal,因 P0 枚举注释"7=Private,8=Internal"写反,实为 7=Internal/8=Private);`Module::getGlobalVariable` 默认 `AllowLocal=false` 会漏取,须传 `true`。
- `InstrProfWriter` 手动 `addRecord` **不传播 IR-level 标志**,须显式 `mergeProfileKind(InstrProfKind::IRInstrumentation)`,否则 Use 报 "Not an IR level instrumentation profile"。
- `addRecord` 返回 void(错误经 `function_ref<void(Error)>` Warn 回调),非 Error 返回。

---

## 10. 改动点清单

### AOT / Pass 侧(小)
- `EJitWrapperGen.cpp`:可选--wrapper 入口加 per-funcIndex 调用计数全局(若不靠 cache hitCount)。
- `EJitRegisterBitcode.cpp`(阶段3,PGO 模式):`preOptimizeBitcode` 去 `ModuleInliner(O2)` 保 `AlwaysInline`,嵌入非预内联 bitcode 供 JIT PGO 内联。

### Runtime 侧(主)
- `EJitOrcEngine.h`(`SpecializationContext`):`CompileTier tier` + `profile` 字段。
- `EJitOptimizer.{h,cpp}`:`runPipeline` tier 分支;`runLightOptPipeline`;`captureCounterGlobals`;引入 Gen/Lowering/Use 头依赖。
- `EJitCompileDriver.cpp`:`compileCold` 据 `req.tier` 设 ctx;Tier-2 前调 profile 合成。
- `EJitTaskPool.{h,cpp}` / `EJitSharedTaskPoolState.h`:`EJitCacheEntry`/`EJitSharedCacheSlot` 加 `hitCount`+`profcAddr`+`profdAddr` **[done stage0]**;`EJitCompileRequest` 携带 `tier`(bit-pack funcIndex 高2位,未破 ABI)**[done stage0]**;`cachePublish` 覆盖时重置新字段 **[TODO stage1c]**;`kEJitSharedAbiVersion` v6->v7 **[done stage0]**。
- `EJitOrcEngine.{h,cpp}`:Tier-1 lookup 后补 `__profc_*`/`__profd_*` 查找 **[done stage1b;`getLastCounterNames()` accessor + compileCold 捕获]**。
- 新增 `EJitProfileMerge.{h,cpp}`:合成 + `__llvm_profile_data` 读取(FuncHash@8/NumCounters@48/counter@profcAddr) **[done stage0/1b]**。
- 新增 `EJitPgoStubs.c`:`__llvm_profile_register_function` no-op stub **[deferred]**:P0 闭环在无 stub 下跑通(emitInitialization 仅在模块声明该符号时引用,EJIT 不声明),verify-then-add。
- `EJitPassBuilder.cpp`:阶段3 补注册 `EphemeralValuesAnalysis`(必要时 `InlineAdvisorAnalysis`) **[TODO stage3;注:上游 PassRegistry.def:356 已注册,但 EJIT 用自定义 EJitPassBuilder,未注册]**。
- `EJitStats`:`tier1Compiles`/`tier2Compiles`/`profileMergeFails` **[done stage0]**。
- `EJIT/CMakeLists.txt`(P0-1):`LINK_COMPONENTS` 加 `ProfileData`/`Instrumentation` **[done stage0]**;`ejit_minimal` fat archive 的 `EJIT_LLVM_LIBS` 暂未加(避撑爆 check-ejit-size fat archive,lipo `--gc-sections` 部署物整合时再加,见 §11.12) **[deferred to lipo integration]**。

---

## 11. 风险与开放问题

1. **`!prof` 存活**:`!prof` 须从 PGOUse 经 IR opts 存活到 codegen MBFI。InstCombine/SimplifyCFG 改写分支可能丢 `!prof`。需实测;必要时 PGOUse 尽量靠后(StructFieldPass 后、mainFPM 前)。
2. **插桩开销**:Tier-1 每边一条 `add`。软实时需测 worst-case。缓解:低阈值短窗口、仅插桩热点候选、入口计数+选择性边。
3. **计数器符号可见性**:强制 ExternalLinkage 后能否被 ORC lookup(隔离 spec JITDylib 内应可)。需实测。
4. **`__llvm_profile_data` 布局**:按 `InstrProfData.inc` 读字段,同 LLVM 编译布局一致;跨版本须对齐。
5. **profile 跨核共享**:非 owner 核若 `EJIT_SRE_SHARED_CODE_POINTERS`,计数器 data 段 VA 须跨核一致。需平台确认。
6. **Sync 模式 Tier-2**:单核 Sync 部署只能内联重编译(阻塞)或不升级;默认建议不升级。
7. **阶段3 CGSCC 改造**:EJitOptimizer 现无 CGSCC 通道,加内联是结构性改造,工作量与风险最大,放最后。
8. **tier 与 LTO 路径**:LTO 下 bitcode 形态不同,插桩点需复核。
9. **激进内联拖累 Baseline**:PASS1 去掉 ModuleInliner 会使现有 Baseline 路径(callee 内 may_const 无法追溯)退化。须以 PGO opt-in 模式隔离:PGO 开才改 PASS1 + JIT 内联,关则维持现状。
10. **CFG hash 匹配**:Tier-1 Gen 与 Tier-2 PGOUse 前缀须完全一致,否则 profile 不匹配(该函数无 PGO 收益,不崩溃)。须保证两 tier 预处理确定性一致(may_const 值相同 + 同 pass 序列)。
11. **激进内联 Flash 代价(P0-6 实测)**:PASS1 去 `buildModuleInlinerPipeline` 后,嵌入 bitcode 对小/可折叠 callee **变大** +14%~22%(B>A),非 v0.3/v0.4 假设的"常 ≤"。两成因:(a) A 的 `GlobalDCE` 整删 callee 定义,B 保留一份(结构地板,随 callee 数线性增长,不随模块体积摊薄);(b) A 的 InstCombine 跨 callee 折叠,B 不透明 call 挡掉(转嫁为 JIT 时重内联+重折叠的 RAM/编译时间)。仅中等 callee 多调用点场景 B<A(−5.8%)。**影响**:SRE Flash 预算紧,激进版对小 callee 闭包(EJIT 常见)净增 Flash;须由 Tier-2 PGO 内联收益 justify,否则按 §12 阶段3 降级路径退保守(预内联 + JIT 仅补热点 callee)。 mitigations:按闭包 callee 体积/调用点数自适应选策略;Flash 预算达阈时局部回退预内联;实测时用真实 ejit_entry 闭包(非小模块样例)重测以校准百分比。
12. **PGO 运行时体积代价(P0-1 实测)**:PGO 须把 `LLVMInstrumentation`+`LLVMProfileData` 加进 `ejit_minimal` 的 `EJIT_LLVM_LIBS`(裁剪设计现整体排除 `libLLVMInstrumentation.a`,见 `EJIT_LIBRARY_TRIMMING.md` §4.7)。独立 probe 实测(`-Os` + `--gc-sections` + `strip --strip-all`,只留 PGO 可达对象,sanitizer/coverage 被 gc 裁掉;probe 两版同链同档,base 版也跑 PassBuilder 隔离 PGO 专属对象):**stripped 增量 ≈ 640 KB**(655,560 B;unstripped ≈ 1.0 MB),主体是 InstrProf 写/读 + OnDisk 索引格式(272 符号)+ BFI/BPI + ProfileSummary。约占 12 MB 运行时二进制的 5%、`check-ejit-size` 10 MB fat-archive 预算的 6.4%。**影响**:SRE Flash/RAM 紧,640 KB 非可忽略,但远小于 fat-archive 上界(整体 Instrumentation.a 含 sanitizer 达数 MB)。mitigations:(a) 务必走 lipo `ld -r --gc-sections --entry=ejit_init` 管线而非直链 fat archive--天然裁 sanitizer,只留 PGO 对象;(b) 阶段3 若不上 CGSCC 内联,可只保留 Gen/Lowering/Use 必需对象进一步瘦身;(c) `check-ejit-size` 10 MB 预算须重估,640 KB 须计入;(d) caveat:probe 用上游 PassBuilder,runtime 用裁剪版 `EJitPassBuilder`,PGO 对象闭包一致故 delta 代表性强,但精确字节数实测期须在真实 runtime 上复测。
---

## 12. 分阶段落地与 Pass 清单

### 前提(必做)
- Tier-1:`PGOInstrumentationGen` + `InstrProfilingLoweringPass` + no-op stub。
- Tier-2:`PGOInstrumentationUse(InMemoryFS)`。

### 阶段 1:零新 IR Pass,吃 codegen 块布局(保底收益)
不加 IR pass,确保 `!prof` 存活到 codegen。收益来自后端(已开,`CodeGenOptLevel::Default`):
`MachineBlockPlacement`(`:1224`)、`TailDuplication`、`BranchFolding`、`MachineSink`、`MachineCFG`(`:1140,1186,1197,1212`)。
**风险**:`!prof` 存活率(见 §11.1)。
**价值**:mispredict 减少 + icache 局部性--PGO 在 EJIT 的核心价值。阶段 1 后应实测决定是否推进。

### 阶段 2:IR 层 profile 消费(中收益,低风险)
FPM/MPM pass,无结构改动:
- **`LoopUnrollPass`** 替 `LoopFullUnrollPass`(`mainFPM_`,`EJitOptimizer.cpp:67`)。已验证 profile-aware(`LoopUnrollPass.cpp:1612-1614,231`)。对 may_const 特化后剩余非定常循环做 partial unroll/peel。
- **`PGOMemOPSizeOpt`**(FunctionPass,`PGOInstrumentation.h:104`):mem-intrinsic size 特化。
- (可选)**`PGOIndirectCallPromotion`**(ModulePass,`h:92`):SPEC4 暂不特化函数指针,收益有限。

### 阶段 3:profile-guided 内联(默认按 callee 形态自适应,最大收益,中风险,结构性改造)
**默认按 callee 形态自适应**(v0.7 改,与 P0-6 实测一致;原 v0.3"默认激进"对小 callee 是 Flash 代价):PASS1 嵌入未预内联 bitcode,JIT Tier-2 用 PGO 数据全权决定内联。这是 PGO 在 EJIT 的最大收益点(profile-guided 内联)。自适应规则:中等 callee 多调用点 -> 激进(非预内联 + JIT PGO 内联,B<A 省 Flash);小/可折叠 callee 主导 -> 保守(预内联 + JIT 仅补热点 callee,避 +14~22% Flash 代价)。

Tier-2 内联:`ModuleInlinerWrapperPass` + `InlinerPass`(CGSCC),用 PSI+BFI(`Inliner.cpp:215,382-383`),`DefaultInlineAdvisor` 自动创建(`:176`)。

**PASS1 改动(已核实 `EJitRegisterBitcode.cpp`)**:
- `computeTransitiveClosure`(`:82-100`)已收集全部内部 callee(有函数体、非 intrinsic);`extractAndSerialize`(`:311-334`)`CloneModule` 后仅删非闭包函数,callee 作为**独立函数定义**保留。故"保留 callee"无需新增收集,只需不合并。
- `preOptimizeBitcode`(`:189-240`,仅 Release/NDEBUG)step 1 = `AlwaysInlinerPass` + `buildModuleInlinerPipeline(O2)`(`:202-208`)是合并 callee 的根因。**改动:PGO 模式下 drop `buildModuleInlinerPipeline`(cost-based 那行),保留 `AlwaysInlinerPass`(mandatory,无 profile 决策);保留 steps 2-5(Mem2Reg/EarlyCSE/InstCombine/SimplifyCFG/reAnnotateMayConst,均 per-function,不跨函数合并)**。即删一行 + 加 mode 判断。
- `reAnnotateMayConst`(step 5)在非内联 bitcode 上仍生效,`!ejit.may_const` 被重建 -> JIT StructFieldPass 可用。✓
- `buildModuleInlinerPipeline` **含 GlobalDCE**(P0-5 已确认;`PassBuilderPipelines.cpp:1055` 那处条件性,无条件 GlobalDCE 在 `:837`/`:1303`),当前预内联后把 callee **整定义删掉**(非"保留一份")。激进版 callee 保持 live。**体积 delta 经 P0-6 实测并非"callsite 复制 vs 独立一份"那么简单**:小/可折叠 callee 场景 B>A(+14%~22%,A 还白赚跨 callee 折叠);中等 callee 多调用点场景 B<A(−5.8%,callsite 复制才占上风)。两版 `.bc` 用 `/tmp/pgo_p0_6_test.cpp` 量得(复制 `extractAndSerialize` + externalize + `WriteBitcodeToFile`,delta 隔离内联决策)。
- Debug 构建 `preOptimizeBitcode` 是 no-op(`:242`),本就非内联(但也不 reAnnotate);Release 才是部署目标。
- 符号注册(`generateSymbolRegisters`/`generateRegistryTable`)遍历原 Module M 的闭包,与提取模块是否内联无关,不受影响。✓

**PGO opt-in 模式(隔离 Baseline)**:去掉 ModuleInliner 会拖累现有 Baseline 路径(JIT 不内联时,callee 内 may_const 无法追溯)。故 PGO 须 opt-in:PGO 开启时 PASS1 产非预内联 bitcode + JIT 各 tier 内联;PGO 关闭时维持现状(预内联 + JIT 不内联)。两套 bitcode/管线由 AOT flag + 运行时配置切换。

**CFG hash 匹配约束(正确性关键)**:Tier-1 Gen 点与 Tier-2 PGOUse 点必须产出相同 CFG(否则该函数 profile 不匹配,无 PGO 收益,不崩溃)。两 tier 在 Gen/Use 前执行完全相同前缀:`preReplacePeriodIndices -> InstCombine -> StructFieldPass(pass1) -> lightOpt`,且均 **pre-inline**。内联在 Gen/Use 之后。

**StructFieldPass 两次**:
- pass 1(pre-inline,Gen/Use 前):entry 函数自身 may_const(指针直接来自全局)。
- pass 2(post-inline):callee 内 may_const 的 GEP 链内联展开后可追溯,再特化。

**Tier-1 heuristic inline(默认开,Gen 之后)**:非内联 bitcode 下,callee 内 may_const 需 inline 展开 GEP 链才能特化,故 Tier-1 在 Gen 之后做 heuristic inline + StructFieldPass2(≈ 当前 Baseline 特化码 + 计数器)。Gen 仍 pre-inline 保 hash 匹配;profile 是 per-function pre-inline 计数器,与 inline 无关。可关以减 Tier-1 编译时间(代价:callee may_const 不特化、运行变慢)。

**bitcode 体积(P0-6 实测)**:非内联版对小/可折叠 callee **更大**(B>A +14%~22%:A 的 GlobalDCE 整删 callee + InstCombine 跨 callee 折叠,B 都拿不到);仅中等 callee 多调用点场景 B<A(−5.8%,callsite 复制占上风)。单模式单 bitcode(PGO on=非内联,off=内联),不翻倍。**结论:激进版对 EJIT 常见的小/可折叠 callee 是 Flash 代价,非收益**;此 Flash 代价须由 Tier-2 PGO 内联收益 justify,且 AOT 丢的折叠转嫁为 JIT 时 RAM/编译时间(JIT 重内联 + 重折叠)。SRE 模块本身小,百分比非纯上界--结构地板随 callee 数线性增长,不随模块体积摊薄。降级路径:Flash 不可承受时退保守(预内联 bitcode,JIT 仅补内联 AOT 漏掉的热点 callee),见 §11.11。

**必须补注册** `EphemeralValuesAnalysis`(`Inliner.cpp:395`)--上游 `PassRegistry.def:356` 虽已注册,但 EJIT 用自定义 `EJitPassBuilder`(`EJitPassBuilder.cpp:35-68` 注册列表不含它),故 stage 3 须在 `EJitPassBuilder` 补注册;建议注册 `InlineAdvisorAnalysis`。

**结构改动**:EJitOptimizer 加 `CGSCCPassManager` + `ModuleToPostOrderCGSCCPassAdaptor`(当前只有 FPM/MPM)。阶段 3 主要工作量。

**顺序**:StructFieldPass1 -> PGOUse -> Inline(PGO) -> StructFieldPass2 -> opts。

**降级**:若 Flash/编译时间不可接受,退化为保守(预内联 bitcode,JIT 仅补内联 AOT 漏掉的热点 callee)--PGO 内联收益随之减小。

### 阶段 4:可选增强
- **`HotColdSplittingPass`**(ModulePass,`HotColdSplitting.h:69`):冷路径隔离,icache 收益。
- LoopPeel(已含在 LoopUnrollPass)。

### 修订后 Tier-2 管线(汇总)

```
preReplacePeriodIndices -> InstCombine
-> StructFieldPass(pass1)                       // entry 自身 may_const(pre-inline)
-> lightOpt                                     // 折叠特化暴露的分支(= Tier-1 Gen 点)
-> PGOInstrumentationUse(InMemoryFS)            // [前提] 标注 !prof + summary
-> [阶段3] ModuleInlinerWrapperPass             // PGO 内联(默认激进,需 CGSCC + EphemeralValuesAnalysis)
-> StructFieldPass(pass2)                       // callee 内 may_const(内联后追溯)
-> mainFPM_: InstCombine, SCCP, SimplifyCFG, ADCE,
            LoopSimplify, LoopUnrollPass(profile-aware),   // [阶段2] 替 LoopFullUnroll
            IndVarSimplify, LoopDeletion, Promote
-> PGOMemOPSizeOpt                              // [阶段2]
-> StructFieldPass (unroll 后再特化)
-> cleanupFPM_: InstCombine, SCCP, SimplifyCFG, ADCE
-> [阶段4] HotColdSplittingPass (可选)
// codegen: MachineBlockPlacement/TailDup/... [阶段1, 自动]
```

---

## 13. EJitPassBuilder 分析依赖补注册

| 阶段 | 需补注册的分析 | 必须? | 依据 |
|------|--------------|-------|------|
| 前提/1/2 | 无(BFI/BPI/PSI/SCEV/DT/LI/TTI/AC 均已注册) | - | `EJitPassBuilder.cpp:50-89` |
| 3 | `EphemeralValuesAnalysis` | 是 | `Inliner.cpp:395`;**注**:上游 `PassRegistry.def:356` 已注册,但 EJIT 用自定义 `EJitPassBuilder`(未注册它),故 stage 3 仍须补注册 |
| 3 | `InlineAdvisorAnalysis` | 建议 | 否则每次自建 default advisor(`:166,176`) |

---

## 14. 关键设计决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| PGO 归属 | EJIT 层(借 Orc transform) | 策略依赖特化/taskpool/cache |
| 分层模型 | Tier0/Tier1/Tier2 | 复用 fallback + cachePublish drain,wrapper 零改 |
| profile 介质 | 内存(`InMemoryFileSystem`) | SRE 无 FS;复用 `PGOInstrumentationUse` 不改源码 |
| 插桩 | `PGOInstrumentationGen` + `InstrProfilingLoweringPass` | Gen 只插 intrinsic,必须 lower |
| 计数器符号 | transform 强制 ExternalLinkage + 映射捕获 | 默认 PrivateLinkage,ORC 查不到 |
| runtime 依赖 | no-op stub `__llvm_profile_register_functions` | ORC 跳 ctor,仅链接需要 |
| profile 合成 | 名取自 `__profc_` 符号名,hash 取自 `__profd_` | `addRecord` 需名字符串+CFG hash |
| 触发 | 懒触发(Async)/不升级(Sync) | Sync 无 worker |
| 换入 | `cachePublish` 同 identity 覆盖 + 重置新字段 | write-lock drain 安全 |
| 失效 | 复用三层 version 检查 | instance 变更即丢 profile |
| 内联策略 | 默认按 callee 形态自适应(v0.7;原默认激进与 P0-6 实测冲突) | PGO 最大收益在内联;按闭包 callee 体积/调用点数自适应选激进/保守;PGO opt-in 隔离 Baseline |
| 内存控制 | 采样子集 + 总量上限 + 降级链 | 守 SRE RAM 约束 |

---

*文档版本: 0.4*
*创建日期: 2026-07-11*
*更新日期: 2026-07-11(v0.7: 检视核实修正 PGSO 前提/PrivateLinkage/GlobalDCE cite/文件名 + Async 前提 + 阶段3 自适应默认 + 同步实现状态)*
