# EJIT Online-PGO Value Profiling（guarded specialization）设计说明

状态：设计说明 + 实现提交拆分（4 个提交）。默认关闭，`EJIT_SRE_PGO_VALUE_PROFILE`
构建宏开启。本文档是对 `EJIT_ONLINE_PGO.md` 的扩展：edge profile 部分不变，
新增 value profile 的采集、合成与消费链路。

> 注：任务基线引用的 `ejit_test/value_profile_audit/`（REPORT_ROUND7.md、
> ejit_vp_shard.h、vp_convert.cpp、icp_bench_*、memop_bench_*）在当前工作树与
> HEAD 历史中不存在（仓库为 partial clone + sparse checkout，无法联网补取）。
> 本实现基于 HEAD（f066337f，已含 Online-PGO/SRE hardening/staged profiling/
> 并发限制）与 LLVM 21.1 官方 InstrProf 机制重新设计并落地；audit 已证明的
> indirect-call promotion、dynamic memop specialization 由本实现以官方 API
> 重新接线，实验 audit benchmark 不进入生产库。

## 1. 目标

对三类 site 采集运行时值分布，Tier-2 生成 **带 guard 的优化热路径 + 语义一致的
通用 fallback**（EJIT 无 deoptimization，绝不无条件推测）：

1. indirect-call target（IPVK_IndirectCallTarget）
2. dynamic memop size（IPVK_MemOPSize，memcpy/memmove/memset/memcmp/bcmp）
3. scalar / loop-bound integer value（自研第 3 类，见 §7）

示例（loop bound）：运行时观测到 `a == 100` 占 99.99%：

```llvm
  %isHot = icmp eq i32 %a, 100
  br i1 %isHot, label %hot.preheader, label %cold.preheader
hot.preheader:                 ; 克隆出的热循环，%a 已替换为常量 100
  call void @llvm.assume(i1 true)   ; Bound == 100 已被 guard 保证
  br label %hot.header
cold.preheader:                ; 未改动的原循环（generic fallback）
  br label %cold.header
```

热路径中的常量 100 由后续 InstCombine / SCCP / LoopUnroll（以及 L3 的
LoopVectorize）直接消费；fallback 完整保留，语义等价。

## 2. 关键上游事实（LLVM 21.1，决定实现形态）

1. `PGOInstrumentationGen`（FDO）自动为 indirect call 与 memop 生成
   `llvm.instrprof.value.profile` intrinsic；`InstrProfilingLoweringPass` 将其
   降低为对运行时函数的调用：
   - `__llvm_profile_instrument_target(i64 value, i8* profd, i32 flatIndex)`
   - `__llvm_profile_instrument_memop (i64 value, i8* profd, i32 flatIndex)`
   `flatIndex` 是按 kind 摊平的：indirect-call 站点在前，memop 站点偏移
   `NumValueSites[IPVK_IndirectCallTarget]`。**当前 EJIT 未提供这两个符号**
   （Tier-1 代码中的调用无法解析）——本实现补上。
2. **ICP 的 profile value 是目标函数 IR 级 PGO 名字的 MD5 哈希，不是运行时
   地址**（`IndirectCallPromotion.cpp`:`Symtab->getFunction(Target)`，Symtab
   由 Tier-2 模块经 `ComputeHash(getIRPGOFuncName(F))` 建立）。上游在 raw
   profile 读取时用 `__llvm_profile_data[].FunctionPointer→NameRef` 做
   addr→hash 转换；EJIT 自产 indexed profile，必须在合并时自己完成该映射
   （§5.3）。未验证的裸地址绝不写入 profile value。
3. `PGOInstrumentationUse` 按 (FuncHash, Kind, SiteIndex) 从 profile 记录取值
   并写 `!prof` 元数据；`PGOIndirectCallPromotion`（module pass）与
   `PGOMemOPSizeOpt`（function pass）从该元数据消费。site 编号 = 同一
   ValueProfileCollector 在 Gen/Use 两次遍历的确定性顺序，因此 **Tier-1 与
   Tier-2 必须在同一 CFG 形态上编号**（lightOpt 前缀相同 + Gen/Use 各自做
   相同的 critical-edge splitting）。
4. 官方 value kind 只有 3 个（IC=0, MemOPSize=1, VTable=2）。scalar 值不能进
   官方 profile → 走独立的 side table（§7），edge/ICP/memop 仍在同一个合法
   profile 里（InstrProfWriter，满足“合并到同一 profile”要求）。
