# EJitWrapperGenPass 设计文档

**版本**: 1.1
**日期**: 2026-04-29
**关联**: SPEC4.md, PLAN4.md
**类型**: AOT Module Pass
**顺序**: AOT Pipeline 第 3 步

---

## 1. 概述

EJitWrapperGenPass 为所有 `ejit_entry` 函数生成 Wrapper 插桩代码。采用单函数混合方案：Wrapper 逻辑直接插入原函数入口，JIT 成功时调用特化函数并返回，JIT 失败时继续执行原函数体（Fallback）。用户调用原函数名即自动经过 JIT 编译路径。

### 1.1 核心职责

- 定位所有带 `!{"ejit_entry"}` metadata 的函数
- 识别函数参数中 `ejit_period_arr_ind` 标注的参数及其关联的时间窗名称
- 在函数入口加载 per-lifecycle dimType globals，构造 dim pairs
- 插入 `ejit_taskpool_compile_or_get(funcIdx, dims, numDims, &fn, &bucket)` 调用
- 插入结果判空、类型转换和特化函数调用逻辑
- 分离原函数体到独立的命名块，保持原逻辑完整

### 1.2 设计约束

| 约束项 | 说明 |
|--------|------|
| 插桩方案 | 单函数混合方案，不创建独立 wrapper 函数 |
| 参数识别 | 通过函数级 `ejit.metadata` 中的 `ejit_period_arr_ind` 条目（`{tag, periodName, argIndex}`） |
| 类型安全 | 生成 PFN 函数指针 typedef 用于类型转换 |
| 最大维度 | 单个函数最多 4 个 `ejit_period_arr_ind` 参数 |
| static 依赖 | 所有 ejit_entry 函数隐式依赖 static 时间窗，无需显式参数 |

---

## 2. 输入 IR 格式

### 2.1 ejit_entry 函数 Metadata

```llvm
; 无 period_arr_ind 参数 (仅依赖 static 时间窗)
define void @process_static() #0 {
    ; ...
}
; !ejit.metadata = distinct !{!0}
; !0 = !{!"ejit_entry"}

; 单 period_arr_ind 参数
define void @process_task(i32 %cellIdx) #0 {
    ; ...
}
; !ejit.metadata = distinct !{!0, !1}
; !0 = !{!"ejit_entry"}
; !1 = !{!"ejit_period_arr_ind", !"cell", i32 0}   ; 参数索引 0 → cell

; 多 period_arr_ind 参数
define void @process_multi(i32 %cellIdx, i32 %trpIdx, i32 %iterations) #0 {
    ; ...
}
; !ejit.metadata = distinct !{!0, !1, !2}
; !0 = !{!"ejit_entry"}
; !1 = !{!"ejit_period_arr_ind", !"cell", i32 0}   ; 参数索引 0 → cell
; !2 = !{!"ejit_period_arr_ind", !"trp", i32 1}    ; 参数索引 1 → trp
```

### 2.2 period_arr_ind 参数识别

`ejit_period_arr_ind` 参数通过函数级 `ejit.metadata` 编码，格式与 PASS4 共用：

```llvm
!{!"ejit_period_arr_ind", !"periodName", i32 argIndex}
```

此 metadata 附加在函数上（`F.setMetadata("ejit.metadata", MD)`），Clang CodeGen 在 CodeGenFunction 时为每个 `ejit_period_arr_ind` 参数生成一条。不再依赖 debug info，`-g0` 模式下同样可用。

---

## 3. 核心算法

### 3.1 主流程

```
输入: Module M (含 ejit_entry + period_arr_ind 参数函数)
输出: Module M (函数入口插入 Wrapper 代码)

步骤:
1. CollectEjitEntryFunctions(M) → 收集所有 ejit_entry 函数
2. ForEach entryFunc:
     ParseParamDimensions(entryFunc) → 提取 period_arr_ind 参数列表
     SplitFunctionAtEntry(entryFunc) → 分离原函数体到 fallback 块
     GenerateWrapperPrologue(entryFunc, dims) → 加载 per-lifecycle dimType globals → 构造 [{dimType, instanceId}] 对 → ejit_taskpool_compile_or_get() → jit_dispatch: call fn + ejit_taskpool_release_read()
     GenerateDispatch(entryFunc) → 生成 pfn 判空 + 分支跳转
```

### 3.2 详细伪代码

