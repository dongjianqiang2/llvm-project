# AIMV — LLVM 侧详细设计方案

**版本**: 1.0
**日期**: 2026-04-29
**关联文档**: SPEC.md, PLAN.md

---

## 0. 设计目标

在 LLVM Pass Pipeline 中侵入式集成 AIMV 诊断信息收集能力：

1. 修改 LoopVectorize / SLPVectorizer：在向量化**被拒绝时**和**成功时**都将结构化诊断数据写入 Module 级 Named Metadata（成功信息的目的是让 driver/CI 能正向确认向量化是否生效，而非仅靠"无失败记录"间接判断）
2. 新增 `AIMVFeedbackPass`（Function Pass）：读取诊断 Metadata，补充 IR 上下文和代价拆解，序列化为 JSON
3. 注册到 PassBuilder Pipeline，无需 plugin 方式加载

**MVP 代价模型数据策略**: MVP 阶段仅通过 `LoopVectorizationCostModel::expectedCost()` 获取**总标量/向量代价**和 VF。逐指令代价 (`instruction_costs`) 需要修改 LoopVectorize 内部状态暴露接口，实现成本高，延后到 Phase 2。MVP 诊断足够让 LLM 判断"向量化是否划算"——scalar_cost vs vector_cost + VF 已经包含了关键决策信息。

---

## 1. 数据通道：`!aimv.diag` Named Metadata

### 1.1 设计思路

LoopVectorize/SLPVectorizer 被拒绝时，已有的 `ORE->emit(...)` 只能产出 human-readable 文本。我们新增一条平行通道——将**结构化**诊断数据写入 `!aimv.diag`，供下游 `AIMVFeedbackPass` 消费。

```
LoopVectorize::processLoop()
  ├── emit remark (现有，不改)
  └── emitAIMVDiagnostic(...)   (新增，写入 !aimv.diag)
         │
         ▼
  NamedMDNode !aimv.diag  ← Module 级
         │
         ▼
  AIMVFeedbackPass::run()
    ├── 读取 !aimv.diag
    ├── 补充 IR 上下文、AA/SCEV 分析结果
    └── 序列化 JSON → aimv-output.json
```

### 1.2 Named Metadata 节点格式

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
  !"loop not vectorized: cannot prove that the loop's memory operations are independent",
  !2,   ; cost_data
  !3,   ; dep_data
  !4,   ; memory_data
  !5    ; loop_info
}
```

### 1.3 子结构定义

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
;         "NoDep"                              ← 无依赖（通常跳过不记录）
;         "Unknown"                            ← 无法确定方向/距离
;         "IndirectUnsafe"                     ← 间接访问（如 A[B[i]]）
;         "Forward"                            ← 词法前向依赖
;         "ForwardButPreventsForwarding"       ← 前向但阻止 store-to-load 转发
;         "Backward"                           ← 词法后向依赖（RAW，阻止向量化）
;         "BackwardVectorizable"               ← 后向但距离允许向量化
;         "BackwardVectorizableButPreventsForwarding" ← 同上但阻止转发
;     注: 使用 Dependence::DepName[Dep.Type] 直接获取，不做二次映射。
;         LLVM 21 的 Dependence struct 只有 3 个 bool 访问器（isForward/isBackward/
;         isPossiblyBackward），无法区分全部 8 种类型。必须直接访问 Dep.Type 字段。
;     [1]: MDString source_ptr  (源指针描述)
;     [2]: MDString sink_ptr    (目标指针描述)
;     [3]: MDString alias_result (安全/不安全分类描述)
; 注: 源/目标 Description 通过 Inst->getName() 或 Inst->getOpcodeName() 获取。
;     过滤掉 NoDep（无实际意义），保留其余 7 种。
; alias_result 使用 Dependence::isSafeForVectorization() 分类:
;   Safe → "safe for vectorization"
;   PossiblySafeWithRtChecks → "possibly safe with runtime checks"
;   Unsafe → "unsafe: prevents vectorization"
!3 = !{i32 2, !{!"Backward", !"ptr %a + i", !"ptr %b + i", !"unsafe: prevents vectorization"}, !{!"IndirectUnsafe", ...}}
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
; 注: SE.getSmallConstantTripCount() 属于 ScalarEvolution，emit 点需要 SC 参数。
;     无 SE 可用的 insertion point（如 Unreachable 路径或 pass 无 SCEV），trip_count = -1。
!5 = !{!"loop_42", i32 3, i32 18, i32 -1, i32 1, i32 0}
```

---

## 2. LoopVectorize 改动点

### 2.1 修改范围

**文件**: `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp`
**改动量**: ~120 行（新增 1 个辅助函数 + 在 4 个拒绝点 + 1 个成功点插入调用）

### 2.2 新增辅助函数

