# EJIT 增量需求：复合 period 与实例折叠 —— 标记方案设计讨论稿

> 状态：**讨论稿 v0.4.17**（2026-08-25）。§1/§2 为已确认的需求与决策；§3/§4 为语法与校验定义（本阶段聚焦对象）；§6/§7 为影响面预览与后续命题。v0.3 新增 **L3 特化折叠（D9）**：dim 级声明 + LCM 推导 + 折叠表达式替换。v0.3.1：**O-1 已决**（spec = 值域大小；缓存规格按折叠值域，见 §6），O-8/O-9 关闭。v0.4 新增 **D10 数组形态与维度规则、D11 成员 period**：数组 = 实例空间容器、pattern 判定规则（§3.7/§3.8）、C18-C21。v0.4.1 修正 §3.7 pattern 判定（嵌值安全模型）：剔除"常量"成分、嵌值资格 ⟺ 下标精确等于复核实例元组、现状 `g[cell+1]` 类嵌值无复核跟踪为洞；新增 O-11。**v0.4.2 修正判定时机（值级判定）**：嵌值资格不依赖下标"来源"、依赖"值"——替换后格子坐标与复核实例坐标都是常量，关卡直接比较 `L == cellIdx`（v0.4.1"判定必须在替换前"推翻）；clang/PASS1/偏移兜底零改动，关卡新增检查。自包含推演见 [EJIT_PATTERN_CONSTANT_MODEL.md](EJIT_PATTERN_CONSTANT_MODEL.md)。**v0.4.3 生命周期管理方案定型（D12/D13）**：只有 period 才有版本号和生效时间窗；运行时方案 3（wrapper 全量构建 specs + periods，结构体指针传入 `ejit_taskpool_compile_or_get`，接口内零算术）；命名体系重命名；D13 规格上限矩阵（含实例空间 ≤ 2^16、cellIdx u16、快照 u64）。现状核实见 §6.1，方案细目见 §6.2。**v0.4.4 lc 形态确定 + 标记传染立项**：`ejit_period_lc` 升级为变参、**显式指定 period 列表**（不推断，部分覆盖不匹配）；cellIdx 维度偏移计算（stride = Π min(spec_j, fold_j)，与 D10 同坐标系）；缺组成 dim 参数 → error；新命题 **P2 period 数据写者纪律**（`ejit_period_writer` 属性，写检测 + 调用纪律 + 指针污点传染，未来实现，§3.3 已记录属性签名）。**v0.4.5 收尾：O-5 已决**（跨 TU 一致性基于编译期检查，运行时冲突检测不需要）+ **实现待办清单**（§7.2，3 类 21 项）。**v0.4.6 分层回退定案（D14/§6.3）**：type（half-static/dynamic）**标在 dim 上**（粒度匹配：特化状态 per-dim，IR 层面无 period 概念）；wrapper **两级降级链**（轮1 全 specs+periods → 拿不到指针轮2 去 dynamic dim 再调 → AOT 兜底，失败原因不区分）；**dim 处理三场景**（① 无 spec-fold+指定优化：参数替换；② 有 spec-fold+指定优化：参数不替换、折叠表达式替换为 `spec % N`；③ 未指定优化 dynamic：全不替换查表）——替换集合 per 请求，**折叠表达式替换发生在 JIT 编译期**（AOT 阶段 dim 非常量，AOT 只收集折叠表达式位置）。**v0.4.7 D13 补 spec 实例空间约束**：spec 实例空间 `Π min(spec, LCM) ≤ 2^16`（icache 直接索引数组容量 ≤ 512KB/函数，与 period 实例空间双约束；LCM ≥ fold ⟹ spec 空间 ≥ period 空间，需分别满足）；icache 每维尺寸不再固定 16，按函数声明维度对齐 2 的幂分配（wrapper 移位常数 AOT 内嵌，快路径零调用不变）。**v0.4.8 指针生命周期与双位发布协议（§6.4）**：解决 fnPtr 替换/销毁与正在执行的调用之间的 TOCTOU（现状 releaser 覆盖即释放有洞）；slot 固定双位 `ptr[2]` + u32 标签（主位指示，切换 = 翻转标签不移动指针）；**先写指针、后切标签**（release/acquire）；最小原子（relaxed/acquire/release ≈ 普通指令，零锁零 RMW）；tick 懒释放（无计时系统：副位 tick 戳 + 编译前差检查，阈值 1s 可放大）；副满延迟编译 = 天然编译节流（防 dynamic 编译风暴）；与版本失效正交（指针失效=双位协议，版本失效=慢路径复核）。**v0.4.9 应用规格与坐标系定案（§6.2/§3.7/§3.4bis/§6.3）**：确立**两个应用规格**——impl_spec_p = min(dim_spec, period_fold)（生命周期侧：active 检查/版本复核/slot 入库校验/编译触发）与 impl_spec_d = min(dim_spec, LCM)（版本特化侧：specId/缓存 key/slot 位置）；**数学性质** `dim_v % impl_spec_d % impl_spec_p === dim_v % impl_spec_p`（两情形论证：dim_spec ≥ LCM 时 LCM 为 fold 整数倍；dim_spec < LCM 时值域内平凡成立）——目的 = 向 `ejit_compile_or_get` 只传 specId 不丢信息，生命周期分量可由 `specId % impl_spec_p` 推出（生命周期一律 % impl_spec_p，版本特化一律 % impl_spec_d）；**pattern 形态规则**（无 fold：`g[dim]` 或 `g[dim % XX]`（XX ≥ spec，值域内恒等）均可；有 fold：必须 `%` 模数 == impl_spec_p；形态不匹配 → 不替换 → 运行期计算）；**cellIdx 坐标系统一 C 行优先（套 B）**：cellIdx = Σ((dim_v % impl_spec_p) × Π_{j>i} D_j)，与 D10 数组线性下标、嵌值关卡 L 同一坐标系，旧式（左维最次）废弃；D13⑤ 约束修正为**分配后**尺寸 Π 2^⌈log2 impl_spec_d⌉ ≤ 2^16（对齐放大 ≤ 2^d，(129,129,3) 折叠后 49923 通过但分配 2MB 超限反例）。**v0.4.10 双位协议三态 tag + 版本失效合流（§6.4，A3/A4 定案）**：tag 从二态升**三态 {0, 1, INVALID}**——deactivate(period, cellIdx) = 版本表失效 + 遍历 slot 匹配置 `tag = INVALID`（两个槽位均成副位，主路径视为空指针），快路径读 INVALID → 落慢路径复核 → 重编译发布新 fnPtr → 置主位自动恢复，activate 无需发布；竞态无害（deactivate→数据写入窗口内旧代码配旧数据一致）。**A4 释放时点与编译触发分离**：编译触发条件 = 副位有指针且 tick 差 ≤ 门限则不编译（节流），超门限才编译；释放时点 = **写入时**遍历释放超门限副位 → 空位写新指针 → 置主位；无空位丢弃编译结果（正确性防护，实际不发生——触发编译的前提即副位超门限，写者互斥下状态不变）。前提显式化：写者互斥（同槽写入仅编译线程串行）+ 在途调用时长上界 < tick 门限（WCET 声明，1s 起步可调 5s）。新增待办 3.10-3.12。**v0.4.11 内部矛盾清理（第三组）**：①**LCM 推导排除恒等 fold**——operand 缺省 = spec 的 fold 值域内恒等（dim_v < spec ⟹ dim_v % spec = dim_v），**不计入 LCM**（计入会无谓抬高，如 lcm(32,10)=160 vs 只需 10），"非 identity"判定 = operand < spec（§3.4bis）；②**D13 补两硬约束**（早暴露原则：规格超限一律编译期 error，不拖到链接/部署期）——⑦ 全局 period ≤ 256（periodId u8 位域）、⑧ 全局版本表 Σ(period 实例空间) ≤ 2^16（裸数组按声明实例空间分配，项 ≈ 8B ⟹ ≤ 512KB；病态最坏账 128MB 封顶）；③**C22 fold operand ≤ 0 → error**（dim % 0 除零 UB，operand = 1 恒等允许）；④表述限定：编译请求"结构零改动"→"结构布局不变（字段仅 D12 更名）"（B2）、§5"使用处元数据完全不变"→"arr/ind 照旧，lc 变参为例外"（B3）、§7.1 D8 行同步 P1 方向定案（B4）；⑤§6.1 补 publish 提交门现状核实（发布权竞争串行化，与双位协议读写同步正交）（D5）；⑥零散：待办 21→24 项、C16 术语 specId、O-7 与 D13③ 同步（D6）。**v0.4.12 代码现状出入修正（第四组 C1-C5，全部核实代码后落）**：①**wrapper urem 为 L3 新增**——现状 emitInstanceVal 仅 Trunc/ZExt 无 urem（EJitWrapperGen.cpp:699-705），"现状已有"表述修正（C1）；②**cacheKey 打包格式核实**——现状 = XOR 散列 `key ^= (dimType<<32) | instanceId; key *= kHashMul`（非位域打包），打包逻辑保留、instanceId → 折叠 specId（C2）；③**C11 修正**——Sema 层无"函数 dim 数 ≤ 4"诊断（SemaEJIT.cpp:207 注释推迟到后端/运行时）与"整型参数"检查，两项为后端/运行时兜底；"数组大小 vs 实例空间一致性"为 D10 新增（C18）非既有（C3）；④**§6.4 归因收窄**——bucket 层无洞（读令牌排空 :1317、排空后锁外释放 :1398-1403）；洞在 **icache 快路径无读令牌**；现状安全靠"生产不装 releaser（fnPtr 永不物理释放 :48-49）+ safety gate 自动禁用缓存（:229-230）"——双位协议价值 = 启用代码回收时对快路径提供无锁读写同步（C4）；⑤**P2 动机补现状警告**——warn_ejit_may_const_modified_without_lc（SemaEJIT.cpp:546-556）已存在，P2 = warning → 类型系统污点的强化（C5）。**v0.4.13 待办补项收尾（第五组）**：三场景表"对应轮次"列显式标注轮1 的 dynamic dim 走场景 1/2 特化嵌值（§6.3）；待办清单新增 2.9（icache 按声明维度定制，AOT 内嵌移位常数）、3.13（tick 来源 = 系统单调计数，SysTick/cycle counter）、3.14（CMake 注释修正，`EJIT_SRE_SHARED_TASKPOOL` 默认 ON 注释过时，代码侧另改）；清单 24 → 27 项。至此五组（坐标/数学、双位协议、内部矛盾、代码出入、待办补项）全部收敛。**v0.4.14 快路径间接引用定案（§6.4）**：icache 格子**不拷贝 fnPtr**，存 **bucket slot 地址**（发布一次写入、永不变——bucket 为固定数组，slot 地址稳定）——双位单元（ptr[2] + tag）收敛到 bucket slot，**快慢路径共享同一发布结构**；**deactivate = 版本表失效 + 刷 slot（tag=INVALID）+ L0 epoch，无需遍历格子**（格子全部指向该 slot，一处失效全链生效）。新洞与保护：bucket slot 可能被**不同 identity** 覆写（cachePublish 桶满驱逐 :1337-1353 / re-init 原地重建 blob :1568 后 Empty 槽复用）→ 格子指向错位 → 快路径执行**错误代码**（非 stale）——**referenced 保护**（被快路径引用的 slot 不驱逐/不复用）；**当前实现阶段：桶满 → 返回 Failed 不缓存（驱逐分支保留不删，后续启用）**；re-init 清掉 referenced 标记 → re-init 时遍历本核格子清空（gIcacheSlots 核内全扫，re-init 罕见可接受）。开销账修订：快路径 = 格子 load（私有行）+ tag acquire + INVALID 比较 + ptr relaxed（共享行，稳态一致）——比 v0.4.10 多一次共享行访问，换取 deactivate **O(1) 失效** + 快慢路径单点发布。待办 3.9/3.10/3.11 更新 + 新增 3.15（referenced 保护 + 满则 Failed），C 类 14 → 15 项、清单 27 → 28 项。**v0.4.15 O-2 已决**：**不允许隐式 1:1 声明**——旧代码迁移必须显式声明（`DEFINE_PERIOD(cell)` 空列表 sugar 即为显式路径），不引入"同名隐式声明"，保持 D6"恰好一次"的单一规则（§3.6 开放项关闭；§7.1 O-2 行、§7.2 状态同步）。**v0.4.16 O-3 已决（§3.4 第 5 条/C15 扩展）**：period fold 算子**仅 MOD**——DIV/SHR/AND 与 dim 间线性组合**不引入**（§3.3 枚举保留为预留，使用即 error）。结构性原因：非 MOD 算子**不可逆**（取商/移位/按位与均丢信息），从 specId 无法恢复折叠分量，破坏 §6.2"只传 specId 不丢信息"推论与 D12 接口设计（wrapper 需额外传值）；组合折叠进一步打破 cellIdx 行优先坐标系与 D13 实例空间 Π 约束。与 O-8/C15（L3 仅 MOD）两层一致。业务量化需求（时间分桶等）走轮2 查表路径（运行期计算，不参与特化）。**v0.4.17 O-6 已决（§3.3/§3.5）**：6 个新属性命名**定案保持现状**（`ejit_dim_decl`/`ejit_dim_type`/`ejit_dim_spec`/`ejit_period_decl`/`ejit_period_fold`/`ejit_dim_spec_fold`，P2 的 `ejit_period_writer` 同）；**声明归组原则**——示例中 fold 声明随所属 period 归组（语法不变，`DEFINE_FOLD` 宏保留，§3.5）。
> 前置阅读：[EJIT_IMPL_OVERVIEW.md](EJIT_IMPL_OVERVIEW.md)（现状实现整理）。
> 代码基线：branch `ejit_dev_spec4` @ `52040abd0c75`。

