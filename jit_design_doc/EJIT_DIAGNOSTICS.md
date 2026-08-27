# EJIT 诊断与调试指南

> 面向已将 EJIT 集成进自己二进制的集成方，说明上板调试时如何用 EJIT 提供的诊断手段定位问题。
>
> 本文不涉及如何构建 EJIT 运行库本身。若某诊断能力调用后无输出或返回 `EJIT_ERR_DISABLED`，说明你链接的 EJIT 运行库未启用该能力，请向 EJIT 提供方确认。

EJIT 的诊断手段按“何时用到”分两类：

- **运行时**（上板调试期）：二进制运行过程中，通过 C API 动态调整日志级别、读取统计、转储 IR/ASM、内省注册表。这是上板排障的主要手段（§1–§8）。
- **编译期**（开发构建期）：`clang` 编译你标注了 ejit 属性的源码时发出的告警 / 报告（§9）。它们在你构建含 `ejit_entry` 的代码时就已产出，帮你提前发现“特化没收益”“维度声明错”等问题。

### 现象 -> 先看哪里

| 现象 | 先看 |
|------|------|
| 注册表 / 特化参数对不对 | §4 注册表与元数据内省 |
| 某个 `ejit_entry` 没收益 / 没被特化 | §9.2 编译期特化价值诊断 + §4 `ejit_print_func_meta` |
| 该编译的没编译 / 编译失败 | §6 错误报告 + §1 日志 + §2.2 taskpool 统计 |
| 命中率低 / 性能差 | §2 统计 + §5 wrapper 计时 |
| 代码内存趋紧 / 耗尽 | §2.3 代码池统计 |
| AArch64 上分支超范围 / 走了 stub | §8 JITLink 分支重定位诊断 |
| 想看某函数特化后的 IR / 汇编 | §3 JIT IR / ASM 转储 |

---

## 1. 运行时日志

运行时日志分四级，可在不重新编译的情况下动态调整。

| 级别 | 值 | 作用 |
|------|----|------|
| `EJIT_LOG_OFF` | 0 | 不输出 |
| `EJIT_LOG_INFO` | 1 | 关键事件（默认）：init/shutdown、编译 begin/OK/FAIL、cache MISS、激活、错误、注册消费摘要 |
| `EJIT_LOG_VERBOSE` | 2 | 逐项细节：每次首次注册、逐函数 struct-field 统计、逐次 `compile_or_get`、taskpool 请求 |
| `EJIT_LOG_DEBUG` | 3 | 内部机理：幂等注册跳过、逐 load 替换失败、staging 内部、funcMeta 缓存 |

```c
void ejit_set_log_level(ejit_log_level_t level);   // 立即生效，影响后续所有日志输出
ejit_log_level_t ejit_get_log_level(void);          // 查询当前级别
```

```c
#include "llvm/ExecutionEngine/EJIT/EJitRuntime.h"

ejit_set_log_level(EJIT_LOG_INFO);     // 生产环境降低日志量
ejit_set_log_level(EJIT_LOG_VERBOSE);  // 排查问题时提升到细节级
```

> 若调用后无任何日志输出，说明你的 EJIT 运行库未启用诊断日志输出，向提供方确认。

---

## 2. 运行时统计

### 2.1 旧版 LRU cache 统计（兼容接口）

```c
typedef struct {
  size_t entryCount;      // cache 中条目数
  size_t totalCodeSize;   // 已缓存代码总字节
  size_t maxSize;         // cache 容量上限
  uint64_t hits;          // 命中次数
  uint64_t misses;        // 未命中次数
  uint64_t evictions;     // 淘汰次数
} ejit_stats_t;

ejit_status_t ejit_get_stats(ejit_stats_t *stats);
```

该接口仅为旧版 LRU `EJitCache` 保留 ABI 兼容性。当前 taskpool 运行时不再维护这组统计；初始化成功后调用会返回 `EJIT_OK`，并将各字段清零。请使用 §2.2 的 taskpool 统计和 §2.3 的代码池统计定位当前实现的问题。

### 2.2 SRE taskpool 统计