```cpp
// [BiSheng] 位置: llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h（新建共享内部头文件）
//
// 声明在该头文件中（非 static），供 LoopVectorize.cpp 和 LoopAccessAnalysis.cpp 共同引用。
// 实现在 LoopVectorize.cpp 中（单一定义）。
//
// 零跨组件符号依赖:
//   - 函数仅依赖 LLVM Core/Analysis 的公共 API（Module, Function, Loop, LoopAccessInfo 等）
//   - 不引用 LLVMAIMV 组件中的任何符号
//   - AIMVFeedbackPass 通过 #include 此头文件调用 parseDiagnostics()，不调用 emitAIMVDiagnostic()

/// [BiSheng] 当 LoopVectorize 拒绝向量化或成功向量化时，将结构化诊断写入 !aimv.diag
///
/// 与 ORE remark 平行输出，供下游 AIMVFeedbackPass 消费。
/// 仅在 LLVMRemarkStreamer 激活或 aimv 诊断被使能时执行
///
/// 注释: -fsave-optimization-record 创建的 LLVMRemarkStreamer 会同时设置
///       getLLVMRemarkStreamer() 和 getMainRemarkStreamer()，因此检查前者即可。
///
/// 激活条件: 仅检查 getLLVMRemarkStreamer()。
///   -aimv-enable / -aimv-output 由 AIMVFeedbackPass 自身消费（不影响此函数）。
///   不使用跨组件的 cl::opt 共享变量，避免动态库构建模式下的链接和初始化顺序问题。
///
/// 重要: 由于此函数仅通过 getLLVMRemarkStreamer() 判断是否写入 !aimv.diag，
///   用户必须通过以下任一选项激活 remark streamer，否则 !aimv.diag 不会被写入：
///     -fsave-optimization-record=<file>   (推荐)
///     -Rpass-missed=loop-vectorize
///   仅使用 -aimv-enable + -aimv-output 而不激活 remark streamer 时，
///   AIMVFeedbackPass 会运行但读不到任何诊断数据（空 !aimv.diag）。
///
/// @param M        Module
/// @param F        被编译的函数
/// @param L        被分析的循环
/// @param LAI      LoopAccessInfo (含依赖分析结果)
/// @param CM       代价模型 (可为 nullptr，表示未到达代价评估阶段)
/// @param VF       选定的 VF
/// @param IC       选定的 IC
/// @param RemarkID 拒绝原因标识或成功标识
/// @param RemarkMsg 诊断文本消息
/// @param SE       ScalarEvolution (可为 nullptr，此时无法获取 trip_count)
/// @param RtCheckCost runtime check 总代价，从 GeneratedRTChecks::getCost() 获取
///                    （不可用时为 -1）
/// @param RtCheckCount runtime pointer check 数量，从 RuntimePointerChecking 获取
///                    （不可用时为 -1）
void emitAIMVDiagnostic(
    Module &M, Function &F, Loop &L,
    LoopAccessInfo *LAI,
    const LoopVectorizationCostModel *CM,
    ElementCount VF, unsigned IC,
    StringRef RemarkID, StringRef RemarkMsg,
    ScalarEvolution *SE = nullptr,
    int RtCheckCost = -1,
    int RtCheckCount = -1) {

  LLVMContext &Ctx = M.getContext();

  // 激活条件: LLVMRemarkStreamer 非空即可。
  // -fsave-optimization-record 和 -Rpass-missed=loop-vectorize 都会设置此 streamer。
  // -aimv-enable / -aimv-output 的处理在 AIMVFeedbackPass::run() 中，
  // 那里额外检查 AIMVFeedbackPass 自己的 OutputPath/EnabledFlag。
  if (!Ctx.getLLVMRemarkStreamer())
    return;

  // 构建 source_location: "file.c:line:col"
  // 使用 raw_string_ostream 避免 Twine 临时对象悬空引用
  DebugLoc StartLoc = L.getStartLoc();
  std::string SrcLoc;
  if (StartLoc) {
    DILocation *DIL = StartLoc.get();
    raw_string_ostream(SrcLoc) << DIL->getFilename() << ":"
                               << DIL->getLine() << ":"
                               << DIL->getColumn();
  } else if (DISubprogram *SP = F.getSubprogram()) {
    raw_string_ostream(SrcLoc) << SP->getFilename() << ":" << SP->getLine();
  } else {
    SrcLoc = "unknown";
  }

  // --- cost_data ---
  // 代价数据来自 LoopVectorizationCostModel::expectedCost():
  //   scalar_cost = expectedCost(ElementCount::getFixed(1))
  //   vector_cost = expectedCost(VF)
  // InstructionCost::getValue() 返回 int64_t，调用前需检查 isValid()。
  // 注意: InstructionCost 没有 operator*，不能写 *(expectedCost(...))。
  SmallVector<Metadata *> CostOps;
  if (CM) {
    auto ScC = CM->expectedCost(ElementCount::getFixed(1));
    auto VecC = CM->expectedCost(VF);
    int ScalarCost = ScC.isValid() ? (int)ScC.getValue() : -1;
    int VectorCost = VecC.isValid() ? (int)VecC.getValue() : -1;
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), ScalarCost)));
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), VectorCost)));
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), VF.getKnownMinValue())));
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), IC)));
  } else {
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), -1)));
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), -1)));
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), 0)));
    CostOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), 0)));
  }

  // --- dep_data (from LAI) ---
  SmallVector<Metadata *> DepOps;
  if (LAI) {
    const MemoryDepChecker &DepChecker = LAI->getDepChecker();
    // getDependences() 返回 const SmallVectorImpl<Dependence> *，需判空
    const auto *Deps = DepChecker.getDependences();
    unsigned DepCount = 0;
    SmallVector<Metadata *> DepEntries;
    if (Deps) {
      for (auto &Dep : *Deps) {
        // 过滤掉 NoDep（无实际意义），保留其余 7 种
        if (Dep.Type == MemoryDepChecker::Dependence::NoDep)
          continue;
        // 直接使用 DepName[Dep.Type] 获取 LLVM 原生类型名。
        // 注: LLVM 21 的 Dependence struct 只有 3 个 bool 访问器
        //     (isForward/isBackward/isPossiblyBackward)，
        //     无法区分全部 8 种 DepType。不存在 isBackwardVectorizable()、
        //     isForwardButPreventsForwarding()、isIndirectUnsafe()、isNoDep() 方法。
        //     直接访问 Dep.Type 字段是唯一正确的方式。
        const char *DepTypeStr = MemoryDepChecker::Dependence::DepName[Dep.Type];
        SmallVector<Metadata *> DepEntry;
        DepEntry.push_back(MDString::get(Ctx, DepTypeStr));
        // getSource/getDestination 接受 const MemoryDepChecker&，返回 Instruction*
        // Instruction::getName() 返回 StringRef；无名称时用 getOpcodeName()
        auto getInstDesc = [](Instruction *I) -> std::string {
          if (!I) return "null";
          if (I->hasName()) return I->getName().str();
          return I->getOpcodeName();
        };
        DepEntry.push_back(MDString::get(Ctx,
            getInstDesc(Dep.getSource(DepChecker))));
        DepEntry.push_back(MDString::get(Ctx,
            getInstDesc(Dep.getDestination(DepChecker))));
        // alias_result: 使用 isSafeForVectorization() 分类
        const char *SafetyStr;
        using SafetyStatus = MemoryDepChecker::VectorizationSafetyStatus;
        using MCDependence = MemoryDepChecker::Dependence;
        switch (MCDependence::isSafeForVectorization(Dep.Type)) {
        case SafetyStatus::Safe:
          SafetyStr = "safe for vectorization"; break;
        case SafetyStatus::PossiblySafeWithRtChecks:
          SafetyStr = "possibly safe with runtime checks"; break;
        case SafetyStatus::Unsafe:
          SafetyStr = "unsafe: prevents vectorization"; break;
        }
        DepEntry.push_back(MDString::get(Ctx, SafetyStr));
        DepEntries.push_back(MDNode::get(Ctx, DepEntry));
        DepCount++;
      }
    }
    DepOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), DepCount)));
    for (auto *E : DepEntries)
      DepOps.push_back(E);
  } else {
    DepOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), 0)));
  }

  // --- memory_data ---
  SmallVector<Metadata *> MemOps;
  if (LAI) {
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), LAI->getNumStores())));
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), LAI->getNumLoads())));
    // num_pred_stores: LAI 不直接提供此字段。
    // 精确值需要 LoopVectorizationLegality::blockNeedsPredication()（仅在 LV 上下文可用）。
    // 此处统一填 0，实施期可在 LoopVectorize 的插入点从 Legal 获取真实值传入。
    unsigned NumPredStores = 0;
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), NumPredStores)));

    // 计算最大对齐: 遍历循环体中所有 Load/Store 指令，从其 getAlign() 获取
    unsigned MaxAlign = 0;
    for (BasicBlock *BB : L.blocks()) {
      for (Instruction &I : *BB) {
        if (auto *LI = dyn_cast<LoadInst>(&I))
          MaxAlign = std::max(MaxAlign, LI->getAlign().value());
        else if (auto *SI = dyn_cast<StoreInst>(&I))
          MaxAlign = std::max(MaxAlign, SI->getAlign().value());
      }
    }
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), MaxAlign)));

    // stride_info: 从内存访问模式推断 stride
    // RtCheckCount == 0 且有内存操作 → 所有访问被判定为独立，大概率连续访问 (stride=1)
    // 否则标记为 "non-constant" 或 "unknown"
    // 精确 stride 需在实施期通过 SCEV 分析每个指针的步长（MVP 可简化为上述二分类）
    bool LikelyStride1 = (RtCheckCount == 0) &&
                         (LAI->getNumStores() + LAI->getNumLoads() > 0);
    MemOps.push_back(MDString::get(Ctx,
        LikelyStride1 ? "stride=1" : "non-constant"));
    // memory_check_count: 从调用者传入（不可用时为 -1）
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), RtCheckCount)));
    // 代价在 LoopVectorize 的 GeneratedRTChecks::getCost() 中计算;
    // 此处从调用者传入或在 insertion point 直接获取 Checks.getCost().getValue()
    // 若不可用（如 legality 阶段拒绝），存 -1
    MemOps.push_back(ConstantAsMetadata::get(
        ConstantInt::get(Type::getInt32Ty(Ctx), RtCheckCost)));
  } else {
    for (int i = 0; i < 7; i++)
      MemOps.push_back(ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Ctx), -1)));
  }

  // --- loop_info ---
  SmallVector<Metadata *> LoopOps;
  LoopOps.push_back(MDString::get(Ctx, L.getName()));
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), L.getNumBlocks())));
  unsigned NumInsts = 0, NumBranches = 0, NumCalls = 0;
  for (BasicBlock *BB : L.blocks()) {
    NumInsts += BB->size();
    for (Instruction &I : *BB) {
      if (isa<BranchInst>(&I) || isa<SwitchInst>(&I)) NumBranches++;
      if (isa<CallBase>(&I)) NumCalls++;
    }
  }
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), NumInsts)));
  // trip_count: 使用 SE.getSmallConstantTripCount(&L)，不可用时为 -1
  int TripCount = SE ? (int)SE->getSmallConstantTripCount(&L) : -1;
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), TripCount)));
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), NumBranches)));
  LoopOps.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), NumCalls)));

  // 组装顶层 MDNode
  MDNode *DiagNode = MDNode::get(Ctx, {
      MDString::get(Ctx, "LoopVectorize"),
      MDString::get(Ctx, RemarkID),
      MDString::get(Ctx, F.getName()),
      MDString::get(Ctx, SrcLoc),
      MDString::get(Ctx, RemarkMsg),
      MDNode::get(Ctx, CostOps),
      MDNode::get(Ctx, DepOps),
      MDNode::get(Ctx, MemOps),
      MDNode::get(Ctx, LoopOps)
  });

  // 追加到 !aimv.diag
  NamedMDNode *NMD = M.getOrInsertNamedMetadata("aimv.diag");
  NMD->addOperand(DiagNode);
}
```

