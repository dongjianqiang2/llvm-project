# EmbeddedJIT 实现整理：period/dim 标记、wrapper 生成与 JIT 结果访问/保存

> 增量需求（复合 period / 实例折叠的标记方案讨论稿）见 [EJIT_PERIOD_COMPOSITE_REQ.md](EJIT_PERIOD_COMPOSITE_REQ.md)。
> 代码基线：branch `ejit_dev_spec4` @ `52040abd0c75`（含 registry 枚举化、icache dim 一致性检查等近期合入）。
> 本文按四个实现设计维度整理：**① period/dim 标记** ② **entry 函数 wrapper 生成** ③ **wrapper 如何访问 JIT 编译结果** ④ **JIT 结果如何保存与更新**。
> 引用路径均以 repo 根（llvm-project/）为基。

## 0. 端到端总览

```
源码属性                       clang                          AOT Pass (late pipeline)
──────────────────────────────────────────────────────────────────────────────────────
ejit_entry(F)        ─┐
ejit_dim("cell")(p)  ─┤ Sema 校验 ──► CodeGen ──► !ejit.metadata  ──► PASS1 提取 F 的 bitcode → .ejit_bitcode / ctor
ejit_period_arr(V)   ─┤ (SemaEJIT)   (CGEJIT)    (MDNode 子节点)      PASS2 period 数组/静态变量 → .ejit_period / ctor
ejit_period_lc(F)    ─┤                                              PASS3 wrapper + lifecycle/funcindex 槽位 → .ejit_period / ctor
ejit_may_const(field)─┘                !ejit.may_const(逐 load)        PASS4 activate/deactivate 插桩
                                                                       └─► ejit_auto_register 构造器 (priority 65535)
                                                                                  │
                                      ┌───────────────────────────────────────────┘
                                      ▼
运行时 ejit_init： 消费 ctor 数据 或 遍历 __start/__stop_ejit_* 段
  ├─ EJitFuncRegistry::resolveAssign("F") → funcIndex，写回 @__ejit_funcidx_F
  ├─ EJitLifecycleRegistry::resolveAssign("cell") → dimType，写回 @__ejit_dimtype_cell
  ├─ PeriodArrayRegistry::registerArray("cell","V",base,size)
  └─ icache 槽位注册（keyed by funcIndex）

调用期： wrapper ──► icache 快路径 ──► L0 ──► bucket cache（版本复核）
                        │miss              │miss   │miss
                        ▼                  ▼       ▼
                  taskpool compileOrGet ──► 同步/异步编译（EJitCompileDriver → ORC → CodePool）
                                              └─► publish：写锁 + 版本复核（提交门控）→ 原子可见
```

---

## 1. period 与 dim 的标记实现

标记从用户语法出发，经 clang 语义层编码为 IR 元数据，再由 AOT pass 转成注册表项，最后在运行时按名字回填成稠密整数槽位。

### 1.1 属性定义（clang/include/clang/Basic/Attr.td:5221-5274）

| 属性（主拼写 / 别名） | 作用对象 | 参数 | 语义 |
|---|---|---|---|
| `ejit_entry` | Function | — | JIT 特化入口函数 |
| `ejit_dim` / `ejit_period_arr_ind` | ParmVar | `PeriodName` | 该参数是某 period 数组的实例下标（"时间窗维度"） |
| `ejit_period` / `ejit_in_period` | Var（标量） | `PeriodName` | 标量 period 变量 |
| `ejit_period_arr` / `ejit_in_period_array` | Var（数组） | `PeriodName` | period 数组（固定大小，Sema 限 ≤100 元素） |
| `ejit_period_lc` / `ejit_period_guard` | Function | `PeriodName` | 生命周期守卫函数：进入时 deactivate、返回时 activate |
| `ejit_may_const` / `ejit_period_const` | Field | — | "可当作常量特化"的结构体字段（软标注，可丢弃） |

> 注意：`ejit_dim` 是规范拼写，`ejit_period_arr_ind` 是别名；IR 标签与所有下游命名一律用 `ejit_period_arr_ind`。

### 1.2 Sema 校验（clang/lib/Sema/SemaEJIT.cpp）