5. `__profd_` 布局（与 `EJitProfileMerge.cpp` 现有偏移同源，64 位）：
   FuncHash@8，NumCounters@48，`NumValueSites[3]`（uint16×3）@52/54/56。
   运行时用它与 `flatIndex` 还原 (kind, per-kind site)。

## 3. Tier-1 采集器：每核 K-way heavy-hitter shard

### 3.1 状态布局（零 malloc、固定容量、固定 ABI）

```
EJitVpSharedState                    // EJIT_SHARED_SECTION 放置（host=.bss）
  magic / abiVersion=2 / structSize  // 独立 ABI 校验，不动 taskpool 的 v10 ABI
  shardStride / sitesPerCore / K / maxCores
  armed                             // EJitAtomicU32，采集使能门（读多写少）
  EJitVpShard shards[maxCores]       // 每核一块，cache-line 对齐
EJitVpShard（每核）:
  EJitAtomicU64 generation          // bit0 = 当前活跃 payload 半区
  EJitAtomicU32 writers[2]          // 每半区已登记、尚未退出的 producer
  payload[2]                        // 双缓冲，每半区：
    sites[sitesPerCore]，每 site：
      EJitAtomicU64 siteKey         // mix(NameRef, kind, siteIdx)
      EJitAtomicU64 total           // 该半区窗口内总次数
      {EJitAtomicU64 value, EJitAtomicU64 count}[K]   // K 路候选
```

- **内存上限（可计算）**：`perCoreBytes = align64(64 + 2 × sitesPerCore ×
  8 × (2 + 2K))`；`totalBytes = maxCores × perCoreBytes`。
  默认 `K=2, sitesPerCore=64, maxCores=32` → 每核约 6272B，
  总量 ≈ 200 KB；全部宏可覆盖（`EJIT_SRE_VP_K / EJIT_SRE_VP_SITES_PER_CORE /
  EJIT_SRE_VP_MAX_CORES`）。**板端注意**：该 blob 计入 SRE RAM 预算（默认
  目标 ~200 KB 级），超出预算时先调小 `maxCores`/`sitesPerCore`（每核下限
  由站点数决定，见 §12 风险），或关闭宏（零占用）。
- 布局全部固定宽度标量、按值访问 → aarch64_be 安全；standard-layout /
  trivially-default-constructible static_assert（复用 taskpool 的约束风格）。
- 宏关闭 → 整个 blob 不编译，共享 ABI、footprint 零影响（taskpool ABI 仍为
  v10）。

### 3.2 热路径记录协议（无 CAS/锁、无 producer 跨核写共享行）

```
record(value, profd, flatIndex):
  if (!armed.loadAcquire()) return;            // 共享行只读，无 RMW
  core = EJitCoreId::current();
  if (core >= maxCores) return;
  shard = shards[core];                        // 之后全部访问核私有行
  gen = shard.generation.loadAcquire(); half = gen & 1;
  shard.writers[half].fetchAdd(1);              // 本核私有 shard
  if (shard.generation.loadAcquire() != gen) → 退出登记并重试
  site = &shard.payload[half].sites[mix(...) & (sitesPerCore-1)];
  siteKey = mix(NameRef@0, kind, perKindIdx)   // 由 NumValueSites 还原
  if (site.siteKey != siteKey) → 替换整槽（siteKey.storeRelaxed, 计数清零）
  else 查 K 路: 命中 → count.fetchAddRelaxed(1)
               空槽 → value.storeRelaxed(v); count.storeRelaxed(1)
               满且未命中 → 覆盖 count 最小的候选（自适应，偏向近期值）
  total.fetchAddRelaxed(1)
  shard.writers[half].fetchSub(1)
```

- 除 `armed`/`generation` acquire 读外，writer 登记和 value 计数均落在**本核
  私有 shard**（每核独占、对齐），无 CAS、无锁、无 producer 跨核写共享行。
  writer 登记使 collector 能证明 retired half 在复制期间不可变。
  直映表（site = hash & mask）碰撞时整槽替换，
  属有界近似（profile 数据本就是近似）；影响仅限 site 归属，不产生正确性
  问题（Tier-2 guard 永远在运行时校验）。
- siteKey 用 64-bit 混合（乘加/旋转），冲突概率可忽略；合并端无需求逆——
  它按 (NameRef, kind, siteIdx) 重算 key 探测（见 §5.2）。