### 2.3 插入点一览

#### 插入点 1: `CantReorderMemOps` — runtime check 代价过高拒绝（最重要）

**说明**: 虽然 remark 文本是 "cannot prove it is safe to reorder memory operations"，
但触发条件实际来自 `isOutsideLoopWorkProfitable()` 返回 false（runtime checks 的代价太高）。
真正的依赖分析失败（UnsafeDep）在 `LoopAccessAnalysis.cpp` 中（见插入点 3）。

**位置**: `LoopVectorize.cpp`，约 10117-10127 行
**现有代码**（简化）:
```cpp
// Line 10117-10127 (approx)
if (!isOutsideLoopWorkProfitable(L, *LVL, VF, Checks)) {
  reportVectorizationFailure("Can't determine memory dependences",
      "unsafe dependent memory operations in loop. Use "
      "#pragma loop distribute(enable) to allow loop distribution "
      "...",
      "CantReorderMemOps", ORE, L);
  return LoopVectorizeResult(false);
}
```
**插入**（在 `reportVectorizationFailure` 调用之后，`return` 之前）:
```cpp
  // CM 可用，SE 可从 PSE 获取
  // Checks.getCost() 返回 InstructionCost，可能 Invalid → 需判空
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  // RtCheckCount: 从 LAI 的 RuntimePointerChecking 获取
  int RtChkCount = LVL.getLAI().hasRuntimePointerChecks() ?
      (int)LVL.getLAI().getRuntimePointerChecking().getNumberOfChecks() : 0;
  emitAIMVDiagnostic(M, F, L, &(*LVL).getLAI(), CM, VF, IC,
                     "CantReorderMemOps",
                     "unsafe dependent memory operations in loop",
                     &PSE.getSE(),
                     RtCost, RtChkCount);
```

