# EJitGuardHandlerPass 设计文档

**版本**: 1.0
**日期**: 2026-04-26
**关联**: SPEC4.md, PLAN4.md
**类型**: AOT Module Pass
**顺序**: AOT Pipeline 第 4 步

---

## 1. 概述

EJitGuardHandlerPass 负责处理 `ejit_guard` (lifecycle) 属性标记的函数。在这些函数的入口插入 `ejit_deactivate(periodName, idx)` 调用，在所有出口点插入 `ejit_activate(periodName, idx)` 调用。这确保在修改时间窗数据期间，相关时间窗处于 deactive 状态，防止其他线程读到不一致的数据或触发对旧值的 JIT 编译。

> **移除说明**: 早期设计在函数入口/出口插入数组级的 `ejit_deactivate_array` / `ejit_activate_array`（携带数组指针参数）。该数组级生命周期激活已被移除，因为产品不需要它；异步 / 共享 taskpool 的热路径中激活状态以 `(lifecycle, instance)` 为键，不携带数组指针维度。PASS4 现在仅发出 name 级别的 `ejit_deactivate(periodName, idx)` / `ejit_activate(periodName, idx)`。若同一 period name 关联多个数组，激活对该 period 实例整体生效。

### 1.1 核心职责

- 定位所有带 `!{"ejit_guard", !"periodName"}` metadata 的函数
- 识别函数参数中对应 `ejit_dim(periodName)` 的参数
- 在函数入口（第一条指令之前）插入 `ejit_deactivate(periodName, idx)` 调用
- 在函数所有出口点（return 指令之前）插入 `ejit_activate(periodName, idx)` 调用
- 支持一个函数标记多个 `ejit_guard`（多时间窗管理）
- 处理 early return 场景 — 所有 return 指令前都需插入

### 1.2 设计约束

| 约束项 | 说明 |
|--------|------|
| 多时间窗 | 单函数可标记多个 `ejit_guard`，需按序配对激活/去激活 |
| 异常处理 | C 语言无异常，不考虑 exception handling |
| 多出口 | C 函数可有多处 return，均需插入 activate 调用 |
| ejit_guard 必须与 ejit_dim 配对 | Sema 已检查，Pass 不做验证 |
| static 时间窗 | `ejit_group(static)` 永远激活，不参与 activate/deactivate |

---

## 2. 输入 IR 格式

### 2.1 ejit_guard 函数 Metadata

```llvm
; 单时间窗生命周期函数
define void @update_cell(i32 %cellIdx) #0 {
  ; ...
}
; !ejit.metadata = distinct !{!0, !1}
; !0 = !{!"ejit_guard", !"cell"}
; !1 = !{!"ejit_dim", !"cell", i32 0}    ; 参数 0 对应 cell 的索引
;      ← 此 metadata 由 Clang CodeGen 在函数级别附加

; 多时间窗生命周期函数
define void @update_both(i32 %cellIdx, i32 %trpIdx) #0 {
  ; ...
}
; !ejit.metadata = distinct !{!0, !1, !2, !3}
; !0 = !{!"ejit_guard", !"cell"}
; !1 = !{!"ejit_guard", !"trp"}
; !2 = !{!"ejit_dim", !"cell", i32 0}
; !3 = !{!"ejit_dim", !"trp", i32 1}
```

### 2.2 Metadata 格式说明

`ejit_dim` metadata 在函数级别编码为：
```llvm
!{!"ejit_dim", !"periodName", i32 argIndex}
```

此 metadata 同时服务于 `EJitWrapperGenPass` 和 `EJitGuardHandlerPass`。

---

## 3. 核心算法

### 3.1 主流程

```
输入: Module M (含 ejit_guard 函数)
输出: Module M (函数入口 + 出口插入 activate/deactivate 调用)

步骤:
1. CollectLifecycleFunctions(M) → 收集所有 ejit_guard 函数及其 metadata
2. ForEach lifecycleFunc:
     ParseLifecycleInfo(funcMeta) → 解析 periodName → argIdx 映射
3. InsertDeactivateAtEntry(func, lifecycleInfo) → 入口插入 deactivate
4. InsertActivateAtExits(func, lifecycleInfo) → 所有 return 前插入 activate
```

