# EmbeddedJIT SRE Taskpool 编译调度机制

> 当前分支:`ejit-taskpool-spec-worker`(v2,实现 SPEC §3.4 单 worker 模型)
> 构建开关:`EJIT_SRE_TASKPOOL`(CMake option,默认 **OFF**;启用后 `ejit_taskpool_*` C ABI 与内部 worker 生效)
> 目标平台:`aarch64_be`(SRE,无 C++ 线程库),host 可跑测试构建

---

## 1. 背景与设计约束

### 1.1 为什么需要 taskpool

`ejit_compile_or_get` 的原实现是一条直通路径：查 LRU cache → 未命中则同步编译 → 写入 LRU cache。
这在以下场景中有局限：

- **无去重**：多个调用方同时请求同一个 (funcIndex, cacheKey)，各自独立编译，浪费 CPU
- **无异步**：编译阻塞调用方，无法利用其他核并行
- **依赖 STL**：`std::unordered_map`、`std::list` 等容器在 SRE bare-metal 环境不可用

### 1.2 平台约束

目标 SRE 平台（aarch64_be）**没有 C++ 线程库**，因此禁用：

- `std::thread` / `std::async` / `std::future` / `std::promise`
- `std::mutex` / `std::shared_mutex` / `std::condition_variable`
- `<atomic>` / `<functional>`

以下 STL 容器通过**重载 `operator new`** 使用平台内存分配，可用：
- `<unordered_map>` / `<vector>` / `<string>`

SRE 平台提供三种基础并发原语：

| 原语 | 说明 | 本系统使用 |
|------|------|-----------|
| 原子变量 | SRE 原子读写/CAS（编译器 `__atomic_*` 内建或平台桩） | **主要使用** |
| 内存栅栏 | SRE acquire/release/full fence | 配合原子变量，保证内存可见性 |
| 核间信号量 | SRE 跨核通知/唤醒 | 可选项（将来用于 worker 唤醒优化，当前非核心） |

本系统以**原子变量 + 内存栅栏**为主要手段，实现轻量级无锁并发设计。在此基础上封装 `EJitAtomic`（底层抽象）和 `EJitRwLock`（读写锁），不再暴露底层原子指令。

平台另需提供:

- **task 创建/销毁原语**:由 SRE 平台提供 task 创建/销毁能力。本系统封装为 `EJitSreTask`(§3.4)抽象层,业务代码不直接接触平台 task API。host 测试用 `std::thread` 实现,SRE 上板使用平台原语;两份实现链接时择一。

### 1.3 核心设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 模型 | 纯异步：producer 入队 + 单 worker 消费 | 单 worker 跑在独立 SRE task 上，由 `EJitSreTask` 抽象创建；不提供同步编译路径 |
| worker 数量 | **固定 1 个** | 队列采用 Vyukov MPSC 无锁实现，单消费者(SC)是无锁正确性的前提；多 worker 需要把 queue 升级到 MPMC，改造面大，当前不需要 |
| worker 平台依赖 | `EJitSreTask` 抽象层封装 | host 测试用 `std::thread`，SRE 用平台 task 原语，链接时择一；与 `EJitAtomic` 同样的"头文件抽象 + 平台实现"模式 |
| cache | 固定分桶 (32) × 每桶 `unordered_map` | 分桶限制 rehash 爆炸半径（rehash 只影响单桶读者），桶内弹性容量 |
| cache 读并发 | try-read（写标志置位 → 立即 fallback） | 读在热路径，不可阻塞等待 |
| cache 写并发 | spin-write（CAS 抢写标志 → spin 等读者退场） | 写不频繁，spin 开销可接受 |
| 队列 | Vyukov 无锁环形队列(自实现，MPSC) | 基础组件。SRE 平台未提供 queue 原语，使用自实现版本；多 producer + 单 consumer，单 worker 假设由此而来 |
| dedup | 扁平数组 `inFlight_[funcIndex]` + 1 bit 占位 | 基础组件。`funcIndex` 直接当数组下标，O(1) 无分桶无扫描；payload 全在 queue，dedup 不持有任何编译参数 |
| dedup 写并发 | CAS 抢占位 (0→1)，冲突 → 立即 fallback | 多 producer 可能冲突，不等待。worker 完成后回零 |
| taskQueue | `EJitTaskQueue` 组合 queue + dedup | 业务组件。向上提供去重的任务提交/消费统一接口，内部协调 queue 与 dedup 的生命周期 |
| 淘汰 | 逐实例 version 比对失效 + publish 覆盖 | toggle bump version → cache entry 逐实例比对不匹配 → 自然 miss |
| 分桶动机 | 限制 rehash 爆炸半径 | 单全局 `unordered_map` rehash 阻塞所有读者；分桶后 rehash 仅阻塞单桶 |
| 原子设施 | `EJitAtomic` wrapper → `EJitRwLock` 封装 | 原子底层集中封装，上层通过 RwLock 接口使用 |
| 模块划分 | 基础组件(Atomic/RwLock/Queue/SreTask/Dedup) + 业务组件(Cache/TaskQueue) + 调度器(Switch/Worker) + JIT Compile Management（编译） | 关注点分离：基础组件不感知编译语义；调度不关心编译细节，编译不关心调度策略 |

---

## 2. 架构总览

整个系统分为两个模块：

| 模块 | 职责 | 关键组件 |
|------|------|---------|
| **Taskpool（调度层）** | 缓存管理、任务队列调度、开关控制、worker 驱动 | Cache, TaskQueue, SwitchController, Worker, Counters |
| **JIT Compile Management（编译层）** | IR 管理、编译流程控制、OrcJIT 引擎 | CompileDriver, OrcEngine, Optimizer, ModuleLoader, CodePool |

Taskpool 负责"这个编译请求谁来处理、结果如何缓存"，编译层负责"具体怎么编译"。Taskpool 通过回调接口调用编译层，不感知 IR/AOT/Opt 细节。

### 2.1 组件关系

```
EJitTaskPool                                       EJitCompileDriver (编译层)
  │
  ├── 业务组件 (有状态)
  │     ├── EJitSwitchController   → 模式(Off/Async) + 每(dimType,instance)独立version
  │     ├── EJitTaskPoolCache      → 结果缓存 (32桶, 每桶 unordered_map + 独立 RwLock)
  │     │                             hashKey = hash(funcIndex, dims) 定位桶,vector 内
  │     │                             identityMatches 解 hash 冲突 + 逐维 version 验
  │     ├── EJitTaskQueue          → 去重的任务提交管理 (组合 Queue + Dedup, §4.2)
  │     │                              tryEnqueue: dedup 占位 → queue push (失败自动回滚)
  │     │                              tryDequeue / release: queue pop / dedup 释放
  │     ├── EJitWorker             → 调度循环 (单 worker, 跑在独立 SRE task 上)
  │     │                             轮询 taskQueue.tryDequeue → runCompile, 软停止
  │     └── EJitTaskPoolCounters   → 无锁原子统计计数器
  │
  ├── 基础组件 (平台抽象, 不感知编译语义)
  │     ├── EJitAtomic             → 原子操作 wrapper (__atomic_* 封装)
  │     ├── EJitRwLock             → 读写锁 (基于 EJitAtomic, Cache 每桶一个)
  │     ├── EJitSreQueue           → 无锁 MPSC 环形队列 (Vyukov 自实现, §3.3)
  │     ├── EJitDedupTable         → 按整数 key 的 CAS 占位表 (§3.5)
  │     └── EJitSreTask            → 平台 task 抽象 (host: std::thread / SRE: 平台原语)
  │
  └── 编译边界
        compileFn_: CompileCallback (函数指针 + ctx 指针)
        runCompile() ──compileFn_(ctx, req, &fn)──→ EJitCompileDriver
                                                       (taskpool 不感知 IR/Orc/Opt)
```

**关键边界**:

- **`EJitTaskPool` 与 `EJitCompileDriver`** 通过 `CompileCallback` 函数指针解耦,taskpool 头文件不引用编译层任何类型
- **`EJitTaskQueue` 与基础组件**:TaskQueue 内部持有 `EJitQueue` + `EJitDedupTable` 实例,组合两者提供去重入队/出队/释放的统一接口；Queue 和 Dedup 作为基础组件不感知编译语义
- **`EJitWorker` 与 `EJitSreTask`**:Worker 持有 task 句柄,task 入口指向 worker 实例方法;SreTask 替换实现(host vs SRE)对 worker 透明
- **`EJitWorker` 与 `EJitTaskQueue`**:单 worker 通过 TaskQueue 的 tryDequeue 消费,单消费者是 Vyukov MPSC 队列的 SC 假设依赖,**多 worker 会破坏无锁正确性**(§1.3 决策)

### 2.2 数据流

```
compile_or_get(funcIndex, dims, numDims, fallback)
      │
      ├─ 1. 参数检查: numDims ≤ 4 且 (numDims==0 || dims!=null)
      │       不满足 → InvalidParam, 返回 fallback
      │
      ├─ 2. 维度开关检查: for each (dimType, instanceId) in dims
      │       any disabled → InstanceDisabled, 返回 fallback
      │
      ├─ 3. cache.lookup(funcIndex, dims, numDims)
      │       命中 → CacheHit, 返回 {fnPtr, bucketIndex, hasReadToken=true}
      │              (read token 外提,调用方用完 fnPtr 后须 release_read(bucketIndex))
      │
      ├─ 4. Off mode → OffMode, 返回 fallback (不入队、不编译)
      │
      └─ 5. 构造 req + 快照 versions[i] = getInstanceVersion(...)
            taskQueue.tryEnqueue(req)        // 内部: dedup 占位 → queue push (失败自动回滚)
              Enqueued       → EnqueuedPending, 返回 fallback
              AlreadyPending → AlreadyPending, 返回 fallback
              QueueFull      → QueueFullFallback, 返回 fallback
              InvalidFunc    → InvalidParam, 返回 fallback        // funcIndex 越界

EJitWorker (内部 task,§5.5,与上面在不同核):
      pollOne() → taskQueue.tryDequeue() → runCompile (检查点 1/2 + publish 提交门,§5.3)
```

### 2.3 Before vs After：`ejit_compile_or_get` 流程变化

```
                BEFORE                                  AFTER
            (ejit_dev_spec4)                      (ejit-taskpool)

ejit_compile_or_get(cacheKey)             ejit_taskpool_compile_or_get(
  └─ getOrCompile                              funcIndex, dims, numDims, &outFn, &outBucket)
       └─ LRU find                          └─ taskPool_->compileOrGet(funcIndex, dims, ...)
          hit  → return pfn                       ├─ 维度开关检查 → any disabled → fallback
          miss →                                  ├─ cache.lookup (hashKey/identity/version)
            decode cacheKey                       │     hit → return {fnPtr, bucketIndex}
            load bitcode                          ├─ Off mode → fallback
            verify periods                        ├─ taskQueue.tryEnqueue(req with versions[])
            OrcJIT compile                        │     dedup CAS + queue push
              ↳ IR pipeline                       │     ─→ EnqueuedPending / AlreadyPending /
            lookup symbol                         │        QueueFull / InvalidParam → fallback
            LRU put                               └─ return fallback (异步路径)
            return pfn
                                                Worker (独立 SRE task,异步):
                                                  pollOne → tryDequeue → runCompile
                                                    检查点1 → compileFn_ → 检查点2
                                                    → cache.publish (提交门重验 version)
```

核心差异：

