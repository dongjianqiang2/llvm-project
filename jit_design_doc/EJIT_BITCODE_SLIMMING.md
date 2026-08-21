# EmbeddedJIT 提取 bitcode 闭包瘦身设计文档

**版本**: 1.0
**日期**: 2026-08-20
**关联**: SPEC4.md, PASS1_EJitRegisterBitcode.md, PASS5_EJitAotModulePass.md, PASS7_EJitRuntime_OrcJITLink.md, EJIT_DIAGNOSTICS.md
**目标**: 在保住特化收益的前提下,显著缩小 PASS1 提取进 `__ejit_bitcode` 的模块体积

---

## 1. 背景与问题

PASS1(`EJitRegisterBitcodePass`)按**传递闭包**提取:从所有 `ejit_entry` 出发,沿 call 图收集全部可达函数与它们引用的全局变量,整包序列化进 bitcode。helper 多的模块,bitcode 体积随闭包线性膨胀。

已有的相关工作:

- **preopt cleanup + 内联**(commit `0ffe768af503`):inliner 之前加 frontend-cleanup round,使小函数/inlinehint helper 内联进 entry,其 standalone 定义被 GlobalDCE 删除——已消化掉一部分闭包
- **`-finline-hint-functions`**(团队即将在场景构建中开启):未标 `inline` 的函数被前端加 `noinline`,两个管线的 inliner 都不内联它们 → preopt 后**幸存者集合变大且由源码标注完全确定**,这是本方案的主要处理对象

关键约束(决定方案形态):

| 约束 | 来源 |
|---|---|
| JIT 管线刻意不跑 inliner,依赖 AOT 时间内联 | EJitOptimizer.cpp `runPipeline` 注释 |
| 模块内 callee 体是特化面:StructFieldPass 折叠 may_const load、IPSCCP 跨调用边传播常量 | EJitOptimizer.cpp Phase 1c/1d |
| spec JITDylib 隔离,不回退主 JD 的进程符号搜索——**所有未定义符号只能靠显式注册解析,host 上 dlsym 也兜不了底** | EJitOrcEngine.cpp:1007-1019(absoluteSymbols define 在 spec JD 上) |
| 提取的克隆模块中,`declare` 不能带 internal 链接(opt 报 `invalid linkage for function declaration`) | 实测 |

## 2. 目标与非目标

**目标**

1. 缩小嵌入 bitcode;规则简单、显式,不堆积启发式
2. 与 `-finline-hint-functions` 的"标注驱动"哲学一致:特化面由用户显式声明

**非目标**

- 不动 JIT 管线(EJitOptimizer 无 inliner 的设计保持)
- 不改运行时 ABI、不改 `ejit_register_symbol` 接口
- 不做 entry-only 提取(保留内联收益与特化面)

## 3. 方案总览:内联后外链化

```
提取:传递闭包照旧全量(不做预先裁剪)
  │
  ├─ preopt(现状,不改):cleanup + AlwaysInliner + O2 module inliner
  │     inlinehint 小函数 → 内联进 entry,GlobalDCE 删定义
  │     noinline / 超阈值函数 → 全部存活
  │
  ├─ 【新增】内联后外链化(extractAndSerialize 内,
  │    diagnostics 之后、internalize 之前 —— 顺序约束:
  │    外链化必须先于 internalize,否则原始 external 链接信息丢失)
  │
  │   对每个非 entry 函数:
  │     函数体 < 阈值(-ejit-externalize-min-insts,默认 16,见 §5)
  │       → 保留定义(现有 internalize 照旧)
  │     否则 → deleteBody() 转 declaration
  │             原 internal 链接 → 改名 ejit_static.<basename>.<hash>.<函数> + ExternalLinkage
  │             (原 external 链接 → 原名 ExternalLinkage)
  │
  └─ 序列化(现有 externalize-globals / internalize 逻辑不变)
```

决策树只有三条分支,全部显式:

| 分支 | 条件 | 结果 |
|---|---|---|
| 内联消化 | inlinehint 且 cost ≤ 阈值 | 体进 entry,standalone 定义消失(已有) |
| 外链 + 注册 | 非 entry 且函数体 ≥ 阈值 | declaration + AOT 侧注册 |
| 单独特化 | 用户标 `ejit_entry` | 保留定义,自带 wrapper/注册/周期配置 |

## 4. 关键设计决策

### 4.1 分类放在内联后

inliner 的 cost 分析就是"哪些函数值得保留函数体"的决策;内联后仍带定义且有调用点存活的函数,天然是外链化候选。**不在提取前重复实现大小/热度启发式**,也不做强制内联(违背 noinline 标注意图,且重新制造 JIT/AOT 分歧)。

