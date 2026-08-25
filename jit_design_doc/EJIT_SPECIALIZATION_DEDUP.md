# EmbeddedJIT 特化版本去重(特化后 IR 指纹合并)设计文档

**版本**: 0.4
**日期**: 2026-08-21
**状态**: **v1 已实现**(分支 ejit_spec_dedup;实现状态见 §14)
**修订**: 0.2 检视修订--v1 合并限定同 funcIdx(StructuralHash 不哈希签名/
  属性/flags,跨函数可构造碰撞,见 §5.4);新增 releaser 硬门(§5.5);
  DryRun 计数口径(§9);清理时机修正(ejit_clear_cache 现为 no-op)。
  0.3 --补指纹存放位置与内存布局(§5.5);新增代码回收与共享指针的
  三层复杂度拆解及回收路径排序(§13)
**关联**: EJIT_ICACHE_ANALYSIS.md, EJIT_ICACHE_MULTIVERSION.md, EJIT_ICACHE_SHARED_TABLE.md, EJIT_SRE_CODE_POOL.md, EJIT_SRE_TASKPOOL.md, PASS6_EJitStructFieldPass.md, PASS7_EJitRuntime_OrcJITLink.md
**目标**: 消除内容等价的特化版本造成的 JIT 代码段膨胀,改善 icache/代码池占用

---

## 1. 背景与问题

多维 `ejit_dim` 特化按 `(funcIndex, dims)` 身份组合出多个特化版本。每个版本独立
codegen、独立占用 code pool,**当前没有任何去重与回收**:

- 每次特化 = 新建独立 JITDylib(`spec_<cacheKey>`)+ 一次
  `allocateCode`(EJitOrcEngine.cpp `loadBitcodeModule`);
- code pool 纯 bump 分配,"Pool memory is not reclaimed in v1"
  (EJitCodePoolMemoryManager.cpp `FinalizedInfo` / `deallocate` 注释);
- 生产环境 `setReleaser` 未接线(仅单测使用),驱逐/重编译都不释放代码。

当多个 instance(如 N 个同配置小区/通道)的 period 数组**内容相同**、或特化常量
实际不影响代码时,产生的多份机器码完全等价,却各占一份代码段。这直接放大了
`.text.ejit` 的驻留体积与 icache 工作集(问题背景见 EJIT_ICACHE_ANALYSIS.md,
多版本分发机制见 EJIT_ICACHE_MULTIVERSION.md)。

## 2. 现状关键事实(方案边界由它们决定)

| # | 事实 | 出处 |
|---|---|---|
| F1 | 编译严格串行:async 走单一 owner worker `runCompile`;sync 模式也仅 owner 可内联编译 | EJitSharedTaskPool.cpp `runCompile` / `compileOrGet` sync 分支 |
| F2 | cache slot 按 `(funcIndex, dims)` 身份键控,`fnPtr`/`codeStart`/`codeSize` 只是值;dispatch、inline-cache cell、per-core L0、`executableCoreMask` 全部按身份或 slot 维度工作,无 "fnPtr 反查身份" 假设 | EJitSharedTaskPoolState.h `EJitSharedCacheSlot`;EJitRuntime.cpp L0 路径 |
| F3 | 特化真实输入 = bitcode(funcIdx) + cellIdx + **运行期数组内容**(StructFieldPass 编译期 `memcpy` 活内存) | EJitOptimizer.cpp `preReplacePeriodIndices`;EJitStructFieldPass.cpp 读值路径 |
| F4 | 特化 pipeline 挂在 LLJIT IRTransformLayer,`lookup` 触发 materialization 时才执行 | EJitOrcEngine.cpp `setTransform` lambda |
| F5 | code pool FinalizedRanges append-only,`findRange` 可解析任意已发布指针的区间 | EJitCodePool.h `recordFinalizedRange`/`findRange` |
| F6 | peer 4K seal 按 slot 独立跟踪(`executableCoreMask`),平台 `enable_ex` 幂等 | EJitSharedTaskPoolState.h;EJitSrePlatform |