| | Before | After |
|------|--------|-------|
| cache | LRU `unordered_map` + linked list | 32 桶 `unordered_map`，弹性容量 + rehash 隔离 |
| dedup | 无 | `EJitDedupTable`(基础组件)，1 bit 占位防止同 funcIndex 重复编译 |
| taskQueue | 无 | `EJitTaskQueue`(业务组件)，组合 queue + dedup 提供去重的任务提交管理 |
| 异步 | 无 | 入队 + 内部 EJitWorker 自动驱动，不暴露外部 poll 接口 |
| 淘汰 | LRU 自动淘汰 | 逐实例 version 比对失效 + publish 覆盖 |
| 维度开关 | 无 | 每 `(dimType, instanceId)` 独立控制（无 IR 耦合） |
| 接口 | 内部 `cacheKey u64` 编码 | `funcIndex + dim pair 数组` 显式传入 |
| STL 依赖 | `<unordered_map>`, `<list>`, `<string>` (std::allocator) | `<unordered_map>` (重载 operator new) + `EJitAtomic` |

### 2.4 异步编译跑在哪个核？

EJIT 内部封装一个 **`EJitWorker`** 模块,启动时通过 `EJitSreTask` 抽象层创建一个独立 task 承载编译循环。**核绑定由 `EJitSreTask` 的 SRE 实现决定**——业务代码、worker 模块本身、taskpool 业务组件均不感知具体运行在哪个核。

```cpp
// EJitTaskPool 内部启动 worker(由 Runtime 初始化时调用)
EJitTaskPool pool;
pool.setCompiler(&driver::compile, &driver);
pool.startWorker();          // 内部: EJitWorker → EJitSreTask::create

// EJitSreTask::create 的 SRE 实现负责具体核绑定:
//   bool EJitSreTask::create(EJitSreTask &out, EntryFn entry, void *ctx, ...) {
//       sre_task_create(&out.handle_, entry, ctx, SRE_CPU_AFFINITY_2);
//   }
// host 实现则用 std::thread,核绑定由 OS 调度器决定。

// worker 内部循环 (在 EJitWorker::run 中,由 SreTask 入口调用):
//   while (!stopRequested()) {
//       if (!pool_.pollOne()) yield();   // 空闲让出
//   }
```

**调用流向**(单 worker):

```
核 0 (业务)                               核 N (JIT worker, 由 SreTask 决定)
ejit_compile_or_get()                     EJitWorker::run() 循环中
  → miss → taskQueue_.tryEnqueue()          → pool.pollOne()
  → 立即返回 fallback                       → taskQueue_.tryDequeue()
                                            → runCompile()
                                               compileFn_(ctx, req, &fn)
                                            → cache_.publish()
                                            → taskQueue_.release()
```

`pollOne` / `pollBudget` 在生产构建是 taskpool 内部成员,**不导出**;只有定义了 `EJIT_SRE_TASKPOOL_TESTING` 的测试构建(§7.2)才把它们导出到 C ABI,供测试代码不启动 worker 直接驱动队列消费、精确控制时序。集成路径下由内部 `EJitWorker` 自动驱动,业务代码不接触这两个函数。

### 2.5 集成与身份模型

§2.1–2.4 描述 taskpool **内部**调度。本节补充 taskpool 与外部的接线——`funcIndex`/`dimType` 从哪来、worker 何时被 `ejit_init` 拉起、`compileFn_` 回调内部如何桥接到编译层、为何 legacy 与 taskpool 两个 ABI 共存但分流。这些是 v2 相对 v1 的关键集成改动,散落在 `EJitFuncRegistry.h`、`EJitLifecycleRegistry.h`、`EJit.cpp`、`EJitCompileDriver.cpp`、`EJitWrapperGen.cpp` 中。

#### 2.5.1 跨模块身份注册（funcIndex / dimType）

§3.5 的 dedup 把 `funcIndex` 直接当扁平数组下标,§5.1 的 switch 把 `(dimType, instanceId)` 直接当二维数组下标。因此二者必须满足:**(a) 同一函数/生命周期跨独立编译的多个模块恒同;(b) 不同函数/生命周期互不冲突**。

v1 用 FNV-1a 名字哈希:`funcIndex` 4096 槽下 50 函数约 26% 碰撞、200 函数约 99%;`dimType` 仅 8 槽,`fnv("cell")%8 == fnv("tenant")%8`。碰撞落在正确性路径上不可接受。v2 改为**进程级 registry 按名字单调分配稠密 index**,注册期完成、运行时只读。

| 组件 | 位置 | 职责 |
|------|------|------|
| `EJitFuncRegistry` | `EJitFuncRegistry.h`(header-only) | 进程单例,`resolveAssign(name)` 首次见到分配下一个稠密 `funcIndex ∈ [0,4096)`,幂等;容量耗尽返回 `kEJitInvalidFuncIndex` |
| `EJitLifecycleRegistry` | `EJitLifecycleRegistry.h`(header-only) | 进程单例,`resolveAssign(name)` 分配稠密 `dimType ∈ [0,8)`,幂等;第 9 个不同生命周期返回 `kEJitInvalidDimType` |
| AOT IR global | `EJitWrapperGen.cpp` (PASS3) | 每个 entry 函数生成 `@__ejit_funcidx_<name>`(i32,初值 `kEJitInvalidFuncIndex`);每个生命周期生成 `@__ejit_dimtype_<name>`(i32,初值 `kEJitInvalidDimType`),Internal linkage |
| 回填 | `ejit_auto_register` 构造期 | `EJitRegistrationStore`(`EJitRegistrationStore.h`)在 AOT 阶段把每个待回填的 global 地址 + 名字收进静态表;`ejit_init` 消费该表,对每个条目调 registry `resolveAssign(name)`,把分配的 slot 写回对应 IR global。裸核/测试构建另有 `__ejit_registry_funcindex[]`/`__ejit_registry_lifecycle[]` 静态表兜底(与 period 注册同模式) |
| 运行时 | `EJitModuleLoader` | 按 registry index 建 bitcode 表;wrapper 运行时从 global load `funcIndex`/`dimType` |

**三项保证**:

1. **跨模块一致**:同名跨任意模块、任意注册序恒得同一 index
2. **不漂移**:新模块注册不移动已分配的 index(单调计数,只增不改)
3. **clean reject**:容量耗尽返回哨兵,`ejit_init` 失败,绝不静默别名

**并发**:`resolveAssign` 仅在注册期单线程运行(先于任何 wrapper 调用与 worker 启动),lookups 只读,happens-before 所有查询,无需锁。

#### 2.5.2 初始化时序与 Async 门控

worker 不能在「注册未完成 / funcIndex 未回填 / 引擎未就绪」时启动——否则会 race 无锁注册写,或消费永远无人编译的请求。taskpool 构造时 `autoStartWorker=false`,worker 建好但停着,直到 `ejit_init` 完成下列时序后才启动:

```
ejit_init → EJit 构造
  1. 消费所有注册数据 (bitcode / period array / static var / symbol)
  2. funcIndex / lifecycle 回填:
       EJitRegistrationStore 中暂存的所有 (name, globalAddr) → registry.resolveAssign(name) → 写回 IR global
  3. OrcEngine::Create 成功 → setSyncEngine(engine); engineReady=true
  4. regPhase_ = Frozen                      // 冻结注册,杜绝 worker 与无锁注册写竞态
  5. if (config.compileMode == Async):
         if (!engineReady)         → recordInitError  // Async 必须有引擎
         else if (!startTaskPoolWorker()) → recordInitError  // worker 必须启动成功
         else                       → worker running
     else (Sync):
         worker remains stopped (taskpool Off)
```

**门控语义**:Async 缺任一前提(`engineReady` / worker 启动成功)`ejit_init` 直接失败,**绝不暴露一个「能收请求但没人消费」的 taskpool**。注册冻结后,`registerBitcode`/`registerPeriodArray`/`registerStaticVar` 全部拒绝(返回 false),避免单 worker 运行时与无锁 registry 写竞态。

**运行时模式切换** `setCompileMode(mode)` 返回 `bool`:

> **命名注意**: 上层 API 的 `CompileMode` 是 `{Sync, Async}`(`EJitOptions.h`,历史保留),但 taskpool 内部 `EJitCompileMode` 是 `{Off, Async}`(`EJitTaskPool.h`)。**"Sync 模式" 并不是 "走 taskpool 同步编译" ——v2 taskpool 是纯异步的(§1.3),没有同步路径**。两者的实际映射:
>
> | 上层 `CompileMode` | taskpool `EJitCompileMode` | 实际行为 |
> |--------------------|---------------------------|---------|
> | `Async` | `Async` | 业务请求走 taskpool 异步路径(本文档主要描述路径) |
> | `Sync` | `Off` | taskpool 停 worker,业务请求绕过 taskpool,落到 legacy `ejit_compile_or_get` + LRU `EJitCache`(§2.5.4) |
>
> 因此"切到 Sync"实际是"关闭 taskpool,回退到 legacy 路径";真正的同步编译能力由 legacy 路径提供,不在本文档范围内。

| 目标模式 | 前提 | 失败行为 |
|---------|------|---------|
| `Async`(taskpool 启用) | `hasSyncEngine()` 且(worker 已运行或 `startTaskPoolWorker()` 成功) | 返回 false,保留旧模式(避免向无 worker 的 taskpool 入队永久 pending 请求) |
| `Sync`(taskpool 关闭) | 无 | `taskpool.setMode(Off)` + `stopTaskPoolWorker()`。**注意**:setMode(Off) 阻止**新**请求入队,但**已入队、未编完的请求**会被 worker 在 stopWorker 软停止之前正常处理完(包括 publish 到 cache)。需要"立即静默"的场景应等到 `pending_count()==0` 再切换。

#### 2.5.3 编译边界内部桥接

§2.1 把 `compileFn_` 当不透明回调。展开其内部链路(均在 `EJitCompileDriver.cpp`):

```
compileFn_(ctx, req, &fn)
  → taskpoolCompileThunk(ctx, req, &fn)        // 适配:函数指针桥,无 std::function
  → EJitCompileDriver::compileNow(req)
       ├─ 校验 instanceId≤255、无重复 dimType
       ├─ 从 loader_.getOrCacheFuncMeta(req.funcIndex) 读回每维 dimType slot   // 按名字从 registry 读,非重算
       ├─ 把 req.dims 的 instanceId 按 meta.dimTypes 顺序打包成 packedDims[4]
       ├─ cacheKey = (funcIndex<<32) | packedDims                               // 重编码为 legacy u64 key
       └─ compileCold(cacheKey, storeLru=false)
            ├─ loader getFuncName/getBitcodeByFuncIdx
            ├─ 逐维 runtimeState_.isActive 校验时间窗
            ├─ SpecializationContext{fnName, cacheKey, dims, optLevel}
            └─ syncEngine_->loadBitcodeModule → lookup                         // IR pipeline + code pool 封固 → fnPtr
```

**dimType 一致性**:wrapper 烤进 `req.dims` 的 `dimType` 与 `compileNow` 从 `loader` meta 读回的 `dimType` 是**同一个 registry slot**(都按名字读),两侧天然一致。`compileNow` 用 meta 的 `dimTypes` 顺序重新对齐 `instanceId`,保证重编码的 `cacheKey` 与 IR 内 period 维度顺序一致——这是 `(funcIndex,dims)` 与 legacy `u64 cacheKey` 两种身份表示能互译的前提。

**`storeLru` 分流**:taskpool 路径 `storeLru=false`,只 publish 到自己的 32 桶 cache,绕过 LRU `EJitCache`;legacy 同步路径 `storeLru=true` 写 LRU。两条 cache 各自独立(见 §2.5.4)。