## 1. 背景与诉求

### 1.1 现状的三个 1:1 假设

| # | 假设 | 现状锚点 |
|---|---|---|
| H1 | dim 名即 period 名，一 dim 一 period | `ejit_dim("cell")` 与 `ejit_period_arr("cell")` 同名配对；`EJitLifecycleRegistry` 每名字一槽 |
| H2 | 实例身份 = (dimType, 原始实参值) | `enabled_[8][256]` / `version_[8][256]`、cache 版本复核、`ejit_activate/deactivate(name, cellIdx)` 同粒度 |
| H3 | 特殊化 key = 失效 key = 原始 dim 元组 | JIT 参数常量替换直接拿实参当 cellIdx；icache `[16]^numDims` 按原始实参索引 |

### 1.2 新诉求

**需求 R1 —— 多 dim 组合成 period（dim-period 关系 1:1 → 多对多）**：

```
dim1 → period1            （1:1，现状已有）
dim2 → period2            （1:1）
dim1 + dim3 → period13    （复合：实例粒度 = (dim1, dim3) 的笛卡尔组合）
dim2 + dim3 → period23    （复合）
```

**需求 R2 —— 实例映射多对一（折叠）**：

```
period23 的实例 = (dim2, dim3 % 10)
⇒ period23(dim2=0, dim3=0) 与 period23(dim2=0, dim3=10) 是同一个时间窗实例
```

形式化：period P 由有序 dim 列表 (d1..dk) 组成，每个 di 可选一个折叠变换 ti（缺省恒等），**P 的实例 = (t1(d1), ..., tk(dk))**。使能/失效/版本状态以 period 实例为粒度；JIT 特殊化仍以原始 dim 元组为粒度。

**需求 R3 —— 特殊化 key 折叠（L3）**：声明为折叠的 dim（如 `slot%10`）可进一步声明"特化折叠"——cell=0,slot=0 与 cell=0,slot=10 不仅共享 period 实例状态（R2），还**共享同一份 JIT 特化代码**。约束：slot 值本身不做常量假设（参数不替换）；但用于 period 数组下标的折叠表达式（`slot%10`）在编译期固化为折叠常量，使访问地址常量化、mayconst 优化可应用。

### 1.3 对现状实现的冲击

1. **命名空间分裂**：`dimId`（参数身份 → 决定特殊化空间）与 `periodId`（使能/失效粒度）必须分离。
2. **特殊化 key ≠ 失效 key**：dim3=0 与 dim3=10 编译出两份不同特化代码，但共享同一 period23 实例的 enable/version 状态——版本复核必须先经折叠映射到 period 实例再做。
3. **lc 守卫的 cellIdx** 不再是单个实参，而是多个实参经映射计算的结果。
4. **JIT 编译侧几乎不受影响**：特殊化仍替换原始 dim 值为常量，`dim3%10` 类索引算术被常量折叠自然消化；`may_const` 机制照常。
5. **icache 不受影响**：按原始 dims 索引，天然区分不同特殊化。

## 2. 设计决策记录（已确认）

| 决策 | 内容 |
|---|---|
| **D1 先声明后使用** | dim 与 period 需要显式声明；声明在 C/C++ 代码中通过属性载体完成（不引入 yaml/外部配置文件）；使用处找不到声明 → **编译失败** |
| **D2 使用处语法不变** | 参数上 `ejit_dim("name")`、数组上 `ejit_period_arr("name")`、函数上 `ejit_period_lc("name")` 写法保持现状；字符串语义变为"引用已声明的名字" |
| **D3 折叠走独立属性** | 折叠通过独立属性增量表达，不在名字字符串里内嵌表达式 |
| **D4 同名 1:1 声明合法** | 同名 dim "cell" 与 period "cell" 合法，语义与现状 1:1 等价；下游元数据编码与运行时注册机制不做破坏性改动 |
| **D5 声明逐条成行、属性增量追加** | 每个 dim/period 本体与每条属性（type/spec/fold）各占一条配置行、各自独立宏与载体；后续新增属性不影响既有声明行 |
| **D6 type/spec 恰好一次** | 每个 dim 的 type 与 spec 都**不允许缺省、不允许重复配置**；编译器强制检查 |
| **D7 fold 缺省规则** | fold 整行可缺省（= identity）；`EJIT_FOLD_MOD` 的 operand 可缺省，缺省取值 = 该 dim 的 spec |
| **D8 分层回退方向（应用细节暂缓）** | type（half-static/dynamic）用于分级回退：dynamic 变化导致全特化失效时，先退回"仅 half-static 视为常量"的中间 JIT 版本，再退 AOT。**应用实现为另一命题，本阶段只定语法** |
| **D9 特化折叠（L3）** | 特化 key 折叠为 **dim 级显式声明**（`ejit_dim_spec_fold(dim, enable)`，宏 `DEFINE_DIM_SPEC_FOLD`）；折叠参数不直接声明，由 AOT 从该 dim 参与的 period fold 声明推导：**operand = LCM(各 operand)、op 仅支持 MOD**（§3.4bis）；**JIT 编译期**语义 = **参数不替换 + 折叠表达式替换**（AOT 阶段 dim 非常量无法替换，AOT 只收集折叠表达式位置；落点见 §6.3） |
| **D10 数组 = 实例空间容器** | period 数组的维度结构与 period 的 dim 列表对应（三种合法形态 §3.7）；各维尺寸 = 该 dim 折叠值域（`min(spec, fold operand)`，与 O-1 缓存规格一致）；数组线性下标 == 实例索引（行优先）；维度一致性为 **warning**（动态指针数组跳过检查）；维度实现独立（period 维度由声明推导、数组维度由用户声明，检查点比对） |
| **D11 成员 period** | 结构体成员可复用 `ejit_period_arr` 标记（Subjects 扩展 FieldDecl）；成员数组 = 嵌套 period 容器（分层激活）；**嵌套限一层**，违反 → **error**；成员 period 的 dim 与外层 period 的 dim 不重叠（error）；成员不注册独立全局，绑定 = (外层类型, 成员名, 字节偏移) 编码进外层元数据 |
| **D12 生命周期管理（运行时方案 3：wrapper 全量构建 + 结构体传递）** | **只有 period 才有版本号和生效时间窗**（dim 只有特化语义）。**两个应用规格**：period 应用规格 `impl_spec_p = min(dim_spec, period_fold)`（生命周期侧取模基准）、dim 版本特化应用规格 `impl_spec_d = min(dim_spec, LCM)`（特化 key 取模基准）。wrapper 每次调用构建两层参数：**特化参数** `specs = {dimType, specId}[]`（specId = dim_value % impl_spec_d）与**生命周期参数** `periods = {periodId, cellIdx}[]`（cellIdx = 折叠元组 **C 行优先**线性化，分量 = `specId % impl_spec_p`——数学性质 `dim_v % impl_spec_d % impl_spec_p === dim_v % impl_spec_p`（两情形论证，§6.2）保证与 `dim_v % impl_spec_p` 一致，传 specId 不丢信息）。结构体指针传入 `ejit_taskpool_compile_or_get`（通用入口 + `_0d.._4d` 快速入口统一扩展签名）。**接口内零算术**：active 检查/版本复核直接查表，复核免重算（identity 全等 ⟹ cellIdx 不变）。命名体系重命名：dims→specs、instanceId→specId、新增 `ejit_period_pair_t{periodId, cellIdx}`、`Slot.versions`→`periodVersions`、`version_[dimType][instanceId]`→`versionByPeriod[periodId][cellIdx]`。跨 TU：函数相关 period 声明在本编译单元不可见 → **编译失败**（D1 严格模式应用）。细目见 §6.2 |
| **D13 规格上限矩阵（编译期约束，声明解析时拦截）** | ① dim 数（函数）≤ 4（**硬**：cacheKey 打包 + icache 指数）；② 每 dim 特化规格 `min(spec, LCM)` ≤ 256；③ period 组成 dim ≤ 4（**硬**：cellIdx 位域）；④ 每 period 分量 `min(spec, fold)` ≤ 256；⑤ **实例空间双约束**：period 实例空间 `Π impl_spec_p ≤ 2^16`（cellIdx 编码 u16；slot 快照 u64 = periodId(u8) \| cellIdx(u16) \| version(u32)，一次比较完成复核）**且 spec 实例空间约束落在分配后尺寸：`Π 2^⌈log2 impl_spec_d⌉ ≤ 2^16`**（icache 每维按 impl_spec_d 对齐 2 的幂分配后 × 8B ≤ 512KB/函数；对齐放大 ≤ 2^d 倍——`(129,129,3)` 的 Π min = 49923 通过但分配 256×256×4 = 262144 单元 = 2MB 超限，故约束不可用 Π impl_spec_d 直接代入；LCM ≥ fold ⟹ spec 空间 ≥ period 空间，两约束需分别满足）；⑥ 单函数相关 period ≤ 8（**软**：slot 预留 + 版本表行数，线性内存代价）；⑦ **全局 period 总数 ≤ 256（硬**：periodId u8 位域，slot 快照 periodId(u8)）；⑧ **全局版本表总账 `Σ(period 实例空间) ≤ 2^16`（硬**：版本表 = enabledByPeriod/versionByPeriod 裸数组按声明实例空间分配，项 ≈ 8B，Σ ≤ 2^16 实例 ⟹ ≤ 512KB——超出编译 error，**早暴露原则**：内存预算超限在编译期拦截，不拖到链接失败/运行时分配失败；病态最坏账（256 period × 2^16 实例 = 128MB）由此封顶）。icache 每维尺寸不再固定 16，按函数声明维度对齐 2 的幂分配（AOT 生成 wrapper 时内嵌移位常数，快路径零调用性质不变；版本区（bucket/slot）是查找结构，容量由内存预算决定、与实例空间解耦，无需此约束；版本表是唯一按实例空间线性放大的状态结构）。编译期保证后运行期校验降级为防御（保留） |
| **D14 分层回退（P1 应用细化：降级链 + dim 处理三场景）** | **type（half-static/dynamic）标在 dim 上**——粒度匹配论证：特化状态是 per-dim 的（一个参数只有替换/保留一种处理），period 级 dynamic 与 per-dim 粒度不匹配（dim 同时属于 static period 与 dynamic period 时无法两全）；且 IR 优化层面已无 period 概念（只剩标记与全局偏移访问），嵌值资格由参数替换状态唯一决定（§6.3）。**wrapper 两级降级链**（简单模型，失败原因不区分）：轮1 带**全部** dim 的 specs + periods（含 dynamic dim，正常查缓存/miss 编译语义）→ 拿不到可用指针则轮2 去掉**全部** dynamic dim 及其相关 periods 再调用（半特化：key 仅 static dim、dynamic period 数据查表，版本不再牵连）→ 仍失败则 AOT 兜底。**dim 处理三场景**（替换集合 per 请求，同一 AOT bitcode 三形态）：① 无 spec-fold + 指定优化 → 参数替换为 dim_value（PASS1 现状）；② 有 spec-fold + 指定优化 → 参数不替换、折叠表达式 `urem %arg, impl_spec_p` 替换为立即数 `specId % impl_spec_p`（**形态规则限定**：仅该 period 声明 fold 的表达式——`% == impl_spec_p`——才替换，§3.7 规则 1；数学性质 §6.2 保证替换值与运行期真实值一致）→ 两场景共同效果：**寻址全常量 → 嵌值**；③ 未指定优化（dynamic）→ 全不替换，运行期算 cellIdx 查表。**折叠表达式替换发生在 JIT 编译期**（EJitOptimizer）——AOT 阶段 dim 非常量无法替换，AOT 只收集折叠表达式位置。细目见 §6.3 |

