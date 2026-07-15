# EJIT 在线 PGO 实现计划(门槛式:详排阶段 0+1,2/3 列轮廓 + 准入门槛)

## 目标与门槛策略

实现 PGO 前提(阶段 0)+ Tier-1 插桩 / Tier-2 PGOUse 保底收益(阶段 1),**全程 PGO opt-in 默认关**(零行为变更)。阶段 1 完成后设**实测门槛**:量保底收益(mispredict/icache,来自后端 `MachineBlockPlacement` 块布局),仅当收益 justify 才推进阶段 2/3。

**Footprint 预算(已实测,据此设门槛)**:
- 运行时 **+640 KB stripped**(P0-1,§11.12):计入 `check-ejit-size` 10MB 预算;走 lipo `--gc-sections` 而非 fat archive。
- bitcode Flash **+14~22%**(小/可折叠 callee,P0-6,§11.11):阶段 3 激进内联的准入门槛;中等 callee 多调用点才省(−5.8%)。

---

## 阶段 0:前提与接线(无行为变更,PGO 关)

| # | 文件 | 改动 |
|---|------|------|
| 1 | `EJIT/CMakeLists.txt` | `LLVMEJIT` LINK_COMPONENTS + `ejit_minimal` `EJIT_LLVM_LIBS` 加 `ProfileData`+`Instrumentation`(IPO/Scalar 已手动加)。校 `check-ejit-size` 吸收 +640KB(不够则提 `MAX_SIZE_MB` 或依赖 lipo gc)。 |
| 2 | 新 `EJitPgoStubs.c` | `__llvm_profile_register_functions` no-op stub。**仅 freestanding/裸核目标**,不进会拉 compiler-rt 的宿主测试二进制(避重复定义)。 |
| 3 | 新 `EJitProfileMerge.{h,cpp}` | 骨架:经捕获指针读 `__profc`/`__profd` -> `InstrProfWriter`(mergeProfileKind(IRInstrumentation)+addRecord+writeBuffer)-> `ctx.profile`。~80 行(§5.3)。 |
| 4 | `EJitOrcEngine.h:49` `SpecializationContext` | 加 `CompileTier tier` + `struct {const char* data; size_t size;} profile`。 |
| 5 | `EJitSreQueue.h:55` `EJitCompileRequest` | tier(2 bit)**编码进 `funcIndex` 高位**(不改善知布局、不 bump 队列 ABI)。加 encode/decode helper。 |
| 6a | `EJitTaskPool.h:150` `EJitCacheEntry` | 加 `EJitAtomicU64 hitCount` + `EJitAtomicUPtr profcAddr/profdAddr`。 |
| 6b | `EJitSharedTaskPoolState.h:147` `EJitSharedCacheSlot` | 同字段(POD,`EJitAtomicU64/UPtr`)。**bump `kEJitSharedAbiVersion` 6->7**(`EJitSharedPlatform.h:74`)。 |
| 7 | `EJitStats`(EJitTaskPool.h:257 / EJitSharedTaskPoolState.h:267) | 加 `tier1Compiles`/`tier2Compiles`/`profileMergeFails`(`EJitAtomicU64`,共享版入 ABI)。 |
| 8 | `EJitOptions` | `cl::opt` `ejit-pgo`(默认 off)+ AOT 侧 PASS1 模式 flag(阶段3 用)。off => 零行为变更。 |
| 9 | `llvm/unittests/ExecutionEngine/EJIT/` | 把 `/tmp/pgo_p0_test.cpp`(闭环)+`pgo_p0_1_probe.cpp`(footprint 回归)+`pgo_p0_6_test.cpp`(Flash delta)提升为 gtest。 |
| 10 | `EJitPassBuilder.cpp` | 本阶段不改(BFI/BPI/PSI 已注册,§13)。 |

**测试点**:现有 EJITTests 全过(PGO 关=无变更);新 gtest 复现 P0 闭环;`check-ejit-size` 含 +640KB 通过;ABI v7 peer 校验。

---

## 阶段 1:Tier-1 插桩 + Tier-2 PGOUse(保底收益 = 后端块布局)

