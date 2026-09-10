# EJIT 多版本代码复用方案

更新：2026-09-10。状态：规格与实现跟踪稿；初始PR只有文档，尚未实现、构建或上板验收。

目标分支：`dongjianqiang2/llvm-project:ejit_dev_spec5`。
个人开发分支：`ChenRuan/llvm-project:codex/ejit-spec5-code-reuse`。
本PR保持Draft，后续实现、调测与验收继续追加在同一分支；达到第11节交付条件后
再转为可合入状态。上传规格不代表已授权跳过设计中的正确性检查或开启产品默认配置。

本轮用户确认的修订：同一共享版本采用一套 profile/布局，舍弃每实例PGO调优；
最终代码使用统一 near 池，不再按 cell 划分17池。下文替代上一版“各实例独立
profile后严格比较”和“跨cell借用第一份代码所在池”的方案。
用户进一步确认：每个共享组只选择一个代表cell运行T1并采样，其余成员跳过独立T1。
沿用64次门槛，但仅计代表identity的真实T1进入次数，不是每个cell64次，也不是所有
cell混加64次。以下内容替代上一轮仍保留逐cell采样的建议。
9月10日review后二次确认：共享模式延长borrowed对象借用期至最后一次编译读取
结束或取消确认；64次指真实进入门槛，允许近似快照，不保证64次完整调用；V1仅支持
Async + 正常在线PGO，不支持运行中切换共享策略。详见3.5、5.6-5.7和8.3。

## 1. 目标与结论

保留每个 `(entry, cell, trp, versions)` 的逻辑缓存和生命周期，只减少它们
实际占用的独立 JIT 代码。不使用 PR223 的“所有实例值都相同”作为正确性承诺，
而是对每个逻辑版本分别折叠、优化，再精确判断能否复用。

推荐第一版：同一 entry 的最终 T2 完整编译单元精确去重；保留真实 cell/TRP
参数；运行时取值只用于折叠获准的 may_const load；hash 在 owner 编译冷路径执行。
先比较PGO之前的共同前缀，建立候选profile组；同组使用同一份冻结profile做T2，
最终仍经完整IR精确比较才共用代码。候选组不等于已经证明最终代码相同。
wrapper 仍从原来的 inline-cache 槽取最终函数指针直接调用，不增加跳板、hash、
共享组查询、引用计数或配置比较。

例子：20 个函数各有 6 个 cell，共 120 个逻辑版本。理想情况下同一函数的
6 份最终结果一致，可变成 20 份物理代码，但仍保留 120 个逻辑缓存记录。
这不是承诺必然减少到 20 份：helper、后续暴露的字段差异和外部绑定均可能阻止合并。
不再因每个cell各自产生一套PGO偏好而主动分裂布局；采用共享profile的性能取舍。
大量 may_const 值相同是有利前提，不是精确相同的证明；少数差异仍可能把版本分组。

不做：跨不同 entry 合并、部分代码 outlining、MFS、TTI phase 多版本、
让多个cell同时共享可写T1计数器、代码搬迁和主动回收。PGO admission需要适配为
代表采样任务/组，非代表只是等待成员，不各占一个profiling名额。发布触发策略
不另行重做，但必须接通新的组完成/等待状态，不能依赖不存在的成员T1完成事件。

## 2. 基线与现状证据

本PR已rebase到spec5的 `3f0e190dd54752f9c1dcb4bcd1fcb5d6349ae59c`。
原创建基线为 `1265b885456811ca33b525eac903a4e8be56f0a9`，
它是在 `a6cbc831b2ec9122788b8b522424ce7e7b073486` 上加入209/219移植；
当前基线另外加入223移植，已有提交不重写。此选择继续
保留旧池布局、发布、初始化和PGO调度；没有带入194/201/203/212。
因此统一near分配域是本方案的目标，也与当前spec5方向一致；不能误称该基线已有17池。

PR223的free_dim已独立适配并进入当前spec5基线。它可提供
“只在分析中求地址、只替换load结果”的基础，但其固定见证0/调用方字段同值承诺，
不能替代本方案的真实cell/TRP求值、逻辑生命周期及最终IR精确比较。不要把已有
arr_ind(cell/TRP)改为free_dim以冒充自动合并。实现时在当前基线验证兼容，
不在本PR重复带入另一份223实现。

以下源码行号来自先前对spec4 `6ff70a16739c09265b05345fea5e7ad6107bb21c`
的只读核对，是设计参考，不是spec5精确行号或已迁移功能。实施前必须按实际spec5
head重定位调用链及配置；这份方案不授权合入新spec4主线或其他未选定PR。

当前 `EJitOptimizer.cpp`：

1. `runPipeline:176-200`：`preReplacePeriodIndices` -> InstCombine ->
   StructFieldPass -> IPSCCP -> InstCombine -> 第二次 StructFieldPass。
2. `preReplacePeriodIndices:758-795`：对匹配的 period 参数直接
   `Argument::replaceAllUsesWith(ConstantInt)`，不局限于 may_const load。
3. `runPipeline:210-276`：共同特化前缀后，T1 light-opt + PGO Gen/Lowering，返回。
4. `runPipeline:279-419`：T2 同样 light-opt，再 PGO Use、ICP/inline/value
   specialization（按配置），最后完整优化流水线。
5. `runOptimizationPipeline:889-915`：LowerExpect/Ox/PGO passes 后，第三次
   StructFieldPass，再 cleanup。这次用的是不带 ctx 的重载，构造 Empty context。

当前 `EJitStructFieldPass.cpp` 不只替换 marked load：`:1224-1231` 还会把
未标 may_const 的 period pointer-base load 替成实际地址，以便传播。
新模式必须限制这条路径；只停掉参数 RAUW 不够。

当前 `EJitOrcEngine.cpp:892-918` 在 ORC IRTransformLayer 中跑 optimizer，
此时 `addIRModule` 已创建 materialization claims。`:1128-1146` 按 cacheKey
删除旧 JITDylib，`:1261` 添加新模块。这种“一逻辑版本一资源所有者”的关系
不能原样用于多逻辑版本共享同一份代码。

## 3. Replace 到底替换什么

### 3.1 分开两种用途

- 身份值：cell/TRP 仍用于选缓存、检查 activate、记录版本和失效依赖。
- 程序参数：cell/TRP 仍是业务调用时传入的真实值，不能整体替成某个实例常量。

新模式禁止 EJIT 用编译请求的 cell/TRP 去 RAUW 整个参数，或给该参数加
`assume(cell == 当前实例)`、`range=单值` 等会重新把所有用途常量化的事实。
普通合法 LLVM 常量传播不被禁止；禁止的是把“本次编译实例”泄漏成全函数假设。

### 3.2 使用只用于分析的求值环境

编译 cell=2、trp=1 时，建立 `EvalEnv={root.cell:2, root.trp:1}`，但不改 IR。
遇到获准的 may_const load，沿其地址表达式求值：

```text
IR:      p = &cfg[cell][trp].gain;  v = load p
分析:    cfg 的已注册 base + cell=2/trp=1 的合法字段偏移 -> 读到 7
写回IR:  只把这个 load 的结果替换为类型一致的常量 7
```

普通 load、store 的地址、指针参数、原始 cell/TRP 的计算及 call 实参保持原样。
仅供已消除 load 使用的 GEP/算术可以由后续 DCE 清掉；仍服务于动态访问的部分保留。
未标记的 period pointer-base load 仅可在分析环境中解析，不能把真实基址写回 IR。

