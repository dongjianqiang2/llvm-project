# PR231 双核小表调测用例

此目录的 `ejit_smalltable_sre_test.c`（controller）和
`ejit_smalltable_sre_business.c`（business）使用真实 `ejit_entry`、生成 wrapper、
Async worker、共同 T1 的真实计数器、完整 profile 和共同 T2；不是手写 JIT
入口或 Linux 二进制改名。服务器没有 BiSheng/SRE SDK，**本包不代表已完成
SDK 最终链接或真实板测**。组件模拟器结果另列，不替代这两项。

## 编译与接入前提

- 使用本分支的 patched EJIT clang 和本次完整 runtime；不能用旧 EJIT archive。
  用例不能定义 `EJIT_DISABLE`，不能忽略未知 EJIT attribute 的警告。
  **两个 C 文件分别编译后链接**，共同使用 `ejit_smalltable_sre_fixture.h`。
  不要提前 LTO 合成一个 TU；observer 必须保持实际外部 controller 调用，
  不能把 controller 的 core-private 状态和调测逻辑克隆进 JIT 采样图。
- **业务 TU 的 wrapper 生成必须加 `-mllvm -ejit-small-table-hooks`**（该选项
  默认OFF）。若在LTO/backend阶段才生成wrapper，该阶段也传此选项；保留
  原EJIT bitcode/AOT pipeline，不另关它们。C setup不能给旧wrapper补hook。
  对实际post-pass IR检查 `pr231_smalltable_entry` 中有
  `ejit_stab_wrapper_enter`、`ejit_stab_wrapper_no_policy_current` 和配对
  `ejit_stab_leave` 调用，不能只看到库导出了同名符号就认为业务已接入。
- 保留 `EJIT_SRE_PGO_BRANCH_AUDIT=ON`、诊断、Async 和在线 PGO。
  原产品默认不自动开启小表；本用例通过新 C setup 明确申请。
- 用例仅替换一个现有 period demo。不要同时链接另一个定义
  `test_ejit_period` 的对象。小表 runtime 七个 hook、新桥接六个 C API
  和两个 shell 入口必须在最终合并对象中有唯一真实定义。
- 复制 `ejit_smalltable_sre_platform.h.example` 到 SDK 工程，绑定真实 task-self
  和共享数据权限接口，定义 `EJIT_PR231_PLATFORM_HEADER` 指向它。
  模板故意无法直接编译；没有提供绑定时，用例打印 `BLOCKED`，不继续初始化。
  不可用 core ID 冒充 task ID，也不能将权限检查写成固定返回零。
- `.mc_shared` 必须在两核映射到同一 VA、同一真正 coherent 存储；共享命令、
  配置、source-state、output 都在此节。共同表/计数器堆对象也须可共享，并通过
  调用核的真实权限接口。仅有 section 名或 fingerprint 一致不证明这个前提。
- 每核私有 runtime/构造器状态必须保持私有；两核各消费同一套注册。
  默认每核首次命令调用一次 `call_init_array_functions()`。若产品启动已经
  完整做过构造，将用例编为 `EJIT_PR231_CALL_INIT_ARRAY=0`，不能重复构造。
- 新固定池域必须在 worker 初始化普通 ORC engine **之前**显式启用；若此前
  已使用旧普通固定池，拒绝接管，重新启动再测。域为单调分配、不回收旧地址。
  `__ejit_code_start/end` 使用产品真实保留区、4K 对齐，程序在其范围内向上
  2M 对齐；建议至少 32MiB 以容纳普通、共同及换代池。不要把其它代码/数据放进
  这块保留区。不要假造地址、删除 guard 或另启动态分配绕过。
- 函数级普通 PGO 交接在真正 owner worker 完成后才使 Host 生效。测试的
  `owner` 指配置/调用侧16号核；runtime pool 的编译 owner/worker 为6号核。
- 本次新增 C 调测桥是有界接口，最多32成员、32 counter pair、256 counter word，
  超出即拒绝，不截断或接收残缺 profile。本用例6×2在界内；不能直接把它当成
  完整6×20产品接入（原 Host 的6×20规格/服务器回归没有改变）。

## 上板执行

从 fresh boot、无任何预热开始，按顺序执行：

```text
core 6
test_ejit_period
core 16
test_ejit_period
core 6
test_ejit_smalltable_print
ejit_taskpool_print_stats
ejit_taskpool_print_compiled
```

