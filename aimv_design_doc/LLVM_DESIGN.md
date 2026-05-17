# AIMV -- LLVM 侧详细设计方案

**版本**: 2.1
**日期**: 2026-05-17
**关联文档**: SPEC.md v1.4, PLAN.md v1.1

---

## 0. 设计目标

在 LLVM Pass Pipeline 中侵入式集成 AIMV 诊断信息收集能力：

1. 修改 LoopVectorize / SLPVectorizer：在向量化**被拒绝时**和**成功时**都将结构化诊断数据写入 Module 级 Named Metadata（成功信息的目的是让 driver/CI 能正向确认向量化是否生效，而非仅靠"无失败记录"间接判断）
2. 新增 `AIMVFeedbackPass`（Function Pass）：读取诊断 Metadata，补充 IR 上下文和代价拆解，序列化为 JSON
3. 注册到 PassBuilder Pipeline，无需 plugin 方式加载
4. 通过 clang Driver `-faimv` 原生入口激活（SPEC §3.1），编译后自动 fork+exec `aimv-driver`

**MVP 代价模型数据策略**: MVP 阶段仅通过 `LoopVectorizationCostModel::expectedCost()` 获取**总标量/向量代价**和 VF。逐指令代价 (`instruction_costs`) 需要修改 LoopVectorize 内部状态暴露接口，实现成本高，延后到 Phase 2。MVP 诊断足够让 LLM 判断"向量化是否划算"——scalar_cost vs vector_cost + VF 已经包含了关键决策信息。

---

## 1. 数据通道：`!aimv.diag` Named Metadata

### 1.1 设计思路

LoopVectorize/SLPVectorizer 被拒绝时，已有的 `ORE->emit(...)` 只能产出 human-readable 文本。我们新增一条平行通道——将**结构化**诊断数据写入 `!aimv.diag`，供下游 `AIMVFeedbackPass` 消费。

```
LoopVectorize::processLoop()
  |-- emit remark (现有，不改)
  +-- emitAIMVDiagnostic(...)   (新增，写入 !aimv.diag)
         |
         v
  NamedMDNode !aimv.diag  <- Module 级
         |
         v
  AIMVFeedbackPass::run()
    |-- 读取 !aimv.diag
    |-- 筛选当前函数的诊断 (FunctionName 过滤)
    |-- 补充 IR 上下文、AA/SCEV 分析结果
    +-- 序列化 JSON -> aimv.json
```

### 1.2 激活机制

**设计**: `emitAIMVDiagnostic()` **无条件写入** `!aimv.diag`，由 `AIMVFeedbackPass` 控制是否消费。

**设计决策理由**: CLAUDE.md 中的 LLVM 21 API 笔记记录了早期方案中检查 `getLLVMRemarkStreamer()` 的设计，但经评估后改为无条件写入。原因：(1) `getLLVMRemarkStreamer()` 在 opt/clang 不同管道路径中返回值不一致（某些配置下 remark 正常输出但 streamer 为空），不可靠；(2) metadata 写入的开销极小（几个 MDNode），即使 AIMV 未启用也不影响编译性能；(3) 将控制点从"写入侧"移到"消费侧"（AIMVFeedbackPass 的 EnabledFlag + OutputPath）更简单可靠。

| 组件 | 行为 |
|------|------|
| `emitAIMVDiagnostic()` | 无条件写入 `!aimv.diag`（不检查 streamer） |
| `AIMVFeedbackPass` | `EnabledFlag == true` 且 `OutputPath` 非空时读取并消费 |

**激活方式**：

```bash
# 推荐: clang Driver 统一激活
clang -O2 -faimv -c task.c -o task.o
# -faimv → -mllvm -aimv-enable → AIMVFeedbackPass 运行 → 读 !aimv.diag → 产 aimv.json

# opt 调试
opt -passes="loop-vectorize,aimv-feedback" \
    -aimv-enable -aimv-output=/tmp/aimv.json \
    -S input.ll -o /dev/null
```

### 1.3 Named Metadata 节点格式

每个被拒绝或成功向量化的循环在 `!aimv.diag` 中追加一个 MDNode：

```llvm
; 格式:
; !aimv.diag = !{!diag_0, !diag_1, ...}
;
; 每个 !diag_i 的 operand 列表:
;   [0]: MDString pass_name        ("LoopVectorize" | "SLPVectorize")
;   [1]: MDString remark_id        ("CantReorderMemOps" | "VectorizationNotBeneficial" | ...)
;   [2]: MDString function_name    (被分析函数名)
;   [3]: MDString source_location  ("file.c:42:5")
;   [4]: MDString diagnostic_msg   (完整 remark 文本)
;   [5]: MDNode  cost_data         (代价拆解，格式见下)
;   [6]: MDNode  dep_data          (依赖分析，格式见下)
;   [7]: MDNode  memory_data       (内存/对齐信息，格式见下)
;   [8]: MDNode  loop_info         (循环基本信息)
;
; 示例:
!0 = !{!0}
!aimv.diag = !{!1}
!1 = !{
  !"LoopVectorize",
  !"CantReorderMemOps",
  !"process_task",
  !"../src/task.c:42:5",
  !"loop not vectorized: cannot prove it is safe to reorder memory operations",
  !2,   ; cost_data
  !3,   ; dep_data
  !4,   ; memory_data
  !5    ; loop_info
}
```

### 1.4 子结构定义

#### cost_data (operand [5])

```llvm
; [0]: i32 scalar_cost  (-1=不可用，如 legality 阶段失败)
; [1]: i32 vector_cost  (-1=不可用)
; [2]: i32 vf           (0=未决定)
; [3]: i32 interleave_count
; 注: 代价数据来自 LoopVectorizationCostModel::expectedCost()，
;     其中 scalar_cost = expectedCost(ElementCount::getFixed(1))，
;     vector_cost = expectedCost(VF)。
;     在 legality 阶段拒绝时（CM 不可用），scalar_cost 和 vector_cost 均为 -1。
!2 = !{i32 24, i32 38, i32 4, i32 1}
```

#### dep_data (operand [6])