求值复用 LLVM DataLayout/APInt/常量折叠工具，按目标类型、位宽、符号扩展、
大小端和地址空间解释。不靠字符串分析 IR，不执行任意业务 call，不读取无边界
的任意指针。只允许注册内存范围或已验证 borrowed-bound 描述符范围内的读。
未知 PHI/select/动态索引、无效偏移、依赖不明、atomic/volatile 均保留 load。

每次折叠同时记录字段所属的生命周期依赖；所依赖的 cell/TRP 必须被该逻辑版本
正确跟踪。不能折叠 TRP 管的数据，却只登记 cell。缺少依赖时跳过该折叠并诊断，
本版不偷偷增加新的缓存维度，也不靠“几个实例目前值碰巧相同”豁免失效管理。

第一版保守支持已能证明合法的标量 may_const 字段；指针值若要折叠，必须保留
其精确地址/符号绑定，地址不同就不能通过去重。无法证明对象和地址语义时跳过。
同VA及受跟踪配置更新的基本要求保留；共享模式的异步借用终点显式延长，见3.5。
不能继续把“原compile callback返回”当成全部后续读取已经结束。

### 3.3 示例

```c
/* mode/gain 是 may_const；live 不是。 */
int A(unsigned cell, unsigned trp, int x) {
  if (cfg[cell][trp].mode == 1)
    return x * cfg[cell][trp].gain + live[cell][trp] + cell;
  return slow(cell, trp, x);
}
```

若某些 cell/TRP 的 mode=1、gain=7，它们分别生成：

```c
int A_shared(unsigned cell, unsigned trp, int x) {
  return x * 7 + live[cell][trp] + cell;
}
```

配置 load 和依赖配置的分支都能消除，动态数据仍读真实 cell/TRP。
gain=9 的版本生成不同 IR，自动形成另一份代码，不要求调用方保证所有 cell 相同。
由 cellIndex 本身决定的分支则保留，这是与旧全参数特化的明确取舍。

### 3.4 Helper 不能漏掉

保留现有独立 ejit_entry 边界：A 调另一个 entry B，仍调用 B 的 wrapper，
不为共享 A 偷偷绑定某个 B(cell0) 的 JIT 地址。

普通内联/内部 helper 的只读求值环境沿可证明的 direct-call 实参传播，
不能仅因其形参也叫 cell 就套用 root.cell。不同实际 cell、多个调用点冲突、
间接调用来源不明时保守拒绝相应折叠。borrowed-bound helper 继续遵守209的
entry、bound 标注和逐调用边维度身份契约，不利用新模式绕过它。
优化后若 helper 内仍留下不同实例的常量地址或常量实参，就应判不相同，不擦除差异。

### 3.5 已确认：跨等待阶段的借用协议

基线的borrowed descriptor只保证到一次compile callback返回；复制rawPtr/size，
或检查generation，均不能凭空延长对象寿命。共享模式新增逻辑请求级借用协议，
覆盖前缀分类、等待代表profile、后续所有replace和最终T2校验读取。

- 调用方保证该期间对象在相同VA有效，用于折叠的字段及地址求值依赖稳定；
  普通动态字段仍可由业务按原规则访问，不因本协议把整个结构体做快照或冻结。
- owner维护与逻辑请求attempt token/生命周期版本绑定的借用状态，见5.8；前缀
  callback返回只结束当前阶段，不代表逻辑借用结束。最终读取完成后清除descriptor并恰好确认一次，
  不必把借用延长到仅剩enable_ex/指针发布的阶段。
- 取消、deactivate、对象更换：先禁止安排后续读取，结束正在进行的读取，再确认
  该旧请求不再读对象。调用方在此确认前不修改相关字段、不释放/替换旧对象。
  仅设置取消标志或发现版本变化不够；等待超时也不等于允许修改/释放。
- 需要可观察的冷路径完成/取消确认机制，包括按受影响的生命周期范围等待旧借用
  结束，避免要求产品追踪wrapper内部所有请求。具体接口名由实现审查确定；
  如新增C API、lipo root或共享字段，必须补齐导出、测试及相应ABI升级。
- 冷代表超时、成员等待超时、queue-full、shutdown按同一协议终结；不能让一个
  冷组无限持有借用。owner无法确认安全结束时明确失败，不能虚构完成确认。

这是用户批准的共享模式使用契约变更，不追溯修改旧模式。对象是全局/shared并不能
替代更新同步；该保证同时适用于延迟阶段再次读取的注册配置。协议建立前，不允许
实现仅把原descriptor塞进waiter、callback返回后再悄悄读取的捷径。稳定T2 wrapper
不增加借用查询、引用计数或完成通知；所有新管理发生在编译/取消冷路径。

“编译借用结束”只证明编译器不再读取该对象，不等于业务对象可以销毁。仍可能
被AOT、在途T1或保留动态访问的T2使用的对象，还必须满足原有执行寿命、配置
失效和业务同步要求。编译借用确认只是更新/释放的必要条件，不替代这些条件。

## 4. Replace 多次执行与新流水线

保留已有多次 replace 的目的：第一轮处理原 load，IPSCCP/inline/展开可能暴露
新的字段读取，后续轮次继续处理。无需把 replace 粗暴地改成只跑一次。

每一轮必须使用同一种“只折叠获准 load”策略，并能重建当前 IR 上的语义映射。
把完整 ctx/依赖环境传入第三轮，不能继续依赖 Empty overload。IR 经 inline、
克隆或删除后不能保留悬空 Instruction/Argument 指针；使相关分析失效并重新构建。
每个真正消除的 load 只统计一次；展开出的多个独立 load 按实际 sites 记账。

每个逻辑版本从当前合法配置建立共同前缀。共享profile要求完整前缀IR、绑定环境
及PGO schema匹配，不只要求LLVM的CFG hash相等；Gen/Use的函数/site对应必须可证明。
逻辑版本配置变化时丢弃它的旧请求并重新分组，不能因cacheKey相近就拿旧profile套用。
原代表成员失效不要求销毁其他仍合法成员使用的冻结profile；它由组管理，不借用
代表成员的临时counter指针，重新加入的成员必须重新做前缀和最终IR校验。

```text
逻辑版本校验 / 解析 bitcode / 隔离其他 entry / 建立解析绑定
  -> 保留 cell/TRP 参数，建立只读求值环境
  -> InstCombine + may_const-load replace
  -> IPSCCP + InstCombine + may_const-load replace
  -> 共同 light-opt
  -> 前缀IR精确比较 + PGO schema校验 -> 候选profile组
       代表: PGO Gen/Lowering -> 远池T1 -> 代表真实进入64次 -> 冻结profile
       非代表: 不编T1、不采样、不占采样名额；暂走AOT并等待组profile
       T2: 各成员使用组profile -> 既有 ICP/inline/value-spec
           -> Ox -> 最后一次 load replace -> cleanup
           -> 最终目标属性确定 / IR verify / 生成规范化比较副本
           -> code key + 精确比较
                命中: 关联已有物理代码及其发布状态
                未中: CodeGen -> JITLink -> 统一near池 -> 原有发布流程
```

不为本功能额外插入 GlobalDCE、MFS 或向量化方案。若所选基线已有这些 IR pass，
比较点位于它们之后。V1 不把新的全局值标注/验证插桩或旧全参数特化模式与共享
模式交叉复用；编译策略必须进入 code key。