### 4.2 无 may_const 豁免

读 may_const 全局的 helper 默认也外链化(正确性不受影响:外链 helper 读活值,entry 侧 guard 保证值与特化常量一致)。需要 helper 内部特化的场景,用户把它标成 `ejit_entry` 做独立特化——现有机制已验证支撑:

- entry 不被 internalize(EJitRegisterBitcode.cpp internalize 步骤跳过 `TAG_EJIT_ENTRY`)
- static 的 entry 也能被 JIT 强制外链(EJitOrcEngine.cpp:903-905)
- 多 entry 闭包并集提取、嵌套特化(E 的 JIT 代码调 AOT 的 H,H 的 wrapper 分派到自己的 JIT 版本)

多 entry 仍共用一份 bitcode，但每次加载 specialization 时只保留当前
entry 的 definition；同一 TU 中的其他 entry 会在 ORC 建立符号声明前转为
declaration，并解析到 PASS1 登记的最终 AOT wrapper 地址。因此 A 的特化上下文
不会顺带改写 B 的 body，调用路径固定为 `A_JIT -> B_wrapper -> B_JIT/AOT`。
这个裁剪必须发生在 `addIRModule` 之前，否则 ORC 的 symbol claim 会与最终
codegen 输出不一致。

local-linkage entry 的自身 bitcode/funcIndex lookup 仍使用源码函数名；PASS1 另外
生成 `ejit_static.<module>.<hash>.<name>` wrapper key，并作为 function attribute
写进 embedded bitcode。只有当该 entry 作为 nested callee 被转成 declaration 时，
ORC 才把声明改成这个唯一 key，避免不同 TU 的同名 static entry 在进程级
`userSymbols` 表中互相覆盖。

### 4.3 注册机制:确定性改名 key

spec JITDylib 隔离(见 §1)意味着**每个外链化的 helper(static 或 external、host 或 bare-metal)都必须显式注册**。而 `userSymbols` 是进程级扁平表(EJitOrcEngine.cpp:106),static 函数名只在模块内唯一——两个 TU 的同名 `static helper` 注册会互相覆盖,静默错绑。

解决:确定性命名,key = `ejit_static.<sanitized 模块 basename>.<原始模块路径的完整 64 位哈希>.<函数名>`。

- 提取侧把克隆模块中的 static helper 改名(只改副本,AOT 本体不动)
- AOT 侧(PASS1 自身的注册发射)对**同闭包 + 同阈值规则**算出的 static helper 生成 `ejit_register_symbol(key, &helper)` 调用与 `.ejit_bitcode` 段条目
- 两个发射点与提取改名共用同一个纯函数 `ejitRegistrationKey(M, F)`,无需跨 pass 传状态
- basename 缩短 key(完整路径会让 key 长达 ~120 字符);哈希负责区分 basename 相同或 sanitize 后碰撞(`a-b.c` vs `a_b.c`)的模块
- 点号在 ELF/JITLink 符号中合法,`mangleAndIntern` 原样处理
- dlsym 误绑不存在(spec JD 隔离,不回退进程符号搜索),改名只为注册表 key 的进程内唯一性

**威胁模型(已知残留)**:哈希区分的是模块路径,不区分同一路径的多次编译变体——同一个 `.c` 以不同 `-D` 编译两次再链接,两个 TU 的 static helper 生成相同 key,运行时 `userSymbols`(std::map,后写覆盖)可能绑错函数体。这是少见构建形态;彻底修复需要每 TU 的身份(如模块内容哈希),留作后续工作。

### 4.4 诊断口径

`runSpecializationDiagnostic` 保持在外链化**之前**运行(现状位置天然满足):"closure" 的 may_const 计数继续按源码级闭包统计,文案不变。

## 5. 数据与阈值

合成场景(entries 调用 static/external、大/小、读/不读 may_const、inlinehint 混合 helper),release clang dump + 真实 bitcode 字节测量(已做重组基线校正):

| 场景 | 模块大小 | 存活 helper | 全部外链化节省 |
|---|---|---|---|
| 合成 + `-finline-hint-functions`(目标世界) | 4128 B | 5 | **520 B(12%)** |
| 合成 无 flag(当前测试世界) | 4536 B | 2(小 helper 被成本内联) | 192 B(4%) |
| `ejit_complex_test.c`(现有真实场景) | 5916 B | 0 | 0 |

