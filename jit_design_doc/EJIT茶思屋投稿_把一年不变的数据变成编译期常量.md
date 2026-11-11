# 把一年不变的数据，变成编译期常量
## ——EmbeddedJIT 时间窗特化的设计与实现

> 在基站和很多嵌入式系统里，有一类数据很特别：硬件配置启动之后就不变了，小区制式配置一次几周不动，上电参数初始化一次就再没人碰。运行期几乎不变——但 AOT 编译器不知道。这篇文章讲我们把这类数据在运行时重新变回编译期常量的完整尝试：标注接口、两阶段 AOT、无锁异步编译、跨核共享，以及一路上踩过的坑。所有数字来自仓内设计文档与测试记录，host 上测的、板上还没测的，都会标清楚。

---

## 一个摊不平的分支

先看一段每个嵌入式工程师都写过的代码：

```c
if (g_boardCfg.boardType == UBBPg) {
    ...
} else {
    ...
}
```

`g_boardCfg.boardType` 是什么？单板出厂就烧死了，上电初始化读一次，之后一年不会变。但在 AOT 编译器眼里，这是一次普通的内存读取：load、比较、跳转，一个都不能少。更糟的是下游——`boardType` 一变，后面十几个分支的方向全跟着变，编译器只能老老实实把两条路的代码都留在那里，i-cache 里两条路都在占地方，分支预测器在猜一个其实永远不会变的方向。

PGO 也救不了这个分支。profile 能告诉编译器"左边的路热"，让它把左边排前面，但删不掉右边的比较本身。

问题的本质是：**程序员比编译器多知道一件事**——这块数据在很长时间里不变。而 C 语言里没有地方让你把这件事写下来。

于是我们做了 EmbeddedJIT（下面简称 EJIT）：让开发者用标注把"我知道的稳定性"声明出来，编译器在 AOT 阶段把函数的 bitcode 和这些声明一起存进可执行文件，运行时按需做一次特化编译——把那些"事实上的常量"直接烙进指令里。业务函数该是什么还是什么，JIT 失败了就原样走 AOT 路径，一次都不会崩。

目标环境很克制：内存受限，软实时要求高，裸核，自然也没有 dlsym、pthread 和 C++ 线程库。这些约束后来塑造了整套设计的一半。

---

## 时间窗常量：把程序员知道的事，告诉编译器

EJIT 的核心抽象叫**时间窗（period）**。语义一句话讲完：某段全局数据，在 `ejit_activate` 到 `ejit_deactivate` 之间保持不变，这段时间内，JIT 可以把标注过的字段当编译期常量。

整个用户接口只有六个标注和几个生命周期函数：

```c
struct CellCfg {
  ejit_may_const uint32_t cellType;   // 声明：时间窗内视为常量
  uint32_t trafficLoad;               // 普通字段，照常从内存读
};

ejit_period_arr(cell) struct CellCfg g_cellCfg[8];  // 归 "cell" 窗管理

ejit_entry                                   // 声明：这个函数值得 JIT
uint32_t process_cell(ejit_period_arr_ind(cell) uint8_t ci)
{
  // JIT 特化时, g_cellCfg[ci].cellType 被替换为编译期常量,
  // 这个分支在特化版本里不存在
  if (g_cellCfg[ci].cellType == FDD)
    return fdd_path(ci);
  return tdd_path(ci);
}

ejit_period_lc(cell)                        // 声明：这个函数会改窗内数据
void change_cell_cfg(ejit_period_arr_ind(cell) uint8_t ci);
```

对应的运行时：

```c
ejit_activate("cell", 3);     // 激活 cell[3]：此后可特化
process_cell(3);              // 首次触发编译, 之后命中缓存
ejit_deactivate("cell", 3);   // cell[3] 数据要变了：特化版本全部作废
```

几个语义决定值得展开：

**激活是 name 级的。** 同一个 period 名字可以挂多个数组（`g_cellCfg` 和 `g_cellPhy` 都归 `"cell"` 管），`activate("cell", 3)` 一次激活两个数组的第 3 号实例。早期版本有带数组指针的激活接口，后来删掉了——产品不需要，而保留它要付出跨核同虚拟地址假设、热路径复杂度、一堆测试语义的成本，收益是零。