## 5. Hash 比较阶段与内容

### 5.1 两个比较点，职责不同

**前置比较点：共同特化/light-opt之后，PGO Gen/Use之前。** 对完整前缀IR、
外部绑定和PGO schema做hash+精确比较，为一致的候选共享版本选择同一份profile。
这一步避免必须先生成各自PGO结果、才能决定该用谁的profile的循环依赖。

**最终复用点：最后一次 replace + cleanup 和全部IR优化之后，机器码生成之前。**
此时比较真实最终产物；只有这里通过才可共用代码地址。前缀一致不证明后续一致，
后续inline/展开/replace仍可能从不同实例读出不同字段值，最终不同时必须拆分代码组。
当前 PGO 的 CFG hash 是 profile 布局标识，不是代码等价性标识，不能挪用。

V1 每个新逻辑版本仍跑 IR 优化，命中时跳过后端 codegen/JITLink/新代码分配。
前置profile组命中不省略最终优化与比较。后续可研究“配置签名命中就跳过全部 IR 优化”，但它需要额外证明完整依赖，
尤其是后续 replace 才出现的字段，第一版不做。

### 5.2 不只比较入口体

比较同一 entry 最终实际发射的完整编译单元：入口、内部 helper、常量初始化器、
函数指针表/alias 关系，以及最终仍存在的定义和声明。直接散列整个最终模块是
保守起点；不要在这里另写一套可能漏掉间接引用的闭包裁剪。

同一入口表面 IR 相同，但 callee 定义、global 对象、函数指针解析地址不同，
不能复用。第一版限定相同源 bitcode 版本、相同 entry，不跨函数做 ICF/outline。

```text
LogicalKey = entry + dimensions + lifecycle versions + runtime generation
ProfileGroupKey = source/entry + policy + bindings + canonical pre-PGO IR
                + validated PGO schema
CodeKey    = entry/source revision + compiler/pipeline/target/ABI policy
           + effective symbol-binding environment + canonical final IR digest
```

CodeKey 不直接加入 cell/TRP 取值或各自生命周期版本号，否则会抵消跨实例去重。
这些仍在 LogicalKey 和发布前校验中；IR 中真实留下的常量、地址绝不能忽略。
CodeKey 包含运行时实例/符号解析环境的代际，禁止跨 shutdown/不兼容重链接误用。

### 5.3 Hash 只是候选索引

建议使用现有 LLVM 哈希工具对规范化内容生成 digest，再精确比较候选的规范化
字节流和符号绑定清单。可以用 SHA-256，但任何 digest 都不能单独作为复用证明。
人为制造 hash 冲突必须落到比较失败并独立编译。

只在比较副本中去掉明确无语义的模块路径、SSA 显示名和调试位置等差异；内部名字
若需要归一化，必须一致地重编号全部引用。不能无差别删除 metadata/attributes。
外部符号身份、解析地址、常量位模式、类型、calling convention、目标 triple/
DataLayout/CPU/features、reloc/code model、alias/TBAA/range/fast-math 等保留。
inline asm、blockaddress、地址有意义的私有全局/函数逃逸等不明场景，V1 拒绝复用。
相同指令 IR 并不自动证明合并两个地址可观察的对象是合法的。
模块私有可写全局、TLS、计数器也不能因 IR 相同就共享；V1 遇到这些独立状态拒绝
复用。普通 AOT 全局若本来就是同一个已注册对象，则按真实符号绑定校验。

LLVM 的 StructuralHash/MergeFunctions 可借鉴哈希筛选和精确比较基础，不能作为
跨 JITDylib、跨编译批次、包含外部绑定的现成完整解决方案。

### 5.4 已选择：共享一套 profile/布局

产品规格已确认：同一共享组不再针对每个cell/TRP单独调优。所有成员使用同一份
冻结profile及同一布局策略，接受它对部分cell不是最优的代价，换取物理代码共享。
这不是删掉!prof或忽略有语义的IR差异；公共profile仍参与真正的PGO优化和后端布局。
前置candidate可以在最终比较时拆分，不能用公共profile强行合并不同配置产生的代码。

**采样来源已确认：只让一个代表cell实际profile，不让所有cell先各采一轮再丢数据。**
每个entry的每个共享候选组选择一个合法、正在活跃的成员作为代表，不强行固定cell0。
代表精确身份包含cell/TRP和生命周期版本；代表cell不是代表CPU核，业务核照常执行。
只为代表生成独立T1和计数器，并累计其真实T1 dispatch 64次，再执行有界快照，
形成不可变组profile。64是进入门槛，不是已完整执行完64次的保证，见5.7。
非代表不编独立T1、不消耗采样配额；profile/T2未就绪时先走AOT，完成最终IR校验后
关联同一个共享T2。它们仍需要各自的编译期字段求值/最终IR校验，不是免验证复用。

记录profile_id/source identity并冻结，不等所有cell完成采样，不随新成员出现而
重选profile。代表样本不是所有cell的流量加权统计，不宣称代表每个cell真实热度。
不同cell的分支偏好不再单独调优；未来重新训练需要显式的新profile代际，不能
原地改写仍被执行的代码，也不因一个成员更新就无条件废掉其他成员的公共profile。

候选组与最终共享组仍分开：若非代表在后续replace中暴露差异、最终IR不等，
它不能拿原组代码。V1让它拆出新组并选自己的代表采样，其他相同成员可继续共用。
记录已失败的候选匹配，避免它反复加入原组、反复生成同样的失败结果。

### 5.5 必需的组级PGO状态适配

这次明确包含以下冷路径状态修改，不能仅换一个profileData指针就算完成：

- 分类后每组只接纳一个代表采样任务，其他成员登记等待关系；并发上限数值沿用
  配置（例如4），单位变成活跃代表采样任务/组，不是等待cell数量。
- 非代表不能因自己没有T1 Ready slot/没有达到hitCount阈值而永远无法入T2。
  由owner在组profile ready时推动其T2校验，不要求再命中一个不存在的成员T1。
- CompileDriver取profile必须支持组profile来源，不能仍只查自己的cacheKey对应
  的tier1Counters，也不能把没有个人T1误判成profile缺失并悄悄退成无PGO编译。
- admission由代表任务/组恰好归还一次；每个成员的dedup、完成、取消另行结算，
  不按组人数重复finish，也不等未安排采样的成员发送T1完成事件。
- 代表冷却、退出或配置失效时，使用有界、可配置的无进展超时；撤销它的新T1
  dispatch资格，可选择另一个合法活跃成员重采。新轮次不混用旧partial counters，
  晚到回调按组/session代际及5.8的request token核对，只结算原请求而不影响新请求；
  NO_RECLAIM下不冒险复用仍可能被旧调用写入的counter内存。
  无合适代表/超过重试预算时释放名额，组保持明确失败或回退状态，不能无限占坑。
- 代表达到阈值后不再授予新的T1 dispatch；已有调用的迟到计数按5.7处理，
  不是宣称计数器已经停止写入。等待发布按既有AOT回退政策。组profile ready、
  T2编译/链接完成与可执行发布是不同状态，不提前写入任何成员的NX函数指针。

组级并发控制、超时选主和完成通知只在冷路径/已有T1采样路径处理，不给稳定T2
wrapper加查组、引用计数或每次调用的profile来源判断。