## 3. 目标与非目标

**目标**

1. 内容等价的特化版本共享同一份机器码:新身份的 cache slot 发布**已存在版本的
   fnPtr**,不再为新版本分配 code pool;
2. 合并判定绝对保守:任何不确定因素都只能导致"不合并",绝不导致"错误合并";
3. 与现有分发机制零冲突:cache publish、inline-cache、L0、跨核 seal、驱逐/重编译
   语义全部不变;
4. 可先测量后启用(DryRun 模式),可配置关闭。

**非目标**

- 不做代码段回收/去分配(仍遵守 v1 "sealed 页不回收" 约束,只是让等价版本
  **根本不产生**第二份代码);
- 不追求降低编译时延(去重命中仍需 parse + 特化 pipeline,省的是 codegen +
  link + seal + pool 分配);
- 不合并"语义相似但不同"的版本(仅字节级等价判定);
- 不改共享内存结构 ABI(`EJitSharedTaskPoolState` 不动,abiVersion 不变)。

## 4. 候选方案与选择

| 方案 | 判据 | 判定点 | 需要回收? | 结论 |
|---|---|---|---|---|
| A. 编译后比机器码 | 全 allocation 字节相同 | codegen 后、finalize/seal 前 | 需要 pool bump 回滚 API | 备选,暂缓 |
| B. 特化后 IR 指纹 | post-pipeline IR 结构相同 | pipeline 后、codegen 前 | 不需要(重复代码不产生) | **采用** |
| C. 编译前按数组内容 hash | 引用 cell 字节相同 | 编译前 | 不需要 | **否决**(见 §5.1) |
| D. 记录"替换值向量"指纹 | 替换出的常量集合相同 | pipeline 内 | 不需要 | **否决**(见 §5.1) |

确定性的单线程编译下,"post-pipeline IR 相同"与"机器码相同"命中的集合一致
(同进程、同 TargetMachine、外部符号同地址),B 以更小的机制覆盖了 A 的全部收益,
且免除 A 的三大负担:pool 回滚、seal 前决策时序、debug 段/模块名字节剔除
(`spec_<cacheKey>` 名字只影响机器码比对,不影响 IR 指纹,见 §6.2)。
A 仅在"存在未预见的 codegen 不确定性"时才有兜底价值,留作观察 DryRun 数据后
的可选项(§12)。

## 5. 关键设计决策

### 5.1 指纹必须取在 pipeline 之后(否决 C/D 的反例)

cellIdx 本身是被 `preReplacePeriodIndices` 烘进 IR 的常量。若函数分支依赖 idx:

```c
EJIT_ENTRY void f(EJIT_DIM(cell) uint8_t idx) {
    if (idx == 3) { x = g_arr[idx].a; }   // cell 0 与 cell 5 走不同分支
    else          { x = g_arr[idx].b; }
}
```

cell 0 与 cell 5 即使**全部字段内容相同**,特化结果也在结构上不同(折叠了不同
分支、读了不同字段)。方案 C(整 cell 内容相同)与方案 D(替换值集合相同,两者
恰好相等)都会错误合并。

而取 pipeline 之后的 IR 做指纹,idx 常量就在 IR 里:分支折叠不同 ⇒ IR 不同 ⇒
不合并。**pipeline 输出天然是全部特化输入(F3)的依赖闭包**,无需逐项枚举输入,
也就没有枚举遗漏。

### 5.2 等价判据与正确性链条

```
指纹相同 ⇒ 特化后 IR 结构相同
        ⇒ (编译串行确定、同进程同 TM、外部符号同地址)
        ⇒ 机器码相同 ⇒ 旧 fnPtr 对新身份语义正确
```

任一环节的怀疑点都必须往"指纹不同"方向失效(§8)。