#### 2.5.4 两个 ABI 分流

taskpool 构建下两个 C ABI **共存但分流**,各走各的 cache,不可混用:

| ABI | 签名 | cache | 进 taskpool? |
|-----|------|-------|-------------|
| `ejit_compile_or_get(cacheKey, out_pfn)` | u64 单 key | LRU `EJitCache`(`EJitCache.h`,本文档不展开) | **否** |
| `ejit_taskpool_compile_or_get(funcIndex, dims, numDims, outFn, outBucket)` | 显式 dims + bucket | taskpool 32 桶(本文档 §4.1) | **是** |

**为何 legacy 不进 taskpool(安全论证)**:legacy ABI **没有 bucket / release_read 能力**。若让它进 taskpool 的 read-token cache,`lookup` 返回的 `fnPtr` 会在调用方执行它**之前**就被「内部」释放读 token → 此时 `fnPtr` 可能已被 worker 覆盖或释放 → **use-after-free 窗口**(§3.2.4 的读计数外提正是为此)。新 ABI 由 wrapper 持有 `bucket`,在 `outFn(args)` **之后**显式 `release_read`,token 生命周期覆盖整次执行。因此 legacy 仅供向后兼容,新 wrapper 一律走 taskpool ABI。该论证现仅存于 `EJitCompileDriver.cpp` 注释,本节收口。

---

## 3. 基础组件

### 3.1 EJitAtomic：原子操作 wrapper

**位置**：`llvm/include/llvm/ExecutionEngine/EJIT/EJitAtomic.h`

所有原子访问集中于此。taskpool 业务逻辑文件**不直接出现** `std::atomic` 或 `__atomic_*` 内建。

```cpp
template <typename T> class EJitAtomic {
public:
    T loadAcquire() const;           // __atomic_load_n(ACQUIRE)
    T loadRelaxed() const;
    void storeRelease(T v);          // __atomic_store_n(RELEASE)
    void storeRelaxed(T v);
    bool compareExchange(T &expected, T desired);  // strong CAS, ACQ_REL/ACQUIRE
    T fetchAdd(T v);                 // acq_rel
    T fetchSub(T v);
};

using EJitAtomicU8   = EJitAtomic<uint8_t>;
using EJitAtomicU32  = EJitAtomic<uint32_t>;
using EJitAtomicU64  = EJitAtomic<uint64_t>;
using EJitAtomicUPtr = EJitAtomic<uintptr_t>;
```

- 基于 `__atomic_*` 编译器内建，**不** `#include <atomic>`，`EJIT_FREESTANDING` 下可用
- 所有 memory order **显式写明**，便于后续替换为平台内建桩
- 不可拷贝/移动：每个实例标识一个固定内存位置

### 3.2 EJitRwLock：基于原子变量的读写锁

**位置**：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRwLock.h`

`EJitRwLock` 在 `EJitAtomic` 之上封装，使用**两个独立原子变量**实现多读单写。

Cache 按桶粒度分配 RwLock：**每个桶 (32 个) 拥有独立的 `EJitRwLock` 实例**，共 32 个锁。写入桶 5 不影响桶 7 的读者。

```cpp
class EJitRwLock {
    EJitAtomicU32 writeFlag_;    // 0 = 空闲, 1 = 写者持有 / 等待写入
    EJitAtomicU32 readers_;      // 当前活跃读者计数
public:
    // 读侧（热路径，不可阻塞）
    // tryRead 成功后 readers_++ 保持，调用方使用完指针后必须主动调用 readRelease(bucketIndex)
    bool tryRead();              // 检查 writeFlag_，置位则立即返回 false。成功则 readers_++
    void readRelease();          // readers_--（调用方不再使用 fnPtr 时主动调用，与 tryRead 配对）

    // 写侧
    bool tryWrite();             // CAS 抢 writeFlag_，失败立即返回 false
    void write();                // CAS 抢 writeFlag_ → spin 等 readers_→0（已抢到后才等读者退场）
    void writeRelease();         // writeFlag_ = 0（与 write/tryWrite 配对）
};

// 32 个桶，每桶独立锁
EJitRwLock bucketLocks_[EJIT_SRE_TASKPOOL_BUCKETS];   // 默认 32
```

#### 3.2.1 tryRead（热路径读）

```cpp
bool tryRead() {
    if (writeFlag_.loadAcquire() != 0)
        return false;           // 写标志置位，立即回退到 fallback，不等待
    readers_.fetchAdd(1);       // 增加读者计数
    // 双重检查：fetchAdd 后写者可能已 CAS 抢到 writeFlag_
    if (writeFlag_.loadAcquire() != 0) {
        readers_.fetchSub(1);   // 写者已到，退出读者身份
        return false;
    }
    return true;                // 成功获取读权限
}
```

#### 3.2.2 write（spin 写入）

```cpp
void write() {
    // 1. CAS 抢写标志
    uint32_t expected = 0;
    while (!writeFlag_.compareExchange(expected, 1))
        expected = 0;           // spin 等写标志释放
    // 2. 已抢到写标志，等待所有读者退出
    while (readers_.loadAcquire() != 0)
        /* spin */;
    // 3. 此时：writeFlag_=1, readers_=0，独占写入
}
void writeRelease() {
    writeFlag_.storeRelease(0); // 释放写标志
}
```

#### 3.2.3 tryWrite（快速失败写入）

```cpp
bool tryWrite() {
    uint32_t expected = 0;
    if (!writeFlag_.compareExchange(expected, 1))
        return false;           // CAS 失败，立即返回，不等待
    if (readers_.loadAcquire() != 0) {
        writeFlag_.storeRelease(0);  // 有读者，回滚写标志
        return false;
    }
    return true;                // CAS 成功且无读者，立即持锁。调用方用完须 writeRelease()
}
```

#### 3.2.4 为什么读计数必须外提

RwLock 的核心设计约束来自一个简单的事实：**fnPtr 指向的代码可能被 worker 释放**。

```
错误做法（lookup 内部归还）：
  lookup() { tryRead → 读 fnPtr → readRelease → return fnPtr; }
                                                    ↑
                                         worker write() 此时可能写入并释放该 fnPtr
                                         调用方还握着这个地址！

正确做法（调用方主动归还，按桶粒度）：
  lookup() { tryRead(bucket) → 读 fnPtr → return {fnPtr, bucket}; }  // 不归还
  调用方: fnPtr(args);                                                 // 安全使用
  调用方: release_read(bucket);                                        // O(1) 归还
  worker write(bucket) { spin 等该桶 readers_→0 → 覆盖 + 释放旧代码; }
```

没有智能指针或 GC 的 C 环境下，无法自动判断 fnPtr 是否仍被使用。因此将"归还"操作暴露为调用方的显式责任：**谁获取了 fnPtr，谁就在使用完后调用 `release_read(bucketIndex)`**。

这带来的保证：`write(bucketIndex)` spin 到该桶 `readers_=0` 时，该桶所有拿到旧 fnPtr 的调用方均已归还，worker 可以安全释放被覆盖的旧代码。其他桶不受影响。

**配对约束**：调用方对同一 `bucketIndex` 的 `release_read` 必须与 `lookup` **成功命中次数严格 1:1 配对**。同线程在第一次 release 之前可再次 lookup 同桶——若命中，`readers_` 累加为 2，须配对两次 release（嵌套的后续 `tryRead` 可能因写者介入而失败，此为正常行为）。这一约束直接对应 RwLock 的 `readers_` 计数语义：每次 `tryRead` 成功隐含一次 `fetchAdd(1)`。

#### 3.2.5 各场景使用矩阵

| 组件 | 操作 | 接口 | 粒度 | 行为 |
|------|------|------|------|------|
| Cache 读 | tryRead | `tryRead(bucketIndex)` → 调用方用完 fnPtr 后 → `readRelease(bucketIndex)` | **每桶**独立锁 | 写标志置位 → 直接 fallback。成功则 readers_++ 保持，读计数外提 |
| Cache 写 | write | `write(bucketIndex)` → 覆盖旧 fnPtr → 释放旧代码 → `writeRelease(bucketIndex)` | **每桶**独立锁 | CAS 抢标志 → spin 等该桶 readers_→0 → 安全释放旧代码 → 写入 |
| TaskQueue 入队 | 组合操作 | `tryEnqueue` → 内部 dedup CAS + queue push | — | dedup 占位失败 → AlreadyPending；push 失败 → 自动回滚 dedup |
| TaskQueue 出队/释放 | 转发 | `tryDequeue` → queue.pop / `release` → dedup.clear | — | consumer 仅通过 TaskQueue 操作，不直接接触基础组件 |
| Dedup (基础组件) 读 | lock-free | **不使用 RwLock** | — | 直接 `loadAcquire`，仅读 1 bit 占位标志 |
| Dedup (基础组件) 写 | CAS | `tryMarkPending` / `clear` | 单字 CAS | CAS 抢 0→1 占位；storeRelease(0) 回零 |

Dedup 不持有 payload（编译参数全部存放于 queue），因此读者只需要看 1 bit 占位标志，不存在 payload 撕裂问题，无需 RwLock，也无需中间屏障态。业务代码不应直接操作 Dedup，应通过 `EJitTaskQueue` 统一入口。

### 3.3 EJitSreQueue：请求结构 + 无锁队列

**位置**：`llvm/include/llvm/ExecutionEngine/EJIT/EJitSreQueue.h`、`.cpp`

#### 3.3.1 EJitCompileRequest（队列元素）

固定布局 POD，无构造/析构、无 STL，可按值传递：

```cpp
struct EJitCompileRequest {
    uint32_t funcIndex;      // 函数标识
    uint32_t numDims;        // 维度对数量 (≤4)
    uintptr_t fallbackPtr;   // AOT fallback 函数指针
    ejit_dim_pair_t dims[4]; // 维度对数组 (numDims 有效)
    uint32_t versions[4];    // 入队时刻各实例 version 快照
    // hashKey = hash(funcIndex, dims, numDims)  // worker dequeue 时自行计算
};
// aarch64: 16 + 4×8 + 4×4 = 64 字节
```

#### 3.3.2 EJitQueue（有界 MPSC 队列）

多生产者/单消费者(MPSC)。**单消费者(SC)是 Vyukov 无锁正确性的硬约束**——因此 §1.3 决策固定 worker 数量为 1。默认容量 1024，向上取整到 2 的幂。

SRE 平台未提供 queue 原语，本系统使用**自实现的 Vyukov 无锁环形队列**作为唯一后端,host 测试与 SRE 上板共用同一份实现。

**Vyukov 环形队列原理**：

```
buffer_: [slot0] [slot1] ... [slot1023]
           ↑                  ↑
      dequeuePos          enqueuePos