```llvm
; [0]: i32 dep_count
; [1...]: MDNode 每条依赖
;   每个依赖子节点:
;     [0]: MDString dep_type    (直接使用 LLVM Dependence::DepName[Dep.Type]):
;         LLVM 21 的 MemoryDepChecker::Dependence::DepType 共 8 种:
;         "NoDep"                              <- 无依赖（通常跳过不记录）
;         "Unknown"                            <- 无法确定方向/距离
;         "IndirectUnsafe"                     <- 间接访问（如 A[B[i]]）
;         "Forward"                            <- 词法前向依赖
;         "ForwardButPreventsForwarding"       <- 前向但阻止 store-to-load 转发
;         "Backward"                           <- 词法后向依赖（RAW，阻止向量化）
;         "BackwardVectorizable"               <- 后向但距离允许向量化
;         "BackwardVectorizableButPreventsForwarding" <- 同上但阻止转发
;
;     注: 使用 Dependence::DepName[Dep.Type] 直接获取，不做二次映射。
;         LLVM 21 的 Dependence struct 只有 3 个 bool 访问器:
;           isForward(), isBackward(), isPossiblyBackward()
;         无法区分全部 8 种类型。不存在以下方法:
;           isBackwardVectorizable(), isForwardButPreventsForwarding(),
;           isIndirectUnsafe(), isNoDep()
;         必须直接访问 Dep.Type 字段来获取精确类型。
;
;         isPossiblyBackward() 对应 "Unknown" 类型（SCEV 无法确定依赖方向）。
;         PossiblyBackward 不是 DepName 数组中的独立条目，而是对 Unknown 依赖
;         语义的描述性称呼。写入 metadata 时统一使用 DepName[Dep.Type] 原始值。
;
;     [1]: MDString source_ptr  (源指针描述)
;     [2]: MDString sink_ptr    (目标指针描述)
;     [3]: MDString alias_result (安全/不安全分类描述)
; 注: 源/目标 Description 通过 Inst->getName() 或 Inst->getOpcodeName() 获取。
;     过滤掉 NoDep（无实际意义），保留其余 7 种。
; alias_result 使用 Dependence::isSafeForVectorization() 分类:
;   Safe -> "safe for vectorization"
;   PossiblySafeWithRtChecks -> "possibly safe with runtime checks"
;   Unsafe -> "unsafe: prevents vectorization"
!3 = !{i32 2, !{!"Backward", !"ptr %a + i", !"ptr %b + i", !"unsafe: prevents vectorization"}, !{!"IndirectUnsafe", ..."}}
```

#### memory_data (operand [7])

```llvm
; [0]: i32 num_stores
; [1]: i32 num_loads
; [2]: i32 num_pred_stores
; [3]: i32 max_alignment       (0=unknown; 通过 getLoadStoreAlignment() / DL.getABITypeAlign 获取)
; [4]: MDString stride_info    ("stride=1" | "non-constant" | "unknown"; 从 LAI 的内存访问模式推断)
; [5]: i32 memory_check_count  (需插入的 runtime check 数量)
; [6]: i32 memory_check_cost   (runtime check 总代价)
!4 = !{i32 1, i32 4, i32 0, i32 8, !"stride=1", i32 2, i32 8}
```

#### loop_info (operand [8])

```llvm
; [0]: MDString loop_id_str   (来自 L.getName()，可能为空则用 "loop_<header_bb_name>")
; [1]: i32 num_blocks
; [2]: i32 num_instructions
; [3]: i32 trip_count          (-1=unknown; 通过 SE.getSmallConstantTripCount(&L) 获取)
; [4]: i32 num_branches
; [5]: i32 num_calls
; 注: SE.getSmallConstantTripCount() 属于 ScalarEvolution，emit 点需要 SE 参数。
;     Loop::getTripCount() 不存在。无 SE 可用时 trip_count = -1。
!5 = !{!"loop_42", i32 3, i32 18, i32 -1, i32 1, i32 0}
```

---

## 2. `emitAIMVDiagnostic()` 实现详解

### 2.1 位置与声明

**声明文件**: `llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h`（共享内部头文件）

**实现文件**: `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp`

该函数声明为非 static，供 LoopVectorize.cpp 和 LoopAccessAnalysis.cpp 共同引用。零跨组件符号依赖：函数仅依赖 LLVM Core/Analysis 的公共 API，不引用 LLVMAIMV 组件中的任何符号。

```cpp
// [AIMV] llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h

void emitAIMVDiagnostic(
    Module &M, Function &F, Loop &L,
    const LoopAccessInfo *LAI,
    LoopVectorizationCostModel *CM,
    ElementCount VF, unsigned IC,
    StringRef RemarkID, StringRef RemarkMsg,
    ScalarEvolution *SE = nullptr,
    int RtCheckCost = -1,
    int RtCheckCount = -1);
```

### 2.2 激活条件

`emitAIMVDiagnostic()` **无条件写入**（不检查 streamer）。`AIMVFeedbackPass` 通过 `EnabledFlag` + `OutputPath` 控制消费（见 §1.2 激活机制）。

### 2.3 函数实现

核心逻辑：

1. **source_location 构建**: `DebugLoc` -> `DILocation::getLine():getColumn()`，回退到 `DISubprogram`，最终回退到 "unknown"。使用 `raw_string_ostream` 避免 Twine 临时对象悬空引用。

2. **cost_data**: 来自 `CM->expectedCost()`。`InstructionCost::getValue()` 返回 `int64_t`，调用前需检查 `isValid()`。CM 为 nullptr 时所有字段填 -1/0。

3. **dep_data**: 来自 `LAI->getDepChecker().getDependences()`，返回 `const SmallVectorImpl<Dependence>*`，需判空。过滤掉 NoDep，直接使用 `DepName[Dep.Type]` 获取 LLVM 原生类型名（LLVM 21 的 Dependence struct 只有 3 个 bool 访问器 `isForward`/`isBackward`/`isPossiblyBackward`，无法区分全部 8 种 DepType）。安全分类使用 `isSafeForVectorization(Dep.Type)`。

4. **memory_data**: `num_stores`/`num_loads` 从 `LAI` 获取。`num_pred_stores` 统一填 0（LAI 不直接提供，精确值需 `LoopVectorizationLegality::blockNeedsPredication()`）。最大对齐通过遍历循环体中所有 Load/Store 指令获取。stride 推断：`RtCheckCount==0` 且有内存操作则标记 "stride=1"，否则 "non-constant"。

5. **loop_info**: 从 `Loop` 对象和 `ScalarEvolution` 获取。`trip_count` 使用 `SE->getSmallConstantTripCount(&L)`（`Loop::getTripCount()` 不存在）。

6. **组装与追加**: 构建 MDNode 并追加到 `!aimv.diag` Named Metadata。

### 2.4 插入点一览

#### 插入点 T1.1: `CantReorderMemOps` -- runtime check 代价过高拒绝（最重要）

**说明**: 虽然 remark 文本是 "cannot prove it is safe to reorder memory operations"，但触发条件实际来自 `isOutsideLoopWorkProfitable()` 返回 false（runtime checks 的代价太高）。真正的依赖分析失败（UnsafeDep）在 `LoopAccessAnalysis.cpp` 中（见插入点 T1.5）。

**位置**: `LoopVectorize.cpp`，在 `isOutsideLoopWorkProfitable()` 返回 false 的分支内，ORE emit 之后、`return false` 之前。

