# EJIT 自校验特化（validated specialization，去 may_const 标注）设计说明

状态：**方向可行性分析，未实施**（v3，修订记录见 §9）。本文档回答一个
问题：如果业务能容忍一段时间内执行过期的特化版本，现有体系能否摆脱
`ejit_may_const` 逐字段标注，改为运行时观察驱动的冻结决策。结论：可行，
大部分基础设施已在库中；`ejit_may_const` 可整体删除，其两个角色
（发现、安全断言）分别由运行时观察和函数级容忍契约接管。

> 灵感来源：PR160 的 may_const substitution verifier
> （`EJitVerify.h` / `EJitVerify.cpp`，`EJIT_VERIFY_SUBSTITUTION`）。
> verify 模式证明了"保留 load、比对冻结值"的插桩形态可行；本方案
> 里它的 site 命名与 per-site 表结构被发现期直接复用（§5.1），其
> in-code 对拍能力保留独立诊断价值（§6.5）。稳态控制信号不升级
> verify 本身，改由冻结表地址读承担（§2.1）。

## 1. 目标

现状的冻结决策链条是 **AOT 静态标注**：`ejit_may_const`（字段级）+
registry（地址级）二者同时满足才可替换（见
`EJitStructFieldPass.cpp` 的 `gv-not-in-map` / `base-unresolved`
失败分类）。该设计有一个已知缺陷——`EJitVerify.h` 自己承认：

> The failure is silent — the memory stays correct, but the specialized
> code no longer reads it.

标注错了 = **静默永久错误**，运行时没有任何机制能发现。

本方案把冻结决策改为 **运行时观察**：编译一个检查版本记录 registry
范围内全局 load 的实际取值，测出稳定的字段冻成常量；稳态下全部调用
走特化版本，后台周期任务直接读冻结地址比对冻结值（不执行任何业务
代码）；发现值变了 → 失效特化版本 → 用新值重编译。标错的代价从
correctness bug 降为有界、可自愈的过期执行。

## 2. 方案生命周期

```
发现期          特化              稳态                    失效
────────       ─────            ──────────────          ───────────
跑检查版本  →  稳定字段冻结  →   特化版本服务调用；  →   校验任务读到分歧
（插桩、记   →   重编译特化   →   后台周期任务只读    →   → bump 该函数 epoch
 录源码 load     版本（PASS6     冻结地址比对冻结      →   → cache/icache 失效
 的实际取值）    编译时读内存     值，不执行任何           → 下次调用 miss →
                 冻值）          业务代码                 → 重编译读新值 → 稳态
```

### 2.1 关键拆分：发现要执行代码，校验不要

（本方案的核心取舍。）两个阶段回答的问题不同，压在同一机制上
（按调用分流采样，即已被否决的 1/N 分流方案）会有一系列缺点：

| | 发现 | 校验 |
|---|---|---|
| 要回答 | 哪些 site 稳定、推导地址对不对 | 已冻结的 site 还成立吗 |
| 机制 | 检查版本真实执行 load，记录取值 | 读 PASS6 推导地址上的现值比对 |
| 需要执行业务代码 | 是（副作用即真实副作用）| **否** |
| 覆盖 | 只有执行过的 site | 全部已冻结 site |
| 时机 | 发现期（一次性/低频）| 每 period tick（时间驱动）|

校验之所以可以不执行代码：PASS6 替换本身就是在 JIT 编译时从推导
地址读进程内存取冻值（`EJitVerify.h` 头注释的表述）——编译完成那
一刻，(site, 推导地址, 冻结值) 三元组就在手里。持久化成冻结表
（§5.3），一个周期任务读地址比对即可。

被否掉的 1/N 分流方案在四个轴上全输：

- **热路径代价**：1/N 计数器要么跨核 RMW 要么 per-core 分片——
  `EJitVpCollector.h` 开头整段设计约束就是热路径不许跨核写共享行；
- **调用边界**：检查点在调用之间，单次调用内部的过期永远轮不到
  （冻住的循环退出条件 = 特化版本内无界过期，§6.1）；
- **冷路径**：未执行的 site 不被校验（§6.3）；
- **速度**：每 N 次调用付一次非特化执行。

1/N 方案用"第 N 次真实调用兼任检查"规避后台核跑副作用代码的
顾虑——地址读校验根本不执行任何代码，该顾虑同样消失。1/N 唯一
多做的一点是稳态下持续再观察未冻结 site（降级 site 的再晋升）；
本方案接受降级 sticky（inline-cache 式），再发现留作可选开关
（§8 第 4 步）。

本质定性：**发现 = 执行采样；校验 = 时间采样的纯内存读**。仍是把
`EJIT_VALUE_PROFILE.md` 的 Tier-1（观察）/ Tier-2（特化）模式
推广到"全局变量 + 周期校验"，但校验不再依附调用。

**过期窗口与恢复窗口要分开**（§6.1 同）：检测延迟 ≤ 校验周期；
校验任务发现分歧即 bump epoch，bump 之后的调用 miss → 走 AOT
fallback（未替换的原始 body，读活内存，**值是对的，只是没特化**）。
所以：