**写数据必须走 lc 函数。** 编译器在 `change_cell_cfg` 入口插 `deactivate`、每个出口插 `activate`，像一把写锁：改配置期间特化版本作废，谁也读不到半新半旧的值。多个窗的插桩是同序的，不是锁的那种逆序——窗之间没有嵌套依赖，同序等价且更简单。

**volatile 字段永远不会被当常量。** 编译器替你把最危险的那一类排除掉了。

时间窗的生命周期和缓存失效的关系，一张图说清：

```
时间 ────────────────────────────────────────────────────────────►

     ejit_activate("cell",3)        ejit_deactivate("cell",3)
            │                              │
  ┌─────────▼──────────────────────────────▼─────────┐
  │           时间窗 cell[3] 生效区间                   │
  │   · g_cellCfg[3].cellType 在 JIT 眼里 = 常量        │
  │   · 首次调用 → 后台特化 → 后续调用直接命中           │
  └─────────▲──────────────────────────────┬─────────┘
            │                              │
      未激活: 纯 AOT 执行          cell[3] 的所有特化版本作废
      (不依赖 JIT 也能跑)         下次 activate 后按需重新特化
```

内置的 `static` 窗代表"整个 JIT 运行期都不变"，永远生效，不用激活——板卡类型这类数据就放这里。数组窗则是给"多实例"数据准备的：一个基站几十个小区，每个小区一份独立的配置、独立的时间窗，函数用哪个小区由 `ejit_period_arr_ind` 参数指定。单个函数最多声明 4 个这样的维度。

---

## AOT 侧：两阶段，和一份"丢了也不出错"的元数据

编译器侧一共五个 AOT pass 加一个 JIT 侧 pass。关键不在数量，在于它们在 pipeline 里的位置——EJIT 的 AOT 是**两阶段**的：

```
非 LTO 路径:

  Clang CodeGen
       │
       ▼
  ┌──────────────────────────────────┐
  │ PASS1  EJitRegisterBitcode        │ ← 永远在所有优化之前
  │  · 沿调用图扫 ejit_entry 的闭包    │
  │  · 克隆进独立 Module, 预优化        │
  │  · 嵌入 .ejit.bitcode 段           │
  │  · 顺带注册闭包用到的外部符号       │
  └──────────────┬───────────────────┘
                 ▼
  ┌──────────────────────────────────┐
  │ PASS2/3/4（PASS5 协调执行）        │ ← 也在优化之前
  │  · 注册 period 数组与 static 变量   │
  │  · wrapper 插桩（见下图）           │
  │  · lc 函数插 activate/deactivate   │
  └──────────────┬───────────────────┘
                 ▼
            O2 / O3 标准优化  ←── 顺便把 wrapper 的 fallback 路径也优化了
                 ▼
            目标代码生成
```

为什么 PASS1 必须抢在优化前面？三个理由，一个比一个隐蔽：

**其一，元数据会被优化"洗掉"。** `ejit_may_const` 挂在 load 指令上（`!ejit.may_const !{}`），而 SROA、InstCombine、GVN 这些 pass 在拆分、重写 load 的时候并不保证保留自定义元数据。必须在它们动手之前把原始 IR 存走。

**其二，闭包要在内联发生前抓全。** `ejit_entry` 调用的所有内部函数都得进 bitcode，否则 JIT 的时候找不到函数体。

**其三，顺序反了会无限递归。** 这条最好笑：wrapper 里包含"请求编译本函数"的调用。如果先插桩再抽 bitcode，JIT 就会把 wrapper 自己编译一遍，wrapper 的 miss 路径又请求编译同一个函数——死循环。

而 PASS2–4 在非 LTO 路径下放在优化**之前**，则是为了让标准优化顺手收拾 wrapper：fallback 路径的分支被优化成 select、返回值 PHI 合并，`noinline` 属性也在 inliner 跑之前就位，防止 wrapper 被内联进调用者。（LTO 路径下 LTO 已经优化完了，这几个 pass 就挪到优化之后，插完即最终 IR。）

