# EJIT 演讲分镜（STORYBOARD）

标题：把运行时，变成编译期。 —— EJIT：时间窗常量 × 运行时特化
副标：一个长在裸核固件里的 JIT（以及我们如何在一颗没有 std::thread 的核上手搓了一个并发运行时）
画布 1600x900。五幕 = 底部时间线上的五个窗口。深色默认，T 切浅色。

## 颜色 = 概念（全片一致）
- amber  #C58300(图表)/#F2B23A(线条)：AOT / 运行时 / 未知
- ice    #009FD8/#6FD3FF：常量 / 冻结 / 特化 / JIT
- violet #9E6FFF/#B9A6FF：worker 核 / 异步编译
- red    #FF3D42/#FF6B6B：失效 / 危险 / 坏消息（永远配图标+文字）
- lime   #56AB00：✓ 完成
- 规则：图表里 amber 与 red 永不同时作为两个数据系列（CVD 下塌缩）

## Act0 开场（窗口 0）
1  Title：hero canvas 时间线，噪声信号在窗口内冻结成常量。
2  离谱的想法：100KB–2MB RAM / 无 OS / 无文件系统 / 无 std::thread → 我们放了一个编译器进去。
3  路线图：五个窗口 = 五幕。
4  一分钟版：标注 → 激活 → 特化 → 失效（四步环）。

## Act1 问题
5  数据在时间窗内是常量（板型/小区类型/载波参数）。时间线图。
6  AOT 的困境：真实 AOT 汇编，每个样本 2 次配置 load + 数据相关分支。
7  为什么不能照搬 JVM/V8：无文件系统/无 deopt/RAM/软实时/无线程库/无 dlsym。
8  第三条路：时间窗常量。validity has a window。

## Act2 机制
9  用户面：7 个标注里的 5 个（真实 ejit_attr_test.c 代码）。
10 标注→IR metadata：真实 IR，!ejit.period_const 挂在 load 上；三重保险。
11 AOT 两阶段：PASS1 在一切优化之前；PASS2-4 在 O2 之前（非 LTO）。
12 真实的 wrapper：funcidx 守卫→compile_or_get→dispatch/fallback→phi；inline-cache 3 指令形态。
13 二进制里多了什么：@__ejit_bitcode、.ejit_bitcode/.ejit_group 注册表、icache cell 表、ctors 65535。
14 运行时闭环（动画泳道）：业务核 miss→入队→worker 编译→publish→命中直跳。
15 魔法 pass：从进程内存读真值，替换 load 为常量。软标注。
16 剩下的交给 LLVM：真实 check_cell before/after IR。
17 汇编对比：stream kernel AOT vs JIT（都是 AArch64 后端真实输出；JIT 循环 8 条指令，0 配置 load，0 数据分支）。

## Act3 疯狂
18 幕间："现在，疯狂的部分。"
19 把 LLVM 塞进固件：53 个 .a 117MB → 21MB(x86)/~12MB(aarch64 预估)；裁剪后 loadable 19.57→18.51MB；llc 1793 输入逐字节一致。
20 W^X 代码池：2MiB 池，finalize 逐 4K 页封固；每个特化至少一页；12 字节函数独占 4096。
21 没有 std::thread：EJitAtomic/Vyukov 1024 环/dedup CAS/32 桶 RwLock/read-token/version 双检查点。（环动画）
22 跨核：共享 POD blob，CAS 选主，同 VA，executableCoreMask；"同 VA ≠ 同执行权限"。
23 特化边界一路外推：标量/数组→指针+bounds→free_dim→local free_dim→bound_ptr→anchored witness。安全底线。
24 无文件系统 PGO：Tier-1 计数 → 64 → 内存 profile(/ejit.prof) → Tier-2。
25 无 deopt 推测：value profile + guard + 热克隆 + 冷回退；≥100 样本 & ≥99%。

## Act4 诚实
26 幕间："我们做的是测量，不是信仰。"
27 47 万倍：官方 perf_bench 474,369.6×（三次 47/49/143 万）；真身是 12 字节 mov+movk+ret。
28 诚实的收益范围：stream 快 / interproc 慢 / tiny 更慢。发散条形图（基线 1×）。
29 探测成本与 inline cache：~50ns 运行时查询 → 3 指令探针；aarch64 9.0/20.2ns。
30 多小区的代价：i-cache 压力 = 小区数 × 每份代码；合一份/放得近/取得早；待上板。
31 PGO 校准：12 个真实闭包 0 剩余 callee → 内联 no-op；价值只在块布局；+640KB。
32 没做完 / 待真机：清单。
33 规模与状态：~25K LOC + 1.5K clang；gtest 272/52/169；lit 55/55 + 28/28；集成 31 目标（27 过 2 红 2 未构建）；7 属性；PR 已到 #274。

## Act5 收束
34 三个带走的东西。
35 七个月时间线（2026.04 spec → 11）。
36 End：窗口关上，回退 AOT，什么都不会坏。谢谢。

## 附录（总览可见）
A1 公开 API 速查；A2 测量方法与出处；A3 术语；A4 源码索引。