- **过期窗口**（执行错值）≤ 校验周期——容忍契约的 T 界的是这个；
- **恢复窗口**（回到特化执行）= 校验周期 + 下次调用间隔 + 重编译
  耗时——性能 SLO 的事，不是契约的事。T 按和式定会保守一个量级
  （重编译可达百 ms 级）。可选优化：分歧后由校验任务直接经
  taskpool 主动触发重编译，消掉调用间隔项——缩短的也只是恢复
  窗口，过期窗口在 bump 那一刻已经结束。

**作用域**：校验机制真正服务的是 **`ejit_period_arr`**
site。标量 `ejit_period` 当前只有内置 `static` 窗（SPEC4 §2.2.1：
自定义标量时间窗是未来扩展；运行时对 "static" 恒返 active，
`EJitRuntimeState.cpp`），按契约启动后不变——正常情况下冻结了就是
对的，校验对 static 是纯防御（预期永不分歧）。发现期仍统一观察
（零成本副产品：错标 static 的字段被观察到不稳定 → 不冻结，从
静默错误降级为丢性能）。冻结表仍收录 static site（§5.3：错标
static 是契约违反，同样该被有界自愈地逮住，代价只是一批几乎
永不分歧的记录），**收录即接线**：static site 同样走 §5.5 的分歧
回写与 §6.2 的降级，无防抖豁免——否则错标 static 的冻后 churn 是
无界重编译循环（分歧 → 重编译 → 再冻 → 再分歧，降级永不触发）。
static 的降级阈值更严（**K=1**，§6.2）：数组分歧行为有合法故事
（"偶尔变"），static 分歧一次就证伪了"永不变"的前提，停手即对。
SPEC4 的自定义标量时间窗落地时，标量重新变为可变人群，同一套
校验直接套用（届时按可变人群重定 K），无需现在建。

## 3. 与现状的角色映射（本方案的关键发现）

现状中 `ejit_period_arr` / `ejit_period`（registry）与 `ejit_may_const`
是**正交**的两个条件，替换需要同时满足：GEP 链根到 registry 成员
（地址可解析）**且** load 带 `!ejit.may_const`（或命中 v1.7 字段偏移
表）。新方案里各角色重新归位：

| 角色 | 现状承担者 | 新方案承担者 |
|---|---|---|
| whitelist：哪些全局可观察/可冻结 | registry ∩ may_const | **仅 registry**（`ejit_period_arr` / `ejit_period`，机制不动）|
| 字段级"冻不冻" | may_const 逐字段标注 | 运行时稳定性阈值 |
| 安全契约（冻结错了能忍多久）| may_const 的隐含断言 | **函数级"容忍过期窗口"属性** |
| denylist：例外字段 | —（may_const 不标即排除）| 可选的字段级排除表（§3.2）|

### 3.1 whitelist = registry（现有机制，不新增标注）

registry 本来就承担"冻结范围的地址边界"：GEP 链追不到注册基址的
load 不进入替换候选。新方案把观察插桩范围直接定为"所有根可达
registry 成员的 load"，may_const 的发现角色完全由观察 + 阈值接管。
registry 附带的 dimension（`ejit_period_arr_ind` 的 index 进 cache
key）与 lifecycle（`ejit_period_lc` 的 activate/deactivate，正好是
失效钩子）继续按现状工作。

site→地址的解析也无需新增机制：PASS6 本来就从 registry 推导冻结
地址。影子表确实只需记录值、不需记录地址——而且这些值另有用途：
它们来自**源码计算的 load 地址**，是 publish 时对拍 PASS6 推导地址
的 ground truth（§5.2）。

### 3.2 denylist（可选，多数场景可省）

whitelist 是全局级粗粒度，denylist 是其内的字段级例外表，语义为
"在 registry 范围之内、测出来再稳定也不许冻"。存在理由：有些字段
**碰巧长期稳定，但语义上不能投机**——例如 emergency mode 标志可能
几个月不变、观察期内完美稳定，稳定性阈值对它完全失效，但它翻转时
过期执行的后果不可接受。这种只有人能判断。

标注结构对比：

| | 现状 may_const | 新方案 |
|---|---|---|
| 结构 | 字段级、必须写对的白名单 | 全局级粗白名单 + 字段级窄黑名单，都可选 |
| 默认 | 不写 = 什么都不冻 | 不写 = registry 内全观察 |
| 标错 | correctness bug（静默永久错误）| 丢性能（黑名单漏写除外，靠容忍契约兜底）|

关键差别：从"**逐字段点名才准入**"变成"**范围内默认准入、点名才
排除**"。点名的负担从"每个想冻的字段都要标"降到"只有知道不能冻
的字段才标"——大部分场景一条都不用写。若真实 workload 中不存在
"稳定但语义不可投机"的字段，denylist 也可以不做。

**定形：denylist 必须是 attribute，标在字段上**——判断
者是业务开发者（"只有人能判断"），字段的归属知识只在源码里：