```cpp
PreservedAnalyses EJitWrapperGenPass::run(Module& M, ModuleAnalysisManager& AM) {
    std::vector<Function*> entryFuncs;

    for (Function& F : M.functions()) {
        if (isEjitEntry(F))
            entryFuncs.push_back(&F);
    }

    for (Function* F : entryFuncs) {
        // Step 1: 识别 period_arr_ind 参数
        DimInfo dims = parsePeriodArrIndParams(F);

        // Step 2: 拆分为 wrapper + fallback + dispatch
        auto [fallbackBB, dispatchBB] = splitFunction(F);

        // Step 3: 生成 wrapper prologue (dims 构建 + compile_or_get)
        auto wr = insertWrapperPrologue(F, dims, fallbackBB, dispatchBB);

        // Step 4: 特化函数调用
        insertDispatchLogic(F, wr.dispatchBB, wr.jitResult);
    }

    return entryFuncs.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
}
```

### 3.3 函数拆分 (splitFunction)

```
splitFunction 将原函数结构从:

    define void @func(params) {
    entry:
        ; ... 原函数体
        ret void
    }

变换为:

    define void @func(params) {
    entry_jit:          ; 新 entry → Wrapper 逻辑
        ; ... dims 构建
        ; ... ejit_taskpool_compile_or_get(i32 funcIdx, ...)
        ; br pfn != null → dispatch_call
        ; br pfn == null → fallback

    dispatch_call:      ; JIT 成功路径
        ; ... 调用 pfn(params)
        ret void

    fallback:           ; JIT 失败路径 = 原函数体
        ; ... 原函数体
        ret void
    }
```

```cpp
// 返回 (fallback 块, dispatch 块)
std::pair<BasicBlock*, BasicBlock*> splitFunction(Function* F) {
    LLVMContext& Ctx = F->getContext();
    BasicBlock* origEntry = &F->getEntryBlock();

    // 创建新的 entry block (jit_entry)
    BasicBlock* jitEntry = BasicBlock::Create(Ctx, "jit_entry", F);
    BasicBlock* dispatchCall = BasicBlock::Create(Ctx, "jit_dispatch", F);
    BasicBlock* fallback = BasicBlock::Create(Ctx, "jit_fallback", F);

    // 将原 entry 的所有指令移动到 fallback
    fallback->getInstList().splice(fallback->end(),
                                    origEntry->getInstList());
    // 修复 fallback 中的 PHI 节点引用等
    // ...

    // 删除原 entry (已空)
    origEntry->eraseFromParent();

    // 新的布局: jit_entry → [jit_dispatch 或 jit_fallback]
    // jit_entry 中生成 wrapper 逻辑，根据 pfn 结果跳转

    return {fallback, dispatchCall};
}
```

### 3.4 Wrapper Prologue 生成

