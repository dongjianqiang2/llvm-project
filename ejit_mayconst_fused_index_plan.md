# EJIT may_const 融合下标折叠:路线 C 开发计划(基于 #258)

- **日期**:2026-11-05
- **基线**:PR #258 head `1340589e0e35`(分支 `ejit_multi_hop_may_const`:含 #256 锚定 witness 轻量层、#247 局部 free_dim、#258 多跳机器;已核对其对 #256 的轻量层改动为零)
- **性质**:开发计划(先文档后实施,延续 #251/#254 模式);实现落在 `ejit_mayconst_fused_index` 分支(本 PR),后续 commit 叠加
- **目标**:让 `p + (周期常量×N + free % N)` 这类**融合下标**的 may_const 访问,在指针型 period、区域大小未登记(unknown bounds)时,经轻量 witness 层折叠——**零业务源码、零部署配置、零 clang/ABI/位码改动**

## 1. 背景:七号站点

站点形态(new_form):

```c
extern ejit_period_arr(trp) Struct_Type *g_Struct;   // 指针形态周期全局,区域大小未登记

// entry 参数:dim1 为 ejit_period_arr_ind(trp),ejit_free_dim_n 为 ejit_free_dim
static inline Struct_Type *GetStruct_Type(UINT8 dim1, UINT16 ejit_free_dim_n)
{
    return (g_Struct + (dim1 * MAX_N + (ejit_free_dim_n % MAX_N)));
}
// structVar 的 may_const 字段访问未折叠
```

特化后地址形态:`dim1` 被替换为周期常量 k,`free % MAX_N` 在 witness 求值下按 0——代表地址落在**第 `k*MAX_N` 号非零元素**。

拒绝机理(#256/#258 的 `checkLightweightPointerWitness` → `hasConstantWitnessElement`):非零元素须由"保留的常量 typed 索引或独立常量地址步"选中,两个判据——

1. GEP operand(1) 须为 `ConstantInt` 且等于元素号;
2. 去掉 witness 假设(NoAssumed)后该步偏移须可折成常量。

融合下标 `add(mul(k, MAX_N), urem(free, MAX_N))` 两判据均不过 → **`nonzero-witness-dynamic-element`**。这与 #254 v2("witness 链只认元素 0;`free+K`(K≠0)拒绝")和 #256("dynamic outer index 不授予权限")两级文档一致——**设计内行为,非 bug**。

对照事实:

- 六站点中 witness 类站点的下标全为**纯 witness**(`urem`/`and`),落元素 0,免结构要求,#256/#258 下可折;
- "常量行 + witness 列"先例(如 `@g_test_car[2][urem(%3,5)]`)生在**全局数组**上——大小写在类型里,常量行存在性编译器可证;本站点是**未知大小的裸指针**,免费的存在性证明没了,才引出担保问题;
- #258 补充事实:多跳走链 `resolveIndirectAddress` 的 GEP 偏移**刻意不带 witness 假设**(代码注释:"Declining the assumption is the conservative half of that trade")——链上融合下标(如 `g_struct[dim1][free%MAX_INDEX]->e[x].field1`)同样不折,属后续独立规格项(见 §7)。

## 2. 规格变更声明(随本 PR 拍板)

**接受 `witness + 真常量` 融合下标为合法的非零元素选择**,即修订两条既定决策:

- #254 v2:witness 链"只认元素 0;`free+K` 形状(K≠0)依旧拒绝";
- #256/#258:"A dynamic outer index such as `slot+1` … is not granted a larger readable extent merely because its zero-valued witness evaluates to a positive number."

**家族不可裁剪**:特化后 `mul(k, MAX_N)` 折成普通 `ConstantInt`,与源码字面量 `p[slot+K]` 无从区分——接受融合形态必然同时解锁 `witness + 真常量` 全家族(含源码级 `slot+K`)。

**代价声明**(诚实):

- 休眠标注激活面扩大:此前从不折的 `slot+K` 类标注会真实生效,标注错误从"潜伏无害"变为"生效冻结错值";
- 审计计数验收线变化:板上验收必须做输出对比 + 审计增量核查(预期增量 ≈ 已知站点数,多出则逐个查);
- 承诺错误的爆炸点是**编译 worker(共享基础设施)**,不是业务函数——这正是担保发放需要规格背书的原因。

**安全论证**(为什么可以放开):真常量选元素(NoAssumed 可证,非 witness 推导)+ 每个非常量项 witness 恰为 0 + 锚点检查(根在注册指针首元素内) + 既有部署契约第一条("前缀可读"承诺,已覆盖"选中的 witness 元素"及未走分支)——与已接受的 typed 非零元素扩展(cell1/2/15)是**同一张担保**,只是认领形式从"独立常量步"扩展到"融合下标分解"。值正确性照旧由 `free_dim + may_const` 承诺(跨 free 维同值)覆盖,本变更不新增值层面信任。

## 3. 设计

**新谓词**(工作名 `constantPartWithoutAssumptions`,输入下标表达式,输出常量部分):

- 加法链分解:`ConstantInt` 或在**空假设**(NoAssumed)下经 `evalWithAssumed` 可折 → 常量项,累加;`Add` → 两侧递归,均须成功;**其余节点 → 须在 Assumed 下折为恰 0**(贡献 0),否则整体失败;
- 两个方向都走既有 `evalWithAssumed`:poison 纪律(nuw/nsw/nneg/samesign/trunc)免费继承,不新造推理;深度上限复用 `kMaxAssumedEvalDepth`。

**第三接受路径**(插入 `hasConstantWitnessElement` 循环内,既有两判据之后,作用于"源地址已在注册指针首元素内"的那条 GEP):

- typed 步(`SourceSize == Stride`):分解 operand(1),常量部分(索引单位)== `Element` → 接受;
- byte 步:分解后 `(Address + Const − Base) / Stride == Element` 且 ≥ Base → 接受(镜像既有 check 2 的条件)。

**拒绝面不变**:witness 推导正值(`xor(slot,1)`)、包裹负偏(`slot−1` → `witness-before-pointer-base`)、无锚(`nonzero-witness-unanchored-root`)、extent 溢出照旧;strict(bounds 已解)、legacy 常量路径、显式登记行为**零变化**;纯常量 `p[k]` 不获新权限。

**诊断**:成功仍报 `lightweight-anchored-element-witness`,失败仍报 `nonzero-witness-dynamic-element`——不新增拒绝原因,拒绝面可定位性保持。

## 4. 改动面

| 文件 | 内容 | 估计行数 |
|---|---|---|
| `llvm/lib/ExecutionEngine/EJIT/EJitStructFieldPass.cpp` | 分解谓词 + 第三接受路径 + 注释(纯增量) | ~70–110 |
| `jit_design_doc/EJIT_POINTER_WITNESS_FALLBACK.md` | 选择规则 / 契约 / 代价声明 | ~25–40 |
| `llvm/unittests/ExecutionEngine/EJIT/EJitPointerPeriodTest.cpp` | 正负例矩阵(§5) | ~250–400 |
| (可选,M2)`ejit_test/` 板上用例 | 端到端复核 | ~60–120 |

**零改动**:clang、位码格式、C ABI、注册接口、CMake、编译选项——业务位码复用(#249/#258 clang 已发射 `ejit_pointer_layout`),只重编运行时。

## 5. 测试计划

- **正例矩阵**:融合单 GEP 下标 `add(常量 C, urem(slot, N))`——element 0/1/2/15 × N=5/10 × typed 元素步 / byte(i8)步 × LE/BE IR;**逐元素不同值**(防元素 0 重定向,沿用 #256 cell1 用例的判定法);L2/L3 与 Baseline/Instrumented/PGOUse 做代表性覆盖。
- **负例**:`slot−1`(负偏)、`xor(slot, 1)`(witness 非零叶)、非 free 动态外层参数(未映射)、未标 sibling 保持活、bounds 已解走 strict 不被轻量越权、显式登记不被轻量救援、巨大常量(extent 溢出)。
- **断言**:标注 load 消失且常量值正确;实参、活负载访问、store 仍用真实 slot 值(不被 witness 重定向)。

## 6. 验证计划

- **基线纪律**:#258 分支在改动前先建 EJITTests 全量基线(该分支家族基线未记录过;构建目录存在别名与陈旧产物陷阱,基线必须重编后记录、双侧同建)。
- **构建**:`ninja -C build_release_x86 LLVMEJIT EJITTests`(含 EJitPointerPeriodTest 所属目标);无 clang 改动。
- **改后**:EJitPointerPeriodTest 新例全绿;EJITTests 全量对照基线做排序 diff 失败名单。
- **提交前**:按 CLAUDE.md 约定跑 code-review 子代理。
- **板上(M2,用户执行)**:六站点 + 七号站点输出对比、审计计数增量核查。

## 7. 里程碑与顺序

- **M0**:本计划 PR(docs only,stack 于 #258,未合;#258 合并后 rebase 到 spec5)。
- **M1**:实现 + 单测(本分支后续 commit)。
- **M2**:板上验收(用户执行)。

**后续不在本计划**:多跳走链 + Assumed——`resolveIndirectAddress` 的 GEP 步引入同一分解谓词,补齐 #257 设计中"带 Assumed 逐跳解析"未落进 #258 的部分;路线 C 落地后约 30–60 行 + 测试 ~200 行,属独立规格项,须另行对齐。