```c
struct CellConfig {
    uint32_t cellIdx;
    ejit_no_freeze uint32_t emergency;  // 测出来再稳定也不许冻
};
```

- **为什么不是编译选项/外部配置表**：字段是嵌套结构体成员，命令
  行路径拼写（`-mllvm -ejit-deny=g_cellCfg.emergency`）丑且脆，
  重命名即静默失效（后果是丢性能，安静得没人会查）；attribute 标
  在成员声明上，路径由 C 语法本身表达，重命名编译器直接报错；
- **Subject = Field**，与 `EjitMayConst` 相同——天然覆盖嵌套
  结构体成员（标在内层 struct 成员上），且与现有 EJIT 属性族
  心智模型对称：同一位置、反向语义（may_const 点名准入，
  no_freeze 点名排除）；
- **传递机制**：Sema 校验（字段所在全局变量带 `ejit_period` /
  `ejit_period_arr`，否则 warn——标了不进观察范围的东西大概率是
  笔误）→ CodeGen 落 `!ejit.no_freeze` metadata → PASS6 决策时
  may_const 检查删除后，no_freeze 是**唯一剩下的字段级判据**，
  metadata 随 IR 走、GEP 解析后一次 `getMetadata` 查询完成排除，
  不需 PASS6 维护第二张数据结构。可与容忍契约属性（§5.7）联动
  warn（标了 no_freeze 但 entry 无容忍契约，通常是标错位置或漏
  契约，二者语义配对）；
- **实施优先级**：设计上现在定形（它影响 PASS6 决策接口的
  `!ejit.no_freeze` 槽位预留），实施仍留 §8 第 4 步——§6.1 的
  "控制流谓词可达的 site 默认不冻"已覆盖 emergency 类场景的大头，
  denylist 只剩残余；若第 4 步被触发，前端到 PASS6 的通道已存在，
  不回头改决策接口。

## 4. 已有基础设施（可行性的主要支撑）

| 需要的能力 | 已存在 |
|---|---|
| 检查版本核心：per-site 值比对调用 | `__ejit_verify_check(site, baked, actual)`，site 命名、per-site 表全套（PR160）|
| 稳定性观察：armed gate、per-core 分片、heavy-hitter、阈值 | `EJitVpCollector`（`EJIT_SRE_PGO_VALUE_PROFILE`），`PgoScalarSite` 的 min-samples / dominance 阈值可复用（`EJIT_SRE_VP_MIN_SAMPLES` / `EJIT_SRE_VP_MIN_CONF_PERCENT`，`EJitCompileDriver.cpp` 现成用法）|
| 检测→失效→重编译 | icache 清零 + switch controller instance version bump + taskpool `compile_or_get`，activate/deactivate 现在就这么走（粒度问题见 §5.4）|
| 周期调度 | period 机制 + SRE task（校验任务与重编译任务共用宿主）|
| site→地址解析 | PASS6 从 registry 推导冻结地址（现状机制）|
| 检查版本的宿主 | AOT fallback body（MissFn）常驻二进制；检查版本从原始 bitcode（O2 前）JIT 编译插桩版 |

检查版本**必须**从原始 bitcode 编译插桩，不能在最终 AOT body 上
插桩——O2 会重写/合并/提升 load，这正是当初两阶段 bitcode 抽取
（PASS1 保留 metadata 的 early pass）存在的原因。代价是每个 entry
多一份 JIT 代码体。

发现期的执行代价与首调用契约：插桩版不折叠（verify 同理，只验证
不提速），发现期内该 entry 以未特化速度运行，换来测量；检查版本
编译完成前，调用走 AOT fallback（MissFn）——沿用现有首调用语义，
无需新机制，但需作为契约写明。

## 5. 新增组件

相对 1/N 分流方案（已否决，§2.1）的结构变化：wrapper 分流删除，
换成冻结表（§5.3）与函数级 epoch（§5.4）。

### 5.1 影子表（发现）

key = **解析后 site 身份**（`<func>:<global>+<byteOffset>` 的 hash，
byteOffset 是含 index×stride 的解析后偏移，与 §5.2 PASS6 侧的推导
地址同一定义）→ 首见值 + 稳定计数 + 发现期分歧计数 + 冻后累计 +
窗口基线 + sticky 降级标志 + 刷新点 tally 快照（七字段；前三个喂
发现期冻结门槛，后四个喂稳态降级窗口，§6.2）。选
解析身份而非显式 (func, dims, site)：

- 数组 site（`ejit_period_arr` 根）的解析偏移天然含 dim 实例——
  `g_arr[cellIdx=3].field` 与 `cellIdx=5` 是不同 key，不需要显式
  dims 分量；
- 不进地址的维度不撕历史：f(cell, trp) 读 `g_cell[cell].field`，
  trp 不进 GEP——(func, dims, site) 会把同一块内存按 trp 值撕成
  N 份观察历史，解析身份自动合并，采样速度与表容量双收；
