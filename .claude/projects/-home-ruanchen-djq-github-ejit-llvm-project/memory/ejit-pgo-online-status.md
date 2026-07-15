---
name: ejit-pgo-online-status
description: "EJIT 在线 PGO 项目进度与待决策点(设计文档 v0.6,P0+footprint 实测完成)"
metadata:
  node_type: memory
  type: project
  originSessionId: 1d6aa49c-891f-48b8-bd89-9f5121f73188
---

分支 `ejit_online_pgo`(从 ejit_dev_spec4 分出,含全部 PGO 工作 Stage 0~3a;用户选留此分支)在做 EJIT 在线 PGO(运行期采边频度反馈重编译)。设计文档 `jit_design_doc/EJIT_ONLINE_PGO.md`(v0.8)。三级分层:Tier-0 AOT fallback / Tier-1 JIT 特化+插桩 / Tier-2 JIT+PGOUse+全优化。

**P0 验证(2026-07-11)全过**:Gen->Lowering->Writer->Use 闭环跑通,`!prof` 标注成功,hash 匹配,计数器强制 ExternalLinkage 可行。关键实现细节(已入文档):计数器全局默认 InternalLinkage 须 `getGlobalVariable(name, true)`;`InstrProfWriter` 须显式 `mergeProfileKind(InstrProfKind::IRInstrumentation)`;`addRecord` 返回 void;profile 合成须在 `compileCold(tier=2)` 入口、`loadBitcode` 前(PGOUse 消费 ctx.profile,数据依赖)。

**Footprint 两维均已实测(完整图景)**:
- P0-1 运行时体积:增 `LLVMInstrumentation`+`LLVMProfileData` 后 **stripped ≈ +640 KB**(unstripped ≈ 1.0 MB;`--gc-sections` 裁掉 sanitizer,主体 InstrProf 写/读+OnDisk 索引 272 符号 + BFI/BPI + ProfileSummary)。约占 12MB 运行时二进制 5%。probe 见 `/tmp/pgo_p0_1_probe.cpp`(`#ifdef PGO_ENABLE` 两版,base 版也跑 PassBuilder 隔离 PGO 专属对象)。mitigation:走 lipo `ld -r --gc-sections --entry=ejit_init` 而非直链 fat archive。见 §11.12。
- P0-6 bitcode Flash:激进内联对小/可折叠 callee +14%~22%,仅中等 callee 多调用点省 −5.8%(推翻原"常 ≤"假设,见 §11.11)。

**待决策的下一步**:① 进 plan mode 排实现计划(阶段0 前提+stub+CMake / 1 / 2 / 3 激进内联+PASS1 改);② 直接实现阶段 0(CMake 加 PGO 依赖 + stub + 把 P0 测试提升为 gtest)。P0 测试构建配方见 [[ejit-pgo-p0-test-build]]。

**实现进度(门槛式计划已批准,`.claude/plans/ejit-online-pgo-impl.md`)**:
- **阶段0 已提交**(`e8ccdf0cc617`,opt-in 接线,PGO 关=零行为变更):Config::enablePgo、CompileTier+SpecializationContext.tier/profileData、EJitCompileRequest tier 编码(funcIndex 高2位,不改善知布局)、EJitCacheEntry/EJitSharedCacheSlot 加 hitCount+profcAddr+profdAddr、kEJitSharedAbiVersion 6->7、stats(tier1/tier2/profileMergeFails)、EJitProfileMerge skeleton、LLVMEJIT LINK_COMPONENTS += Instrumentation/ProfileData。3 套件全过(EJITTests81/TaskPool82/CodePool40);14 个 EJITSharedTaskPoolTests 失败 + check-ejit-size 超 10MB 均**既有**(stash 验证)。
- 阶段0 遗留:EJitPgoStubs.c 推迟(emitInitialization 仅在模块声明时引用,EJIT 不声明;P0 无 stub 跑通);fat archive(lipo 前)不加 PGO lib(避撑爆 check-ejit-size),lipo COMMON_LIBS + check-ejit-size 改用 lipo 产物留部署物整合;P0 gtest 提升推迟到阶段1。
- **下一步:阶段1**(Tier-1 Gen/Lowering/captureCounterGlobals + Tier-2 PGOUse,runPipeline tier 分支,compileCold 顺序约束,热点触发,cachePublish 重置),完成后量保底收益门槛再决定 2/3。