slot = { EJitAtomicU32 sequence, EJitCompileRequest data }
```

**push（多生产者 CAS 抢占）**：

```
pos = enqueuePos
seq = slot[pos & mask].sequence (acquire)
if seq == pos:      → CAS(enqueuePos, pos, pos+1) 抢占 → 写 data → seq.storeRelease(pos+1)
if seq < pos:       → 队列满（生产者追上了消费者一整圈），返回 false
if seq > pos:       → 其他生产者已抢占，重读 enqueuePos
```

**pop（单消费者）**：

```
pos = dequeuePos
seq = slot[pos & mask].sequence (acquire)
if seq == pos+1:    → CAS(dequeuePos, pos, pos+1) 取得 → 读 data → seq.storeRelease(pos+mask+1)
if seq < pos+1:     → 队列空，返回 false
```

pop 端的 CAS 看似支持多消费者,**但 Vyukov 队列的 sequence 编码方案在多消费者并发时会出现"幽灵空"伪信号**(消费者 A 抢到 slot 后还没写 sequence,消费者 B 看到旧 sequence 误判为空,提前返回)。本系统通过 §1.3 worker 数量决策避开这个陷阱。如果将来确实需要多 worker,需要把队列升级到 MPMC 实现(例如 Michael-Scott 队列或带 sequence 双计数器的 MPMC ring)。

### 3.4 EJitSreTask：平台 task 抽象

**位置**：`llvm/include/llvm/ExecutionEngine/EJIT/EJitSreTask.h`、`EJitSreTask_host.cpp`、`EJitSreTask_sre.cpp`

`EJitSreTask` 是平台 task 创建/销毁原语的抽象层,**与 `EJitAtomic` 同级**——头文件定义接口,平台实现分别在 `_host.cpp`(用 `std::thread`)和 `_sre.cpp`(用 SRE 平台原语)中提供,链接时择一。

```cpp
class EJitSreTask {
public:
    using EntryFn = void (*)(void *ctx);

    // 创建并启动 task。entry 在新 task 上下文以 ctx 为参数调用。
    // name 用于平台调试(SRE 任务名 / pthread_setname),可为 nullptr。
    // 返回 true 表示创建成功;失败时 out 保持未初始化。
    static bool create(EJitSreTask &out, EntryFn entry, void *ctx,
                       const char *name = nullptr);

    // 请求软停止并等待 task 退出。task 内部应通过 stopRequested() 自检。
    // 调用后该实例不可再用。幂等。
    static void destroy(EJitSreTask &task);

    // task 内部查询:外部是否已请求停止
    bool stopRequested() const;

private:
    void *handle_ = nullptr;       // host: std::thread*; SRE: 平台句柄
    EJitAtomicU32 stopFlag_;
};
```

**约束**:

- **不支持强停止**:SRE 平台 task 强终止行为各异且容易导致编译中途资源泄漏。仅支持软停止(set flag → task 在循环顶端自检退出)。
- **不暴露核绑定接口**:核绑定是平台属性,在 SRE 实现的 `create` 内部根据上下文决定;host 实现交给 OS 调度器。`EJitSreTask` 接口对核绑定**保持透明**。
- **不可拷贝/移动**:每个实例对应一个固定 task 句柄。

### 3.5 EJitDedupTable：CAS 占位表

**位置**：`llvm/include/llvm/ExecutionEngine/EJIT/EJitDedupTable.h`

`EJitDedupTable` 是**通用基础组件**——按整数 key 做 CAS 占位/释放,不感知编译语义。去重粒度是 **仅 `funcIndex`**，由于 `funcIndex` 本身是整数，**直接作为数组下标**——不需要分桶，不需要扫描，不需要状态机：

```cpp
// 扁平数组：1 bit 占位，0 = 空闲，1 = 已有 in-flight
EJitAtomicU32 inFlight_[MAX_FUNC_INDEX];
```

#### 3.5.1 为什么是 1 bit 而不是状态机

dedup 的唯一职责是**"防止同一 key 同时被标记为 in-flight"**。在当前架构下：

- **payload 全部由 queue 持有**：`EJitCompileRequest` 完整结构体入队（§3.3.1），dedup 不存任何编译参数
- **跨核可观察由 queue 提供**：任何核扫 queue 的 ring buffer 即可看到所有待处理工作
- **失效由上层调度保证**：version bump 后 worker 在检查点丢弃过时结果（§5.3），不需要 dedup 参与

producer 和 consumer 对 dedup 的全部诉求是一个二元判断："这个 key 当前有没有人在做？" 1 bit 占位完全表达了这个语义。

**为什么只按 funcIndex 去重,不按 (funcIndex, dims) 去重**

dedup 粒度的取舍。同一函数 `func=5`,两个请求 dims 不同(`[(0,3)]` vs `[(0,7)]`)——它们是**不同的特化版本**。按 `funcIndex` 去重的话:

```
T0:  producer A:  compile(func=5, dims=[(0,3)])  → dedup[5]: 0→1 (Claimed) → 入队
T1:  producer B:  compile(func=5, dims=[(0,7)])  → dedup[5]==1 → AlreadyPending → fallback
T2:  worker:      取 A 编译完 → publish A → dedup[5]: 1→0
T3:  producer B (下次调用): compile(func=5, dims=[(0,7)]) → dedup[5]: 0→1 → 入队
```

B 在 T1 没能为自己排队,而是被当成 A 的重复;直到 worker 处理完 A、B 下次调用时才进队。**B 走了一次额外的 AOT fallback**。这是按 funcIndex 去重的代价。

**为什么仍这么选**:

1. **单 worker 串行编译**:即使 (funcIndex, dims) 粒度去重把 A、B 都入队,worker 也只能一个个编。总编译时间 = N 个不同 dims 的编译耗时之和,粒度细化不缩短;只改变"何时排队"。
2. **dedup 复杂度**:funcIndex 直接做扁平数组索引(§3.5.2),O(1) 无 hash 无扫描。改成 (funcIndex, dims) 需要 hash 表或 4096×N 二维数组——前者增加并发复杂度,后者按最坏维度组合膨胀(8 dimType × 256 instance × 4096 funcIndex 显然不可接受)。
3. **业务模式假设**:多 dims 的初次调用通常**不在性能关键路径**——业务侧首次触发各 cell 时本来就允许 fallback,典型场景下用户更在乎"稳态后命中率",不是"冷启动期每次都命中"。

**何时这个取舍会出问题**:如果业务高频在同函数的不同 dims 间切换、且每次切换都希望立即命中(不能容忍一次 AOT fallback),粒度需要细化。当前设计假设不是这种场景。

#### 3.5.2 接口

```cpp
class EJitDedupTable {
    EJitAtomicU32 inFlight_[MAX_KEYS];   // 0 = 空闲, 1 = 占位中
public:
    // 尝试占位。CAS(0 → 1)
    //   返回 true：占位成功，调用方负责后续操作或 clear 回滚
    //   返回 false：已有 in-flight，调用方走 AlreadyPending
    bool tryMarkPending(uint32_t key);

    // 释放占位。storeRelease(0)
    //   场景：处理完成 / 失配丢弃 / 后续操作失败回滚
    void clear(uint32_t key);
};
```

热点路径仅一次 `compareExchange(0, 1)`，O(1) 无扫描。

#### 3.5.3 协作时序

```
producer:
  if (!dedup.tryMarkPending(key))        // CAS 0 → 1
      return AlreadyPending;
  if (!后续操作()) {
      dedup.clear(key);                   // 回滚占位
      return Failure;
  }
  return Success;

consumer (worker):
  取出工作;
  if (失配)           { dedup.clear(key); return; }
  处理();
  if (失配 || 失败)   { dedup.clear(key); return; }
  publish();
  dedup.clear(key);                        // 收尾
```

整个生命周期内 dedup 只有两种操作：CAS 占位、storeRelease 释放。无中间状态。

#### 3.5.4 与 queue 的职责分工

| 职责 | dedup | queue |
|------|-------|-------|
| 防止同 key 重复处理 | ✅ | — |
| 持有完整 payload | — | ✅ |
| 跨核观测待处理工作 | — | ✅ |
| 表达"谁在做、做到哪一步" | — | — |

第三行的"做到哪一步"既不在 dedup 也不在 queue——这是 consumer 自己的局部状态，不对外暴露。Dedup 保持 1 bit 精简，不为外部可观察性膨胀。

---

## 4. 结果缓存

Cache 用 32 桶,每桶一个 `unordered_map<hashKey, vector<EJitCacheEntry>>` + 独立 `EJitRwLock`。hashKey 仅用于定位桶,**身份匹配靠 vector 内 identityMatches** 解 hash 冲突(详见 §4.1)。

### 4.1 EJitTaskPoolCache

#### 4.1.1 hashKey 计算

hashKey 由 golden ratio hash 把 funcIndex 与各维 (dimType, instanceId) 混合,只用于选桶,**不参与身份判定**:

```cpp
uint64_t hashKey(uint32_t funcIndex, const ejit_dim_pair_t* dims, uint32_t numDims) {
    uint64_t key = funcIndex;
    for (uint32_t i = 0; i < numDims; i++) {
        key ^= ((uint64_t)dims[i].dimType << 32) | dims[i].instanceId;
        key *= 0x9e3779b97f4a7c15ULL;  // golden ratio
    }
    return key;
}
```

分桶公式:

```cpp
bucket = hashKey % EJIT_SRE_TASKPOOL_BUCKETS   // 默认 32
```

#### 4.1.2 桶与 entry 结构

32 个桶，每桶一个 `unordered_map` + 独立 `EJitRwLock`。**map 的 value 是 `vector<EJitCacheEntry>`**——同一 hash key 可能对应多个不同 identity（hash 冲突），靠 vector 内全身份匹配兜底：

```cpp
struct Bucket {
    EJitRwLock lock;
    std::unordered_map<uint64_t /*hashKey*/, std::vector<EJitCacheEntry>> entries;
};

Bucket buckets_[32];
```

每个函数最多关联 4 个生命周期维度，IR 中静态确定。entry 存储完整身份（funcIndex + 全部 dims）+ 每维 version 快照，**hash 冲突场景下靠 identityMatches 区分**：

```cpp
struct EJitCacheEntry {
    uint32_t funcIndex;        // 完整身份 (1): funcIndex
    uint32_t numDims;          // 关联维度数 (≤4)
    EJitDimPair dims[4];       // 完整身份 (2): {dimType, instanceId} × numDims
    uint32_t versions[4];      // 编译时刻每维 version 快照
    uintptr_t fnPtr;           // JIT 编译出的函数指针
};

bool identityMatches(const EJitCacheEntry &E, funcIndex, dims, numDims) {
    return E.funcIndex == funcIndex && E.numDims == numDims
        && (逐维 E.dims[i] == dims[i]);
}
```

**hash 冲突处理**：golden-ratio hash 是 64-bit,但仍**可能**碰撞。同一 `bucket.entries[key]` 的 vector 可挂载多个不同 identity 的 entry,lookup/publish 都遍历该 vector 并以 `identityMatches` 精确匹配——不同 identity 的请求**不会互相覆盖、不会假命中**。

**分桶的核心目的**：限制 `unordered_map` rehash 的爆炸半径。单全局 map rehash 会阻塞**所有**读者；分 32 个桶后，单桶 rehash 最多阻塞该桶的读者，其余 31 个桶不受影响。

**lookup**（try-read 语义，hashKey 内部计算）：

```
lookup(funcIndex, dims, numDims) → {fnPtr, bucketIndex, hasReadToken}：
  1. hashKey = hash(funcIndex, dims, numDims)
  2. bucketIndex = hashKey % 32
  3. buckets_[bucketIndex].lock.tryRead()  → 失败则立即返回 miss(hasReadToken=false)
  4. it = buckets_[bucketIndex].entries.find(hashKey)
     → 未找到 → miss,readRelease()
  5. for E in *it->second:                       // 遍历 vector,处理 hash 冲突
       if (!identityMatches(E, funcIndex, dims, numDims)) continue
       逐维度比对 version:
         for i in 0..E.numDims:
             cur = switch_.getInstanceVersion(E.dims[i].dimType, E.dims[i].instanceId)
             if cur != E.versions[i] → 该 entry 已陈旧,break(回到 miss)
       全部匹配 → 命中,返回 {fnPtr=E.fnPtr, bucketIndex, hasReadToken=true}
  6. 没有 identityMatches 的 entry → miss,readRelease()