- `ejit_period`：要求全局存储期；数组类型报错（应改用 `ejit_period_arr`）；同名 period/period_arr 冲突报错。
- `ejit_period_arr`：固定数组且大小 ≤100（`err_ejit_period_arr_too_large`）；也接受指向 struct/class 的指针（大小编码为 0）。
- `ejit_dim`：仅限整型参数；每个函数最多 4 个（`MAX_PERIOD_ARR_IND_PARAMS`，在 `ActOnFunctionDeclarator` 里补检，见 SemaDecl.cpp:10519）。
- `ejit_entry`：不允许递归（`RecursiveCallVisitor`）。
- `ejit_period_lc`：必须存在同名的 `ejit_dim` 参数（`err_ejit_period_lc_no_index`）。
- 与 `always_inline` 冲突时警告并丢弃（原因是 LTO 内联器会先于 AOT pass 运行，内联后 entry/lc 函数消失）。

### 1.3 CodeGen：IR 编码（clang/lib/CodeGen/CGEJIT.cpp）

`emitEjitFunctionMetadata`（CGEJIT.cpp:28-85）在函数上挂 **distinct `!ejit.metadata`** MDNode，子节点按标签区分：

```llvm
; ejit_entry 的 process_task(uint8_t cellIdx)（第 0 参标记 ejit_dim("cell")）
!ejit.metadata = distinct !{
  !{!"ejit_entry"},
  !{!"ejit_period_arr_ind", !"cell", i32 0}   ; 常量 = 参数下标，不是 dim 值
}
```

`emitEjitGlobalMetadata`（CGEJIT.cpp:114-167）在全局变量上挂同类元数据：

```llvm
; ejit_period_arr("cell") struct CellConfig g_cellCfg[16]
!ejit.metadata = distinct !{
  !{!"ejit_period_arr", !"cell", i32 16},     ; 常量 = 数组大小
  !{!"ejit_may_const_field", i32 8},          ; 每个 may_const 字段的字节偏移（PASS6 fallback）
  ...
}
```

**关键设计点：标记携带的编译期常量只有两个——dim 参数的下标（i32）和 period 数组的大小（i32）。** dim 的"值"永远是运行期实参；特化发生在 JIT 侧（见 §4.2）。

其余编码细节：

- `ejit_entry` / `ejit_period_lc` 在 CodeGen 时即加 `noinline`（CGEJIT.cpp:45-46, 64-66）——LTO 内联器跑在 PASS1/PASS3/PASS4 之前，这是函数能存活到 AOT pass 的唯一保障（Sema 只负责拒掉 always_inline）。
- `!ejit.may_const`：逐 load 标记，由 `EmitLoadOfScalar` 在字段带 `ejit_may_const` 时附上（CGExpr.cpp:2077-2080）；union/位域/volatile 字段不发。
- **extern 声明路径也发元数据**（CodeGenModule.cpp:5477-5480，`GetAddrOfGlobalVar`）：只 `extern` 声明 period 数组的 TU，其提取出的 bitcode 同样带 `!ejit.metadata`，否则 JIT 侧 PASS6 认不出该全局。

### 1.4 AOT pass 对标记的消费

late pipeline 中 `EJitAotModulePass`（PASS5，协调器）依次运行 PASS2→PASS3→PASS4；PASS1 单独排在早期（优化前）。模块里没有任何 `!ejit.metadata` 时整体跳过。

| Pass | 读什么标记 | 产出 |
|---|---|---|
| PASS1 `EJitRegisterBitcode` | `ejit_entry` | 按调用闭包裁剪模块并序列化 entry 的 bitcode → `ejit_register_bitcode(name, ptr, size)` + `.ejit_bitcode` 段条目（`EJIT_REG_BITCODE`）；外部符号发 `EJIT_REG_SYMBOL` |
| PASS2 `EJitRegisterPeriod` | `ejit_period_arr`、`ejit_period` | `ejit_register_period_array(periodName, varName, base, size)` + `.ejit_period` 条目（`EJIT_REG_PERIOD_ARRAY=1` / `EJIT_REG_STATIC_VAR=2`） |
| PASS3 `EJitWrapperGen` | `ejit_entry` + `ejit_period_arr_ind` | wrapper 本体（§2）；每个 dim 名字一个 **dimType 槽位全局** `@__ejit_dimtype_<periodName>`；每个 entry 一个 **funcIndex 槽位全局** `@__ejit_funcidx_<fnName>`；注册条目 `EJIT_REG_LIFECYCLE=5` / `EJIT_REG_FUNCINDEX=6` /（icache 开启时）`EJIT_REG_ICACHE_SLOT=7` |
| PASS4 `EJitPeriodHandler` | `ejit_period_lc` | 在入口（allocas 之后）插 `ejit_deactivate(name, cellIdx)`，在每个 return 前插 `ejit_activate(name, cellIdx)`（EJitPeriodHandler.cpp:157-185） |

