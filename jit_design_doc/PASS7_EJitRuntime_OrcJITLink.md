# EmbeddedJIT 运行时库设计文档 — 基于 OrcJIT + JITLink

**版本**: 2.0
**日期**: 2026-07-23
**关联**: SPEC4.md, PLAN4.md, PASS1–6 设计文档

> **v2 更新 (2026-07)**: 本文档已按当前实现同步。同步和异步请求共用
> `EJitCompileDriver`、ORC 编译链以及 taskpool cache/dedup 协议；旧的
> `EJitSyncCompiler`、`EJitAsyncCompiler`、LRU `EJitCache` 和双引擎设计
> 已移入明确标记的折叠历史区，不再作为现行实现。
**类型**: 运行时库 (libejit.a)
**核心框架**: LLVM OrcJIT + JITLink

---

## 1. 架构概述

### 1.1 设计目标

EmbeddedJIT 运行时库基于 LLVM OrcJIT + JITLink 构建，支持同步 (Sync) 和异步 (Async) 两种编译模式，并针对嵌入式环境的资源受限特点（Flash 存储、低 RAM）进行定制。

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         EmbeddedJIT 运行时库 (libejit.a)                      │
│                                                                             │
│  ┌──────────────────────┐    ┌──────────────────────┐                       │
│  │   C 语言 API 层        │    │   C++ 内部 API 层      │                       │
│  │   (EJitRuntime.h)     │    │   (EJit.h)            │                       │
│  │   ejit_init/shutdown  │    │   EJit class          │                       │
│  │   ejit_activate/      │    │   Config / Registry   │                       │
│  │     deactivate        │    │                       │                       │
│  │   ejit_taskpool_compile_or_get │    │                       │                       │
│  └──────────┬───────────┘    └───────────┬───────────┘                       │
│             │                            │                                    │
│  ┌──────────┴────────────────────────────┴──────────────────────────────┐   │
│  │                      EJitRuntimeCore (核心状态管理)                     │   │
│  │                                                                       │   │
│  │  ┌───────────────┐  ┌───────────────────────────┐                       │   │
│  │  │ PeriodArray    │  │ RuntimeState              │                       │   │
│  │  │ Registry       │  │ (activate/deactivate)     │                       │   │
│  │  └───────────────┘  └───────────────────────────┘                       │   │
│  └───────────────────────────────────────────────────────────────────────┘   │
│                                    │                                          │
│  ┌─────────────────────────────────┴────────────────────────────────────┐   │
│  │                    EJitOrcEngine (OrcJIT + JITLink 封装)               │   │
│  │                                                                       │   │
│  │  ┌─────────────┐  ┌──────────────────┐  ┌────────────────────────┐  │   │
│  │  │ LLJIT-based │  │ IRTransformLayer │  │ EJitJITLinkMemoryMgr   │  │   │
│  │  │ Engine      │  │ (EJitStructField │  │ (嵌入式内存管理)         │  │   │
│  │  │             │  │  IR Transform    │  │                        │  │   │
│  │  └─────────────┘  └──────────────────┘  └────────────────────────┘  │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                    │                                          │
│  ┌─────────────────────────────────┴────────────────────────────────────┐   │
│  │                     Taskpool 编译调度层                                 │   │
│  │                                                                       │   │
│  │  ┌─────────────────────────┐    ┌───────────────────────────────┐    │   │
│  │  │ EJitTaskPool            │    │ EJitSharedTaskPool            │    │   │
│  │  │ (单实例调度)              │    │ (跨核共享 + 单 worker)          │    │   │
│  │  └─────────────────────────┘    └───────────────────────────────┘    │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                    │                                          │
│  ┌─────────────────────────────────┴────────────────────────────────────┐   │
│  │                      Taskpool Code Cache                              │   │
│  │  ┌──────────────────────┐  ┌────────────────────────────────────┐    │   │
│  │  funcIndex + dims      │  │ bucket/slot + version gate        │    │   │
│  │  + versions/generation │  │ shared publish protocol           │    │   │
│  │  └──────────────────────┘  └────────────────────────────────────┘    │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 1.2 选择 OrcJIT + JITLink 的理由

| 考量 | MCJIT (旧) | OrcJIT + JITLink (新) |
|------|-----------|---------------------|
| 内存管理 | 固定 SectionMemoryManager | JITLinkMemoryManager 完全可覆盖 |
| 并发支持 | 无内置线程池 | ExecutionSession 可扩展；当前内部线程数固定为 0 |
| 模块隔离 | 弱 | JITDylib + ResourceTracker 精确控制 |
| 嵌入式适配 | 困难 | 自定义 allocator + BasicLayout |
| LLVM 演进 | Legacy (维护模式) | 当前主力 |
| IR 变换 | 无内置 | IRTransformLayer 天然支持 |

---

## 2. 核心组件设计

### 2.1 EJitOrcEngine — LLJIT 封装

EJitOrcEngine 封装 LLJIT 实例，管理 JIT 编译生命周期。

```cpp
// llvm/lib/ExecutionEngine/EJIT/EJitOrcEngine.h

namespace llvm::ejit {

class EJitOrcEngine {
    struct Impl;
    std::unique_ptr<Impl> P;               // LLJIT、optimizer、activeCtx 等私有状态

public:
    static Expected<std::unique_ptr<EJitOrcEngine>>
    Create(const Config& config,
           PeriodArrayRegistry& periodReg,
           EJitRuntimeState& runtimeState);

    Error loadBitcodeModule(StringRef bitcodeData,
                            uint64_t cacheKey,
                            const std::string& funcName);

    Expected<void*> lookup(uint64_t cacheKey,
                           const std::string& funcName);

    void setActiveContext(const SpecializationContext* ctx);
    void addUserSymbol(const std::string& name, void* addr);
};

} // namespace llvm::ejit
```

### 2.1.1 LLJIT 创建流程

```cpp
Expected<std::unique_ptr<EJitOrcEngine>>
EJitOrcEngine::Create(const EJitConfig& config) {
    auto engine = std::make_unique<EJitOrcEngine>();

    // 注意: 使用 Expected<> 返回错误，不在嵌入式场景调用 cantFail/abort

    // 步骤 1: 配置 JITTargetMachineBuilder
    auto JTMBOrErr = JITTargetMachineBuilder::detectHost();
    if (!JTMBOrErr) {
        return JTMBOrErr.takeError();
    }
    auto JTMB = std::move(*JTMBOrErr);

    // 步骤 2: 构建 LLJIT。ORC 内部编译线程始终关闭；
    // 异步性由 ORC 外部的 taskpool worker 提供。
    orc::LLJITBuilder Builder;

    Builder.setJITTargetMachineBuilder(JTMB);

    // 启用 code pool 时注入 EJitCodePoolMemoryManager。
    Builder.setObjectLinkingLayerCreator(
        [&](orc::ExecutionSession& ES)
            -> Expected<std::unique_ptr<orc::ObjectLayer>> {
            return std::make_unique<orc::ObjectLinkingLayer>(
                ES, makeCodePoolMemoryManager());
        });

    Builder.setNumCompileThreads(0);

    // 创建 LLJIT 实例
    auto JOrErr = Builder.create();
    if (!JOrErr) {
        return JOrErr.takeError();
    }
    engine->J = std::move(*JOrErr);

    // PreFixup: retarget standard AArch64 pointer-jump stubs (branch ->
    // $__STUBS -> $__GOT -> destination) to a direct B/BL when the resolved
    // displacement fits the architectural ±128 MiB range. This plugin must
    // run before the diagnostic plugin so PostFixup reporting sees the actual
    // direct/stubbed result.
    if (auto *OLL = dyn_cast<orc::ObjectLinkingLayer>(
            &engine->J->getObjLinkingLayer()))
        OLL->addPlugin(std::make_shared<EJitLinkOptimizationPlugin>());

    // PostFixup: EJitLinkDiagPlugin audits every AArch64 branch relocation
    // and reports which remain bridged through a stub+GOT (out of ±128 MiB
    // or unsafe chain) instead of a direct BL. INFO emits one summary per
    // graph; VERBOSE additionally emits each relocation.
    if (auto *OLL = dyn_cast<orc::ObjectLinkingLayer>(
            &engine->J->getObjLinkingLayer()))
        OLL->addPlugin(std::make_shared<EJitLinkDiagPlugin>());

    // 步骤 3: 注册 IRTransformLayer 回调
    // 完整的 JIT Pipeline: 参数替换 → InstCombine → StructFieldPass → IPSCCP →
    //   InstCombine → StructFieldPass → 模块清理 → 标准优化 → 向量化 (详见 §2.4)
    // 注意: IRTransformLayer::TransformFunction 签名为
    //   Expected<ThreadSafeModule>(ThreadSafeModule, MaterializationResponsibility&)
    // withModuleDo 的回调签名为 Expected<Error>(Module&)，原地修改 Module
    engine->J->getIRTransformLayer().setTransform(
        [engine](orc::ThreadSafeModule TSM,
                 orc::MaterializationResponsibility& R)
            -> Expected<orc::ThreadSafeModule> {
            Error Err = TSM.withModuleDo([engine](Module& M) -> Error {
                const SpecializationContext* ctx =
                    engine->getActiveContext();
                if (!ctx)
                    return Error::success();

                // 完整流水线实现于 EJitOptimizer::runPipeline
                // (llvm/lib/ExecutionEngine/EJIT/EJitOptimizer.cpp)。JIT 侧不再
                // 单独运行 Inline —— callee 已在 AOT 预优化
                // (EJitRegisterBitcodePass: AlwaysInline + ModuleInliner(O2)) 内联，
                // 跨函数常量改由 IPSCCP 在调用边界传播。
                //   1a preReplacePeriodIndices —— ejit_period_arr_ind 参数 → 常量
                //   1b runInstCombine          —— 折叠常量 GEP 链
                //   1c EJitStructFieldPass      —— may_const load → 常量
                //   1d runInterproceduralPropagation —— 内部化非 entry 定义 + IPSCCP
                //   1e/1f runInstCombine + EJitStructFieldPass（对 callee 再做一轮）
                //   1g runModuleCleanup —— RPO attrs + DAE + GlobalDCE（模块级清理）
                //   2-4 LowerExpect + buildFunctionSimplificationPipeline(O1/O2/O3)
                //        + 二次 StructFieldPass + cleanup(InstCombine/SCCP/SimplifyCFG/ADCE)
                //   5   向量化（仅 L2/L3，见 §2.4.3）：L2 = SLP + 部分展开；
                //        L3 加 LoopVectorize + LoopLoadElimination
                //   6   最终 GlobalDCE：2-5 阶段折叠 expect 守卫、删除调用点后
                //        清扫失去引用者的 callee（1g 时这些调用点尚未死）
                engine->P->optimizer->runPipeline(M, *ctx);

                return Error::success();
            });
            if (Err)
                return std::move(Err);
            return std::move(TSM);
        });

    // 步骤 4: 注册静态变量和用户显式注册的 JIT 外部符号。
    // 裸核构建关闭 host process-symbol 搜索；enable_ex、SRE_Task* 等
    // 平台符号仍必须由最终链接提供强定义，不能由 ejit_register_symbol 补齐。

    return engine;
}
```

### 2.1.2 AArch64 近目标分支松弛

JITLink 会先为外部 `B/BL` 目标建立
`branch -> $__STUBS -> $__GOT -> destination` 链。代码池与 AOT 代码距离较近时，
`EJitLinkOptimizationPlugin` 在 `PreFixup` 阶段读取已经解析的最终地址；若
`Branch26PCRel` 的位移处于 `[-2^27, 2^27 - 4]` 字节且四字节对齐，则将边直接
指向最终目标，由正常 fixup 编码单条 `B/BL`。否则保留原 stub 路径。

**门控用地址，不用 `isExternal()`**：JITLink 的 `applyLookupResult` 在 PreFixup
之前用 `Sym->getAddressable().setAddress(addr)` 解析外部符号，但**不**把它转成
`isAbsolute`，所以已解析的外部符号 `isExternal()` 仍为 `true`。若用
`if (Destination->isExternal()) continue` 门控，会把每个已解析外部 stub 全部跳过
-> 0 放松（这是曾出现的 bug）。pass 改为按地址门控
`if (TargetAddr == 0) continue`，仅跳过真正未解析的（如弱引用无定义的外部）。

**skip 分类与审计**：pass 在 `EJIT_DIAG` INFO 行按原因分类每个 stubbed
`Branch26PCRel`，与三个 skip 计数器对齐：`chain-mismatch`（stub/GOT 链不匹配，
含非零 GOT addend）、`unresolved`（`TargetAddr == 0`）、`out-of-range`（超
±128 MiB 或未对齐），以及 `relaxed`（已放松计数）。格式：
`relaxAArch64BranchStubs: graph=<name> Branch26PCRel: <total> total,
<stubbed> stubbed (chain-mismatch=<cm> unresolved=<ur> out-of-range=<oor>),
<relaxed> relaxed`。

**PostFixup 审计**：`EJitLinkDiagPlugin`（紧跟 optimization plugin 之后注册）对
每条 AArch64 分支重定位审计：INFO 汇总 "N stubbed (M exceed ±128MB)"，VERBOSE
逐条打印 `[STUBBED] <reloc> -> <target> (<dist>, within/EXCEEDS ±128MB)`。顺序
固定（optimization 先、diag 后），使 PostFixup 审计看到放松后的真实结果。

该优化只发生在链接阶段，不增加运行时热路径状态，也不改变 code-pool/shared
taskpool ABI。已经分配的 stub/GOT 不主动删除，因此本改动降低执行开销，但不以
缩小本次 JITLink allocation 为目标。

---

## 2.2 EJitCodePoolMemoryManager — 嵌入式内存管理器

### 2.2.1 设计目标

当前 `EJitCodePoolMemoryManager` 将 JITLink `BasicLayout` 的 segment
从 `EJitCodePoolManager` 按需分配，而不是预留一块固定 slab：

- code pool 默认大小为 2MiB，耗尽时按需创建下一池；
- `allocate` 按 layout 分配连续空间并记录所有 executable segment；
- 只对 executable segment 做 RX seal，rodata/GOT/writable data 不会误封；
- legacy 模式在 lookup 时封整个 2MiB pool；
- 4K 模式在 pool 创建时执行 `split_2m_to_4k`，finalize 后逐个
  executable page 调用 `enable_ex` 并同步 I-cache；
- `deallocate` 运行 JITLink dealloc actions，但当前不回收物理
  code-pool 内存，pool lifetime 与 engine 一致。

平台 adapter 只声明 `enable_ex`、`split_2m_to_4k` 和
`SRE_MemDbgAlloc`（固定代码池模式 `EJIT_FIXED_CODE_POOL=ON` 下不调 `SRE_MemDbgAlloc`，
改用链接脚本区域 `[__ejit_code_start, __ejit_code_end)`，并需 `enable_rw`，签名
`unsigned enable_rw(unsigned level, unsigned long long va)`，与 `enable_ex` 对称）。目标
最终链接必须提供强定义；这些平台依赖不能由 `ejit_register_symbol` 补齐。固定代码池区域
详见 `EJIT_SRE_CODE_POOL.md` §14。

<details>
<summary>历史设计：固定 slab EJitJITLinkMemoryManager（已删除）</summary>

| 约束 | 规格 |
|------|------|
| 最大代码区 | 512KB (可配置) |
| 页大小 | 4KB (ARM Cortex-A / AArch64) |
| 分配策略 | 固定大小的 slab 分配器 |
| 回收 | 在 deallocate 时调用 `munmap` 等效操作 |
| 并发安全 | 需要 mutex (异步编译时后台线程访问) |

### 2.2.2 实现

```cpp
// llvm/lib/ExecutionEngine/EJIT/EJitJITLinkMemoryManager.h

namespace llvm::ejit {

class EJitJITLinkMemoryManager : public jitlink::JITLinkMemoryManager {
public:
    EJitJITLinkMemoryManager(size_t maxTotalSize, uint64_t pageSize = 4096);

    // 分配: 解析 LinkGraph → 分配 Code + Data 段 → 返回
    void allocate(const jitlink::JITDylib* JD,
                  jitlink::LinkGraph& G,
                  OnAllocatedFunction OnAllocated) override;

    // 释放: 归还已 finalize 的分配
    void deallocate(std::vector<FinalizedAlloc> Allocs,
                    OnDeallocatedFunction OnDeallocated) override;

    // 统计信息
    size_t getCurrentUsage() const;
    size_t getMaxUsage() const;
    size_t getAllocationCount() const;

private:
    // 嵌入式 slab 分配器
    // 预分配一块连续内存 (如 512KB)，在其中分配固定大小段

    struct SlabRegion {
        void* baseAddr;              // slab 基地址
        size_t totalSize;            // 总大小
        size_t usedSize;             // 已使用大小
        std::mutex allocMutex;       // 分配锁
    };

    SlabRegion codeSlab_;            // 代码 slab (RX)
    SlabRegion dataSlab_;            // 数据 slab (RW)

    struct Allocation {
        FinalizedAlloc alloc;
        size_t size;
        uint64_t slabOffset;
    };
    std::vector<Allocation> activeAllocs_;

    // 使用 JITLink 的 BasicLayout 辅助划分段
    struct SegmentAlloc {
        orc::ExecutorAddr addr;
        size_t workingSize;
        size_t targetSize;
    };
    std::vector<SegmentAlloc> allocateSegments(LinkGraph& G);
    void applySegments(LinkGraph& G, const std::vector<SegmentAlloc>& segs);
};
```

```cpp
// allocate 实现
void EJitJITLinkMemoryManager::allocate(
    const jitlink::JITDylib* JD,
    jitlink::LinkGraph& G,
    OnAllocatedFunction OnAllocated) {

    // 步骤 1: 使用 BasicLayout 分析段布局
    BasicLayout BL(G);

    // 步骤 2: 为每个 AllocGroup 分配内存
    // AllocGroup 按 (MemProt, MemLifetime) 分类
    //   类型: ReadOnly (RO), ReadWrite (RW), ReadExec (RX)
    //   Lifetime: Standard, Finalize

    for (auto& KV : BL.segments()) {
        auto& AG = KV.first;       // AllocGroup (描述保护/生命周期)
        auto& Segs = KV.second;    // Segment 列表

        for (auto& Seg : Segs) {
            size_t allocSize = Seg.ContentSize + Seg.ZeroFillSize;

            // 选择 slab: code → RX, data → RW
            SlabRegion* slab = nullptr;
            if (AG.getMemProt() == orc::MemProt::Read ||
                AG.getMemProt() == orc::MemProt::ReadWrite) {
                slab = &dataSlab_;
            } else if (AG.getMemProt() == orc::MemProt::ReadExec) {
                slab = &codeSlab_;
            }

            if (!slab) {
                OnAllocated(nullptr);
                return;
            }

            // 从 slab 分配
            std::lock_guard<std::mutex> lock(slab->allocMutex);

            if (slab->usedSize + allocSize > slab->totalSize) {
                // OOM: 触发 Code Cache 淘汰
                OnAllocated(make_error<StringError>(
                    "EJIT: Code memory exhausted",
                    inconvertibleErrorCode()));
                return;
            }

            uintptr_t allocAddr = (uintptr_t)slab->baseAddr + slab->usedSize;
            slab->usedSize += allocSize;

            // 设置内存保护
            applyMemoryProtection(allocAddr, allocSize, AG.getMemProt());

            Seg.Addr = orc::ExecutorAddr(allocAddr);
            Seg.WorkingMem = MutableArrayRef<char>(
                (char*)allocAddr, Seg.ContentSize);
        }
    }

    // 步骤 3: 应用布局到 LinkGraph
    BL.apply();

    // 步骤 4: 创建 InFlightAlloc 并传给 callback
    auto FA = std::make_unique<EJitInFlightAlloc>(...);
    OnAllocated(std::move(FA));
}
```

### 2.2.3 嵌入式优化: 固定 slab 分配

```
嵌入式 Code Cache 内存布局 (2MB 示例):

┌──────────────────────────────────────────────────────────────┐
│                      Code Slab (RX) — 2MB                   │
│  ┌──────────┬──────────┬──────────┬───────────────────────┐  │
│  │ func_1   │ func_2   │ func_3   │ ... (free)            │  │
│  │ (48KB)   │ (32KB)   │ (56KB)   │                       │  │
│  └──────────┴──────────┴──────────┴───────────────────────┘  │
├──────────────────────────────────────────────────────────────┤
│                      Data Slab (RW) — 128KB                   │
│  ┌──────────┬──────────┬───────────────────────────────────┐ │
│  │ data_1   │ data_2   │ ... (free)                        │ │
│  └──────────┴──────────┴───────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────┘

分配策略:
- Code Slab: 按顺序分配 (bump allocator), 不支持单独释放
  → deallocate 只在整个 slab 重置时生效 (clearCache)
- Data Slab: 同上

LRU 淘汰时:
- 当 slab 使用率超过阈值 → evictLRU()
- 重置整个 slab → 重新编译活跃函数
- 或者: 使用更好的 allocator 支持碎片回收 (后续优化)
```

</details>

---

## 2.3 Taskpool 同步/异步调度

当前同步和异步模式使用同一条编译链：

```text
wrapper
  └─ ejit_taskpool_compile_or_get[_0d...4d]
       ├─ cache hit → 返回 JIT fnPtr
       └─ cache miss
            ├─ Off   → 返回 fallback
            ├─ Sync  → 当前调用栈 compileNow → compileCold → publish
            └─ Async → dedup + MPSC queue → 返回 fallback
                                      └─ 单 worker 消费并 publish
```

- `EJitTaskPool` 用于单实例调度；`EJitSharedTaskPool` 将队列、dedup、
  lifecycle version 和 cache 放入跨核共享 POD 状态。
- shared 模式通过 CAS 选出一个 owner，只由 owner 启动一个 worker。
  worker 使用 owner 私有的 `EJitCompileDriver` 和 `EJitOrcEngine`；peer
  可以拥有自己的初始化对象，但不另启 worker 编译同一共享请求。
- ORC 内部始终 `setNumCompileThreads(0)`。目标侧 worker 由
  `EJitSreTask_sre.cpp` 的平台任务接口驱动，不依赖 C++ 线程库。
- worker 在 `Initializing` 或 Ready 空队列时调用平台 yield；退出前由
  owner stop/join，之后才允许析构 owner 私有 ORC 状态。
- queue full 会回滚 dedup；编译前后及 publish 前会重查 generation 和
  lifecycle version，避免 deactivate/re-init 后发布旧结果。

<details>
<summary>历史设计：独立 Sync/Async compiler 与 C++ 后台线程（已删除）</summary>

### 2.3.1 隔离架构

```
                         ┌─────────────┐
                         │ 调用线程      │
                         │ (用户代码)    │
                         └──────┬──────┘
                                │
                    ejit_taskpool_compile_or_get()
                                │
                    ┌───────────┴───────────┐
                    │  EJitCompileDriver    │
                    │  (编译调度器)           │
                    └───────────┬───────────┘
                                │
              ┌─────────────────┴─────────────────┐
              │                                   │
     ┌────────┴────────┐                 ┌───────┴──────────┐
     │ Sync Path       │                 │ Async Path        │
     │                 │                 │                   │
     │ 调用线程执行:     │                 │ 提交到:            │
     │   loadModule    │                 │   AsyncCompiler   │
     │   runPasses     │                 │   (后台线程)        │
     │   codeGen       │                 │       │           │
     │   cache.put     │                 │   loadModule      │
     │   return pfn    │                 │   runPasses       │
     │                 │                 │   codeGen         │
     │  阻塞直到完成     │                 │   cache.put       │
     │                 │                 │                   │
     │  返回: pfn/NULL │                 │  返回: NULL       │
     └─────────────────┘                 └───────────────────┘
```

### 2.3.2 同步编译器

```cpp
// 同步编译器: 在调用线程上执行完整编译流程
class EJitSyncCompiler {
public:
    struct Result {
        void* funcPtr;               // 特化函数指针 (NULL 表示失败)
        size_t compileTimeMs;        // 编译耗时
        size_t codeSize;             // 代码大小
    };

    Result compile(EJitOrcEngine& engine,
                   const std::string& bitcodeData,
                   const SpecializationContext& ctx) {
        Result result = {nullptr, 0, 0};
        auto startTime = std::chrono::steady_clock::now();

        // Step 1: 设置编译上下文 (供 IRTransformLayer 回调读取)
        engine.ActiveCtx = &ctx;
        engine.OptLevel = ctx.optLevel;

        // Step 2: 加载 bitcode module 到 LLJIT (使用 uint64_t cacheKey)
        if (auto Err = engine.loadBitcodeModule(bitcodeData, ctx.cacheKey, ctx.fnName)) {
            engine.ActiveCtx = nullptr;
            return result;
        }

        // Step 3: lookup 触发 materialization
        auto addr = engine.lookup(ctx.cacheKey, ctx.fnName);
        engine.ActiveCtx = nullptr;  // 清理上下文
        if (!addr) {
            return result;
        }

        result.funcPtr = addr->toPtr<void*>();
        result.codeSize = engine.getModuleCodeSize(ctx.fnName);

        auto endTime = std::chrono::steady_clock::now();
        result.compileTimeMs = std::chrono::duration_cast<
            std::chrono::milliseconds>(endTime - startTime).count();

        return result;
    }
};
```

### 2.3.3 异步编译器

```cpp
// 异步编译器: 后台线程 + 请求队列 + 隔离引擎实例
// v2: 已移除，由 taskpool worker 取代
// 以下为历史设计参考
public:
    EJitAsyncCompiler(EJitConfig& config, EJitCache& cache,
                       EJitRuntimeState& runtimeState);
    ~EJitAsyncCompiler();

    // 启动后台线程
    void start();

    // 停止后台线程 (等待当前编译完成)
    void stop();

    // 提交异步编译请求 (非阻塞)
    // 若相同 cacheKey 已有编译在进行中，直接忽略 (dedup)
    void submitRequest(CompileRequest req);

private:
    // 后台线程主循环
    void workerLoop();

    // 执行单次编译 (在后台线程中)
    void compileOne(const CompileRequest& req);

    // === 线程安全隔离 ===

    // 后台线程拥有独立的 LLVM 资源:
    std::unique_ptr<EJitOrcEngine> workerEngine_;  // 独立 LLJIT 实例
    // 注意: workerEngine_ 有自己的 LLVMContext, TargetMachine
    // 与用户调用线程完全隔离，避免 LLVMContext 竞态

    // 共享的只读数据 (线程安全):
    EJitConfig& config_;           // 编译配置 (const 访问)
    EJitCache& cache_;            // Code Cache (内部有 mutex)
    EJitRuntimeState& runtimeState_; // 时间窗激活状态 (内部有 mutex)

    // 线程控制
    std::thread workerThread_;
    std::queue<CompileRequest> requestQueue_;
    std::mutex queueMutex_;
    std::condition_variable queueCV_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};

    // 正在编译的请求集合 (按 uint64_t cacheKey)
    // 防止同一 cacheKey 重复提交编译请求
    std::set<uint32_t> requestsInFlight_;
    std::mutex inFlightMutex_;
};

// 编译请求 (v1.7: cacheKey 在 ctx 内, 去掉冗余字段)
struct CompileRequest {
    std::string funcName;
    std::string bitcodeData;
    SpecializationContext ctx;         // 含 uint64_t cacheKey
    uint64_t timestamp;
};
```

```cpp
void EJitAsyncCompiler::submitRequest(CompileRequest req) {
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        // 去重: 相同 cacheKey (uint64_t) 已有编译在进行中，跳过
        if (requestsInFlight_.count(req.ctx.cacheKey)) {
            return;
        }
        requestsInFlight_.insert(req.ctx.cacheKey);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        requestQueue_.push(std::move(req));
    }
    queueCV_.notify_one();
}

void EJitAsyncCompiler::workerLoop() {
    while (!stopping_.load()) {
        CompileRequest req;

        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCV_.wait(lock, [this] {
                return !requestQueue_.empty() || stopping_.load();
            });

            if (stopping_.load()) break;
            if (requestQueue_.empty()) continue;

            req = std::move(requestQueue_.front());
            requestQueue_.pop();
        }

        // 在后台线程编译 (使用隔离的 Engine 实例)
        compileOne(req);
    }

    // 处理完剩余请求
    while (true) {
        CompileRequest req;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (requestQueue_.empty()) break;
            req = std::move(requestQueue_.front());
            requestQueue_.pop();
        }
        compileOne(req);
    }
}

void EJitAsyncCompiler::compileOne(const CompileRequest& req) {
    // 异步安全：编译前重新检查时间窗激活状态
    // getOrCompile() 中的 isPeriodActive 检查与 compileOne() 实际执行之间
    // 存在时间窗，用户线程可能在此期间调用了 deactivate()。
    // 若时间窗已失效，跳过编译并清理 in-flight 记录。
    if (!runtimeState_->isPeriodActive(req.ctx)) {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        requestsInFlight_.erase(req.ctx.cacheKey);
        return;
    }

    // 内存序保证：在读取 may_const 字段值之前获取同步屏障
    // 确保用户线程在 activate() 中对全局变量的写入对编译线程可见
    // （与 RuntimeState::activate 中的 release 屏障配对）
    std::atomic_thread_fence(std::memory_order_acquire);

    // 设置编译上下文 (供 IRTransformLayer 回调读取)
    workerEngine_->ActiveCtx = &req.ctx;
    workerEngine_->OptLevel = req.ctx.optLevel;

    EJitSyncCompiler syncCompiler;
    auto result = syncCompiler.compile(*workerEngine_,
                                       req.bitcodeData,
                                       req.ctx,
                                       req.ctx.cacheKey);

    workerEngine_->ActiveCtx = nullptr;  // 清理上下文

    if (result.funcPtr) {
        // 编译成功: 存入 Cache (内部 mutex 保护)
        cache_.put(req.ctx.cacheKey, result.funcPtr, result.codeSize);
    }
    // 编译失败: 不存缓存, 后续调用 retry 或 fallback

    // 编译完成 (成功或失败), 从 in-flight 集合移除
    {
        std::lock_guard<std::mutex> lock(inFlightMutex_);
        requestsInFlight_.erase(req.ctx.cacheKey);
    }
}
```

### 2.3.4 隔离级别总结

| 资源 | 同步模式 | 异步模式 |
|------|---------|---------|
| LLVMContext | 调用线程拥有 (单实例) | 后台线程拥有独立实例 |
| LLJIT (ExecutionSession) | 调用线程使用 | 后台线程拥有独立 LLJIT |
| TargetMachine | 调用线程使用 | 后台线程独立创建 |
| MemoryManager (JITLink) | 调用线程使用 | **独立 slab**（同步/异步引擎各自拥有 JITLinkMemoryManager，避免 bump-allocator 空间竞争） |
| Code Cache | 调用线程访问 | 共享 (mutex 保护) |
| PeriodArrayRegistry | 调用线程更新 (activate/deactivate) | 只读快照 |

</details>

---

## 2.4 IRTransformLayer — EJitStructFieldPass 集成

### 2.4.1 Transform 回调

Transform 回调在 §2.1.1 `EJitOrcEngine::Create()` 中注册为内联 lambda。
`EJitCompileDriver::compileCold` 在 load/lookup 前通过
`setActiveContext(&ctx)` 安装当前特化上下文，并在所有返回路径清空；
回调从 engine 私有实现读取该上下文。shared 模式只有 elected owner
worker 进入这条编译链，不会由多个 core 并发改写同一个 `activeCtx`。

```cpp
// §2.1.1 中的注册代码 (简化引用)
engine->J->getIRTransformLayer().setTransform(
    [engine](orc::ThreadSafeModule TSM, ...) -> Expected<orc::ThreadSafeModule> {
        Error Err = TSM.withModuleDo([engine](Module& M) -> Error {
            const SpecializationContext* ctx = engine->getActiveContext();
            if (!ctx)
                return Error::success();

            // 全部阶段封装在 EJitOptimizer::runPipeline 内（见 §2.4.2/§2.4.3）：
            //   preReplacePeriodIndices → runInstCombine → EJitStructFieldPass
            //   → runInterproceduralPropagation(内部化 + IPSCCP)
            //   → runInstCombine → EJitStructFieldPass
            //   → runOptimizationPipeline(LowerExpect + buildFunctionSimplificationPipeline)
            // JIT 侧不再运行 Inline（AOT 预优化已内联）。
            engine->P->optimizer->runPipeline(M, *ctx);
            return Error::success();
        });
        if (Err)
            return std::move(Err);
        return std::move(TSM);
    });
```

`preReplacePeriodIndices`、`runInstCombine`、`runInterproceduralPropagation`、`runOptimizationPipeline` 现为 `EJitOptimizer` 的成员函数（`llvm/lib/ExecutionEngine/EJIT/EJitOptimizer.cpp`，详见 §2.4.2, §2.4.3）。
### 2.4.2 JIT Pipeline 各阶段实现

**`preReplacePeriodIndices`** — 将 `ejit_period_arr_ind` 参数替换为运行时常量值：

```cpp
void preReplacePeriodIndices(Module& M, SpecializationContext* ctx) {
    Function* F = M.getFunction(ctx->fnName);
    if (!F) return;

    for (int i = 0; i < ctx->period_count; ++i) {
        // 从函数 metadata 查找 periodName 对应的参数索引
        int argIdx = findPeriodArrIndArg(F, ctx->dimensions[i].periodName);
        if (argIdx < 0) continue;

        Argument* arg = F->getArg(argIdx);
        Constant* constVal = ConstantInt::get(arg->getType(),
                                               ctx->dimensions[i].cellIdx);
        arg->replaceAllUsesWith(constVal);
    }
}
```

**`runInstCombine`** — 在参数替换后传播常量，折叠初始分支：

```cpp
void runInstCombine(Module& M) {
    FunctionPassManager FPM;
    FPM.addPass(InstCombinePass());
    FunctionAnalysisManager FAM;
    // 为每个函数运行 InstCombine
    for (Function& F : M) {
        if (!F.isDeclaration())
            FPM.run(F, FAM);
    }
}
```

**`runInterproceduralPropagation`** — 内部化所有非 `ejit_entry` 定义后运行 IPSCCP，把 1a–1c 在每个调用点变成常量的实参推进 callee 函数体（并把常量返回值推回调用点）。JIT 侧**不再单独运行 Inline**：callee 已在 AOT 预优化（`EJitRegisterBitcodePass`：AlwaysInline + ModuleInliner(O2)）内联。

**`runModuleCleanup`** — 特化后的模块级清理（Phase 1g，所有档位都跑）：`ReversePostOrderFunctionAttrsPass` 推断函数属性供下游折叠，`DeadArgumentEliminationPass` 删除被 IPSCCP 常量化的参数（只改写 local-linkage 函数，外部 `ejit_entry` 不受影响），`GlobalDCEPass` 删除失去调用点的函数——缩小 JIT 后端需编译的代码量。注意：此轮 DCE 看不到 expect 守卫（`__builtin_expect`）的死半边——守卫要到 Phase 2（LowerExpect）才折叠；那些调用点在 Phase 2-5 才死掉的 callee 由 Phase 6 的最终 GlobalDCE 清扫（见 §2.4.3）。

```cpp
void EJitOptimizer::runInterproceduralPropagation(Module& M) {
    // IPSCCP 仅能推理 local linkage 且未被取地址函数的实参；
    // 特化模块自包含（只对外查找 ejit_entry 符号），因此把每个
    // 非 entry 定义内部化，使 IPSCCP 能枚举其所有调用点。
    for (Function& F : M.functions()) {
        if (F.isDeclaration() || F.hasLocalLinkage())
            continue;
        if (hasMDStringEntry(F.getMetadata(MD_EJIT_METADATA), TAG_EJIT_ENTRY))
            continue;
        F.setVisibility(GlobalValue::DefaultVisibility);
        F.setLinkage(GlobalValue::InternalLinkage);
    }
    ModulePassManager MPM;
    MPM.addPass(IPSCCPPass());
    MPM.run(M, MAM_);
}
```

### 2.4.3 优化 Pipeline (L1/L2/L3) — 第二次 StructFieldPass 之后

后特化清理直接复用 **真实的 LLVM 函数级简化流水线** `PassBuilder::buildFunctionSimplificationPipeline`（O1/O2/O3 各预建一份并缓存），而不是手工拼装的 pass 序列。`ctx.optLevel`（L1→O1、L2→O2、L3→O3）选择这三条已缓存的 FPM 之一，并**门控 Phase 5 向量化**（L2/L3 运行、L1 跳过）；Phase 1a–1g 与 Phase 2/4 对所有档位一致。注意这是**函数级**简化流水线，并非完整的 `clang -O2` module 流水线——除 phase 1d 的 IPSCCP 和 phase 1g 的模块清理（RPO attrs + DAE + GlobalDCE）外，不含 GlobalOpt、CalledValuePropagation、ArgumentPromotion 等 module/CGSCC pass。

```cpp
// 构造时预建（EJitOptimizer 构造函数）
// TM 由引擎用同一个 JTMB createTargetMachine 后传入（EJitOrcEngine::Create）；
// 无 TM 时 TTI 退化为 32-bit 基线，向量化永不触发（见本节约末的注）。
PassBuilder PB(TM);
PB.registerModuleAnalyses(MAM_);   PB.registerCGSCCAnalyses(CGAM_);
PB.registerFunctionAnalyses(FAM_); PB.registerLoopAnalyses(LAM_);
PB.crossRegisterProxies(LAM_, FAM_, CGAM_, MAM_);
lowerExpectFPM_.addPass(LowerExpectIntrinsicPass());        // Phase 2
simplifyO1_ = PB.buildFunctionSimplificationPipeline(O1, ThinOrFullLTOPhase::None);
simplifyO2_ = PB.buildFunctionSimplificationPipeline(O2, ThinOrFullLTOPhase::None);
simplifyO3_ = PB.buildFunctionSimplificationPipeline(O3, ThinOrFullLTOPhase::None);
cleanupFPM_.addPass(InstCombinePass());  cleanupFPM_.addPass(SCCPPass());
cleanupFPM_.addPass(SimplifyCFGPass());  cleanupFPM_.addPass(ADCEPass());
cleanupMPM_.addPass(ReversePostOrderFunctionAttrsPass());   // Phase 1g
cleanupMPM_.addPass(DeadArgumentEliminationPass());
cleanupMPM_.addPass(GlobalDCEPass());
vectorizeL2_ = buildVectorizeFPM(PTO, /*SpeedupLevel=*/2, /*EnableLoopVectorize=*/false); // Phase 5
vectorizeL3_ = buildVectorizeFPM(PTO, /*SpeedupLevel=*/3, /*EnableLoopVectorize=*/true);
finalDCEMPM_.addPass(GlobalDCEPass());  // Phase 6: 最终死代码清扫

void EJitOptimizer::runOptimizationPipeline(Module& M, ejit::OptimizationLevel level) {
    FunctionPassManager& simplifyFPM = simplifyFPMForLevel(level); // L1→O1 / L2→O2 / L3→O3
    for (Function& F : M.functions())
        if (!F.isDeclaration()) {
            lowerExpectFPM_.run(F, FAM_);   // Phase 2: LowerExpect（不在简化流水线内）
            simplifyFPM.run(F, FAM_);       // Phase 3: 真实 -Ox 函数简化流水线
        }
    // Phase 4: 展开暴露出新的常量下标数组访问后，再跑一次 StructFieldPass + 轻量 cleanup
    runStructFieldPass(M);
    for (Function& F : M.functions())
        if (!F.isDeclaration())
            cleanupFPM_.run(F, FAM_);
    // Phase 5: 特化后向量化（仅 L2/L3）。L2 = SLP + 部分展开；L3 加 LoopVectorize。
    // 在最终 StructFieldPass 之后运行，向量化器看到的是完全特化的循环。
    if (level >= L2)
        runVectorization(M, level);
    // Phase 6 在 runPipeline 中：runOptimizationPipeline 之后对模块跑一次
    // 最终 GlobalDCE（finalDCEMPM_）。
}
```

> **注**：`buildFunctionSimplificationPipeline` 包含 SROA、InstCombine、SCCP、GVN、CorrelatedValuePropagation、BDCE、DSE、loop-rotate、LICM、loop-unroll 等，但**不含向量化**——向量化由 Phase 5 在最终 StructFieldPass 之后按档位追加（L1 跳过，对齐 `clang -O1`）。Phase 5 的 SLP 在 L2+ 无条件运行（宿主侧由 cl::opt 门控，嵌入式运行时无此概念）——以 JIT 编译时延换代码质量，属有意取舍，实测编译时延应顺带评估。UnrollAndJam 则**不启用**：宿主把它挂在 `-enable-unroll-and-jam`（cl::opt，默认 OFF）后面，L2/L3 都跑它会使 JIT 比对应的 AOT O2/O3 更激进，代码体积也不利嵌入式缓存。Phase 6 的最终 GlobalDCE 有必要性：expect 守卫（`__builtin_expect`）要到 Phase 2（LowerExpect）折叠后才能删掉守卫死半边的调用，而 Phase 1g 的 DCE 在此之前已运行——Phase 2-5 新死掉的 callee 只有靠这最后一扫才不会被 JIT 后端编译。首次 Inline 由 AOT 预优化完成；JIT 侧不再做二次 Inline。

---

## 2.5 EJitCompileDriver — 编译调度器

`EJitCompileDriver` 是 taskpool compile callback 与 ORC 之间的统一冷路径：

1. `compileNow(req)` 校验 `numDims <= 4`、instance 范围和重复 dimType；
2. 按函数 metadata 的 dimType 顺序重排请求维度并构造 64-bit cache key；
3. `compileCold(cacheKey, false)` 按 dense `funcIndex` 查函数名和 bitcode；
4. 检查 lifecycle active 状态，构造 `SpecializationContext`；
5. 在同一个 `jitEngine_` 上执行 load + lookup，返回 fnPtr。

cache hit、dedup、enqueue 和 publish 均由 taskpool 完成，compile driver
不再拥有独立的同步/异步 compiler 或 LRU cache。

<details>
<summary>历史设计：driver 内置 Sync/Async compiler（已删除）</summary>

```cpp
// 编译调度器
class EJitCompileDriver {
public:
    EJitCompileDriver(EJitConfig& config,
                      EJitCache& cache,
                      PeriodArrayRegistry& periodReg);

    // 统一入口: 获取或编译特化函数
    // 返回 NULL 表示需要 fallback
    void* getOrCompile(uint32_t funcIdx,
                       uint64_t cacheKey,
                       int count);

private:
    EJitConfig& config_;
    EJitCache& cache_;
    PeriodArrayRegistry& periodReg_;

    // 编译引擎 (同步/异步共用或独立)
    std::unique_ptr<EJitSyncCompiler> syncCompiler_;
    std::unique_ptr<EJitAsyncCompiler> asyncCompiler_;

    // Bitcode 数据缓存 (funcName → bitcode bytes)
    // AOT 嵌入的 bitcode 在 ejit_init 时加载到此缓存
    std::unordered_map<std::string, std::string> bitcodeCache_;

    // 构建 Cache Key (v1.8: uint64_t)
    // funcIdx(32b) | dim[0](8b) | dim[1](8b) | dim[2](8b) | dim[3](8b)
    uint64_t buildCacheKey(uint32_t funcIdx, uint64_t cacheKey);

    // 构建 SpecializationContext
    SpecializationContext buildContext(const std::string& funcName,
                                       uint64_t cacheKey);
};
```

```cpp
void* EJitCompileDriver::getOrCompile(uint32_t funcIdx,
                                       uint64_t cacheKey,
                                       int count) {
    // Step 1: 构建 Cache key (funcIdx from wrapper, 无字符串开销)
    uint32_t funcIdx = hashFuncName(funcName)  // deterministic, zero map lookup;
    uint64_t cacheKey = EJitCache::buildCacheKey(funcIdx, dims, count);

    // Step 2: 查 Cache
    if (void* cached = cache_.getOrNull(cacheKey)) {
        return cached;  // 命中 → 直接返回
    }

    // Step 3: 验证时间窗状态
    SpecializationContext ctx = buildContext(funcName, dims, count);
    if (!isPeriodActive(ctx)) {
        return nullptr; // 未激活 → fallback
    }

    // Step 4: 获取 Bitcode
    auto it = bitcodeCache_.find(funcName);
    if (it == bitcodeCache_.end()) {
        return nullptr; // 无 bitcode → fallback
    }

    // Step 5: 编译 (同步/异步)
    if (config_.compileMode == CompileMode::Sync) {
        auto result = syncCompiler_->compile(
            *syncEngine_, it->second, ctx, cacheKey);
        if (result.funcPtr) {
            cache_.put(cacheKey, result.funcPtr, result.codeSize);
        }
        return result.funcPtr;  // 同步: 立即返回结果
    } else {
        // 异步: 提交编译请求 → 立即返回 NULL
        CompileRequest req;
        req.funcName = funcName;
        req.ctx.cacheKey = cacheKey;
        req.bitcodeData = it->second;
        req.ctx = std::move(ctx);
        asyncCompiler_->submitRequest(std::move(req));
        return nullptr;  // 异步: 下次调用再查 Cache
    }
}
```

</details>

---

## 2.6 Taskpool Code Cache

当前 cache 由 `EJitTaskPool` / `EJitSharedTaskPool` 持有：

- identity 为 `funcIndex + dims`，并保存对应 lifecycle versions；
- shared cache 使用固定 bucket/slot POD 布局和显式原子/短临界区；
- publish 先写 identity、versions、code range 和 fnPtr，再发布 Ready；
- reader 返回的 bucket token 由 `ejit_taskpool_release_read` 释放；
- `EJIT_SRE_TASKPOOL_NO_RECLAIM` 在“代码生命周期内不物理回收”的前提下
  改用 seqlock load-only hit，release 成为 no-op；
- deactivate/version bump 后旧 slot 不会作为有效命中返回；编译中的旧结果
  也会在 publish gate 被丢弃。

<details>
<summary>历史设计：独立 LRU EJitCache（已删除）</summary>

```cpp
// Code Cache 管理器 — iterator-embedded LRU (单 hash 查找完成 LRU bump)
class EJitCache {
public:
    using LruList = std::list<uint64_t>;

    struct Entry {
        void* funcPtr;
        size_t codeSize;
        LruList::iterator lruIt;          // embedded → O(1) splice/erase
        SmallVector<std::string, 4> periodDeps;
    };

    EJitCache(size_t maxEntries = 4096,
              size_t maxTotalSize = 32 * 1024 * 1024,
              size_t maxSingleFuncSize = 512 * 1024);

    // 查询: unique_lock (splice 修改链表指针，不可 shared)
    void* getOrNull(uint64_t cacheKey);

    // 存入
    bool put(uint64_t cacheKey, void* funcPtr, size_t codeSize,
             ArrayRef<std::string> periodDeps = {});

    // 时间窗失效 → 清理依赖缓存 (periodIndex_ 索引)
    void invalidateByPeriod(const std::string& periodName, uint8_t cellIdx);

    void clear();
    Stats getStats() const;

    static uint64_t buildCacheKey(uint32_t funcIdx,
        const std::pair<std::string, uint8_t>* dims, unsigned count);

private:
    void evictLRU();

    mutable MutexType mutex_;               // BareMetalMutex or shared_mutex
    std::unordered_map<uint64_t, Entry> cache_;
    LruList lruList_;                       // uint64_t key, LRU order
    std::unordered_map<std::string, std::set<uint64_t>> periodIndex_;

    size_t maxEntries_;
    size_t maxTotalSize_;
    size_t maxSingleFuncSize_;
    size_t currentTotalSize_ = 0;
};
```

</details>

---

## 2.7 运行时初始化

### 2.7.1 EJitRegistrationStore — 全局注册暂存区

`EJitRegistrationStore` 是一个进程级全局单例，解决 `ejit_auto_register` (constructor 时执行) 与 `ejit_init` (用户 main 中调用) 之间的数据传递问题。

**问题**: `ejit_auto_register()` 通过 `@llvm.global_ctors` (优先级 65535) 在 `main()` 之前执行，调用 `ejit_register_period_array()`、`ejit_register_bitcode()` 等注册函数。但此时 `ejit_init()` 尚未调用，正式的 `PeriodArrayRegistry`、`BitcodeTracker` 等数据结构还不存在。若注册函数直接操作这些未创建的对象，会导致空指针或数据丢失。

**方案**: 注册函数统一写入全局单例 `EJitRegistrationStore`；`ejit_init()` 启动时从单例 `consume()` 取出所有暂存数据，填充正式 Registry，然后清空单例。

```cpp
// llvm/lib/ExecutionEngine/EJIT/EJitRegistrationStore.h

namespace llvm::ejit {

// 全局注册数据暂存区 (进程级单例)
// 生命周期:
//   constructor 阶段: ejit_auto_register() → 注册函数 → 写入 Store
//   main() 阶段:      ejit_init() → consume() → 填充正式 Registry → Store 清空
class EJitRegistrationStore {
public:
    static EJitRegistrationStore& instance() {
        static EJitRegistrationStore store;
        return store;
    }

    // --- 写入接口 (constructor 阶段, 单线程) ---

    void registerBitcode(const std::string& funcName,
                         const uint8_t* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        bitcodes_.push_back({funcName, data, size});
    }

    void registerPeriodArray(const std::string& periodName,
                             const std::string& varName,
                             void* baseAddr, uint64_t arraySize) {
        std::lock_guard<std::mutex> lock(mutex_);
        periodArrays_.push_back({periodName, varName, baseAddr, arraySize});
    }

    void registerStaticVar(const std::string& varName,
                           void* varAddr) {
        std::lock_guard<std::mutex> lock(mutex_);
        staticVars_.push_back({varName, varAddr});
    }

    // --- 消费接口 (ejit_init 调用) ---

    struct StoredData {
        std::vector<BitcodeEntry> bitcodes;
        std::vector<PeriodArrayEntry> periodArrays;
        std::vector<StaticVarEntry> staticVars;
    };

    // 取出所有暂存数据并清空内部容器
    // 仅在 ejit_init 中调用一次
    StoredData consume() {
        std::lock_guard<std::mutex> lock(mutex_);
        StoredData data;
        data.bitcodes = std::move(bitcodes_);
        data.periodArrays = std::move(periodArrays_);
        data.staticVars = std::move(staticVars_);
        return data;
    }

private:
    EJitRegistrationStore() = default;

    struct BitcodeEntry {
        std::string funcName;
        const uint8_t* data;    // 生命周期: 指向 .ejit.bitcode ELF section，由 OS loader 加载，
                                // 进程生命周期内有效，不可释放。若未来支持动态卸载共享库，
                                // 需在 dlclose 前清空对应 entries 并确保无进行中的 JIT 编译。
        size_t size;
    };
    struct PeriodArrayEntry {
        std::string periodName;
        std::string varName;
        void* baseAddr;
        uint64_t arraySize;
    };
    struct StaticVarEntry {
        std::string varName;
        void* varAddr;
    };

    std::mutex mutex_;  // 防御性: constructor 阶段单线程, 保证接口安全
    std::vector<BitcodeEntry> bitcodes_;
    std::vector<PeriodArrayEntry> periodArrays_;
    std::vector<StaticVarEntry> staticVars_;
};

} // namespace llvm::ejit
```

### 2.7.2 ejit_init

当前 `ejit_init` 创建一个 `EJit` 实例。初始化顺序为：

1. 消费 constructor 注册数据，或在静态注册模式下遍历 linker ranges；
2. 按名称分配 dense `funcIndex` 和 lifecycle `dimType`，回填 wrapper globals；
3. 注册 bitcode、period arrays、static vars 和用户符号；
4. 创建一个 `EJitOrcEngine` 并安装到 `EJitCompileDriver::jitEngine_`；
5. 冻结 registration；
6. Async 模式启动 private worker，或执行 shared owner election 并由 owner
   启动唯一 worker；Sync/Off 不启动后台 worker。

任何注册容量、ABI/fingerprint、engine 创建或 worker 启动失败都会使
`ejit_init` 返回错误，不暴露半初始化 taskpool。

<details>
<summary>历史设计：双 engine 与 EJitAsyncCompiler 初始化（已删除）</summary>

**全局单例类型定义**：

```cpp
// 进程级 EJIT 单例，由 ejit_init 创建，ejit_shutdown 释放
struct EJitInstance {
    std::unique_ptr<PeriodArrayRegistry> periodReg;
    std::unique_ptr<BitcodeTracker> bitcodeTracker;
    std::unique_ptr<EJitRuntimeState> runtimeState;
    std::unique_ptr<EJitCache> cache;
    std::unique_ptr<EJitOrcEngine> syncEngine;
    std::unique_ptr<EJitOrcEngine> asyncEngine;
    std::unique_ptr<EJitAsyncCompiler> asyncCompiler;
    std::unique_ptr<EJitCompileDriver> driver;
    EJitConfig config;
};

static EJitInstance gEJitInstance;
```

```cpp
ejit_status_t ejit_init(const ejit_config_t* config) {
    // Step 1: 解析配置
    EJitConfig cfg = config ? parseConfig(config) : EJitConfig::defaults();

    // Step 2: 从全局暂存区消费 constructor 阶段的注册数据
    auto storedData = EJitRegistrationStore::instance().consume();

    // Step 3: 创建核心组件并使用暂存数据填充
    //   3a. PeriodArrayRegistry
    auto periodReg = std::make_unique<PeriodArrayRegistry>();
    for (auto& entry : storedData.periodArrays) {
        periodReg->registerArray(entry.periodName, entry.varName,
                                  entry.baseAddr, entry.arraySize);
    }
    for (auto& entry : storedData.staticVars) {
        periodReg->registerStatic(entry.varName, entry.varAddr);
    }

    //   3b. Bitcode 缓存 (funcName → bitcode 映射)
    auto bitcodeTracker = std::make_unique<BitcodeTracker>();
    for (auto& entry : storedData.bitcodes) {
        bitcodeTracker->registerBitcode(entry.funcName,
                                         entry.data, entry.size);
    }

    //   3c. RuntimeState (activate/deactivate 状态管理)
    auto runtimeState = std::make_unique<EJitRuntimeState>();

    //   3d. Code Cache
    auto cache = std::make_unique<EJitCache>(
        cfg.maxCacheSize, cfg.maxCacheEntries, cfg.maxSingleFunctionSize);

    // Step 4: 创建 OrcJIT 引擎 (同步模式)
    auto syncEngineOrErr = EJitOrcEngine::Create(cfg);
    if (!syncEngineOrErr) {
        logError("EJit: failed to create sync engine");
        return EJIT_ERR_COMPILE_FAILED;
    }
    auto syncEngine = std::move(*syncEngineOrErr);
    syncEngine->setPeriodRegistry(periodReg.get());
    syncEngine->setRuntimeState(runtimeState.get());

    // Step 5: 创建异步引擎 (如果配置为异步)
    std::unique_ptr<EJitOrcEngine> asyncEngine;
    std::unique_ptr<EJitAsyncCompiler> asyncCompiler;
    if (cfg.compileMode == CompileMode::Async) {
        auto asyncEngineOrErr = EJitOrcEngine::Create(cfg);
        if (!asyncEngineOrErr) {
            logError("EJit: failed to create async engine");
            return EJIT_ERR_COMPILE_FAILED;
        }
        asyncEngine = std::move(*asyncEngineOrErr);
        asyncEngine->setPeriodRegistry(periodReg.get());
        asyncEngine->setRuntimeState(runtimeState.get());

        asyncCompiler = std::make_unique<EJitAsyncCompiler>(
            cfg, *cache, *runtimeState);
        asyncCompiler->setEngine(asyncEngine.get());
    }

    // Step 6: 创建编译调度器
    auto driver = std::make_unique<EJitCompileDriver>(
        cfg, *cache, *periodReg, *bitcodeTracker);
    driver->setSyncEngine(syncEngine.get());
    if (asyncCompiler) {
        driver->setAsyncCompiler(asyncCompiler.get());
    }

    // Step 7: 验证注册数据完整性
    if (storedData.periodArrays.empty() && storedData.staticVars.empty()) {
        // 警告: 无 EmbeddedJIT 数据 — 可能是无 ejit 代码的普通程序
        // 不视为错误，后续 ejit_taskpool_compile_or_get 全部走 fallback
        logWarning("EJit: no registration data consumed from store");
    }

    // Step 8: 启动异步编译器 (如果配置)
    if (asyncCompiler) {
        asyncCompiler->start();
    }

    // Step 9: 存储全局单例
    gEJitInstance = EJitInstance{
        std::move(periodReg),
        std::move(bitcodeTracker),
        std::move(runtimeState),
        std::move(cache),
        std::move(syncEngine),
        std::move(asyncEngine),
        std::move(asyncCompiler),
        std::move(driver),
        cfg
    };

    return EJIT_OK;
}
```

</details>

### 2.7.3 Auto Register 与 ejit_init 的时序

```
程序加载
    │
    ├── 操作系统加载 .ejit.bitcode section → 内存
    │
    ├── llvm.global_ctors 执行 (优先级 65535)
    │   └── ejit_auto_register():
    │       ├── ejit_register_bitcode("funcName", ptr, size)
    │       │   → 写入 EJitRegistrationStore::instance().bitcodes_
    │       ├── ejit_register_period_array("cell", ...)
    │       │   → 写入 EJitRegistrationStore::instance().periodArrays_
    │       ├── ejit_register_static_var(...)
    │       │   → 写入 EJitRegistrationStore::instance().staticVars_
    │       └── ...
    │
    ├── main()
    │   └── ejit_init(NULL)  (用户调用)
    │       ├── EJitRegistrationStore::instance().consume()
    │       │   → 取出所有暂存数据，清空 Store
    │       ├── 用暂存数据填充 PeriodArrayRegistry
    │       ├── 用暂存数据填充 BitcodeTracker
    │       ├── 创建 CompileDriver 和单条 ORC 编译链
    │       ├── 冻结 registration
    │       ├── 验证注册数据完整性
    │       └── Async: 启动 private worker 或完成 shared owner election
    │
    └── 业务代码...
        └── ejit_activate("cell", 3)
        └── process_task(3)  ← 首次调用触发 JIT
```

---

## 3. 线程安全模型

当前目标平台并发模型不是 C++ 线程模型：

- shared state 只包含 fixed-width POD、`EJitAtomic`、queue cells、dedup、
  switch/version、cache slots 和 counters；
- LLVMContext、LLJIT、optimizer、STL 容器、callback 与平台 task 对象均为
  owner 私有，不放入 shared section；
- queue producer 写完整 request 后 release-publish sequence，consumer
  acquire 后读取；
- cache publish 在发布 Ready/fnPtr 前写完 identity、versions 和 code range；
- 编译、ORC/JITLink、内存申请、平台 seal 和日志不在 bucket 短临界区内执行；
- activate/deactivate bump version，worker 在 dequeue、compile 后和 publish
  gate 复查版本；不匹配则释放/丢弃结果；
- `aarch64_be` 下字段按声明类型访问，不做字节流解析，不使用 bitfield。

host 测试适配层可以使用 `std::thread` 模拟 worker，但 target core 文件不依赖
`std::thread`、`std::mutex`、`std::condition_variable`、future 或 promise。

<details>
<summary>历史设计：mutex/C++ 后台线程模型（已删除）</summary>

### 3.1 线程角色

```
┌──────────────────────┐     ┌──────────────────────┐     ┌──────────────────────┐
│   用户线程 (Main)      │     │   后台编译线程          │     │   可能的多用户线程      │
│   调用:                │     │   执行:                │     │   (未来扩展)          │
│   - ejit_activate     │     │   - loadBitcode       │     │                      │
│   - ejit_deactivate   │     │   - IRTransformPass   │     │                      │
│   - process_task()    │     │   - optimize          │     │                      │
│     → Wrapper         │     │   - codeGen           │     │                      │
│       → getOrCompile  │     │   - cache.put         │     │                      │
│                       │     │                       │     │                      │
│   RuntimeState        │     │   独立的 LLVMContext   │     │                      │
│   (R/W, mutex保护)     │     │   独立的 LLJIT         │     │                      │
│                       │     │   共享 MemoryMgr       │     │                      │
│   Cache (查询)         │     │   (mutex 保护)         │     │                      │
│                       │     │                       │     │                      │
│   仅在主线程:           │     │   Cache (写入)         │     │                      │
│   - activate/deactiv. │     │                       │     │                      │
└──────────────────────┘     └──────────────────────┘     └──────────────────────┘
```

### 3.2 锁策略

| 数据结构 | 锁类型 | 读端 | 写端 |
|---------|--------|------|------|
| EJitCache | shared_mutex (or BareMetalMutex) | getOrNull (独占 — splice write) | put/evict (独占) |
| RuntimeState | mutex | 任意 | 仅主线程 (activate/deactivate) |
| PeriodArrayRegistry | 无锁 (只读) | 任意 (init 后只读) | 仅 ejit_auto_register (初始化时) |
| BitcodeCache | 无锁 (只读) | 任意 | 仅 ejit_auto_register (初始化时) |
| JITLinkMemoryManager | mutex (per slab) | 代码生成 | 代码生成 (sync + async) |
| Async Request Queue | mutex + cv | - | submitRequest + workerLoop |

### 3.3 LLVM 内部线程安全性

```cpp
// ThreadSafeModule 的正确使用
// 用户线程 (同步模式):
//   getOrCompile → syncCompiler.compile
//     → TSM.withModuleDo([](Module &M) { ... })
//     → LLVMContext 被 withModuleDo 的锁保护

// 后台线程 (异步模式):
//   workerLoop → compileOne
//     → workerEngine→compile  (独立的 LLJIT 实例)
//     → 独立的 LLVMContext, 无竞态
```

### 3.4 异步编译的内存序保证

异步模式下，用户线程和编译线程共享全局变量数据（`ejit_may_const` 字段值）。必须确保以下 happens-before 关系成立：

```
用户线程                          编译线程
───────                          ───────
ejit_activate(name, idx)
  ├─ 写入 RuntimeState
  │  (设置为 active)
  ├─ atomic_thread_fence
  │  (memory_order_release)    ─────────→  compileOne()
  │                                         ├─ isPeriodActive() → active
  │                                         ├─ atomic_thread_fence
  │                                         │  (memory_order_acquire)
  │                                         └─ 读取 may_const 字段值
  │                                            (可见 activate 前的写入)

ejit_deactivate(name, idx)
  ├─ 读取 RuntimeState
  ├─ 设置为 inactive ───────────→  compileOne()
  │  (若 deactivate 先于            ├─ isPeriodActive() → INACTIVE
  │   compileOne 的检查)             └─ 跳过编译, 不读字段值
  └─ ...
```

**实现要点：**

```cpp
// RuntimeState::activate 内部
void EJitRuntimeState::activate(const std::string& periodName, int cellIdx) {
    std::lock_guard<std::mutex> lock(mutex_);
    // ... 更新激活状态 ...
    // Release 屏障：确保所有用户线程对全局变量的写入
    // 在 activate 返回后对编译线程可见
    std::atomic_thread_fence(std::memory_order_release);
}

// RuntimeState::deactivate 内部
void EJitRuntimeState::deactivate(const std::string& periodName, int cellIdx) {
    std::lock_guard<std::mutex> lock(mutex_);
    // ... 更新激活状态 ...
}
```

**关键规则**：
- `ejit_activate` 必须在**写入** may_const 全局变量之后调用（或 activate 本身不代表写入，写入由业务代码在 activate 前完成）。若写入与 activate 并不同步，业务代码应在 activate 前自行加 barrier。
- `compileOne` 在读取 may_const 字段值前执行 `acquire` fence，与 activate 中的 `release` fence 配对，保证字段修改可见。
- `isPeriodActive` 检查本身在 mutex 保护下，确保 deactivate 后提交的编译请求能看到失效状态。
- 若 `isPeriodActive` 返回 false，`compileOne` 直接跳过，不读取任何字段值——因此不存在 "读到半修改值" 的窗口。

</details>

---

## 4. 嵌入式优化

### 4.1 内存预算

```cpp
// 嵌入式默认配置
struct EmbeddedDefaults {
    // JIT 代码内存 (JITLink slab)
    static constexpr size_t kCodeSlabSize = 2 * 1024 * 1024;  // 2MB
    static constexpr size_t kDataSlabSize = 128 * 1024;       // 128KB

    // Code Cache
    static constexpr size_t kMaxCacheSize = 32 * 1024 * 1024; // 32MB
    static constexpr size_t kMaxCacheEntries = 4096;
    static constexpr size_t kMaxSingleFuncSize = 512 * 1024;  // 512KB

    // LLVM 内部
    static constexpr size_t kLLVMStackSize = 256 * 1024;  // 256KB (编译线程栈)
};
```

### 4.2 按需加载 Bitcode

```cpp
// BitcodeTracker: 维护 funcName → bitcode 位置映射
// 不预加载所有 bitcode, 而是在首次 JIT 编译时按需解析
class BitcodeTracker {
    struct Entry {
        std::string funcName;
        const uint8_t* bitcodeData;   // 指向 .ejit.bitcode section (进程生命周期有效，见 §2.7.1 BitcodeEntry)
        size_t bitcodeSize;
    };

    std::unordered_map<std::string, Entry> entries_;

    // 按需加载: 首次调用时 parseModule
    // parse 开销 ~50KB 临时内存 (在 LLVMContext 中)
    std::unordered_map<std::string, std::string> parsedModules_;

public:
    void registerBitcode(const std::string& funcName,
                         const uint8_t* data, size_t size);

    Expected<StringRef> getBitcode(const std::string& funcName);
};
```

### 4.3 静态链接优化

```cpp
// 嵌入式场景: libejit.a 静态链接
// 可以移除不需要的功能:
//
//   - 调试符号 (DWARF) → strip
//   - TargetParser (仅 ARM/AArch64) → 编译时选择
//   - Disassembler (MCDisassembler) → 移除
//   - Remarks → 移除
//   - 不需要的 Target (仅 ARM/AArch64) → 链接时 GC

// CMake 配置:
//   -DLLVM_TARGETS_TO_BUILD="AArch64;ARM"
//   -DLLVM_ENABLE_ZSTD=OFF
//   -DLLVM_ENABLE_ZLIB=OFF
//   -DLLVM_ENABLE_TERMINFO=OFF
//   -DLLVM_ENABLE_THREADS=ON          (异步编译需要)
```

---

## 5. 错误处理与 Fallback

### 5.1 错误传播路径

```
JIT 编译失败
    ↓
EJitOrcEngine::compileFunction → return Error
    ↓
EJitCompileDriver::getOrCompile → return nullptr
    ↓
ejit_taskpool_compile_or_get → return NULL
    ↓
Wrapper → 跳转到 jit_fallback → 执行 AOT 代码
```

### 5.2 错误日志

```cpp
// 结构化错误日志
struct EJitError {
    ErrorCode code;
    std::string message;
    std::string funcName;
    std::string cacheKey;
    uint64_t timestamp;
    size_t attemptedMemUsage;
};

// 日志记录 (环形缓冲区, 避免 malloc 在错误路径失败)
class EJitLogger {
    static constexpr size_t kMaxErrors = 32;
    EJitError errors_[kMaxErrors];
    size_t writeIdx_ = 0;
    std::mutex mutex_;

public:
    void log(ErrorCode code, const std::string& msg,
             const std::string& func, const std::string& key);
    const EJitError* getLastError() const;
};
```

---

## 6. 完整 JIT 编译时序

### 6.1 同步编译时序

```
时间 →
用户调用: ─────────────────────────────────────────────────────────
           adjust_param(idx)
           │
           ├─ wrapper:
           │    ├─构建 dims
           │    ├─ejit_taskpool_compile_or_get()
           │    │   ├─查 Cache → MISS
           │    │   ├─验证时间窗状态
           │    │   ├─compileNow() → compileCold()
           │    │   │   ├─loadBitcodeModule
           │    │   │   ├─[IRTransformLayer]:
           │    │   │   │   ├─参数替换 + InstCombine
           │    │   │   │   ├─EJitStructFieldPass
           │    │   │   │   ├─IPSCCP
           │    │   │   │   └─函数级优化 Pipeline
           │    │   │   ├─IRCompileLayer:
           │    │   │   │   └─LLVM → Object
           │    │   │   ├─ObjectLinkingLayer:
           │    │   │   │   └─JITLink 链接+code pool 分配
           │    │   │   └─lookup → 函数指针
           │    │   ├─taskpool publish
           │    │   └─return pfn
           │    ├─pfn != NULL
           │    └─调用 pfn(idx) → 特化函数
           │
           └─编译耗时与目标函数大小、优化级别和平台有关
```

### 6.2 异步编译时序

```
时间 →

业务核:                  单 taskpool worker:
──────                  ─────────────────
adjust_param(idx)
│
├─ wrapper:
│    ├─构建 dims
│    ├─ejit_taskpool_compile_or_get()
│    │   ├─查 Cache → MISS
│    │   ├─提交 CompileRequest
│    │   └─return fallback ──→   runWorkerLoop():
│    │                              ├─从队列取请求
│    ├─pfn == NULL                  ├─runCompile()
│    └─fallback(AOT)                │   ├─loadBitcode
│                                   │   ├─IRTransform
│    ←返回 (运行 AOT 代码)            │   ├─优化
│                                   │   ├─CodeGen
│                                   │   ├─MemoryAlloc
│                                   │   └─publish (version/generation gate)
│                                   │
下次调用:                             │
adjust_param(idx)                   │
├─ wrapper:                         │
│    ├─ejit_taskpool_compile_or_get()       │
│    ├─查 Cache → HIT! ☆          │
│    ├─return pfn ────────────────  │
│    └─调用 pfn → 特化函数
│
└─命中开销由 cache 查询、间接调用和 read-token release 构成
```

---

## 7. 文件与组件清单

```
llvm/lib/ExecutionEngine/EJIT/
├── EJit.cpp                     # 主类实现 (EJit)
├── EJitRuntime.cpp              # C 运行时接口 (ejit_init/shutdown/activate...)
├── EJitOrcEngine.cpp            # LLJIT 封装 + 引擎创建
├── EJitCodePoolMemoryManager.cpp # code-pool JITLink 内存管理器
├── EJitTaskPool.cpp             # 单实例 taskpool
├── EJitSharedTaskPool.cpp       # 跨核共享 taskpool/cache/worker
├── EJitSreTask_sre.cpp          # 目标平台 task 适配
├── EJitSreQueue.cpp             # taskpool queue 适配
├── EJitCompileDriver.cpp        # 编译调度器 (Sync/Async 统一入口)
├── EJitCodePool.cpp             # code pool 分配与 2M/4K seal
├── EJitStructFieldPass.cpp      # 结构体字段特化 Pass (JIT Pipeline)
├── EJitOptimizer.cpp            # 优化 Pipeline (L1/L2/L3)
├── EJitModuleLoader.cpp         # Bitcode 按需加载器
├── EJitLogger.cpp               # 错误日志 (环形缓冲区)
├── EJitRegistration.cpp         # AOT 注册回调实现 (
│                                  ejit_register_period_array,
│                                  ejit_register_bitcode)
└── CMakeLists.txt

llvm/include/llvm/ExecutionEngine/EJIT/
├── EJit.h                       # C++ 主 API
├── EJitRuntime.h                # C 运行时接口
├── EJitOrcEngine.h              # EJitOrcEngine 声明
├── EJitCodePoolMemoryManager.h  # 内存管理器声明
├── EJitTaskPool.h               # 单实例 taskpool
├── EJitSharedTaskPool.h         # shared taskpool API
├── EJitSharedTaskPoolState.h    # shared POD 状态布局
├── EJitStructFieldPass.h        # StructField Pass 声明
├── EJitOptimizer.h              # 优化 Pipeline 声明
├── EJitOptions.h                # 配置选项
├── EJitError.h                  # 错误类型定义
└── EJitRegistration.h           # 注册 API 声明
```

---

## 8. 测试策略

### 8.1 单元测试

```cpp
// EJitJITLinkMemoryManagerTest.cpp
// TEST(MemoryManager, BasicAllocation)     - 基本分配+释放
// TEST(MemoryManager, OOM_ReturnsError)    - 超出限制
// TEST(MemoryManager, ConcurrentAlloc)     - 并发分配
// TEST(MemoryManager, MixedProtections)    - RX + RW 混合

// EJitTaskPoolTest.cpp
// - queue / dedup / sync / async / activate-version / freeCode / stats

// EJitSharedTaskPoolTest.cpp
// - owner election / MPSC / cross-core dedup / cache publication
// - generation / peer execute permission / big-endian layout / NO_RECLAIM

// EJitCodePoolMemoryManagerTest.cpp, EJitCodePoolTest.cpp
// - JITLink layout / executable ranges / 2M and 4K sealing / failures

// EJitLinkOptimizationPluginTest.cpp
// - relaxAArch64BranchStubs: forward/backward/in-range/boundary/out-of-range/
//   misaligned/addend/GOT-addend/big-endian/other-arch
// - setAddress 流程: RelaxesSetAddressResolvedExternal (in-range 外部符号经
//   applyLookupResult 的 setAddress 解析后必须放松)、KeepsUnresolvedExternalStubbed
//   (TargetAddr==0 不放松) -- 锁定 "门控用地址不用 isExternal()" 修复
```

### 8.2 集成测试

```c
// test_ejit_integration.c
// 场景 1: 端到端同步编译
//   定义 struct + ejit_may_const + ejit_period_arr
//   定义 ejit_entry 函数
//   AOT 编译 + bitcode 嵌入
//   运行时: init → activate → 调用 → 验证特化结果与预期一致

// 场景 2: 端到端异步编译
//   同上，配置 EJIT_COMPILE_ASYNC
//   首次调用返回 AOT 结果 → worker poll/等待发布 → 后续调用返回 JIT 结果

// 场景 3: 时间窗失效
//   JIT 编译 → activate → deactivate → activate(new_value)
//   验证特化函数使用新值重新编译

// 场景 4: Cache 与并发协议
//   验证 queue-full dedup rollback、publish gate、generation/version 失效

// 场景 5: 多时间窗
//   ejit_entry 依赖 cell+trp
//   不同 (cellIdx, trpIdx) 组合生成不同的特化版本
```

---

## 9. 实施注意事项

1. **ExecutionSession 生命周期**: OrcJIT 的 `ExecutionSession` 析构时报告所有未释放的资源（`ResourceTracker`）。确保通过 `clear()` 或 `ResourceTracker::remove()` 正确释放。

2. **LLVMContext 隔离**: 异步编译器的 LLVMContext 必须独立于调用线程的 LLVMContext。`ThreadSafeModule` 的创建使用独立 `LLVMContext` 工厂。

3. **JITLink slab 碎片**: 当前 bump-allocator 设计不支持单独释放。LRU 淘汰后碎片可能无法重用。后续可考虑：
   - 分段 slab（每个函数独立段）
   - 压缩 (移动活跃代码, 修正重定位)
   - 或者接受碎片，限制函数数量

4. **ARM/AArch64 内存保护**: 代码段需要 `PROT_READ | PROT_EXEC`，数据段需要 `PROT_READ | PROT_WRITE`。使用 `mprotect` 或等效系统调用设置。AArch64 同样适用此保护模型。

5. **`CloneToNewContextOnEmit`**: 异步模式下必须调用 `IRLayer::setCloneToNewContextOnEmit(true)` 确保多线程安全。

6. **全局单例**: `ejit_init` 创建的实例是进程级单例。`ejit_shutdown` 释放所有资源。不支持多次 init/shutdown 循环（简化设计）。

7. **Signal-safe 内存操作**: 在某些嵌入式系统上，`mmap`/`munmap` 不可用。提供预分配 slab + 手动内存保护设置的回退。

---

*文档版本: 1.1*
*创建日期: 2026-04-26*
*更新日期: 2026-04-29*

---

## 双路径注册机制 (v1.2)

### 注册路径

| 路径 | 触发方式 | 适用 |
|---|---|---|
| 构造器 | `llvm.global_ctors` → CRT 调用 `ejit_auto_register()` | hosted |
| 静态注册表 | `ejit_init()` 遍历 `__ejit_registry_bitcode[]` + `__ejit_registry_period[]` | 裸核 / `forceStaticRegistry=true` |
| 手动 API | `ejit_register_*()` 在 `ejit_init()` 之后调用 | 运行时动态 |

### 运行时选择

```cpp
StoredData data = EJitRegistrationStore::instance().consume();
if (config_.forceStaticRegistry || data.empty()) {
    walkTable(__ejit_registry_bitcode);   // PASS1
    walkTable(__ejit_registry_period);    // PASS2
} else {
    // 构造器路径(现有)
}
```

### C API dual-path

`ejit_register_bitcode` / `ejit_register_period_array` / `ejit_register_static_var` /
`ejit_register_symbol` 均支持双路径：`gEJIT` 非空 → 直接转发引擎，否则暂存 `EJitRegistrationStore`。

### 裸核运行时修复

- `Builder.setLinkProcessSymbolsByDefault(false)` — 避免 dlopen/dlsym
- `EJIT_DEFAULT_TARGET_TRIPLE` 强制要求 — 代替 detectHost()
- `setNumCompileThreads(0)` — 单线程
- `forceStaticRegistry` 配置项 — 强制走静态注册表路径

### ejit_config_t 新增字段

```c
typedef struct {
    ...
    bool forceStaticRegistry;  // true = 强制静态表路径
} ejit_config_t;
```

### 测试

`ejit_test/ejit_manual_register_test.c` — x86 上 PASS，验证 forceStaticRegistry + JIT 编译执行。

---

*文档版本: 2.0*
*更新日期: 2026-06-01*