## 3. 语法定义（本阶段聚焦）

### 3.1 声明载体与宏机制

声明属性附着在文件作用域"载体"静态变量上（纯声明载体，无运行期语义），约定集中在公共头文件（如 `ejit_period_defs.h`），所有使用 ejit 标记的 TU 必须包含：

- 每条配置行用 `__COUNTER__` 生成独立载体变量名（不依赖属性跨 redeclaration 合并语义）；
- 载体变量加 `unused` 抑制告警；`static` 使每个 TU 都有自己的元数据副本（AOT pass 逐 TU 可见），注册表条目运行时按名去重、冲突报错；
- `#name` 字符串化：用户写裸名字，宏转成属性要求的字符串字面量。

### 3.2 宏族定义

```c
/* ---------- 公共辅助宏（ejit_period_defs.h） ---------- */
#define EJIT_CAT_(a, b) a##b
#define EJIT_CAT(a, b) EJIT_CAT_(a, b)

/* 变参个数统计（0..8）与逐参字符串化（FOR_EACH），标准预处理技巧 */
#define EJIT_NARG(...)  EJIT_NARG_(,##__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
#define EJIT_NARG_(...) EJIT_ARG_N(__VA_ARGS__)
#define EJIT_ARG_N(_0,_1,_2,_3,_4,_5,_6,_7,_8,N,...) N

#define EJIT_STR(x) #x
#define EJIT_STR_ALL_0()
#define EJIT_STR_ALL_1(a)       EJIT_STR(a)
#define EJIT_STR_ALL_2(a, ...)  EJIT_STR(a), EJIT_STR_ALL_1(__VA_ARGS__)
/* ... 以此类推到 EJIT_STR_ALL_8 ... */
#define EJIT_SELECT(_1,_2,_3,_4,_5,_6,_7,_8,NAME,...) NAME
#define EJIT_STR_ALL(...) EJIT_SELECT(__VA_ARGS__, \
    EJIT_STR_ALL_8, EJIT_STR_ALL_7, EJIT_STR_ALL_6, EJIT_STR_ALL_5, \
    EJIT_STR_ALL_4, EJIT_STR_ALL_3, EJIT_STR_ALL_2, EJIT_STR_ALL_1, \
    EJIT_STR_ALL_0)(__VA_ARGS__)

/* ---------- dim：本体 + 属性（各恰好一次） ---------- */
#define DEFINE_DIM(dim_name) \
  __attribute__((ejit_dim_decl(#dim_name))) \
  static int EJIT_CAT(ejit_dim_decl_anchor_, __COUNTER__) \
      __attribute__((unused));

#define DEFINE_DIM_TYPE(dim_name, dim_type) \
  __attribute__((ejit_dim_type(#dim_name, #dim_type))) \
  static int EJIT_CAT(ejit_dim_type_anchor_, __COUNTER__) \
      __attribute__((unused));

#define DEFINE_DIM_SPEC(dim_name, dim_spec) \
  __attribute__((ejit_dim_spec(#dim_name, dim_spec))) \
  static int EJIT_CAT(ejit_dim_spec_anchor_, __COUNTER__) \
      __attribute__((unused));

/* ---------- period：本体 + 组成 dim 列表（空列表 = 同名 1:1 sugar） ---------- */
#define DEFINE_PERIOD(period_name, ...) \
  EJIT_PERIOD_IMPL(EJIT_NARG(__VA_ARGS__))(period_name, __VA_ARGS__)
#define EJIT_PERIOD_IMPL(n) EJIT_CAT(EJIT_PERIOD_IMPL_, n)
#define EJIT_PERIOD_IMPL_0(p) \
  __attribute__((ejit_period_decl(#p))) \
  static int EJIT_CAT(ejit_period_decl_anchor_, __COUNTER__) \
      __attribute__((unused));
#define EJIT_PERIOD_IMPL_1(p, ...) EJIT_PERIOD_IMPL_N(p, __VA_ARGS__)
/* ... 2..8 同 EJIT_PERIOD_IMPL_1 ... */
#define EJIT_PERIOD_IMPL_N(p, ...) \
  __attribute__((ejit_period_decl(#p, EJIT_STR_ALL(__VA_ARGS__)))) \
  static int EJIT_CAT(ejit_period_decl_anchor_, __COUNTER__) \
      __attribute__((unused));

/* ---------- fold：可增量追加；operand 可缺省（→ 该 dim 的 spec）；示例中随所属 period 归组声明（§3.5，O-6） ---------- */
#define DEFINE_FOLD(...) EJIT_FOLD_SELECT(EJIT_NARG(__VA_ARGS__))(__VA_ARGS__)
#define EJIT_FOLD_SELECT(n) EJIT_CAT(DEFINE_FOLD_, n)
#define DEFINE_FOLD_3(period_name, dim_name, fold_op) \
  __attribute__((ejit_period_fold(#period_name, #dim_name, fold_op))) \
  static int EJIT_CAT(ejit_fold_decl_anchor_, __COUNTER__) \
      __attribute__((unused));
#define DEFINE_FOLD_4(period_name, dim_name, fold_op, operand) \
  __attribute__((ejit_period_fold(#period_name, #dim_name, fold_op, operand))) \
  static int EJIT_CAT(ejit_fold_decl_anchor_, __COUNTER__) \
      __attribute__((unused));

/* ---------- 特化折叠（L3）：dim 级声明；enable 开关；折叠参数由编译器从 period fold 推导（§3.4bis） ---------- */
#define DEFINE_DIM_SPEC_FOLD(dim_name, enable) \
  __attribute__((ejit_dim_spec_fold(#dim_name, enable))) \
  static int EJIT_CAT(ejit_spec_fold_anchor_, __COUNTER__) \
      __attribute__((unused));
```

> 实现注：`EJIT_STR_ALL_2..8`、`EJIT_PERIOD_IMPL_1..8` 为机械展开，clang 已验证 `##__VA_ARGS__`（GNU 扩展）与 `__COUNTER__`；C++20 下可改用 `__VA_OPT__`。

### 3.3 属性定义（Attr.td 签名）

| 属性 | 参数 | 约束（Sema 强制） |
|---|---|---|
| `ejit_dim_decl` | `StringArgument dim` | 每 dim 恰好一次 |
| `ejit_dim_type` | `StringArgument dim, StringArgument type` | 每 dim 恰好一次；type ∈ 白名单 `{half-static, dynamic}`（预留 `static`）；**语义（D14）**：dynamic = 降级候选——wrapper 降级链轮2 去掉该 dim（不特化、数据查表），轮1 仍正常参与全特化（§6.3） |
| `ejit_dim_spec` | `StringArgument dim, IntArgument spec` | 每 dim 恰好一次；spec ∈ (0, 256] |
| `ejit_period_decl` | `StringArgument period, VariadicStringArgument dims` | 每 period 恰好一次；dims 均已声明；空 dims = 同名 1:1 sugar |
| `ejit_period_fold` | `StringArgument period, StringArgument dim, EnumArgument op, Optional IntArgument operand` | 每 (period, dim) 至多一条；period 必须声明且包含该 dim；operand 缺省 → Sema 填入该 dim 的 spec |
| `ejit_dim_spec_fold` | `StringArgument dim, BoolArgument enable` | 每 dim 恰好一次（D6）；dim 必须已声明；enable=true 时该 dim 必须存在非 identity 的 MOD fold 声明（C14/C15），operand 推导见 3.4bis |
| `ejit_period_lc`（本次升级为变参） | `VariadicStringArgument periods`（现状单参 `StringArgument`） | 函数级（生命周期控制，使用处属性）；每个 period 必须已声明；**每个 period 的全部组成 dim 必须都在该函数参数上以 `ejit_dim` 标记**，缺 → **error**；PASS4 插桩 cellIdx 按维度偏移计算（§6.2），缺组成 dim 参数 → error |
| `ejit_period_writer`（**未来实现，见 §7 命题 P2**） | `StringArgument period`（暂定） | 函数级；声明该函数修改 period 实例数据；功能：写检测（写 mayconst 属性 / memcpy / memset / strcpy 进属性或持有属性的结构体 → 必须标记）+ 调用纪律（writer 只能被 writer/lc 函数调用，且不得进入 entry 调用图）+ 指针污点传染（writer 进入类型系统：指向 writer 的指针携带污点，保存/传递/间接调用全链传染，降级赋值与 void* 中转报错）。本轮仅记录属性名与语义，实现暂缓 |

折叠 op 枚举：`EJIT_FOLD_IDENTITY=0 / EJIT_FOLD_MOD=1 / EJIT_FOLD_DIV=2 / EJIT_FOLD_SHR=3 / EJIT_FOLD_AND=4`（O-3 已决：DIV/SHR/AND 为**预留值**，使用即 error——fold 算子仅 MOD，见 §3.4 第 5 条）。

### 3.4 缺省规则（D7 细化）

1. **fold 整行缺省**：period 的某 dim 无 `DEFINE_FOLD` 行 → 变换为 identity。
2. **operand 缺省**：`DEFINE_FOLD(p, d, EJIT_FOLD_MOD)` → operand 取该 dim 的 spec。
3. **Sema 期填实**：fold 与 dim 声明同在公共头文件、同一 TU 可见，Sema 收集后直接把缺省 operand 补成 spec 数值写入元数据，运行时零推断；spec 不可见时直接报错（与 D1 严格模式一致）。
4. 观察：对合法值域（dim 值 < spec）而言，`v % spec ≡ v`——缺省 operand 的语义是"折叠机制统一走取模路径，不配置时退化为恒等"，行为安全。
5. **fold 算子集合（O-3 已决，v0.4.16）**：`fold_op` **仅支持 MOD**（`dim % operand`），与 L3 特化折叠一致（O-8/C15）——DIV/SHR/AND 与 dim 间线性组合**不引入**。结构性原因：非 MOD 算子不可逆（取商/移位/按位与均丢信息），从 specId 无法恢复折叠分量，破坏 §6.2"只传 specId 不丢信息"推论与 D12 接口设计（wrapper 需额外传值）；组合折叠进一步打破 cellIdx 行优先坐标系与 D13 实例空间 Π 约束。业务量化需求（时间分桶等）走轮2 查表路径（运行期计算，不参与特化）。

### 3.4bis 特化折叠参数推导（D9 细化）

1. **operand = LCM**：AOT 收集该 dim 在所有 period 上的 fold 声明（缺省 operand 按 3.4 填 spec 后），取各 **operand < spec** 的 operand 的最小公倍数作为特化 key 折叠模数（**operand = spec 即值域内恒等**——dim_v < spec ⟹ `dim_v % spec = dim_v`，无折叠需求，**不计入 LCM**；计入会无谓抬高模数，如 spec=32、恒等 fold + fold 10 → lcm(32,10)=160 而实际只需 10）。理由：entry 函数假设可能依赖**所有** period 的全局变量（不做访问闭包分析）——特化 key 折叠后相同必须蕴含**每个** period 的折叠值都相同，模数必须是各 operand 的公倍数，LCM 是最小安全值。
2. **op 仅支持 MOD**（C15）：identity（无 fold 声明）不参与推导；DIV/SHR/AND 或 op 不一致 → 编译错误。
3. **enable=false**：显式关闭 L3（等同无声明效果）；声明本身仍受 D6"恰好一次"约束，重复声明（无论取值）报错。
4. **运行时零推断**：LCM 在编译期算好写入元数据（§5），wrapper 插桩与 JIT 编译期直接消费，运行时无推导。
5. **正确性（v0.4.9 重述）**：key 折叠相同（specId 相同）⟹ 对每个**有折叠表达式**的 period，替换常量 = `specId % impl_spec_p`，数学性质（§6.2：`dim_v % impl_spec_d % impl_spec_p === dim_v % impl_spec_p`，两情形论证）保证 == 实例真实折叠值 `dim_v % impl_spec_p` ⟹ 替换常量与运行期语义一致，**无条件安全**（无需访问模式证明）。恒等 period（impl_spec_p = spec）无折叠表达式（§3.7 形态规则），不参与替换。替换值由 specId 唯一确定 ⟹ 同 key 族内版本行为一致。

### 3.5 端到端示例