双通道注册：**构造函数路径**（`ejit_auto_register`，priority 65535，`-enable-ejit-global-ctors` 默认开）+ **静态表路径**（无条件发射到 `.ejit_period` / `.ejit_bitcode`，裸机/`forceStaticRegistry` 用）。运行时构造路径优先，为空时回落静态表（EJit.cpp:137）。

### 1.5 注册表结构与运行时回填

`ejit_reg_entry_t`（EJitRegistryEntry.h:26-46，LP64 上 40 字节，与 AOT 发射的 `{ i32, ptr, ptr, ptr, i64 }` 结构一致）：

```c
typedef enum {
  EJIT_REG_BITCODE = 0, EJIT_REG_PERIOD_ARRAY = 1, EJIT_REG_STATIC_VAR = 2,
  EJIT_REG_SYMBOL = 3,  EJIT_REG_NONE = 4,          // 保留哨兵，表 ABI 稳定
  EJIT_REG_LIFECYCLE = 5, EJIT_REG_FUNCINDEX = 6, EJIT_REG_ICACHE_SLOT = 7
} ejit_reg_type_t;
typedef struct {
  ejit_reg_type_t type;
  const char *name1, *name2;  // funcName/periodName/varName/symbolName；name2 仅 varName
  const void *ptr;            // bitcode 数据 / 数组基址 / 符号地址 / &i32 槽位 / &icache 槽基址
  uint64_t size;              // bitcode 大小 / 数组大小 / numDims / 0
} ejit_reg_entry_t;
```

- 段名以 `.` 开头（`.ejit_bitcode` / `.ejit_period`）故意阻止链接器自动合成 `__start_/__stop_` 符号，由 `ejit_registry.ld:68-84` 手工括界（`KEEP`）。
- `ejit_init`（EJit.cpp:144-234）遍历 `[__start_ejit_bitcode, __stop_ejit_bitcode)` 和 `[__start_ejit_period, __stop_ejit_period)`：
  - `LIFECYCLE` / `FUNCINDEX` / `ICACHE_SLOT` 都是 **fixup 条目**——`resolveAssign(name)` 按名字分配槽位，直接写穿 `ptr` 指向的 wrapper 内部全局（EJit.cpp:166-227）。
  - 槽位耗尽（dimType 满 8、funcIndex 满 4096）是硬初始化错误。

**dimType 与 funcIndex 的分配语义**：进程全局注册表（`EJitLifecycleRegistry` ≤8 槽、`EJitFuncRegistry` <4096，单调计数器、只增不回收）**按名字一次性分配**，再回填进各模块的内部全局。这样每个模块无需互相可见即可得到一致的稠密编号；funcIndex 指纹（FNV-1a，与顺序无关）还用于共享任务池里拒绝注册表分叉的 peer（EJitCompileDriver.cpp:262-268）。

### 1.6 非显然的设计点（与直觉不符处）

1. **标量 `ejit_period` 的 period 名字在 PASS2 被丢弃**——静态变量只按 varName 注册，只有 `"static"` 语义生效（EJitRegisterPeriod.cpp:66-68）。
2. **dim 不携带任何常量值**；IR 里唯一与 dim 绑定的编译期数字是参数下标。
3. 三个 fixup 类型（5/6/7）与 period 数据共用 `.ejit_period` 段；`EJIT_REG_NONE=4` 是 ABI 哨兵。
4. `noinline` 在 CodeGen 时加而不是 Sema——这是 LTO 安全的关键时序。
5. 诊断 `warn_ejit_undeclared_period_dep` 及其 note 在 .td 里是死定义，等价检查在 `EJitAotModulePass.cpp:87-89` 直接打印 `errs()`。

---

## 2. entry 函数的 wrapper 生成（PASS3，EJitWrapperGen.cpp）

### 2.1 入口识别与守卫

