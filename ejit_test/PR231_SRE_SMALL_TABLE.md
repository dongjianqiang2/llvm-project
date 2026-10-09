# PR231 双核小表调测用例

此目录只需编译 `ejit_smalltable_sre_test.c` 一个 C 文件，业务、启动与调测都在其中。
使用真实 `ejit_entry`、生成 wrapper、
Async worker、共同 T1 的真实计数器、完整 profile 和共同 T2；不是手写 JIT
入口或 Linux 二进制改名。服务器没有 BiSheng/SRE SDK，**本包不代表已完成
SDK 最终链接或真实板测**。组件模拟器结果另列，不替代这两项。

## 编译与接入前提

- 使用本分支的 patched EJIT clang 和本次完整 runtime；不能用旧 EJIT archive。
  用例不能定义 `EJIT_DISABLE`，不能忽略未知 EJIT attribute 的警告。
  **只将此 C 文件加入工程**，不要再编译旧版 `business.c`，避免业务符号重复。
  真实 observer 通过共享的可变 volatile 函数指针槽调用；PASS1将槽外部化并
  注册实际地址，不追踪其初始化器，测试控制状态不会成为JIT采样图副本。
  不要把该槽改成const、移除volatile，或改回同TU直接调用observer。
- **此文件的 wrapper 生成必须加 `-mllvm -ejit-small-table-hooks`**（该选项
  默认OFF）。若在LTO/backend阶段才生成wrapper，该阶段也传此选项；保留
  原EJIT bitcode/AOT pipeline，不另关它们。C setup不能给旧wrapper补hook。
  对实际post-pass IR检查 `pr231_smalltable_entry` 中有
  `ejit_stab_wrapper_enter`、`ejit_stab_wrapper_no_policy_current` 和配对
  `ejit_stab_leave` 调用，不能只看到库导出了同名符号就认为业务已接入。
- 保留 `EJIT_SRE_PGO_BRANCH_AUDIT=ON`、诊断、Async 和在线 PGO。
  原产品默认不自动开启小表；本用例通过新 C setup 明确申请。
  沿用已有 BE/SRE 的 `EJIT_SRE_SHARED_TASKPOOL`、`EJIT_SRE_SHARED_CODE_POINTERS`、
  `EJIT_SRE_CODE_POOL`、`EJIT_FIXED_CODE_POOL` 和 `EJIT_CODE_POOL_4K_SEAL=ON`
  能力组合；这些是既有 runtime 的共享/权限能力，不是新的用例 SDK 绑定。
- 用例仅替换一个现有 period demo。不要同时链接另一个定义
  `test_ejit_period` 的对象。小表 runtime 七个 hook、新桥接七个 C API
  和两个 shell 入口必须在最终合并对象中有唯一真实定义；严格 demo roots
  同时保留 `g_pr231_probe_dispatch` 共享 OBJECT 和 `pr231_probe_inflight`
  真实 AOT FUNC，缺任一即报错，不依赖碰巧被别的引用留下。
- **不需要 SDK task-self 接口或 `EJIT_PR231_CURRENT_TASK_ID` 宏**。shell C
  入口只向现有 worker 命令队列提交；worker 内部操作沿真实入口调用链传递
  不可公开构造的执行上下文，校验 pool、启动代际和重启 epoch，避免自排队。
  旧 bindings ABI 不变；可选 `current_task_id` 仅供真实 SDK 身份诊断，未提供时
  `workerTaskIdentity=0`（未知），不把内部上下文或核号冒充任务 ID。