```c
/* ejit_period_defs.h —— 所有使用 ejit 标记的 TU 必须包含 */
DEFINE_DIM(dim1)   DEFINE_DIM_TYPE(dim1, half-static)  DEFINE_DIM_SPEC(dim1, 16)
DEFINE_DIM(dim2)   DEFINE_DIM_TYPE(dim2, dynamic)      DEFINE_DIM_SPEC(dim2, 64)
DEFINE_DIM(dim3)   DEFINE_DIM_TYPE(dim3, dynamic)      DEFINE_DIM_SPEC(dim3, 32)

DEFINE_PERIOD(period1,  dim1)
DEFINE_PERIOD(period2,  dim2)
DEFINE_PERIOD(period13, dim1, dim3)
DEFINE_PERIOD(period23, dim2, dim3)
DEFINE_FOLD(period23, dim3, EJIT_FOLD_MOD, 10)   /* fold 随所属 period 归组声明（O-6） */
DEFINE_DIM_SPEC_FOLD(dim3, true)                 /* L3：dim3 特化 key 折叠，operand 推导 = LCM(10) = 10 */

/* ---- 使用处：与现状完全一致 ---- */
__attribute__((ejit_period_arr("period1")))  struct Cfg1  g_cfg1[N1];
__attribute__((ejit_period_arr("period13"))) struct Cfg13 g_cfg13[N13];
__attribute__((ejit_period_arr("period23"))) struct Cfg23 g_cfg23[N23];

__attribute__((ejit_entry))
void process(int a __attribute__((ejit_dim("dim1"))),
             int b __attribute__((ejit_dim("dim2"))),
             int c __attribute__((ejit_dim("dim3")))) { ... }

__attribute__((ejit_period_lc("period13")))
void on_period13(int cell __attribute__((ejit_dim("dim1"))),
                 int sub  __attribute__((ejit_dim("dim3")))) { ... }
```

### 3.6 兼容与迁移

- 旧代码迁移成本 = 公共头文件加声明行：`DEFINE_DIM(cell)` + `DEFINE_DIM_TYPE(cell, ?)` + `DEFINE_DIM_SPEC(cell, ?)` + `DEFINE_PERIOD(cell)`（空列表 sugar）。
- **已决（v0.4.15，O-2）**：**不允许隐式 1:1 声明**——不引入"同名隐式声明"让旧代码零迁移；旧代码必须显式声明（`DEFINE_PERIOD(cell)` 空列表 sugar 即为显式路径），保持 D6"恰好一次"的单一规则。

### 3.7 period 数组的三种形态与 pattern 判定（D10 细化）

period P = (d1..dk)，各维折叠值域 **D_i = min(spec_i, fold operand_i)**（与 O-1 缓存规格一致）。三种合法容器形态：

| 形态 | 声明 | 维度 | 实例索引 |
|---|---|---|---|
| a 逐维 | `Data g[D1][D2]...[Dk]` | 每维 = 对应 dim 折叠值域 | C 行优先线性化（天然 == 实例索引） |
| b 展平 | `Data g[D]`，D = Π D_i | 总槽 = 实例空间 | 显式线性化表达式（如 `cell*10 + slot%10`） |
| c 嵌套成员 | 外层 `Data g[D1]` + 成员数组 `dim2_var[D2]`（§3.8） | 每层各自 = 折叠值域 | 分层实例索引 |

**嵌值判定规则（v0.4.2 值级判定；v0.4.9 pattern 形态规则定案）**：

1. **pattern 形态（嵌值候选，按该全局声明的 period 校验）**：访问下标成分 ∈ {dim 直用, dim 的折叠表达式, 常量}——折叠表达式的形态由该 period 的 **impl_spec_p** 决定（§6.2）：
   - **period 无 fold**（impl_spec_p = spec）：直用 `g[dim]`，或 `g[dim % XX]`（XX ≥ spec，值域内恒等）均可；
   - **period 有 fold**（impl_spec_p = min(spec, fold) < spec）：必须 `g[dim % impl_spec_p]`（模数**精确等于** impl_spec_p）；
   - 形态不匹配（如声明 fold 10 却写 `% 3`，或 `% 16` 介于 fold 与 spec 之间）→ 折叠替换不命中 → 保持运行期计算（advisory warning，C21）。
   替换后的可常量化：场景 1（参数替换）下 `g[dim]`/`g[dim % XX]` 自然常量；场景 2（折叠表达式替换）下仅 `g[dim % impl_spec_p]` 被替换为立即数 `specId % impl_spec_p`——数学性质（§6.2）保证 == `dim_v % impl_spec_p`，与运行期真实值一致。
2. 嵌值资格（唯一判据，值级）：`L == 本次调用实例 cellIdx`（该全局所属 period 的实例索引，C 行优先坐标系，与 §6.2 同式）。`g[cell+1]` → L ≠ cellIdx → 不嵌，保持运行期读取；常量下标 `g[5]` 不再单独排除——与复核实例相等时嵌值且安全（v0.4.2）。
3. **判定时机（v0.4.2 修正）**：判定在**替换后**的关卡（JIT，StructFieldPass）做——格子坐标与复核实例坐标都是常量，直接比较。（v0.4.1 曾写"必须替换前判定"，错把"无法区分来源"推成"无法判定安全性"；安全性只看值，不看来源。）
4. **安全论证**：允许嵌值 ⟺ 访问指向的格子 == 本次调用的实例（复核覆盖的实例）。实例可**重新激活并重写数据**，而复核只覆盖本次调用 dims 推导的实例；嵌值与复核覆盖同一组格子 ⟹ 数据一变即版本提升 → 复核发现 → 重编译，永远一致。`g[cell+1]` 对应实例 (1) 不在 f_0（cell=0）的复核范围 (0) 内——实例 (1) 重激活重写时 f_0 无感知，数据过期。**该限制为无条件正确性要求**（实例生命周期与 type 无关）；"嵌入实例集复核"（O-11）是补偿优化而非替代。完整推演见 [EJIT_PATTERN_CONSTANT_MODEL.md](EJIT_PATTERN_CONSTANT_MODEL.md)。
5. b 形态展平索引：替换后由关卡值比较判定（`g[cell*10+slot%10]` → 58 == cellIdx ✓；`g[cell*10+5]` → 55 ≠ cellIdx ✗；`slot%10` 为 period23 的 impl_spec_p = 10 时替换值 = `specId % 10`）；其他算子（/、`<<`、函数调用）→ 折叠替换不命中 → 保持运行期计算（advisory warning，C21）。
6. 取地址：`&g[...]` 元素地址在**同函数内**传递（SSA 拷贝/PHI）后解引用的 load 继续应用——JIT 编译期 direct GEP 全常量路径已支持（索引替换后全常量，`EJitStructFieldPass.cpp:230-269`）；L 检查作用于替换后 GEP 链（含经指针的链），无需新机制；**跨函数调用传递 → 放弃**（O-10）。
7. slot 值本身永不常量（spec_fold 语义）：`printf(cell, slot)` 不应用；只有访问下标中的折叠表达式固化为折叠常量。

维度一致性检查（**warning**，不阻止编译；动态指针数组跳过检查）：常量数组每维尺寸 == 对应 dim 折叠值域（a/c），b 形态总槽 == Π 折叠值域；维度实现独立——period 维度由声明推导、数组维度由用户声明，检查点比对。

### 3.8 成员 period（D11 细化）

结构体成员复用 `ejit_period_arr`（Subjects 扩展 FieldDecl；Sema 按语境分流——成员需外层容器检查）：

```c
struct Data {
    may_const_attr int xxx;
    __attribute__((ejit_period_arr("period2"))) int dim2_var[D2];  /* 成员 period 容器 */
} g[D1];   /* 外层 period1 容器（分层激活：cell 变化只失效外层，slot 变化只失效成员） */
```

- **注册**：成员不注册独立全局；绑定 = (外层数组类型, 成员名, 字节偏移) 编码进外层 `ejit_period_arr` 元数据的**成员描述表**（编译期常量偏移）。
- **版本复核**：复合访问 `g[cell].dim2_var[slot%10].may_const` 需**双 period 版本校验**（period1(cell) + period2(slot%10)）；cache entry 版本快照含嵌套 period 实例。
- **约束（违反 → error）**：嵌套限一层（成员数组元素不能再含 period 标记成员，C19）；成员 period 的 dim 与外层 period 的 dim 不重叠（正交性，C20）。
- **替代方案（非侵入，备选）**：声明层绑定宏 `DEFINE_PERIOD_MEMBER(period2, Data, dim2_var)`——struct 定义不动（第三方类型），AOT 按类型名+成员名关联；当成员属性无法加在类型上时使用。

## 4. Sema 校验矩阵

| # | 场景 | 结果 |
|---|---|---|
| C1 | `ejit_dim("X")` 参数 / `ejit_period_arr("P")` / `ejit_period_lc("P")`：名字未声明 | **错误** |
| C2 | dim 本体重复声明（两条 `DEFINE_DIM(dim1)`） | **错误**（D6） |
| C3 | dim 缺 type / type 重复 | **错误**（D6） |
| C4 | dim 缺 spec / spec 重复 | **错误**（D6） |
| C5 | type 不在白名单 | **错误** |
| C6 | spec ≤ 0 或 > 256 | **错误** |
| C7 | period 重复声明；组成 dim 未声明 | **错误** |
| C8 | fold：period 未声明 / period 不含该 dim / (period,dim) 重复 | **错误** |
| C9 | fold 缺省 operand 时该 dim 的 spec 不可见 | **错误**（Sema 期填实，见 3.4） |
| C10 | fold op 枚举非法 | **错误**（EnumArgument 自动） |
| C11 | 既有检查保留：lc 必须有同名 dim 参数（err_ejit_period_lc_no_index）、period 数组大小 ≤ 100（err_ejit_period_arr_too_large）。**修正（v0.4.11 核实）**：Sema 层无"函数 dim 数 ≤ 4"诊断（SemaEJIT.cpp:207 注释推迟到后端/运行时）与"整型参数"检查——两项为后端/运行时兜底，不列 Sema；"数组大小 vs 实例空间一致性"为 D10 新增（C18），非既有 | 不变 |
| C12 | `ejit_dim_spec_fold` 的 dim 未声明 | **错误** |
| C13 | 同一 dim 重复 spec_fold 声明（含 enable 取值不同） | **错误**（D6） |
| C14 | spec_fold dim 在所有 period 上均无非 identity fold 声明（operand 无法推导） | **错误** |
| C15 | period fold 的 op 非 MOD（DIV/SHR/AND） | **错误**（fold 算子仅 MOD——O-3 已决，L3 与 period fold 两层一致，§3.4 第 5 条） |
| C16 | 推导 LCM > 256（specId = dim_v % impl_spec_d 值域上界，impl_spec_d ≤ 256） | **错误** |
| C17 | （advisory）spec_fold dim 的函数体中无匹配 (MOD, operand) 形态的访问 | warning，不阻止（保守正确） |
| C18 | 数组每维尺寸 / 展平总槽与 period 折叠值域不一致（动态指针跳过） | **warning**（D10） |
| C19 | 嵌套 period 超过一层（成员数组元素含 period 标记成员） | **error**（D11） |
| C20 | 成员 period 的 dim 与外层 period 的 dim 重叠 | **error**（D11） |
| C21 | （advisory）非 dim 派生的 period 访问：展平索引含 {+, ×} 之外算子或独立常量偏置、dim 派生变换（`g[cell+1]`）、常量下标（`g[5]`）、非 dim 下标 | warning，不阻止；嵌值资格由关卡值级判定（§3.7 规则 2/3/5）——格子 == 复核实例时仍嵌值 |
| C22 | fold operand ≤ 0（`dim % 0` 除零 UB） | **错误**（operand ≥ 1；operand = 1 恒等 0，允许） |

新增诊断命名沿用 `err_ejit_*` 风格（`err_ejit_dim_redecl` / `err_ejit_dim_missing_type` / `err_ejit_dim_dup_type` / `err_ejit_dim_missing_spec` / `err_ejit_dim_dup_spec` / `err_ejit_bad_dim_type` / `err_ejit_bad_dim_spec` / `err_ejit_fold_*` 等）。

## 5. IR 元数据编码草案

载体全局上发射 `!ejit.metadata`（复用 `emitEjitGlobalMetadata` 机制，逐 TU 发射）：

```llvm
!ejit.metadata = distinct !{
  !{!"ejit_dim_decl",    !"dim3"},
  !{!"ejit_dim_type",    !"dim3", !"dynamic"},
  !{!"ejit_dim_spec",    !"dim3", i32 32},
  !{!"ejit_period_decl", !"period23", !"dim2", !"dim3"},
  !{!"ejit_period_fold", !"period23", !"dim3", i8 1, i32 10}   ; op=MOD, operand 已填实
  !{!"ejit_dim_spec_fold", !"dim3", i1 true, i32 10}   ; enable + LCM 编译期算好
}
```