- 识别：函数带 `!ejit.metadata` 且含 `TAG_EJIT_ENTRY` 子节点、非声明（EJitWrapperGen.cpp:445-450）。
- dim 信息：解析 `ejit_period_arr_ind` 子节点得 `{PeriodName, ArgIndex}`（:110-132）。
- 幂等：入口块名为 `jit_entry` 且引用 `@__ejit_funcidx_*`/`@__ejit_icache_fn_*` 即视为已 wrap（:474-495）；全部已 wrap 时返回 `PreservedAnalyses::all()`。
- 校验：dim 数 >4、函数内 dim 名字重复、下标越界、非整型参数 → 报错。

### 2.2 默认形态：单函数 wrapper（icache 关闭）

确认 **"单函数混合方案"**：不生成独立 wrapper 函数，wrapper 逻辑直接拼进原函数入口（EJitWrapperGen.cpp:945-958）。

拼接动作（`spliceOriginalBody`，:787-792）：**原入口块的全部指令整体搬进新块 `jit_fallback`**（修好后继 PHI 与 use 后删除原入口块）；新建 `jit_entry` 插在原入口块位置成为新入口，`jit_call`/`jit_dispatch` 追加。其余原始基本块原封不动——原函数体完整保留为 AOT fallback。

生成后的 CFG：

```
caller
  └─► jit_entry                          ; 新入口
        allocas: %ejit_out_fn, %ejit_out_bucket [, %ejit_dims[4 x {i32,i32}]]
        %ejit_funcidx = load i32, @__ejit_funcidx_F
        %ejit_idx_ok  = icmp ne %ejit_funcidx, 0xFFFFFFFF
        br %ejit_idx_ok ? jit_call : jit_fallback      ; 未注册/槽位耗尽 → 直接 AOT

      jit_call                            ; 查找（§3）
        (dimType ← load @__ejit_dimtype_<period>；instance ← zext/trunc 实参)
        %status = call ejit_taskpool_compile_or_get[_Nd](...)
        %ejit_fn = load %ejit_out_fn
        br (status==0 && fn!=null) ? jit_dispatch : jit_fallback

      jit_dispatch                        ; JIT 命中
        %r = call fn(全部原始实参)          ; 签名与 F 完全一致
        call ejit_taskpool_release_read(bucket)
        ret %r

      jit_fallback                        ; 原入口块（整体搬入）+ 原始其余 BB 不动
        <原指令……> br <原终结>
```

**fallback 语义**：原函数体只经两条 miss 边可达（funcidx 无效 / 任意 status≠0 或 fn==null——包括编译失败、实例被 deactivate、队列满、运行时未初始化 `EJIT_ERR_NOT_ACTIVE`）。JIT 完全不存在时行为与 AOT 完全一致，代价仅一次 load+比较。

### 2.3 icache 变体（`-ejit-inline-cache`，"LEVER B"，默认关）

- `F` 的全部块移入内部函数 `F_miss`（复制 F 的属性与 target-cpu 以保证内联器行为一致、加 noinline/cold）；`F_miss` 内是 §2.2 的完整慢路径 + AOT 体。
- `F` 本身变成 **无帧分发器**：`jit_entry` 用 dim 实参 GEP `@__ejit_icache_fn_F[16]^N`（D=16 是 2 的幂 → 移位，无乘法；**下标只用 instance 值，不含 dimType**，:866-879）→ 普通 load（align 8）+ null 判断（`llvm.expect` 提示命中）→ 命中 `musttail call fn(args)`，未命中 `musttail call F_miss(args)`。
- 可选 `-ejit-dispatcher-cluster` 把分发器聚到 `.text.ejit_dispatch` 段（iTLB/缓存行局部性）。
- 开 timing 时命中路径退化为普通调用（musttail 要求 call 紧跟 ret，插桩会破坏该约束，:891-897）。

### 2.4 wrapper 的运行时契约

wrapper 生成的外部符号（EJitCommon.h:91-108 定义名字，:456-469 声明）：

| 符号 | 签名 | 用途 |
|---|---|---|
| `ejit_taskpool_compile_or_get` | `i32(funcIndex, ptr dims, i32 numDims, ptr outFn, ptr outBucket)` | 通用查找/编译（>2 dims 时用） |
| `ejit_taskpool_compile_or_get_0d/1d/2d` | dims 以标量对传参 | 固定维数快路径（`DimCount ≤ 2` 时发射；3d/4d 名字存在但当前不可达，:655） |
| `ejit_taskpool_release_read` | `void(bucketIndex)` | 释放命中时持有的 bucket 读 token |
| `ejit_taskpool_trace_now/trace_wrapper` | — | 可选 timing |

