# LLVM 15.0.4 → 21.1.8 编译器升级差异分析与质量加固建议

> 分析日期：2026-07-10
> 版本跨度：15.0.4 (2022-09) → 21.1.8 (2026-01)，跨越 6 个大版本
> 提交数量：~114,000 commits
> 变更文件：~8,400 files (+1.81M / -0.74M)

---

## 目录

1. [版本发布历史](#1-版本发布历史)
2. [重大 Breaking Changes 逐版本分析](#2-重大-breaking-changes-逐版本分析)
3. [C/C++ 标准支持变化](#3-cc-标准支持变化)
4. [ABI 不兼容变更汇总](#4-abi-不兼容变更汇总)
5. [编译器优化变化](#5-编译器优化变化)
6. [安全加固特性演进](#6-安全加固特性演进)
7. [目标架构变更 (AArch64/ARM/x86)](#7-目标架构变更)
8. [LLD 链接器变化](#8-lld-链接器变化)
9. [libc++ 变化](#9-libc-变化)
10. [clang-tidy 变化](#10-clang-tidy-变化)
11. [Sanitizers 变化](#11-sanitizers-变化)
12. [质量加固建议](#12-质量加固建议)
13. [升级迁移检查清单](#13-升级迁移检查清单)

---

## 1. 版本发布历史

| 版本 | 发布日期 | 关键特性 |
|------|---------|---------|
| **LLVM 15.0.4** | 2022-09 | 基线版本 |
| **LLVM 16.0.0** | 2023-03-18 | C++17 默认标准；新 pass manager 强制；memory() 属性 |
| **LLVM 17.0.0** | 2023-09-05 | 不透明指针强制；void* 解引用错误；print<all> |
| **LLVM 18.1.0** | 2024-03-05 | 模板名称重整；__int128 ABI 修复；.pch 默认扩展名 |
| **LLVM 19.1.0** | 2024-09-17 | PAuth/GCS；SVE 变为 ARMv9.0 可选；C++17 完成 |
| **LLVM 20.1.0** | 2025-03-04 | TBAA 默认启用；-fwrapv-pointer；le32/le64 移除 |
| **LLVM 21.1.8** | 2026-01-15 | Lifetime DSE；coroutine ABI 稳定化；break/continue 严格化 |

---

## 2. 重大 Breaking Changes 逐版本分析

### 2.1 LLVM 16.0.0 (15 → 16)

#### C/C++ 语言层面
| 变更 | 影响描述 | 应对措施 |
|------|---------|---------|
| **默认 C++ 标准改为 gnu++17** | 原为 gnu++14，可能因 C++17 新规则导致编译差异 | 显式指定 `-std=gnu++14` 恢复旧行为 |
| **-Wimplicit-function-declaration 升级为错误** | C99/C11/C17 中隐式函数声明变为错误 | 添加函数声明或使用 `-Wno-error=implicit-function-declaration` |
| **-Wimplicit-int 升级为错误** | C 中省略类型说明时产生错误 | 显式添加类型说明 |
| **-Wincompatible-function-pointer-types 升级为错误** | 函数指针类型不兼容变为错误 | `-Wno-error=incompatible-function-pointer-types` |
| **void* 间接访问 (C++)** | C++ 中 `*void_ptr` 变为默认错误 (可通过 `-Wno-error=void-ptr-dereference` 降级) | 规范代码，先转型再解引用 |
| **元素类型对齐检查** | 禁止对齐大于元素大小的数组类型 | 修复数组定义 |

#### C++ ABI 变化
| 变更 | 影响 | 应对 |
|------|------|------|
| packed struct 中非 POD 成员对齐对齐 | 对齐 GCC 行为，非 POD 成员不再 packed | `-fclang-abi-compat=15.0` |
| POD 分类（含 defaulted 特殊成员） | 匹配 GCC 的 Itanium ABI 分类 | `-fclang-abi-compat=15.0` |

#### 被移除的特性
- 旧 pass manager 编译标志：`-fexperimental-new-pass-manager` / `-fno-legacy-pass-manager` **（完全移除）**
- `fneg` 常量表达式
- ARMv2/ARMv2A/ARMv3/ARMv3M 目标
- Resource directory 改为仅用 major 版本号（如 `$prefix/lib/clang/16/`）

#### LLVM IR 变更
- `readnone`/`readonly`/`writeonly`/`argmemonly`/`inaccessiblememonly` 函数属性合并为 `memory()` 统一属性
- 引入 Target Extension Types

#### LLD 16 ELF 变更
- 输入段初始化和重定位扫描并行化，链接速度大幅提升
- Zstd 压缩调试信息支持（`--compress-debug-sections=zstd`）
- `--no-undefined-version` 变为默认行为（version script 中无匹配符号时报错）
- Android 特定：AArch64/PPC32/PPC64 的 initial-exec TLS 设置 DT_STATIC_TLS
- 移除 `r` 模式下定义 `__global_pointer$` 和 `_TLS_MODULE_BASE_`

---

### 2.2 LLVM 17.0.0 (16 → 17)

#### C/C++ 语言层面
| 变更 | 影响 |
|------|------|
| asm goto 间接边拆分 | 内联汇编中相同 label 的两个输入可能不再相等 |
| `__builtin_object_size` 在柔性数组成员场景中行为变化 | FAM 中 designated initializer 的 sizeof 影响结果 |
| void* 解引用（C++）从 warning-as-error → error | 旧 `-Wvoid-ptr-dereference` 仅用于 C 中降级 warning |

#### C++ 语言
| 变更 | 影响 |
|------|------|
| `std::experimental::coroutine_traits` 不再被搜索 | 必须使用 `std::coroutine_traits` |
| `static_assert(false)` 可在模板定义中合法（CWG2518） | 对模板库有影响 |
| `final` 类型的 `dynamic_cast` 优化 | 直接比较 vtable 指针（可以通过 `-fno-assume-unique-vtables` 禁用） |
| `std::forward_like` 在 -O0 下变为内置函数 | 改进 -O0 代码生成 |

#### ABI 变化
- 修复特化成员函数的 trivial copyable 判定（#62555）

#### LLVM IR 变更
- **不透明指针（Opaque Pointers）完全强制**，`-opaque-pointers` 选项移除
- `nofpclass` 属性引入
- `llvm.ldexp`/`llvm.frexp` 内建函数
- `select` 常量表达式移除

#### AArch64 后端
- 支持 FEAT_GCS（Guarded Control Stacks）、FEAT_CHK、FEAT_ATS1A 汇编
- `preserve_all` 调用约定
- `.arch` 指令修复

#### LLD 17 变更
- 线程数上限默认 16 核
- `--remap-inputs=` 选项
- `--lto=` 和 `--lto-CGO[0-3]` 支持 unified LTO
- `SHF_MERGE`/`--build-id=fast`/`--icf=` 切换至 64-bit xxh3 哈希
- AArch64 DT_AARCH64_MEMTAG_* 动态标签
- AArch64 short range thunk 实现
- x86-64 大代码段与代码段分离放置

---

### 2.3 LLVM 18.1.0 (17 → 18)

#### C/C++ 语言层面
| 变更 | 说明 |
|------|------|
| **模板化 operator== 反转修复** | C++20 中之前接受的代码变为歧义；可通过 `-Wambiguous-reversed-operator` 告警接受 |
| **PCH 扩展名改为 .pch** | 从 `.gch` 改为 `.pch`；使用 `-include a.h` 时 `.gch` 文件会被忽略 |
| **`__has_cpp_attribute`/`__has_c_attribute` 修复** | 部分 C++11 属性返回值改变 |
| **operator!= 查找修复** | 修复 P2468 相关问题 |

#### C++ 语言
- **函数模板名称重整（Name Mangling）变化**：当满足以下任一条件时代码的 mangled name 会改变：
  - 模板参数依赖前一个参数（`template<typename T, T V>`）
  - 函数有任何约束（constrained template parameter 或 requires-clause）
  - 模板参数列表包含推导类型（`auto`/`decltype(auto)`/推导的类模板特化）
  - 模板模板参数具有不同参数列表
  - **恢复旧行为**：`-fclang-abi-compat=17`
- `-Wenum-constexpr-conversion` 在系统头文件/宏中默认启用（Clang 19 将升级为错误）
- `ClassScopeFunctionSpecializationDecl` AST 节点移除
- C++20 named modules：所有依赖模块必须通过命令行指定（移除了硬编码 import 路径）
- `-fdelayed-template-parsing` 在 MSVC 目标的 C++20 下不再默认

#### ABI 变化
- **x86-64 SystemV ABI**：`__int128` 参数不再拆分为寄存器+栈（完全按寄存器传递）

#### LLVM IR 变更
- 大量常量表达式变体被移除：`and`/`or`/`lshr`/`ashr`/`zext`/`sext`/`fptrunc`/`fpext`/`fptoui`/`fptosi`/`uitofp`/`sitofp`

#### LLD 18 变更
- FatLTO 对象支持（`--fat-lto-objects`）
- `-Bsymbolic-non-weak` 选项
- `--lto-validate-all-vtables-have-type-infos` 防止不安全全程序 devirtualization
- 使用 `cdsort` 算法（替代 `hfsort`）排序调用图剖面的输入段
- common-page-size 允许大于系统 page-size

#### AArch64 后端
- 支持 Cortex-A520/A720/X4 CPU
- Neoverse-N2 修正为 Armv9.0a（+crypto 需要显式启用）
- 2023 架构扩展汇编/反汇编支持

---

### 2.4 LLVM 19.1.0 (18 → 19)

#### C/C++ 语言层面
- `GCC_INSTALL_PREFIX` 废弃：导致致命的配置错误
- C99 起支持 raw string literals（`-std=gnuXY` 模式）
- C++ 中 `'!s'` 修饰字符串字面量比较修复

#### C++ 语言
| 变更 | 说明 |
|------|------|
| **模板参数自阴影诊断** | `template<class T> void T();` 现在报错 |
| **-frelaxed-template-template-args 默认启用** | flag 已废弃，负拼写可兼容旧版本 |
| **pointer-to-member 限定** | `decltype(&(foo::bar))` 不再形成 pointer-to-member |
| **依赖类型一元运算符语义分析** | 修复了 libstdc++ 14.1.0 兼容性（14.1.1+ 已修复） |
| **C++17 特性声称"完成"** | 所有 C++17 核心特性已实现 |

#### ABI 变化
- **Microsoft name mangling 修复**：静态局部变量线程安全初始化、lifetime extended temporary、auto NTTP 指针类型 (MSVC 1920+)
- **Microsoft 调用约定修复**：template constructor 类返回、deleted copy assignment 返回
- **AArch64 FMV ifunc 全局别名移除**：与 GlobalOpt 交互不良（#96197）

#### 指针验证（PAuth）支持（关键新特性）
- ELF pointer authentication relocations 完全支持
- `GNU_PROPERTY_AARCH64_FEATURE_PAUTH` note 生成
- `llvm.ptrauth.auth`/`llvm.ptrauth.resign` 内建函数
- `-z gcs`/`-z gcs-report` Guarded Control Stack 支持

#### LLVM IR 变更
- Memory Model Relaxation Annotations (MMRAs)
- `nusw`/`nuw` flags 添加到 `getelementptr`
- `icmp`/`fcmp`/`shl` 常量表达式变体移除
- Debug 内建函数 → Debug records（debug info 基础设施重大变更）
- 实验性 vector intrinsic 重命名（移除 `experimental.` 前缀）

#### AArch64 后端
- 支持 Cortex-R82AE/A78AE/A520AE/A720AE/A725/X925/Neoverse-N3/V3/V3AE CPU
- **SVE/SVE2 变为 ARMv9.0 可选特性**（`+v9a` 不再隐含 `+sve`/`+sve2`，但现有 v9.0+ CPU 仍然默认启用）
- `-mbranch-protection=standard` 默认启用 FEAT_PAuth_LR
- `lld --build-id` 默认改为 20 字节 SHA1

#### LLD 19 变更
- 实验性 CREL 重定位支持（**无版本兼容保证**）
- EI_OSABI 从输入目标文件推断
- `--compress-sections` 选项
- `GNU_PROPERTY_AARCH64_FEATURE_PAUTH` 支持
- `--debug-names` 选项
- `--enable-non-contiguous-regions` 选项

---

### 2.5 LLVM 20.1.0 (19 → 20)

#### **重大 Breakage 警告级别**

| 变更 | 影响评级 | 应对 |
|------|---------|------|
| **TBAA 标签默认启用** | ⚠️ **高** | 检查 strict aliasing 违规代码；使用 `-fno-pointer-tbaa` 禁用 |
| **指针溢出 UB 更积极优化** | ⚠️ **高** | `ptr+offset < ptr` 优化为 false；使用 `-fwrapv-pointer` 或 `-fno-strict-overflow` |
| **-fwrapv 不再影响指针溢出** | ⚠️ **中** | 需要 `-fwrapv-pointer` 显式指定 |
| **le32/le64 目标移除** | ⚠️ **中** | 移动到新目标或更新配置 |
| **clang-rename 移除** | ⚠️ **低** | 使用其他重构工具 |
| **RenderScript 支持移除** | ⚠️ **低** | 迁移至 Vulkan |
| **enum constexpr 转换诊断不可压制** | ⚠️ **中** | enum 值超出范围的代码需要修复 |
| **多余的 template header 默认报错** | ⚠️ **中** | 使用 `-Wno-error=extraneous-template-head` |
| **`_Complex _BitInt` 被拒绝** | ⚠️ **低** | 不使用即可 |

#### C++ 语言
- `__is_nullptr` 类型特性已移除
- `__is_referenceable` 已废弃（Clang 21 移除）
- 字符串字面量常量比较修复（CWG2765）
- `-Wdeprecated-literal-operator` 默认启用
- `[[clang::lifetimebound]]` 在 void 返回函数或类型上现在报错

#### ABI 变化
- **Itanium 构造 vtable 名称修复**（可通过 `-fclang-abi-compat=19` 恢复旧行为）
- **成员式友元函数模板 mangled 为成员**（可通过 `-fclang-abi-compat=19` 恢复）
- Microsoft 返回类型 name mangling 修复

#### LLVM IR 变更
（Release Notes 模板文件，实际变更见具体提交）

---

### 2.6 LLVM 21.1.8 (20 → 21)

#### **重大 Breakage 警告级别**

| 变更 | 影响评级 | 应对 |
|------|---------|------|
| **Lifetime DSE 积极优化死对象存储** | ⚠️ **高** | `-fno-lifetime-dse` 禁用 |
| **break/continue 在循环条件/增量部分严格化** | ⚠️ **中** | GCC-compatible 行为变更 |
| **`__has_feature(modules)` 语义变更** | ⚠️ **中** | 不再在 `-std=c++20` 下为 true（需 `-fmodules`） |
| **`_BitInt(N)` 模板参数推导为 size_t** | ⚠️ **中** | 匹配数组大小推导行为 |
| **嵌套局部类作用域检查** | ⚠️ **低** | 跨 block scope 定义的嵌套类被拒绝 |
| **`__is_referenceable` 移除** | ⚠️ **低** | 使用等价替代 |
| **coroutine resum/destroy ABI 使用 C calling convention** | ⚠️ **低(i686/MIPS/PPC64/Lanai)** | 平台 C 与 fastcc 不一致的目标有影响 |

#### ABI 变化
| 变更 | 说明 |
|------|------|
| `_BitInt` 位域 >255bits 的 MSVC ABI 修复 | 内部跟踪字段从 `unsigned char` 变为 `uint64_t` |
| x86-64 `__regcall` 调用约定修复 | struct 传递方式变化（含数组/浮点/`_Complex float`） |
| Itanium lambda mangling 修复 | NSDMI 中的 closure-prefix 保留 |
| AArch64 SVE builtin type MSVC name mangling | 影响 Microsoft ABI 的符号名称 |
| Coroutine resum/destroy 使用 platform C calling convention | i686/MIPS O32/PPC64 ELFv1/Lanai 有实际影响 |
| x86-64 大向量 (256/512-bit) struct 返回修复 | 影响 `-fclang-abi-compat=20` |

#### C++ 语言
- `__is_referenceable` 移除完成
- `!nonnull`/`!align` metadata 在引用上的生成
- 严格的整数到枚举转换（`const E x = (E)-1;` 若值超出范围不再视为常量）
- CWG400 完全实现：`using CurrentClass::Foo;` 在不指向基类时被拒绝
- Overload resolution (P3606)：非模板完美匹配时不实例化模板候选

#### LLVM IR 变更（当前 main）
- `llvm.convert.to.fp16` / `llvm.convert.from.fp16` 移除
- `denormal-fp-math` 字符串属性迁移到 `denormal_fpenv` 属性
- `nooutline` 变为属性
- 浮点字面量表示法重大变更（十六进制 `f0x` 前缀）

#### AArch64 后端（当前 main）
- `sysp`/`mrrs`/`msrr` 指令不需要 `+d128` feature 门控
- x29/x30 寄存器 clobber 使用 xN 名称时修复

---

## 3. C/C++ 标准支持变化

### C 标准支持演进

| 版本 | 新增支持 |
|------|---------|
| LLVM 16 | C2x 部分特性：`bool`/`static_assert`/`true`/`false` 作为关键字；`typeof`/`typeof_unqual` |
| LLVM 17 | C2x `#elifdef`/`#elifndef`；`__has_c_attribute` 支持 |
| LLVM 18 | C23 部分支持：`#embed`（编译器内置）；`nullptr` 常量 |
| LLVM 19 | C23 更多特性：`typeof` 语法完善 |
| LLVM 20 | C23 继续推进；`_Complex _BitInt` 被拒绝 |
| LLVM 21 | `__has_feature(modules)` 不依赖 C++20 标准；标准继续推进 |

### C++ 标准支持演进

| 版本 | 关键 C++ 变化 |
|------|-------------|
| LLVM 16 | C++17 默认；C++20 协程完善（除 Windows） |
| LLVM 17 | `static_assert(false)` 在模板中合法（CWG2518）；C++20 约束比较（CA104） |
| LLVM 18 | C++20 模板名称重整修复；C++20 named modules 正式化 |
| LLVM 19 | C++17 声称"完成"；`-frelaxed-template-template-args` 默认启用 |
| LLVM 20 | CWG2765 字符串字面量常量比较修复；`-Wdeprecated-literal-operator` 默认启用 |
| LLVM 21 | CWG400/DR692/DR1395/DR1432 实现；`export` 在 module implementation partition 中被拒绝 |

### C++ 标准模式默认值变化

| LLVM 版本 | 默认 C++ 标准 |
|-----------|-------------|
| 15.x | `gnu++14` |
| 16.0+ | `gnu++17` |

---

## 4. ABI 不兼容变更汇总

> 所有 ABI 修复都可以通过 `-fclang-abi-compat=<N>` 恢复旧行为，**_但仅适用于 Itanium ABI 目标_**（Linux/macOS/Android 等，不包括 Windows MSVC ABI）。

| LLVM 版本 | ABI 变更 | `-fclang-abi-compat` 恢复 | 影响范围 |
|-----------|---------|--------------------------|---------|
| 16 | packed struct 非 POD 成员对齐 | `15.0` | 所有 Itanium ABI 目标（除 Darwin/PS4/AIX） |
| 16 | POD 分类（defaulted 特殊成员） | `15.0` | 同上 |
| 17 | 特殊成员函数 trivial copyable 判定 | 无 | 罕见场景 |
| 18 | 函数模板名称重整 | `17` | 受约束模板/推导类型模板的符号 |
| 18 | `__int128` x86-64 SystemV 传递方式 | 无 | x86-64 Linux |
| 19 | Microsoft name mangling (auto NTTP/static init/临时对象) | `-fms-compatibility-version=19.14` | Windows/MSVC ABI |
| 19 | Microsoft 调用约定(template constructor/deleted copy assign) | 无 | Windows/MSVC ABI |
| 19 | AArch64 FMV ifunc 全局别名移除 | 无 | AArch64 |
| 20 | Itanium 构造 vtable 名称修正 | `19` | 所有 Itanium ABI 目标 |
| 20 | 成员式友元函数模板 mangling | `19` | 所有 Itanium ABI 目标 |
| 21 | `_BitInt` 位域 >255bits MSVC ABI | 无 | Windows/MSVC ABI |
| 21 | x86-64 `__regcall` struct 传递 | 无 | Linux x86-64 (`__regcall`) |
| 21 | Itanium lambda NSDMI closure mangling | 无 | 所有 Itanium ABI 目标 |
| 21 | AArch64 SVE builtin MSVC mangling | 无 | Windows/AArch64 |
| 21 | coroutine resum/destroy C calling conv | 无 | i686/MIPS O32/PPC64 ELFv1/Lanai |
| 21 | x86-64 256/512-bit vector struct 返回修复 | `20` | x86-64 |

---

## 5. 编译器优化变化

### 5.1 主要优化改进

#### 内联（Inliner）
- LLVM 16-17：内联成本模型持续改进（ML-inliner 稳定）
- LLVM 18-19：LLVM 内联器引入更精确的 cold callsite 处理
- LLVM 20-21：内联决策中更多使用 BFI（块频率信息）

#### 向量化（Loop/SLP Vectorizer）
- LLVM 16：VPlan 在 Loop Vectorizer 中逐步成熟
- LLVM 17：SLP 向量化器添加了更多的 reduction 和交错模式
- LLVM 18：Cost model 大量改进
- LLVM 19：实验性 vector intrinsic 稳定化（移除 `experimental.` 前缀）
- LLVM 20-21：更多的 SVE 自动向量化支持，矢量宽度推断改进

#### 全局优化
- LLVM 16: Memory attribute 统一化
- LLVM 17: `nofpclass` 浮点优化属性
- LLVM 19: `nusw`/`nuw` GEP flags 引入
- LLVM 20: **TBAA 默认启用**（基于类型的别名分析）— **可能影响严格别名违例代码**
- LLVM 21: **Lifetime DSE 优化**（死对象存储积极消除）

#### 链接时优化 (LTO)
- LLVM 17: `--lto=` 支持 unified LTO
- LLVM 18: FatLTO 支持；`--lto-validate-all-vtables-have-type-infos`
- LLVM 19: Threat Model 改进（whole-program devirtualization safety）
- LLVM 20-21: ThinLTO 分布式支持

### 5.2 可能的性能回归风险点

| 风险点 | 版本 | 说明 |
|--------|------|------|
| TBAA 默认启用 | 20 | 违反 strict aliasing 的代码可能被错误优化 |
| 指针溢出 UB 优化 | 20 | 依赖于指针包装语义的边界检查被优化掉 |
| Lifetime DSE | 21 | 依赖于死对象存储的代码行为改变 |
| `nooutline` 属性变化 | 21 | 属性语法改变可能影响 LTO |
| SVE/SVE2 可选化 | 19 | `-march=armv9-a` 不再默认启用 SVE |
| LLVM 18 vs 15 向量化差异 | 18 | Cost model 变更可能导致部分循环不再向量化 |

---

## 6. 安全加固特性演进

### 6.1 Pointer Authentication (PAuth) - LLVM 19+
- **完全支持的 ELF pointer authentication relocations**：`R_AARCH64_AUTH_ABS64`、`R_AARCH64_AUTH_RELATIVE`
- `GNU_PROPERTY_AARCH64_FEATURE_PAUTH` note 在 `.note.gnu.property` 中生成
- `llvm.ptrauth.auth`/`llvm.ptrauth.resign` 内建函数
- `-mbranch-protection=standard` 在支持 FEAT_PAuth_LR 时默认启用 `bti+pac-ret+pc`

### 6.2 Guarded Control Stack (GCS) - LLVM 17+
- FEAT_GCS 汇编支持（LLVM 17）
- `-z gcs` / `-z gcs-report` 链接器选项（LLVM 19）
- ARMv9.6-A 支持（LLVM 20）

### 6.3 Control Flow Integrity (CFI)
- LLVM 16-21：CFI 持续改进，Forward-Edge CFI 逐步完善
- 新增 `-fsanitize-cfi-stack-depth-callback-min=` 参数

### 6.4 libc++ 加固（Hardening）
从 LLVM 18+ 开始 libc++ 正式引入 Hardening Modes：

| 模式 | 宏定义 | 用途 |
|------|--------|------|
| **Unchecked (none)** | `_LIBCPP_HARDENING_MODE_NONE` | 无检查（默认） |
| **Fast** | `_LIBCPP_HARDENING_MODE_FAST` | 安全关键检查，低开销，适合生产 |
| **Extensive** | `_LIBCPP_HARDENING_MODE_EXTENSIVE` | 扩展检查，中等开销 |
| **Debug** | `_LIBCPP_HARDENING_MODE_DEBUG` | 全部检查，大开销，仅用于测试/CI |

应用方式：编译时添加 `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST`

### 6.5 Sanitizers 进化（详见第11节）
- ASan：Stack-use-after-return 改进；更精确的错误定位
- UBSan：新增 null/alignment checks for aggregates（LLVM 20+）
- Realtime Sanitizer (RTSan)：LLVM 19+ 新增（#92460）
- Numerical Sanitizer：LLVM 18+ 新增（#94322）

### 6.6 其他安全相关特性
| 特性 | 版本 | 说明 |
|------|------|------|
| KCFI (Kernel CFI) | LLVM 17 | 内核级 CFI 支持 |
| MemTag (MTE) 链接器支持 | LLVM 17 | DT_AARCH64_MEMTAG_* dynamic tags |
| `-fwrapv-pointer` | LLVM 20 | 使指针溢出有明确定义 |
| `-fno-strict-overflow` 含义扩展 | LLVM 20 | 同时含义 signed int + pointer overflow |
| `-fno-pointer-tbaa` | LLVM 20 | 禁用默认的 TBAA 标记 |
| `-fsanitize=pointer-overflow` | LLVM 16+ | 检测指针溢出（生产增强） |
| `-fsanitize-memory-param-retval` 默认启用 | LLVM 16 | MSan 下检测未初始化参数传递 |
| `ShadowCallStack` | LLVM 16+ | 持续改进 |
| `-fno-lifetime-dse` | LLVM 21 | 禁用生命周期 DSE 优化 |

---

## 7. 目标架构变更

### 7.1 AArch64

| 版本 | CPU 支持新增 | 关键变更 |
|------|-------------|---------|
| 16 | Cortex-A715, Cortex-X3, Neoverse V2 | FMV 实现（beta）；SEH unwind 修复（Windows）；Armv8.3 Complex Number |
| 17 | - | FEAT_GCS/FEAT_CHK/FEAT_ATS1A；preserve_all cc |
| 18 | Cortex-A520, A720, X4 | Neoverse-N2 修正为 Armv9.0a；2023 架构扩展 |
| 19 | R82AE, A78AE, A520AE, A720AE, A725, X925, Neoverse-N3/V3/V3AE | **SVE/SVE2 变为 ARMv9.0 可选**；PAuth 完全支持 |
| 20 | FUJITSU-MONAKA | Armv9.6-A (2024) 支持；`.balign N, 0` 填充行为变更 |
| 21 | - | x29/x30 clobber 修复；`sysp`/`mrrs`/`msrr` 行为修复 |

### 7.2 x86

| 版本 | CPU 支持新增 |
|------|-------------|
| 16 | AMX-FP16, CMPCCXADD, AVX-IFMA, AVX-VNNI-INT8, AVX-NE-CONVERT, znver4 |
| 17 | - |
| 18 | - |
| 19 | **3DNow! 完全移除**；**Intel KNL/KNM 移除** |
| 20 | AVX10.2, AMX-AVX512, AMX-FP8 |
| 21 | (持续改进) |

### 7.3 移除的目标
- LLVM 16: ARMv2/ARMv2A/ARMv3/ARMv3M
- LLVM 19: 3DNow! (所有 intrinsics 和 codegen)
- LLVM 20: le32/le64 targets; RenderScript
- LLVM 21: (无重大移除)

---

## 8. LLD 链接器变化

### 8.1 ELF 关键变化

| 版本 | 关键特性 |
|------|---------|
| 16 | 并行化（输入段初始化+重定位扫描）；Zstd 压缩调试；`--no-undefined-version` 默认 |
| 17 | 线程上限 16 核；`--remap-inputs=`；unified LTO；xxh3 哈希 |
| 18 | FatLTO；`--lto-validate-all-vtables-have-type-infos`；`cdsort` 算法 |
| 19 | CREL 重定位（实验性）；`--compress-sections`；PAuth 重定位；`--debug-names` |
| 20 | `-z nosectionheader`；随机段填充（A/B 测试）；`.note.GNU-stack` SHF_EXECSTR 检查 |
| 21 | ThinLTO 分布式；`--why-live=`；AArch64 SHF_AARCH64_PURECODE；`--xosegment`/`--no-xosegment` |

### 8.2 默认行为变更
| 变更 | 版本 | 说明 |
|------|------|------|
| `--no-undefined-version` 默认 | 16 | version script 中符号未定义报错 |
| `--build-id` 默认 SHA1 | 19 | 从 8 字节 fast 改为 20 字节 SHA1 |
| `.note.GNU-stack` 安全检查 | 20 | 非 relocatable 链接中 exec 栈标记被拒绝（除非 `-z execstack`） |
| `r` 模式不再定义 `__global_pointer$` | 16 | 影响 RISC-V |

---

## 9. libc++ 变化

### 9.1 Hardening（加固）支持
- **LLVM 18+**：正式引入 Hardening Modes（详见第 6.4 节）
- 对 ABI 无影响
- 通过 `-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST` 启用生产级检查

### 9.2 重大移除/废弃
| 特性 | 版本 | 替代 |
|------|------|------|
| `unary_function`/`binary_function` (C++17+) | 16 | 移除 |
| `std::char_traits` 基础模板 | 18 (预计) | 需提供特化 |

### 9.3 标准演进
- LLVM 16-21: C++20/23/26 库特性持续实现
- LLVM 19+: C++26 库特性逐步引入

---

## 10. clang-tidy 变化

### 10.1 新增检查按版本

| 版本 | 新增 checks（举例） |
|------|-------------------|
| 16 | `bugprone-suspicious-realloc-usage`, `modernize-use-std-format` 等 |
| 17 | `modernize-use-std-numbers`, `readability-avoid-return-with-void-value` 等; 支持 YAML 格式的 Checks 配置 |
| 18 | `bugprone-reserved-identifier`, `modernize-replace-disallow-copy-and-assign-macro` |
| 19 | clangd 中 `readability-identifier-naming` 支持 `textDocument/rename` 全项目重命名; `misc-const-correctness` 支持 FastCheckFilter: None |
| 20 | C++20 Modules 支持改进 |
| 21 | `clang-tidy-diff.py` 新增 `-warnings-as-errors` 参数 |

### 10.2 配置变更
- LLVM 17: `.clang-tidy` 中的 `Checks` 字段支持 YAML 列表格式
- LLVM 20+: 模块支持逐步改进

---

## 11. Sanitizers 变化

### 11.1 AddressSanitizer (ASan)
- LLVM 16+: Stack-use-after-return 改进
- LLVM 17+: 更精确的 use-after-scope 检测
- LLVM 18+: array cookie poisoning 支持自定义 `operator new[]`
- LLVM 19+: stable ABI 选项（`-fsanitize-stable-abi`）
- LLVM 20+: 持续的错误定位改进

### 11.2 MemorySanitizer (MSan)
- LLVM 16: `-fsanitize-memory-param-retval` 默认启用（检测未初始化参数传递）
- LLVM 17+: use-after-destroy 检测
- LLVM 18+: 更多的 origins 跟踪改进

### 11.3 UndefinedBehaviorSanitizer (UBSan)
- LLVM 16+: 持续改进
- LLVM 20+: 新增 null/alignment checks for aggregates (#164548)
- `-fsanitize=pointer-overflow` 持续优化

### 11.4 新 Sanitizers
| Sanitizer | 版本 | 说明 |
|-----------|------|------|
| **Numerical Sanitizer** | LLVM 18+ (#94322) | 数值计算相关检查 |
| **Realtime Sanitizer (RTSan)** | LLVM 19+ (#92460) | 实时系统安全 |
| **Metadata Sanitizer (实验性)** | LLVM 20+ | `-fexperimental-sanitize-metadata=` |

### 11.5 其他编译器-rt 变化
- GPU Sanitizer 支持（`-fgpu-sanitize`）
- 覆盖率检测新增 stack depth callback 选项

---

## 12. 质量加固建议

### 12.1 预升级准备

1. **代码库审计**
   - 搜索所有使用隐式函数声明、隐式 int、隐式类型转换的代码
   - 检查是否有 strict aliasing 违规（Clang 20 TBAA 默认启用）
   - 检查是否有指针包装语义依赖（Clang 20 指针溢出 UB 优化）
   - 检查是否有依赖死对象存储的行为（Clang 21 lifetime DSE）

2. **ABI 兼容性审查**
   - 确定项目是否对外暴露 C++ ABI（动态库、插件系统）
   - 如果暴露 ABI，需要评估是否有 ABI 兼容性需求
   - 设置 `-fclang-abi-compat=15` 保留旧 ABI（注意：LLVM 18 开始有些变更不能完全恢复）

3. **测试基础设施升级**
   - 确保所有 CI 测试已就绪
   - 添加新的 warning flags 到代码编译中（特别是 `-Werror` 场景）
   - 考虑集成 libc++ hardening（至少 Fast 模式）

### 12.2 分阶段升级策略

```
阶段 1: 编译自检（使用新编译器编译旧代码，发现 warnings/errors）
   ↓
阶段 2: 修复编译错误（所有 Breaking Changes 逐个解决）
   ↓
阶段 3: 功能测试（单元测试 + 集成测试）
   ↓
阶段 4: 性能回归测试（基准测试对比）
   ↓
阶段 5: 安全加固（启用新安全特性）
   ↓
阶段 6: 生产部署（灰度发布 + 监控）
```

### 12.3 关键编译标志建议

```bash
# 诊断/迁移期建议添加
-Wno-error=implicit-function-declaration    # LLVM 16 新默认
-Wno-error=implicit-int                     # LLVM 16 新默认  
-Wno-error=incompatible-function-pointer-types  # LLVM 16 新默认
-Wno-error=void-ptr-dereference            # LLVM 16/17 C++
-Wno-error=enum-constexpr-conversion       # LLVM 18/19/20
-Wno-error=ambiguous-reversed-operator     # LLVM 18
-Wno-error=extraneous-template-head        # LLVM 20

# ABI 兼容性（如果需要保持旧 ABI）
-fclang-abi-compat=15                      # 保持 LLVM 15 ABI（但不完全兼容 LLVM 18+ 变更）

# 安全/行为兼容性
-fwrapv-pointer                            # LLVM 20+ 避免指针溢出 UB 
-fno-pointer-tbaa                          # LLVM 20+ 禁用默认 TBAA
-fno-lifetime-dse                          # LLVM 21+ 禁用生命周期 DSE
```

### 12.4 测试重点

| 测试领域 | 优先级 | 说明 |
|---------|--------|------|
| C++ ABI 兼容性 | **P0** | 动态链接/插件系统必须验证 |
| 内联汇编 | **P0** | LLVM 17 asm goto 变更 |
| 模板实例化 | **P0** | LLVM 18 name mangling 变更 |
| 常量表达式 | **P1** | LLVM 20 enum constexpr 检查 |
| 类型别名分析 | **P1** | LLVM 20 TBAA 可能导致错误优化 |
| 指针运算边界检查 | **P1** | LLVM 20 指针溢出 UB |
| 对象生命周期 | **P1** | LLVM 21 lifetime DSE |
| 向量化行为 | **P2** | 跨 6 个版本的 cost model 变化 |
| LTO 行为 | **P2** | ThinLTO 分布式支持 |
| ARM/AArch64 调用约定 | **P1** | 目标平台特有 |
| 代码大小 | **P2** | -Os/-Oz 优化差异 |

### 12.5 安全加固推荐启用

```bash
# 生产环境推荐
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST   # libc++ 快检
-fsanitize=pointer-overflow                              # 检测指针溢出

# 测试/CI 推荐
-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE  # libc++ 扩展检查
-fsanitize=address,undefined                               # ASan + UBSan

# 如果目标平台支持 AArch64 PAuth
-mbranch-protection=standard   # LLVM 19+ 默认启用 BTI+PAC-RET+PAuth-LR

# 如果需要兼容旧版 ABI
-fclang-abi-compat=15          # 但建议逐步迁移到新版 ABI
```

---

## 13. 升级迁移检查清单

### 编译阶段
- [ ] 代码中无 `-Wimplicit-function-declaration`（16）
- [ ] 代码中无 `-Wimplicit-int`（16）
- [ ] 代码中无 `-Wincompatible-function-pointer-types`（16）
- [ ] 无 void* 间接访问（16/17）
- [ ] 无含有非 16 位对齐的数组类型（16）
- [ ] 无 `std::experimental::coroutine_traits` 使用（17）
- [ ] 无 enum-to-bool 的 constexpr 转型（19/20 逐步严格）
- [ ] 无多余 template header（20）
- [ ] 无 `_Complex _BitInt` 类型（20）
- [ ] 无 `__is_nullptr`/`__is_referenceable` 使用（20/21）
- [ ] 无依赖模板参数自阴影的代码（19）
- [ ] 无 `le32`/`le64` 目标配置（20）
- [ ] 无 `clang-rename` 工具依赖（20）
- [ ] 无 RenderScript 依赖（20）
- [ ] 无 `break`/`continue` 在循环条件/增量语句中（21）
- [ ] 无依赖 `__has_feature(modules)` 在 `-std=c++20` 下的检查（21）
- [ ] 检查 `std::char_traits` 非标准特化（16 已废弃）

### ABI 兼容性
- [ ] 动态库/插件 ABI 兼容性测试通过
- [ ] 确定 `-fclang-abi-compat` 使用版本
- [ ] 如果使用 MVSC ABI，检查 auto/decltype(auto) name mangling（19）
- [ ] 如果使用 AArch64 SVE + MSVC ABI，检查 builtin type mangling（21）
- [ ] 如果使用 `__regcall`，检查 struct 传递方式（21）

### 安全配置
- [ ] 评估并可能启用 libc++ hardening
- [ ] 评估 `-fwrapv-pointer` 需求（20）
- [ ] 评估 TBAA 是否启用（20）
- [ ] 评估 `-fno-lifetime-dse` 是否需要（21）
- [ ] 评估 `-mbranch-protection=standard` 启用（AArch64, 19+）
- [ ] 更新 sanitizer 配置（利用新功能）

### 链接阶段
- [ ] 检查 LLD 版本兼容性（特别是 CREL 实验性特性，19）
- [ ] 检查 `--build-id` 类型（19 默认改为 SHA1）
- [ ] 检查 `--no-undefined-version` 行为（16 默认启用）
- [ ] 检查 `.note.GNU-stack` 处理（20 新增检查）
- [ ] 更新 LTO 配置（FatLTO, ThinLTO 分布式等新特性）

### 工具链
- [ ] 更新 clang-tidy 配置（利用新检查）
- [ ] 更新 `.clang-tidy` 文件格式（17 支持 YAML 列表）
- [ ] 更新 clang-format 配置
- [ ] 更新 CMake 最低版本要求（LLVM 16 需要 CMake 3.20+）
- [ ] 更新编译 Python 版本要求（LLVM 19 需要 Python 3.8+）
- [ ] 更新 resource directory 路径（16 改为仅 major 版本）

---

## 附录：关键参考资料

- [LLVM 16.0.0 Release Notes](https://releases.llvm.org/16.0.0/docs/ReleaseNotes.html)
- [Clang 16.0.0 Release Notes](https://llvm.github.io/www-releases/16.0.0/tools/clang/docs/ReleaseNotes.html)
- [LLVM 17.0.0 Release Notes](https://releases.llvm.org/17.0.0/docs/ReleaseNotes.html)
- [Clang 17.0.0 Release Notes](https://llvm.github.io/www-releases/17.0.0/tools/clang/docs/ReleaseNotes.html)
- [LLVM 18.1.0 Release Notes](https://releases.llvm.org/18.1.0/docs/ReleaseNotes.html)
- [Clang 18.1.0rc Release Notes](https://prereleases.llvm.org/18.1.0/rc3/tools/clang/docs/ReleaseNotes.html)
- [LLVM 19.1.0 Release Notes](https://releases.llvm.org/19.1.0/docs/ReleaseNotes.html)
- [Clang 19.1.0 Release Notes](https://llvm.github.io/www-releases/19.1.0/tools/clang/docs/ReleaseNotes.html)
- [LLVM 20.1.0 Release Notes](https://releases.llvm.org/20.1.0/docs/ReleaseNotes.html)
- [Clang 20.1.0 Release Notes](https://llvm.github.io/www-releases/20.1.0/tools/clang/docs/ReleaseNotes.html)
- [Clang 21.1.0 Release Notes](https://releases.llvm.org/21.1.0/tools/clang/docs/ReleaseNotes.html)
- [LLVM 15 → 16 Phoronix Overview](https://www.phoronix.com/news/LLVM-16.0-Released)
- [LLVM 19 → 20 Phoronix Overview](https://www.phoronix.com/news/LLVM-20.1-Released)
- [libc++ Hardening Documentation](https://llvm.org/docs/libcxx/Hardening.html)
- [LLD 21 ELF Changes](https://maskray.me/blog/2025-09-07-lld-21-elf-changes)
- [LLVM Pointer Overflow UB Change (PR #122486)](https://github.com/llvm/llvm-project/pull/122486)
- [LLVM LLVM 15 → 21 Opaque Pointers Migration](https://llvm.org/docs/OpaquePointers.html)

---

> **总结**：LLVM 15.0.4 → 21.1.8 跨越 3 年半的大型升级，涉及约 11.4 万个提交。影响最大的变更是：
> 1. **LLVM 20**：TBAA 默认启用 + 指针溢出 UB 优化（可能导致代码行为静默变化）
> 2. **LLVM 18**：函数模板名称重整（C++ ABI break）
> 3. **LLVM 21**：Lifetime DSE（死对象存储消除）
> 4. **LLVM 19**：SVE/SVE2 ARMv9.0 可选（AArch64 架构变更）
>
> 建议采用**分阶段升级**策略，从编译自检开始，逐步修复后通过全面的功能测试和性能回归测试再进入生产。