- 身份与指令序无关（GEP 链根 + 偏移），检查版本与 PASS6 两种编译
  共用同一命名（makeSiteName 现成）。

实现注记：检查版本里的解析偏移是**运行时**算的（实参代入后
`ptr - base`），PASS6 侧是编译期常量（param substitution 后 GEP
index 已折叠，`accumulateFullOffset` 全常量要求）；两者定义一致，
对齐即可。VP collector 的 key 是 (funcHash, kind, siteIdx)，复用其
key 结构思想可以（按解析身份重定义），**per-core 分片协议则只保护
发现期的高频近似采样**——观察散在所有调用核上、数据是 heavy-hitter
近似（丢样本只损 profile 质量），分片防的是热路径跨核缓存行乒乓。
分歧计数不是这类数据：它是**罕见事件的精确计数**（§7 前提下每
校验 tick 至多一次），照抄 collector 协议反而三伤——回写全挤进
校验核的一个分片（`EJitCoreId::current()` 在校验任务上恒返同核），
与该核的正常观察竞争 direct-mapped 槽位、把热路径样本挤掉；计数
落点取决于校验任务调到哪个核，语义错位、不可复现；且 collector 的
generation flip 是 owner worker 专属（single-owner），回写方自行
读/flip 分片与其快照节奏打架。落点见 §5.3/§5.5（决策计数记影子表
中心结构的 per-key 冻后累计，sticky 降级标志同在影子表）；仓库先例
即 `EJitVpStats`——热路径近似
数据走分片，冷路径精确计数走中心结构。放 shared section（多核
共享）。按 key 持久积累稳定/分歧历史：重编译后的冻结集合 = 该
entry 的稳定集减去已降级 site（§6.2）。容量注意 §6.4——候选集是
registry 根可达 load（数组 site 再乘 dim 实例数），verify 的
64-site 固定表与 VP collector 的 64 sites/core 大概率不够。

**分歧证据的两个时间窗**：

- **冻结前（发现期）**：检查版本执行时观察到该 site 取值变化——
  归**冻结门槛**消费（观察期内就 churn 的 site 不该成为候选，
  §6.2 第一道闸）；
- **冻结后（稳态）**：校验发现冻值 ≠ 当前内存值（§5.5 回写）——
  归**降级窗口**消费（"M 次检查内 K 次"的分母是校验 tick，不是
  采样数）。

两者时间上不相交（发现期在该 site 冻结时结束）、分母不同，分开
计数：冻结时记该 site 的发现期分歧计数为**基线**，降级看冻后
增量 ≥ K。这个分立对 static 的 K=1 是必需的——"分歧一次即降级"
指的是冻后第一次校验分歧，不是发现期历史。第二个窗口仍是 may_const
错标最典型的暴露形态：**发现期稳定、冻结之后才开始 churn** 的
site，其证据全部来自校验侧——§5.5 若不回写，降级永不触发，只剩
per-function 预算整函数退 AOT 一条路（static 连这条路都没有，见
§2.1），即无界重编译循环。

### 5.2 PASS6 决策来源切换与 publish 对拍

冻结候选从"`!ejit.may_const` metadata + v1.7 字段偏移表"改为"影子表
稳定项"。替换逻辑（GEP 链追踪、地址解析、值替换）不动。

**冻结原子性不变量**：影子表只决定候选资格，实际冻值一律来自编译
时刻的内存读（"替换逻辑不动"天然保证）。这使耦合字段（如 ptr+len）
从同一次编译的相邻读冻结，不会按各 site 的观察时刻撕裂。实现时
严禁改成"冻影子表里的观察值"。

**publish 对拍**：候选 site 冻结前，用影子表里**源码
load 的观察值**对拍 PASS6 从推导地址读出的值，不一致 → 该 site 本次
不冻结并计数。错基址/错偏移在第一次特化时即被拦截（§6.5），而不是
永远静默读错地址。site 命名沿用 verify 的
"<func>:<global>+<byteOffset>"，发现插桩与 PASS6 共用同一命名——
该身份由 GEP 链根 + 偏移决定，与指令序无关，两种编译可对应。

顺带红利：v1.7 字段偏移表 fallback（`EJitStructFieldPass.cpp`，为
"优化 pass 丢弃 per-load metadata"而存在）整类脆弱性随 may_const
删除而消失。

### 5.3 冻结表（校验）

每个 publish 的 cache entry 一份 (site, 推导地址, 冻结值, 宽度) 记录，
随 entry 的 release 回调同生共死。多 dim entry 按 dim 实例记录
（推导地址含 period index，不同实例地址不同）。校验 = 周期任务读
地址比对：零业务代码执行、零 wrapper 热路径开销、覆盖全部已冻结
site（§6.3）。