- **AOT wrapper 不替换任何参数为常量**——所有实参原样转发给 JIT 函数指针。真正的"时间窗常量替换"发生在 JIT 侧（§4.2）。
- 通用路径的 dims 是栈上 `[4 x {i32,i32}]` alloca，布局与运行时 `ejit_dim_pair_t{dimType, instanceId}` 一致；dimType 每次调用现取（`@__ejit_dimtype_<name>`），instance 取实参。
- `ejit_taskpool_release_read` 在间接调用**之后**执行——读 token 覆盖了 fn 指针的整个使用期（防回收，§4.5）。

---

## 3. wrapper 如何访问 JIT 编译结果

### 3.1 身份键

所有查找的统一键是 **`(funcIndex, dims[])`**：funcIndex 是注册期按名字分配的稠密编号（`[0, 4096)`）；每个 dim 是 `{dimType∈[0,8), instanceId∈[0,256)}`，其中 dimType 在调用点现取、instanceId 是实参。同一函数的多个 period 维按元数据顺序排列（编译侧用 `getOrCacheFuncMeta` 重排到一致顺序）。

### 3.2 四层查找（读路径，从快到慢）

```
wrapper 入口
 │  ① icache：GEP @__ejit_icache_fn_F[dims] → 普通 load + null 判断 → 直接/尾调
 │     命中即返回，不进 taskpool，无任何锁与 token（只读路径）
 ▼
ejit_taskpool_compile_or_get（EJitRuntime.cpp:583-672）
 │  ② L0（共享构建）：每核 64 槽 seqlock 单元（EJitSharedTaskPool.h:613-642）
 │     哈希选槽 → seqlock 读 + epoch==dispatchEpoch + 身份全比对；命中无 token
 │  ③ bucket cache：bucketTryRead（无阻塞 tryRead）→ 身份匹配 → **逐 dim 版本复核**
 │     命中时持有读 token，调用完 wrapper 才 releaseRead；miss 原因若是
 │     "实例被禁用/off/不可共享"则直接终结（fallback），不进慢路径
 ▼
compileOrGet（EJitTaskPool.cpp:438-562）
    off 模式 → fallback；Sync → 调用线程内联编译；Async → dedup CAS + MPSC 无锁环
    入队（EJitWorker 编译，调用线程先跑 AOT，返回 EJIT_PENDING）
```

icache 填充（`icacheFill`，EJitSharedTaskPool.cpp:322-339）：任何成功 resolve（命中或新编译）后按 `idx = idx*16 + instanceId` 行主序**普通 store** 写入槽位——同核顺序性 + 数据依赖使原子操作不必要。安全前提：生产环境不接物理释放回调（`icacheReclamationSafe_`），缓存的指针永不悬挂（§4.5）。

### 3.3 访问同步语义

- **读 token 覆盖使用期**：bucket 命中返回的 fn 指针在 `releaseRead` 之前一直有效——写者发布新版本前必须等 readers 归零，杜绝 use-after-free。
- **版本复核是每次命中都做的**：entry 里快照了每个 dim 的 `versions[]`，与 `SwitchController::getInstanceVersion` 的 acquire load 比对，不匹配即 miss——period 切换后旧代码**惰性**失效，无物理驱逐（§4.4）。
- 共享构建 NO_RECLAIM 变体用 publishSeq 上的纯读 seqlock（每命中零 RMW），依赖"代码永不释放"。
- 锁分层：查找路径无 `std::mutex`（bucket 用自旋 EJitRwLock tryRead / 两字协议；L0 与 icache 无锁）；互斥量只用于单核簿记（CodePool、RuntimeState、RegistrationStore）。

### 3.4 共享构建的跨核可执行性（简述）

共享 cache slot 携带 `codeStart/codeSize/poolBase/poolSize/poolId`（`EJitCompiledCodeInfo` 镜像）。非 owner 核首次命中一个 slot 时：先放掉 bucket 读锁，对自己的页表执行 `split_2m_to_4k` + 逐页 `enable_ex`（`prepareExecForCurrentCore`），再重新校验 slot 状态/代数/身份后才返回 fn 指针，成功后置 `executableCoreMask` 对应位。

---

## 4. JIT 编译结果的保存与更新

### 4.1 机器码存储：EJitCodePool