```cpp
// 返回 (dispatchCall 块, jit_result 值) 供 insertDispatchLogic 使用
struct WrapperResult {
    BasicBlock* dispatchBB;
    Value* jitResult;
};

WrapperResult insertWrapperPrologue(Function* F, const DimInfo& dims,
                                     BasicBlock* fallback, BasicBlock* dispatchCall) {
    LLVMContext& Ctx = F->getContext();
    IRBuilder<> Builder(Ctx);
    BasicBlock* jitEntry = &F->getEntryBlock();
    Builder.SetInsertPoint(jitEntry);

    // Step 1: 计算 funcIdx (FNV-1a hash) 并分配 dims 数组 (alloca)
    unsigned dimCount = dims.params.size();
    Value* dimsArray = nullptr;
    if (dimCount > 0) {
        // dim pairs: [{dimType, instanceId}], 从 per-lifecycle globals 加载, mem2reg 后零栈分配
        StructType* dimTy = StructType::get(Ctx, {
            PointerType::getUnqual(Ctx),  // name: const char*
            Type::getInt8Ty(Ctx)          // index: uint8_t
        });
        ArrayType* dimsArrTy = ArrayType::get(dimTy, dimCount);
        dimsArray = Builder.CreateAlloca(dimsArrTy);

        // Step 2: 填充每个 dims[i]
        for (unsigned i = 0; i < dimCount; i++) {
            Value* dimPtr = Builder.CreateConstGEP2_32(dimsArrTy, dimsArray, 0, i);

            // name 字段: 时间窗名称字符串
            Constant* periodNameStr = getOrCreateGlobalString(M, dims.params[i].periodName);
            Value* nameField = Builder.CreateConstGEP2_32(dimTy, dimPtr, 0, 0);
            Builder.CreateStore(periodNameStr, nameField);

            // index 字段: 参数实际值
            Value* indexField = Builder.CreateConstGEP2_32(dimTy, dimPtr, 0, 1);
            Value* argVal = F->getArg(dims.params[i].argIdx);
            // ejit_period_arr_ind 参数类型可能为 i8/i16/u8 等，统一 ZExt 到 i32
            Value* indexVal = Builder.CreateZExtOrTrunc(argVal, Type::getInt32Ty(Ctx));
            Builder.CreateStore(indexVal, indexField);
        }
    }

    // Step 3: 计算 funcIdx (FNV-1a hash, 零运行时开销)
    Value* funcIdxVal = ConstantInt::get(Type::getInt32Ty(Ctx),
                                        hashFuncName(F->getName()));

    // Step 4: 构建 dims 指针
    Value* dimsPtr = dimsArray
        ? Builder.CreateBitCast(dimsArray, PointerType::getUnqual(Ctx))
        : ConstantPointerNull::get(PointerType::getUnqual(Ctx));

    // Step 5: count 常量
    Value* countVal = ConstantInt::get(Type::getInt32Ty(Ctx), dimCount);

    // Step 6: out_pfn (NULL, 保留参数)
    Value* outPfn = ConstantPointerNull::get(PointerType::get(PointerType::getUnqual(Ctx), 0));

    // Step 7: 调用 ejit_taskpool_compile_or_get(i32 funcIdx, ...)
    // i32 @ejit_taskpool_compile_or_get(i32 funcIndex, ptr dims, i32 numDims, ptr outFn, ptr outBucket)
    //                           int count, void** out_pfn);
    Function* compileFn = getOrDeclareCompileOrGet(M);
    Value* result = Builder.CreateCall(compileFn,
        {funcNamePtr, dimsPtr, countVal, outPfn}, "jit_result");

    // Step 8: 判空分支 → fallback 或 dispatchCall
    Value* isNull = Builder.CreateICmpEQ(result,
        ConstantPointerNull::get(PointerType::getUnqual(Ctx)), "jit_is_null");

    Builder.CreateCondBr(isNull, fallback, dispatchCall);

    return {dispatchCall, result};
}
```

### 3.5 Dispatch 逻辑生成

```cpp
void insertDispatchLogic(Function* F, BasicBlock* dispatchBB, Value* jitResult) {
    LLVMContext& Ctx = F->getContext();
    IRBuilder<> Builder(Ctx);
    Builder.SetInsertPoint(dispatchBB);

    // opaque pointer 模型下无需类型转换，直接用 ptr 调用
    std::vector<Value*> args;
    for (Argument& arg : F->args()) {
        args.push_back(&arg);
    }

    CallInst* pfnCall = Builder.CreateCall(F->getFunctionType(), jitResult, args);

    // 处理返回值
    if (F->getReturnType()->isVoidTy()) {
        Builder.CreateRetVoid();
    } else {
        Builder.CreateRet(pfnCall);
    }

    // fallback 块保持原函数体不变，最终以原始 ret 指令结尾
}
```

---

## 4. 输出 IR 变化

### 4.1 仅 static 依赖 (无 period_arr_ind 参数)

```llvm
; 输入:
define void @process_static() {
  ; ... 原函数体 ...
}

; 输出:
define void @process_static() {
jit_entry:
  i32 <hash>  ; FNV-1a hash of function name
  %jit_result = call ptr @ejit_taskpool_compile_or_get(i32 <hash>, ptr null, i32 0, ptr null)
  %jit_is_null = icmp eq ptr %jit_result, null
  br i1 %jit_is_null, label %jit_fallback, label %jit_dispatch

jit_dispatch:
  call void %jit_result()
  ret void

jit_fallback:
  ; ... 原函数体 ...
  ret void
}
```

### 4.2 单维度参数 (cell)

```llvm
; 输入:
define void @process_task(i32 %cellIdx) {
  ; ... 原函数体 ...
}

; 输出:
define void @process_task(i32 %cellIdx) {
jit_entry:
  %dims = alloca [1 x { ptr, i8 }], align 8
  %dim0_name = getelementptr ... { ptr, i8 }* %dims, i32 0, i32 0
  store ptr @".str.cell", ptr* %dim0_name
  %dim0_idx = getelementptr ... { ptr, i8 }* %dims, i32 0, i32 1
  store i8 %cellIdx, ptr* %dim0_idx
  i32 <hash>  ; FNV-1a hash of function name
  %dims_ptr = bitcast ... to ptr
  %jit_result = call ptr @ejit_taskpool_compile_or_get(i32 <hash>, ptr %dims_ptr, i32 1, ptr null)
  %jit_is_null = icmp eq ptr %jit_result, null
  br i1 %jit_is_null, label %jit_fallback, label %jit_dispatch

jit_dispatch:
  call void %jit_result(i32 %cellIdx)
  ret void

jit_fallback:
  ; ... 原函数体 ...
  ret void
}
```