**分歧计数的落点**：**决策输入记在影子表中心结构的 per-key 冻后
分歧累计**上——校验任务按解析身份 key 原子递增（罕见事件、单次
RMW），降级看该累计（§6.2）。为什么不在冻结表记录里（per-entry
计数列随 entry 同生共死，看似顺手）：**被 entry 生命周期杀死**——
分歧必 bump epoch（§5.5 第 2 步），entry 在自身第一次分歧后即
失效、重编译 publish 新 entry，per-entry 计数结构性至多 1
（例外仅 republish 前的 stale 窗口），K≥2 的降级沦为时序竞赛：
冷函数（惰性重编译、旧 entry 躺着被反复计数）能收敛，热函数
（miss 即重编译、快速换 entry）每次计数都是 1、永不降级，退化为
整函数预算兜底——恰好"热函数 + churn site"是最烧预算的场景，
直接违背"降级是正常路径、预算是最后兜底"（§6.2）。per-key 累计
不受 entry 生死影响，且同一 key 可同时活在多个 entry 里（static
site 的解析偏移与 dim 无关，跨 dims entry 共享 key，§6.4），从
per-entry 列 seed 累计也不可行。冻结表记录可留一条**本 entry
计数列作诊断**（写者同样只有校验任务，写前 epoch/version 握手防
TOCTOU，版本变了放弃本次写），但**不进降级判定**。中心结构的
单写者论证：冻后分歧的递增者只有校验任务一个；影子表分片不借道
（§5.1：分片只保护发现期高频近似采样）；先例即 `EJitVpStats`——
冷路径精确计数走中心结构。

**收录范围**：数组根 site 是主角；static 标量 site 也收录——错标
static（契约违反、启动后被改写）与数组侧错标是同一个 bug 形态，
同样该被有界自愈地逮住，代价只是一批几乎永不分歧的记录（§2.1
作用域）。

校验读的对齐前提：按冻结宽度读推导地址，对齐且
≤ 指针宽度的读在主流架构上是原子的（x86/aarch64 单拷贝原子）；
PASS6 冻的是单条 load，宽度 ≤ 8B 由 `createConstantFromMemory`
的类型约束保证。但**"自然对齐"是假设不是保证**——packed struct
字段可以错对齐，且 PASS6 替换路径没有 align 检查（freeze 用
memcpy 不炸，炸的是校验侧的并发读）。撕裂的假分歧来源要分清：

- 并发写**本字段**：那是真分歧，撕裂值只用于 != 比较，无害；
- **跨字段叠写**：packed struct 里相邻字段的非对齐 store（aarch64
  非对齐访问非单拷贝原子）暂时搅浑本字段字节又写回原值，校验恰好
  落在窗口内 → 假分歧 → 白烧一次重编译，反复触发还会把无辜 site
  拖进 backoff 降级。

因此对齐要**在 freeze 时 enforce**，不是表结构里声明前提等 backoff
兜底：记录 site 时查 `LI->getAlign() ≥ store size`（或推导地址 %
宽度 == 0），不满足的 site 干脆不冻——packed 字段罕见，保守处理
零成本。

### 5.4 函数级 epoch（失效粒度）

SwitchController 的 version 是 per (dimType, instanceId)，cache 命中
按 dim 逐一复查该 version（`EJitTaskPool.cpp` lookup 的 version
recheck；cache 身份本身是 funcIndex + dims）。一个函数的一个字段
分歧若 bump 共享 version，**同 period 下所有函数的缓存全部 miss、
全部重编译**——跨函数放大，§6.2 的 per-function 预算拦不住。

现有 activate/deactivate 这样做是对的：period 值变了，共享它的所有
函数都真需要重编译。分歧失效不同：只有字段分歧的那个函数需要。
因此需加 per-funcIndex epoch（类似 dedup table 的按 funcIndex 直接
索引数组），cache 身份匹配后多复查这一项。icache 槽本就按函数注册
（`EJit.cpp` 的 gIcacheSlots），按函数清天然成立；需要补的是 cache
侧的按函数失效。

### 5.5 分歧→失效接线

校验任务发现分歧后做两件事，缺一不可：

1. **回写分歧计数**：按解析身份 key 递增影子表中心结构的 per-key
   冻后分歧累计（§5.3）——降级由该累计驱动（§6.2），不受 entry
   生死影响。这条线不能省：发现期稳定、冻后才开始 churn 的 site
   （恰恰是 may_const 错标最典型的暴露形态）若无回写则永远不降级，
   static 侧（无预算兜底）直接构成无界重编译循环；
2. **bump 该函数 epoch** → cache/icache 失效 → 下次调用 miss →
   重编译读新值（重编译时该 site 若已降级则不再冻结，§5.1）。

达到降级窗口（M 次检查内该 key 累计增量 ≥ K，窗口锚点见 §6.2）
时另做一步：按解析身份 key 在影子表置 **sticky 降级标志**——跨核
RMW 一次，降级是罕见事件，代价可忽略；此后该 site 永不冻（§6.2）。
累计与标志都在影子表的**中心结构**，不进 per-core 分片（§5.1：
分片只保护发现期高频近似采样，罕见精确计数不借道）。

复用现有 publish/version 机制，只是粒度从 per-instance 细化到
per-function；重编译预算（§6.2）在降级之上做最后兜底，不是第一道闸。