每函数 bitcode 体积(外链化净收益):2 指令=36B,3 指令=40B,7 指令=80B,18 指令=152B,26 指令=188B(每函数 ~30B 固定开销 + ~5-7B/指令)。

注册开销估算:~190B/条(AOT 侧 key 字符串 ~50B(basename 后,见 §4.3)+ 地址 8B + 调用代码 ~20B;运行时 `userSymbols` 红黑树节点 + 字符串堆 ~110B)。若 key 用完整模块路径则字符串一项就 ~120B,故 §4.3 采用 basename。

**阈值结论**:严格盈亏平衡点 ≈ 函数体 190B ≈ 20-25 条指令。取 **`getInstructionCount() >= 16` 外链,否则保留定义**——刻意偏激进:注册开销落在 AOT 二进制 rodata 与运行时 RAM,而省下的是嵌入 bitcode(Flash/缓存),本方案的目标资源是后者;两者不是一个账本。该阈值对 static/external、host/bare-metal 统一适用(§4.3:全部需注册)。

## 6. 实现步骤

### 6.1 PASS1 提取侧(`EJitRegisterBitcode.cpp`)

`run()` 中在 `computeTransitiveClosure` 之后计算外链化集合(与注册侧同一规则、同一原始模块,天然一致):

```
ToExternalize = { F ∈ ClosureFuncs : 非 entry 且
                  getInstructionCount() >= EJitExternalizeMinInsts }
```

阈值为 cl::opt(`-ejit-externalize-min-insts`,默认 16,定义在 EJitPassOptions.cpp),无需重编译即可按场景微调;0 表示去掉体积下限(全部外链)。

`extractAndSerialize` 中、`runSpecializationDiagnostic` 之后、internalize 循环之前,应用:

```
for F in ToExternalize:
  Cur = Extracted->getFunction(F 的名字)     // preopt 完全内联掉的已不存在
  Cur->deleteBody()                          // 变 declaration
  Cur->setLinkage(ExternalLinkage)           // declare 不能带 internal(实测 opt 报错)
  Cur->setDSOLocal(false)                    // InternalLinkage 隐含 dso_local,改 linkage 不会清除
  if 原 internal 链接:
    Cur->setName(ejitStaticHelperKey(M.getName(), F->getName()))
```

决策集在 preopt 之前算、变换在 preopt 之后应用:被 preopt 内联消化掉的 helper(GlobalDCE 已删)自动跳过;两条注册路径(ctor + 段表)为它多发的死条目无害。

### 6.2 AOT 侧注册(PASS1 自身的发射机制,非 PASS5)

PASS1 已为闭包外部引用维护两套注册发射(ctor 路径 `generateSymbolRegisters` 发射 `ejit_register_symbol` 调用;bare-metal 路径 `generateRegistryTable` 往 `.ejit_bitcode` 段发射 `EJIT_REG_SYMBOL` 条目,运行时已有消费代码 `tableSymbols → userSymbols`)。外链化 helper 直接接入这两处:

- key = `ejitStaticHelperKey(M.getName(), F->getName())`(internal)或原名(external,进程内唯一)
- 地址取 AOT 原函数 `&F`(顺带使 `--gc-sections` 下函数体存活)
- 无需动 PASS2-PASS5,无需跨 pass 传状态(决策集同源)

### 6.3 测试

- **lit**:`ejit-externalize-helpers.ll` —— 大 static → declaration+改名 / 小 static → 保留定义 / external → declaration / entry 不动;ctor 调用、key 字符串、段表条目均有 CHECK;提取侧与注册侧同文件双前缀校验
- **集成**:`ejit_test/ejit_closure_slim_test.c`(noinline 标注模拟 `-finline-hint-functions` 世界;断言结果与 AOT 语义一致且 `entryCount > 0`,防止注册失败时静默回退 AOT)
- **诊断**:外链化前运行 `runSpecializationDiagnostic`,输出不变
- **回归**:EmbeddedJIT lit 全套 + clang CodeGen/Sema ejit lit 全套

## 7. 开放决策

| 项 | 建议 | 说明 |
|---|---|---|
| 阈值 | `-ejit-externalize-min-insts`(默认 16) | 已做成 cl::opt;严格盈亏平衡 ~20-25 指令(§5),16 是"Flash 优先"的偏激进取值,可按真实场景数据微调 |
| 注册时机 | 挂在现有 AOT 注册函数(全局 ctor 路径) | 与现有 globals/period 注册一致 |
| 未来优化 | 静态段注册(`.ejit_period` 风格 name→addr 表) | 可省去运行时 std::map 开销,进一步降低小函数外链成本 |