**阶段1 进展(3 提交,进行中)**:
- `ebf4e814a4d7` stage 1a:runPipeline tier 分支(Baseline/Instrumented/PGOUse)+ captureCounterGlobals(强制 `__profc_*/__profd_*` External + 记录 pgoName)+ EJitPgoTest gtest(Tier-1 产 `__profc_foo`+External;Tier-2 同源克隆标注 `!prof` 证 Gen/Use hash 匹配 + `!prof` 存活)。**§11.1 风险实测:低**(未被消除的分支 `!prof` 经 mainFPM_ 存活;被 select 化的分支丢 `!prof` 良性)。
- `a024dc5b0ff4` stage 1b:compileCold tier 注入(Config.enablePgo gate;首次=Instrumented,Tier-2=PGOUse)+ Tier-1 counter addr 捕获(ORC lookup `__profc_/__profd_`,存驱动侧 tier1Counters_ map)+ Tier-2 profile 合成(EJitProfileMerge 读 `__llvm_profile_data` FuncHash@8/NumCounters@48 + counter 值@profcAddr,在 loadBitcode 前)+ engine getLastCounterNames accessor。
- **Slice C 待做(task 15)**:热点触发(hitCount 累加 + 懒 Tier-2 enqueue,Async only)+ cachePublish 重置 hitCount/profcAddr/profdAddr(Tier-2 publish)+ version checkpoint 丢弃 Tier-2;驱动侧 tier1Counters_ map 应迁到 cache entry(经 publish)。Slice C 完成后才有端到端自动 PGO 流(PGO on -> Tier-1 采 -> 自动 Tier-2 重编译)。
- 当前状态:PGO on 时 Tier-1 插桩 + counter 捕获工作;但无自动 Tier-2 触发(trigger 未做),Tier-2 仅在手动构造 tier=PGOUse req 时跑。3 套件全过(EJITTests83/TaskPool82/CodePool40)。

**阶段1 Slice C 完成(非共享端到端,5 提交)**:
- `b003eddc2df9` 1c-part1:非共享 publish strip-tier-for-identity + Tier-2 reset + compileCold clear + gtest
- `837cf53577e9` 1c-part2:共享 cachePublish strip+reset + 非共享 reset 改无条件(两套 taskpool reset 缺口闭合,review Point 2 解决)
- `07c3e3335196` 1c-part3a:hitCount 原子化(EJitAtomic 加 move 语义 + EJitCacheEntry.hitCount=EJitAtomicU64)+ 命中自增 + hitCountOf accessor + gtest
- `bd26d48fd7e0` 1c-part3b:**自动 Tier-2 触发**(非共享,端到端):dedup strip tier + lookup tier2Arm 信号(threshold 跨越一次性) + compileOrGet 命中分支懒入队 encodeReqTier(PGOUse) req + gtest(setPgoEnabled(true,3)+3 命中触发+pollOne 发布 reset)。**PGO on -> Tier-1 采 -> 命中累加 -> 阈值触发自动 Tier-2 重编译 -> 覆盖 Tier-1(reset)** 跑通。
- 验证:EJITTests83 / EJITTaskPoolTests85 / EJITCodePoolTests40 全过;EJITSharedTaskPoolTests 55过/14既有失败无新增。PGO 关=零行为变更。
- **仍 deferred**:共享 taskpool 触发(对称,共享 dedup/queue + 14 既有失败,单独单元);三层 version checkpoint Tier-2 丢弃(§7.2,部分经现有 publish version gate);驱动侧 tier1Counters_ 迁 cache entry;EJitPgoStubs.c(verify-then-add);真实 ejit_init 端到端集成测试(各部件已分别测)。
- **下一步**:阶段1 gate--在代表性 ejit_entry 上实测保底收益(mispredict/icache,Tier-2 vs Tier-1/Baseline),justify 后才推进阶段2/3。或先补共享触发 + 真实集成测试。

**阶段1 gate 通过(v0.8,`2f93d49b556a`)**:保底收益(MachineBlockPlacement 块布局)实测确认。EJitOptimizer runPipeline 产 Baseline vs Tier-2(99/1 profile)IR,llc -O2 aarch64:MBP 消费 !prof **翻转块布局**,热路径从"分支 taken 99%"变"fall-through"(mispredict ~99%->~1% + icache 连续)。Caveat:收益条件性(Baseline 自然布局已最优则无改善)。!prof 经 mainFPM_ 存活到 codegen 被 MBP 消费。gate 工具 `/tmp/pgo_stage1_gate.cpp`。**justify 推进阶段2/3**。

**下一步(可推进)**:① 阶段2(IR profile 消费:LoopUnrollPass 替 LoopFullUnroll + PGOMemOPSizeOpt,低风险);② 阶段3(PGO 内联,默认自适应,最大收益/中风险,需 CGSCC + EphemeralValues 注册 + PASS1 改);③ 补 deferred(共享触发/version checkpoint/真实集成测试/EJitPgoStubs verify)。