元数据这块多说两句，因为它是整个设计里我最喜欢的一个决定。`!ejit.may_const` 是**软标注**：

> 优化把它丢了，最多错过一次特化机会（fallback 到 AOT 结果），不会产生错误代码。这区别于 volatile——volatile 丢了是错误行为，所以 volatile 必须是指令位，而 may_const 只能是元数据。

为了保证它尽量不丢，做了两重保险：LLVM 层面把 `ejit_may_const` 注册成固定 metadata kind，`copyMetadataForLoad` 在 InstCombine/SROA/Mem2Reg 重写 load 时显式保留它；PASS1 预优化跑完后，再由 `reAnnotateMayConst` 按全局变量上记录的字节偏移，把新 load 丢掉的标注重新挂回去。真的还是丢了怎么办？PASS6 只认 load 上的元数据，没有就跳过——当没这个标注，函数照常执行。

**整个系统贯彻同一个错误哲学：任何环节失败，降级为 AOT，绝不产生错误代码。** 后面你会看到这条原则反复出现。

wrapper 的插桩是"单函数混合方案"——不生成单独的 wrapper 函数，直接把分发逻辑插进原函数入口，原函数体原封不动变成 fallback 块：

```
              用户调用 process_cell(ci)
                       │
                       ▼
           ┌───────────────────────┐
           │ jit_entry (插桩入口)   │
           │  从 wrapper globals   │
           │  构造 dims[]          │
           │  compile_or_get(...)  │
           └───────┬───────┬───────┘
          pfn != NULL     pfn == NULL
               │               │
               ▼               ▼
      ┌────────────────┐  ┌──────────────────────┐
      │ jit_dispatch   │  │ jit_fallback         │
      │  call pfn(args)│  │  = 原函数体原样保留,   │
      │  release_read  │  │    一个字都没动        │
      └────────────────┘  └──────────────────────┘
```

翻译成 C 大概是：

```c
uint32_t process_cell(uint8_t ci, ...) {
  ejit_dim_pair_t dims[1];
  dims[0].dimType    = __ejit_dimtype_cell;   // ejit_init 时回填
  dims[0].instanceId = ci;
  void *fn; uint32_t bucket;
  if (ejit_taskpool_compile_or_get(funcIdx, dims, 1,
                                   &fn, &bucket) == EJIT_OK) {
    uint32_t r = ((PFN_t)fn)(ci, ...);        // 特化版本
    ejit_taskpool_release_read(bucket);        // 归还读令牌
    return r;
  }
  /* jit_fallback: 原函数体 */
}
```

---

## 运行时：一次特化发生了什么

StructFieldPass 干的活最直接：沿 GEP 链算出字节偏移，从进程内存把当前值读出来换成常量。

特化前后对比，眼见为实：

```llvm
; bitcode 里的样子
%idx  = zext i8 %ci to i64
%ptr  = getelementptr %CellCfg, ptr @g_cellCfg, i64 0, i64 %idx, i32 0
%t    = load i32, ptr %ptr, !ejit.may_const !{}
%cmp  = icmp eq i32 %t, 2                    ; FDD
br i1 %cmp, label %fdd, label %tdd

; PASS6 替换后 (ci=3, g_cellCfg[3].cellType 当时刻的值恰好是 FDD)
%cmp  = icmp eq i32 2, 2
br i1 true, label %fdd, label %tdd           ; SimplifyCFG 直接删掉 tdd
```

---

## 没有线程库的世界里，自己搭一个编译调度器

到目前为止都还算"常规 JIT 工程"。接下来的部分是要解决裸核上运行。

目标环境是 SRE 裸核：`std::thread`、`std::mutex`、`std::condition_variable`、`<atomic>`，全都没有。能用的只有三样：编译器内建的 `__atomic_*` 原子操作、内存栅栏、平台 task 原语。EJitAtomic 全部基于 `__atomic_*` 内建实现。

调度器的形态是**纯异步、单 worker**：业务核（producer）miss 时入队即返回 fallback，一个 worker 慢慢消费。没有同步编译路径——要同步，开个模式让调用线程自己走一遍编译即可，但队列、去重、缓存是同一套。