### 5.3 判定点前移:重构 compile 链

现状 pipeline 在 IRTransformLayer 里(F4),materialization 内部无法"返回已有
地址替代发码"。重构为**急切执行**(全部仍在 owner 核串行,F1):

```cpp
// EJitOrcEngine 新 API,替代 loadBitcodeModule + lookup 两步外部调用
struct SpecializeResult { void *fnPtr = nullptr; bool deduped = false; };

Expected<SpecializeResult> EJitOrcEngine::specializeAndResolve(
    StringRef bitcodeData, uint64_t cacheKey, const std::string &fnName) {
  // 1. parse + 前置修正(dso_local、static entry 外链化)
  //    -- 今 loadBitcodeModule 开头段原样搬
  // 2. optimizer->clearAnalyses(); runPipeline(M, ctx)
  //    -- 从 IRTransformLayer lambda 搬出;dump _pre.ll/_opt.ll、
  //       captureDump 一并搬(同一 lambda 内的既有逻辑)
  // 3. fp = hashModuleIR(M)                       // §5.4
  // 4. if (DedupEntry *E = dedupIndex_.find(cacheKey>>32 /*funcIdx*/, fp))
  //       return {E->fnPtr, /*deduped=*/true};    // 到此为止:不建 JD、
  //                                                // 不 codegen、不占 pool
  // 5. 今 loadBitcodeModule 尾段:收集 globalSymbols、建 spec JD、
  //    absoluteSymbols、addIRModule(transform 层此时直通,§5.6)
  // 6. fnPtr = lookup(cacheKey, fnName);          // 现逻辑不变
  //    dedupIndex_.insert(funcIdx, fp, fnPtr);
  //    return {fnPtr, false};
}
```

- `compileCold`(EJitCompileDriver.cpp)改调这一个 API;dims 校验、isActive
  检查不动;taskpool 的 `runCompile`/sync 路径**零改动**——`compileFn_`
  返回旧 fnPtr 后,`codeRangeFn_`、版本门(cp1/cp2)、`cachePublish` 照旧;
- dedup 命中时该 cacheKey **不创建 JITDylib**:`specDylibs` 无此 key,
  `ejit_clear_cache`/重编译遍历不到,无需清理,行为一致;
- legacy 调用方 `compileCold(cacheKey, /*storeLru=*/true)` 走同一 API,
  行为不变。

### 5.4 指纹构造:`StructuralHash` + initializer 补充哈希

采用 in-tree `llvm::StructuralHash(M, /*DetailedHash=*/true)`
(llvm/lib/IR/StructuralHash.cpp),已核实其跨编译确定性恰好满足需求:

- GlobalValue(外部函数/全局)按**名字**哈希(`hashGlobalValue`)——两次 parse
  的不同指针无影响;名字不同 ⇒ 指纹不同,保守方向正确;
- 本地 value 按首次出现顺序分配 ID(`ValueToId`)——结构相同 ⇒ ID 序相同;
- **Module 名不参与哈希**(`update(M)` 只遍历 globals/functions)——
  `spec_<cacheKey>` 天然无害,无需规范化;
- 常量(含替换进的 ConstantInt)在 DetailedHash 下进指令操作数哈希——
  **特化常量差异在此被捕获**。

**必须补的盲点**:`update(const GlobalVariable &GV)` 对模块内定义的全局只哈希
类型 ID,不含 **initializer**;指令引用 GV 也只按名字。风险场景:O2-ish
`runOptimizationPipeline` 中 GlobalOpt 可能把静态变量提升为常量全局、把特化
常量编进 initializer——两个版本指令完全相同、仅 initializer 不同,会漏检
(与机器码方案下 AArch64 literal pool 是同类问题,提前到 IR 层)。

补法:自写 ~30 行 initializer 哈希 walker(类型 + Constant opcode + 递归操作数,
GV 引用按名字),对每个非声明 GV 折进指纹。不用 `M.print`→SHA-1 兜底
(全模块打印代价高;walker 足够)。