```c
typedef struct {
  uint64_t cacheHits;                   // 命中 taskpool cache 的调用数
  uint64_t asyncCompiles;               // worker 成功编译数
  uint64_t asyncEnqueues;               // 入队请求数
  uint64_t alreadyPending;              // 被合并的重复提交
  uint64_t queueFull;                   // 队列满被拒的入队
  uint64_t compileFailed;               // 失败 / 取消 / 丢弃的编译
  uint64_t publishFailed;               // 无法进入 cache 的结果
  uint64_t instanceDisabled;            // 实例禁用快速路径命中
  uint64_t instanceDisabledPreActivate; // 首次激活前命中的子集（init->activate 窗口）
  uint32_t readyEntries;                // 存活 ready cache 条目
  uint32_t pendingEntries;              // 存活 in-flight 去重槽
  uint32_t queueApproxSize;             // 近似异步队列深度
  uint32_t reserved;                    // 保留，恒为 0
} ejit_taskpool_stats_t;

ejit_status_t ejit_taskpool_get_stats(ejit_taskpool_stats_t *out);
void          ejit_taskpool_print_stats(void);   // 人可读形式经日志输出
void          ejit_taskpool_print_compiled(void);// 列出所有已编译特化（funcIdx/name/dims/fn）
uint32_t      ejit_taskpool_get_worker_core(void);// 返回 worker 运行所在核
unsigned      ejit_taskpool_pending_count(void); // 在途数量
```

**作用**：定位“为何没编译 / 为何编译慢 / 队列是否拥塞”。看 `cacheHits` 判断命中率，`alreadyPending` 判断重复提交合并，`queueFull`/`queueApproxSize` 判断拥塞，`compileFailed`/`publishFailed` 判断编译失败。

**结构体解读**：前九个 `uint64_t` 是从 taskpool 建立以来的累计事件计数，后三个有效 `uint32_t` 是读取瞬间的存量快照。常见组合含义如下：

| 字段 / 组合 | 含义与判断 |
|-------------|------------|
| `cacheHits` | 已发布特化被复用的次数；应结合总调用量看趋势，不能单独当作命中率百分比。 |
| `asyncEnqueues` / `asyncCompiles` | 前者是进入异步队列的请求，后者是 worker 成功编译并发布的结果。两者短时不相等是正常的；长期差距扩大时结合 pending、失败和队列满计数排查。 |
| `alreadyPending` | 相同函数和维度组合已在编译，当前请求被去重合并；高值不等于失败，但持续快速增长通常表示热点在等待同一个编译。 |
| `queueFull` | 请求因异步队列无空位而未入队；非零即说明出现过瞬时或持续拥塞。 |
| `compileFailed` | 编译失败，以及编译期间版本 / generation 变化而被取消或丢弃的结果总数。 |
| `publishFailed` | 编译已经产出，但无法写入 taskpool cache；与 `compileFailed` 分开看可区分“没编出来”和“编出后没发布”。 |
| `instanceDisabled` | 请求命中了未激活实例的快速返回路径；`instanceDisabledPreActivate` 是其中发生在首次 activate 前的子集。 |
| `readyEntries` | 当前可命中的已发布特化数，不是累计编译数。 |
| `pendingEntries` / `queueApproxSize` | 前者是正在去重跟踪的编译身份数，后者是队列近似深度；两者长期不回落通常表示 worker 阻塞或处理能力不足。 |

`ejit_taskpool_print_stats()` 除上述 C 结构体外，还会直接打印 shared taskpool 快照：

| 输出字段 | 含义 |
|----------|------|
| `initState` | 初始化状态：0 未初始化、1 初始化中、2 可用、3 失败、4 正在停止。 |
| `ownerCore` / `workerTaskId` | 赢得 owner 选举的核与其 worker 任务 ID；Ready 时 task ID 应非 0。 |
| `gen` | shared taskpool 每次重新初始化递增的代数，用于识别是否发生过重建。 |
| `lastInitErr` | 初始化失败原因：0 无错误、1 worker 启动失败、2 owner 创建 JIT engine 失败。 |
| `initAttempts` | owner 选举累计尝试次数；异常增长通常意味着多核反复初始化。 |
| `share` | 是否允许非 owner 核直接使用共享代码指针。 |
| `regFingerprint` | owner 发布的 funcIndex / dimType 映射摘要；各核 attach 时用它拒绝不一致的注册表。 |
| `execPrepFailed` | 已找到共享代码，但当前核执行前准备失败而回退的次数。 |

同一接口随后打印 inline-cache slot 注册表，用于把“taskpool cache 已命中但 wrapper inline cache 始终不命中”进一步定位到未注册、维度形状不一致或填充失败。

> 若 `cacheHits` 等逐调用计数全为零，说明运行库未启用逐调用统计（这些计数有热点路径开销，默认关闭）；`print_compiled()`、`get_worker_core()`、`pending_count()` 不受此影响，始终可用。