复用profile必须验证函数/site对应、CFG hash、edge/value-site数量与种类，以及
Gen/Use共同前缀策略。跨T1对象的VP间接调用目标不能直接复制私有函数地址，需按
稳定源符号/PGO身份和已验证目标映射转换；映射失败丢弃对应VP信息并诊断，不造目标。
守卫特化必须保留guard/fallback，不因某个代表cell采到100%就无条件优化其他成员。
无法建立合法组profile时独立处理/走既有失败回退，不能使用不匹配的profile。

### 5.6 原始采样身份与完整ProfileBundle

采样隔离必须从原始记录开始，而不是只给最终profile_id加组号。每轮建立唯一的
`sampling_session_id`，逻辑上包含group_id、profile_epoch和T1实例身份，并绑定
代表LogicalKey/生命周期版本。不得用一个全局“当前组”或CPU核号代替这个身份。

edge计数器的存储和注册、IC/memop/scalar的记录、快照、reset、取消和迟到写入
都必须保留session归属。允许不同具体编码，但需证明同名函数、同site在两个组或
两轮之间不会混合。基线VP的NameRef/kind/site key以及双缓冲generation不能单独
提供这个保证；也不能按函数名reset另一组仍在写的状态。session重用必须受代际和
在途写入保护，哈希碰撞不得被当成另一个session的有效样本。

停止/重选只影响目标session，不通过全局disarm/reset暂停其他组。任何全核快照
即使同时收集多个session，也要保留各自归属并分发，不能消费完一组就丢掉其他组。
旧T1迟到写入只可留在旧session或被明确丢弃，不能污染新代表；未退出的旧writer
不能访问已经清零复用的存储。共享VP blob与taskpool是不同ABI，哪个布局变化就
升级哪个，不能只升级taskpool而漏掉collector兼容校验。

每组持有不可变、完整的`ProfileBundle`，至少包括：

- indexed profile：edge与官方IC/memop profile载荷。
- 独立scalar/loop-bound side table，对应ctx.scalarValueSites；仅复制
  ctx.profileData不能带上这些信息。
- 函数/site schema、PGO名字和CFG hash、site种类/数量、Gen/Use策略，以及
  已验证目标映射；不借用代表的临时vector或未验证的私有函数地址。
- session/profile代际、代表逻辑身份、实际dispatch数、采样时间窗、快照质量，
  以及已开启的may_const等诊断采样数据；非代表不伪装成有个人采样。

缺少合法基础edge/schema时失败，不静默改成无PGO；合法的空VP、未启用的VP
种类、采集不完整而舍弃的VP种类要区分标记。schema/目标映射无法验证的VP数据
不进入优化，所有成员使用相同的已冻结结果和质量标记，不各自补用不同时间的快照。
代表、非代表、后加入成员均从同一个bundle取T2输入。原代表失效不释放其他合法
成员仍使用的bundle；最终IR仍精确比较，不能因bundle相同就免除验证。

### 5.7 已确认：64次真实进入，近似有界快照

V1不要求收齐64次完整函数返回，不新增用于达成这种精确窗口的T1退出跟踪。
最后几次调用可能还在循环或更新计数。collector的writer握手只证明某个payload
半区可安全读取，不证明整个T1调用已返回；单个edge counter原子也不是整体快照。

顺序必须明确：达到64次真实dispatch上限，关闭本session的新dispatch；有界地
收集该session的edge/VP/诊断计数；验证可用性并冻结不可变bundle；再允许组T2。
各组件不要求一个瞬间的全局一致切点，但必须属于同一session并记录实际快照质量。
不能以“允许近似”为理由读正在被无保护重置的payload或混入其他session。

profile里的64表示实际进入数，不能打印成64次完整调用。记录quotaEnd与
freezeCompletedAt两个时点；排名的sample_cycles使用既定的实际dispatch窗口，
不把冻结等待、T2排队或编译时间算进去。缺失shard、舍弃VP种类、迟到写入的
丢弃/隔离策略要可诊断；无法精确计数的丢弃项标明未知，不伪造精确数。

计数可能包含quotaEnd之后的在途工作，而sample_cycles已经截止；二者不能因
各自为真实观测，就自动组成精确的hits/cycle或消除收益。记录各计数组件的观测
边界/质量，无法确定边界时标unknown；派生指标的窗口兼容性与近似标记按7.1处理。

冻结后晚到样本不修改bundle，不进入下一轮；必要的旧存储按NO_RECLAIM/安全
退出规则保留。窗口不完整或未观察到某路径不能作为不可达证明；原分支语义及
所有value specialization的guard/fallback保留。快照无法在预算内安全完成时，
明确失败并有界结算，不让waiter/admission永久等待，也不静默改成无PGO编译。

### 5.8 请求尝试身份与三种完成事件

`LogicalKey`识别逻辑版本，group/profile/session识别组和样本，code_id识别物理
资源；它们都不能代替一次请求尝试的`request_token`。每个新建或取消后重建的
逻辑请求，在建立dedup/借用/waiter等副作用前获得不会与仍可能回调的旧请求混淆
的token。即使LogicalKey、生命周期版本、group和profile_epoch全相同也需新token。
token不得直接使用可复用的对象地址；编码须涵盖owner重启/代际和序号重用边界。

同一次尝试内部的排队/发布重试沿用token，不把一次物理任务重试伪装成新请求；
若尝试已终结或借用已确认结束，重新安排需要对象读取的工作必须建立新的尝试
及借用，不能恢复已结束descriptor。token只走冷路径，不增加稳定wrapper步骤。

所有异步编译完成、取消确认、借用确认、dedup释放和publish waiter操作均匹配
目标request_token及所需逻辑/组代际。旧回调只能完成旧请求尚未结算的资源，
重复回调幂等；不能清理同key的新请求，也不能因为旧请求不再current就漏掉其
仍在等待的借用确认。基线`dedupMark/Clear(funcIndex,generation)`和按funcIndex
finish的粒度不足以直接管理成员尝试；需有token-aware的所有权层，不能只在日志
打印token而仍用旧粒度释放。token需要跨核传输时显式评估队列/共享ABI。

三种事件必须分别建模，不复用一个含糊的finish；下述名称是规格事件而非已有API：

| 事件 | 所属身份与明确迁移 | 恰好一次的动作 |
| --- | --- | --- |
| SamplingFinished | `(group_id, profile_epoch, sampling_session_id)`从Collecting进入BundleFrozen或SamplingAborted；已停止新dispatch并建立迟到写入隔离 | 归还该代表任务实际持有的admission；非代表不归还；不等待T2发布，也不据此释放在途writer存储 |
| CompileBorrowEnded | request_token从BorrowActive进入ReadsClosed：后续读取禁止、当前编译读取结束、descriptor不再可用 | 确认该尝试编译借用结束；成功准备或取消均可到达；不表示业务执行寿命结束 |
| LogicalPublished | request_token对应的待发布映射，经物理代码可执行准备与最新身份校验进入Ready | 结算该逻辑发布及其dedup/waiter，不重复归还组admission；失败保留可重试状态或明确终结，取消不能伪报Published |

请求取消先进入Cancelling，关闭未来读取/发布；借用未安全结束前不能确认完成。
终结时只移除本token实际持有的关系，已结束的事件不重复结算。非代表可以没有
SamplingFinished事件但仍独立完成借用与发布；物理代码命中也不能跳过逻辑结算。
共享CodeRecord失败/重试不允许批量调用旧finish去误释放其他成员或新一轮采样。