### 3.3 快照协议（双缓冲 + generation release/acquire）

```
takeSnapshot(perCoreSites out):
  for each core c:
    等待 inactive half 的 writers == 0，清零后再 flip generation
    old = shards[c].generation.fetchAdd(1, acq_rel);   // 生产者切到另一半区
  bounded drain: yield EJIT_SRE_VP_DRAIN_TICKS 次      // 收缩 straggler 窗口
  for each core c:
    if writers[old & 1] != 0: 本轮跳过该 shard（下轮安全清理后复用）
    copy payload[old & 1] 槽                         // 整个半区已不可变
    clear payload[old & 1]                             // 清零退役半区
  armed 生命周期见下（首个 Tier-1 捕获置 1，最后一个 Tier-2 发布后置 0）。
```

- producer 先登记 writer、再复核完整 generation；flip 后才登记旧 generation 的
  producer 会退出而不接触 payload。collector 仅在 writer 归零后复制，因此
  `(value,count,total)` 不会跨更新拼接。
- 每轮快照覆盖 `[上一轮 flip, 本轮 flip)` 全窗口（两半区轮流承接不相交窗口）。
- `armed` 生命周期：首个 Tier-1 捕获时置 1（此时该函数尚不可调用，安全）；
  最后一个采集轮次的 Tier-2 **实际发布成功**后由编译驱动置 0。编译或发布
  失败不消费轮次，已发布的 Tier-1 继续采集供下一次重试。

## 4. 采集门控 / 准入（复用 staged admission，满足 §7/§15）

- 只对 `admitPgoFunction` 准入的函数采集：Tier-1 代码只有其函数被准入且发布
  后才会运行；`armed` 由 compile driver 在 Tier-1 编译成功后置 1、最后一个
  采集轮次被 Tier-2 合并后置 0（驱动内轮次计数）。未获准函数继续 AOT（现有
  语义不变）。
- 失败清理对齐现有 `tier1Counters_` 生命周期：
  - Tier-1 编译失败 / version 变化 / 发布失败 → 现有 `finishPgoFunction(abort)`
    路径，`armed` 复位，下一轮重采；
  - Tier-2 编译失败 → 保留 admission（现有语义），site 数据已被快照消费、
    随下一轮重采；已验证目标表（tier1Vp_）也保留，重试合并沿用同一
    addr→MD5 映射（下一次同 key 的 Tier-1 编译自然覆盖旧表）；
  - Tier-2 发布成功 → `finishPgoFunction(completed)`，计数器与 VP 状态清理；
  - instance toggle / owner 换代（generation 变）→ runCompile 三个 checkpoint
    丢弃编译，VP 侧靠“下次 Tier-1 编译重置该函数 site”兜底（无陈旧跨轮泄漏：
    新一轮 Tier-1 编译前驱动调用 `ejitVpResetFunction(hash)`）。

## 5. Tier-2 合并与 profile 合成（InstrProfWriter 官方 API，不手写二进制）

### 5.1 输入

- 现有 `PgoCounterRef[]`（edge counters，不变）；
- **Tier-1 编译时捕获的模块函数表**（optimizer 在 capture 阶段记录每个函数的
  `name + ComputeHash(getIRPGOFuncName(F))` + scalar site 数），驱动侧在 Tier-1
  编译后经 ORC lookup 得到各函数运行时地址 → 存入 `tier1ValueTargets_`
  （addr → MD5）；外部符号用 userSymbols 表补齐；无法解析的目标直接丢弃；
  - 注意：phase-1 会把非 entry 函数 internalize，而 ORC 的 IR 层不给
    local-linkage 符号建符号表项 → 分两步保证静态目标可 lookup：①
    Instrumented tier 在 addIRModule **之前**把所有已定义函数强制 External
    （让 ORC claim 符号）；② optimizer 的 capture（transform 之后）再次
    把它们改回 External（否则发射出的 object 里是 local 符号，已 claim 的
    符号会链成 null 绝对地址）。两步与 `__profc_*` 的 §5.2 claim 同款机制；
    捕获的 pgoHash 在②之前计算（= internalize 后的 IR-PGO 名字哈希，与
    Tier-2 的 ICP symtab 一致）；
- **快照**（§3.3 的 per-core site 数据）。

### 5.2 聚合