`ejit_taskpool_print_compiled()` 的摘要中，`entries` 是本次尽力遍历到的 Ready 特化数，`slots` 是 cache 占用，`buckets skipped` 表示遍历时因并发竞争而跳过的 bucket；因此它不是强一致快照。摘要分别统计 `baseline/tier1_collecting/tier2`、主代码池落点和带 MFS 冷区的版本数。明细中的 `dims=[d:i,...]` 表示 `dimType:instanceId`，`pool=near|far` 表示入口代码实际落点，`mfs_cold=yes` 表示同一版本还有 `.text.ejit_cold` companion range。VERBOSE 级额外显示各维度版本 `ver`、热代码大小 `size`、`cold_size`、池内编号 `pool_id` 和发布代数 `gen`。

### 2.3 代码池统计

```c
typedef struct ejit_code_pool_stats_t {
  uint64_t poolCount;           // 已创建的 2MiB 池总数
  uint64_t sealedCount;         // 当前已封存（RX）的池
  uint64_t activeCount;         // 仍可写（RW）的池
  uint64_t usedBytes;           // 各池 bump 偏移之和
  uint64_t reservedBytes;       // 各池容量之和
  uint64_t wastedBytes;         // 已封存池尾部未用字节
  uint64_t sealInvocations;     // enable_ex 成功次数（4K 模式按 4K 页计）
  uint64_t splitInvocations;    // split_2m_to_4k 成功次数（4K 模式）
  uint64_t finalizedRangeCount; // 记录的可执行区间数
} ejit_code_pool_stats_t;

typedef struct ejit_code_pool_stats_v2_t {
  ejit_code_pool_stats_t total; // 两个池合计，与旧接口相同
  ejit_code_pool_stats_t near;  // 最终 Baseline / Tier-2 固定近端池
  ejit_code_pool_stats_t far;   // 临时 Instrumented Tier-1 动态池
} ejit_code_pool_stats_v2_t;

ejit_status_t ejit_get_code_pool_stats(ejit_code_pool_stats_t *out);
ejit_status_t ejit_get_code_pool_stats_v2(ejit_code_pool_stats_v2_t *out);
void          ejit_print_code_pool_stats(void);
```

**作用**：监控 JIT 代码内存占用与是否趋近耗尽（`usedBytes` vs `reservedBytes`）。旧接口保持 ABI 不变并返回全部池合计；v2 的 `near` 合并 hot/cold 固定池以保持 ABI，v3 进一步拆分 `nearHot/nearCold/farTier1`。`ejit_print_code_pool_stats()` 同时打印 `total`、`near-hot(final)`、`near-cold(mfs)` 和 `far(tier1)`。返回值：`EJIT_OK` 成功；`EJIT_ERR_NOT_ACTIVE` 运行时未初始化；`EJIT_ERR_INVALID_PARAM` 入参为空；`EJIT_ERR_DISABLED` 运行库未含代码池支持。

**结构体解读**：

| 字段 / 组合 | 含义与判断 |
|-------------|------------|
| `poolCount` / `reservedBytes` | 已经实际划出的 2 MiB pool 数及其容量总和；固定区域中尚未划出的空间不计入。 |
| `activeCount` / `sealedCount` | 当前仍可写的 RW pool 与已封固的 RX pool 数。两者描述页状态，不等同于“有用 / 无用代码”。 |
| `usedBytes` | bump cursor 已消耗空间之和；4K 封固模式会按页对齐，可能略高于有效机器码字节数。 |
| `wastedBytes` | 已封固 pool 尾部未使用且不能继续分配的空间，可用于判断封固粒度带来的碎片。 |
| `sealInvocations` | 成功切换为可执行状态的次数；4K 模式按页计，不能直接当成编译函数数。 |
| `splitInvocations` | 成功将 2 MiB 映射拆成 4K 页的次数，仅 4K 模式有意义。 |
| `finalizedRangeCount` | 已记录的可执行代码区间数，通常比 pool 数更接近已完成的代码分配批次数。 |

> **固定代码段模式**：若你的运行库使用固定代码段模式（代码池位于链接脚本固定的 `.text.ejit` 区域，给 JIT 稳定地址），每次页状态转换会在 **INFO 级**打印 `enableRwRange` 系列日志（`begin` / `OK` / `FAIL`（未归属 / 跨池 / `enable_rw` 失败）/ `rollback`（部分失败时回退 RW->RX）），用于诊断 W^X 页状态转换。封固粒度（4K 页 vs 整 2MiB 池）影响 `sealInvocations` / `splitInvocations` 的计数粒度。

---

## 3. JIT IR / ASM 转储