任务队列是自实现的 Vyukov 无锁环形队列。多生产者并发入队没问题；消费端我们**只用一个消费者，且把这当成正确性的硬约束**。

最后一个组件是失效。`deactivate` 不能去遍历缓存删条目（那要在热路径上加锁），所以是**基于版本号的惰性失效**：每个时间窗实例一个单调递增的 version，`deactivate` 做一次 CAS(enabled→0) 加一次 version++，然后靠三个懒失效点各自把关：

```
 ejit_deactivate("cell", 3)
        │  CAS(enabled→0) + version++    ← version 单调递增, 永不回退
        ▼
 懒失效点 1  Cache      新查询逐实例比对 version → 不匹配 → 自动 miss
 懒失效点 2  Queue      worker 出队时比对入队时的版本快照 → 不匹配 → 丢弃
 懒失效点 3  In-flight  编译前再比对一次 → 不匹配 → 编译完也不 publish
```

worker 还有两道检查点（出队后、publish 前）各自比对版本，挡住"编译期间数据被改"的竞态。检查点 2 到 publish 之间的窗口是良性的：publish 在锁内重新快照版本，写进去的版本若是过时的，效果等同于 toggle 晚到一纳秒——发布的指针立刻失效，不引入新问题。

版本号的单调性是整个机制的地基：请求快照的 version 一旦被一次 toggle 甩在身后，就永远追不上当前值。**状态可以恢复（再 activate），version 不会。** 32 位版本号要 ~21 亿次 toggle 才回绕，实际不可能。enabled 和 version 是两个独立的原子变量而不是塞进一个 U32，也是刻意的——合并就得写 CAS 循环，拆开则 `setEnabled` 永远只需一次 CAS。

把四个组件拼起来，异步编译的全貌是三个泳道构成的一个环：

```
 ┌─ 业务核 0..N (producer) ──────────────────────────────────────┐
 │  ① compile_or_get(funcIdx, dims)                             │
 │  ② 实例开关检查 (禁用 → 直接 fallback)                          │
 │  ③ 缓存 tryRead ──命中──► ④ 执行特化代码 + release_read        │
 │        │ miss                                                │
 │        ▼                                                     │
 │  ⑤ dedup CAS 占位 + 入队 (队满/冲突 → 回滚占位, 返回 fallback)  │
 └───────────────────────────┬──────────────────────────────────┘
                             ▼
 ┌─ 跨核共享段 (纯 POD + 原子变量, 同虚拟地址) ────────────────────┐
 │  TaskQueue (Vyukov 环形, 1024 槽)     结果缓存 (32 桶 × 16 槽)  │
 │  dedup inFlight[4096]                 每桶独立 RwLock           │
 │  SwitchController (enabled + version, 8×256 实例, 共 10 KiB)    │
 └───────────────────────────┬──────────────────────────────────┘
                             ▼
 ┌─ Worker 核 (生产 preset 钉在核 6) ─────────────────────────────┐
 │  ⑥ pollOne 出队 (队列空则 yield 让出)                           │
 │  ⑦ 检查点 1: version 失配 → 丢弃                                │
 │  ⑧ JIT 编译: bitcode → 特化 → ORC/JITLink → 代码池               │
 │  ⑨ 检查点 2 → publish (spin 等该桶读者清零)                      │
 └───────────────────────────┬──────────────────────────────────┘
                             ▼
                      写回结果缓存 ──► 下次 ③ 命中
```

---

## 它适合谁，不适合谁

一套判断标准，按踩过的坑整理：

**适合：**
- 配置类数据有明确的长稳定窗口（板卡、制式、组网参数），且读取发生在热点路径
- 首次编译延迟在异步模式下副作用影响可控
- 数据变更能收敛到少数几个写函数（能插生命周期栅栏）
- 有一点富余内存：bitcode + JIT代码池 + 运行时本体

**不适合：**
- 数据高频变化（每次变都意味着重新特化，JIT 变成了负担）
- 硬实时硬截止（编译时机不可承诺）

最后一个判断我觉得比技术清单更重要：**这个方案的本质不是"上 JIT"，而是给 C 程序员补一个表达"数据稳定性"的语言机制，然后用一套足够保守的工程把它兑现成指令。