### 5.6 防抖策略

见 §6.2。

### 5.7 函数级容忍契约属性

见 §6.1。一个 clang attribute，声明该 entry 容忍 T 时间窗内的过期
执行。

## 6. 必须解决的问题

### 6.1 语义契约（最重要）

两次校验之间的窗口内，特化版本用的是错值，且**副作用不回滚**——
过期期间写出去的状态，检测到变化也不会修复。所以"容忍过期"必须是
函数级 opt-in，且声明的是"过期执行的**效果**可容忍"，不只是"读到
的值过期"。这是 may_const 安全角色的新去处，不能省：观察只能证明
"值曾经稳定"，不能证明"过期执行该值无害"。

契约的 T 界的是**过期窗口**（执行错值），不是恢复窗口（§2.1）：
检测延迟 ≤ 校验周期，bump 之后调用走 AOT fallback 值已正确，所以
T ≈ 校验周期即可，不包含重编译耗时——按和式定会保守一个量级。

契约必须写明两个固有盲区（不是实现缺陷，是采样式校验的属性）：

- **变了又变回**：两次校验之间值翻转又翻回，检测不到，窗口内的
  错误执行被永久放过。缩短校验周期只能缩小概率，不能消除。
- **单次调用内无界过期**：校验能在 T 内**发现**分歧，但正在执行中
  的调用无法被纠正（代码已在跑，任何方案都做不到）。若冻住的值
  控制循环退出/超时/终止，单次调用的过期窗口无界。这类值类（循环
  界、超时、退出/停机标志）默认不冻或必须进 denylist（§3.2）；
  "单次调用可长时间运行"的 entry 应被契约排除。

### 6.2 冻结策略与防抖

纯观察会把碰巧稳定的易变全局（计数器、定时器、序列号）冻进去，
必须三道闸：

- **冻结门槛**：连续 N 次采样同值 + dominance 类阈值（复用
  `PgoScalarSite` 机制与 `EJIT_SRE_VP_MIN_SAMPLES` /
  `EJIT_SRE_VP_MIN_CONF_PERCENT` 现成阈值）；发现期分歧在此消费
  （观察期内就 churn 的 site 不该成为候选，§5.1）；
- **降级 backoff**：某 site 冻结后 **M 次检查内增量 ≥ K** → 不再
  冻结该 site（inline-cache 式 demotion，sticky，§5.5 置标志）。
  分母是校验 tick，计数只含冻后增量——发现期分歧在冻结时记为
  基线、不进入此窗口（两个时间窗不相交、分母不同，§5.1）；增量
  记在影子表中心结构的 per-key 累计上（§5.3），不受 entry 生死
  影响。冻后 churn 是此闸的主要输入。
  **M 窗口的锚点与刷新规则**：per-key 累计是单调的（不清零），
  窗口 = 最近 M 个校验 tick 的增量。实现为 per-key 记**窗口基线**
  与**刷新点快照**两个原子字段（§5.1 第七字段即快照）：基线 =
  当前窗口起点的累计值，比较 `累计 − 基线 ≥ K`；每 M 个 tick
  检查一次刷新（不做逐 tick 滑动，校验任务单核）。刷新点比较
  **累计与快照**——

  - `累计 == 快照`（最近 M tick 无分歧）→ **重置基线** = 当前
    累计：干净期把老分歧老化出局，恢复速率语义（"偶尔变"的 site
    每次分歧隔了干净窗口，永不累计到 K）；
  - `累计 ≠ 快照` 且 `累计 − 基线` < K → **基线保留、窗口顺延
    M tick，快照更新为当前累计**：老分歧仍在窗口内，跨刷新点的
    K 不丢（K−1 次落在窗口 A、1 次落在 B 的 straddle 场景仍能
    到 K），重置资格留给下一个干净窗口；
  - `累计 − 基线` ≥ K → **已降级，随置 sticky**。

  判据设计要点：干净必须按"快照未变"（最近 M tick 无分歧）判定，
  不能按"自窗口起点以来零分歧"（`累计 − 基线 == 0`）——后者在
  顺延过的窗口恒不成立，会让任何分歧过的 key 基线永久钉死，速率
  判据退化成"首次分歧以来累计 K 次"（每年变一次的数组 site 在
  第 K 年被降级，M 名存实亡）。**K 与 M 按根类型分档**：数组根
  site 用通用档（分歧行为有合法故事——"偶尔变"是 cellindex 类
  配置的正常模式）；**static 根 site 取 K=1**（M 不需要分档，
  通用值即可）——分歧一次即证伪"启动后不变"的契约前提，K>1
  意味着在已坏的前提上多烧 K−1 次无谓重编译。K=1 语义上对（前提
  碎了就停手）、预算上省（立即停止烧预算）；代价是极罕见的"一次
  性晚初始化"场景（启动后某次首次赋值）永久丢该 site 的特化——
  可接受的保守，且该场景下 static 的契约本就要求赋值发生在使用前；