提供两种互补的转储方式：**文件式**（一次性、全量、便于离线分析）与**内存捕获式**（运行时按名过滤、经日志回读）。

### 3.1 文件式 IR 转储（`dumpJITDir`）

把每个特化的优化前、优化后 LLVM IR（`.ll`）分别落盘，文件名为 `<funcName>_<cacheKey>_pre.ll` 和 `<funcName>_<cacheKey>_opt.ll`，便于离线 diff / 审查。通过 `ejit_init()` 配置项启用：

```c
ejit_config_t cfg = {};
cfg.compileMode  = EJIT_COMPILE_ASYNC;
cfg.optLevel     = EJIT_OPT_L2;
cfg.enableLogger = true;
cfg.dumpJITDir   = "/tmp/ejit_ir";   // 非空即开启
ejit_init(&cfg);
```

### 3.2 内存捕获式 IR+ASM 转储

```c
void ejit_dump_func(const char *name);  // 按名开启捕获；name="*" 捕获全部；NULL/"" 关闭后续捕获
void ejit_dump_all(bool enable);        // 等价于 ejit_dump_func("*")（enable=true 时）
void ejit_print_dumped(const char *name);// 打印 entry 函数视图；NULL/"" 打印本核全部
void ejit_print_dumped_module(const char *name); // 打印完整特化模块视图
```

运行时按函数名过滤捕获后续 JIT 编译产生的**优化后 IR 与汇编**，再通过 RAW 诊断输出逐行回读。在线 PGO 开启时会跳过临时 Tier-1 插桩代码，只捕获最终发布的 Tier-2；这样不会因诊断汇编生成而额外拉长 Tier-1 的生命周期竞争窗口。`ejit_print_dumped(name)` 只显示指定 entry 函数，便于聚焦单函数；`ejit_print_dumped_module(name)` 显示该 entry 对应的完整特化模块，包括模块中保留的被调函数。

> - 捕获为**精确名匹配**，`"*"` 例外（捕获全部）。
> - 每次捕获同时保存单函数视图和完整模块视图；同名函数重新编译时替换旧记录。
> - 完整 IR/ASM 负载保留在 worker 核本地，不拷入共享 taskpool 内存；回读不会跨核查找，需在执行编译的 worker 核调用打印接口。
> - RAW 转储输出不受普通运行时日志等级阈值抑制。
> - 若回读内容只有 IR、没有汇编，说明运行库未启用汇编发射，向提供方确认。

```c
ejit_dump_func("process_cell");   // 只捕获感兴趣函数的下一次特化
// ... 触发该函数的 JIT 编译 ...
ejit_print_dumped("process_cell"); // 回读 IR+ASM
ejit_print_dumped_module("process_cell"); // 回读该 entry 的完整特化模块

ejit_dump_all(true);              // 捕获所有特化
```

---

## 4. 注册表与元数据内省

```c
void ejit_print_registry(void);             // 打印全部已注册项
void ejit_print_func_meta(const char *name);// 打印某函数的 !ejit.metadata
void ejit_print_active(void);               // 打印当前激活的时间窗实例
void ejit_print_version(void);              // 打印运行库构建标识
```

| API | 作用 |
|-----|------|
| `ejit_print_registry` | 列出所有已注册 bitcode（funcIdx / name / size）、period 数组（period / var / base / size）、静态变量（var / addr），以及 funcIndex / lifecycle 计数。用于验证 AOT 注册是否正确填充运行时。 |
| `ejit_print_func_meta` | 解析并打印 `name` 的 `!ejit.metadata`：是否为 `ejit_entry`、其 `period_arr_ind` 参数槽、period 数组、`may_const` 字段偏移。用于诊断特化参数绑定与常量替换资格。 |
| `ejit_print_active` | 列出每个注册 period 下当前激活的 (period, cell)。静态变量视为恒激活。用于诊断“某 period 实例为何编译 / 为何没编译”。 |
| `ejit_print_version` | 打印运行库构建标识：LLVM 发行版本号与 llvm-project 源码的 git commit + 分支。**无需初始化运行时、不受日志级别门控**，便于将现场设备行为与确切源码版本对应。 |

这些接口直接展开内部注册记录，字段含义如下：