- 逐函数、逐 kind（0..2）、逐 siteIdx 重算 siteKey 并探测快照；累加各核
  (value,count,total)，产出 top-N（N=K，按 count 降序）+ total + 置信度
  `top1*100/total`。
- 合并端读 `__profd_` 的 NumValueSites 得到每函数 IC/memop 站点数（与运行时
  同一布局来源，同一 LLVM 构建 → 一致）。
- **函数身份与 CFG hash 分离（重要）**：全部 value siteKey 使用
  `__profd_.NameRef`，即 `ComputeHash(getIRPGOFuncName(F))`。`FuncHash` 仅作为
  InstrProf 的 CFG hash；结构相同、CFG hash 相同的不同函数不会共享 value 数据。

### 5.3 地址→profile value 映射（§2.2，防“裸地址冒充 hash”）

ICP 值 = `ComputeHash(getIRPGOFuncName(Target))`：
- 快照中的 IC value（运行时地址）查 `tier1ValueTargets_`；命中 → 写 MD5；
- 未命中（未知地址，例如寄存器伪造/外部未注册符号）→ **丢弃该值**，绝不
  原样写入。

### 5.4 写出

- `NamedInstrProfRecord(Name, Hash, Counts)` + `reserveSites(kind, n)` +
  `addValueData(kind, site, InstrProfValueData{...}, nullptr)` → 同一个
  `InstrProfWriter` 与 edge counters 一起 `writeBuffer()`；
- scalar 站点不进 profile → 聚合结果放 `SpecializationContext::scalarSites`
  side table，随 ctx 进入 Tier-2 transform（§7）。

## 6. Tier-2 管线（EJitOptimizer::runPipeline, PGOUse 分支）

```
lightOpt (不变, hash 对齐前缀)
PGOInstrumentationUse(profile)                 // 不变；标注 !prof + branch weight
EJitValueProfilePass(mode=Annotate)            // 新增：仅贴 !ejit.vp 元数据（见 §7）
ModuleInlinerWrapperPass                       // 不变（PGO 内联）
PGOIndirectCallPromotion                       // 新增（官方 ICP，消费 !prof）
EJitScalarValueSpec                            // 新增（§7，消费 side table+元数据）
runOptimizationPipeline                        // 不变；O2 看到常量→LoopUnroll 等
  (pgoUseFPM_ 含 PGOMemOPSizeOpt，位置不变)
```

- ICP/memop/edge 数据同在一个合法 profile（满足 §9）；
- Baseline / PGO 关闭 → 管线逐字节不变；VP 宏关闭 → 新增 pass 不编译不进管线。

## 7. Scalar / loop-bound 专用链路（第 3 类 site）

### 7.1 发现与插桩（EJitValueProfilePass，两模式同序编号）

- 候选：循环出口条件分支 `br i1 (icmp (IV 或循环变体值), (循环不变量整数
  Bound)), exit, latch`，Bound 非常量整数（i8..i64）。仅处理能安全识别
  “循环上界”形态的分支。
- **Tier-1（Instrument 模式）**：在 Gen+Lowering 之后运行（此时 CFG 已含
  critical-edge split），按确定性遍历顺序为每个候选分配 (funcHash, siteIdx)
  并插入 `call void @ejit_vp_record_scalar(i64 hash, i32 siteIdx, i64 value)`。
  插入的是直接调用、不新增 BB → 不影响 CFG hash / 站点计数。
- **Tier-2（Annotate 模式）**：在 Use 之后、Inline 之前运行（同一拆分后 CFG
  → 同一编号），仅给该分支贴 `!ejit.vp = !{i32 siteIdx, i64 funcHash}` 元数据
  （不变更 CFG/hash）；元数据随内联进入调用者体内，供变换 pass 定位。
- 每函数站点数在 Tier-1 编译时随捕获信息带回（驱动保存），合并端据此探测。

### 7.2 变换（EJitScalarValueSpec，独立小 pass）

对每个携带 `!ejit.vp` 的分支 site，查 side table：
- 门槛：`total >= EJIT_SRE_VP_MIN_SAMPLES`（默认 100）且
  `top1*100 >= EJIT_SRE_VP_MIN_CONF_PERCENT * total`（默认 99%）；
- 安全限制：只处理 **LoopSimplify 形态、单一出口块、单一 latch** 的循环；
  其余（多出口、多 latch、非整数、Bound 在循环内定义等）跳过并在 DEBUG 报告
  原因（不伪造收益）；