```cpp
// [AIMV] AIMV T1.1: emit structured diagnostic for CantReorderMemOps
{
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  int RtChkCount = (int)LVL.getLAI()->getNumRuntimePointerChecks();
  emitAIMVDiagnostic(
      *L->getHeader()->getParent()->getParent(),
      *L->getHeader()->getParent(), *L,
      LVL.getLAI(), &CM, VF.Width, IC,
      "CantReorderMemOps",
      "unsafe dependent memory operations in loop",
      PSE.getSE(), RtCost, RtChkCount);
}
```

**上下文**: CM 可用，SE 从 PSE 获取，`Checks.getCost()` 返回 `InstructionCost` 可能 Invalid 需判空，`RtCheckCount` 使用 `getNumRuntimePointerChecks()`（PtrRtChecking 始终已初始化）。

> **实施期注意**: `LVL.getLAI()` 的实际返回类型（指针 `const LoopAccessInfo*` vs 引用 vs optional）需在实施期确认 LLVM 21 的 `LoopVectorizationLegality` 定义。所有插入点中的 `LVL.getLAI()` 调用假设返回指针类型；若返回引用，需去掉指针取地址操作。

#### 插入点 T1.4: `VectorizationNotBeneficial` -- 代价模型拒绝

当 `VF.Width.isScalar()` 时，代价模型判定向量化不划算。此处同时发出 VectorizationNotBeneficial 和 InterleavingNotBeneficial 两个 remark。

```cpp
// [AIMV] AIMV T1.4: VectorizationNotBeneficial
{
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  int RtChkCount = (int)LVL.getLAI()->getNumRuntimePointerChecks();
  emitAIMVDiagnostic(
      *L->getHeader()->getParent()->getParent(),
      *L->getHeader()->getParent(), *L,
      LVL.getLAI(), &CM, VF.Width, IC,
      VecDiagMsg.first, VecDiagMsg.second,
      PSE.getSE(), RtCost, RtChkCount);
}
```

**注意**: `VecDiagMsg.first` 可能是 `"VectorizationNotBeneficial"` 或其他动态值（如 `"InterleavingNotBeneficialAndDisabled"`），AIMVFeedbackPass 的 `inferSeverity()` 会将其归类为 "missed"。

#### 插入点 T1.5: `UnsafeDep` -- 依赖分析失败（LoopAccessAnalysis）

**位置**: `llvm/lib/Analysis/LoopAccessAnalysis.cpp`，在 `emitUnsafeDependenceRemark()` 函数末尾。

真正的 UnsafeDep 发射点在 `LoopAccessAnalysis.cpp` 中，不在 `LoopVectorizationLegality.cpp`。`LoopVectorizationLegality.cpp` 仅转发 LAI->getReport() 中的已有 remark。

**关键上下文限制**: `emitUnsafeDependenceRemark()` 是 `LoopAccessInfo` 的私有成员函数，无参数。在此作用域中：
- **可用**: `TheLoop` (Loop*), `PSE` (PredicatedScalarEvolution, 含 `PSE->getSE()`), `PtrRtChecking` (RuntimePointerChecking), `getDepChecker()` (MemoryDepChecker)
- **可通过指针链获取**: `TheLoop->getHeader()->getParent()` -> Function*, `Function->getParent()` -> Module*
- **不可用**: VF (ElementCount), IC (unsigned), CM (CostModel), Checks (GeneratedRTChecks) -- 这些只在 LoopVectorize 的代价模型阶段才确定

```cpp
// [AIMV] AIMV T1.5: emit structured diagnostic for UnsafeDep
// This function runs during legality analysis; no CM/VF/IC available.
{
  Function *Fn = TheLoop->getHeader()->getParent();
  Module *Mod = Fn->getParent();
  int RtChkCount = (int)getNumRuntimePointerChecks();
  emitAIMVDiagnostic(*Mod, *Fn, *TheLoop,
                     this, /*CM=*/nullptr,
                     ElementCount::getFixed(0), /*IC=*/0,
                     "UnsafeDep", Info,
                     PSE->getSE(),
                     /*RtCheckCost=*/-1, RtChkCount);
}
```

**注意**: `Info` 是 `emitUnsafeDependenceRemark()` 中构建的本地变量，包含完整的安全依赖描述文本。`Report` 是 `unique_ptr<OptimizationRemarkAnalysis>`，其消息内容通过 `operator<<` 流式写入，无 `getRemarkMsg()` 方法。`PSE` 是 `std::unique_ptr<PredicatedScalarEvolution>`，通过 `PSE->getSE()` 获取 `ScalarEvolution` 引用。

#### 插入点 T1.6: `InterleavingNotBeneficial` -- 交错不利

当 IC==1 且未强制交错时，代价模型判定交错不划算。与 T1.4 处于同一代码块（两者同时发出）。

```cpp
// [AIMV] AIMV T1.6: InterleavingNotBeneficial
{
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  int RtChkCount = (int)LVL.getLAI()->getNumRuntimePointerChecks();
  emitAIMVDiagnostic(
      *L->getHeader()->getParent()->getParent(),
      *L->getHeader()->getParent(), *L,
      LVL.getLAI(), &CM, VF.Width, IC,
      IntDiagMsg.first, IntDiagMsg.second,
      PSE.getSE(), RtCost, RtChkCount);
}
```

#### 插入点 T1.7: `LoopVectorized` -- 向量化成功（正向记录）

**说明**: Driver 依靠 `!aimv.diag` 中的 passed 记录来**正向确认**向量化成功。仅靠"无 missed 记录"无法区分"向量化成功"与"Pass 未运行"。

位于 `processLoop()` 函数中，非 epilogue 向量化路径（InnerLoopVectorizer 分支），VPlan 已执行完毕、向量化决策已确定为 true 的位置。

```cpp
// [AIMV] AIMV T1.7: record successful vectorization
{
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  int RtChkCount = (int)LVL.getLAI()->getNumRuntimePointerChecks();
  std::string VFMsg =
      "loop vectorized: VF=" + std::to_string(VF.Width.getKnownMinValue());
  emitAIMVDiagnostic(
      *L->getHeader()->getParent()->getParent(),
      *L->getHeader()->getParent(), *L,
      LVL.getLAI(), &CM, VF.Width, IC,
      "LoopVectorized", VFMsg,
      PSE.getSE(), RtCost, RtChkCount);
}
```

**注意**: 不在 `reportVectorizationFailure` 路径或 early-exit 路径插入。Epilogue 向量化路径暂未添加 AIMV 诊断，可后续补充。

### 2.5 SLPVectorize 改动点（可选，Phase 2）

**文件**: `llvm/lib/Transforms/Vectorize/SLPVectorizer.cpp`