### 3.2 详细伪代码

```cpp
PreservedAnalyses EJitGuardHandlerPass::run(Module& M, ModuleAnalysisManager& AM) {
    struct LifecycleInfo {
        std::string periodName;
        unsigned argIdx;         // ejit_dim 参数索引
    };
    std::vector<std::pair<Function*, std::vector<LifecycleInfo>>> lcFuncs;

    for (Function& F : M.functions()) {
        MDNode* MD = F.getMetadata("ejit.metadata");
        if (!MD) continue;

        std::vector<LifecycleInfo> lcInfo;
        bool isLifecycle = false;

        for (const MDOperand& Op : MD->operands()) {
            MDNode* Entry = cast<MDNode>(Op);
            StringRef tag = cast<MDString>(Entry->getOperand(0))->getString();

            if (tag == "ejit_guard") {
                isLifecycle = true;
                std::string periodName = cast<MDString>(
                    Entry->getOperand(1))->getString().str();

                // 查找对应的 ejit_dim 参数
                int argIdx = findPeriodArrIndArg(MD, periodName);
                if (argIdx >= 0) {
                    lcInfo.push_back({periodName, (unsigned)argIdx});
                }
            }
        }

        if (isLifecycle && !lcInfo.empty()) {
            lcFuncs.push_back({&F, lcInfo});
        }
    }

    for (auto& [F, lcInfoList] : lcFuncs) {
        insertDeactivateAtEntry(F, lcInfoList);
        insertActivateAtExits(F, lcInfoList);
    }

    return lcFuncs.empty() ? PreservedAnalyses::all() : PreservedAnalyses::none();
}
```

### 3.3 入口 Deactivate 插入

```cpp
void insertDeactivateAtEntry(Function* F,
                              const std::vector<LifecycleInfo>& lcInfoList) {
    LLVMContext& Ctx = F->getContext();
    BasicBlock& entryBB = F->getEntryBlock();

    // 找到 entry 块的第一条非 alloca 指令
    Instruction* firstNonAlloca = entryBB.getFirstNonPHI();
    while (isa<AllocaInst>(firstNonAlloca) && firstNonAlloca != nullptr) {
        firstNonAlloca = firstNonAlloca->getNextNode();
    }

    IRBuilder<> Builder(Ctx);
    if (firstNonAlloca) {
        Builder.SetInsertPoint(firstNonAlloca);
    } else {
        Builder.SetInsertPoint(&entryBB);
    }

    // 声明运行时函数
    Function* deactivateFn = getOrDeclareDeactivate(M);

    // 为每个 lc 时间窗插入 deactivate 调用
    // 调用顺序: metadata 中出现顺序 (与 activate 配对)
    for (auto& lcInfo : lcInfoList) {
        // 参数: periodName 字符串 + cellIdx (name + index，无数组指针维度)
        Value* periodNameStr = Builder.CreateGlobalStringPtr(lcInfo.periodName);
        Value* argVal = F->getArg(lcInfo.argIdx);

        Value* cellIdx = Builder.CreateZExtOrTrunc(argVal, Type::getInt32Ty(Ctx));

        Builder.CreateCall(deactivateFn, {periodNameStr, cellIdx});
    }
}
```

### 3.4 出口 Activate 插入