命中时 lookup 不归还 read token。调用方用完 fnPtr 后:
  result = cache_.lookup(funcIndex, dims, numDims);
  result.fnPtr(args, ...);
  ejit_taskpool_release_read(result.bucketIndex);
```

**开关失效**：`set_instance_enabled(dim, id)` → version++ → 任何包含该实例的 cache entry 在步骤 5 的逐维 version 比对失败 → 视为 miss。publish 时同 identity 直接覆盖 entry.fnPtr。零额外清理开销。

**为什么返回 bucketIndex**：`release_read(bucketIndex)` 直取 `buckets_[bucketIndex].lock.readRelease()`，O(1)。

**publish**（write 语义，单 worker 线程执行）：

```
publish(funcIndex, dims, numDims, req.versions[], fnPtr):
  1. hashKey   = hash(funcIndex, dims, numDims)
  2. bucket    = hashKey % 32
  3. buckets_[bucket].lock.write()   // spin 等该桶 readers_→0,§3.2.2
  4. 提交门重验: for i in 0..numDims:
       if req.versions[i] != switch_.getInstanceVersion(dims[i]):
         writeRelease + return VersionMismatch     // 锁内 toggle 兜底,§5.3
  5. for E in buckets_[bucket].entries[hashKey]:    // vector 链上找同 identity
       if identityMatches(E, funcIndex, dims, numDims):
         oldFn = E.fnPtr
         E.versions[i] = req.versions[i]            // 用提交门已重验的快照
         E.fnPtr = fnPtr                            // 覆盖
         writeRelease
         if oldFn && oldFn != fnPtr && releaseFn_:
           releaseFn_(oldFn)                        // ★锁外释放,见下方注释
         return Published
  6. 未命中 identity (冲突链上无同 identity 或链为空):
       buckets_[bucket].entries[hashKey].push_back({funcIndex, dims, versions, fnPtr})
       writeRelease
       return Published
```

**为什么旧 fnPtr 在锁外释放**：`releaseFn_` 可能回调 code pool / ORC / 平台分配器,**禁止**在桶写锁内执行(§10.1 短临界区约束)。`write()` 已 spin 到该桶 readers_=0 + entry 已指向新 fnPtr,因此旧 fnPtr 对该桶后续 lookup 不可达,锁外释放安全。


### 4.2 EJitTaskQueue：去重的任务提交管理

**位置**：`llvm/include/llvm/ExecutionEngine/EJIT/EJitTaskQueue.h`

`EJitTaskQueue` 是**业务组件**——它组合两个基础组件（`EJitQueue` + `EJitDedupTable`），向上提供去重的任务提交/消费统一接口。TaskQueue 是 producer（`compileOrGet`）和 consumer（`EJitWorker`）操作队列的唯一入口。

```cpp
class EJitTaskQueue {
    EJitQueue        queue_;   // MPSC 无锁环形队列 (§3.3)
    EJitDedupTable   dedup_;   // CAS 占位表 (§3.5)
public:
    // ===== Producer 侧 =====
    // 去重入队：内部先 dedup.tryMarkPending → 成功则 queue.push
    //   占位失败 → AlreadyPending
    //   push 失败 → 自动回滚 dedup.clear，返回 QueueFull
    //   push 成功 → Enqueued
    EnqueueResult tryEnqueue(const EJitCompileRequest &req);

    // ===== Consumer 侧 =====
    // 出队：直接转发 queue.pop。返回 true 表示取到工作。
    bool tryDequeue(EJitCompileRequest &out);

    // 释放占位：dedup.clear。worker 编译完成/失配丢弃时调用。
    void release(uint32_t funcIndex);
};
```

**设计要点**:

- **去重与入队原子化**：`tryEnqueue` 内部保证 dedup 占位 + queue push 为一个不可分割的操作序列——push 失败时自动回滚 dedup，调用方无需手动协调两者的生命周期
- **出队与释放分离**：`tryDequeue` 只从 queue 取数据，不操作 dedup；`release` 由 consumer 在合适的时机显式调用（编译完成 / 失配丢弃）。这保留了 consumer 在"取出工作"到"确认完成"之间自由执行重操作（编译）的能力
- **对基础组件透明封装**：TaskQueue 不改变 Queue 和 Dedup 的并发语义——Queue 仍为 MPSC、Dedup 仍为 CAS 占位。TaskQueue 只是把两者的调用编排成两个高层接口

**协作时序**：

```
producer (compileOrGet):
  result = taskQueue_.tryEnqueue(req);
  // 内部: dedup.tryMarkPending → queue.push (失败自动 clear)
  switch (result):
    Enqueued        → return fallback (等待 worker 处理)
    AlreadyPending  → return fallback (同 funcIndex 已在编)
    QueueFull       → return fallback (队列满，dedup 已回滚)

consumer (worker / runCompile):
  if (!taskQueue_.tryDequeue(req)) return;    // 队列空
  if (version 失配)        { taskQueue_.release(req.fi); return; }
  ok = compileFn_(req);
  if (version 失配 || !ok) { taskQueue_.release(req.fi); return; }
  cache.publish(req, fn);
  taskQueue_.release(req.fi);                  // 收尾
```

**与直接操作 Queue + Dedup 的对比**：

| | 直接操作 | 通过 TaskQueue |
|------|--------|------|
| producer 入队 | 手动 dedup.tryMarkPending → queue.push → 失败手动 clear | `tryEnqueue` 一步完成 |
| consumer 操作 | 手动 queue.pop + 多处 dedup.clear | `tryDequeue` + 统一 `release` |
| 回滚逻辑 | 分散在 compileOrGet 和 runCompile 多处 | 封装在 tryEnqueue / release 内 |
| Queue/Dedup 类型依赖 | 调用方直接 #include 两个基础组件头文件 | 调用方只依赖 TaskQueue 一个头文件 |

---

## 5. 调度器逻辑

### 5.1 EJitSwitchController

每个生命周期实例 `(dimType, instanceId)` 独立控制。**enabled 标志和 version 使用两个独立的原子变量**，避免在同一条原子操作中混合两个语义：

```
enabled_[dimType][instanceId]:  EJitAtomicU8   (0=禁用, 1=启用)
version_[dimType][instanceId]:  EJitAtomicU32  (单调递增，仅 enabled 状态变化时 +1)
```

容量(`MAX_DIM_TYPES=8`、`MAX_INSTANCES=256`、内存占用)归口在 §6.3,本节不重复。

```cpp
static_assert(MAX_DIM_TYPES == 8 && MAX_INSTANCES == 256);

class EJitSwitchController {
    EJitAtomicU8  enabled_[MAX_DIM_TYPES][MAX_INSTANCES];  // 2 KiB
    EJitAtomicU32 version_[MAX_DIM_TYPES][MAX_INSTANCES];  // 8 KiB
    EJitAtomicU32 mode_;  // Off=0 / Async=1
public:
    // ===== 热路径：直接读取，无位移 =====
    bool isInstanceEnabled(uint32_t dimType, uint32_t instanceId) {
        return enabled_[dimType][instanceId].loadRelaxed();
    }
    uint32_t getInstanceVersion(uint32_t dimType, uint32_t instanceId) {
        return version_[dimType][instanceId].loadAcquire();
    }

    // ===== 控制面：一次 CAS(enabled)，无循环 =====
    // 仅当 enabled 实际发生变化时，version 递增。
    // CAS 失败 = 已是目标状态 → no-op，version 保持不变。
    void setEnabled(uint32_t dimType, uint32_t instanceId, bool wantOn) {
        uint8_t expected = wantOn ? 0 : 1;   // 期望当前是相反状态
        uint8_t desired  = wantOn ? 1 : 0;
        if (enabled_[dimType][instanceId].compareExchange(expected, desired))
            version_[dimType][instanceId].fetchAdd(1);
    }
};
```

**热路径优化**：`isInstanceEnabled` 和 `getInstanceVersion` 都是纯 load，无需 `>> 1` 或 `& 1` 位移——因为 enabled 和 version 各自独立在专用原子变量中。

**为什么不用合并 U32（CAS 循环）？**

合并方案中 enabled 和 version 共享一个 U32 字，`setEnabled` 必须用 CAS 循环：load → 拆 version+enabled → 算新值 → CAS → 失败则重试。而拆分方案中 `setEnabled` 仅一次 CAS(enabled, old→new) 且从不循环——因为 enabled 从 0→1 (或 1→0) 只要成功一次就达到目标，中间没有需要重算的依赖。version 只在 CAS 成功后才 `fetchAdd`，是一个独立的原子操作，不参与 CAS 的条件判断。

**两个原子变量之间的窗口**：

```
disable 路径:
  CAS(enabled, 1→0) 成功  ← enabled 先变
  极小窗口...
  fetchAdd(version, 1)     ← version 后变

窗口内: enabled=0, version=旧值
```

分析两个方向：

- **窗口内有 producer 进来**：`isInstanceEnabled` 看到 enabled=0 → 直接返回 InstanceDisabled，不经过 version 比对。安全。
- **窗口内并发 enable/disable**：另一个线程 CAS(enabled, 0→1) 会成功或失败，version 最终各自 fetchAdd 一次，总增量正确。可能出现一次多余失效（version 连续涨了两次而中间没有实际编译产物），但不会造成错误命中。

**连续 toggle 的 version 单调性保证**：

`version_[][]` 通过 `fetchAdd(1)` 递增——这是一个 acq_rel 原子操作，**永远不回退**。考虑最坏情况：N 个线程并发对同一 `(dimType, instanceId)` 调用 `setEnabled`（连续 "关→开→关→开…"）：

- 每次状态实际翻转（CAS 成功）恰好对应一次 `fetchAdd(1)`
- 已是目标态的 setEnabled CAS 失败 → no-op，**不消耗 version 增量**
- N 次成功翻转 → version 累计 +N，严格单调

因此任何已入队请求快照的 `req.versions[i]` 一旦被一次成功 toggle "甩在身后"，就**永远无法再次匹配** current version。worker 检查点 1/2 的失配判定不会因为"先关后开恢复到原状态"而误判——状态可能恢复，但 version 不会。同样地，cache entry 中存储的 `dims[i].version` 一旦被 toggle，对应实例的 lookup 比对就**单调地、永久地** miss，直到下一次 publish 写入新 version 快照覆盖该 entry。

**为什么 version 用 32-bit？**

`EJitAtomicU32` version 回绕周期 ~2.1×10⁹ 次 toggle，实际不可能触发。enabled 用 U8 是因为它仅表达布尔值，1 字节已足够，且 `__atomic_compare_exchange_1` 原生支持。

**关闭时的三层全懒失效**：

```
setEnabled(0, 3, false)              // CAS(enabled 1→0) + fetchAdd(version)
  ↓