SLP 主要处理 BB 内向量化，失败模式比 LoopVectorize 简单。暂定在 `tryToVectorize()` 返回 false 时写入，格式复用 `!aimv.diag`，pass_name = `"SLPVectorize"`。

**初期可跳过**，MVP 聚焦 LoopVectorize。

---

## 3. AIMVFeedbackPass 设计

### 3.1 文件结构

```
llvm/lib/Transforms/AIMV/
|-- AIMVFeedbackPass.cpp          # Pass 主实现 (~170 行)
|-- AIMVDiagnosticParser.cpp      # !aimv.diag metadata 解析器 (~100 行)
+-- CMakeLists.txt                # 构建配置
```

```
llvm/include/llvm/Transforms/AIMV/
+-- AIMVFeedback.h                # 公共头文件 (~77 行)
```

```
llvm/lib/Transforms/Vectorize/
+-- AIMVDiagnostic.h              # emitAIMVDiagnostic() 共享内部头文件 (~64 行)
```

### 3.2 公共头文件

**文件**: `llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h`

```cpp
// [AIMV] llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h

class AIMVFeedbackPass : public PassInfoMixin<AIMVFeedbackPass> {
public:
  // 注意: 此 Pass 通过 FUNCTION_PASS("aimv-feedback", AIMVFeedbackPass())
  // 默认构造注册。成员变量不会被自动设置。
  // run() 内部直接从 cl::opt 全局变量读取配置，而非依赖这些 setter。
  // setter 保留用于单元测试场景（手动构造 + 配置 Pass）。
  void setOutputPath(const std::string &Path);
  void setTargetFunction(const std::string &FuncName);
  void setEnabled(bool V = true);

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static StringRef name() { return "aimv-feedback"; }

  /// [AIMV] 从 !aimv.diag 解析出的原始诊断记录
  struct RawDiagnostic {
    std::string PassName;        // "LoopVectorize" | "SLPVectorize"
    std::string RemarkID;        // "CantReorderMemOps" | ...
    std::string FunctionName;
    std::string SourceLocation;  // "file.c:42:5"
    std::string RemarkMsg;
    // 代价
    int ScalarCost = -1;
    int VectorCost = -1;
    int VF = 0;
    int IC = 0;
    // 依赖 — dep_type 直接使用 LLVM Dependence::DepName[Dep.Type]
    struct DepEntry {
      std::string Type;  // 8 种类型: "NoDep"|"Unknown"|"IndirectUnsafe"|"Forward"|
                         // "ForwardButPreventsForwarding"|"Backward"|
                         // "BackwardVectorizable"|"BackwardVectorizableButPreventsForwarding"
      std::string Source;
      std::string Sink;
      std::string AliasResult;
    };
    std::vector<DepEntry> Dependencies;
    // 内存
    int NumStores = 0, NumLoads = 0, NumPredStores = 0;
    int MaxAlignment = 0;
    std::string Stride = "unknown";
    int MemCheckCount = 0, MemCheckCost = 0;
    // 循环
    int NumBlocks = 0, NumInsts = 0, TripCount = -1;
    int NumBranches = 0, NumCalls = 0;
  };

  static std::vector<RawDiagnostic> parseDiagnostics(Module &M);

private:
  std::string OutputPath;
  std::string TargetFunction;
  bool EnabledFlag = false;  // 由 -aimv-enable 设置
};
```

### 3.3 Pass 实现要点

#### 命令行参数

```cpp
static cl::opt<std::string> AIMVOutputPath(
    "aimv-output", cl::desc("AIMV JSON diagnostic output path"), cl::Hidden);
static cl::opt<bool> AIMVEnable(
    "aimv-enable", cl::desc("Enable AIMV diagnostic collection"), cl::Hidden);
static cl::opt<std::string> AIMVTargetFunction(
    "aimv-target-function", cl::desc("Only analyze the specified function"),
    cl::Hidden);
```

所有参数标记为 `cl::Hidden`（不在 `-help` 中显示），通过 `-mllvm` 传递。

**重要**: `FUNCTION_PASS("aimv-feedback", AIMVFeedbackPass())` 注册的是默认构造的 Pass 实例，成员变量 `EnabledFlag`/`OutputPath`/`TargetFunction` 不会被自动设置。Pass 的 `run()` 方法必须从 `cl::opt` 全局变量直接读取，而非依赖成员变量：

```cpp
PreservedAnalyses AIMVFeedbackPass::run(Function &F, FunctionAnalysisManager &AM) {
  // 从 cl::opt 读取配置（非成员变量——默认构造不会设置它们）
  const bool Enabled = AIMVEnable;
  const std::string Output = AIMVOutputPath;
  const std::string Target = AIMVTargetFunction;

  if (!Enabled || Output.empty())
    return PreservedAnalyses::all();
  // ... 后续逻辑使用 Output 和 Target
}
```

#### 缓存优化（避免 O(N*M) 开销）

Function Pass 对每个函数独立运行。同一 Module 的 `!aimv.diag` 会被 N 个函数各解析一次。Pass 实例内缓存首次 `parseDiagnostics()` 结果：

```cpp
// [AIMV] 缓存 parseDiagnostics() 结果，避免多函数重复解析
// static 缓存对当前单线程 pass manager 安全。
// 注意: 若未来 LLVM 的 new PM 启用多线程 pass 执行，
//       需改用 ModuleAnalysisManager 的 AnalysisResult 缓存机制。
static Module *CachedModule = nullptr;
static std::unique_ptr<std::vector<RawDiagnostic>> CachedDiags;

PreservedAnalyses AIMVFeedbackPass::run(Function &F, FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M)
    return PreservedAnalyses::all();

  // 从 cl::opt 全局变量读取配置（默认构造的 Pass 实例不携带这些值）
  const std::string Output = AIMVOutputPath;
  const std::string Target = AIMVTargetFunction;
  const bool Enabled = AIMVEnable;

  // 激活条件: Enabled == true 且 Output 非空
  // 由 -mllvm -aimv-enable + -mllvm -aimv-output=<path> 设置
  if (!Enabled || Output.empty())
    return PreservedAnalyses::all();

  // 缓存: 仅在 Module 变更时重新解析
  if (CachedModule != M) {
    CachedDiags = std::make_unique<std::vector<RawDiagnostic>>(parseDiagnostics(*M));
    CachedModule = M;
  }

  // 筛选当前函数的诊断...
}
```

后续函数直接复用 `CachedDiags` 做 `FunctionName` 过滤，消除 O(N*M) 开销。

**多函数处理**: 同一 Module 的 `!aimv.diag` 包含所有函数的诊断。Pass 对每个函数运行时筛选 `FunctionName` 匹配的记录。所有函数的诊断写入同一个 JSON 文件（由 `function_name` 字段区分）。Driver 读取后按 `function_name` 索引各函数的诊断，按函数顺序迭代处理。