**阶段2 完成(`e9434993fd6a`)**:Tier-2 专属 `mainFpmPgo_`(`LoopUnrollPass` profile-aware 替 `LoopFullUnrollPass` + `PGOMemOPSizeOpt`),Baseline 保持 `mainFPM_`(LoopFullUnroll)不变(opt-in 隔离)。`runOptimizationPipeline` 加 CompileTier 参数选 FPM。注意 `LoopUnrollPass` 是 FunctionPass(非 Loop pass),直接加 FPM 不入 LPM。EJITTests83/TaskPool85/CodePool40 全过,Baseline 零变更。

**下一步**:① 阶段3(PGO 内联,默认自适应,最大收益/中风险:CGSCC `ModuleToPostOrderCGSCCPassAdaptor` + `ModuleInlinerWrapperPass`,EJitPassBuilder 注册 EphemeralValuesAnalysis,PASS1 `preOptimizeBitcode` PGO 模式去 `buildModuleInlinerPipeline`,自适应默认按 callee 形态);② 补 deferred;③ 阶段4(HotColdSplitting 可选)。

**阶段3a 完成(`dd0df11505d2`,JIT 侧,在 ejit_online_pgo 分支)**:Tier-2 PGOUse 后跑 `ModuleInlinerWrapperPass`(CGSCC inline)。EJitPassBuilder 注册 3 个之前缺的分析:`EphemeralValuesAnalysis`(FAM,InlinerPass 必需)+ `InlineAdvisorAnalysis`(MAM,wrapper 必需)+ `LazyCallGraphAnalysis`(MAM,CGSCC adaptor 必需)。gtest `Tier2PgoInlinesHotCallee`(foo 调 bar,Tier-2 后 bar 内联进 foo)。EJITTests84/TaskPool85/CodePool40 全过。**注意**:3a 仅 JIT 侧,生产 bitcode 预内联无 callee 可内联(JIT inline 是 no-op),需 3b(PASS1 非预内联)才生效。

**阶段3 方向反转(v0.10,`18ef07459381`+`ace1e9e5151d`,经 call-back 开销论证)**:原 v0.9 "non-pre bitcode + JIT PGO 内联"反效果(去掉 AOT 预内联后冷小 callee 退化为 JIT 函数内 call = 要避免的 call-back 开销;JIT 只内联热的)。**改为:保留 AOT 预内联(buildModuleInliner 内联小/便宜 callee 消除其 call)+ JIT PGO 内联(Stage 3a 内联 AOT 没内联的热 medium/large callee)**。冷 callee 不内联(JIT hot-only 正确)。既最激进消除 call(只留冷 medium/large call),又避开 P0-6 Flash 代价(预内联不产生 +14~22%)。**放弃 3b-core**(不去 buildModuleInliner,无需 PASS1 改/clang 重建)。`EJitPgoPolicy`(non-pre 决策)moot 已删。Stage 3a 在预内联 bitcode 上已正确工作。EJITTests84 全过。

**下一步**:① Tier-1 heuristic inline(设计选项,Gen 后内联以特化 callee may_const);② 补 deferred(共享触发/version checkpoint/真实 ejit_init 集成测试/EJitPgoStubs verify);③ 阶段4(HotColdSplitting 可选);④ 真实闭包校准(Stage 1 gate 用 synthetic 模块,需真实 ejit_entry 闭包复测保底收益 + JIT PGO 内联效果)。**Stage 3 生产生效无需 clang 重建**(预内联保留 + JIT 内联已实现)。

**deferred 进展**:
- `0f4f0ccd5242` version checkpoint(§7.2)测试:toggle 在 Tier-2 arming 与 publish 间 -> Tier-2 丢弃(publish VersionMismatch,hitCount 不重置)。现有 publish version gate 足够,无需改码,测试钉死。
- `fee6c388dabf` EJitProfileMerge 真 addr 读取测试:构造 fake `__llvm_profile_data`(FuncHash@8/NumCounters@48)+ counter 数组,合成 profile 喂 PGOUse -> !prof(证 offset 读取 + FuncHash 匹配)。闭合了 EJitProfileMerge 偏移未测件(偏移错会静默产垃圾 hash -> 无 !prof 无收益)。
- `dd0153a956bf` Tier-1 heuristic inline 标 moot(v0.10 pivot 后:AOT 预内联小 callee + Tier-2 PGO 内联热 medium/large 已覆盖特化;Tier-1 cost-based inline 对 AOT 未内联的 medium/large 是 no-op)。
- stub verify:代码分析够(emitInitialization 仅在模块声明时引用,EJIT 不声明;P0 无 stub 跑通)-> 无需 stub。
- **仍 deferred**:① 共享触发(扩到 Async 多核部署目标,对称非共享,但共享 dedup/queue + 14 既有失败);② 真实全 runtime 集成测试(ORC lookup __profc_/__profd_ + compileCold 接线端到端,EJitProfileMerge 偏移已测但 ORC lookup + 全 wiring 未端到端测)。