```cpp
void insertActivateAtExits(Function* F,
                            const std::vector<LifecycleInfo>& lcInfoList) {
    LLVMContext& Ctx = F->getContext();
    Function* activateFn = getOrDeclareActivate(M);

    // 收集所有 return 指令
    std::vector<ReturnInst*> returnInsts;
    for (BasicBlock& BB : *F) {
        if (ReturnInst* RI = dyn_cast<ReturnInst>(BB.getTerminator())) {
            returnInsts.push_back(RI);
        }
    }

    // 在每个 return 指令之前插入 activate 调用
    // 同序插入 activate (与 deactivate 相同顺序)
    // 注: 时间窗之间无嵌套依赖关系，同序和逆序语义等价，同序更简单
    for (ReturnInst* RI : returnInsts) {
        IRBuilder<> Builder(Ctx);
        Builder.SetInsertPoint(RI); // 插入点: return 之前

        for (auto& lcInfo : lcInfoList) {
            Value* periodNameStr = Builder.CreateGlobalStringPtr(lcInfo.periodName);
            Value* argVal = F->getArg(lcInfo.argIdx);
            Value* cellIdx = Builder.CreateZExtOrTrunc(argVal, Type::getInt32Ty(Ctx));

            Builder.CreateCall(activateFn, {periodNameStr, cellIdx});
        }
    }
}
```

### 3.5 激活粒度（name + index only）

激活状态仅以 periodName + cellIdx 为键，不再需要查找对应的全局数组指针。若同一 periodName 关联多个 `ejit_group` 数组，`ejit_activate(periodName, idx)` 对该 period 实例整体生效（name 级别）。因此 PASS4 不再需要 `getArrayPtrForPeriod` 之类的数组指针推断。

---

## 4. 输出 IR 变化

### 4.1 单时间窗函数

```llvm
; 输入:
define void @update_cell(i32 %cellIdx) {
entry:
  ; ... 函数体 ...
  ret void
}

; 输出:
define void @update_cell(i32 %cellIdx) {
entry:
  ; === 入口: deactivate ===
  call void @ejit_deactivate(ptr @".str.cell", i32 %cellIdx)
  ; === 原函数体 ===
  ; ...
  ; === 出口: activate ===
  call void @ejit_activate(ptr @".str.cell", i32 %cellIdx)
  ret void
}
```

### 4.2 多时间窗函数 (多出口)

```llvm
; 输入:
define void @update_both(i32 %cellIdx, i32 %trpIdx) {
entry:
  %cmp = icmp slt i32 %cellIdx, 0
  br i1 %cmp, label %early_exit, label %body

body:
  ; ... 函数体 ...
  ret void

early_exit:
  ret void
}

; 输出:
define void @update_both(i32 %cellIdx, i32 %trpIdx) {
entry:
  ; deactivate 按 metadata 出现顺序 (cell 先, trp 后)
  call void @ejit_deactivate(ptr @".str.cell", i32 %cellIdx)
  call void @ejit_deactivate(ptr @".str.trp",  i32 %trpIdx)

  %cmp = icmp slt i32 %cellIdx, 0
  br i1 %cmp, label %early_exit, label %body

body:
  ; ... 函数体 ...
  ; activate 同序 (cell 先, trp 后，与 deactivate 顺序一致)
  call void @ejit_activate(ptr @".str.cell", i32 %cellIdx)
  call void @ejit_activate(ptr @".str.trp",  i32 %trpIdx)
  ret void

early_exit:
  ; 所有 return 前均插入 activate (同样同序)
  call void @ejit_activate(ptr @".str.cell", i32 %cellIdx)
  call void @ejit_activate(ptr @".str.trp",  i32 %trpIdx)
  ret void
}
```

### 4.3 name 级激活（无数组指针维度）

```llvm
; 激活仅以 periodName + cellIdx 为键，无论该 periodName 关联一个还是多个数组
; 都使用相同的 name 级调用（对该 period 实例整体生效）
call void @ejit_deactivate(ptr @".str.custom_p", i32 %idx)
; ...
call void @ejit_activate(ptr @".str.custom_p", i32 %idx)
```

---

## 5. 关键数据结构

```cpp
// 生命周期函数信息
struct LifecycleFuncInfo {
    Function* F;
    std::vector<PeriodAssociation> associations;
};

// 时间窗 lc → period_arr_ind 关联
struct PeriodAssociation {
    std::string periodName;      // ejit_guard 参数
    unsigned paramIdx;           // 对应的 ejit_dim 参数索引
};

// 运行时 API 声明 (name + index，无数组指针维度)
// void ejit_deactivate(const char* periodName, uint8_t cellIdx);
// void ejit_activate(const char* periodName, uint8_t cellIdx);
```