### 4.3 多维度参数 (cell + trp)

```llvm
; 输入:
define void @process_multi(i32 %cellIdx, i32 %trpIdx, i32 %iter) {
  ; ... 原函数体 ...
}

; 输出:
define void @process_multi(i32 %cellIdx, i32 %trpIdx, i32 %iter) {
jit_entry:
  %dims = alloca [2 x { ptr, i8 }], align 8
  ; dims[0] = {"cell", cellIdx}
  %dim0 = getelementptr ... [2 x { ptr, i8 }]* %dims, i32 0, i32 0
  %dim0_name = getelementptr ... { ptr, i8 }* %dim0, i32 0, i32 0
  store ptr @".str.cell", ptr* %dim0_name
  %dim0_idx = getelementptr ... { ptr, i8 }* %dim0, i32 0, i32 1
  store i8 %cellIdx, ptr* %dim0_idx
  ; dims[1] = {"trp", trpIdx}
  %dim1 = getelementptr ... [2 x { ptr, i8 }]* %dims, i32 0, i32 1
  %dim1_name = getelementptr ... { ptr, i8 }* %dim1, i32 0, i32 0
  store ptr @".str.trp", ptr* %dim1_name
  %dim1_idx = getelementptr ... { ptr, i8 }* %dim1, i32 0, i32 1
  store i8 %trpIdx, ptr* %dim1_idx

  i32 <hash>  ; FNV-1a hash of function name
  %dims_ptr = bitcast ... to ptr
  %jit_result = call ptr @ejit_taskpool_compile_or_get(i32 <hash>, ptr %dims_ptr, i32 2, ptr null)
  %jit_is_null = icmp eq ptr %jit_result, null
  br i1 %jit_is_null, label %jit_fallback, label %jit_dispatch

jit_dispatch:
  call void %jit_result(i32 %cellIdx, i32 %trpIdx, i32 %iter)
  ret void

jit_fallback:
  ; ... 原函数体 ...
  ret void
}
```

---

## 5. 关键数据结构

```cpp
// ejit_period_arr_ind 参数信息
struct PeriodArrIndParam {
    unsigned argIdx;                // 参数在函数参数列表中的索引
    std::string periodName;         // 关联的时间窗名称 (如 "cell", "trp")
    Type* originalType;             // 原始参数类型 (用于 ZExt)
};

// 维度信息汇总
struct DimInfo {
    std::vector<PeriodArrIndParam> params;
    bool hasStaticOnly;             // 仅依赖 static 时间窗 (dimCount = 0)
    unsigned maxDimensions = 4;     // 最大维度限制
};

// dim pairs: [{dimType, instanceId}] per lifecycle dimension
// loaded from per-lifecycle globals, zero allocas after mem2reg
// IR 类型: { i8*, i8 }
```

---

## 6. 错误处理

| 错误场景 | 处理策略 |
|---------|---------|
| 函数中无法找到 jit_result | assert — 这是 Pass 内部逻辑错误 |
| period_arr_ind 参数超过 4 个 | Sema 已在编译前端报错，Pass 不再检查 |
| 原函数有非 entry 块开头 | 不需要特殊处理，fallback 块从原 entry 开始即可 |
| 空函数体 | 仍然插入 Wrapper (仍可能有特化价值) |
| ejit_taskpool_compile_or_get 未声明 | Pass 自动声明外部函数 |

---

## 7. 与其他 Pass 的交互

```
EJitRegisterBitcodePass  →  提取 bitcode 并嵌入
EJitRegisterPeriodPass   →  注册时间窗变量
        ↓
EJitWrapperGenPass       →  插入 Wrapper 代码 (本 Pass)
        ↓
EJitPeriodHandlerPass    →  处理生命周期函数
```

| 依赖项 | 说明 |
|--------|------|
| ejit.metadata (ejit_entry) | 来自 Clang CodeGen，定位需要 wrapper 的函数 |
| ejit_period_arr_ind 参数信息 | 来自函数级 `ejit.metadata`（`!{"ejit_period_arr_ind", !"periodName", i32 argIndex}`） |
| @ejit_taskpool_compile_or_get | ExternalLinkage 声明，由运行时库提供 |
| dimType 加载 | 从 per-lifecycle globals 加载，mem2reg 后零栈分配 |