第一层 Cache：新 lookup → 逐实例比对 version → 发现不匹配 → 自动 miss
第二层 Queue：worker dequeue 时逐实例比对 req.versions[] → 不匹配 → 丢弃
第三层 In-flight：runCompile gate 前逐实例比对 → 不匹配 → 丢弃，不 publish
```

**原理**：cache entry 存储编译时的 `dims[].version` 快照。toggle 仅 bump 一个实例的 version → 下一次 lookup 遍历 entry.dims[] 时发现该实例 version 不匹配 → miss。

### 5.2 compileOrGet：统一入口

```cpp
CompileOrGetResult compileOrGet(funcIndex, dims, numDims, fallback) {
    // 1. 参数检查
    if (numDims > 4 || (numDims > 0 && !dims))
        return {InvalidParam, fallback};

    // 2. 维度开关检查
    for each (dimType, instanceId) in dims:
        if (!switch_.isInstanceEnabled(dimType, instanceId))
            return {InstanceDisabled, fallback};

    // 3. cache hit (内部 hash → bucket → 遍历 vector 全身份匹配 → 逐维 version)
    EJitCacheLookupResult hit = cache_.lookup(funcIndex, dims, numDims);
    if (hit.hasReadToken && hit.fnPtr)
        return {CacheHit, hit.fnPtr, hit.bucketIndex, hasReadToken=true};

    // 4. Off 模式
    if (switch_.getMode() == EJitCompileMode::Off)
        return {OffMode, fallback};

    // 5. 构造请求 + 去重入队 → 立即返回
    EJitCompileRequest req{funcIndex, dims, numDims, fallback};
    for i in 0..numDims:
        req.versions[i] = switch_.getInstanceVersion(dims[i].dimType, dims[i].instanceId);
    switch (taskQueue_.tryEnqueue(req)) {
        case Enqueued        : return {EnqueuedPending,   fallback};
        case AlreadyPending  : return {AlreadyPending,    fallback};
        case QueueFull       : return {QueueFullFallback, fallback};
        case InvalidFuncIndex: return {InvalidParam,      fallback};  // funcIndex >= 4096
    }
}
```

返回值状态全集（与 `enum class EJitCompileOrGetStatus` 对齐）:

| 状态 | fnPtr | bucketIndex | hasReadToken | 含义 |
|------|-------|-------------|--------------|------|
| `CacheHit` | JIT 函数指针 | 有效 | **true** | 命中缓存。调用方用完 fnPtr 后**必须** `release_read(bucketIndex)` |
| `OffMode` | fallback | 0 | false | Taskpool 全局 Off 模式 |
| `InstanceDisabled` | fallback | 0 | false | 请求的某个生命周期实例被禁用 |
| `EnqueuedPending` | fallback | 0 | false | 异步入队成功，等待 worker 编译 |
| `AlreadyPending` | fallback | 0 | false | 同 funcIndex 已有 in-flight |
| `QueueFullFallback` | fallback | 0 | false | 队列满，dedup 已自动回滚 |
| `CompileFailed` | fallback | 0 | false | 异步编译失败/version 中途变更(由 worker 路径产生,反映在 stats) |
| `InvalidParam` | fallback | 0 | false | numDims>4 / dims==null / dimType≥8 / instanceId≥256 / funcIndex≥4096 |

> `hasReadToken=true` 仅在 `CacheHit` 时出现,是「**必须** release_read」的唯一信号。所有其他状态下调用方都不持有 token,**禁止** release_read(§3.2.4 1:1 配对约束)。

### 5.3 runCompile：编译执行路径

由 worker `EJitWorker::run` 循环通过 `pool.pollOne()` 间接调用。`pollOne`/`pollBudget` 在生产构建是 taskpool 内部成员函数,不暴露为 C ABI;只有测试构建(§7.2 `EJIT_SRE_TASKPOOL_TESTING`)才把它们导出到 C ABI 供测试代码手动驱动。

核心流程 + 两次 version 检查:

```cpp
void runCompile() {
    // 0. 从 TaskQueue 取工作
    EJitCompileRequest req;
    if (!taskQueue_.tryDequeue(req)) return;
    // hashKey = hash(req.funcIndex, req.dims, req.numDims)  // 由 cache 内部计算

    // 1. 检查点 1: 入队后到编译开始前的失效 — 逐维比对 req.versions vs 当前 version
    if (!versionsMatch(req)) {
        taskQueue_.release(req.funcIndex);
        return;                                       // 丢弃,不编译
    }

    // 2. 编译 (跨边界回调到 EJitCompileDriver,§2.5.3)
    void *fn = nullptr;
    bool ok = compileFn_ && compileFn_(compileCtx_, req, &fn);

    // 3. 编译失败先于检查点 2 判断 — 省一次 version 比对
    if (!ok || !fn) {
        taskQueue_.release(req.funcIndex);
        return;
    }

    // 4. 检查点 2: 编译期间的失效
    if (!versionsMatch(req)) {
        cache_.retireCode(fn);                        // 通过 releaseFn_ 释放陈旧码
        taskQueue_.release(req.funcIndex);
        return;
    }

    // 5. 提交门发布 (§4.1 publish): 持桶写锁后再次重验 version,失配则不覆盖
    switch (cache_.publish(req.funcIndex, req.dims, req.numDims, req.versions, fn)) {
        case Published:
            taskQueue_.release(req.funcIndex);
            return;
        case VersionMismatch:
        case InvalidParam:
        case Failed:
            cache_.retireCode(fn);                    // 释放未发布的陈旧/失败码
            taskQueue_.release(req.funcIndex);
            return;
    }
}
```

两个检查点保证了 **实例开关竞态安全**(见 §5.4)。TaskQueue 不参与 version 校验,`release` 只承担去重占位释放;版本失效完全由 SwitchController + 这两个检查点 + publish 提交门(§4.1)共同完成。

**三层 version 检查的层级:哪层是"必须",哪层是"优化"**

总共三层 version 检查,**但它们的角色不对等**:

| 层 | 位置 | 作用 | 不可省 / 可省 |
|----|------|------|------|
| **检查点 1** | dequeue 后,编译前 | 入队等待期间发生 toggle → 直接丢弃,**省一次编译**(嵌入式场景 ~10-100ms) | **可省**(优化层)。漏掉时:多做一次编译,结果在检查点 2 / 提交门被拦下,正确性不受损。toggle 频率高时收益大,低时几乎无收益 |
| **检查点 2** | 编译后,publish 锁前 | 编译期间发生 toggle → 立即 retire,**省一次桶写锁竞争 + 提交门 spin**(几 µs 级) | **可省**(优化层)。漏掉时:陈旧码会去 publish 拿锁,被提交门拦下 retire,正确性不受损。边际收益最低——锁竞争开销远小于检查点 2 拦下的"重复 retireCode 调用"代价 |
| **publish 提交门** | publish 持桶写锁后立即重验 | "检查点 2 → 拿写锁" 期间的 toggle → `VersionMismatch` 不写 entry | **必须**(正确性闸)。**没有它**任何陈旧 fnPtr 都可能被盖上发布瞬间的 version 戳,后续 lookup 误命中 |

**最小满足正确性**:只需 publish 提交门。
**当前实现**:三层全开,前两层是早期发现失效的优化,publish 提交门是兜底。

**取舍依据**:嵌入式场景一次编译 ~10-100ms,检查点 1 在"业务 toggle 频率 >> 1/编译时间"时显著收益(每次 toggle 命中检查点 1 省的一次编译 ≈ 100ms,vs 检查点 1 自身开销 ~10ns)。检查点 2 边际价值最低,如果未来需要简化代码,可以先删检查点 2。

如果业务侧 toggle 频率极低(分钟级以上),三层中只保留 publish 提交门也足够;但目前三层全开**总开销 ≤ 100 ns/请求**(三次原子读),不构成性能负担,代码精简价值不大。

**检查点 2 与 publish 写锁之间的窗口**:检查点 2 通过后、`cache.publish` 拿到桶写锁之前,仍可能再次 toggle。这一窗口由 publish 的**提交门**(§4.1 publish 步骤 4)兜底——持桶写锁的瞬间逐维重验 `req.versions[i]` vs current version,任一不匹配返回 `VersionMismatch`,**不**写 entry,worker 走 `retireCode` 回收陈旧码。从而:

- 发布的 entry 必然带着发布瞬间的 current version 快照,后续 lookup 比对失败即 miss。
- 不存在"陈旧 fnPtr 被盖上新 version 标记"的窗口——这种结果会被提交门拦下。
- 提交门通过之后立即又 toggle 也不构成正确性问题:entry 持有的 version 此刻确为最新,下一次 lookup 自然 miss,等下一次 publish 同 identity 覆盖(§4.1)。

### 5.4 实例开关失效机制

逻辑失效不依赖逐 entry 释放,统一通过 `EJitSwitchController::setEnabled`(§5.1) 触发——一次成功的 enabled 翻转 bump 对应实例的 version,**所有**引用该实例的 cache entry 在下一次 lookup 时自然 miss,所有 in-flight 请求在 worker 检查点丢弃。本节展开这条路径并说明 `freeCode` 的边界。

`freeCode`(即 cache 的 `releaseFn_` 回调)**不参与失效路径**,仅在两个场景被调用:

1. **模块 destroy**:`EJitTaskPoolCache::shutdown` 把所有 entry 的 fnPtr 收集后,在桶锁全部释放后统一调 `releaseFn_`。
2. **publish 覆盖**:同 identity 写入新 fnPtr 时,旧 fnPtr 在桶 `writeRelease` 之后调 `releaseFn_`(`write()` 已 spin 到 `readers_=0`,旧 fnPtr 不再可达)。

详见 §4.1 publish 与 §10.4。

**失效路径**:

```
setEnabled(dimType=0, instanceId=3, wantOn=false)        // §5.1
  → CAS(enabled_[0][3], 1→0) 成功
  → fetchAdd(version_[0][3], 1):  5 → 6
  ─────────────────────────────────────────────
  第一层 Cache  : 引用 (0,3) 的 entry.versions[i]=5 ≠ 当前 6
                  → lookup 步骤 5 逐维 version 比对失败 → miss (§4.1)
  第二层 Queue  : worker tryDequeue 后检查点 1
                  req.versions[i]=5 ≠ 6 → release + 丢弃,不编译 (§5.3)
  第三层 In-flight: 检查点 2 / publish 提交门
                  → retireCode + 丢弃,不 publish (§4.1, §5.3)
```

**与 runCompile 的竞态**:`setEnabled` 在 worker 编译进行中触发 → version bump → worker 在检查点 1 / 检查点 2 / publish 提交门**三处任一**发现 `versions[i]` 不匹配 → `taskQueue_.release` + 不写 cache。三层中只要一层捕获,陈旧结果就不会进入 cache。

**物理槽位回收**:version 失效只是逻辑 miss,不删除 map entry。物理回收靠下一次同 identity 的 publish 覆盖——cache 自身不主动收集"已失效但未覆盖"的 entry(无定时清理、无 GC)。这是有意取舍:同名特化迟早会再次发布,陈旧 entry 在内存中"饿死"一段时间是可接受的;若需更激进的回收,可在 `releaseFn_` 注入侧实现。

### 5.5 EJitWorker：调度循环模块

`EJitWorker` 把"消费 TaskQueue + 调用 runCompile"封装成独立模块,跑在由 `EJitSreTask` 创建的独立 task 上。本系统采用**单 worker 模型**(§1.3 决策),依赖此假设维持 Vyukov 队列的 SC 正确性。

**职责**:

- 在初始化时启动 worker task(`EJitSreTask::create`)
- 提供 task 入口函数,在循环里轮询 `taskQueue_.tryDequeue()` → 空闲让出
- 在销毁时请求软停止并等待 task 退出(`EJitSreTask::destroy`)
- 维护 worker 局部统计(已处理数、空轮询数、运行标志)

**接口**:

```cpp
class EJitWorker {
public:
    explicit EJitWorker(EJitTaskPool &pool, const char *name = "ejit-worker");
    ~EJitWorker();   // 内部调用 stop()