## 6. ORC、代码缓存与零新增 wrapper 开销

### 6.1 提前得到最终 IR，不在 transform 中临时跳转

建议将 T2 的准备/优化分成显式 `prepareOptimizedModule` 阶段，在调用
`addIRModule` 之前返回最终 ThreadSafeModule、目标策略、绑定清单和诊断结果。
miss 才将准备好的模块交给 ORC；明确标记已优化，避免 IRTransformLayer 再跑一遍。
代表T1复用原Gen/Lowering/远池编译实现；请求分派和profile来源按组适配，Gen/Use
共同前缀复用同一实现，不维护两套不同副本。
这一调整也避免在 ORC 已 claim 大量符号后，因命中共享代码而留下未完成 claim。

### 6.2 一个物理资源，多个逻辑引用

owner 私有维护 `CodeRecord`：code-id、digest/精确比较材料、entry 地址、全部
exec/data ranges、实际 pool、ORC 资源所有者、pending/published/failed 状态。
它关联冻结的profile_id/代际；候选ProfileGroup另外保存前缀/schema和样本来源。
多个 LogicalKey 关联同一 CodeRecord。对同一 entry 的重复版本直接返回共享地址，
不生成每版本转跳函数，不重复 addIRModule/link 一份相同 object。

hash 命中且已 published：再次检查request_token与当前逻辑版本/代际，owner可
发布该逻辑cache的Ready及真实执行范围，按5.8结算，但不再分配或封同一物理页。
Ready不授权owner替未准备的producer填wrapper icache；首次执行/填槽按6.4处理。
hash 命中但尚 RW/NX：以request_token作为该CodeRecord的独立waiter身份，
不能提前把地址交给wrapper。实际封页成功后逐个校验token和版本，合法者发布，
失效者仅结算自己的尝试，不能按LogicalKey删除一个后来重建的waiter。
失败、重试、queue-full、取消按每个逻辑请求准确结算；代表采样admission按组结算
一次，不能被每个复用成员各归还一次。

不改变原有发布触发条件，不另建等待所有 cell 聚齐的屏障，不重做212。
单 owner 串行路径下也应显式维护 Building/LinkedPending/Published/Failed 状态，
避免未来回调/重入时拿到半成品。

### 6.3 失效与生命周期

cell0 变配置：清它自己的逻辑映射/icache、重新读配置生成结果；若新结果与已有组
相同可再关联，否则生成新 CodeRecord。cell1 仍可运行旧共享代码。
不能在 cell0 重编时按原 cacheKey 删除其他 cell 仍使用的 JITDylib。

第一版沿用 NO_RECLAIM，物理代码资源活到既有安全 engine teardown；不因最后一个
逻辑引用消失而立刻重写或释放 RX 页。引用为0不代表所有核已经退出函数。
精确比较材料设置字节预算，不破坏仍可执行代码。不长期保留每个版本完整
LLVMContext；耗尽时按6.5处理，不能把“停止去重后继续无限独立编译”当成内存回退。

### 6.4 性能与权限边界

稳定 wrapper 的目标机器码必须与当前一致：原始参数 -> 原表槽 load -> 分支调用。
不加运行时 hash、shared-id 解引用、trampoline、配置 guard 或 per-call refcount。
hash/比较/引用维护仅编译、失效和发布冷路径承担。

这只保证 wrapper 没有新增开销，**不保证函数体零成本**：原本可因 cell/TRP
常量化而消掉的地址计算/普通参数分支会保留。要比较其代价与指令工作集下降的收益。

共享地址携带真实执行区间。必须沿用并验证正确的跨核权限准备/缓存同步契约。
若 enable_ex 仅影响本核，而全局 icache 槽能让未准备的核直接跳进来，则需在发布
前完成相关核的执行权限准备，或采用已支持的核私有填表机制。不能用“表里已经有
指针”替代权限证明，也不能同时无条件承诺任何平台下都安全且无需命中检查。
本方案不通过随意替换 TLBI 指令或添加 wrapper 慢判断掩盖这个前置条件。

V1的首次冷路径必须写清：owner只发布逻辑Ready、code_id及真实exec/data范围，
不以“非代表跳过T1”为理由直接回填所有成员槽。producer首次miss/resolve读取
这些信息，执行现有本核权限/缓存准备，随后重新校验身份、生命周期代际和实际
code/ranges；准备失败则干净回退，不交出未准备的指针。基线peerPrepareSlot的
先释放bucket锁、准备、再取锁重验流程应保留；等待中的操作另按token避免串请求。

用户已确认同一完整逻辑身份可能跨核或迁移，并要求沿用spec4既有切换机制。
本功能不以代码共享为由重做权限发布，不预设必须新增全核握手、执行域API或
核私有表。稳定命中仍读原逻辑槽并直接调用；多个槽可填同一物理地址，不能因此
改为每次先查CodeRecord。首次miss、失效重填、核切换需要的准备沿用已验证路径，
新增工作只限于共享代码真实范围/归属适配及避免owner越过原路径批量填槽。

源码核对边界（spec4@6ff70a16）：resolveMatchedSlot以executableCoreMask记忆
本核已准备状态，缺失时调用peerPrepareSlot并重验；但wrapper直接命中不读这个
mask。icacheFill对非零维度仍写着各核使用不同维度身份的部署假设（约713-735行），
0维则按平台跨核可执行能力门禁。因此不能仅凭taskpool存在本核准备就断言任意
全局槽都支持同身份迁移，也不能把这个既有假设直接认定为版本合并引入的新bug。

实施前核对实际产品表映射、权限/缓存同步和迁移失效流程，证明现有机制适用于
同身份跨核；已有平台全域保证或实际核私有映射可直接沿用，不重复另造协调。
PR基于spec5，必要的spec4适配必须逐项列明，不能声称仅因spec4具备就已带入。
若真实存在未准备核可绕过miss的缺口，单独列为基线/平台问题并与产品对齐修法，
不能忽略风险，也不能未经确认把全核屏障或命中检查塞进本PR。

验收保留同身份迁移、未执行T1的非代表首次使用、准备失败和权限代际变化，
核对共享前后wrapper指令和已有冷路径行为。准备失败不交出未准备指针、不假报
快速可执行；现有同步保证的适用范围仍需有证据，不能只凭裸fn地址或成功日志。

### 6.5 共享状态的统一容量和耗尽行为

初始化时为每entry/全局组数、等待成员数、前缀和精确比较材料、冻结profile与
scalar side table、失败匹配记录设置有限的数量/字节预算；跟踪T1和T2实际代码
池用量及NO_RECLAIM下已退休但尚占用的资源。具体数值结合目标内存测量冻结，
不能仅限制hash表而放任profile、waiter或重选资源无限增长。

资源预留失败要在建立新的等待关系/admission前拒绝；中途失败恰好回滚一次。
已有合法Published成员继续运行，新请求在没有预算时明确回退AOT并计数；不能
悄悄独立编译更多代码，也不能每次miss都重复申请、编译、重采。重试受进展事件、
退避和总预算约束，冷代表重选同时限制次数及新T1代码资源。

