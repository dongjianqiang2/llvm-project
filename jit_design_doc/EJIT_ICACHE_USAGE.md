# EJIT Inline Cache 产品侧使用指南

> 面向产品/业务侧的使用说明。设计细节见
> [EJIT_ICACHE_MULTIVERSION.md](EJIT_ICACHE_MULTIVERSION.md)（§13 单槽、聚类）与
> [EJIT_ICACHE_SHARED_TABLE.md](EJIT_ICACHE_SHARED_TABLE.md)（§3.1 聚类、§4 P2、§8 验证）。

## 1. 功能概述

EJIT 的 wrapper 每次调用 `ejit_entry` 函数时默认进 taskpool 查询当前特化版本。
Inline Cache 把该指针直接缓存在每函数的全局变量里：命中路径变成一条 load +
尾调用（aarch64_be 实测 4-6 cycle），不再进入运行时。

三个编译开关按层递进，**全部默认关闭、可任意组合，源码标注无需任何改动**：

| 开关 | 作用 | 每函数占用（共享内存） |
|---|---|---|
| `-mllvm -ejit-inline-cache` | 基础探测：每函数一张 `[16]^N` 指针表，按维度实参值直接索引 | 16^N × 8B（4 维函数 512KB） |
| `-mllvm -ejit-icache-single-slot` | 单槽坍缩：多个维度参数在一次测试里共享同一个值时，表坍缩为一个标量 | 8 B |
| `-mllvm -ejit-icache-cluster` | D-cache 聚类：所有函数的单元表连排进独立子段，8 个 8B 单元共享一条 64B 行 | 不变，且全区连排 |

三个开关均为模块级（整个编译单元生效）；任一开关关闭都不影响其余 EJIT
行为，输出等价于未开启。

> **平台前提（重要，先读）**：单槽模式的"命中加速"要求平台满足
> `icacheCrossCoreExecutable`——即 **4K-seal 形态**。其生效条件是构建同时开启
> `EJIT_SRE_CODE_POOL=ON` 与 `EJIT_CODE_POOL_4K_SEAL=ON`（**只开后者无效**：
> CMake 仅在两者同开时才把 define 传给 LLVMEJIT；host 预设默认
> `EJIT_SRE_CODE_POOL=OFF`，缓存里的 `4K_SEAL=ON` 因此空转，驱动实际接线
> legacy 逐核 prepareCodeFn_）。在"逐核准备执行权限"的平台，**标量槽一律不
> 发布**（正确性门：共享标量任何核都会读，未密封的指针会让他核跳到未准备
> 页）。此时单槽表现为：特化照常发生、结果正确，但每次调用都走 taskpool
> 慢路径并叠加探测开销——**比不开单槽的基础 icache 更慢**（维表槽不受该门
> 约束、照常直达）。因此 host / 非 4K-seal 平台不要开启单槽做性能评测；单槽
> 部署以 4K-seal 板为前提（板侧 aarch64_be 预设两者皆 ON）。聚类开关不受此
> 门影响。**一行判别法**：诊断开启时看运行日志有无
> `icacheFill DECLINE ... scalar slot shares one cell across cores`——出现即
> 该平台不发布标量槽。

## 2. 使用步骤

### 2.1 编译命令

```bash
# 基础：每函数 [16]^N 表
clang -O2 ... -mllvm -ejit-inline-cache my_app.c ...

# 单槽：多个维度参数共享一个值的部署形态
clang -O2 ... -mllvm -ejit-inline-cache -mllvm -ejit-icache-single-slot my_app.c ...

# 单槽 + 聚类（推荐组合）
clang -O2 ... -mllvm -ejit-inline-cache -mllvm -ejit-icache-single-slot \
  -mllvm -ejit-icache-cluster my_app.c ...
```

### 2.2 链接脚本

单元表默认放在跨核共享段 `.mc_shared`（与 taskpool 状态同区），任意核的
生命周期切换都能直接清空其他核的缓存。开启聚类后，单元表改放派生子段
`.mc_shared.ejit_icache`，板级链接脚本需要映射它：