---

## 6. 错误处理

| 错误场景 | 处理策略 |
|---------|---------|
| ejit_guard 无对应 ejit_dim | 跳过（Sema 已处理） |
| alloca 干扰入口点定位 | 跳过 alloca 指令，在第一条非 alloca 后插入 |
| 无 return 指令的函数 | 不应发生（well-formed IR 至少有一个 return / unreachable） |
| unreachable 指令 | 不插入（unreachable 不是正常出口） |

---

## 7. 与其他 Pass 的交互

```
EJitRegisterBitcodePass  →  提取 bitcode
EJitRegisterGroupPass   →  注册时间窗变量
EJitWrapperGenPass       →  生成 Wrapper
        ↓
EJitGuardHandlerPass    →  处理生命周期函数 (本 Pass - 最后一步)
```

| 依赖项 | 说明 |
|--------|------|
| ejit.metadata | Clang CodeGen 生成的函数 + 全局变量 metadata |
| ejit_deactivate / ejit_activate | 运行时库提供的外部符号 (name + index) |
| 全局数组 IR 变量 | 从 Module 中查找 (不需要前序 Pass 的预处理) |

---

## 8. 测试策略

### 8.1 Lit 测试

```llvm
; test_period_handler.ll
; RUN: opt -passes=ejit-guard-handler -S %s | FileCheck %s

; 测试场景:
; TEST 1: 单 ejit_guard + 单出口
;   CHECK: call void @ejit_deactivate
;   CHECK: ret void
;   CHECK: call void @ejit_activate
;
; TEST 2: 多 ejit_guard (cell + trp) + 多出口
;   CHECK-DAG: call void @ejit_deactivate(ptr @".str.cell"
;   CHECK-DAG: call void @ejit_deactivate(ptr @".str.trp"
;   CHECK-DAG: call void @ejit_activate(ptr @".str.cell"
;   CHECK-DAG: call void @ejit_activate(ptr @".str.trp"
;   验证同序: activate 顺序与 deactivate 一致 (cell 先 trp 后)
;
; TEST 3: 无 ejit_guard 函数的 Module
;   CHECK-NOT: call void @ejit_deactivate
;
; TEST 4: 所有 return 前均有 activate
;   计数 check: activate 出现次数 = return 指令数 × period 数
```

### 8.2 验证点

| 验证项 | 方法 |
|--------|------|
| deactivate/activate 配对 | 计数检查 |
| 逆序配对 | 检查 activate 的插入顺序 |
| 多出口完整性 | 检查所有 return 前均有 activate |
| 无 lifecycle 函数的 Module | 不做修改 |

---

## 9. 实施注意事项

1. **Deactivate/Activate 顺序**: 对于多时间窗函数，deactivate 和 activate 均按 metadata 出现顺序执行（同序）。EmbeddedJIT 的时间窗之间无嵌套依赖关系（不像 mutex），同序语义等价且实现更简单。

2. **Return 指令定位**: 使用 `BasicBlock::getTerminator()` 获取 return / unreachable 指令，`Builder.SetInsertPoint(RI)` 在 return 之前插入 activate 调用。

3. **空函数**: 仅有 `entry:` 和 `ret void` 的空函数中，deactivate 在 entry 开头插入，activate 在 ret 前插入。

4. **noreturn 函数**: 如果函数有 unreachable terminator（如调用 `abort()` 后），不插入 activate。因为 deactivate 永久生效是没有意义的，应该让 Sema 警告这种情况。

5. **运行时接口选择**: 使用 name 级接口 `ejit_deactivate(periodName, cellIdx)` / `ejit_activate(periodName, cellIdx)`。激活状态仅以 `(lifecycle, instance)` 为键，不携带数组指针维度；早期的数组级接口 `ejit_activate_array` / `ejit_deactivate_array` 已移除。

---

*文档版本: 1.0*
*创建日期: 2026-04-26*