可淘汰的比较索引/失败匹配材料与必须保留的执行资源分开管理。活跃bundle、
未完成借用、在途writer和NO_RECLAIM代码不能为满足表预算而提前释放；移除一个
索引不清除仍有效成员的映射。失败匹配记录淘汰后仍有全局/组级重试预算，不能
重新开启无限“入组-拆组-重采”循环。池耗尽后停止新物理编译并明确AOT回退，
保留现有可执行代码；诊断显示拒绝原因、预算上限/占用/峰值和不可回收字节。

## 7. Codepool 关系与诊断

已确认采用**统一near池**：所有最终T2代码组都从同一near分配域分配，不再按cell
划分16+public，也不保留跨cell引用首个cell私有池的方案。容量和链接脚本按实际
共享代码总量配置，不能把旧17池方案中的4MiB public直接视作已足够的统一池。
“统一池”是逻辑分配域，不要求只有一个物理slab；底层按现有容量规则管理。

每个函数理想上只剩一份T2，但不同函数、不同配置/绑定产生的代码组仍可并存。
统一池不等于强行全局只留一个版本。暂时保留临时T1远池与最终T2 near池的隔离；
本次不把带可写profile counters的T1混入统一near RX页，也不修改seal/W^X规则。

诊断至少区分：logical_versions、unique_code_objects、reused_versions、
logical_exec_bytes、unique_exec_bytes、code_id、actual_pool、published/pending。
print_compiled 每个逻辑版本可显示相同 fn 地址及 code_id，地址排序不变。
codepool stats 按物理 range 去重；不能逐 cell 重复累加共享机器码。
ranking区分真实独立采样与组profile来源；共享字节归属单独展示，不伪造负载消除次数。
若成员没有实际采样数据则明确显示未采样，不能复制代表的hits/cycles冒充它的测量。

### 7.1 调测适配是交付内容，不是后续补丁

用户明确指出版本合并后调测改动规模较大。实现开始前先冻结下表语义；代码复用
与诊断一起验收，不以“能运行了，调测之后再改”视为完成。

| 接口/视图 | 必须回答的问题 | 适配要求 |
| --- | --- | --- |
| print_compiled | 哪个函数、cell/TRP、生命周期版本对应哪份代码？ | 逻辑视图保留每版本行，补code_id/实际地址/pool/发布状态；另提供物理组汇总及成员，不把相同地址误报为异常 |
| code_pool_stats | 实际占了多少代码/池空间？ | 同一物理range只计一次；区分exec union、bump used、pending、padding和NO_RECLAIM保留空间，不把fn_size当整个allocation |
| taskpool_stats/PGO进度 | 完成了几个逻辑版本，真正生成了几份机器码？ | 分开逻辑请求完成、物理codegen/link、reuse命中；显示代表profile进度0/64、组代际、等待成员数；非代表无独立采样配额，admission每组结算一次 |
| dump/print_dumped/module dump | 这个cell当前执行的到底是哪份IR/ASM？ | 通过逻辑identity解析到code_id；复用命中虽未codegen也能指向已有代码的保存视图；无保存内容明确提示，不能返回别的cell旧dump |
| may_const ranking/density | 实际采样收益和公共profile分别来自谁，真实代码代价多少？ | 保留hits/sites/cycles及scope/profile_id/source、观测窗口/质量；未采样成员不复制代表数据；site映射或窗口未证明兼容时不混算精确收益；物理range去重 |
| inline-cache/lifecycle诊断 | 哪些槽指向共享体，更新后谁仍在使用？ | 多槽同地址合法；打印identity/code_id/当前状态；不为补“执行次数”在wrapper热路径增加计数 |

不要让每个打印函数各自遍历、猜测、累加一套共享关系。owner 侧提供有界的统一
诊断快照，包含逻辑版本、物理代码和关联边；各接口只做筛选/格式化。
peer 通过既有owner请求模式读取，不直接访问owner私有容器；刷新有超时、代际检查、
失败/不完整标记。快照不是为打印而停止业务的强一致全局事务，不能把读取失败显示成0。

旧接口的参数和明确已有字段语义尽量保留；需要版本选择/分组等能力时用明确扩展
接口或新版本结构，不把旧“版本数”字段悄悄改成“代码份数”。保留旧dump默认选择
语义并在输出注明实际identity；精确cell/TRP查询另给selector，避免按名字猜版本。
新增外部C入口同步lipo roots、文档、C ABI和头文件无依赖板端原型，不能只改打印代码。

近似采样的派生排名另有硬约束：原始hits、dispatch-window cycles、quotaEnd、
freezeCompletedAt及可获得的计数组件边界分别保留。`removed_hits/sample_cycles`
及再除物理cacheline的density，只有在site归属与统计窗口均可证明兼容时才能作
同口径比较；相同64配额或同一bundle本身不是窗口兼容证明。无法证明时展示
approximate/不可比较及原因；可选数值必须明确标为estimate，不能混入默认的
精确收益降序榜、给出确定性能变化结论，或用0代替未知值。即使窗口兼容，指标
仍是消除工作量代理，不等于实测吞吐或时延收益，不伪造非代表的运行频率。

不能为取得看似同窗的比值，把sample_cycles悄悄延长至freezeCompletedAt，或
撤销已批准的近似64次语义。精确排序无法提供时明确显示不可用，原始数据仍可查；
需要新增计数/同步机制才能取得精确窗口时，另行对齐开销，不能加进稳定T2路径。

示意输出仅为设计，不是已有接口或实测结果：

```text
entry=A logical_versions=6 unique_code_objects=1 reused_versions=5
cell=0 trp=0 code_id=42 fn=0x... publish=published
cell=1 trp=0 code_id=42 fn=0x... publish=published
code_id=42 members=6 exec_bytes=1024
```

验收至少覆盖同一时点的多接口交叉核对：6版本共用1份；1版本配置变化后变2份；
pending到published；取消一个waiter；共享首个成员失效而其余继续运行；dump未开启
或候选索引被淘汰时的准确提示。诊断快照/格式化不进入稳定wrapper命中路径。

## 8. 需要对齐的规格

| 项目 | 建议规格 | 是否变化 |
| --- | --- | --- |
| 启用范围 | 默认 OFF，显式开启共享模式；V1 仅同 entry 的最终 T2 | 新能力 |
| cell/TRP | 仍占原缓存维度、由原生命周期管理；不再全参数常量化 | 优化语义变化 |
| may_const | 只对获准且证明合法的 load 取运行时常量；其他访问不借此改写 | 收紧替换范围 |
| 配置差异 | 不要求所有实例相同；不同结果自动分组，无法证明则独立编译 | 无新增字段同值承诺 |
| T1/采样/发布 | 每组一个代表，64次真实进入后做近似有界快照；其他成员AOT等待；发布触发不另改 | 用户已确认，不是64次完整返回 |
| borrowed请求 | 编译借用覆盖等待及后续读取；最终读取结束/取消确认只是更新释放的必要条件，仍遵守业务对象寿命及配置失效同步 | 用户已确认延长，仅共享模式 |
| 请求尝试 | 每次新尝试独立token；采样归还、借用结束与逻辑发布分开且幂等 | 新模式冷路径所有权约束，不能套用旧函数级finish |
| 跨核快填 | 同一身份允许跨核/迁移；沿用已验证spec4/产品机制并适配共享代码范围 | 不预设新增全核协调；基线前提需核对，稳定wrapper不加判断 |
| 排名窗口 | 未证明hits/cycles窗口兼容时只展示近似/不可比较，不进入精确收益排名 | 近似采样下的诊断约束，不额外跟踪T1完整返回 |
| 函数指针 | 同地址只表示共用物理代码；逻辑身份仍由entry/cell/TRP/版本判断 | 调测不能靠地址区分版本 |
| 公共 C ABI | 尽量不改业务签名与 wrapper ABI；由编译策略开关控制 | 需重编EJIT及一致配置 |
| 共享 state | hash/profile/IR管理表由编译worker独占管理，业务核仍读原cache；只有共享内存布局变化才升级ABI | 不是内部map一变就升级 |
| 代码池 | 所有最终T2使用统一near分配域，不再分cell；T1远池暂保留 | 用户已确认统一池方向 |
| PGO 策略 | 同候选组使用固定公共profile，舍弃每实例调优；最终IR仍精确比较才共享代码 | 用户已确认，不靠删除metadata合并 |
| MFS/outline | 第一版不启用/不集成共享体拆分 | 本轮排除 |