- **默认不用额外 SDK 绑定头或映射宏**。与 PR230/MFS 用例一样，已有 runtime
  worker、共享节和 code pool 接线负责平台工作。用例的旧 bindings ABI 保留，
  `current_task_id`、`delay_ticks`、`prepare_shared_data` 均传 NULL；runtime
  使用真实共享边界校验、已有 SRE delay 和实际对象权限准备，不装成功空回调。
  小表专用表 payload 不再依赖任意 owner `new[]` 跨核可见：显式小表路径从已有
  `__ejit_code_start/end` 固定域获取独占的数据块，在独立 RW/NX 页中分代分配；
  普通 code/T1 counter 沿原实际 range 权限路径。没有新 SDK 函数或全局 allocator
  改造。缺少真实 bounds/domain、越界、不支持的权限或容量耗尽即拒绝/AOT。
- `.mc_shared` 必须在两核映射到同一 VA、同一真正 coherent 存储；共享命令、
  配置、source-state、output、callback槽及调测flags都在此节。最终 linker 必须
  在完整节前后定义 `__ejit_shared_start` / `__ejit_shared_end`，runtime 检查真实
  范围后允许访问；用例在两核setup调用薄 C API `ejit_small_table_sre_prepare_data`
  检查所有这些实际对象。真实AOT callback代码也须在调用核可执行。固定域内
  小表页/计数器的同物理共享、cache coherence 和初始静态共享节 R/W 是产品部署
  契约，不能用相同 VA、section 名或 fingerprint 代替真实板测证明。
- 每核私有 runtime/构造器状态必须保持私有；两核各消费同一套注册。
  默认每核首次命令调用一次 `call_init_array_functions()`。若产品启动已经
  完整做过构造，将用例编为 `EJIT_PR231_CALL_INIT_ARRAY=0`，不能重复构造。
- 新固定池域必须在 worker 初始化普通 ORC engine **之前**显式启用；若此前
  已使用旧普通固定池，拒绝接管，重新启动再测。域为单调分配、不回收旧地址。
  `__ejit_code_start/end` 使用产品真实保留区、4K 对齐，程序在其范围内向上
  2M 对齐；建议至少 32MiB 以容纳普通、共同及换代池。不要把其它代码/数据放进
  这块保留区。不要假造地址、删除 guard 或另启动态分配绕过。
- 小表数据预算：使用固定域内独占2MiB块（按需领取），每代按4KiB页分配，
  总数据块预算最多16MiB且受固定域剩余空间约束；最多256个分配记录，单次资源
  仍受4MiB容量上限约束。固定域/数据页不与任何代码页重叠，不封为RX、不回收
  复用旧地址；真实最后leave之前表/borrow仍保活，retire只改变生命周期账目。
  容量、权限或range描述拒绝时回AOT，不放宽尺寸、不静默动态堆回退。
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
[EJIT] ... common T1 requested ... members=12 budget=64 owner_context=worker sdk_worker_task=0
[EJIT] ... FULL profile consumed ... samples=64 pairs=... words=... root_count=64 T2=published
[STAB231] HELD_CANCEL physical=1 borrow=1 retained=...
[STAB231] PASS cold: members=12 samples=64 pairs=... words=... profile=... commonT2=1; ... OK
[STAB231] stage=3 (3=PASS,4=FAIL; not product acceptance)
```

检查项：所有成员值和普通 live load/store 正确；普通 PGO enqueue/pending 为零；
本用例没有 SDK task-id 绑定，`sdk_worker_task=0` 是未知诊断，不是身份鉴权失败；
正确 worker 执行由内部上下文和实际 owner-worker 操作记录验证。
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

SDK 最终链接脚本也要合并全部共享输入并重新定义实际边界（lipo 两阶段已保留）：

```ld
.mc_shared : ALIGN(64) {
  __ejit_shared_start = .;
  KEEP(*(.mc_shared .mc_shared.*))
  __ejit_shared_end = .;
}
```

该段应沿用现有产品共享内存 REGION/loader映射，不另放入普通核私有 `.data`。
边界必须覆盖完整最终节，不沿用部分链接时的旧地址；检查器会拒绝缺失/重名边界、
ABS假地址、边界与节不一致，以及对象extent越界。仅边界正确不证明同物理coherence。

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