    // 启动 worker task。失败返回 false。
    // 已经在运行时再次调用是 no-op。
    bool start();

    // 请求软停止并等待退出。幂等。
    void stop();

    // 监控
    bool   isRunning() const;
    uint64_t processedCount() const;   // 已成功处理的请求数
    uint64_t spinCount() const;        // 空轮询次数(队列为空)

private:
    static void taskEntry(void *ctx);  // SreTask 入口,转发到 run()
    void run();                        // 实际循环

    EJitTaskPool &pool_;
    const char  *name_;
    EJitSreTask  task_;
    EJitAtomicU64 processed_;
    EJitAtomicU64 spins_;
    EJitAtomicU32 running_;
};
```

**循环逻辑**:

```cpp
void EJitWorker::run() {
    running_.storeRelease(1);
    while (!task_.stopRequested()) {
        if (pool_.pollOne()) {          // 内部: taskQueue_.tryDequeue → runCompile
            processed_.fetchAdd(1);
        } else {
            spins_.fetchAdd(1);
            // 空闲让出。具体策略由实现选择:
            //   - 简单版: 让 CPU(yield/pause)
            //   - 优化版: 核间信号量等待(§1.2 提到的可选项)
        }
    }
    running_.storeRelease(0);
}
```

**生命周期与 EJitTaskPool 的耦合**:

- `EJitTaskPool` 持有 `EJitWorker worker_`(值类型成员,非可选;与 taskpool 同生共死)
- taskpool 构造时 worker 已建好,但是否立即启动由 `autoStartWorker` 决定:**集成路径下传 `false`**(`EJitCompileDriver` 这么干),由 `ejit_init` 完成注册冻结 + 引擎就绪后再调 `startWorker()`(§2.5.2);测试可传 `true` 即起即用
- 析构时 `EJitTaskPool::~EJitTaskPool` 先 `stopWorker()` 再 `cache_.shutdown()`,保证 worker 不会在 cache 拆除期间还在 publish
- **Worker 不暴露给 C ABI**——和 Cache/TaskQueue 一样,是 taskpool 内部组件

**单 worker 假设的传递路径**:

```
EJitWorker::run() 中只此一处 pollOne(生产构建中是唯一调用方)
       ↓
EJitTaskPool::pollOne() → taskQueue_.tryDequeue() → runCompile
       ↓
EJitQueue::pop()  ← Vyukov MPSC 的 SC 端
```

只要 `startWorker` 在整个 taskpool 生命周期内**最多调用一次成功**(再次调用返回 no-op),且生产构建不暴露任何外部 pop 入口,就不会出现两个并发 pop 路径。`pollOne` / `pollBudget` 仅在测试构建(§7.2 的 testing-only 接口)下可用,与内部 worker 不同时启用。

**与 §10 核间语义的关系**:worker 跑在独立核时,worker 与 producer 的所有共享状态(queue cell、dedup 占位、cache entry、SwitchController version)都通过 `EJitAtomic` 的 acquire/release 配对发布,见 §10.2。

---

## 6. 容量模型与约束

### 6.1 hash 分布与弹性容量

hashKey 由 golden ratio hash 生成(§4 hashKey 计算),`bucket = hashKey % 32`。每桶 `unordered_map<u64, vector<EJitCacheEntry>>` 弹性增长,无硬上限;hash 冲突由 vector 内 `identityMatches` 解(§4.1)。总容量受平台内存约束。

### 6.2 rehash 隔离

单桶 `unordered_map` rehash 时，该桶 `write()` 持有写锁（publish 路径），或 `unordered_map` 内部触发 rehash（insert 时）。无论哪种，只阻塞该桶的读者，其余 31 个桶不受影响。

这就是分桶的核心价值——不是容量问题，是延迟隔离。

### 6.3 各层约束

| 边界 | 值 / 标识 | 说明 |
|------|---------|------|
| 单 funcIndex 在飞请求 | 1 | TaskQueue 去重(Dedup 1 bit 占位,§3.5) |
| funcIndex 总数上限 | `kEJitMaxFuncIndex` = 4096(`EJIT_SRE_TASKPOOL_MAX_FUNC_INDEX`) | Dedup `inFlight_[]` 扁平数组容量;耗尽时新名字 → `kEJitInvalidFuncIndex`,wrapper 直接 fallback(§2.5.1) |
| 维度类型上限 | `MAX_DIM_TYPES` = 8 | SwitchController 一维;Lifecycle registry 容量(§5.1) |
| 单维度实例上限 | `MAX_INSTANCES` = 256 | instanceId ∈ [0, 255](§5.1) |
| SwitchController 内存 | **10 KiB** | enabled 8×256×1B + version 8×256×4B,二维静态数组,零运行时分配 |
| Dedup 内存 | 16 KiB | `inFlight_[4096]` × 4 B(`EJitAtomicU32`),零运行时分配 |
| 单桶 cache 容量 | 弹性 | 桶内 `unordered_map<u64, vector<EJitCacheEntry>>`,受平台内存限制 |
| 单维度参数上限 | 4 | `EJitCompileRequest::dims[4]`(§3.3.1) |
| 逻辑失效粒度 | 每实例 version | toggle bump → lookup 自动 miss(§5.4) |

### 6.4 淘汰机制

| 途径 | 何时 | 操作 |
|------|------|------|
| 同 identity 覆盖 | publish 命中已有 entry | vector 内匹配 entry 的 `fnPtr` 被覆盖;旧 fnPtr 在桶 `writeRelease` 之后调 `releaseFn_`(§4.1) |
| 逻辑失效 | `setEnabled` bump version | 引用该实例的 entry 在 lookup 逐维 version 比对失败 → miss(§5.4);entry 占位仍在 map 中,等下一次同 identity publish 覆盖 |
| 全量清理 | `EJitTaskPoolCache::shutdown` | 仅在 `EJitTaskPool` 析构时触发(`stopWorker` → `cache.shutdown`),遍历所有桶,锁外统一调 `releaseFn_`(§10.4)。注意:运行期 `setCompileMode(Sync)` 只停 worker(setMode(Off) + stopTaskPoolWorker),**不**调 shutdown——cache 内已发布的 entry 与 fnPtr 保留;直到 EJit 实例本身销毁才整体清理。 |

---

## 7. C ABI

### 7.1 维度对结构体

```c
// 一个生命周期维度对：(维度类型, 实例ID)
// 例如：dimType=0("小区"), instanceId=3 → 小区3 的该特化版本
typedef struct {
    uint32_t dimType;       // 维度类型编号
    uint32_t instanceId;    // 该维度下的实例 ID
} ejit_dim_pair_t;
```

### 7.2 对外接口

生产构建提供 4 个 C 函数(随 `libLLVMEJIT.a`)。worker 由 taskpool 内部 `EJitWorker` 模块管理(§5.5),不在 C ABI 暴露。

```c
// ================= 核心编译接口 (AOT wrapper 调用) =================
// funcIndex: ejit_entry 函数索引
// dims: 维度对数组，描述此调用的特化维度 (如 [(0,3), (1,5)])
// numDims: 维度对数量
// outFn: 输出函数指针 (hit 或编译成功) / fallback
// outBucket: 输出桶号，调用方使用完 fnPtr 后须 release_read(outBucket)
ejit_status_t ejit_taskpool_compile_or_get(uint32_t funcIndex,
                                            const ejit_dim_pair_t* dims,
                                            uint32_t numDims,
                                            void** outFn,
                                            uint32_t* outBucket);

// ================= 开关控制 =================
// 控制某个生命周期实例的启用/禁用
// 禁用后：该实例相关的现有缓存视为失效，新请求直接 fallback
void ejit_taskpool_set_instance_enabled(uint32_t dimType, uint32_t instanceId,
                                        uint32_t enabled);

// ================= 读计数释放 =================
// 调用方使用完 compile_or_get 返回的 fnPtr 后调用
// outBucket 由 compile_or_get 返回，O(1) 定位桶锁
void ejit_taskpool_release_read(uint32_t bucketIndex);

// ================= 监控 =================
unsigned ejit_taskpool_pending_count(void);
ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
```

**测试专用接口** —— 仅在定义 `EJIT_SRE_TASKPOOL_TESTING` 宏时编译,**不进入生产 `libLLVMEJIT.a`**:

```c
#ifdef EJIT_SRE_TASKPOOL_TESTING
// 直接驱动 queue 消费一次。host 测试中用于显式控制 pop 时序,
// 替代真实的 EJitWorker 循环。**不应在内部 worker 已启动时调用**——
// 测试代码自己负责保证生命周期不重叠(测试模式要么用真实 worker,要么用
// 这两个函数,二选一,详见 §9.2 的两类测试模式划分)。
unsigned ejit_taskpool_poll_one(void);
unsigned ejit_taskpool_poll_budget(unsigned maxItems);
#endif
```

**角色总结**:

```
开关控制 (任何时候可调):
   ejit_taskpool_set_instance_enabled(dimType, instanceId, enabled)

生产者侧 (业务 / AOT wrapper)         消费者侧 (内部 EJitWorker, 自动驱动)
   ejit_taskpool_compile_or_get(        SRE task 内循环:
     funcIndex, dims, numDims,            pool.pollOne()
     &fnPtr, &bucket)                       (核绑定由 SreTask SRE 实现决定)
     │
     ├─ 命中 → 调 fnPtr(...) → ejit_taskpool_release_read(bucket)
     │           ▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲▲
     │           必须配对释放(§3.2.4),否则旧 fnPtr 无法回收
     └─ 未命中 → 入队 → 立即返回 fallback,worker 后台编译

[TESTING-ONLY,EJIT_SRE_TASKPOOL_TESTING 下才进 C ABI]
   ejit_taskpool_poll_one() / ejit_taskpool_poll_budget(n)
     —— 用于在测试中关掉内部 worker、手动驱动消费(§9.2)
```

**统计结构体**：

```c
typedef struct {
    uint64_t cacheHits, asyncCompiles, asyncEnqueues;
    uint64_t alreadyPending, queueFull;
    uint64_t compileFailed, publishFailed, instanceDisabled;
    uint32_t readyEntries, pendingEntries, queueApproxSize;
    uint32_t reserved;
} ejit_taskpool_stats_t;
```

新增 status code（additive，旧值不变）：`EJIT_ERR_QUEUE_FULL`、`EJIT_ERR_INSTANCE_DISABLED`、`EJIT_PENDING`。

### 7.3 AOT Wrapper IR 结构

PASS3 `EJitWrapperGen` 为每个 `ejit_entry` 函数把原函数体改写为四块结构,业务侧直接调原名,实际执行的是 wrapper:

```
jit_entry:                                    // 函数新入口,执行 allocas + funcIndex 守卫
   %dims    = alloca [4 x EJitDimPair]
   %outFn   = alloca ptr
   %outBkt  = alloca i32
   %funcIdx = load i32, @__ejit_funcidx_<name>
   %ok      = icmp ne i32 %funcIdx, kEJitInvalidFuncIndex
   br i1 %ok, label %jit_call, label %jit_fallback   // 未注册/容量耗尽 → 直接走 AOT 体