若需要每个 entry 独立启用，可后续增加前端 attribute；V1 优先使用明确、默认关闭
的 owner 编译策略，避免为试验再改 WrapperGen。若引入 attribute 则必须重编Clang
和业务 bitcode。具体选项名字不是既有接口，本稿不把草案名称说成已实现规格。

### 8.1 “不能靠JIT指针判断逻辑版本”的含义

```text
A / cell0 / trp0 / version3 -> code42 -> fn=0x1000
A / cell1 / trp0 / version8 -> code42 -> fn=0x1000
```

两条记录地址相同，是共用同一份机器码，不代表cell0和cell1变成同一个配置身份。
业务照常传各自cell，调用没有新增步骤。cell0关闭/更新不能顺手使cell1失效。
因此调测、失效和关联管理用逻辑key识别版本，用code_id/地址识别物理代码。
这句话不是禁止比较指针，也不是让业务新增判断，只是说明指针相等能证明什么。

### 8.2 “owner私有”和“共享ABI”的含义

owner是管理编译和发布的worker（标准用例core6），peer是其他业务/调测核。
“去重表私有”指IR/hash/profile组/CodeRecord管理容器只由该worker操作，其他核
不直接读写这个容器；不是把JIT代码放到只有core6能执行的内存。代码仍在可跨核
执行的统一near区域，业务核仍从原共享cache/icache拿最终函数指针。

ABI在这里指各核共同解释的共享内存结构布局：字段的偏移、宽度和排列。
如果为peer读取新增code_id/组统计等共享字段，旧镜像可能按旧偏移误读，必须升级
布局版本并让所有相关核使用匹配构建，attach时拒绝不兼容镜像。
若仅调整owner内部容器，或由owner经已有命令通道打印结果而不改共享结构，
就不需要因此升级共享ABI。同一构建中C++组件的一致性要求仍然存在。
它不意味着新增wrapper指令，也不等于改变业务函数签名或切换CPU大小端模式。

### 8.3 已确认的V1支持矩阵

| 组合 | V1行为 |
| --- | --- |
| 共享OFF | 保持对应基线模式，不建立共享组 |
| 共享ON + Async + 正常在线PGO | 支持本规格 |
| 上述正常PGO同时开启日志/排名等诊断 | 支持，不等同于audit-only；必须按组/代表正确归属 |
| 共享ON + PGO关闭 | 初始化/配置验证明确拒绝，不静默禁用PGO或进入等待组 |
| 共享ON + Sync | 明确拒绝，不建立无T1进展来源的组 |
| 共享ON + audit-only | 明确拒绝，不把诊断采样当成正常PGO组生命周期 |
| 运行中切换共享策略，或把活跃共享模式切成上述不支持组合 | 拒绝并保留原状态，不原地迁移在途组 |

检查必须早于queue/waiter/admission副作用，错误通过明确接口结果/诊断反馈；
不能靠CodeKey分开就假设每种模式都能推进。改变策略需在安全shutdown/drain后
重新初始化。VP种类因构建选项未启用可在正常PGO中明确标记disabled，不等于
整个PGO关闭；不允许以此隐藏schema不匹配或有样本却串组的错误。

## 9. 实施阶段与验收

1. **只改折叠策略，不复用代码**：完成 preserved-dim EvalEnv 与三轮 replace；
   对照旧模式确认动态参数/写入/调用不被错误冻结，PGO Gen/Use 对应正确。
2. **先验证公共profile和重复审计**：建立前缀candidate/schema、固定代表样本和
   最终IR比较，仍各自编译。验证公共profile没有非法VP地址/布局映射；输出分组、
   拒绝原因和预计节省；每组仅代表运行T1/64次，其他成员跳过，配套组级等待和超时。
3. **接物理资源复用**：拆准备与CodeGen、共享CodeRecord、逻辑发布waiter和
   失效关系；保持原wrapper和原封页时机，完整覆盖失败路径。同时完成第7.1节
   统一诊断视图、接口兼容和跨接口核对，不把调测留到发布后补。
4. **目标验证后上板**：AArch64 BE artifacts、真实ORC/JITLink、跨核执行/权限、
   单C用例和产品对比通过后再开启实验构建；每阶段可单独回退。

阶段2的组级调度前先落实5.8的token/完成事件；阶段3的共享发布前先验证6.4
已确认跨核部署所需的平台权限/确认协议。阶段1可独立验证preserved-dim折叠，
通过该阶段不等于后续组状态机或跨核发布已经验证。

核心用例与负例：

- 同 entry 6cell/多个TRP，配置相同但 live 数据各异：真实T2结果正确、写到正确
  cell/TRP；预期同函数共享地址。配置不同、只差一个影响结果的 may_const 时必须分组。
- 6cell x 20函数：保留120逻辑版本，统计真实独立代码和页数，而非只看函数地址。
  加1cell异值、再变回；持续喂全体 identities，不能按4个采样波次阻塞用例。
- 内联helper/未内联helper/另一个entry wrapper、同.c函数指针表、alias/global
  绑定、borrowed结构体：外部绑定不同或可观察私有对象不应误共享。
- CFG/值不同但强制hash相同；NaN/有符号零/不同位宽常量；原始参数分支和动态store。
- 相同前缀的cell产生不同真实热度时，仍按同一组profile生成T2；明确记录代表，
  不伪造各cell采样数据。PGO schema不兼容不可共用；前缀相同但第三轮折叠不同
  必须拆最终代码组。代表失效/冷代表/后加入成员/VP地址映射均有回归。
- 同组6cell只1份代表T1、代表真实64次，而不是6份T1/384次，也不是混合cell凑64次；
  非代表在profile ready前走AOT，之后无需个人T1计数即可完成T2校验及共享发布。
  冷代表只调用一次、代表失效后重选、迟到回调、admission重复完成都必须有回归。
- 公共profile的guard/fallback在非代表cell上必须语义正确；代表样本不能当作所有
  cell的实际采样统计。非支持模式按8.3拒绝，不交叉命中、不留下等待组。
- 同entry的两个配置组代表并发：同名IC/memop/scalar站点不混样本，清理一组不
  清另一组；重选后旧T1迟到记录不能进入新轮次，包括全核快照和reset路径。
- scalar/loop-bound热值：代表、非代表、后加入成员拿到同一完整bundle，含
  side table/schema/目标映射，最终IR不因漏传scalar载荷而无故拆组。