- 2MiB 对齐 slab 上的 **bump 分配器**（默认池 2MiB，`EJitCodePool.h:117-151`）；slab 只存裸机器码，无逐函数头。
- **4K 封页模式**：每次分配独占新 4K 页（used 圆整到页界，:251-267），JITLink `finalize` 时对每个可执行段逐页 `enable_ex`（sealCodeRange，:354-403）；池满直接滚动到新池。
- **legacy 整池模式**：池满或已封时整池 seal 一次后滚动（:270-291），RW→RX 翻转点在 `EJitOrcEngine::lookup` 返回指针前。
- 每次 finalize 记录 `EJitCompiledCodeInfo{fnPtr, codeStart, codeSize, poolBase, poolSize, poolId}`（:524-558），供 peer 核在自己的上下文里精确 seal 相同的 4K 页。
- 内存管理器**从不释放池内存**（v1 决策，EJitCodePoolMemoryManager.cpp:219-220）——封过的 RX 页不可回收，废物尾巴计入 `wastedBytes` 统计。

### 4.2 编译：驱动 → ORC → JIT 优化管线

`compileNow`（EJitCompileDriver.cpp:480-551）：校验请求（instanceId ≤255、dimType 不重复）→ 按元数据顺序重排 dims → 打包 64-bit `cacheKey = (funcIndex<<32) | packedDims` → `compileCold`。

`compileCold`（:348-477）：取函数名与 bitcode（`EJitModuleLoader::getBitcodeByFuncIdx`，按 funcIndex 索引）→ **时间窗门控**（逐 dim 检查实例 enabled/active，先于任何编译）→ 构造 `SpecializationContext{fnName, cacheKey, dimensions, optLevel}` → `loadBitcodeModule` → `lookup`。

ORC 引擎（EJitOrcEngine.cpp）：
- LLJIT 单线程编译；IRTransformLayer 上跑特化管线；**每个 cacheKey 一个 JITDylib**（`spec_<key>`），先移除同 key 的旧 JD（:775-952）。
- 解析 period 数组/静态变量/用户符号为绝对地址符号。
- JIT 管线（EJitOptimizer.cpp:82-127）第 1 阶段就是**特化本身**：`preReplacePeriodIndices` 把 `ejit_period_arr_ind` 参数**替换为常量 instanceId**（:129-166）→ InstCombine 折叠 GEP 链 → `EJitStructFieldPass`（PASS6）把带 `!ejit.may_const` 的 load 换成从 period 数组内存镜像读出的常量（≤64 位整型/浮点，指针型除外）→ 内部化 + IPSCCP 跨调用边传播常量 → 再两轮 InstCombine+StructFieldPass；第 2-4 阶段跑精简的 L1/L2/L3 优化（无向量化）+ 常量界循环展开。

### 4.3 提交（publish）：如何"保存"一个新结果

私有池 `EJitTaskPoolCache::publish`（EJitTaskPool.cpp:172-238）：

1. **写锁**：CAS writeFlag + 自旋等 readers 归零（不可能有读者还握着将被覆盖的指针）。
2. **提交门控**：锁内逐 dim 复核 `versions[i] == 当前版本`——不匹配说明编译期间发生过 period 切换，新代码已过期，**拒收**（宁可丢结果也不贴错版本号）。
3. 同身份 → 原位覆盖（更新 versions/fnPtr）；否则链尾新增（哈希链允许碰撞共存）。
4. **锁外**释放被覆盖的旧指针（回调可能重入 ORC/分配器）。

共享池 `EJitSharedTaskPool::cachePublish`（EJitSharedTaskPool.cpp:1308-1408）同构，多了**代数门控**（`req.generation != state->generation` 拒收，防 owner 重初始化期间的迟到发布）与严格的**publish-last 顺序**：

```
slot.state = Publishing（release）→ 写身份/versions/hash/代码范围字段
→ fnPtr = 新指针（release）→ executableCoreMask → state = Ready（release，最后写）
→ dispatchEpoch++（退役所有核的 L0）→ 放写锁 → 锁外释放旧指针
```

读者只要 acquire 看到 `Ready`，就能看到完整一致的 slot 内容；`dispatchEpoch++` 使所有 L0 条目整体过期。槽位选择：同身份 → 首个空槽 → 首个占用槽（确定性驱逐）。

### 4.4 period 切换如何"更新"结果：惰性三层失效