- **重编译预算**：每函数每 period 的重编译次数上限，超限退回纯
  AOT。这是降级之上的**最后兜底**，不是第一道闸：正常收敛路径是
  site 级降级（一 site 只烧 K 次重编译就停，static 更是 K=1 即停），
  预算只在多 site 齐 churn、降级来不及压住时整函数止损。static
  site 不豁免——static 标错的契约违反同样走降级。

预算模型成立的前提是 §5.4 的按函数失效：否则一次分歧 = 同 period
全体重编译，per-function 预算形同虚设。

### 6.3 冷路径覆盖

地址读校验覆盖**全部已冻结 site**，与该 site 是否执行无关——
"只有执行过的 site 会被校验"（verify 头注释同样承认"Coverage is
limited to sites that execute"）的盲区消失。剩余盲区在发现侧：从未
执行的 site 不会被观察、不会成为候选，其字段永远走 generic 路径。
这是性能盲区（保守方向），不是正确性盲区，需文档化。

### 6.4 插桩范围、代码体积与表容量

候选集从 registry∩may_const 扩到整个 registry——registered 全局上
所有根可达 load 都要插桩。检查版本的代码体积和编译开销需拿真实
模块量一下。

表容量是同类问题：verify 的固定 per-site 表上限 64
（`kVerifyMaxSites`），VP collector 每核 64 site
（`EJIT_SRE_VP_SITES_PER_CORE`）——对扩大后的候选集大概率不够。
容量估算按 §5.1 的解析身份 key：数组根 site 按 dim 实例数放大
（每实例一个 key），static 标量根 site 不放大（解析偏移与 dim
无关，跨 entry 共享）。影子表需按真实模块定容量与溢出策略
（截断、优先保留高频 site）。

另外 `EJitRegisterBitcode.cpp` 现有的"closure 内无 may_const 读取"
警告（防呆：防止 entry 白编译）随 may_const 删除而失去判据，但防呆
本身可改造而非删除：改为"closure 内无 registry 根可达读"（PASS1
已做 closure 全局引用分析，registry 成员名模块级可见）。

### 6.5 校验的地址推导盲区

冻结表只在 PASS6 推导出的地址上读：若推导本身错（错注册基址、错
字段偏移），它读错地址也永远与自己的冻结值一致，**永远不分歧**。
只有源码 load 的实际取值能证伪推导——这正是 §5.2 publish 对拍存在
的原因，也是 verify 模式保留独立诊断价值的原因：in-code 对拍
（真实 load 值 vs 推导地址冻值）是它独有的能力，冻结表不能替代。

## 7. 负载形状假设（成立前提）

```
值变化间隔  >>  校验周期  >>  重编译耗时
```

"值变化"要分两条路写准：

- **走窗口切换的变化**（`ejit_activate` / `deactivate` 的 cell
  切换）：已有共享 version bump → 重编译，粒度是对的，新方案不动
  这条路；
- **不走窗口切换的就地写入**（active cell 的字段在窗口内被直接
  改写：错标的 churn 字段、不经 deactivate/activate 的原地重配）：
  这是校验机制真正补的洞。若业务保证一切改写都伴随 activate，
  校验对数组就纯是防御性的；若存在原地写，校验是唯一 catch。

cellindex 类配置（人时间尺度变化：重配置、故障切换）满足；一个每
几秒翻转一次的全局会让方案退化成编译风暴（由 §6.2 防抖兜底，但该
函数基本告别特化）。**动手前先用 verify 模式跑真实负载，拿各字段
的翻转率数据验证假设**——verify 的 per-site 表（checks /
mismatches / lastFrozen / lastActual）给出的正是这个测量。

## 8. 推进顺序

1. **冻结表 + 地址读校验（保留 may_const）**：PASS6 在替换时顺手
   记录 (site, 推导地址, 冻结值, 宽度)——这些值它本来就在算，只加
   side-table 写入；周期任务（SRE task）读地址比对；分歧 → 走现有
   version bump 失效路径重编译（此阶段可接受共享 version 的过失效，
   粒度细化留给第 2 步）。不碰 verify 模式、不碰 wrapper、不碰前端。
   单这一步就把现状最危险的"静默永久错误"变成有界自愈。

   （备选方案曾考虑"把 verify 从诊断升级为控制信号"，已否决：
   verify 是默认 OFF 的诊断编译模式（缺标志时链接失败是其刻意
   设计），counters 明确 "never a control input"
   （`EJitVerify.h`）；要当控制信号，要么特化版本永久保留 load+
   比对（放弃替换收益），要么先做 wrapper 分流——即先做第 2 步的
   一半。冻结表才是真正的最小改动。）

2. **无标记发现**：检查版本插桩范围 = registry 根可达 load；冻结
   决策走影子表阈值 + publish 对拍；per-function epoch；
   `ejit_may_const` 及其前端支持删除。删除面如实计入：Attr.td /
   AttrDocs.td / DiagnosticSemaKinds / SemaDeclAttr / SemaEJIT /
   CGEJIT / CGExpr / CGValue，AOT 侧 EJitPassOptions /
   EJitRegisterBitcode 的警告与计数、EJitStructFieldPass 的
   may_const 收集与 v1.7 偏移表，加 clang lit 测试——是一次不小
   的清理。