- 第64次进入后故意暂停T1、制造迟到计数和缺失shard：快照有界、安全且明确
  approximate，不误报64次完整调用，不把冻结/排队时间算成采样执行时间。
- 固定相同64次dispatch，只改变最后一次长调用的暂停/恢复和快照延迟：显示
  计数窗口/质量差异，窗口不兼容时不把比值变化报告成确定收益变化或精确排名。
- borrowed非代表长时间等待期间cancel/deactivate/更换对象：取消确认前禁止
  更新，确认后编译器不再读取旧对象；业务AOT/T1/T2的对象寿命另行保持，所有
  借用/dedup/admission终结不重复、不遗漏。
- R1取消后以相同LogicalKey、版本、group/profile epoch建立R2；注入R1的旧编译、
  取消、借用及发布回调，R2的waiter/dedup/借用不受影响；R1仍需安全结算自己的
  旧资源，不能泄漏。重复回调、owner重启和token复用边界也有负例。
- 小预算持续入组/取消/重选/配置变化，验证元数据增长有界、T1重选受预算限制，
  NO_RECLAIM池耗尽后停止新编译并AOT回退；已有合法代码和waiter不被错误释放。
- 复用已发布代码、复用pending代码、单个waiter失效、发布失败重试、generation
  变化、共享代码首个逻辑拥有者更新：不暴露NX、不误释放、不漏PGO完成/取消。
- wrapper ON/OFF 大端反汇编逐条比较，不出现新hash/跳板/额外load；所有执行核
  实际运行共用T2并返回，不能以AOT正确结果或owner单核权限代替。
- 未执行个人T1的非代表核，owner先完成复用Ready发布，其首次调用仍完成本核
  所需准备和重验，或复用已有有效的平台准备证明；准备失败不填槽。同一逻辑
  身份再由另一核调用（含0维），必须按现有已验证迁移机制安全执行，不能仅因
  首核成功就假定次核准备完毕。覆盖准备失败/超时和权限代际变化，恢复有界，
  共享前后稳定wrapper机器码与已有切换冷路径行为对照验证。

测量拆四组：旧全参数特化；保留参数+各自profile不去重；保留参数+公共profile
但不去重；公共profile+最终代码去重。分别拆出动态参数、PGO偏好和物理共享的影响。
分别观察may_const消除、动态参数新增工作、IR优化/hash/codegen时间、unique exec
bytes、L1I/L2I misses、ITLB、前端/后端停滞、吞吐和p99。命中率下降预期需要实测，
不能把“代码缩小”直接换算成固定性能提升。

板端用例单header-free `.c`，worker6/producer16；产品管理init-array，打印接口
只读、重复命令不重新init，先有真实T2执行证据再请求产品验收。

## 10. 参考

以下主线链接为本次事实依据，实施前需按最终选定spec5 head重定位：

- [当前replace及PGO流水线](https://github.com/dongjianqiang2/llvm-project/blob/6ff70a16739c09265b05345fea5e7ad6107bb21c/llvm/lib/ExecutionEngine/EJIT/EJitOptimizer.cpp#L176)
- [整体参数替换](https://github.com/dongjianqiang2/llvm-project/blob/6ff70a16739c09265b05345fea5e7ad6107bb21c/llvm/lib/ExecutionEngine/EJIT/EJitOptimizer.cpp#L758)
- [最后一轮replace的空context](https://github.com/dongjianqiang2/llvm-project/blob/6ff70a16739c09265b05345fea5e7ad6107bb21c/llvm/lib/ExecutionEngine/EJIT/EJitOptimizer.cpp#L889)
- [额外的pointer-base替换](https://github.com/dongjianqiang2/llvm-project/blob/6ff70a16739c09265b05345fea5e7ad6107bb21c/llvm/lib/ExecutionEngine/EJIT/EJitStructFieldPass.cpp#L1224)
- [ORC transform位置](https://github.com/dongjianqiang2/llvm-project/blob/6ff70a16739c09265b05345fea5e7ad6107bb21c/llvm/lib/ExecutionEngine/EJIT/EJitOrcEngine.cpp#L889)
- [现有每版本JITDylib生命周期](https://github.com/dongjianqiang2/llvm-project/blob/6ff70a16739c09265b05345fea5e7ad6107bb21c/llvm/lib/ExecutionEngine/EJIT/EJitOrcEngine.cpp#L1128)
- [LLVM MergeFunctions：哈希筛选后比较](https://llvm.org/doxygen/MergeFunctions_8cpp_source.html)
- [LLVM StructuralHash接口范围](https://llvm.org/doxygen/IR_2StructuralHash_8h_source.html)
- [LLVM ORCv2资源与materialization](https://llvm.org/docs/ORCv2.html)

上游文档解释机制，不能替代本仓库/本目标验证。本稿无实现、构建、上板或性能结论。

## 11. 本PR交付跟踪

本PR作为规格、实现和验收的统一入口。初始仅提交本文；功能代码继续追加到同一
个人分支，不为“规格已上传”提前合并一个未完成的功能PR。功能较多，本PR保留
少量完整的功能提交，不要求整体squash为一个提交，也不拆成大量零碎提交。
每个功能提交带上对应测试和说明，并尽量保持可独立构建、review；调试、fixup和
格式修正于最终整理时归回所属功能提交，跨模块验收可独立保留。提交边界按实际
依赖确定，不硬定数量，不重写spec5已有的209/219等历史。

- [x] 提交规格：保留cell/TRP、仅折叠may_const load、最终IR精确去重、
  一个代表cell采样、共享profile/布局、统一最终near池、稳定wrapper无新增步骤。
- [ ] 完成spec5调用链审计和223基础适配边界；记录实现时的精确base与策略开关。
- [ ] 完成三轮replace的只读求值环境和生命周期依赖验证，保留动态LD/ST/call实参。
- [ ] 完成前缀候选组、代表T1真实64次采样、非代表等待/推进、超时和失败结算。
- [ ] 完成3.5借用完成/取消确认协议、5.6全种类session隔离及完整ProfileBundle，
  以及5.7近似快照质量诊断，不能只改最终profile_id。
- [ ] 完成5.8请求attempt token、三种独立完成事件及旧回调不污染同key新请求回归。
- [ ] 完成最终IR规范化/hash/精确比较，包含外部绑定、完整发射单元和冲突负例。
- [ ] 完成ORC共享资源、逻辑版本映射、统一near池及安全发布/失效/NO_RECLAIM处理。
- [ ] 完成第7.1节全部调测语义和兼容性，不把逻辑数量、物理字节和代表样本混算。
- [ ] 确认并验证6.4跨核首次填槽契约；验证7.1不兼容窗口不伪报精确派生排名。
- [ ] 完成6.5全状态预算/耗尽回退与8.3模式门禁的负例，不产生永久等待或忙重试。
- [ ] 完成真实PGO/ORC回归、AArch64大端产物/依赖检查，以及wrapper无新增开销证据。
- [ ] 提供单header-free C、worker6/producer16的6cell x20函数标准用例及异值/更新
  负例；不重复init-array，不串行等待PGO波次，以真实T2执行证明成功。
- [ ] 完成产品侧正确性及性能验收，记录未决限制，更新规格/操作说明后解除Draft。

“已合并代码”的测试、对当前规格的模拟或host通过，均不能替代最后的目标产物和
产品验收。某一阶段暂缓时保持对应项未完成，并保留清楚的基线与证据。