#### 插入点 2: `VectorizationNotBeneficial` — 代价模型拒绝

**位置**: `LoopVectorize.cpp`，约 10133-10138 行
**现有代码**（简化）:
```cpp
// VF.Width is scalar
reportVectorizationFailure("Vectorization not beneficial",
    "the cost-model indicates that vectorization is not beneficial",
    "VectorizationNotBeneficial", ORE, L);
```
**插入**:
```cpp
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  int RtChkCount = LVL.getLAI().hasRuntimePointerChecks() ?
      (int)LVL.getLAI().getRuntimePointerChecking().getNumberOfChecks() : 0;
  emitAIMVDiagnostic(M, F, L, &(*LVL).getLAI(), CM, VF, IC,
                     "VectorizationNotBeneficial",
                     "the cost-model indicates that vectorization is not beneficial",
                     &PSE.getSE(),
                     RtCost, RtChkCount);
```

#### 插入点 3: LAA 转发的 unsafe dep — 依赖分析失败

**位置**: `LoopAccessAnalysis.cpp`，`LoopAccessInfo::emitUnsafeDependenceRemark()` 内
**说明**: 真正的 UnsafeDep 发射点在此，不在 LoopVectorizationLegality.cpp。
`LoopVectorizationLegality.cpp:1200-1208` 仅转发 LAI->getReport() 中的已有 remark。
此处的上下文：无 CM（legality 阶段，代价模型未构建），无 PSE，无 Checks。
但 LAI 可用。

**方案**: 将 `emitAIMVDiagnostic()` 声明放在新建的头文件
`llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h` 中（非 static）。
该头文件由 LoopVectorize.cpp 和 LoopAccessAnalysis.cpp 共同包含。

**插入**（CM/PSE/Checks 均不可用 → 使用默认值）:
```cpp
  // LoopAccessAnalysis 上下文中无 CM/PSE/Checks，所有诊断相关参数使用默认值
  // RtCheckCount: LAI 自身有 RuntimePointerChecking 信息
  int RtChkCount = LAI->hasRuntimePointerChecks() ?
      (int)LAI->getRuntimePointerChecking().getNumberOfChecks() : 0;
  emitAIMVDiagnostic(M, F, L, LAI, /*CM=*/nullptr, VF, IC,
                     "UnsafeDep",
                     LAI->getReport()->getRemarkMsg(),
                     /*SE=*/nullptr,
                     /*RtCheckCost=*/-1, RtChkCount);
```

#### 插入点 4: `InterleavingNotBeneficial` — 交错不利

**位置**: `LoopVectorize.cpp`，约 10152-10159 行
**插入**: 同模式，RemarkID = `"InterleavingNotBeneficial"`