**二级哈希**:叠加一个独立 FNV-1a 于指令流,`DedupEntry` 存双哈希,命中需两个
都相等,压缩碰撞概率到可忽略。

**覆盖面与构造保证(检视修订)**:`StructuralHash` 不哈希的维度比想象的多--
除 initializer 外,还包括**函数签名(参数/返回类型,只哈希 arg_size 与
isVarArg)、函数属性(byval/sret/align 等)、指令 flags(nsw/nuw/volatile/
atomic/fastmath)、全部 metadata**(`update(const Function&)` 与
`hashInstruction` 的实现)。这些维度不逐一补哈希,而是由 **v1 合并范围限定在
同 funcIdx 内**一次性覆盖:同 funcIdx ⇒ 同一 bitcode ⇒ 签名/属性/metadata
构造上全同;flags 由同一确定性 pipeline 产出,亦构造上全同。限定不成立的
反例(跨函数碰撞,已核实可构造):

```llvm
define void @f(i32 %a) { ret void }   ; %a 未使用
define void @g(i8  %a) { ret void }   ; arg_size 同为 1,体同构
                                      ; -> StructuralHash 相同,签名却不同
```

跨 funcIdx 合并若未来开启,必须先补齐签名 + 属性 + flags 哈希(见 §12)。

**实现期断言**(写进实现清单):

- [ ] gtest 断言同一输入两次编译指纹相同(确定性回归);
- [ ] gtest 断言不同 cellIdx 且内容不同的两次编译指纹不同;
- [ ] 构造 GlobalOpt 提升用例,断言 initializer walker 使指纹分化;
- [ ] gtest 断言同 funcIdx 不同签名的两个函数(手构 bitcode)不会被合并
      (key 含 funcIdx 后构造上不可能,断言防回归)。

### 5.5 去重索引

```cpp
struct DedupEntry {
  uint32_t funcIndex;   // v1 限定同 funcIdx 合并(见 §5.4 覆盖面说明)
  stable_hash fp;      // StructuralHash(Detailed)
  uint64_t fp2;        // FNV 二级哈希
  uint64_t irUnits;    // 函数数 + 指令计数粗计数(快速比对)
  void *fnPtr;         // 已发布版本的入口
};                     // 40B/条(512 项 = 20KB)
```

**存放位置**:owner 编译核私有内存,挂 `EJitOrcEngine::Impl`(pimpl)上,
与 `specDylibs` 同级同生命周期。不放跨核共享段(查询/插入只在 owner 编译线程
发生,放共享段要动 abiVersion 而收益为零;命中结果对 peer 的可见性走既有
cache slot 的 fnPtr);不存 IR 副本,只存哈希--这是用双哈希 + irUnits
代替"留全文精确比较"的原因:

```
┌─ 跨核共享段(EJitSharedTaskPoolState)────┐  ┌─ owner worker 核私有 ───────────┐
│  cache buckets/slots (identity -> fnPtr)   │  │  EJitOrcEngine::Impl           │
│  queue / 在飞 dedup / counters             │  │    specDylibs (map)            │
│  ← 指纹不放这里:ABI 零改动                │  │    dedupIndex ← 指纹在这里     │
│                                            │  │    (40B × 512 = 20KB 定长)    │
└────────────────────────────────────────────┘  └────────────────────────────────┘
```

读写时机都在 `specializeAndResolve` 内、编译线程上完成:pipeline 后算指纹
-> `find(funcIdx, fp)` -> 命中返回旧 fnPtr;未命中走完 codegen/lookup 后
`insert`。无锁(F1 串行),无跨核发布。

- **owner 私有、定长、开地址**:默认 512 项,满则对新条目静默降级
  (记一次日志)。定长换嵌入式内存可预测;挂引擎私有状态(与 `specDylibs`
  同生命周期);