状态模型：`EJitSwitchController` 的 `enabled_[8][256]` + `version_[8][256]`（EJitTaskPool.h:73-95）。`ejit_activate/deactivate`（PASS4 插桩调用）CAS enabled 位，**仅当位真正翻转时** version++（共享构建额外 `dispatchEpoch++` 退役 L0）。

失效是**惰性**的，三个防线：

1. **生产者早拒**：`tryCacheHit` 在查缓存前先查 `isInstanceEnabled`——被禁实例永远读不到旧代码，直接 fallback。
2. **工作者检查点**：worker 编译前后各拍一次版本快照（cp1/cp2），过期则丢弃结果并清 dedup 位。
3. **提交门控**：§4.3 锁内复核——最后一道，杜绝"编译期间切换、之后贴错版本"的窗口。
4. 另加**命中复核**：lookup 每次命中都比对版本，旧 entry 自然 miss（共享池含 generation 比对）。

### 4.5 内存回收语义

- slab 内存 v1 **永不回收**：池只滚动不释放；`deallocate` 只跑 JITLink 的 dealloc actions。
- 代码退役仅通过可选 `ReleaseCallback`（生产环境不接）——旧指针纯逻辑丢弃。**这正是不回收 icache/L0 指针的安全基础**（`icacheReclamationSafe_` 在有物理释放器时自动禁用）。
- 停机路径：`cache_.shutdown()` 在每个 bucket 写锁下逐条释放存活 fnPtr 且恰好一次。

---

## 附录 A：关键文件索引

| 环节 | 文件 |
|---|---|
| 属性定义/文档 | `clang/include/clang/Basic/Attr.td:5221-5274`、`AttrDocs.td:9435-9549` |
| Sema | `clang/lib/Sema/SemaEJIT.cpp`、`SemaDeclAttr.cpp:7894-7912` |
| IR 编码 | `clang/lib/CodeGen/CGEJIT.cpp`（逐 load 标记在 `CGExpr.cpp:2077-2080`） |
| 共享常量/标签 | `llvm/include/llvm/ExecutionEngine/EJIT/EJitCommon.h` |
| AOT pass | `llvm/lib/Transforms/EmbeddedJIT/`（EJitRegisterBitcode / EJitRegisterPeriod / EJitWrapperGen / EJitPeriodHandler / EJitAotModulePass） |
| 注册表定义/行走 | `llvm/include/llvm/ExecutionEngine/EJIT/EJitRegistryEntry.h`、`llvm/lib/ExecutionEngine/EJIT/EJit.cpp:144-234`、`ejit_registry.ld` |
| wrapper 运行时入口 | `llvm/lib/ExecutionEngine/EJIT/EJitRuntime.cpp:583-935` |
| 缓存/提交 | `EJitTaskPool.cpp:126-238`（lookup/publish）、`EJitSharedTaskPool.cpp:1308-1408`（cachePublish）、`EJitSharedTaskPool.h:613-642`（L0） |
| 代码池 | `EJitCodePool.cpp:220-291`（allocateCode）、`:354-403`（sealCodeRange）、`EJitCodePoolMemoryManager.cpp` |
| 编译 | `EJitCompileDriver.cpp:348-551`、`EJitOrcEngine.cpp:775-997`、`EJitOptimizer.cpp:82-246`、`EJitStructFieldPass.cpp` |
| 状态/注册表 | `EJitSwitchController`（EJitTaskPool.h:73-95）、`EJitFuncRegistry.h`、`EJitLifecycleRegistry.h`、`EJitRuntimeState.cpp:101-160` |

## 附录 B：关键常量速查

| 常量 | 值 | 含义 |
|---|---|---|
| `MAX_PERIOD_ARR_IND_PARAMS` | 4 | 单函数 dim 上限 |
| `kEJitMaxDimTypes` | 8 | dimType（lifecycle）槽位数 |
| `EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX` | 4096 | funcIndex 上限 |
| `EJIT_ICACHE_DIM_SIZE` | 16 | icache 每维分桶数（2 的幂，移位索引） |
| `kEJitInvalidFuncIndex` / `kEJitInvalidDimType` | 0xFFFFFFFF | 未回填哨兵 |
| period 数组最大元素数 | 100 | Sema 限制 |
| `EJIT_CTOR_PRIORITY` | 65535 | 注册构造器优先级 |
| 代码池默认 | 2MiB 池 / 2MiB 对齐 / minAlign 64 / 4K 封页 | EJitCodePool.h:117-151 |