#### 插入点 5: 向量化成功 — 正向记录

**说明**: Driver 依靠 `!aimv.diag` 中的 passed 记录来**正向确认**向量化成功。
仅靠"无 missed 记录"无法区分"向量化成功"与"Pass 未运行"。

**精确位置**（LLVM 21）: 在 `LoopVectorize.cpp` 的 `processLoop()` 函数中，
`LoopVectorizeResult` 构造处（约 line 10260-10280），VPlan 已执行完毕、
向量化决策已确定为 true 的单一路径。实施前需锁定该版本的确切行号。

**注意**: 不要在 `reportVectorizationFailure` 路径或 early-exit 路径插入。

### 2.5 部分向量化策略

一个函数可能包含多个循环，部分循环可能已成功向量化、部分仍然失败。

**`!aimv.diag` 的语义**: 每个循环独立记录——被拒绝的循环写 missed，成功的循环写 passed。
`AIMVFeedbackPass` 输出的 JSON 中可能同时包含同一函数的 missed 和 passed 诊断。

**Driver 的迭代策略**:
- **函数级迭代** — Driver 每轮关注**一个**目标循环（由 `loop_location` 指定）
- MCP 只收到该循环的 missed 诊断 + history
- 终止条件: 目标循环从 missed 变为 passed（而非函数内所有循环都 passed）
- Driver 可通过 `--loop-line` 参数或自动选择第一个 missed 循环来指定目标

**多循环场景**: 用户需为不同循环分别运行 `aimv-driver`（或 CI 批量工具逐循环调用）。
同一文件多循环并行分析由文件锁保证互斥（见 DRIVER_DESIGN §9）。

**插入**:
```cpp
  // 记录成功向量化，severity 为 "passed"（存储为 MDString 或通过 RemarkID 前缀区分）
  // 方案 A: 新增 Severity 字段（参考下面"增强 Metadata 格式"）
  // 方案 B: RemarkID 用 "Passed" 前缀表示成功（MVP 阶段更简单）
  //
  // MVP 建议: RemarkID = "LoopVectorized" + 原有标识，RemarkMsg 描述 VF/IC 信息
  auto RtCostIC = Checks.getCost();
  int RtCost = RtCostIC.isValid() ? (int)RtCostIC.getValue() : -1;
  int RtChkCount = LVL.getLAI().hasRuntimePointerChecks() ?
      (int)LVL.getLAI().getRuntimePointerChecking().getNumberOfChecks() : 0;
  emitAIMVDiagnostic(M, F, L, &(*LVL).getLAI(), CM, VF, IC,
                     "LoopVectorized",
                     "loop vectorized: VF=" + Twine(VF.getKnownMinValue()).str(),
                     &PSE.getSE(),
                     RtCost, RtChkCount);
```

### 2.4 SLPVectorize 改动点（可选，Phase 2）

**文件**: `llvm/lib/Transforms/Vectorize/SLPVectorizer.cpp`

SLP 主要处理 BB 内向量化，失败模式比 LoopVectorize 简单。暂定在 `tryToVectorize()` 返回 false 时写入，格式复用 `!aimv.diag`，pass_name = `"SLPVectorize"`。

**初期可跳过**，MVP 聚焦 LoopVectorize。

---

## 3. AIMVFeedbackPass 设计

### 3.1 文件结构

```
llvm/lib/Transforms/AIMV/
├── AIMVFeedbackPass.cpp          # Pass 主实现 (~350 行)
├── AIMVDiagnosticParser.cpp      # !aimv.diag metadata 解析器 (~150 行)
└── CMakeLists.txt                # 构建配置
```

```
llvm/include/llvm/Transforms/AIMV/
└── AIMVFeedback.h                # 公共头文件
```

### 3.2 头文件

```cpp
// [BiSheng] llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h

#ifndef LLVM_TRANSFORMS_AIMV_AIMVFEEDBACK_H
#define LLVM_TRANSFORMS_AIMV_AIMVFEEDBACK_H

#include "llvm/IR/PassManager.h"
#include <string>

namespace llvm {

/// [BiSheng] AIMVFeedbackPass — 收集向量化诊断信息并导出 JSON
///
/// 类型: Function Pass（对每个函数独立处理该函数关联的 !aimv.diag 诊断）
/// 运行时机: LoopVectorize + SLPVectorize 之后
/// 输入: 从 Module 的 !aimv.diag named metadata 读取，筛选当前函数的诊断记录
/// 处理: 补充源码上下文、IR 片段、AA/SCEV 分析结果
/// 输出: 追加写入 --aimv-output=<path> 指定的 JSON 文件
///
/// Pipeline 注册 (PassRegistry.def):
///   FUNCTION_PASS("aimv-feedback", AIMVFeedbackPass())
///
/// 注意: 作为 Function Pass，通过 F.getParent() 获取 Module 访问 !aimv.diag。
///       对空 Module（无 !aimv.diag）或无诊断的函数，不产生输出。
///
/// 命令行:
///   opt -passes="loop-vectorize,aimv-feedback" -aimv-output=/tmp/diag.json < input.ll
class AIMVFeedbackPass : public PassInfoMixin<AIMVFeedbackPass> {
public:
  /// [BiSheng] 设置 JSON 输出文件路径
  void setOutputPath(const std::string &Path) { OutputPath = Path; }

  /// [BiSheng] 设置只导出指定函数的诊断（空=全部）
  void setTargetFunction(const std::string &FuncName) {
    TargetFunction = FuncName;
  }

  /// [BiSheng] 显式启用（即使无 remark streamer 也运行）
  void setEnabled(bool V = true) { EnabledFlag = V; }

  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  static StringRef name() { return "aimv-feedback"; }

  /// [BiSheng] 从 !aimv.diag 解析出原始诊断向量
  /// @return 空 vector 表示无诊断数据
  /// @{
  struct RawDiagnostic {
    std::string PassName;      // "LoopVectorize" | "SLPVectorize"
    std::string RemarkID;      // "CantReorderMemOps" | ...
    std::string FunctionName;
    std::string SourceLocation; // "file.c:42:5"
    std::string RemarkMsg;
    // 代价
    int ScalarCost = -1;
    int VectorCost = -1;
    int VF = 0;
    int IC = 0;
    // 依赖
    struct DepEntry {
      std::string Type;        // LLVM Dependence::DepName[]: "Backward"|"Forward"|"Unknown"|"IndirectUnsafe"|...
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

  static std::vector<RawDiagnostic>
  parseDiagnostics(Module &M);

private:
  std::string OutputPath;
  std::string TargetFunction;
  bool EnabledFlag = false;  // -aimv-enable 设置
};

} // namespace llvm

#endif
```