- 变换（每个循环至多一处，按确定性顺序取第一个合格 site）：
  1. 在 preheader 造 guard：`icmp eq Bound, V` → 热/冷两条路径；
  2. 克隆整个循环体（CloneBasicBlock + 值映射），冷克隆保持原样（fallback）；
  3. 热克隆（原循环）内把 Bound 的全部使用替换为常量 V，并在热 preheader
     插 `llvm.assume`；
  4. 修复出口块 PHI（冷克隆边）、补 DT 更新（重算），返回
     PreservedAnalyses::none()；
- 语义：guard 保证热路径只在 `Bound==V` 时进入；热克隆是原循环在 Bound≡V 下
  的精确重命名 → 与 fallback 语义一致；0/负值/边界值/溢出情形由 guard 天然
  覆盖（V=0 → 零次循环，热路径照常折叠；V=INT_MAX → 与原文一致）。
- 之后的 O2 管线看到常量 V：LoopUnroll/InstCombine/SCCP 折叠。**若合法地
  不展开/不向量化（如 O2 无 vectorizer、代价模型拒绝），板端用例只报告
  guard/常量传播证据与原因，不伪造收益**（§10）。

## 8. 平台性（§14）

- `EJIT_SRE_PGO_VALUE_PROFILE` 默认 OFF：CMake option → 仅对 LLVMEJIT 加
  `-DEJIT_SRE_PGO_VALUE_PROFILE`（+子宏），其余构建逐字节不变；
- aarch64_be：全部固定宽度标量按值访问；`NumValueSites`/FuncHash 用与现有
  profile 合成相同的同构建布局假设（static_assert 64 位）；
- freestanding / 无 libc++ runtime allocation：采集器零 malloc（固定 blob）；
  合并端用驱动侧 `std::vector`（编译期工作线程内存，与现有合成一致）；
- W^X / peer enable_rw：Tier-1 的 `__profc_` 可写页与新增 VP 无关；VP blob
  在共享段（RW data），不经 code pool；ICP 产物仍是纯代码，无新增可写页；
- 固定共享内存 ABI：VP 使用独立 blob + 独立 magic/version/size 校验，
  不 bump taskpool ABI（v10 不变）；宏关闭时共享内存布局零变化；
- 并发限制：快照/合并全部发生在 owner worker（单一编译者）内，与现有
  `EJIT_SRE_PGO_MAX_CONCURRENT_PROFILES` 无冲突。

## 9. 日志（§16）

- INFO：函数级进度与最终摘要（每函数一行：admitted、`VP merge ... ics/memops/
  scalars/dropped`、Tier-2 完成/失败；板端可经 `ejit_vp_get_stats` 断言）；
- site 级细节（每 site top-N/置信度/丢弃原因、scalar spec 决策）一律 DEBUG
  （`LLVM_DEBUG`，release 构建编译期移除），INFO 不存在逐 site 输出，天然免
  限流；transform 的跳过原因同样只进 DEBUG。

## 10. 测试与板端验证

- 单元测试（gtest，`EJitValueProfileTests`）：
  - shard：K 路命中/替换/直映碰撞、armed 门、siteKey 稳定性、跨核隔离
    （EJitCoreId::setCurrentForTest 模拟多核）；
  - 快照协议：generation 切换、writer 握手、不可变 retired half、双缓冲窗口；
  - 合并：IC 地址→MD5 映射（未知地址丢弃）、flatIndex 拆分、top-N/置信度、
    同 CFG hash 的不同函数隔离、与 edge counters 同 profile（读回校验）；
  - scalar pass：guard+克隆+常量替换+assume、fallback 保留、门槛拒绝、
    多出口跳过；语义等价抽查（0/负/边界值、随机对照）。
- 板端 demo（`ejit_test/ejit_vp_multicore_test.c`，仿 `ejit_pgo_sre_multicore_test.c`）：
  - worker 固定核心 8；producer 核 18/19/20 并发；
  - `EJIT_SRE_PGO_MAX_CONCURRENT_PROFILES` 控制并发 profiling 数，超出者走
    AOT（校验其结果与其他一致）；
  - 热函数：间接调用（经函数指针，静态目标函数）+ 动态尺寸 memcpy（多为
    63B，偶为 16B）+ `for (i=0;i<a;++i)` 循环，`a` 在 127/128（≈99.2%）调用
    中为 100、其余为 7（越过 99% 支配阈值，故意留少量其他值）；
  - 校验：所有结果与 AOT 参考一致；Tier-2 发布后经 `ejit_dump_func`/verify
    观察到 guard 分支与常量 100；统计 `tier2Compiles >= 预期`、`compileFailed==0`；
  - 汇报常量传播/展开证据；未展开/未向量化时打印原因（如 L2 无 vectorizer），
    不伪造收益。