#### Source Code Versioning 说明

JSON 输出中的 `source_context` 来自 IR 的 `!dbg` 映射，反映的是**当前编译轮次**的 IR 级源码位置。对于多函数文件：
- LLVM Pass 每次编译时输出的是**当前 IR** 对应的源码上下文
- Driver 发送给 MCP 的 `source_code` 为**当前文件的完整内容**（可能已包含前序函数的变更，SPEC §3.1），由 Driver 侧管理
- LLVM Pass 不需要处理源码版本问题——它只输出 IR 层的源码映射

#### 源码反向映射回退链（消除 !dbg 漂移风险）

优化后的 IR 行号可能因 pass 重排而漂移。`extractSourceContext()` 按以下优先级回退：

```
1. !dbg metadata -> DILocation::getLine()      (最精确)
2. DILocation::getScope() -> DISubprogram       (函数级匹配)
3. 循环 header 的 DebugLoc                     (循环级粗略匹配)
4. 函数名字符串匹配 + "approximate" 标记       (最后手段)
```

回退到第 3/4 级时，诊断 JSON 中增加 `"source_accuracy": "approximate"` 标记，MCP prompt 中明确告知大模型"行号可能偏差 +/-5 行"。

#### severity 推断规则

`!aimv.diag` metadata 中未显式存储 severity，AIMVFeedbackPass 解析时按 RemarkID 推断：

```cpp
static std::string inferSeverity(const std::string &RemarkID) {
  if (RemarkID == "CantReorderMemOps" ||
      RemarkID == "VectorizationNotBeneficial" ||
      RemarkID == "UnsafeDep" ||
      RemarkID == "InterleavingNotBeneficial")
    return "missed";
  if (RemarkID == "LoopVectorized" ||
      RemarkID.find("Passed") != std::string::npos)
    return "passed";
  return "analysis";
}
```

**PossiblyBackward 上下文**: 当 `dep_data` 中存在 `dep_type == "Unknown"` 的依赖时（`isPossiblyBackward()` 返回 true 的类型），该循环的向量化失败与 SCEV 无法确定的依赖方向有关。severity 推断不受 dep_type 直接影响——severity 由 RemarkID 决定，dep_type 为 MCP 提供更精细的分析线索。

**注意**: `InterleavingNotBeneficialAndDisabled`、`InterleavingAvoided` 等 RemarkID 变体未显式列在 "missed" 列表中，会落入 "analysis"。实施期应补充完整匹配或使用前缀匹配。

#### Pass 运行逻辑

`AIMVFeedbackPass::run(Function &F, FunctionAnalysisManager &AM)` 核心流程：

```
0. Module *M = F.getParent()
   - 若 M 为空 -> 返回 PreservedAnalyses::all()

1. 激活条件检查:
   - if (OutputPath.empty() && !EnabledFlag) -> 返回
   - 这些由 -mllvm -aimv-enable + -mllvm -aimv-output=<path> 设置
   - clang Driver -faimv 自动转发这两个选项

2. 缓存优化: 复用 parseDiagnostics() 结果（见上文）

3. 筛选: 只保留 FunctionName == F.getName() 的诊断
   - 若设置了 TargetFunction，进一步过滤
   - 若筛选后为空 -> 返回 PreservedAnalyses::all()

4. 获取目标平台信息:
   - AM.getResult<TargetIRAnalysis>(F) -> TargetTransformInfo
   - Module: M->getTargetTriple()
   - vector_width: TTI.getRegisterBitWidth(RGK_FixedWidthVector)
   - CPU/Features: 从 Module 的 target-cpu/target-features attributes 获取

5. 对每条匹配的诊断:
   a. 在 LoopInfo 中按位置匹配对应的 Loop*
   b. 从 Loop* 的 DebugLoc 定位源码位置
   c. 调用 extractSourceContext() 获取源码片段
   d. 调用 extractIRSnippet() 获取 IR 片段

6. 构建函数级 JSON 文档 (与 MCP Server AnalyzeRequest 格式兼容)
   - target: {triple, cpu, features, vector_width}
   - diagnostics: [{pass_name, remark_id, severity, cost_model, dependencies,
                     memory_info, source_context, ir_snippet}, ...]

7. 追加写入 OutputPath 指定的 JSON 文件
   - 多函数共享同一 JSON 文件: 所有函数的诊断写入同一个 aimv.json
   - Driver 读取后按 function_name 索引各函数的诊断
   - 并发写入防护: 使用 std::mutex 保护文件写入（Function Pass 在同一 FPM 内串行运行，
     但为保险起见加锁）

8. 返回 PreservedAnalyses::all() (本 pass 只读)
```

### 3.4 JSON 输出格式

输出文件与 PLAN.md 中定义的 `AnalyzeRequest` 的 **diagnostics 部分**对应。完整 AnalyzeRequest 需要的 `function`（source_code/signature/loop_line）由 Driver 在发送 MCP 请求前从源文件和编译参数中补充。

**`target` 字段**: AIMVFeedbackPass **尽力填充** `target` 中的 `triple`/`cpu`/`features`/`vector_width`（从 Module attributes + TTI 获取，见 §3.3 步骤 4）。Driver 发送 MCP 请求前**不覆盖** target 字段——但 Driver 会用编译参数中的 `-mcpu=` / `-mfpu=` 补充空值（若 Pass 无法获取）。

**多函数输出**: 所有函数的诊断写入同一个 JSON 文件，由 `function_name` 字段区分。Driver 按 `function_name` 索引各函数的诊断，按函数顺序迭代处理。

```json
{
  "request_id": "aimv-<module_id>-<func_name_hash>",
  "target": {
    "triple": "armv7-unknown-linux-gnueabi",
    "cpu": "cortex-a9",
    "features": ["neon", "vfp3"],
    "vector_width": 128
  },
  "diagnostics": [
    {
      "pass_name": "LoopVectorize",
      "remark_id": "CantReorderMemOps",
      "remark_text": "unsafe dependent memory operations in loop",
      "severity": "missed",
      "function_name": "process_task",
      "loop_location": "../src/task.c:42:5",
      "source_context": "",
      "ir_snippet": "",
      "cost_model": {
        "scalar_cost": 24,
        "vector_cost": 38,
        "vf": 4,
        "interleave_count": 1
      },
      "dependencies": [
        {
          "dep_type": "Backward",
          "source_ptr": "ptr %b + i + 1",
          "sink_ptr": "ptr %a + i",
          "alias_result": "unsafe: prevents vectorization"
        }
      ],
      "memory_info": {
        "num_stores": 1, "num_loads": 2,
        "num_pred_stores": 0,
        "max_alignment": 4,
        "stride": "stride=1",
        "memory_check_count": 2,
        "memory_check_cost": 8
      },
      "loop_info": {
        "num_blocks": 3, "num_instructions": 18,
        "trip_count": -1, "num_branches": 1, "num_calls": 0
      }
    },
    {
      "pass_name": "LoopVectorize",
      "remark_id": "LoopVectorized",
      "remark_text": "loop vectorized: VF=4",
      "severity": "passed",
      "function_name": "init_buf",
      "loop_location": "../src/task.c:15:3",
      ...
    }
  ]
}
```