### 3.3 Pass 实现要点

**源码反向映射回退链（消除 !dbg 漂移风险）**:

优化后的 IR 行号可能因 pass 重排而漂移。`extractSourceContext()` 按以下优先级回退：

```
1. !dbg metadata → DILocation::getLine()      (最精确)
2. DILocation::getScope() → DISubprogram       (函数级匹配)
3. 循环 header 的 DebugLoc                     (循环级粗略匹配)
4. 函数名字符串匹配 + "approximate" 标记       (最后手段)
```

回退到第 3/4 级时，诊断 JSON 中增加 `"source_accuracy": "approximate"` 标记，
MCP prompt 中明确告知大模型"行号可能偏差 ±5 行"。

**severity 推断规则**:

!aimv.diag metadata 中未显式存储 severity，AIMVFeedbackPass 解析时按以下规则推断：

```
RemarkID 匹配:
  "CantReorderMemOps" | "VectorizationNotBeneficial"
  | "UnsafeDep" | "InterleavingNotBeneficial"
    → severity = "missed"

  "LoopVectorized" (或包含 "Passed" 前缀)
    → severity = "passed"

  其他   → severity = "analysis"
```

`AIMVFeedbackPass::run(Function &F, FunctionAnalysisManager &AM)` 核心流程：

```
0. 缓存优化: Function Pass 对每个函数独立运行，同一 Module 的 !aimv.diag 会被
   重复解析 N 次（N=函数数）。实施时在 pass 实例内缓存首次 parseDiagnostics() 结果:
     if (CachedModule != M) { CachedDiags = parseDiagnostics(*M); CachedModule = M; }
   后续函数直接复用 CachedDiags 做 FunctionName 过滤，消除 O(N*M) 开销。

1. 通过 F.getParent() 获取 Module &M
   - 若 Module 无 !aimv.diag Named Metadata → 返回 PreservedAnalyses::all()

2. 调用 parseDiagnostics(M) 解析 !aimv.diag metadata
   - 筛选: 只保留 FunctionName == F.getName() 的诊断
   - 若筛选后为空 → 返回 PreservedAnalyses::all()

3. 获取分析结果:
   - AM.getResult<AAResults>(F)
   - AM.getResult<ScalarEvolution>(F)（若有）
   - AM.getResult<LoopInfo>(F)
   - AM.getResult<TargetIRAnalysis>(F) → TargetTransformInfo
   - Module 级信息: M->getTargetTriple(), TTI->getRegisterBitWidth(true) 推算 vector_width
     CPU/Features 从 Module 的 target-cpu/target-features attributes 获取
     （-mcpu=-aimv-target-cpu= 作为 fallback，通过 setTargetCpu() 注入）

4. 对每条匹配的诊断:
   a. 如果设置了 TargetFunction，仅处理目标函数（跳过不匹配的）
   b. 在 LoopInfo 中按位置匹配对应的 Loop*
   c. 从 Loop* 的 DebugLoc 定位源码位置
   d. 调用 extractSourceContext() 获取源码片段
   e. 调用 extractIRSnippet() 获取 IR 片段

5. 构建函数级 JSON 文档 (与 MCP Server AnalyzeRequest 格式兼容)
   - target: {triple, cpu, features, vector_width}
   - function: {name, signature, source_code, source_file, loop_line}
   - diagnostics: [{pass_name, remark, severity, cost_model, dependencies,
                     memory_info, source_context, ir_snippet}, ...]

6. 追加写入 OutputPath 指定的 JSON 文件（多函数共享同一文件时使用 JSON Lines 或数组追加）
   - 注意: Function Pass 对每个函数独立运行，多函数编译时并发写入同一 JSON 文件
   - 实现期需加文件级互斥锁（fcntl.flock / CreateMutex）或使用 per-function 独立文件

7. 返回 PreservedAnalyses::all() (本 pass 只读)
```

### 3.4 JSON 输出格式