### 板端验证命令（本地无法执行 SRE 行为的兜底）

```sh
# 主机侧（能跑的部分）
# 默认 maxCores=32，覆盖 demo 的 producer core 18/19/20；自定义时不得小于 21。
../build.sh release x86 \
  --sre-pgo-value-profile --sre-pgo-max-concurrent-profiles=2 --stats
cd ejit_test && EJIT_CLANG=../build_release_x86/bin/clang ./build.sh --run ejit_vp_multicore_test

# 板端（SRE，逐核会话；核 8 = worker）
# 板复位后严格按顺序执行。核 8 首次调用只启动 worker 并返回；看到
# "worker ready" 后，再启动三个 producer。18/19/20 会在共享 gate 等齐，
# 所以三条 producer 命令应分别在三个核会话中执行，不能串行等待前一条返回。
core[8]  -> test_ejit_period
core[18] -> test_ejit_period
core[19] -> test_ejit_period
core[20] -> test_ejit_period

# 三个 producer 最终都应打印：
#   [VP-MC][core=..] PASS lane=.. compiles=6 ...
# 并且 vp stats 至少满足：merges>=3、ics>=1、memops>=1、scalars>=1、
# specialized>=1、dropped=0。不要调用 ejit_shutdown，worker/taskpool 需要
# 跨逐核 shell 调用保持存活。

# producer 全部 PASS 后，在核 8 查看捕获的 Tier-2 IR：
ejit_print_dumped "vp_mc_func0"
# 重点确认 loop-bound guard（bound == 100）、冷 fallback、ICP direct-call
# guard，以及 63/16 字节 memop 分布对应的优化结果。

# 预期日志（INFO）：
#   [EJIT] PGO profile start func=..: 0/64 (active=1/..)
#   [EJIT] PGO Tier-1 published func=..: collecting 0/64 hits
#   [EJIT] VP snapshot func=..: sites=N ics=.. memops=.. scalars=..
#          promoted=.. dropped=0 top1=100 conf=99% total=..
#   [EJIT] PGO Tier-2 published ...
#   [EJIT] PGO profile complete func=..: completed=1 deferred=0
#   [VP-MC][core=18] PASS lane=0 compiles=.. sink=0x..
```

## 11. 提交拆分

1. **Runtime value collector and snapshot protocol** — `EJitVpCollector.{h,cpp}`、
   `EJitAtomic::fetchAddRelaxed`、LLVMEJIT CMake 接线、单测。
2. **Profile synthesis and Tier-2 pipeline integration** — `EJitProfileMerge`
   value data + addr→MD5、optimizer 捕获函数表、driver 注册 runtime 符号与
   合并调用、`PGOIndirectCallPromotion` 接入、`SpecializationContext` side table。
3. **Guarded scalar/loop-bound specialization** — `EJitValueProfilePass`
   （发现/插桩/标注）+ `EJitScalarValueSpec`（guard+克隆+assume+fallback）、
   `ejit_vp_record_scalar`。
4. **Tests, board demo and documentation** — gtest 扩展、板端用例、构建脚本、
   本文档与 EJIT_ONLINE_PGO.md 引用更新。

## 12. 风险与边界（诚实声明）

- 快照使用每半区 writer 计数 + generation 复检；drain 后仍繁忙的旧半区会在
  下一轮先回收再复用，不会因跨越 drain 窗口而整窗丢失。heavy-hitter 本身仍是
  有界近似，不承诺精确逐条计数，只承诺 top-N/置信度稳定。
- 直映表 site 碰撞整槽替换 → 极端碰撞下 top-N 可能换出；容量参数可调，
  默认 64 sites/core 对 demo/生产规模（每函数 ≤ 数十站点）充足。
- memop 特化沿用仓库现有 pass 位置（O2 之后）；常量尺寸展开由后端完成，
  不在本任务伪造额外 IR 级收益。