### 3.5 Metadata 解析器

**文件**: `llvm/lib/Transforms/AIMV/AIMVDiagnosticParser.cpp`

将 `!aimv.diag` Named Metadata 解析为 `RawDiagnostic` 向量。核心辅助函数：

- `mdStringOperand(N, I)`: 安全获取 MDNode 的第 I 个 MDString operand
- `int32Operand(N, I)`: 安全获取 MDNode 的第 I 个 i32 ConstantAsMetadata operand

解析逻辑按 1.3 节定义的 operand 布局逐字段读取，所有字段都有默认值和容错处理（`std::optional` + 丢弃 malformed entries）。

---

## 4. clang Driver `-faimv` 集成（主入口）

### 4.1 概述

`-faimv` 是用户唯一需要的新 flag（SPEC §3.1）。clang Driver 原生解析后：
1. 编译阶段转发 `-mllvm` flags 激活 AIMVFeedbackPass
2. 编译完成后 `fork+exec aimv-driver` 完成闭环

改动总量约 20 行 C++。

### 4.2 Options.td 注册

**文件**: `clang/include/clang/Driver/Options.td`

```td
// [AIMV]
def faimv : Flag<["-"], "faimv">, Group<f_Group>,
  HelpText<"Enable AIMV AI-driven vectorization analysis">;
def fno_aimv : Flag<["-"], "fno-aimv">, Group<f_Group>;
```

### 4.3 Clang.cpp Driver 逻辑

**文件**: `clang/lib/Driver/ToolChains/Clang.cpp`

在 `Clang::ConstructJob()` 中，编译参数构建阶段插入：

```cpp
// [AIMV] -faimv: enable AIMV feedback pass
if (Args.hasFlag(options::OPT_faimv, options::OPT_fno_aimv, false)) {
  // 1. 转发 LLVM 后端选项，激活 AIMVFeedbackPass
  CmdArgs.push_back("-mllvm");
  CmdArgs.push_back("-aimv-enable");
  CmdArgs.push_back("-mllvm");
  CmdArgs.push_back("-aimv-output=" + Output.getFilename() + ".aimv.json");

  // 2. 启用 optimization remarks（用户可见的诊断输出）
  //    注: emitAIMVDiagnostic() 已改为无条件写入 !aimv.diag，
  //       不再依赖 remark streamer。此 flag 仅为用户提供可读的 remark 输出。
  if (!Args.hasArg(options::OPT_fsave_optimization_record))
    CmdArgs.push_back("-fsave-optimization-record=json");
}
```

### 4.4 fork+exec aimv-driver

编译完成（`CC1Command` 执行完毕）后，若 `-faimv` 生效，执行 `aimv-driver`：

> **实施期注意**: 以下代码为理想化逻辑。fork+exec 的精确插入点需在实施期根据 clang Driver 的实际架构确定——可能位于 `Driver::ExecuteCompilation()`、`CC1Command::Execute()` 的后置钩子、或通过自定义 `Command` 子类实现。此处展示的是功能语义，不是最终插入位置。

```cpp
// [AIMV] 编译完成后 fork+exec aimv-driver
// 此段逻辑在 clang driver 的 executeJobs / ExecuteCommand 流程中，
// 仅当 -faimv 生效且 aimv.json 产出后才执行。
if (AIMVEnabled) {
  std::string AimvJsonPath = Output.getFilename().str() + ".aimv.json";

  // 检查 aimv.json 是否存在（编译可能无失败循环，不产出文件）
  if (llvm::sys::fs::exists(AimvJsonPath)) {
    // findProgramByName 返回 ErrorOr<std::string>，需 .get() 获取完整路径
    auto Found = llvm::sys::findProgramByName("aimv-driver");
    if (Found) {
      // 注意: 所有字符串参数必须存为 std::string，不能让 StringRef
      // 指向临时对象（字符串拼接结果是临时量，StringRef 会悬空）
      std::string FullPath = Found.get();
      std::string FromJsonArg = "--from-json=" + AimvJsonPath;
      std::string SourceArg = "--source=" + Input.getFilename().str();

      std::vector<StringRef> Args = {
        FullPath,
        FromJsonArg,
        SourceArg,
      };

      // fork + exec + waitpid
      int ExitCode = llvm::sys::ExecuteAndWait(FullPath, Args);

      if (ExitCode != 0) {
        // aimv-driver 失败: 回滚由 driver 内部 source_manager 处理
        // 将 driver 退出码传播给 clang
        // 注: stderr 已由 driver 输出错误信息
        return ExitCode;
      }
      // exit 0: 源码已修改（或无失败无需修改）
    } else {
      // aimv-driver 未安装: 打印警告，退化到普通编译
      llvm::errs() << "[AIMV] warning: aimv-driver not found in PATH, "
                   << "falling back to normal compilation\n";
    }
  }
}
```

### 4.5 防无限 fork 设计（Anti-fork-chain）

**关键设计**（SPEC §3.1）: `-faimv` 只存在于 clang Driver 层（C++）。`aimv-driver` 内部重编译时使用 LLVM 后端 flag，不传 `-faimv`：

```
Round 1 (用户触发):
  clang -O2 -faimv -c task.c              <- 用户命令，含 -faimv
    |-- Driver 编译, 启动 AIMVFeedbackPass
    +-- Driver fork aimv-driver --from-json=aimv.json

Round 1-N (aimv-driver 内部，BuildOrchestrator 调用 clang):
  clang -O2 -mllvm -aimv-enable -mllvm -aimv-output=<path> -c task.c
    |-- 不含 -faimv
    |-- 不会触发 clang Driver 再次 fork
    +-- 只运行 LLVM 层的 AIMVFeedbackPass 收集诊断
```

因此不会产生 fork 链。

### 4.6 fork+exec 异常处理

| 场景 | clang Driver 行为 |
|------|------------------|
| `aimv-driver` 未安装 / 找不到 | 打印警告到 stderr，退化到普通编译（exit code 不变） |
| `aimv-driver` 崩溃（非零退出 / signal） | 回滚源码（aimv-driver 内部 source_manager 维护备份），打印错误到 stderr，exit code = aimv-driver 退出码 |
| `aimv-driver` 超时（默认 120s，可配置） | SIGKILL 终止，回滚源码（同上），打印超时错误 |
| 用户 Ctrl+C（SIGINT） | 传递给 aimv-driver，由其回滚源码后退出；clang Driver waitpid 后退出 |
| 用户 kill（SIGTERM） | 同 SIGINT |