---

## 8. 测试策略

### 8.1 Lit 测试

```llvm
; test_wrapper_gen.ll
; RUN: opt -passes=ejit-wrapper-gen -S %s | FileCheck %s

; 测试场景:
; TEST 1: 仅 static 依赖 — dims=NULL, count=0
;   CHECK: call ptr @ejit_taskpool_compile_or_get(i32 <hash>, ptr null, i32 0, ptr null)
;
; TEST 2: 单维度参数 — dims[1] 构建
;   CHECK: alloca [1 x { ptr, i8 }]
;   CHECK: call ptr @ejit_taskpool_compile_or_get(i32 <hash>, ptr %dims_ptr, i32 1, ptr null)
;
; TEST 3: 多维度参数 — dims[2] 构建
;   CHECK: alloca [2 x { ptr, i8 }]
;   CHECK: call ptr @ejit_taskpool_compile_or_get(i32 <hash>, ptr %dims_ptr, i32 2, ptr null)
;
; TEST 4: JIT 成功路径 — 调用 pfn
;   CHECK: jit_dispatch:
;   CHECK: call void %jit_result
;
; TEST 5: JIT 失败路径 — fallback
;   CHECK: jit_fallback:
;   CHECK: icmp eq ptr %jit_result, null
;   CHECK: br i1 %jit_is_null, label %jit_fallback, label %jit_dispatch
;
; TEST 6: 返回值处理 — 非 void 函数
;   CHECK: %ret = call i32 %jit_result(i32 %arg)
;   CHECK: ret i32 %ret
```

### 8.2 验证点

| 验证项 | 方法 |
|--------|------|
| dimType 加载正确 | FileCheck 匹配 alloca + store + taskpool call |
| Cache key 匹配 | 运行时测试验证 |
| Fallback 路径可达 | FileCheck 匹配 CFG 分支 |
| 原函数体完整性 | 与原 IR 比较 (jit_fallback 块内容) |
| 参数传递完整性 | pfn 调用参数列表匹配原始参数 |
| 无 ejit_entry 函数的 Module | FileCheck: not check |

---

## 9. 实施注意事项

1. **SplitFunction 的 PHI 节点处理**: 如果原函数的 entry 块有 PHI 节点，移动到 fallback 块后 PHI 节点的前驱块需要更新。但 entry 块通常无 PHI 节点（entry 块无前驱）。

2. **dimType global 加载**: 每个 period 对应一个 `@__ejit_dimtype_<name>` global，wrapper 加载其值构造 dim pairs。mem2reg 优化后 allocas 提升为寄存器。

3. **类型统一 (ZExt/Trunc)**: period_arr_ind 参数统一转为 i32 后存储到 dim pair 的 instanceId 字段。

4. **jit_entry 块拆分**: jit_entry 块中的 wrapper 逻辑是指令序列，正确建立 Control Flow: `jit_entry → (pfn != null ? jit_dispatch : jit_fallback)`。

5. **返回值**: 若原始函数有返回值，注意 pfn 调用返回值的正确传递。pfn 的调用约定必须与原函数一致。

6. **debug info**: 函数拆分后，fallback 块的 debug location 可保留原函数的 debug info，jit_entry 和 jit_dispatch 使用新的 debug location 或留空。

---

## 10. Inline Cache 模式（LEVER B：frame-less hit path）

开启 `-ejit-inline-cache`（默认 off）后，wrapper 的命中路径变为 **frame-less**：
命中时直接尾调用缓存的特化函数，**无栈帧、无 `compile_or_get`、无 `release_read`**。
慢路径被抽取到每函数独立的 `noinline MissFn`（`<name>_miss`），自带栈帧；miss 罕见，尾调用开销可忽略。
完整设计（cache 数据结构、fill 线性化、安全模型、AOT↔runtime 契约）见 `EJIT_ICACHE_MULTIVERSION.md`。

### 10.1 命中路径（F，frame-less）