- **key = (funcIndex, fp, fp2, irUnits)**:v1 不做跨函数合并。理由见 §5.4--
    `StructuralHash` 不哈希签名/属性/flags/metadata,跨函数碰撞会产生
    wrong-code(调用约定/属性不匹配);同 funcIdx 下这些维度由同 bitcode
    构造上保证相同。跨函数合并移至 §12,前置条件是补齐这些维度的哈希;
- **清理时机**:owner 代数更替(generation bump)时清空(唯一现实触发点;
  `ejit_clear_cache` 现为 no-op--legacy LRU 已退役,EJit.cpp `clearCache`)。
  严格说 v1 代码永不回收、旧 fnPtr 永远可调用(F5),不清也健全;清空是保守
  策略,换取"索引生命周期与 ORC 引擎一致"的心智模型;
- **releaser 硬门(检视修订)**:`runCompile`/sync 路径在 VersionMismatch 与
  publish 失败时会对 `compileFn_` 返回的 fn 调 `releaseFn_`(EJitSharedTaskPool.cpp
  `runCompile` 尾部、sync 分支)。dedup 命中时该 fn 是**其他身份正在派发的
  共享指针**,一旦释放即多身份 UAF。因此 dedup 启用是**硬不变量:
  dedupMode != OFF ⇒ releaser 必须为 null**--init 时 assert,或检测到
  `setReleaser` 接线即自动降级 dedup OFF(与 `icacheReclamationSafe_` 门
  同构;该门只管 icacheFill,管不到本路径,不能作为替代)。

### 5.6 IRTransformLayer 直通

引擎加 `P->preSpecialized` 标志:第 5 步 `addIRModule` 前置位,transform
lambda 检查到即原样返回。编译全部串行在 owner 核(F1),普通 bool 无竞态,
加 assert。dump/capture 逻辑随 pipeline 迁走后,transform 内不再有其他职责。

### 5.7 发布路径零改动的逐点核对

去重命中后,新身份 slot 发布**旧 fnPtr + 旧代码区间**,现有机制逐点透明:

| 机制 | 键 | 共享同一 fnPtr 的影响 |
|---|---|---|
| cache slot / `cachePublish` | `(funcIndex, dims)` | 无:别名的只是 fnPtr 值,发布 API 不改一行 |
| `codeRangeFn_` → `findRange` | FinalizedRanges(append-only,F5) | 旧指针照常解析出旧区间 |
| inline-cache cell / per-core L0 | 身份 | 无(每身份一个 cell,值相同而已) |
| peer 4K seal `executableCoreMask` | 每 slot 独立 | 同一页可能被 peer 经两个 slot 各 seal 一次;`enable_ex` 幂等(F6),冷路径小开销 |
| 驱逐/重编译 | slot | 驱逐后再编译 → 指纹命中 → 又发布旧指针,天然幂等 |
| `releaseFn_`(VersionMismatch/publish 失败路径) | - | **唯一例外**:会对 `compileFn_` 返回的共享指针调释放;由 §5.5 releaser 硬门排除(dedup ON ⇒ releaser 为 null) |
| `dispatchEpoch` | 全局事件 | 发布行为与普通发布一致,无增量 |

## 6. 失效方向分析(方案核心安全性)

| 不确定因素 | 失效方向 |
|---|---|
| StructuralHash 未覆盖的边角(metadata/attr/打印不确定性) | 指纹不同 → 不合并(安全) |
| initializer walker 遗漏新形式的 IR 差异 | 同上 |
| 索引满 | 新条目不去重(安全) |
| 哈希碰撞 | 双 64 位哈希 + irUnits 都相等,概率压到可忽略(512 条,生日碰撞 ~10⁻¹⁵ 量级) |
| 竞态/TOCTOU | 见 §7 |

唯一能朝"错误合并"方向失效的是哈希碰撞,已由双哈希压制。