### 4.7 BackendUtil.cpp 选项处理

**文件**: `clang/lib/CodeGen/BackendUtil.cpp`

处理 `-aimv-output` 和 `-aimv-enable` 的 cl::opt 定义和传递：

```cpp
// [AIMV] LLVM 后端 cl::opt 定义
static cl::opt<std::string> AIMVOutputPath("aimv-output",
    cl::desc("AIMV JSON output path"),
    cl::Hidden);
static cl::opt<bool> AIMVEnable("aimv-enable",
    cl::desc("Enable AIMV feedback pass"),
    cl::Hidden);
```

---

## 5. Pass 注册与 Pipeline 集成

### 5.1 PassRegistry 注册

**文件**: `llvm/lib/Passes/PassRegistry.def`

```cpp
// [AIMV] AIMV vectorization feedback analysis
FUNCTION_PASS("aimv-feedback", AIMVFeedbackPass())
```

### 5.2 Pipeline 插入

**文件**: `llvm/lib/Passes/PassBuilderPipelines.cpp`

在 `addVectorPasses()` 中，LoopVectorize + SLPVectorize 之后插入：

```cpp
// [AIMV] AIMV: collect vectorization diagnostics after LoopVectorize+SLPVectorize
// The pass internally checks for AIMV enabled + output path before running.
FPM.addPass(AIMVFeedbackPass());
```

Pass 内部通过 `EnabledFlag`/`OutputPath` 检查决定是否实际执行，因此无条件注册到 pipeline 不会影响未启用 AIMV 的编译性能。

### 5.3 与现有 Remark 基础设施的关系

```
clang -O2 -faimv -fsave-optimization-record=opt.yaml -aimv-output=aimv.json src.c

       +----------------------------------------+
       |         Pass Pipeline                  |
       |                                        |
       |  LoopVectorizePass                     |
       |    |-- ORE->emit(remark)  --> YAML     | <- 现有路径
       |    +-- emitAIMVDiagnostic() --> !aimv.diag | <- 新增路径
       |                                        |
       |  SLPVectorizerPass                     |
       |    +-- (同上)                           |
       |                                        |
       |  AIMVFeedbackPass                      |
       |    |-- 读取 !aimv.diag                 |
       |    |-- 补充上下文                       |
       |    +-- 输出 aimv.json                  | <- 新增输出
       |                                        |
       |  YAMLRemarkSerializer --> opt.yaml     | <- 现有路径
       +----------------------------------------+
```

两条路径**平行独立**，互不干扰。

---

## 6. 构建集成

### 6.1 CMakeLists.txt

**文件**: `llvm/lib/Transforms/AIMV/CMakeLists.txt`

```cmake
add_llvm_component_library(LLVMAIMV
  AIMVFeedbackPass.cpp
  AIMVDiagnosticParser.cpp

  LINK_COMPONENTS
  Core
  Analysis
  Support
  TransformUtils
  Vectorize
  TargetParser
)
```

注: 仅使用 `LINK_COMPONENTS`，不使用 `LINK_LIBS`（避免重复链接，遵循 CLAUDE.md 约定）。

### 6.2 顶层 CMakeLists.txt 改动

**文件**: `llvm/lib/Transforms/CMakeLists.txt`

```cmake
# [AIMV] AIMV feedback pass
add_subdirectory(AIMV)
```

### 6.3 Pass 插件注册

**文件**: `llvm/lib/Passes/CMakeLists.txt`

将 `AIMV` 添加到 `LINK_COMPONENTS`，确保 PassRegistry.def 中的 `FUNCTION_PASS("aimv-feedback", ...)` 可解析。

### 6.4 头文件包含

**文件**: `llvm/lib/Passes/PassBuilderPipelines.cpp`

```cpp
#include "llvm/Transforms/AIMV/AIMVFeedback.h"
```

---

## 7. 命令行参数

### 7.1 用户可见选项

| 选项 | 说明 |
|------|------|
| `-faimv` | 启用 AIMV AI 驱动向量化分析（自动设置 remark streamer + AIMV Pass + fork driver） |
| `-fno-aimv` | 禁用（默认） |

### 7.2 LLVM 后端选项（`-mllvm` 传递，通常不直接使用）

| 选项 | 说明 |
|------|------|
| `-aimv-output=<path>` | AIMV JSON 输出路径 |
| `-aimv-enable` | 激活 AIMVFeedbackPass |
| `-aimv-target-function=<name>` | 只分析指定函数 |
| `-aimv-dry-run` | 产出 aimv.json 但不调用 driver |

### 7.3 使用示例

```bash
# 用户视角：只需 -faimv
clang -O2 -faimv -c src/task.c -o task.o

# opt 调试：手动传 LLVM 后端
opt -passes="loop-vectorize,aimv-feedback" \
    -aimv-output=aimv.json -aimv-enable \
    -S src.ll -o /dev/null

# 仅收集诊断（不调用 driver）
clang -O2 -faimv -mllvm -aimv-dry-run -c src/task.c -o task.o
```

---

## 8. 改动汇总

| 文件 | 变更类型 | 改动量 | 说明 |
|------|---------|--------|------|
| `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` | 修改 | +190 行 | emitAIMVDiagnostic() 实现 + 5 个插入点调用 |
| `llvm/lib/Analysis/LoopAccessAnalysis.cpp` | 修改 | +15 行 | UnsafeDep 拒绝点增加 AIMV 写入 |
| `llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h` | **新建** | ~64 行 | 共享内部头文件，emitAIMVDiagnostic() 声明 |
| `llvm/lib/Transforms/AIMV/AIMVFeedbackPass.cpp` | **新建** | ~170 行 | Pass 主实现 |
| `llvm/lib/Transforms/AIMV/AIMVDiagnosticParser.cpp` | **新建** | ~100 行 | Metadata -> RawDiagnostic 解析 |
| `llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h` | **新建** | ~77 行 | 公共头文件 |
| `llvm/lib/Transforms/AIMV/CMakeLists.txt` | **新建** | ~13 行 | 构建配置 |
| `llvm/lib/Transforms/CMakeLists.txt` | 修改 | +1 行 | 添加子目录 |
| `llvm/lib/Passes/PassBuilderPipelines.cpp` | 修改 | +5 行 | 头文件包含 + 在 addVectorPasses 中注册 |
| `llvm/lib/Passes/PassRegistry.def` | 修改 | +2 行 | FUNCTION_PASS 注册 |
| `llvm/lib/Passes/CMakeLists.txt` | 修改 | +1 行 | 链接 LLVMAIMV |
| `clang/lib/CodeGen/BackendUtil.cpp` | 修改 | +15 行 | `-aimv-output` / `-aimv-enable` cl::opt 处理 |
| `clang/lib/Driver/ToolChains/Clang.cpp` | 修改 | +20 行 | `-faimv` 解析 + LLVM flags 转发 + fork+exec |
| `clang/include/clang/Driver/Options.td` | 修改 | +3 行 | 注册 `-faimv` / `-fno-aimv` |