```llvm
define i32 @funcA(args...) {
jit_entry:                                       ; frame-less entry
  slot = GEP @__ejit_icache_fn_funcA, 0, i0, i1    ; [D]^numDims 多维索引（纯移位）
  p = load slot, align 8                           ; 普通 ldr（每核私有槽，无需 atomic）
  hit = expect(p != null, true)
  br hit, label %jit_icache_dispatch, label %jit_miss
jit_icache_dispatch:                             ; 命中：尾调用特化（无 release_read）
  tail call void %p(args...)
  ret
jit_miss:                                        ; miss：尾调用 MissFn
  tail call void @funcA_miss(args...)
  ret
}

define internal noinline void @funcA_miss(args...) {  ; 慢路径（自带栈帧；继承 F 的 target-cpu/features/tune-cpu + section）
  ; funcidx 守卫 -> compile_or_get -> dispatch (call spec + release_read) / fallback (AOT body)
}
```

- `@__ejit_icache_fn_<name>`：每函数私有 `[D]^numDims` 数组（`D = EJIT_ICACHE_DIM_SIZE = 8`，power-of-2），按 `ejit_dim` 参数值索引；numDims=0 退化为标量（= v2 单态）。
- 命中路径**不调任何 runtime 函数**：直接 GEP + load + null-check + 尾调用。
- `llvm.expect(hit, true)` 提示命中分支，使 regalloc / shrink-wrapping 对所有维度（含 0 维）保持 frame-less。
- **musttail 仅在 `!EJitWrapperTiming` 时设置**：hit/miss 两条尾调用标 `TCK_MustTail`，降低成 `br`（复用调用方栈帧），故 wrapper 无栈帧。`-ejit-wrapper-timing` 开启时 hit 路径在 call 与 ret 之间插 `trace_now`/`trace_wrapper`，违反 musttail 规则，改用普通带帧 call + trace + ret（hit 路径不再 frame-less，但 IR 合法、功能不变；miss 路径仍 musttail）。
- **MissFn 必须继承 F 的 target 属性**：`Function::Create` 新建的 MissFn 只继承 module 默认的 `uwtable`/`frame-pointer`，丢 F 的 `target-cpu`/`target-features`/`tune-cpu`。这会让 `TTI::areInlineCompatible` 判 MissFn 不是 helper 的 subtarget 超集 -> CGSCC InlinerPass 对所有 helper 报 `conflicting attributes` -> MissFn 零内联（AOT fallback 体积异常缩水）。修复：`MissFn->setAttributes(F->getAttributes())` 复制 F 全部属性，再 `addFnAttr(NoInline)`；同时 `setSection(F->getSection())` 让 MissFn 与 F 同段。`!ejit.metadata` 是 metadata（不在 `AttributeList` 里），不会被复制——MissFn 不应被当 ejit_entry 处理。
- 跨函数 splice 修正：原函数体搬到 MissFn 后，把 `F->getArg(i)` 引用重映射到 `MissFn->getArg(i)`，否则跨函数参数引用会让 SelectionDAG 崩溃。
- 幂等：`isAlreadyWrapped` 识别 entry 块中对 `@__ejit_icache_fn_<name>` 的 `LoadInst`/`GetElementPtrInst`（以及 `@__ejit_funcidx_<name>`），重复运行不会复制探针。

### 10.2 性能（最新实测，`-Os`，aarch64_be，D=16）

| numDims | hit 路径实测 cycle |
|---|---|
| 0 | 4 |
| 1 | 5 |
| 2 | 6 |
| 3 | 8 |
| 4 | 9 |

命中路径 = 探针（GEP 移位索引 + load + null-check）+ 尾调用 `br`；无栈帧、无 call、无 `release_read`、无跨核 RMW。对比 icache off / 首次 miss 的 taskpool 路径（`compile_or_get` + `readers_` RMW + 桶扫描 + `release_read`），命中路径压到 4–9 cycle。2–4 维的实测 cycle 比指令数（4,5,7,9,11）还少，因为 `sxtw` 与 `adrp` 延迟重叠（ILP）。

> 注：上述 frame-less / cycle 数据前提是 `!EJitWrapperTiming`。开启 `-ejit-wrapper-timing` 时 hit 路径退化为带帧 plain call + trace + ret（见 §10.1 musttail gating），cycle 不适用。



### 10.3 约束与安全

- 维度数 ≤ `EJIT_ICACHE_MAX_DIMS`(=4)，超限编译报错（`emitError`）。
- 每个 `ejit_dim` 参数值 < `D`，无运行时边界检查（契约）。
- 安全：生产 NO_RECLAIM（JIT 代码不释放）下缓存指针永不悬空；若 runtime 接了 code releaser，安全门（`icacheReclamationSafe_`）自动关闭缓存，退化为 taskpool 路径。

---

*文档版本: 1.2*
*创建日期: 2026-04-26*
*最后更新: 2026-07-25*