输出文件与 PLAN.md 中定义的 `AnalyzeRequest` 的 **diagnostics 部分**对应。
完整 AnalyzeRequest 需要的 `function`（source_code/signature/loop_line）和 `target` 信息
由 Driver 在发送 MCP 请求前从源文件和编译参数中补充。
AIMVFeedbackPass 仅产出 IR 侧可获取的诊断数据。关键点：

```json
{
  "request_id": "aimv-<module_hash>-<timestamp>",
  "target": {
    "triple": "armv7-unknown-linux-gnueabi",
    "cpu": "cortex-a9",
    "features": ["neon", "vfp4"],
    "vector_width": 128
  },
  "diagnostics": [
    {
      "pass_name": "LoopVectorize",
      "remark_id": "CantReorderMemOps",
      "remark_text": "loop not vectorized: unsafe dependent memory operations",
      "severity": "missed",
      "function_name": "process_task",
      "loop_location": "../src/task.c:42:5",
      "source_context": "for (int i = 0; i < n; i++) {\n    a[i] = b[i+1] + c[i];\n}",
      "ir_snippet": "define void @process_task(...) ...\n  %i = phi i64 [ 0, %entry ], [ %i.next, %for.body ]\n  %gep_a = getelementptr ...",
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
    }
  ]
}
```

### 3.5 Pass 注册

**文件**: `llvm/lib/Passes/PassRegistry.def`

```cpp
// [BiSheng] AIMV vectorization feedback analysis
FUNCTION_PASS("aimv-feedback", AIMVFeedbackPass())
```

---

## 4. PassBuilder Pipeline 集成

### 4.1 修改位置

**文件**: `llvm/lib/Passes/PassBuilderPipelines.cpp`
**方法**: `PassBuilder::addVectorPasses()`
**插入位置**: SLPVectorizer 之后，VectorCombine 之前

```cpp
void PassBuilder::addVectorPasses(OptimizationLevel Level,
                                  FunctionPassManager &FPM, bool IsFullLTO) {
  FPM.addPass(LoopVectorizePass(
      LoopVectorizeOptions(!PTO.LoopInterleaving, !PTO.LoopVectorization)));

  // ... 其他 vectorization 相关 passes ...

  if (PTO.SLPVectorization) {
    FPM.addPass(SLPVectorizerPass());
  }

  // [BiSheng] AIMV: 在 LoopVectorize + SLPVectorize 之后收集诊断
  // 作为 Function Pass 插入 FPM，pass 内部通过 F.getParent() 访问 Module
  // 仅在有 remark streamer 或 -aimv-output 指定时实际执行
  FPM.addPass(AIMVFeedbackPass());

  // ... 后续 passes (VectorCombine, InstCombine 等) ...
}
```

### 4.2 条件激活

AIMVFeedbackPass 不是无条件运行。通过检查 remark streamer 是否存在来决定是否激活：

```cpp
// [BiSheng] AIMVFeedback Pass 内部
PreservedAnalyses AIMVFeedbackPass::run(Function &F, FunctionAnalysisManager &AM) {
  Module *M = F.getParent();
  if (!M)
    return PreservedAnalyses::all();

  // 激活条件（三选一）:
  //   1. getLLVMRemarkStreamer() 非空（-fsave-optimization-record / -Rpass-missed 触发）
  //   2. !OutputPath.empty()（-aimv-output 显式指定）
  //   3. AIMVFeedbackPass 自身的 EnabledFlag（-aimv-enable 触发）
  // 注意: EnabledFlag 和 OutputPath 是 AIMVFeedbackPass 的私有成员，
  //       由 BackendUtil.cpp 在构造 pass 时设置，不依赖跨组件符号。
  if (!M->getContext().getLLVMRemarkStreamer() && OutputPath.empty() && !EnabledFlag)
    return PreservedAnalyses::all();

  // ... 正常执行 ...
}
```

触发条件（任一满足即运行）：
- `-fsave-optimization-record=<file>` — 写入 YAML remarks（同时激活 remark streamer）
- `-Rpass-missed=loop-vectorize` — 输出 remarks（激活 remark streamer）
- `-aimv-output=<path>` — 设置 OutputPath（AIMVFeedbackPass 私有成员，由 BackendUtil.cpp 注入）
- `-aimv-enable` — 设置 EnabledFlag（AIMVFeedbackPass 私有成员）

**关键**: 这四个触发条件全部在 AIMVFeedbackPass 内部处理，不依赖任何跨组件的 `cl::opt` 变量。
`-aimv-output` 和 `-aimv-enable` 在 `clang/lib/CodeGen/BackendUtil.cpp` 中解析后，
通过 `AIMVFeedbackPass::setOutputPath()` / `AIMVFeedbackPass::setEnabled()` 注入 pass 实例。

**注意（激活条件不一致问题）**: `emitAIMVDiagnostic()` 仅检查 `getLLVMRemarkStreamer()` 来
决定是否写入 `!aimv.diag`。因此，仅使用 `-aimv-enable` + `-aimv-output` 而不激活 remark streamer 时，
`AIMVFeedbackPass` 会运行但读不到诊断数据——因为 `emitAIMVDiagnostic()` 根本没写入。
正确用法必须同时指定 `-fsave-optimization-record` 或 `-Rpass-missed=loop-vectorize` 之一。
这是有意的取舍：避免在 `emitAIMVDiagnostic()` 中引入跨组件依赖。

### 4.3 与现有 Remark 基础设施的关系