- 缺省 operand 在 Sema 期填实（3.4），元数据永远存实际数值，运行时零推断。
- 特化折叠（L3）：`ejit_dim_spec_fold` 子节点存 enable 与推导好的 LCM；EJitOptimizer 从 bitcode 中的声明元数据解析替换规则（PASS1 需保留声明载体进 bitcode，见 §6）。
- 使用处元数据：`ejit_period_arr_ind` / `ejit_period_arr` 等**照旧**（D4 的"下游编码不改"由此成立）；**`ejit_period_lc` 为例外**——v0.4.4 升级为变参（显式 period 列表），使用处编码随签名变化。
- 标签常量在 `EJitCommon.h` 增加 `TAG_EJIT_DIM_DECL` / `TAG_EJIT_DIM_TYPE` / `TAG_EJIT_DIM_SPEC` / `TAG_EJIT_PERIOD_DECL` / `TAG_EJIT_PERIOD_FOLD`。

## 6. 注册表与运行时影响预览

（本阶段只定标记；以下为影响面定位，type 的应用语义见 §7。）

### 6.1 现状核实（生命周期管理讨论起点，2026-08-19 核实）

**接口名**：wrapper 生成的运行时接口是 `ejit_taskpool_compile_or_get`（`FN_TASKPOOL_COMPILE_OR_GET`，EJitCommon.h:91-104，含 `_0d.._4d` 固定签名快速入口；C ABI 在 EJitRuntime.cpp:583；核心在 EJitSharedTaskPool）。签名：`(funcIndex, dims: {dimType, instanceId}[], numDims, outFn, outBucket)`。

**dims 数组的两重身份（5 步闭环，EJitSharedTaskPool.cpp）**：

| # | 环节 | 现状代码 |
|---|---|---|
| 1 | **特化 key**：`hashIdentity`/`slotIdentityMatches` 按 (funcIndex, dims[]) 全等匹配 | :482-504 |
| 2 | **编译触发时版本快照**：`Req.versions[i] = instanceVersion(dims[i].dimType, dims[i].instanceId)`，组成 `{dimType, offset, version}[]` | :2007-2010 |
| 3 | **发布时保存**：`target->dims[i]` / `target->versions[i]` | :1367-1370 |
| 4 | **命中前复核**：`Slot.versions[i] != instanceVersion(...)` → stale → miss | :530-537 |
| 5 | **失效通道**：`invalidateByPeriod` 空实现（EJit.cpp:525）——无主动遍历清 slot；失效完全靠 `setInstanceEnabled` 的 `version.fetchAdd(1)`（:385）→ 下次复核版本不一致自然 miss。**版本号是唯一失效机制** |

**publish 提交门 + 读令牌（现状核实补，v0.4.11）**：① **提交门**（EJitSharedTaskPool.cpp:1321-1324）：编译结果发布有原子提交门——竞争同一 slot 的多个编译完成时，串行化"谁有资格写入"（版本快照复核 + generation gate 后落位，失败者丢弃结果）；② **读令牌排空**（:1317 `bucketWrite` 自旋等读者计数归零、:1399-1406 releaseRead）：发布时等所有在读读者完成才写 slot，旧 fnPtr 在排空后锁外释放（:1398-1403）——**bucket 层读写同步现状已有**。与双位协议（§6.4）**正交**：提交门/读令牌管**慢路径**（发布权竞争 + 排空同步，有锁自旋），双位协议管**快路径**（无锁 tag + 双位，读者零开销）；icache 快路径不进读令牌（每次调用进读令牌 = 每次调用开销）。

**粒度现状（对照"只有 period 才有版本号和生效时间窗"）**：

- ✅ **激活/失效入口已是 (periodName, cellIdx) 粒度**：`ejit_activate/deactivate(periodName, cellIdx)`（EJitRuntime.cpp:389-413），PASS4 在 `ejit_period_lc` 插桩。
- ❌ **版本表 per-dim**：`version_[dimType][instanceId]`（:199-204）——1:1 现状下与 per-period 等价，复合后必须拆。
- ❌ **复核 per-dim**：`Slot.versions[numDims]` 逐 dim 比对（:530-537）。

**结论**：dims 数组同时承担"特化 key"与"版本定位器"两个角色，1:1 时代两者合一；复合后必须拆开——key 用 spec fold 值（现状 instanceId 已是折叠值，保持），版本定位改 per-period。**方案刷新待讨论完成后更新本表**。

| 组件 | 影响 |
|---|---|
| 注册表 | 新增 `EJIT_REG_PERIOD_DEF = 8`（沿用 5/6/7 的加法模式；40B ABI 不变）。name1=period 名，ptr=dim/fold 描述表，size=表项数；跨 TU 重复条目按名去重、冲突报错 |
| 命名空间 | `DimRegistry`（dim 名 → dimId + type 编号 + spec）与 `PeriodRegistry`（period 名 → periodId + 组成/折叠表）分离；type 字符串 → u8 编号（`static=0/half-static=1/dynamic=2`，预留）加速运行期计算 |
| 缓存尺寸（O-1 已决） | **spec = dim 值域大小**（取值 ∈ [0, spec)，C6 边界维持）。缓存规格：该 dim 有 L3 特化折叠 → 折叠值域 `min(spec, LCM)`（dim < spec 且折叠值 < LCM ⟹ 折叠值 < min(spec, LCM)，裸 LCM 在 spec < LCM 时浪费槽位）；无 L3 → 按 spec。替换写死的 `EJIT_ICACHE_DIM_SIZE=16` 与 SwitchController 的 256 |
| SwitchController | `enabled_/version_` 粒度从 (dimType, instance) 改为 **(periodId, cellIdx)**；cellIdx = 折叠元组行优先线性化实例号（u16，D13⑤）；表结构：`enabledByPeriod[periodId][cellIdx]`（u8）+ `versionByPeriod[periodId][cellIdx]`（u32），**每 period 一行按声明实例空间分配**（无预留浪费，维持裸数组无 map） |
| 查找/版本复核 | wrapper 传 **specs（特化 key）+ periods（生命周期参数）** 结构体指针（D12）；缓存 key = specs 照旧；版本复核 = slot 快照（u64 = periodId\|cellIdx\|version）一次比较，**免重算**（identity 全等 ⟹ cellIdx 不变）；active 检查直接查表，接口内零算术 |
| wrapper（PASS3） | 构建两层参数：**specs**——spec_fold dim 实参插 `urem %LCM`（**L3 新增**：现状 emitInstanceVal 仅 Trunc/ZExt 无 urem，EJitWrapperGen.cpp:699-705；icache GEP 下标、bucket cache key、编译请求 specId 三处统一取折叠值）；**periods**——对函数相关每个 period（编译期静态推导：组成 dim ⊆ 函数参数集）插指令算 cellIdx（分量 = specId % impl_spec_p——数学性质保证 == dim_v % impl_spec_p，operand/stride 为 AOT 立即数），栈上构建数组传入；相关 period 声明不可见 → 编译失败（D12 跨 TU） |
| 编译/JIT | 非 L3 dim 照旧（参数替换，特殊化 key = 原始元组）；`EJitOptimizer` 对 spec_fold dim 改为：**参数不替换** + 模式匹配 (MOD, operand) 形态的折叠表达式并替换为 `ConstantInt(specId mod operand)` → InstCombine 折叠 GEP → `EJitStructFieldPass` 走全常量路径（**零改动**）；编译请求**结构布局不变**（dims 全传，完整性校验照过；字段仅按 D12 更名 dims→specs、instanceId→specId、packedDims→packedSpecs） |
| PASS4 lc 插桩 | `ejit_period_lc` 升级为**变参**（`"p1","p2",...`，每 period 各插一对 activate/deactivate）；cellIdx 按**维度偏移**内联计算：`cellIdx = Σ ((arg_i % impl_spec_p) × stride_i)`（分量取模），`stride_i = Π_{j>i} impl_spec_p_j`（C 行优先套 B，左维最主，与 D10 数组维度、嵌值关卡 L 同一坐标系）；缺组成 dim 参数 → **error**（§3.3 lc 行）。`ejit_activate/deactivate` 签名保持 `(name, cellIdx)`，cellIdx 语义升级为实例索引 |
| icache | spec_fold dim 的 GEP 下标用折叠值（槽位自动共享）；维度尺寸改用 spec（O-1/O-9 联动） |
| PASS2 | period 数组注册携带组合信息（来自声明元数据，按名关联） |
| 数组编码（D10/D11） | `ejit_period_arr` 元数据扩展：完整维度列表（现状只最外层 count）+ 成员 period 描述表（类型, 成员名, 字节偏移）；`reAnnotateMayConst` 兜底匹配器升级为 pattern 判定（支持多动态索引，现状只允许一个）；JIT 期 direct GEP 全常量路径已覆盖取地址再解引用，无需新机制 |
| 嵌值安全（v0.4.2） | **关卡值级判定**（§3.7 规则 3/4）：JIT 期 StructFieldPass 新增检查——GEP 全常量 ⟹ 数组线性下标 L（段级，覆盖三种形态）⟹ `L == cellIdx(该 period)` 才嵌值；clang/PASS1/偏移兜底**零改动**。**现状洞**：`g[cell+1]` 类访问参数替换后 GEP 全常量即嵌值，且无版本复核跟踪——本修订同时是现状修复 |

### 6.2 生命周期管理方案（v0.4.3 定型，D12/D13）

**接口签名**（通用入口 + `_0d.._4d` 快速入口统一扩展）：

```c
typedef struct { uint32_t dimType; uint32_t specId; } ejit_spec_pair_t;    // 原 EJitDimPair 改名
typedef struct { uint8_t periodId; uint16_t cellIdx; } ejit_period_pair_t;  // 新增（D13⑤：cellIdx u16）

ejit_status_t ejit_taskpool_compile_or_get(
    uint32_t funcIndex,
    const ejit_spec_pair_t *specs, uint32_t numSpecs,       // 特化参数（原 dims）
    const ejit_period_pair_t *periods, uint32_t numPeriods, // 生命周期参数（新增）
    void **outFn, uint32_t *outBucket);
```

**两个应用规格（v0.4.9 定案）**：

- **period 应用规格 `impl_spec_p`**（per-period，该 period 每个组成 dim 各一个）：`impl_spec_p = min(dim_spec, period_fold)`。作用有二：
  1. **运行期**：生命周期分量的取模基准——分量 = `dim_v % impl_spec_p`；用于 active 检查、版本复核、slot 入库校验、编译是否可触发；
  2. **IR/JIT 期**：pattern 一致性校验的基准（按该全局声明的 period 校验，形态规则见 §3.7 规则 1）。
- **dim 版本特化应用规格 `impl_spec_d`**（per-dim）：`impl_spec_d = min(dim_spec, LCM)`。作用：特化 key 的取模基准——`specId = dim_v % impl_spec_d`（缓存 key、slot 保存位置）。

**关键性质（替换值可推导性 / 传参不丢信息）**：`dim_v % impl_spec_d % impl_spec_p === dim_v % impl_spec_p`。两情形论证：

```
① dim_spec ≥ LCM：impl_spec_d = LCM、impl_spec_p = fold（fold | LCM）
   ⟹ (x % LCM) % fold = x % fold（数论：M | N ⟹ (x % N) % M = x % M）
② dim_spec < LCM：impl_spec_d = dim_spec，dim_v % dim_spec = dim_v（dim_v < spec 值域）
   ⟹ 平凡成立
```

**推论**：向 `ejit_compile_or_get` 传入 `specId = dim_v % impl_spec_d` **不丢信息**——接口只消费两个值：特化侧 `dim_v % impl_spec_d`（key）、生命周期侧 `dim_v % impl_spec_p`，后者可由 `specId % impl_spec_p` 推出。**生命周期相关一律 `% impl_spec_p`；版本特化相关一律 `% impl_spec_d`**，两域不混用。适用域：性质服务于折叠表达式替换的正确性（替换值 `specId % impl_spec_p` 与运行期真实值 `dim_v % impl_spec_p` 一致）；恒等 period（impl_spec_p = spec）无折叠表达式（§3.7 形态规则），不依赖该性质。

**信息模型**（三层，命名对应重命名）：

| 层 | 结构 | 采集位置 | 用途 |
|---|---|---|---|
| 特化参数 specs | `{dimType, specId}`，specId = dim_value % impl_spec_d | wrapper（L3 后插 urem，现状 instanceId 为原始值，折叠为新增） | 缓存 key、JIT 编译输入 |
| 生命周期参数 periods | `{periodId, cellIdx}`，cellIdx = 折叠元组行优先线性化 | wrapper（新增：`specId % impl_spec_p` + 行优先乘加） | active 检查、版本复核/快照 |
| version | 运行期查表 | 接口内 | 版本一致性 |

**cellIdx 坐标系（v0.4.9 统一为 C 行优先，左维最主）**：wrapper 与 PASS4 lc 同式——