jit_call:                                     // 构造 dims[] 并请求 taskpool
   ; 对每个 ejit_period_arr_ind 参数 i:
   ;   %dt = load i32, @__ejit_dimtype_<periodName_i>     ← registry 回填的 slot
   ;   store i32 %dt,        ptr getelementptr(%dims, i, 0)
   ;   store i32 %arg_i,     ptr getelementptr(%dims, i, 1)
   %st = call i32 @ejit_taskpool_compile_or_get(
              i32 %funcIdx, ptr %dims, i32 <numDims>, ptr %outFn, ptr %outBkt)
   %fn = load ptr, ptr %outFn
   %hit = icmp eq i32 %st, 0       ; CacheHit
   %has = icmp ne ptr %fn, null
   %disp = and i1 %hit, %has
   br i1 %disp, label %jit_dispatch, label %jit_fallback

jit_dispatch:                                 // 调特化码,配对 release_read 后返回
   %bkt = load i32, ptr %outBkt
   %ret = call <retty> %fn(<原参数>)          ; void 函数则无 %ret
   call void @ejit_taskpool_release_read(i32 %bkt)
   ret <retty> %ret                            ; void: 单独 ret void

jit_fallback:                                 // 从原函数体 splice 来的指令序列
   <原 entry 块全部指令>
```

**关键设计点**:

| 位置 | 设计 | 理由 |
|------|------|------|
| `jit_entry` 守卫 | funcIndex 仍为 `kEJitInvalidFuncIndex` 时直接 fallback | 未注册函数 / `EJitFuncRegistry` 容量耗尽时绝不进 taskpool,§2.5.1 |
| `dimType` 从 IR global load | 不是 wrapper 编译期常量 | 不同模块独立编译,只有运行时 registry 回填后才知道 slot,§2.5.1 |
| `outBucket` + `release_read` 配对 | 仅在 `jit_dispatch` 调用 release_read,**且只在成功执行完 fn 之后** | `tryRead` 隐含 `readers_++`,fnPtr 在调用期间不可被释放,§3.2.4 |
| 任何分支不通过 dispatch | 在 dispatch 之前/`jit_call` 失败侧均**不**调 release_read | `lookup` 未命中(`hasReadToken==false`)时 token 已在 lookup 内归还,无需配对 |
| `noinline` 属性 | wrapper(改名后的 `ejit_entry`)添加 `noinline` | 防止 inliner 把 wrapper 折进 caller,使 wrapper 失去函数边界(可选,由 `EJitWrapperGen.cpp` 内的 `cl::opt<bool> EJitNoInlineEntry` 控制) |

**与 §2.5.2 的衔接**:wrapper 加载 `@__ejit_funcidx_<name>` 时,若 `ejit_init` 失败,该 global 仍是初始哨兵值——守卫判定 false,函数静默走 fallback,不会调到一个未初始化的 taskpool。这是「`ejit_init` 失败 + 应用继续运行」场景的兜底。

---

## 8. Trace 与调试

关键路径埋了默认空展开的 trace 宏：

```cpp
#ifndef EJIT_TASKPOOL_TRACE
#define EJIT_TASKPOOL_TRACE(...) do {} while (0)
#endif
```

上板时可用 `-D'EJIT_TASKPOOL_TRACE(...)=SRE_printf(__VA_ARGS__)'` 重定义。参数限整数/指针/C 字符串。当前实际埋点(取自 `EJitTaskPool.cpp` / `EJitWorker.cpp` / `EJitSreTask_sre.cpp`):

| 函数 | 埋点 |
|------|------|
| `EJitTaskPool::compileOrGet` | request / reject(invalid dims) / disabled / hit / fallback(off/full) / enqueued / coalesced / reject(out of range) |
| `EJitTaskPool::runCompile` | begin / drop before compile / failed / drop after compile / publish ok / publish drop / publish failed |
| `EJitWorker::start` / `stop` | start name / start failed / start accepted / stop begin / stop complete (with processed/spins) |
| `EJitSreTask::create` / `destroy`(SRE 实现) | create begin/end with handle / destroy begin/complete |

`compileOrGet` 与 `runCompile` 的埋点用的是同一族 `EJIT_DIAG` 宏(默认条件展开,见 `EJitDiag.h`);上板诊断把宏点亮即可获得完整一次请求的端到端时间线。`EJitSwitchController::setEnabled` 当前未埋点——若需要观察 toggle 时机,可在 `set_instance_enabled` 调用方加一行 `EJIT_DIAG`。

---

## 9. 构建与测试

### 9.1 构建

```bash
./build.sh release aarch64_be --freestanding --sre-taskpool
```

| 开关 | 说明 | 默认 |
|------|------|------|
| `--sre-taskpool` | 开关 taskpool | OFF |
| `--sre-taskpool-buckets=<n>` | cache 桶数 | 32 |
| `--sre-taskpool-queue-capacity=<n>` | 队列容量 (pow2) | 1024 |

对应 CMake option：`EJIT_SRE_TASKPOOL`、`EJIT_SRE_TASKPOOL_BUCKETS`、`EJIT_SRE_TASKPOOL_QUEUE_CAPACITY`。

**`EJitSreTask` 实现选择**:链接时根据构建目标择一,与上述 CMake option 正交:

| 构建目标 | SreTask 实现 | 说明 |
|---------|------------|------|
| host (Linux/macOS 测试) | `EJitSreTask_host.cpp` | 用 `std::thread` 实现 task,核绑定交给 OS 调度器 |
| SRE 上板 (`--freestanding`) | `EJitSreTask_sre.cpp` | 调用 SRE 平台 task 原语,核绑定由 SRE 实现内部决定 |

构建脚本根据是否传入 `--freestanding` 自动选择对应 `.cpp` 编入 `libLLVMEJIT.a`。

### 9.2 测试

```bash
cmake --build build-ejit-sre-taskpool --target check-ejit-taskpool -j8
```

`EJITTaskPoolTests` 编译 `EJitTaskPool.cpp` + `EJitSreQueue.cpp` + `EJitSreTask_host.cpp` + `EJitWorker.cpp`(带 `EJIT_SRE_TASKPOOL_TESTING`),不依赖 `EJITTests`,host 可跑。两类测试模式:

- **不启动 worker 模式**(默认):测试代码用 mock compiler + 显式 `pollOne`/`pollBudget` 模拟时序,**不使用真实线程**,并发交错由测试钩子精确控制
- **启动 worker 模式**:用 `EJitSreTask_host.cpp`(`std::thread`)启动真实 worker,验证 `EJitWorker` 的启停、循环、计数;mock compiler 注入可控延迟

覆盖用例:

- atomic wrapper、RwLock 每桶隔离、SwitchController (enabled+version 编码)
- queue(基础组件：容量/FIFO/满返回)
- dedup(基础组件：funcIndex 直接索引 O(1)、CAS 占位/释放)
- taskQueue(业务组件：去重入队 tryEnqueue、自动回滚、tryDequeue/release)
- cache(hash 分布、逐实例 version 比对失效)
- async 路径、实例开关竞态(编译中 toggle、publish 窗口内 version 变更)
- worker 启停幂等、stop 后 task 退出、stopRequested 自检、processed/spin 计数
- stats 计数、request flat POD 断言

---

## 10. 核间语义速查

§3–§5 已沿组件展开发布协议与临界区约束的论证。本章是**面向审阅与上板适配**的浓缩清单——每一条在哪个组件落地、详细论证在哪一节,一行交代清楚。本章**不**引入新规则。

### 10.1 临界区：哪里持锁,持锁里做什么

| 锁 | 持锁者 | 持锁里允许做 | 持锁里禁止做 |
|----|-------|------------|------------|
| 桶 `EJitRwLock::write`(§3.2.2) | publish / shutdown | 写 entry 字段、收集旧 fnPtr | 调 `releaseFn_`、调 `compileFn_`、ORC/JITLink、SRE_printf、阻塞队列 I/O |
| 桶 `EJitRwLock::tryRead`(§3.2.1) | lookup,token 由调用方持有 | 读 entry、调 fnPtr | 等待写者(立即失败 fallback) |
| `EJitIpcBucketLock`(§10.5) | 跨核短状态更新预留 | 见 §10.5 | 同上 |

落地点:`EJitTaskPoolCache::publish` / `shutdown` 都把 `releaseFn_` 调用**移到桶写锁释放之后**(§4.1)。

### 10.2 发布协议：每个共享状态的写入/读取顺序

| 共享状态 | 写者 | 写入顺序 | 读者 | 读取顺序 |
|---------|------|---------|------|---------|
| `EJitQueue` cell(§3.3) | producer | 写 `EJitCompileRequest` → `sequence.storeRelease(pos+1)` | worker | `sequence.loadAcquire`(等于 pos+1) → 读 data |
| `EJitDedupTable::inFlight_`(§3.5) | producer / worker | CAS(0,1) 抢 / `storeRelease(0)` 释放 | producer | CAS 抢的同语义,无单独 read 路径 |
| Cache entry(§4.1) | worker 持桶写锁 | 写字段 → `writeRelease`(release) | reader 持 `tryRead`(acquire) | `tryRead` 看到 writeFlag=0 → 读 entry |
| `EJitSwitchController::version_`(§5.1) | `setEnabled` 翻转 → `fetchAdd(1)`(acq_rel) | — | `getInstanceVersion` `loadAcquire` | — |
| `EJitSwitchController::enabled_`(§5.1) | `setEnabled` CAS(单次 acq_rel) | — | `isInstanceEnabled` `loadRelaxed` | — |

### 10.3 version 失效：一次 toggle 三处兜底

`setEnabled(d, i, !)` 成功一次 → `version_[d][i]` 单调 +1(§5.1) → 三层独立兜底:

1. **lookup**: entry.versions[i] 不再匹配 → miss(§4.1)
2. **worker 检查点 1 / 检查点 2**: req.versions[i] 不再匹配 → release + 丢弃(§5.3)
3. **publish 提交门**: 持桶写锁瞬间重验 req.versions[i] → 不匹配则 `VersionMismatch`,不写 entry(§4.1)

三层都看不到陈旧 version 写入新 entry 的窗口。详细窗口分析:§5.3 检查点 2 → publish 窗口、§5.1 单调性证明。

### 10.4 旧 fnPtr 释放：`releaseFn_` 的两个唯一触发点

- **publish 覆盖**(§4.1):写锁内覆盖 fnPtr,写锁外调 `releaseFn_(oldFn)`。`write()` 已 spin 到 `readers_=0`,旧 fnPtr 对该桶不再可达。
- **`EJitTaskPoolCache::shutdown`**(§4.1):遍历所有桶收集 fnPtr,锁全部释放后统一调 `releaseFn_`。

`releaseFn_` 由 `EJitCompileDriver` 注入(指向 code pool retire);未注入时退化为逻辑丢弃。**不是 C ABI 公开点**,不参与 toggle 失效路径(失效由 §10.3 完成)。

### 10.5 跨核扩展点（当前未在生产路径使用）

`EJitIpcBucketLock` + `EJitSharedBarrier`(`EJitIpcLock.h`)提供 32 桶 spin lock 与 `fenceAcquire/Release/Full` 屏障封装。当前 cache 桶互斥由各自的 `EJitRwLock` 实现,**不**经 `EJitIpcBucketLock`;该锁主要给测试钩子和未来的跨核短状态更新预留。真实平台符号 `EJitBucketTryLock/Lock/Unlock` 仅声明(`EJIT_SRE_TASKPOOL_PLATFORM_IPC_LOCK`),不提供 weak fallback。

### 10.6 大端共享结构约束

- 共享结构使用固定宽度整数与明确对齐。
- 不使用 bitfield(`EJitDedupSlot`/`EJitCacheEntry` 等均为标量原子字段)。
- 不按字节解析整数,不把 native layout 持久化为跨端文件协议。