**总新增文件**: 5 个（~424 行）
**总修改文件**: 9 个（~252 行）

---

## 9. 多函数文件处理

### 9.1 `!aimv.diag` 的语义

每个循环独立记录——被拒绝的循环写 missed，成功的循环写 passed。`AIMVFeedbackPass` 输出的 JSON 中可能同时包含同一函数的 missed 和 passed 诊断。

### 9.2 Driver 的多函数迭代策略

**按函数顺序处理**（SPEC §3.1）：
- `aimv.json` 按**函数**粒度组织诊断，每个函数独立记录
- `aimv-driver` 按**函数**粒度迭代：对文件内所有失败函数**顺序处理**，每次只修改一个函数
- 影子文件协议**每函数独立应用**：函数 A 验证通过后立即 `mv` 到原文件，不等待 B 完成。B 编译时看到的源码已包含 A 的变更，且 B 失败时不需要回滚 A
- 轮次计数**每函数独立**：函数 A 成功向量化不影响函数 B 继续迭代
- 终止条件: 目标循环从 missed 变为 passed

### 9.3 LLVM 侧处理

- `AIMVFeedbackPass` 输出所有函数的诊断到同一个 JSON 文件
- `parseDiagnostics()` 解析整个 `!aimv.diag`，缓存结果
- 每个函数的 `run()` 调用筛选 `FunctionName == F.getName()` 的记录
- LLVM Pass 不关心 Driver 侧的影子文件协议和原子替换

---

## 10. 默认配置

SPEC §3.1 定义的默认配置：
- `aimv_level=conservative`（保守模式。全自动场景下保守修改是安全基线，用户可通过配置提升）
- `max_rounds=5`
- `mcp_url=http://localhost:8080`
- `test_cmd=""`（空则跳过测试，仅做编译验证）

这些配置由 Driver 侧管理，LLVM Pass 不涉及。

---

## 11. 测试策略

### 11.1 LLVM Lit 测试

```
llvm/test/Transforms/AIMV/
|-- aimv_diag_metadata.ll     # 验证 !aimv.diag 的正确生成
|-- aimv_feedback_json.ll     # 验证 JSON 输出格式
|-- aimv_dep_fail.ll          # 依赖分析失败场景
|-- aimv_cost_reject.ll       # 代价模型拒绝场景
+-- lit.local.cfg
```

### 11.2 测试 Pattern

```llvm
; RUN: opt -passes="loop-vectorize,aimv-feedback" -pass-remarks-output=%t.yaml -pass-remarks-missed=loop-vectorize -aimv-output=%t.json -aimv-enable -S < %s
; RUN: FileCheck %s -check-prefix=JSON < %t.json

; JSON: "pass_name": "LoopVectorize"
; JSON: "remark_id": "CantReorderMemOps"

define void @test_dep_fail(ptr %a, ptr %b, i32 %n) {
entry:
  br label %loop
loop:
  %i = phi i32 [ 0, %entry ], [ %i.next, %loop ]
  %gep_a = getelementptr i32, ptr %a, i32 %i
  %gep_b = getelementptr i32, ptr %b, i32 %i
  store i32 0, ptr %gep_a
  %v = load i32, ptr %gep_b
  %i.next = add i32 %i, 1
  %cmp = icmp slt i32 %i.next, %n
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
```

### 11.3 clang Driver 集成测试

```
clang/test/Driver/
+-- aimv.c    # 验证 -faimv 正确传递 -mllvm flags
```

```c
// RUN: %clang -### -O2 -faimv -c %s -o %t.o 2>&1 | FileCheck %s
// CHECK: "-aimv-enable"
// CHECK: "-aimv-output=
// CHECK-NOT: "-faimv"
```

---

## 12. 待完善项

以下设计点已在当前实现中留有占位或简化处理，需在后续迭代中完善：

> **优先级说明**: §12.1 和 §12.2 的 `source_context` / `ir_snippet` 直接影响 MCP Server 的 AI 分析质量——空字段意味着 LLM 缺少关键上下文信息。建议在 MVP 阶段就实现基础版本的源码映射和 IR 提取，即使行号精度不够（可标记 `"source_accuracy": "approximate"`）。§12.3-§12.5 可延后到 Phase 2。

### 12.1 源码反向映射（source_context）**[高优先级]**

当前 `source_context` 字段输出为空字符串。空字段意味着 MCP 的 AI 分析缺少最关键的源码上下文，直接影响建议质量。完整实现需要：
- `!dbg` metadata -> `DILocation::getLine()` (最精确)
- `DILocation::getScope()` -> `DISubprogram` (函数级匹配)
- 循环 header 的 DebugLoc (循环级粗略匹配)
- 函数名字符串匹配 + "approximate" 标记 (最后手段)

回退到第 3/4 级时，诊断 JSON 中增加 `"source_accuracy": "approximate"` 标记。

### 12.2 IR 片段提取（ir_snippet）**[高优先级]**

当前 `ir_snippet` 字段输出为空字符串。与 source_context 相同，空 ir_snippet 严重削弱 AI 分析能力。实现需要：
- 在 LoopInfo 中按位置匹配对应的 `Loop*`
- 从 `Loop*` 的 blocks 提取 IR 指令文本
- 截取适当长度（建议 500-1000 字符，避免 token 浪费）

### 12.3 CPU/Features 信息

当前 `target.cpu` 和 `target.features` 为空。实现需要：
- 从 Module 的 `target-cpu` / `target-features` function attributes 获取
- 或从 command-line `-mcpu=` / `-aimv-target-cpu=` 注入

### 12.4 Epilogue 向量化路径

当前仅在非 epilogue 向量化路径（InnerLoopVectorizer 分支）添加了 T1.7（成功诊断）。Epilogue 向量化路径暂未添加，可后续补充。

### 12.5 severity 推断完善

`InterleavingNotBeneficialAndDisabled`、`InterleavingAvoided` 等 RemarkID 变体未显式列在 "missed" 列表中，当前会落入 "analysis"。应补充完整匹配列表或使用前缀匹配。

---

*文档版本: 2.1*
*创建日期: 2026-04-29*
*最后更新: 2026-05-17*