```ld
.mc_shared.ejit_icache : ALIGN(64)
{
  KEEP(*(.mc_shared.ejit_icache))
} > 你的共享内存 region   /* 与 .mc_shared 同一个 region */
```

> **通配陷阱**：若脚本用 `*(.mc_shared*)` 通配匹配共享段，该通配会把派生段名
> 吞回 `.mc_shared` 原样交错——聚类静默失效。必须先把通配拆成精确的
> `*(.mc_shared)`，再加上面的条目。

**为什么值得开聚类**：未聚类时，单元与同 TU 的周期数组（时间窗切换时写入）
和 taskpool 状态（每次慢路径写入）挤在相同缓存行，邻居写入会把探测热行反复
驱逐；聚类后全镜像的单元区只有寥寥几行（8 单元/行），取一次常驻。单元本身
"读多写极少"（探测只读、写入只发生在每次特化和时间窗切换），聚行共享不会
引入伪共享。

### 2.3 运行时纪律（单槽模式契约）

单槽模式把"按维度取值隔离"交给了部署纪律：

- 同一次测试内，函数的所有 `ejit_dim` 参数必须恒等于同一个值；
- 取值变化只允许发生在进程/板级复位边界，或 `ejit_deactivate` → 修改配置 →
  `ejit_activate` 之后（生命周期切换会自动清空缓存槽，下一次调用重新特化并
  冻结新值）。

> **红线**：同进程内不切生命周期直接换值调用 = 静默命中旧特化 =
> **错误代码**，无任何诊断。这是设计使然（哨兵探测无守卫），契约必须由
> 产品侧保证。基础模式（表按维度索引）与聚类开关不受此契约影响。

### 2.4 链接后验证（一次性）

```bash
# 开聚类：应恰好一条输出段、起始地址 % 64 == 0、落在共享区窗口内
readelf -SW image.elf | grep ejit_icache

# 所有单元符号应地址连续、都在该段范围内
nm image.elf | grep __ejit_icache_fn_
```

共享区落位（P2 契约）代码侧无法自检，readelf/nm 是唯一检测手段；多核部署
务必在首次联调时验证一次。

## 3. 收益对照（每函数）

| 项目 | 基础表（[16]^N） | 单槽 | 单槽 + 聚类 |
|---|---|---|---|
| 共享内存占用 | 16^N × 8B（4 维 512KB） | 8 B | 8 B，且全区连排 |
| 探测指令 | N 级 GEP + load + 尾调用 | load + 尾调用 | 同左 |
| 每次切换的清空写 | 16^N 次 | 1 次 | 1 次（稠密顺序扫） |
| 探测行稳定性 | 与周期数组/blob 同行，易被邻居写驱逐 | 同左 | 独立子段，仅单元共享行 |

特化身份、生命周期回收、慢路径行为在三种模式下完全一致；单槽仅改变缓存
布局，不改变特化与失效语义。

## 4. 故障速查

| 症状 | 判定 | 处置 |
|---|---|---|
| 换值后结果仍是旧值 | 单槽契约违规：同进程裸换值，未走生命周期切换或复位边界 | 换值必经 `ejit_deactivate` → `ejit_activate`；确认同一次测试内维度参数恒同值 |
| 多核下只有本核命中 / 各核行为不一致 | P2：单元槽没落在跨核共享区——聚类段未映射（孤儿）或被通配吞回 | `readelf -SW` 核对 `.mc_shared(.ejit_icache)` 落位；通配脚本先拆 pattern 再加条目；`nm` 核对单元符号连续 |
| 开了开关但一直走慢路径 | ① 平台接线 per-core prepareCodeFn_（无 4K seal）时标量槽 fill 被拒——预期行为，taskpool 提供正确结果；② 日志出现 icache slot 哨兵不匹配（cell[0] != &MissFn）——共享段被 NOLOAD/零初始化，未携带初值数据；③ 生命周期实例未激活 | ① 功能正确无需处理，需命中加速则部署 4K-seal 形态；② 修正段属性；③ 先 `ejit_activate` 再调用 |