| 输出 | 字段含义 |
|------|----------|
| `registry: funcIndexes / lifecycles` | 已分配的函数索引数与 lifecycle（`dimType`）数，用于快速发现容量或跨核注册数量不一致。 |
| `bitcodes: idx / name / size` | `idx` 是 wrapper 与 taskpool 共同使用的 `funcIndex`；`size` 是注册 bitcode 字节数，0 通常表示该索引没有可读取的位码。 |
| `period arrays: period / var / base / size` | lifecycle 名、数组变量名、运行时基址和元素数量；`base` / `size` 错误会直接导致维度取值或常量替换错误。 |
| `static vars: var / addr` | 恒激活静态变量名及运行时地址。 |
| `func_meta: funcIdx / found` | 查到的函数索引，以及注册 bitcode 中是否存在该函数定义。`found=0` 表示只有声明或没有定义。 |
| `func_meta: ejit_entry` | metadata 是否把该定义标为 JIT entry。 |
| `func_meta: op / tag / vals` | `op` 是 `!ejit.metadata` 的原始 operand 序号；`tag` 是元数据类别，`vals` 是 period、参数槽或字段偏移等原始值。 |
| `active: period / active / cell` | `active` 是该 period 当前激活 cell 数，随后每行 `cell` 给出具体实例索引；这是 JIT gate 实际查询的同一份状态。 |

```c
ejit_init(&cfg);
ejit_print_registry();                // 确认注册表
ejit_print_func_meta("process_cell"); // 查看某函数的特化元数据
ejit_print_active();                  // 查看当前激活实例
ejit_print_version();                 // 可在 init 前后任意时刻调用
```

---

## 5. Wrapper 计时

在 AOT 生成的 `ejit_entry` wrapper 中插入计时探针，测量 taskpool 查找、间接 JIT 调用、读令牌释放各段耗时，定位 wrapper 开销。运行库自动按固定间隔（默认每 100000 次调用）聚合打印一行汇总，避免日志刷屏。

**启用**：计时探针需在编译你的 ejit 代码时开启：

```bash
clang -fembed-bitcode ... -mllvm -ejit-wrapper-timing process.c
```

运行时聚合 API 由插桩 wrapper 自动调用，一般无需手动调用：

```c
uint64_t ejit_taskpool_trace_now(void);   // 取当前时间戳
void ejit_taskpool_trace_wrapper(uint32_t funcIndex, uint32_t status,
                                 void *fnPtr, uint32_t bucketIndex,
                                 uint64_t tBeforeLookup,
                                 uint64_t tAfterLookup,
                                 uint64_t tAfterFn,
                                 uint64_t tAfterRelease);
```

> 仅当 wrapper 以 `-mllvm -ejit-wrapper-timing` 构建时才会产生计时；时间戳单位由平台决定。

汇总行 `wrapper_timing_agg` 的字段含义：`func` 为 `funcIndex`，`status` 为本次聚合槽记录的 taskpool 返回状态，`fn` 为调用目标地址，`bucket` 为 cache bucket，`count` 为本窗口样本数；`get_fn_avg`、`fn_call_avg`、`release_avg` 和 `total_avg` 分别是查找目标、执行 JIT 函数、释放读令牌及 wrapper 总路径的平均耗时。先比较 `fn_call_avg` 与其余三段，才能区分业务函数变慢和分发框架开销。

---

## 6. 错误报告

```c
typedef struct {
  int  code;
  char message[256];
  char funcName[128];
} ejit_error_t;

const ejit_error_t *ejit_get_last_error(void);
```

返回最近一次错误的指针（code / message / funcName），底层为预分配环形缓冲（最多 32 条），无动态分配。

| 字段 | 含义 |
|------|------|
| `code` | 对应附录中的 `ejit_status_t` 错误码。 |
| `message` | 最近一次被记录错误的简短原因，不保证包含底层编译器的完整诊断。 |
| `funcName` | 与错误关联的函数或注册项名称；错误不属于特定函数时可能为空。 |

返回指针由 EJIT 内部环形缓冲持有，后续错误记录可能覆盖其内容；需要长期保留时应由调用方立即复制这三个字段。

配套配置项 `enableLogger`（`ejit_config_t` 的 `bool` 字段）控制错误环形缓冲是否启用；它与 §1 的运行时日志等级是两套独立机制。注意该 C 结构体字段**无默认值**：零初始化的 `ejit_config_t` 会得到 `enableLogger = false`，需显式置 `true` 才会记录错误。freestanding 构建会强制关闭该错误记录器：

```c
ejit_config_t cfg = {};
cfg.enableLogger = true;   // 必须显式设置；零初始化结构体为 false
ejit_init(&cfg);
```

```c
if (ejit_taskpool_compile_or_get(...) != EJIT_OK) {
  const ejit_error_t *e = ejit_get_last_error();
  if (e) printf("EJIT error %d: %s (%s)\n", e->code, e->message, e->funcName);
}
```

---

## 7. 缓存失效