| # | 文件 | 改动 |
|---|------|------|
| 1 | `EJitOptimizer.cpp:93` `runPipeline` | 加 tier 分支(§4):<br>• **Baseline**(PGO 关/tier=Baseline):现行管线不变。<br>• **Instrumented**:preReplace->InstCombine->StructFieldPass(pass1)->lightOpt(InstCombine+SimplifyCFG)->`PGOInstrumentationGen`+`InstrProfilingLoweringPass`->`captureCounterGlobals`->[heuristic inline+StructFieldPass2,默认开]->return(codegen,计数器落 RW data)。<br>• **PGOUse**:preReplace->InstCombine->StructFieldPass(pass1)->lightOpt->`PGOInstrumentationUse`(InMemoryFS,ctx.profile)->[阶段3 才内联]->StructFieldPass(pass2)->runOptimizationPipeline->return。<br>**Gen/Use 点对齐**:两 tier 同前缀(preReplace+InstCombine+StructFieldPass1+lightOpt,均 pre-inline)保 CFG hash 匹配(§11.10 正确性关键)。 |
| 2 | `captureCounterGlobals`(EJitOptimizer 或 EJitProfileMerge) | Lowering 后遍历 `M.globals()`,`getGlobalVariable(name,/*AllowLocal=*/true)`(P0-3)取 `__profc_*`/`__profd_*`,强制 `ExternalLinkage`,记录 `{pgoName, profcName, profdName, hash}`。 |
| 3 | `EJitOrcEngine` Tier-1 lookup | 编译后 `lookup(cacheKey, "__profc_"+pgoName)`/`"__profd_..."`,存 cache entry `profcAddr`/`profdAddr`(§5.2)。隔离 spec JITDylib 内可查(P0 已证)。 |
| 4 | `EJitCompileDriver.cpp` compileNow(:424)/compileCold(:292) | compileNow 解码 req.tier;**Tier-2**:读 Tier-1 cache entry 的 `profcAddr`/`profdAddr` -> `EJitProfileMerge` 合成 `ctx.profile` -> **在 `loadBitcodeModule`(:385)之前**(§5.3 顺序:PGOUse 消费 ctx.profile,数据依赖)-> 设 `ctx.tier=PGOUse`。compileCold 扩参或经成员透传 tier+profile。 |
| 5 | 热点触发(§6) | `classifyHit`/`bucketTryRead` 命中 `hitCount.fetchAdd(1)`;懒触发:`hitCount >= kTier2Threshold`(默认 64)+ version 稳定 -> 入队 `CompileRequest{tier=PGOUse}`。**Sync 模式不升级**(只 Tier-1)。 |
| 6 | `cachePublish` 重置(§7.1) | Tier-2 publish 重置 `hitCount=0`/`profcAddr=0`/`profdAddr=0`(`EJitSharedTaskPool.cpp:1051` / `EJitTaskPool.cpp:222`)。 |
| 7 | version 交互(§7.2) | 三层 checkpoint:instance toggle -> 丢弃 Tier-2,下次 miss 重 Tier-1。 |

**测试点**:
- gtest:Tier-1 产 `__profc_*`(ExternalLinkage)+ 捕获 addr;Tier-2 消费 profile + 标注 `!prof`;cachePublish 重置字段;version bump 丢弃 Tier-2。
- **`!prof` 存活**(§11.1 风险):验证 `!prof` 经 IR opts 存活到 codegen `MachineBlockPlacement`(MBFI)。必要时把 PGOUse 靠后(StructFieldPass 后、mainFPM 前)。
- **实测门槛**:在代表性 ejit_entry 上量 Tier-2 vs Tier-1/Baseline 的 mispredict/icache 改善。

---

## 门槛:量保底收益(阶段 1 完成)

- 收益 justify(+640KB runtime + 复杂度)-> 推进阶段 2/3。
- 不 justify -> 止步,PGO 对此 workload 不值。

---

## 阶段 2(门槛后,轮廓):IR 层 profile 消费

- `mainFPM_`(`EJitOptimizer.cpp:67`)用 `LoopUnrollPass`(profile-aware)替 `LoopFullUnrollPass`;对 may_const 特化后剩余非定常循环 partial unroll/peel。
- mainFPM 后加 `PGOMemOPSizeOpt`。
- 低风险(FPM pass,无结构改动)。

## 阶段 3(门槛后,轮廓):PGO-guided 内联(激进,最大风险)

- `EJitPassBuilder` 注册 `EphemeralValuesAnalysis` + `InlineAdvisorAnalysis`(§13)。
- `EJitOptimizer` 加 CGSCC 通道(`ModuleToPostOrderCGSCCPassAdaptor`)+ `ModuleInlinerWrapperPass`,在 Tier-2 PGOUse 分支跑 PGO 内联。
- PASS1(`EJitRegisterBitcode.cpp:205`):PGO 模式 drop `buildModuleInlinerPipeline`(保 `AlwaysInliner`)-> 非预内联 bitcode。**[P0-6 准入:小/可折叠 callee +14~22% Flash,按 Flash 预算门槛]**
- PGO opt-in 隔离:PGO 关 => 现行预内联 bitcode + JIT 不内联(Baseline 不退化,§11.9)。
- Tier-1 heuristic inline(默认开)+ StructFieldPass2(callee may_const)。
- Flash 不可承受 -> 退保守(预内联 + JIT 仅补热点 callee)。

---

## 跟踪的风险(文档 §11)

§11.1 `!prof` 存活(阶段1 测)、§11.11 Flash 代价(阶段3 门槛,P0-6)、§11.12 runtime +640KB(阶段0 预算,P0-1)、§5.3 顺序约束(阶段1 compileCold)、§11.10 CFG hash 匹配(阶段1 Gen/Use 对齐)。

## 不在本计划范围

阶段 4(HotColdSplitting,可选);LTO bitcode 形态(§11.8,需要时再议)。