EJITTests 85 / TaskPool 86 / CodePool 40 全过。

**⚠ v0.11 关键 finding(集成测试抓到 §5.2 bug,`ce01efcbac29`+`091408fd214d`)**:ORC lookup `__profc_*/__profd_*` **失败**。根因:JITDylib 符号 claim 在 `addIRModule` 时据原模块算,而 `__profc_*` 是 IRTransformLayer transform 内 Gen 生成(addIRModule 后,不在原模块)-> 未被 claim -> lookup 不到,即便 `captureCounterGlobals` 强制 External。**影响:Tier-1 捕获路径在生产不工作**(compileCold 的 `__profc_` lookup 失败 -> Tier-2 合成无 profile -> Tier-2 退化为无 PGO 的 Baseline,不崩但 PGO 失效)。此前 MockCompiler 触发测试 + fake-addr 合成测试均未覆盖此路径(都用 mock/fake 绕过了真 ORC lookup)。测试 `DISABLED_OrcLookupAndRealAddrProfileMerge` 留作复现。**修复方向**:不经 ORC lookup,改经码池内存管理器(post-compile 符号地址表)取计数器地址 + `JITDylib::define(absoluteSymbols)` 注册,或 transform 后手动注册。**这是 PGO 生产生效的阻塞项,须先修**。

**v0.12 §5.2 已修(`99ff0a02fdbf`+`280576a60434`)**:在 IRTransformLayer transform lambda 里 runPipeline 后,对 `__profc_*/__profd_*`(getLastCounterNames)调 `R.defineMaterializing(...)` 扩展 MR claim -> base 层 emit 注册 -> ORC lookup 可用。lambda 的 R 从 const 改非 const(	TransformFunction 本就期望非 const)。集成测试 `OrcLookupAndRealAddrProfileMerge`(原 DISABLED)现通过:真 ORC lookup + 真 addr FuncHash@8/NumCounters@48 匹配 IR Gen hash + 真 addr 合成 profile。**PGO 生产阻塞解除**:Tier-1 捕获 -> Tier-2 真 profile -> PGOUse !prof + JIT profile-guided 内联 + MBP 块布局。EJITTests 86 / TaskPool 86 / CodePool 40 全过。

**v0.13 真实闭包校准(`a32cd57197f7`)**:12 个真实 EJIT 测试程序(ejit_complex/perf_bench/zstd_bench/fold_loop/nested_struct/multidim/trace/timing/ptr_period/jit_verify/multiversion/new_attr)AOT 预内联后**全部 0 剩余 callee**(ejit_entry 37-87 insts,自包含)。**含义:JIT PGO 内联(Stage 3a)对真实 EJIT 闭包是 no-op**(无 callee 可内联)。**PGO 在真实 EJIT 的价值 = MBP 块布局(Stage 1 保底收益)only**,非内联。§0 原"PGO 最大收益在内联"对真实 EJIT 不成立(无 callee);内联仅未来大闭包(medium/large callee AOT 未内联)才相关。**PGO go/no-go:hinge on MBP 收益(Stage 1 gate v0.8 已证机制)是否 justify +640KB 运行时 + Tier-1 插桩 + Tier-2 重编译开销**。Stage 2(LoopUnroll)对小真实闭包边际。Stage 3(inline)moot。

**v0.14 真实闭包 MBP 校准通过(`9fffd3c37ba7`)**:ejit_complex_test.c 的真实 `process_multi_dim`(87 insts):12 个 `!prof` 经 runPipeline 存活到 codegen,MBP 消费后**真实 asm 与 Baseline 不同**--分支条件交换、块重排(热路径 fall-through)、分支方向反转(`b.ne`->`b.eq`)、冷块外提。**PGO 保底收益在真实 EJIT 闭包上验证成立**(非仅 synthetic v0.8 gate)。校准工具 `/tmp/pgo_real_mbp.cpp`。**PGO go/no-go 信息齐全**:MBP 收益已证真实闭包有效;代价 +640KB+插桩+重编译;JIT 内联对真实闭包 no-op。