```c
void ejit_clear_cache(void);
void ejit_invalidate(const char *periodName, uint32_t cellIdx);
```

- `ejit_clear_cache`：在 shared-taskpool 构建中淘汰各核 L0 dispatch cache，并清空共享 inline cache。旧版 LRU 清理路径已经退役，因此该接口**不会释放已生成的 JIT 代码内存，也不保证触发重新编译**。
- `ejit_invalidate`：校验 `cellIdx` 范围后执行旧版按 period 失效路径，并在 shared-taskpool 构建中同样淘汰 dispatch / inline cache。当前实现不会定向删除 taskpool 中已发布的编译结果或释放代码内存。

这两个接口当前主要用于避免外部状态变化后继续命中陈旧的快速分发指针，不应作为代码内存回收接口使用。

---

## 8. JITLink 分支重定位诊断（AArch64）

**作用**：诊断 AArch64 / aarch64_be 上 JIT 代码的分支重定位是否落在 `B/BL` 的 ±128MiB 直跳范围内，超范围时是否被 stub 化（`ADRP x16; LDR x16,[x16]; BR x16` 经 `$__GOT` 间接跳），以及 stub 化的边能否被松弛回直跳。仅 AArch64 目标生效。

**启用**：需运行库启用诊断日志 + 运行时日志级别 ≥ `EJIT_LOG_INFO`（默认即 INFO）。由 JITLink 诊断 / 优化插件在每次链接 AArch64 graph 后输出。

**INFO 级输出**（每个链接的 graph，带 `[EJIT]` 前缀）：

- **relax 汇总**：`relaxAArch64BranchStubs: graph=<g> Branch26PCRel: <n> total, <n> stubbed (chain-mismatch=<n> unresolved=<n> out-of-range=<n>), <n> relaxed`，并给出 stub 化边的跳过原因分解。
- **`[STUBBED]` 审计**：每条被 stub 化的分支重定位一行（带两空格缩进），`  [STUBBED] <bl/b/...> @0x<addr> -> stub@0x<addr> (ADRP x16; LDR x16,[x16]; BR x16) -> $__GOT -> <target> @0x<addr> | direct dist=<n> (<EXCEEDS/within> +-128MB)`。
- **linkdiag 汇总**：`linkdiag: graph=<g> summary: <n> stubbed (<n> exceed +-128MB), <n> direct`。

**VERBOSE 级**追加：graph 头 `linkdiag: graph=<g> triple=<triple> -- branch relocation audit`，以及逐条直跳重定位（带两空格缩进）`  [direct ] <reloc> @0x<addr> -> <target> @0x<addr> | dist=<n>`。

用于定位“为何某 call 没被优化成直跳 / 为何走了 stub / stub 是否被松弛”。

---

## 9. 编译 ejit 代码时的诊断

以下诊断在你用 `clang -fembed-bitcode` 编译标注了 ejit 属性的源码时发出，走 `stderr` 或 clang 诊断通道，经 `-mllvm` 标志或 `-W` 诊断组控制。**不受运行时日志级别影响**，它们在编译期即完成。lit 测试用 `2>&1` 捕获（输出在 stderr，不在 IR `.ll` 里）。

### 9.1 Sema 诊断

clang Sema 阶段默认启用以下告警，归入诊断组 `embedded-jit`（即 `-Wembedded-jit`，可用 `-Wno-embedded-jit` 整组关闭）：

| 告警 | 触发条件 | 文本 |
|------|----------|------|
| `warn_ejit_attr_missing_on_definition` | 函数声明带 `ejit_entry` 或 `ejit_period_lc`，但定义未重复该属性。Sema 会禁用该定义的 EmbeddedJIT 特化，并指向原声明。可单独用 `-Wno-embedded-jit-attr-missing-on-def` 关闭。 | `function %0 is declared with '%1' but defined without it; EmbeddedJIT specialization is disabled for this definition` |
| `warn_ejit_always_inline_conflict` | 对 `ejit_entry` / `ejit_period_lc` 函数标注 `always_inline`。这些函数必须保持非内联才能在内联器中存活供 JIT 特化，用户 `always_inline` 与之冲突，Sema 告警并忽略 `always_inline`。 | `'always_inline' is incompatible with %0, which must remain out-of-line so it survives the inliner for JIT specialization; ignoring 'always_inline'`（`%0` = `ejit_entry` 或 `ejit_period_lc`） |
| `warn_ejit_may_const_modified_without_lc` | 在**未**标注 `ejit_period_lc` 的函数中写 `ejit_may_const` 字段。`may_const` 字段只允许在时间窗切换（lifecycle）时变更，普通函数写入会破坏特化一致性。 | `modifying ejit_may_const field %0 of %1 without ejit_period_lc attribute`（`%0` = 字段名，`%1` = 持有该结构体的变量或其父记录） |