3. **防抖与预算**：降级 backoff、重编译预算、冷路径盲区文档化。
4. （可选）字段级 denylist 与再发现开关，仅当真实 workload 存在
   "稳定但语义不可投机"的字段、或降级 site 需要再晋升。denylist
   按 §3.2 定形实施：`ejit_no_freeze` attribute（Field subject）→
   `!ejit.no_freeze` metadata → PASS6 排除查询。

第 1 步不依赖后续任何一步，可先行合入。

## 9. 修订记录

- **v1 → v2.11**（2026-09-05，十二轮评审演进，本版 v3 合并定稿）：
  本方案经多轮评审迭代而成，历次修订要点已并入正文，此处保留演进
  脉络，要点如下：

  - **发现/校验拆分**（v1→v2 的核心重构）：v1 用 wrapper 1/N 分流
    到检查版本做稳态校验，评审否决——热路径代价（跨核 RMW）、
    调用边界（单次调用内过期轮不到检查）、冷路径盲区、每 N 次调用
    一次非特化执行，四轴全输；v2 起拆分为"发现靠执行采样、校验靠
    冻结表地址读"（§2.1），校验零业务代码执行。
  - **影子表 key**：曾用 (func, dims, site)，评审指出应按**解析后
    site 身份**（GEP 链根 + 解析偏移，与 PASS6 推导地址同定义）：
    数组 site 偏移已含 dim 实例，不进地址的维度不撕观察历史，
    与检查版本/PASS6 共用同一命名（§5.1）。
  - **作用域收窄**：标量 `ejit_period` 当前仅内置 `static` 窗
    （SPEC4 §2.2.1），按契约启动后不变；校验/防抖/预算真正服务
    `ejit_period_arr` site（§2.1）。static site 仍进冻结表
    （收录即接线降级，K=1，§6.2）——错标 static 与数组侧错标是
    同一 bug 形态。
  - **窗口语义修正**：过期窗口（执行错值，≤ 校验周期）与恢复窗口
    （回到特化执行，另加调用间隔 + 重编译耗时）分拆——容忍契约
    的 T 界前者（§2.1 / §6.1）。
  - **分歧证据两窗分立**：发现期分歧喂冻结门槛（观察期内 churn
    不该成候选）；冻后分歧喂降级窗口（分母是校验 tick）。两窗
    不相交、分母不同，冻结时记基线、降级看冻后增量（§5.1 / §6.2）。
  - **计数落点定案**：决策输入记影子表中心结构的 per-key 冻后
    累计——per-entry 计数列被 entry 生命周期杀死（分歧必 bump
    epoch、entry 即被替换，计数结构性至多 1，热函数永不降级）；
    照抄 VP collector 分片协议亦否决（挤占校验核 direct-mapped
    槽位、计数落点随调度不可复现、与 owner worker 的 flip 节奏
    冲突）；先例 `EJitVpStats`（§5.1 / §5.3 / §5.5）。
  - **M 窗口锚点与刷新规则**：per-key 单调累计 + 窗口基线快照 +
    刷新点快照比较；干净按"快照未变"（最近 M tick 无分歧）判定，
    不能按"自窗口起点零分歧"（顺延窗口恒不成立，基线永久钉死、
    速率退化成累计判据）；顺延窗口须更新快照（§6.2）。
  - **对齐执行点**：freeze 时查 `LI->getAlign() ≥ store size`，
    不满足不冻；跨字段叠写（packed struct 非对齐 store）为假分歧
    来源（§5.3）。
  - **publish 对拍**：影子表源码 load 观察值对拍 PASS6 推导地址
    读值，拦截错基址/错偏移（冻结表自身拦不住推导错误，§5.2 /
    §6.5）。
  - **denylist 定形**：`ejit_no_freeze` attribute（Field subject，
    与 EjitMayConst 同位反向）→ `!ejit.no_freeze` metadata →
    PASS6 一次查询排除；设计定形、实施留第 4 步（§3.2 / §8）。
  - **冻结原子性不变量**：影子表只定候选资格，冻值一律来自编译
    时刻内存读——耦合字段不按观察时刻撕裂（§5.2）。
  - **per-function epoch**：SwitchController version 是 per
    (dimType, instanceId)，一字段分歧若 bump 共享 version 会
    flush 同 period 全体——需 per-funcIndex epoch（§5.4）。
  - **表结构**：影子表 per-key 七字段（首见值、稳定计数、发现期
    分歧计数、冻后累计、窗口基线、sticky 标志、刷新点快照），
    容量按"registry 根可达 load × dim 实例数（数组 site）"估
    （§5.1 / §6.4）。
  - **首调用契约**：检查版本编译完成前调用走 AOT fallback（MissFn），
    沿用现有语义（§4）。

  以上要点正文均已展开，正文与记录冲突时以正文为准。