## 7. 并发与竞态

- **编译串行**(F1):索引读写无锁;
- **TOCTOU(编译期间数组内容被改)**:指纹是对 pipeline **实际产出**的 IR 算的。
  撕裂读 ⇒ 指纹不再等于任何干净版本 ⇒ 走正常编译。与今天"撕裂编译照样发布"
  同类,是已接受的风险,**没有引入新窗口**;
- **activate/deactivate 竞态**:现有版本门(cp1/cp2、`cachePublish` 内重验)
  对 dedup 命中路径同样生效——`compileFn_` 返回后校验的语义不变;
- **peer 并发读 cache**:与普通发布完全相同(slot 状态机 + bucket 锁),
  别名不改变发布协议。

## 8. 开销

| 路径 | 增量 |
|---|---|
| dedup 未命中(每次编译) | 一次 StructuralHash + walker,O(模块大小),远小于 pipeline 本身 |
| dedup 命中 | 仍付 parse + pipeline;省 codegen + JITLink + seal + pool 分配。**收益是代码段体积与 icache,不是时延** |
| 常驻内存 | 索引 20KB(512 × 40B,§5.5) |
| 共享内存 ABI | 零 |

收益上限 = 被合并版本数 × 单版本代码体积;命中率取决于负载中"等值内容多实例"
与"常量不影响代码"的占比,**先 DryRun 实测再启用**(§9)。

## 9. 配置与 DryRun

`ejit_config_t` 末尾追加(additive,零默认 = 关):

```c
typedef enum {
  EJIT_DEDUP_OFF = 0,     // 默认,行为与今天完全一致
  EJIT_DEDUP_DRY_RUN = 1, // 算指纹、计数"本可合并",照常编译(测量用,零风险)
  EJIT_DEDUP_ON = 2,      // 启用合并
} ejit_dedup_mode_t;
```

- DryRun 是上板测量步骤:统计"本可合并次数 × 当次代码体积"即预估节省量,
  且因不改变行为,可与 ON 版本做 A/B 回归。**计数口径**:只在最终发布成功
  (过版本门)时计数--version-gate 失败被丢弃的编译不计,避免高估;
- 统计口径注意:dedup 命中同样走 `Published` -> `asyncCompiles` +1,
  现有计数看不出"省了一次编译";靠 dedup 专属 diag/计数区分;
- 诊断:`EJIT_DIAG("dedup merge func=%u -> fn=%p (fp=%016llx)")` +
  owner 侧计数;v1 不进 `ejit_taskpool_stats_t` C-ABI(加字段需过 abiVersion),
  后续有需要再走 ABI 升级。

## 10. 测试计划

**gtest(`llvm/unittests/ExecutionEngine/EJIT/`)**

1. 双等值 cell:编译 `f(cell0)`、`f(cell1)`(内容相同)→ 同一 fnPtr,
   dedup 计数 +1,code pool `usedBytes` 只涨一份;
2. **健全性回归**:§5.1 的 idx 分支函数,等值 cell → **必须不合并**
   (fnPtr 不同);
3. cell1 内容改写后重编译 → 不同 fnPtr(指纹随内容分化);
4. initializer walker 单测:GlobalOpt 提升用例 → 指纹分化;
5. 确定性回归:同输入两次编译指纹相同;
6. 驱逐/重编译幂等:驱逐 slot → 重编译 → 指纹命中 → 同 fnPtr;
7. `ejit_clear_cache` / generation bump → 索引清空(下一编译全新);
8. DryRun:计数生效、fnPtr 仍各不相同、行为与 OFF 一致;
9. releaser 硬门:dedup ON + `setReleaser` 接线 -> init 报错或 dedup 自动
   降级 OFF(不会出现共享指针被释放的路径)。

**集成(`ejit_test/`)**

9. N 个同配置 cell 的 workload → `ejit_get_code_pool_stats` 的
   usedBytes/finalizedRangeCount 增量 ≈ 一份;