```
cellIdx = Σ (分量_i × stride_i)，分量_i = dim_v % impl_spec_p，stride_i = Π_{j>i} D_j
D_j = period 第 j 维尺寸（= impl_spec_p）
```

与 D10 数组线性下标（§3.7 形态 a/b）、嵌值关卡 L 同一坐标系。例：period13 = (dim1, dim3)，D_0 = 16、D_1 = 32，调用 (a=3, c=25) → cellIdx = 3×32 + 25 = **121**（数组 `g13[3][25]` 线性下标一致）。**注意与旧式（stride 左维最次，`Σ arg_i × Π_{j<i}`）互为转置，旧式废弃**；分量必须取模（`dim_v % impl_spec_p`），恒等 period（impl_spec_p = spec）时 = dim_v。

**wrapper 侧**（AOT 生成，每次调用执行）：相关 period 集合编译期静态推导（函数 dim 集合 × 全局 period 定义，组成 ⊆ 参数集才相关）；对每个 period 插指令算 cellIdx（stride 为 AOT 立即数，分量 = `specId % impl_spec_p`——性质保证与 `dim_v % impl_spec_p` 一致，无需 dim_v），栈上构建 periods 数组传入。相关 period 声明不可见 → 编译失败。

**PASS4 lc 插桩**（本次升级）：`ejit_period_lc` 显式指定 period（可列表），每个 period 按名查声明 → 组成 dim 列表 → 按名找 `ejit_dim` 参数 → 维度偏移计算：

```
cellIdx = Σ ((arg_i % impl_spec_p) × stride_i)   // stride_i = Π_{j>i} D_j（C 行优先）
入口：  ejit_deactivate("p", cellIdx)   // 每个 period 各插一对
返回前：ejit_activate("p", cellIdx)
```

stride 用 `impl_spec_p`（= min(spec, fold)）——与 D10 数组各维尺寸、wrapper 侧、嵌值关卡 L 同一坐标系；分量取模（恒等 period 即 arg_i）。lc 函数缺任一组成 dim 参数 → 编译报错（§3.3 约束）。

**运行时三条路径**：

| 路径 | 频率 | 逻辑 | 新增算术 |
|------|------|------|---------|
| active 检查 | 每次调用 | `enabledByPeriod[periodId][cellIdx]` 直接查；deactivate 态取 version 失败 → fallback | 0（wrapper 已算好） |
| 版本复核 | 每次调用 | slot 快照 u64（periodId\|cellIdx\|version）vs 当前查表值一次比较；identity 全等 ⟹ cellIdx 不变（免重算） | 0 |
| 版本快照 | 编译触发（低频） | 用传入 periods 查版本表，打包进编译请求 → 发布时存 slot | 0（查表） |

**数据结构**：维持裸数组（嵌入式无 map）——`enabledByPeriod[periodId][cellIdx]`（u8）+ `versionByPeriod[periodId][cellIdx]`（u32），每 period 一行按声明实例空间分配（注册时，无预留浪费）；slot 快照 `periodVersions[8]`（u64/项）。

**改动清单**：

| 组件 | 改动 |
|------|------|
| EJitWrapperGen.cpp | period 构建指令 + 传参 + 命名改名 |
| EJitRuntime.cpp（C ABI） | 签名扩展（5 变体 + 通用）、入参校验 |
| EJitSharedTaskPool / EJitTaskPool | slot 快照 per-period、复核/快照逻辑、版本表 |
| Sema / 注册表解析 | D13 编译期约束检查 |
| 命名 | dims→specs、instanceId→specId、packedDims→packedSpecs、versions→periodVersions、version_[dimType][instanceId]→versionByPeriod[periodId][cellIdx] |

### 6.3 分层回退（D14 细化，v0.4.6）

**type（half-static/dynamic）为什么标在 dim 上**：

- **粒度匹配**：特化状态是 per-dim 的——一个参数只有"替换为常量 / 保留实参"一种处理，不存在 per-period 的中间态。若 dynamic 标在 period 上，dim1 同时属于 period1（static）与 period12（dynamic）时：选特化 → period12 失效牵连整个特化版本，period1 的嵌值白嵌；选不特化 → period1 嵌值收益白丢——**无法两全**。
- **IR 层面无 period 概念**：优化管线里只剩标记与全局偏移访问，嵌值资格由**参数替换状态**唯一决定（替换后 cellIdx 常量 ⟹ 嵌值关卡 `L == cellIdx` 成立）。判定点是 per-dim 的、机械的；period 级 dynamic 需要把 per-period 信息穿透优化管线、在每个数据访问点做额外决策，制造新的不一致面。
- **一致性自动成立**：type 标 dim ⟹ dynamic dim 不特化 ⟹ 其所有相关 period 的数据访问统一查表（L 非常量，天然不嵌值），不存在"保证 period1 保持不可变假设"的问题——因为对 dynamic dim 相关数据根本不假设。

**降级链（wrapper 侧生成，简单模型）**：

```
轮1: ptr = compile_or_get(全部 specs【dim1, dim2】 + 全部 periods【period1, period12】)
轮2: ptr = compile_or_get(去 dynamic 的 specs【dim1】 + 去 dynamic 相关 periods【period1】)
轮3: ptr = AOT 兜底
```

- 每轮内部走完整语义：查缓存 → miss 则编译 → 版本复核 → **拿到可用指针才返回**；失败原因不区分（复核失败 / 编译失败 / 缓存不可用统一降级）
- dynamic dim **一次性全部去掉**（非逐维降级）；其相关 periods 一并去掉（period 由组成 dim 定位实例，缺 dim 无法算 cellIdx）
- 轮1 是完整尝试：dynamic dim 也参与特化（业务平时快）；**复核失败即降级，不反复重编译**（dynamic 的语义就是"失效即降级"）
- 轮2 的缓存 key 仅含 static dim，有效性仅受 static 相关 periods 影响；dynamic 相关 period 数据运行期查表（实参算 cellIdx）→ 版本变化不牵连轮2 版本
- D13 上限矩阵：dynamic dim 参与轮1 特化与校验；轮1 编译失败（如超限）自然落入降级链

**dim 处理三场景（JIT 编译期按请求的特化集合做替换，同一 AOT bitcode 三形态）**：

| 场景 | 判定 | 参数（dim） | 折叠表达式 `urem %arg, N` | 寻址/数据 | 对应轮次 |
|---|---|---|---|---|---|
| 1 | 无 spec-fold + 指定优化 | **替换为 dim_value**（PASS1 现状） | —（无折叠） | cellIdx 全常量 → **嵌值** | 轮1/轮2 的 static dim |
| 2 | 有 spec-fold + 指定优化 | **不替换**（保留实参） | **替换为立即数 `spec % N`** | cellIdx 全常量 → **嵌值** | 轮1/轮2 的 static dim |
| 3 | 未指定优化（dynamic） | **不替换**（保留实参） | **保留**（运行期 % 指令） | cellIdx 运行期计算 → **查表** | 轮2 的 dynamic dim；AOT 全部（**轮1 的 dynamic dim 走场景 1/2 特化嵌值**，见下） |

- **场景 1 与场景 2 殊途同归**：替换对象不同（参数 vs 折叠表达式），共同效果是**寻址全常量 → 拿到嵌值资格**；fold = identity（无 spec-fold）时场景 1 是场景 2 的特例
- 替换集合由请求决定：轮1 = 全部 dim 按场景 1/2；轮2 = static dim 按场景 1/2 + dynamic dim 按场景 3；轮3 = 全部场景 3
- **AOT 只收集折叠表达式位置（标记），不做替换**——AOT 阶段 dim 是非常量，替换无从发生；折叠替换发生在 JIT 编译期（EJitOptimizer，D9 落点）

**数学性质（v0.4.9 重述：替换值可推导性 / 传参不丢信息）**：

```
dim_v % impl_spec_d % impl_spec_p === dim_v % impl_spec_p
```

- ① dim_spec ≥ LCM：impl_spec_d = LCM、impl_spec_p = fold（fold | LCM）⟹ `(x % LCM) % fold = x % fold`；
- ② dim_spec < LCM：impl_spec_d = dim_spec、dim_v % dim_spec = dim_v（值域内）⟹ 平凡成立。
- 推论：specId（= dim_v % impl_spec_d）确定后，各 period 分量（dim_v % impl_spec_p）均可由 `specId % impl_spec_p` 推出——wrapper 传 specId 不丢信息；JIT 编译期只凭 specId 即可把 `urem %arg, impl_spec_p`（形态匹配，§3.7 规则 1）替换为立即数 `specId % impl_spec_p`，与运行期真实值一致。
- 适用域：恒等 period（impl_spec_p = spec ∤ LCM）上性质不成立——但恒等 period 无折叠表达式（形态规则只允许直用或 `% XX ≥ spec`，后者场景 2 不替换），无替换值需求，不依赖该性质。
- 替换后参数若无其他使用被 DCE，语义一致。

**落点**：

| 组件 | 改动 |
|------|------|
| EJitWrapperGen.cpp | 多段 compile_or_get 调用 + 失败分支；轮2 的 specs/periods 集合静态推导（去 dynamic dim 及其相关 period）；AOT 兜底调用 |
| 接口（C ABI） | compile_or_get 失败通道语义：拿不到可用指针 → 返回非 OK，wrapper 据此降级 |
| JIT 编译管线 | 按请求特化集合替换：场景 1 = PASS1 参数替换（现状）；场景 2 = 折叠表达式替换（EJitOptimizer，D9 落点）；场景 3 = 跳过替换 |
| Sema | type 白名单 + dynamic 语义（§3.3）；AOT 收集折叠表达式位置标记 |

### 6.4 指针生命周期与双位发布协议（v0.4.8）

**问题**：所有特化版本（轮1 嵌值 + 轮2 查表）的 fnPtr 都会遇到替换/回收——替换时若立即销毁旧指针，**正在执行旧版本的调用悬空**（TOCTOU：load 指针与 call 之间无屏障，清理关不掉这个窗口）。**现状归因（v0.4.11 核实收窄）**：① bucket 层**无此洞**——已有读令牌排空（`bucketWrite` 自旋等读者计数归零再写，EJitSharedTaskPool.cpp:1317；释放发生在排空后锁外 :1398-1403）；② **icache 快路径无读令牌**——格子纯指针、发布写入无读者同步，替换/回收时快路径命中旧指针即悬空；③ 现状安全依赖"生产不装 releaser（fnPtr 永不物理释放，:48-49）；装 releaser 则 safety gate 自动禁用缓存（:229-230）"。**双位协议的价值 = 启用代码回收时对快路径提供无锁读写同步**——读令牌在快路径不可用（每次调用进读令牌 = 每次调用开销）。同步语义（调用计数/锁）引入每次调用开销，不可取。

**双位 + 标签协议**（固定双位，切换 = 置标签，指针不移动；**tag 三态** {0, 1, INVALID}；v0.4.14 修订为**间接引用模型**）：

**间接引用模型（v0.4.14 定案）**：快路径格子（icache 数组元素）**不拷贝 fnPtr**，存**指向 bucket slot 的地址**——发布（icacheFill）时写入一次，之后**永不变**（bucket 为固定数组，slot 地址在 pool 生命周期内稳定）。双位单元（ptr[2] + tag）位于 **bucket slot** 上：慢路径本就读它，快路径经格子间接读它——**快慢路径共享同一发布结构，失效一处、全链生效**（deactivate 无需遍历格子，见"与版本失效"段）。

```
slot 布局:  ptr[2]（指针位）+ tag: u32（0/1 = 主位指示；INVALID = 全失效）
快路径格子:  一个 cell = slot 地址（发布时写入后不变；0 = 从未发布）
读者（wrapper 快路径，每次调用）:
    s  = 格子.load(relaxed)            // slot 地址（本核私有行，程序序）
    if (s == 0) → 慢路径               // 从未发布
    t  = s->tag.load(acquire)          // u32 原子，不撕裂（slot 共享行）
    if (t == INVALID) → 走慢路径      // 视为空（deactivate 后立即对快路径生效）
    p  = s->ptr[t].load(relaxed)       // 编译成普通 load
    if (p) call p
写者（替换/首次发布，低频）:
    释放超门限副位（见 tick 管理段）    // 腾空位
    ptr[空位].store(新指针, relaxed)   // 先写指针
    tag.store(空位, release)          // 后置主位（发布）
    // 格子不需要动——slot 地址不变
```

**INVALID 态**：`tag = INVALID` 时两个槽位都是副位（主路径视为空指针）。由 deactivate 置入（见"与版本失效"段），新编译发布时退出。