以下 Sema 告警默认关闭，需按需显式开启：

| 诊断组 | 触发条件 | 文本 |
|--------|----------|------|
| `-Wembedded-jit-addr-of-may-const` | 对 `ejit_may_const` 字段取地址，但所得指针不带 `const`，可能绕过 lifecycle 写入约束。 | `taking address of ejit_may_const field %0 of %1 without const qualifier; a non-const pointer allows the field to be written outside ejit_period_lc, breaking the JIT's time-window constancy assumption` |
| `-Wembedded-jit-undeclared-period-dep` | 函数引用 `ejit_period_arr("P")`，但未通过 `ejit_period_arr_ind("P")` 声明依赖。 | `function %0 references ejit_period_arr '%1' but does not declare a dependency on it via ejit_period_arr_ind` |

### 9.2 特化价值诊断

由 EJIT 的 AOT 位码提取 pass（PASS1）在提取位码上运行 `runSpecializationDiagnostic` 发出，经 `-mllvm` 标志控制。

**特化闭包**（诊断推理的范围）：`ejit_entry` 自身 + 经**直接调用**可达的全部已定义非 intrinsic 函数（不动点传播）。**外部声明调用与间接调用（函数指针）不计入**，因为 JIT 无法内联它们，其 `may_const` 读取不会进入该 entry 的特化。因此 #1 不会产生误报。

| 标志 | 默认 | 类别 | 触发条件 |
|------|------|------|----------|
| `-mllvm -ejit-warn-no-specialization` | **on** | #1 告警 | entry 的特化闭包内**无任何** `!ejit.may_const` load（特化无收益，考虑移除 `ejit_entry`） |
| `-mllvm -ejit-warn-unused-dim` | **on** | #2 告警 | 声明了 `ejit_period_arr_ind("P")` 但闭包**从不索引** `ejit_period_arr("P")`（死维度） |
| `-mllvm -ejit-report-mayconst` | **off** | info 报告 | 报告每个 entry 闭包内 `may_const` 读总数 K 及其中在循环内的 J |
| `-mllvm -ejit-warn-few-mayconst=<N>` | `0`（off） | opt-in 告警 | entry 闭包内 `may_const` 读数 **< N**，且其中**没有任何循环内读取**时告警。默认关闭 |

**输出文本**（走 `stderr`）：

```
#1    EJit warning: ejit_entry function '<name>' reads no ejit_may_const field in its specialization closure; no JIT specialization value, consider removing ejit_entry
#2    EJit warning: ejit_entry function '<name>' declares ejit_period_arr_ind('<P>') but its specialization closure never indexes an ejit_period_arr('<P>'); unused specialization dimension, consider removing it
info  EJit info: ejit_entry function '<name>': <K> ejit_may_const read[s] (<J> in loops)   # K==1 时 read 无 s
few   EJit warning: ejit_entry function '<name>' has only <K> ejit_may_const read[s] in its specialization closure (threshold: <N>); low specialization surface, consider adding more may-const fields   # K==1 时 read 无 s
```

- **关闭默认 on 的告警**：`-mllvm -ejit-warn-no-specialization=false`（同理 `-ejit-warn-unused-dim=false`）。
- **info 报告**只提供信息，不控制编译流程。`may_const` 数量并不能可靠衡量特化价值：单次读取若位于热循环中或直接控制分支，也可能带来很高收益。因此唯一不会误报的默认阈值是 0（即 #1）；N>0 时需要用户结合场景判断。
- **`-ejit-warn-few-mayconst=N`** 是 opt-in 的个数阈值告警，默认关闭。只要闭包中存在循环内 `may_const` 读取就不会触发，避免把单个但位于热循环中的高价值读取误报为“特化面较窄”。它不与 #1 冲突：#1（零=确定无价值）是默认 sound 基线；N>0 时仍需结合这些读取控制的分支或计算人工判断。
- “引用 period 但未声明依赖”的检查现位于 Clang Sema，且默认关闭；需要时启用 §9.1 的 `-Wembedded-jit-undeclared-period-dep`。它与 #2（声明但不用）方向相反、互补。

### 9.3 提取位码转储

```bash
clang -fembed-bitcode ... -mllvm -ejit-dump-bitcode-dir=/tmp/ejit_bc process.c
```