10. 共享 taskpool 构建:peer 核 resolve 合并 slot → 4K seal 成功(走既有
    `icacheCrossCoreExecutable` 路径),执行结果与 OFF 构建逐位一致。

**lit**:不涉及(纯运行时特性)。

## 11. 实施步骤

| 步骤 | 内容 | 规模 |
|---|---|---|
| 1 | EJitOrcEngine:拆 `loadBitcodeModule` → `specializeAndResolve`(搬运 parse 前置段与 pipeline/dump/capture;`preSpecialized` 直通标志) | ~200 行 |
| 2 | 指纹:`StructuralHash(Detailed)` + initializer walker + FNV 二级 | ~80 行 |
| 3 | DedupIndex(定长开地址、generation/clear_cache 清理钩子) | ~80 行 |
| 4 | `compileCold` 接新 API;`ejit_config_t` dedupMode;diag/计数 | ~60 行 |
| 5 | §10 测试 | ~300 行 |

合计约 2–3 天(含测试)。

## 12. 非目标 / 未来工作

- **方案 A(机器码比对合并)**:B 启用后理论上无增量收益;仅当 DryRun/线上
  数据怀疑存在 codegen 不确定性时再评估,届时需 pool bump 回滚 API + seal 前
  决策 + debug 段剔除;
- **per-DedupEntry 体积统计与命中率上报进 C-ABI**:待 DryRun 数据证明价值后
  走 abiVersion 升级;
- **代码回收(releaser + 引用计数)**:见 §13--共享指针回收的三层复杂度
  拆解与回收路径排序;
- **跨 funcIdx 合并**:v1 明确不做(§5.4 反例:未用参数类型不同即碰撞)。
  开启前置条件:指纹补齐函数签名(参数/返回类型)、函数属性(byval/sret/
  align)、指令 flags(nsw/volatile/atomic/fastmath)与 metadata 哈希,
  并有跨函数合并的调用约定等价性论证;收益上限低(不同 entry 特化后 IR
  全同的概率小),优先级最低。

## 13. 未来工作:代码回收与共享指针的设计输入

v1 不释放任何代码(生产 releaser 为 null、pool append-only、sealed 页不
回收)。若未来引入回收,共享指针的复杂性是**三层叠加**,dedup 只新增第三层:

| 层 | 障碍 | 与 dedup 的关系 |
|---|---|---|
| ① pool 页回收 | bump 分配 + "sealed 页不回收"是 v1 硬约束;`deallocate()` 只跑 dealloc action,字节不还给池;回收 sealed 页需 unseal(RX->RW)+ 复用,是 W^X/TLB 议题 | 无关,今天就不存在回收 |
| ② 无痕派发 | icache 命中路径 plain `ldr + br`,不持 read token(EJIT_ICACHE_MULTIVERSION §6 明言:接 releaser 即 UAF);慢路径才有 token 可做静默检测 | 无关,先于 dedup 存在 |
| ③ 别名引用计数 | slot 驱逐 ≠ 可释放(其他 identity 仍指向同一 fnPtr);计数须覆盖 slot + icache cell + L0,还要等 ② 的在飞窗口排空 | **dedup 新增** |

共享指针的完整扇出(释放安全 = 下列持有者全部断开):

```
                           ┌─ dedup index 条目本身(owner 私有)
                           ├─ 共享 cache slot × N(每 identity 一个,可跨 bucket)
一个 canonical fnPtr ─别名→ ├─ icache cell × N(共享表,按 identity;drain 才清零)
                           ├─ per-core L0 memo × N(按 identity,epoch 失效)
                           └─ 某核在飞调用(寄存器中的 fnPtr,无 token 窗口)
```

易漏点:`cachePublish` 驱逐旧 slot / 同 identity 重编译覆盖时,同样会对旧
fnPtr 调 `releaseFn_`(不止版本竞态路径)。dedup ON 时这些全是共享指针,
由 §5.5 硬门一并挡掉。