**顺序要求（关键）**：**必须先写指针、后切标签**（release/acquire 发布语义）；反过来读者会读到"新标签 + 未写入位"。读者两个方向都安全：读旧标签 → 旧位（现在是副位）→ 延迟释放中，安全；读新标签 → acquire 同步保证新位指针完整可见。

**最小原子语义（≈ 普通指令）**：relaxed/acquire/release 的 load/store **无锁前缀**（x86：mov + 编译屏障，TSO 下 mov 本身有序；ARM：ldar/stlr 单指令自带屏障）——不需要显式 fence。**不能完全不用原子**：编译器可能把指针 store 移到 release 标签之后（读者读新标签 + 未写位 → 错）。RMW / 锁协议在整个协议中不存在。缓存可见性延迟 → 读者短暂滞后（读旧位，安全）。

**副位 tick 门限管理（无计时系统的嵌入式；A4 定案）**：每个槽位记 tick 戳；门限 = 1s 起步（热点函数调用 ms 级，可调整 5s）。

- **编译触发条件**：编译前检查——副位有指针且 tick 差 ≤ 门限 → **不触发编译**（延迟）；副位无指针或 tick 差 > 门限 → 触发编译。
- **释放时点（写入时）**：编译完成后写入流程——遍历副槽位，**释放 tick 差 > 门限的槽位指针**（变空位）；新结果写入任意空位并置为主位。
- **无空位兜底**：无空槽位 → **丢弃本次编译结果**（正确性防护；实际不会发生——触发编译的前提即副位超门限，且同槽写者互斥下状态不会在编译期间变化）。
- 未超门限不删仅内存代价（代码池多驻留一份），安全。**副位仅编译线程访问**（wrapper 永远读主位）→ 无并发、无锁。

**编译节流（意外收益）**：副位未超门限 → 编译推迟 → dynamic 频繁变化时编译率被自动封顶，**防编译风暴**。

**与版本失效合流（A3 定案，v0.4.14 修订为 O(1) 失效）**：版本失效不再只靠慢路径复核——**deactivate(period, cellIdx) = 版本表失效 + 刷新该实例的 slot 置 `tag = INVALID` + bump L0 epoch 粗失效**。**无需遍历任何格子/槽位**：快路径格子全部指向该 slot（间接引用模型），slot 一处失效，所有引用它的格子自动视为空。此后快路径读 INVALID → 视为空 → 落慢路径复核 → 版本不匹配 → 触发编译 → 新 fnPtr 发布（置主位，格子仍无需动）→ 快路径自动恢复，**activate 无需发布任何东西**（版本表 bump 即驱动重编译）。竞态无害性：deactivate 写入 tag 的微秒级窗口内旧代码仍执行——但此刻数据尚未写入，旧代码配旧数据**一致无害**；数据写入后任何路径（快/慢）都拿不到旧指针。INVALID 且副位未超门限的延迟编译期间，调用走降级链（轮2 查表永远新鲜）——正确性不受影响。与**指针失效**（替换/销毁，标签翻转）两个维度，组合使用。

**驱逐/re-init 错位保护（v0.4.14 新增）**：间接引用引入一个**错位洞**——bucket slot 可能被**不同 identity** 覆写：① cachePublish 桶满时驱逐"第一个占用槽"（EJitSharedTaskPool.cpp:1337-1353 `evict = &Slot`）；② owner re-init 原地重建 blob（:1568 `initSharedStorage`），清空后的 Empty 槽可被任意 identity 复用。两种情况下格子里的 slot 地址指向**别的结果**——快路径执行**错误代码**（不是 stale——stale 还是自己的旧版本，错位是别人的结果，灾难级）。**保护：被快路径引用的 slot 永不被驱逐/复用**——icacheFill（发布）时对目标 slot 置 referenced 标记；cachePublish 选驱逐目标时跳过 referenced 槽。**当前实现阶段（v0.4.14）：桶满 → 返回 `Failed`（本次编译结果直接交调用者，不缓存）**——驱逐分支**保留不删**（后续启用回收/复用策略时再改判定）；re-init 会清掉 referenced 标记——re-init 时遍历本核格子清空（gIcacheSlots 注册表核内遍历，re-init 罕见，全扫可接受）。

**前提声明（显式）**：① **写者互斥**——同一 slot 的写入仅编译线程串行发生（副位仅编译线程访问，天然成立，无需锁）；② **在途调用时长上界**——读者从 load 到 call 返回的存活期 < tick 门限（嵌入式 WCET 分析可得），保证释放副位时无在途读者持有该指针。

**开销账（v0.4.14 修订）**：快路径 = 1 次格子 load（本核私有行，程序序）+ 1 次 tag acquire load + 1 次 INVALID 比较 + 1 次 ptr relaxed load（后三者位于 slot 共享行，稳态无写者、缓存一致，代价 ≈ L1 命中）——零锁、零 RMW、零版本校验；比 v0.4.10（格子直接存 fnPtr，tag/ptr 全私有行）**多一次共享行访问**，换取 deactivate **O(1) 失效**（不遍历）+ 快慢路径单点发布（无拷贝）。写侧 = relaxed store + release store（格子写入仅发布时一次）。

## 7. 后续命题与开放问题

**命题 P1 —— type 的应用语义（D8/D14 已细化，方向定案；余项暂缓）**：

- **应用模型已定型（D14/§6.3）**：type 标在 dim 上；wrapper 两级降级链（轮1 全特化 → 轮2 去 dynamic 半特化 → AOT 兜底）；dim 处理三场景（参数替换 / 折叠表达式替换 / 全不替换），替换集合 per 请求。
- 失效钩子：dynamic period 数据变化 → 版本 bump → 轮1 嵌值版本复核失败 → **wrapper 降级轮2**（不反复重编译）；half-static 仍近似一次写入。"写入路径需要版本提升接口"与 P2 的写入窗口同域，落地时合并考虑（P2 未来实现）。
- 余项暂缓：Tier-1（轮2）是否需要独立 icache 层；是否引入第三种 `static` type（编译期恒定，§3.3 白名单已预留）。

**命题 P2 —— period 数据写者纪律（标记传染，暂缓实现）**：

- **动机**：period 数组（mayconst 属性）数据被 JIT 嵌值；若写者在不受控时机修改数据，特化版本与数据脱节。现状已有 `warn_ejit_may_const_modified_without_lc` 编译期告警（SemaEJIT.cpp:546-556，修改 mayconst 数据未走 lc → warning）；**P2 将其从 warning 强化为类型系统污点**（静态保证，见规则 ②）：写入被编译器验证只能在受控窗口（lc 失效窗口）发生。现状"实例数据一次性写入"（A-1）只是假设。
- **属性 `ejit_period_writer`**（§3.3 已记录，本轮仅定名）：声明该函数修改 period 实例数据；period 由写对象的全局标记（`ejit_period_arr`）追溯。
- **规则 ① 写者必标**：函数体写 mayconst 属性（直接写，或 memcpy/memset/strcpy 进属性或持有属性的结构体）→ 必须标 `ejit_period_writer(period)`，缺 → error。
- **规则 ② 类型系统污点（指针传染）**：writer 是函数类型的属性；指向 writer 函数的指针（赋值/初始化/参数传递）携带污点，保存/返回/间接调用**全链自动传染**；降级赋值（污染 → 干净指针）→ error（丢属性 = 漏检）；禁止经 `void*` 中转。静态分析无法区分指针的运行期分支（`cond ? writer : normal`），保守处理：污染指针的所有间接调用点视为可能调用 writer。
- **规则 ③ 调用纪律**：writer 函数（含被污染指针的间接调用）只能被同为 writer(period) 或 lc(period) 的函数调用（= 写入只发生在 lc 失效窗口）；writer 不能出现在任何 entry 函数的调用图（含内联）（= JIT 特化执行期间数据不变）。
- **规则 ④ lc 完整性**：lc(period) 必须包含 period 全部组成 dim 参数，缺 → error（**本次实现**，§3.3）。
- **与 P1 的关系**：P1 的"dynamic 数据运行期可变 → 写入路径需要版本提升接口"与 P2 的写入窗口同域，落地时可合并考虑。

**开放问题**：

| 编号 | 问题 |
|---|---|
| O-1 | ~~spec 语义~~ **已决（v0.3.1）**：spec = 值域大小；缓存规格按折叠值域（见 §6） |
| O-2 | ~~隐式 1:1 声明（3.6）~~ **已决（v0.4.15）**：不允许隐式 1:1 声明，必须显式声明（`DEFINE_PERIOD(cell)` 空列表 sugar） |
| O-3 | ~~折叠算子集合 `%N /N >>S &M` 是否覆盖业务；是否需要 dim 间线性组合~~ **已决（v0.4.16）**：fold 算子仅 MOD；DIV/SHR/AND 与 dim 间线性组合不引入（枚举保留为预留，使用即 error；业务量化走轮2 查表，§3.4 第 5 条） |
| O-4 | 各注册表容量：dim 数、period 数、每 period 实例空间上限 |
| O-5 | ~~跨 TU 一致性~~ **已决（v0.4.5）**：基于编译期检查——相关 period 声明在本编译单元不可见 → 编译失败（D12 严格模式）；运行时冲突检测不需要 |
| O-6 | ~~属性命名暂定~~ **已决（v0.4.17）**：定案保持现状（`ejit_dim_decl`/`ejit_dim_type`/`ejit_dim_spec`/`ejit_period_decl`/`ejit_period_fold`/`ejit_dim_spec_fold`/`ejit_period_writer`）；声明归组原则——fold 随所属 period 归组（语法不变，§3.5） |
| O-7 | FOR_EACH/NARG 宏机制在 clang 全版本验证（宏容量按 8 预留）；period 组成 dim 数上限 = 4 硬（D13③） |
| O-8 | ~~L3 是否扩展 AND 等 op~~ **已决（v0.3.1）**：L3 仅支持 MOD，不扩展 |
| O-9 | ~~spec_fold dim 的缓存维度~~ **已决（v0.3.1）**：按折叠值域 `min(spec, LCM)`，见 O-1/§6 |
| O-10 | 取地址跨函数传递（指针参数/返回值携带 period 标签）：本轮不做，函数内直接传递已支持 |
| O-11 | （可选优化）嵌入实例集复核：把被放弃嵌值的实例集（如 `g[cell+1]` 的实例 (1)）纳入特化版本复核范围，恢复其嵌值资格——本轮不做（§3.7 规则 4 的关卡检查为无条件正确性要求，O-11 仅为补偿优化） |
| O-12 候选 | 维度级生命周期：lc 部分覆盖（只含 period 部分组成 dim）时无法定位完整实例——本轮不匹配（lc 缺组成 dim 参数 → error，见 §3.3）；若未来需要"period 下各 dim 独立激活"，另行立项 |

### 7.1 决策编号速查（D1–D13）

| 编号 | 一句话 |
|---|---|
| D1 | 先声明后使用：使用处找不到声明 → 编译失败（严格模式也管跨 TU） |
| D2 | 使用处语法不变：ejit_dim / ejit_period_arr / ejit_period_lc 写法照旧 |
| D3 | 折叠走独立属性，不内嵌进名字字符串 |
| D4 | 同名 1:1 声明合法，与现状语义等价、下游机制不改 |
| D5 | 声明逐条成行、属性增量追加（宏载体，__COUNTER__ 编号） |
| D6 | type/spec 恰好一次，不允许缺省、不允许重复 |
| D7 | fold 缺省规则：整行缺省 = identity；operand 缺省 = 该 dim 的 spec |
| D8 | 分层回退方向已定案（D14/§6.3：type 标 dim + wrapper 两级降级链）；余项暂缓（命题 P1） |
| D9 | 特化折叠（L3）：dim 级声明，operand = 相关 period fold 的 LCM、op 仅 MOD；参数不替换 + 折叠表达式替换 |
| D10 | 数组 = 实例空间容器：维度与 period dim 列表对应，各维 = min(spec, fold operand)，线性下标 == 实例索引 |
| D11 | 成员 period：结构体成员复用 ejit_period_arr，嵌套限一层，绑定 = (类型, 成员, 字节偏移) |
| D12 | 生命周期管理（方案 3）：只有 period 有版本号与生效时间窗；wrapper 全量构建 specs + periods 结构体传入；接口内零算术；命名重命名；跨 TU 编译失败 |
| D13 | 规格上限矩阵（编译期拦截，早暴露）：dim ≤ 4 硬、impl_spec_d = min(spec,LCM) ≤ 256、period 组成 ≤ 4 硬、分量 impl_spec_p ≤ 256、**实例空间双约束**（period Π impl_spec_p ≤ 2^16 → cellIdx u16；spec **分配后** Π 2^⌈log2 impl_spec_d⌉ ≤ 2^16 → icache 数组 ≤ 512KB/函数，对齐放大 ≤ 2^d）、period ≤ 8 软、**全局 period ≤ 256 硬（periodId u8）**、**版本表 Σ(period 实例空间) ≤ 2^16 硬（≤ 512KB）** |