在编译期把每个 TU 提取出的 EmbeddedJIT 位码（`.bc` + `.ll`）落盘到指定目录，文件名含 PID + 模块名，便于并行 `-j` 构建不冲突。用于调试符号提取 / 闭包计算结果。默认空（关闭）。

---

## 10. 常见问题排查

### 10.1 首次上板（bring-up）

1. `ejit_init()` 后调用 `ejit_print_version()` 记录运行库构建标识，与预期源码版本对齐。
2. `ejit_print_registry()` 确认 AOT 注册表正确（bitcode / period 数组 / 静态变量都到位）。
3. 默认 `EJIT_LOG_INFO` 观察 init / 编译 / cache MISS 等关键事件；必要时 `ejit_set_log_level(EJIT_LOG_VERBOSE)`。

### 10.2 某个 `ejit_entry` 没收益 / 没被特化

1. 编译期先看 §9.2：#1 告警（闭包无 `may_const`）说明该 entry 特化无收益，应移除 `ejit_entry` 或补 `may_const` 标注；`-mllvm -ejit-report-mayconst` 看 `may_const` 读总数及循环内占比，评估收益。
2. 若声明和定义分离，确认没有 §9.1 的 `-Wembedded-jit-attr-missing-on-def` 告警；该告警出现时定义已被禁用特化。
3. 运行时 `ejit_print_func_meta(name)` 确认特化参数绑定与 `may_const` 资格。
4. `ejit_print_active()` 确认相关 period 实例是否处于激活态。

### 10.3 该编译的没编译 / 编译失败

1. `ejit_get_last_error()` 读失败原因（code / message / funcName）。
2. `EJIT_LOG_VERBOSE` 看逐次 `compile_or_get` 与 taskpool 请求。
3. `ejit_taskpool_get_stats()` 看 `compileFailed` / `publishFailed` / `queueFull` / `queueApproxSize`，判断是失败、被拒还是拥塞。
4. `ejit_taskpool_print_compiled()` 看实际编译了哪些特化。

### 10.4 命中率低 / 性能差

1. `ejit_taskpool_get_stats()` 读 `cacheHits` / `asyncCompiles` / `alreadyPending`。
2. 若 wrapper 开销可疑，对 ejit 代码加 `-mllvm -ejit-wrapper-timing`，观察周期汇总中查找 / 调用 / 释放各段耗时。
3. `ejit_get_stats()` 是已退役 LRU 的兼容接口，当前恒返回零，不能用于分析 taskpool 命中率。

### 10.5 代码内存趋紧 / 耗尽

1. `ejit_get_code_pool_stats()` 看 `usedBytes` vs `reservedBytes`、`wastedBytes`、`poolCount`。
2. 固定代码段模式下观察 §2.3 的 `enableRwRange` 日志是否有 `FAIL` / `rollback`。
3. `ejit_clear_cache()` 不释放已生成的代码内存；若代码池接近耗尽，应调整代码池配置或由 EJIT 提供方确认当前版本支持的回收方案。

### 10.6 AArch64 分支超范围

`EJIT_LOG_INFO` 观察 §8 的 relax / `[STUBBED]` / linkdiag 汇总：`out-of-range` / `exceed +-128MB` 计数非零即存在超范围分支走了 stub（间接跳），影响性能。VERBOSE 可看每条直跳 / stub 的距离。

### 10.7 想看某函数特化结果

`ejit_dump_func(name)` 捕获后续特化，`ejit_print_dumped(name)` 回读该 entry 的单函数 IR+ASM；需要查看其被调函数及完整模块时改用 `ejit_print_dumped_module(name)`。捕获与回读必须位于执行编译的同一 worker 核。

---

## 附录：状态码

```c
EJIT_OK                  =  0
EJIT_PENDING             =  1
EJIT_ERR_INVALID_PARAM   = -1
EJIT_ERR_NOT_ACTIVE      = -2
EJIT_ERR_COMPILE_FAILED  = -3
EJIT_ERR_CACHE_FULL      = -4
EJIT_ERR_MEMORY          = -5
EJIT_ERR_BITCODE_NOT_FOUND = -6
EJIT_ERR_QUEUE_FULL      = -7
EJIT_ERR_DEDUP_FULL      = -8
EJIT_ERR_DISABLED        = -9
EJIT_ERR_INSTANCE_DISABLED = -10
```

> 所有运行时诊断 API 的头文件：`llvm/include/llvm/ExecutionEngine/EJIT/EJitRuntime.h`（公共 C ABI）。编译期诊断由 clang Sema 与 EJIT AOT pass 实现。