```
clang -O2 -fsave-optimization-record=opt.yaml -aimv-output=aimv.json src.c

       ┌────────────────────────────────────────┐
       │         Pass Pipeline                  │
       │                                        │
       │  LoopVectorizePass                     │
       │    ├── ORE→emit(remark)  ──▶ YAML     │ ← 现有路径
       │    └── emitAIMVDiagnostic() ──▶ !aimv.diag │ ← 新增路径
       │                                        │
       │  SLPVectorizerPass                     │
       │    └── (同上)                           │
       │                                        │
       │  AIMVFeedbackPass                      │
       │    ├── 读取 !aimv.diag                 │
       │    ├── 补充上下文                       │
       │    └── 输出 aimv.json                  │ ← 新增输出
       │                                        │
       │  YAMLRemarkSerializer ──▶ opt.yaml     │ ← 现有路径
       └────────────────────────────────────────┘
```

两条路径**平行独立**，互不干扰。

---

## 5. 构建集成

### 5.1 CMakeLists.txt

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

### 5.2 顶层 CMakeLists.txt 改动

**文件**: `llvm/lib/Transforms/CMakeLists.txt`

```cmake
# [BiSheng] AIMV feedback pass
add_subdirectory(AIMV)
```

### 5.3 Pass 插件注册

**文件**: `llvm/lib/Passes/CMakeLists.txt`

将 `LLVMAIMV` 添加到 `LINK_COMPONENTS`，确保 PassRegistry.def 中的 `FUNCTION_PASS("aimv-feedback", ...)` 可解析。

---

## 6. 命令行参数

### 6.1 新增选项

**文件**: `clang/lib/Driver/ToolChains/Clang.cpp` 或 `clang/lib/CodeGen/BackendUtil.cpp`

| 选项 | 说明 |
|------|------|
| `-aimv-output=<path>` | 指定 AIMV JSON 诊断输出路径 |
| `-aimv-target-function=<name>` | 只分析指定函数 |
| `-aimv-enable` | 显式启用 AIMV 诊断收集（**必须与 `-fsave-optimization-record` 或 `-Rpass-missed` 配合使用**，单独使用无效——见 §4.2 注意事项） |

### 6.2 使用示例

```bash
# 完整流程：编译 + 收集 opt-info YAML + AIMV JSON
clang -O2 \
      -fsave-optimization-record=opt.yaml \
      -aimv-output=aimv.json \
      -Rpass-missed=loop-vectorize \
      -g \
      src/task.c -o task

# aimv.json 可直接 POST 到 MCP Server
curl -X POST http://mcp-server:8080/api/v1/analyze-vectorization \
     -H "Content-Type: application/json" \
     -d @aimv.json
```

---

## 7. 改动汇总

| 文件 | 变更类型 | 改动量 | 说明 |
|------|---------|--------|------|
| `llvm/lib/Transforms/Vectorize/LoopVectorize.cpp` | 修改 | +120 行 | 新增 `emitAIMVDiagnostic()` + 4 拒绝点 + 1 成功点调用 |
| `llvm/lib/Transforms/Vectorize/LoopAccessAnalysis.cpp` | 修改 | +5 行 | UnsafeDep 拒绝点增加 AIMV 写入 |
| `llvm/lib/Transforms/Vectorize/AIMVDiagnostic.h` | **新建** | ~20 行 | 共享内部头文件，emitAIMVDiagnostic() 声明 |
| `llvm/lib/Transforms/AIMV/AIMVFeedbackPass.cpp` | **新建** | ~350 行 | Pass 主实现 |
| `llvm/lib/Transforms/AIMV/AIMVDiagnosticParser.cpp` | **新建** | ~150 行 | Metadata → RawDiagnostic 解析 |
| `llvm/include/llvm/Transforms/AIMV/AIMVFeedback.h` | **新建** | ~80 行 | 公共头文件 |
| `llvm/lib/Transforms/AIMV/CMakeLists.txt` | **新建** | ~15 行 | 构建配置 |
| `llvm/lib/Transforms/CMakeLists.txt` | 修改 | +1 行 | 添加子目录 |
| `llvm/lib/Passes/PassBuilderPipelines.cpp` | 修改 | +3 行 | 在 addVectorPasses 中注册 |
| `llvm/lib/Passes/PassRegistry.def` | 修改 | +2 行 | FUNCTION_PASS 注册 |
| `llvm/lib/Passes/CMakeLists.txt` | 修改 | +1 行 | 链接 LLVMAIMV |
| `clang/lib/CodeGen/BackendUtil.cpp` | 修改 | +15 行 | `-aimv-output` 选项处理 |

**总新增文件**: 5 个（~615 行，含 `AIMVDiagnostic.h`）
**总修改文件**: 7 个（~145 行）

---

## 8. 测试策略

### 8.1 LLVM Lit 测试

```
llvm/test/Transforms/AIMV/
├── aimv_diag_metadata.ll     # 验证 !aimv.diag 的正确生成
├── aimv_feedback_json.ll     # 验证 JSON 输出格式
├── aimv_dep_fail.ll          # 依赖分析失败场景
├── aimv_cost_reject.ll       # 代价模型拒绝场景
└── lit.local.cfg
```

### 8.2 测试 Pattern

```llvm
; RUN: opt -passes="loop-vectorize,aimv-feedback" -aimv-output=%t.json -S < %s
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

---

*文档版本: 1.0*
*创建日期: 2026-04-29*