### 7.2 实现待办清单

按组件分三类，共 28 项。状态列：**待办** = 未开工；**本次方案范围** = v0.4.x 复合 period 方案的直接改动；**未来命题** = 与 P1/P2/O 编号关联的延后项；**代码侧** = 独立于本方案的小修正。

**A. clang 前端（4 项）**

| # | 事项 | 细节 / 关联 | 状态 |
|---|---|---|---|
| 1.1 | 新增声明属性 6 个 | `ejit_dim_decl` / `ejit_dim_type` / `ejit_dim_spec` / `ejit_period_decl` / `ejit_period_fold` / `ejit_dim_spec_fold`（§3.1/§3.3，D5）；`ejit_period_writer` 已记录签名但**未来实现**（P2） | 待办（本次方案范围） |
| 1.2 | `ejit_period_lc` 升级变参 | 显式 period 列表（不推断）；缺组成 dim 参数 → error（P2 规则 ④，本轮实现） | 待办（本次方案范围） |
| 1.3 | D13 规格上限矩阵编译期检查 | 声明解析时拦截：dim ≤ 4（硬）、impl_spec_d = min(spec,LCM) ≤ 256、period 组成 ≤ 4（硬）、分量 impl_spec_p = min(spec,fold) ≤ 256、实例空间双约束（period Π impl_spec_p ≤ 2^16；spec **分配后** Π 2^⌈log2 impl_spec_d⌉ ≤ 2^16，对齐放大 ≤ 2^d）、单函数 period ≤ 8（软） | 待办（本次方案范围） |
| 1.4 | D1 严格模式 | 相关 period 声明在本编译单元不可见 → 编译失败（O-5 已决：跨 TU 靠编译期，不做运行时冲突检测） | 待办（本次方案范围） |

**B. AOT passes（8 项）**

| # | 事项 | 细节 / 关联 | 状态 |
|---|---|---|---|
| 2.1 | PASS1 多动态索引 + 折叠表达式替换 | 替换落点 = 折叠表达式（MOD 形态）→ 折叠常量（D9/L3，§3.4bis）；operand = LCM 推导 | 待办（本次方案范围） |
| 2.2 | wrapper 构建 periods | specs 构建现状已有（specId = dim_v % impl_spec_d）；新增 periods = {periodId, cellIdx}[]，cellIdx 由 specId 推导：分量_i = specId % impl_spec_p（数学性质 == dim_v % impl_spec_p），cellIdx = Σ(分量_i × Π_{j>i} impl_spec_p_j)（套 B）；结构体指针传入（D12 方案 3） | 待办（本次方案范围） |
| 2.3 | PASS4 变参插桩 | 显式 period 列表；cellIdx = Σ((arg_i % impl_spec_p) × stride_i)（分量取模），stride_i = Π_{j>i} impl_spec_p_j（套 B 左维最主，§6.2 坐标系段） | 待办（本次方案范围） |
| 2.4 | PeriodRegistry 生成 | 声明 → 注册表行：periodId、声明实例空间（Π impl_spec_p）、cellIdx 映射 | 待办（本次方案范围） |
| 2.5 | PASS2 组合信息编码 | dim→period 关联表、D11 成员 period 绑定（类型, 成员, 字节偏移） | 待办（本次方案范围） |
| 2.6 | L3 折叠替换落点 | EJitOptimizer 唯一行为改动点；PASS6 / 注册表零改动；编译请求结构布局不变（字段仅 D12 更名）（D9） | 待办（本次方案范围） |
| 2.7 | 嵌值关卡 `L == cellIdx` | 值级判定（v0.4.2）；StructFieldPass 新增 per-period cellIdx 表（按 ctx 匹配） | 待办（本次方案范围） |
| 2.8 | 命名改名 | dims→specs、instanceId→specId、packedDims→packedSpecs（D12 命名体系） | 待办（本次方案范围） |
| 2.9 | icache 按声明维度定制 | wrapper 生成时按函数声明维度对齐 2 的幂分配 icache（移位常数 AOT 内嵌，快路径零调用不变）；尺寸 = 2^⌈log2 impl_spec_d⌉（v0.4.7/v0.4.9，运行时侧见 3.9） | 待办（本次方案范围） |

**C. 运行时（15 项）**

| # | 事项 | 细节 / 关联 | 状态 |
|---|---|---|---|
| 3.1 | 接口签名扩展 | `ejit_taskpool_compile_or_get` 通用入口 + `_0d.._4d` 快速入口统一扩展：specs + periods 结构体指针传入（D12） | 待办（本次方案范围） |
| 3.2 | `ejit_activate`/`ejit_deactivate` cellIdx 改 u16 | C ABI 现为 `uint8_t cellIdx`（EJitRuntime.cpp:389/403），D13 实例空间 ≤ 2^16 下截断；PASS4 IR 已是 i32，只改 C ABI 声明 | 待办（本次方案范围） |
| 3.3 | 版本表重构 | `enabled_[8][256]` / `version_[8][256]` 裸数组 → `enabledByPeriod` / `versionByPeriod`：每 period 一行，按声明实例空间分配（D12） | 待办（本次方案范围） |
| 3.4 | slot 快照 u64 | periodId(u8) \| cellIdx(u16) \| version(u32) 一次比较完成复核（D13；放宽 2^32 的路径 = 两 u64） | 待办（本次方案范围） |
| 3.5 | 复核逻辑 | identity 全等 ⟹ cellIdx 不变免重算；版本复核走快照（§6.2） | 待办（本次方案范围） |
| 3.6 | name→periodId 解析 | wrapper 传入字符串名 → 注册表查 periodId（替换现状 dimType 直用） | 待办（本次方案范围） |
| 3.7 | compileNow packedSpecs 打包 | cacheKey = XOR 散列 `key ^= (dimType<<32) \| specId; key *= kHashMul`（EJitSharedTaskPool hashIdentity/cacheLookup）——**打包逻辑保留**，instanceId → 折叠 specId（L3 语义变化，格式不变；现状无位域打包，v0.4.11 核实修正） | 待办（本次方案范围） |
| 3.8 | 命名改名 | `Slot.versions`→`periodVersions`、`version_[dimType][instanceId]`→`versionByPeriod[periodId][cellIdx]`（D12） | 待办（本次方案范围） |
| 3.9 | icache 折叠值下标 + 格子内容 | icache 以 specId = dim_v % impl_spec_d 为下标存储（D9 替换后语义）；每维尺寸按 2^⌈log2 impl_spec_d⌉ 对齐分配，AOT 内嵌移位常数（v0.4.7）；**格子存 bucket slot 地址**（不拷贝 fnPtr，发布时写入后不变，v0.4.14） | 待办（本次方案范围） |
| 3.10 | 双位协议落地 | slot 双位 ptr[2] + tag 三态（0/1/INVALID）；快路径 = 格子 load（slot 地址）→ tag.load(acquire) → INVALID 走慢路径 → ptr[t].load(relaxed) → call；写入流程 = 释放超门限副位 → 空位写新指针 → 置主位（格子不动）（§6.4） | 待办（本次方案范围） |
| 3.11 | deactivate 刷 slot | deactivate(period, cellIdx) = 版本表失效 + 刷新该实例 slot 置 tag=INVALID + bump L0 epoch——**无需遍历**（格子全部指向该 slot，v0.4.14 修订） | 待办（本次方案范围） |
| 3.12 | tick 门限管理 | 编译触发条件（副位 tick 差 > 门限才编译）+ 写入时释放超门限副位 + 无空位丢弃兜底；门限 1s 起可调 5s（A4，§6.4） | 待办（本次方案范围） |
| 3.13 | tick 来源定义 | tick = 系统单调计数（SysTick/cycle counter 等，无计时系统也必有节拍）；编译触发与写入流程读取，差值判门限（§6.4） | 待办（本次方案范围） |
| 3.14 | CMake 注释修正 | `EJIT_SRE_SHARED_TASKPOOL` 已默认 ON 但注释仍写 "Default OFF"（代码侧，另改） | 待办（代码侧） |
| 3.15 | referenced 驱逐保护 + 满则 Failed | icacheFill 时对目标 slot 置 referenced；cachePublish 选驱逐目标跳过 referenced 槽；**当前实现：桶满 → Failed 不缓存（本次结果直接交调用者，驱逐分支保留不删）**；re-init 时遍历本核格子清空（§6.4） | 待办（本次方案范围） |

**命题与开放问题状态**：

- **P1**（type 应用语义 / 分层回退）：暂缓，不排期（D8 仅定方向）。
- **P2**（period 数据写者纪律）：未来实现；规则 ④ lc 完整性已并入 1.2 本轮做。
- **O-4 / O-7**：待定，不影响主线实现（其中 O-4 注册表容量与 D13 关联，落地时以 D13 为准）。
- **O-2**：已决（v0.4.15）——不允许隐式 1:1 声明，必须显式声明（§3.6）。
- **O-3**：已决（v0.4.16）——fold 算子仅 MOD，DIV/SHR/AND 与 dim 间线性组合不引入（§3.4 第 5 条/C15）。
- **O-6**：已决（v0.4.17）——属性命名定案保持现状；fold 随所属 period 归组声明（§3.5）。
- **O-5**：已决（编译期检查）。**O-10 / O-11**：本轮不做。
- **O-12 候选**（维度级生命周期）：如需"period 下各 dim 独立激活"另行立项，与 P1 的 dynamic 维度相关。

## 8. 术语对照

| 术语 | 含义 |
|---|---|
| dim（维度） | 绑定到函数参数的身份；由 name/type/spec 三条声明构成；决定特殊化空间 |
| period（时间窗） | 由 1..N 个 dim 组成；实例空间为折叠后元组；使能/失效/版本粒度 |
| fold / 折叠 | dim 值 → period 实例分量的多对一变换（缺省恒等；MOD operand 缺省 = dim spec） |
| 特化折叠 spec_fold（L3） | dim 级声明（`DEFINE_DIM_SPEC_FOLD(dim, enable)`）：该 dim 不参与特化 key 的原始取值；编译期参数不替换、折叠表达式（MOD 形态）替换为折叠常量；operand = 相关 period fold 的 LCM |
| pattern 访问 | 索引成分 ∈ {dim 直用, dim 的 fold 表达式, 常量} 的 period 数组访问；满足才应用常量假设，否则整条访问保守不应用 |
| 实例索引 | 折叠后元组按声明顺序行优先线性化；数组线性下标 == 实例索引（D10 三种形态一致） |
| type | dim 的稳定性分类（half-static / dynamic，预留 static）——应用语义见命题 P1 |
| spec | dim 的值域/缓存尺寸参数：决定 icache 维度与状态表尺寸 |
| impl_spec_p（period 应用规格） | `min(dim_spec, period_fold)`，per-period。运行期：生命周期分量 = dim_v % impl_spec_p（active 检查、版本复核、slot 入库校验、编译触发）；IR/JIT 期：pattern 一致性校验基准（按全局声明的 period 校验，见 §3.7 形态规则） |
| impl_spec_d（版本特化应用规格） | `min(dim_spec, LCM)`，per-dim。specId = dim_v % impl_spec_d（缓存 key、slot 保存位置） |
| icache 格子（快路径单元） | 每函数 [D]^numDims 直接索引数组元素；**存 bucket slot 地址**（v0.4.14 起，不拷贝 fnPtr）——发布时写入一次后不变；deactivate 只刷 slot 即全链失效（§6.4） |
| 实例 instance | period 的一个具体取值组合；对应 period 数组的一个下标 |
| 特殊化 specialization | 针对原始 dim 元组编译出的 JIT 代码版本 |
| 特化参数 specs / specId | wrapper 传给运行时接口的参数（原 dims/instanceId 改名）：`{dimType, specId}`，specId = dim_v % impl_spec_d；决定缓存 key |
| 生命周期参数 periods / periodId / cellIdx | wrapper 传给运行时接口的参数（新增）：`{periodId, cellIdx}`，periodId = 编译器分配的 period 编号，cellIdx = 实例空间线性化编号（u16，见下）；决定 active/版本状态 |
| cellIdx | period 实例的线性化编号（C 行优先，套 B；1:1 现状下退化为该 dim 折叠值）；cellIdx = Σ(分量_i × stride_i)，分量_i = specId % impl_spec_p（数学性质保证 == dim_v % impl_spec_p），stride_i = Π_{j>i} D_j（D_j = impl_spec_p_j，声明顺序，左维最主）；与 D10 数组线性下标、嵌值关卡 L 同一坐标系 |