默认6 cell × 2 TRP、**每代合计64次**共同 T1 采样；换代后再完成64次，
最终打印保留 generation=2 的真实 T2 和 IR。重新启动后在16号核改用
`test_ejit_period 1` 可验证可配置的8次配额，6号核启动方式不变。
不在命令之间人为加秒级采样间隔；桥接只按实际 worker 完成等待。
每个 run 必须 fresh boot，不能把上一轮 PGO、计数器和固定池地址重置后继续。
用例在首次request之前自动调用 `ejit_dump_func("pr231_smalltable_entry")`
准备真实IR捕获，不需要测试人员预先另输dump命令。

期望关键日志如下（数值以实际编译结果为准，不要求固定 counter pair 数量）：

```text
[STAB231] worker=6 ready; run test_ejit_period on core16
[EJIT] ... common T1 requested ... members=12 budget=64 actual_worker_task=...
[EJIT] ... FULL profile consumed ... samples=64 pairs=... words=... root_count=64 T2=published
[STAB231] HELD_CANCEL physical=1 borrow=1 retained=...
[STAB231] PASS cold: members=12 samples=64 pairs=... words=... profile=... commonT2=1; ... OK
[STAB231] stage=3 (3=PASS,4=FAIL; not product acceptance)
```

检查项：所有成员值和普通 live load/store 正确；普通 PGO enqueue/pending 为零；
真实共同计数器完整记录、T2 的真实 PGO entry metadata 恰好64/8；
不能把分支 counter[0] 当 entry 次数。quota+1 在 T2 发布前回 AOT，不改共同计数器；
全部预期 counter/data pair 被完整冻结并消费，12个逻辑 slot 进入共同 T2；
拒绝 borrow 回 AOT、无租约泄漏；在真实调用内 cancel 时代码/表/borrow 仍保留，
实际 wrapper leave 后才释放；新 generation 再从真实共同 T1 的零计数开始。

`FAIL`、`BLOCKED`、非零返回、stage非3、丢失 counter pair、残留普通 pending、
借用未释放、指针/数值不对均不算通过。`worker ready`、包能加载、已有旧测试全绿
也不等于上述检查完成。完整产品6×20规模、业务 readiness、跨核缓存可见性及长时
运行仍是额外验收门槛。预热共存对照保留在服务器 generated-wrapper 回归中，
此板用例默认**不预热**，不要预热后重试并称为冷启动通过。
普通 `ejit_taskpool_print_compiled` 不列独立Host共同对象，可能仍显示0；共同
T2以 `test_ejit_smalltable_print` 的 `tier=2/full_profile=1`、真实入口计数和
12个published slot为准。普通enqueue/pending保持0正是这条冷启动用例的判据。

## lipo / 最终对象检查

在本次 runtime 包的 `gc-merge` 和 `merge` 都使用 `--require-small-table-sre`；
已有 AArch64 脚本可设置 `EJIT_REQUIRE_SMALL_TABLE_SRE=1`。最终含用例的合并输入
再加 `--require-demo`。库本身通常不含 shell 入口，不能以库通过代替最终应用检查。
两阶段不能使用 strip 或删除 `.symtab/.strtab`。

```text
python3 ejit_test/lipo/lipo.py gc-merge --input=包含用例的归档.a --build-dir=本次构建目录 --output=保留后.a --require-small-table-sre --require-demo
python3 ejit_test/lipo/lipo.py merge --input=保留后.a --build-dir=本次构建目录 --output=merged_ejit_smalltable_test.o --require-small-table-sre --require-demo
python3 ejit_test/check_pr231_board_symbols.py merged_ejit_smalltable_test.o --require-be
```

若 SDK 工程用自己的最终 linker script，仍要对**实际上板传输的文件**运行末项检查，
其中 ET_REL 保持默认 `--kind object`，ET_EXEC/ET_DYN 显式加 `--kind linked`。
并在发送/接收端核对 SHA256。检查包含真实符号表、大小端/ELF属性、唯一 hook 和 shell
入口、非空 bitcode/period 注册边界及 `.mc_shared`。它不证明所有 SRE 未定义符号已由
SDK解析或 DLIB loader 已接受；保留最终 SDK 链接日志和板端加载结果。