若真要做回收,按代价排序的三条路径:

1. **代际批量重置(首选)**:只在 owner 代数更替 / 显式 reset 等静默点整代
   丢弃(清全部 slot + drain 全部 cell + 池换代)。无引用计数、无单条目
   释放,与"generation bump 清 dedup 索引"同构;
2. **合并条目永生,只回收独占条目**:refcount>1 永不释放,refcount==1 走
   正常回收。规则简单;合并条目每份内容本就只有一份代码,不回收也不会
   重新膨胀;
3. **完整引用计数 + 静默期**:计数覆盖 slot/cell/L0 + 在飞调用排空(需要给
   icache 命中路径补 quiescence 机制,等于要改 ②)。最完整也最贵,仅当
   单代内 churn 严重才值得。

## 14. 实现状态(v1,分支 ejit_spec_dedup)

| 组件 | 位置 | 状态 |
|---|---|---|
| 指纹 + 定长开地址索引 | `EJitDedupIndex.{h,cpp}`(新) | 完成:fp1=StructuralHash(Detailed),fp2=FNV(定义全局 identity+initializer+计数),irUnits;key=(funcIndex,fp1,fp2,irUnits),容量 512 |
| 引擎一次性编译入口 | `EJitOrcEngine::specializeAndResolve` + `ParsedSpecModule` 拆分(parse/emit 共享 legacy 路径)+ transform 层 `preSpecialized` 直通标志 | 完成 |
| 驱动接线 | `EJitCompileDriver::compileCold` 改调新 API;`effectiveDedupMode()` releaser 硬门(检出即降级 Off + 清索引) | 完成 |
| 配置 | `ejit_config_t.dedupMode`(additive,0/1/2)+ `Config::dedupMode` + `parseConfig` clamp | 完成 |
| 池侧 | `EJitTaskPoolCache/EJitTaskPool::hasReleaser()`、`EJitSharedTaskPool::hasReleaser()`(读共享 icacheReleasersWired 计数,peer-aware) | 完成 |
| 诊断 | merge/would-merge/insert-full 三类 EJIT_DIAG;引擎侧计数(dedupIndex().stats()) | 完成(v1 不进 C-ABI,见 §9) |
| 测试 | `EJitDedupTest.cpp`:指纹确定性/常量分化/initializer 盲点、索引 CRUD、等值 cell 合并+可执行性、idx 分支不合并、内容变化分化、DryRun 口径、同 identity 重编译再合并、Off 空索引、releaser 门两级 | 完成(12 用例全绿) |

**与设计的偏差/实现决定**

1. DryRun 命中后照常编译,插入时同指纹**刷新**已有条目(非新增),故 entries 计数按"不同指纹数"理解;
2. dedup 命中时该 cacheKey 不建 JITDylib,与 §5.3 一致;同 identity 重编译会先移除 stale JD 再命中索引,幂等返回旧指针;
3. `ParsedSpecModule` 成员声明序(Ctx 先、M 后)是正确性要求:反向销毁时 Module 析构要触碰存活的 context;
4. 既有缺口顺带修复(host gtest 链接,与本方案无关但阻塞验证):LLVMEJIT 补链 X86AsmParser(InitializeAllAsmParsers 无条件引用);新增 `EJitTestHostStubs.cpp` 提供 host x86-64 glibc 没有的全局 `__stack_chk_guard`(SRE libc 有);EJitDump 引擎级测试的手工模块缺 `ejit_entry` 标签被管线 internalize 后 lookup 失败,补标签修复;
5. 已知未做:§10-9 集成用例(ejit_test)、共享统计 C-ABI 字段、EJITTaskPoolTests 在 freestanding 配置下的链接(全局 `add_definitions(-DEJIT_FREESTANDING)` 既有问题,与本方案无关)。
