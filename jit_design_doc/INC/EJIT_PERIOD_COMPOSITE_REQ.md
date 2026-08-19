# EJIT 增量需求：复合 period 与实例折叠 —— 标记方案设计讨论稿

> 状态：**讨论稿 v0.4.2**（2026-08-19）。§1/§2 为已确认的需求与决策；§3/§4 为语法与校验定义（本阶段聚焦对象）；§6/§7 为影响面预览与后续命题。v0.3 新增 **L3 特化折叠（D9）**：dim 级声明 + LCM 推导 + 折叠表达式替换。v0.3.1：**O-1 已决**（spec = 值域大小；缓存规格按折叠值域，见 §6），O-8/O-9 关闭。v0.4 新增 **D10 数组形态与维度规则、D11 成员 period**：数组 = 实例空间容器、pattern 判定规则（§3.7/§3.8）、C18-C21。v0.4.1 修正 §3.7 pattern 判定（嵌值安全模型）：剔除"常量"成分、嵌值资格 ⟺ 下标精确等于复核实例元组、现状 `g[cell+1]` 类嵌值无复核跟踪为洞；新增 O-11。**v0.4.2 修正判定时机（值级判定）**：嵌值资格不依赖下标"来源"、依赖"值"——替换后格子坐标与复核实例坐标都是常量，关卡直接比较 `L == cellIdx`（v0.4.1"判定必须在替换前"推翻）；clang/PASS1/偏移兜底零改动，关卡新增检查。自包含推演见 [EJIT_PATTERN_CONSTANT_MODEL.md](EJIT_PATTERN_CONSTANT_MODEL.md)。
> 前置阅读：[EJIT_IMPL_OVERVIEW.md](EJIT_IMPL_OVERVIEW.md)（现状实现整理）。
> 代码基线：branch `ejit_dev_spec4` @ `52040abd0c75`。

## 1. 背景与诉求

### 1.1 现状的三个 1:1 假设

| # | 假设 | 现状锚点 |
|---|---|---|
| H1 | dim 名即 period 名，一 dim 一 period | `ejit_dim("cell")` 与 `ejit_period_arr("cell")` 同名配对；`EJitLifecycleRegistry` 每名字一槽 |
| H2 | 实例身份 = (dimType, 原始实参值) | `enabled_[8][256]` / `version_[8][256]`、cache 版本复核、`ejit_activate/deactivate(name, cellIdx)` 同粒度 |
| H3 | 特殊化 key = 失效 key = 原始 dim 元组 | JIT 参数常量替换直接拿实参当 cellIdx；icache `[16]^numDims` 按原始实参索引 |

### 1.2 新诉求

**需求 R1 —— 多 dim 组合成 period（dim-period 关系 1:1 → 多对多）**：

```
dim1 → period1            （1:1，现状已有）
dim2 → period2            （1:1）
dim1 + dim3 → period13    （复合：实例粒度 = (dim1, dim3) 的笛卡尔组合）
dim2 + dim3 → period23    （复合）
```

**需求 R2 —— 实例映射多对一（折叠）**：

```
period23 的实例 = (dim2, dim3 % 10)
⇒ period23(dim2=0, dim3=0) 与 period23(dim2=0, dim3=10) 是同一个时间窗实例
```

形式化：period P 由有序 dim 列表 (d1..dk) 组成，每个 di 可选一个折叠变换 ti（缺省恒等），**P 的实例 = (t1(d1), ..., tk(dk))**。使能/失效/版本状态以 period 实例为粒度；JIT 特殊化仍以原始 dim 元组为粒度。

**需求 R3 —— 特殊化 key 折叠（L3）**：声明为折叠的 dim（如 `slot%10`）可进一步声明"特化折叠"——cell=0,slot=0 与 cell=0,slot=10 不仅共享 period 实例状态（R2），还**共享同一份 JIT 特化代码**。约束：slot 值本身不做常量假设（参数不替换）；但用于 period 数组下标的折叠表达式（`slot%10`）在编译期固化为折叠常量，使访问地址常量化、mayconst 优化可应用。

### 1.3 对现状实现的冲击

1. **命名空间分裂**：`dimId`（参数身份 → 决定特殊化空间）与 `periodId`（使能/失效粒度）必须分离。
2. **特殊化 key ≠ 失效 key**：dim3=0 与 dim3=10 编译出两份不同特化代码，但共享同一 period23 实例的 enable/version 状态——版本复核必须先经折叠映射到 period 实例再做。
3. **lc 守卫的 cellIdx** 不再是单个实参，而是多个实参经映射计算的结果。
4. **JIT 编译侧几乎不受影响**：特殊化仍替换原始 dim 值为常量，`dim3%10` 类索引算术被常量折叠自然消化；`may_const` 机制照常。
5. **icache 不受影响**：按原始 dims 索引，天然区分不同特殊化。

## 2. 设计决策记录（已确认）

| 决策 | 内容 |
|---|---|
| **D1 先声明后使用** | dim 与 period 需要显式声明；声明在 C/C++ 代码中通过属性载体完成（不引入 yaml/外部配置文件）；使用处找不到声明 → **编译失败** |
| **D2 使用处语法不变** | 参数上 `ejit_dim("name")`、数组上 `ejit_period_arr("name")`、函数上 `ejit_period_lc("name")` 写法保持现状；字符串语义变为"引用已声明的名字" |
| **D3 折叠走独立属性** | 折叠通过独立属性增量表达，不在名字字符串里内嵌表达式 |
| **D4 同名 1:1 声明合法** | 同名 dim "cell" 与 period "cell" 合法，语义与现状 1:1 等价；下游元数据编码与运行时注册机制不做破坏性改动 |
| **D5 声明逐条成行、属性增量追加** | 每个 dim/period 本体与每条属性（type/spec/fold）各占一条配置行、各自独立宏与载体；后续新增属性不影响既有声明行 |
| **D6 type/spec 恰好一次** | 每个 dim 的 type 与 spec 都**不允许缺省、不允许重复配置**；编译器强制检查 |
| **D7 fold 缺省规则** | fold 整行可缺省（= identity）；`EJIT_FOLD_MOD` 的 operand 可缺省，缺省取值 = 该 dim 的 spec |
| **D8 分层回退方向（应用细节暂缓）** | type（half-static/dynamic）用于分级回退：dynamic 变化导致全特化失效时，先退回"仅 half-static 视为常量"的中间 JIT 版本，再退 AOT。**应用实现为另一命题，本阶段只定语法** |
| **D9 特化折叠（L3）** | 特化 key 折叠为 **dim 级显式声明**（`ejit_dim_spec_fold(dim, enable)`，宏 `DEFINE_DIM_SPEC_FOLD`）；折叠参数不直接声明，由 AOT 从该 dim 参与的 period fold 声明推导：**operand = LCM(各 operand)、op 仅支持 MOD**（§3.4bis）；编译期语义 = **参数不替换 + 折叠表达式替换**（落点见 §6） |
| **D10 数组 = 实例空间容器** | period 数组的维度结构与 period 的 dim 列表对应（三种合法形态 §3.7）；各维尺寸 = 该 dim 折叠值域（`min(spec, fold operand)`，与 O-1 缓存规格一致）；数组线性下标 == 实例索引（行优先）；维度一致性为 **warning**（动态指针数组跳过检查）；维度实现独立（period 维度由声明推导、数组维度由用户声明，检查点比对） |
| **D11 成员 period** | 结构体成员可复用 `ejit_period_arr` 标记（Subjects 扩展 FieldDecl）；成员数组 = 嵌套 period 容器（分层激活）；**嵌套限一层**，违反 → **error**；成员 period 的 dim 与外层 period 的 dim 不重叠（error）；成员不注册独立全局，绑定 = (外层类型, 成员名, 字节偏移) 编码进外层元数据 |

## 3. 语法定义（本阶段聚焦）

### 3.1 声明载体与宏机制

声明属性附着在文件作用域"载体"静态变量上（纯声明载体，无运行期语义），约定集中在公共头文件（如 `ejit_period_defs.h`），所有使用 ejit 标记的 TU 必须包含：

- 每条配置行用 `__COUNTER__` 生成独立载体变量名（不依赖属性跨 redeclaration 合并语义）；
- 载体变量加 `unused` 抑制告警；`static` 使每个 TU 都有自己的元数据副本（AOT pass 逐 TU 可见），注册表条目运行时按名去重、冲突报错；
- `#name` 字符串化：用户写裸名字，宏转成属性要求的字符串字面量。

### 3.2 宏族定义

```c
/* ---------- 公共辅助宏（ejit_period_defs.h） ---------- */
#define EJIT_CAT_(a, b) a##b
#define EJIT_CAT(a, b) EJIT_CAT_(a, b)

/* 变参个数统计（0..8）与逐参字符串化（FOR_EACH），标准预处理技巧 */
#define EJIT_NARG(...)  EJIT_NARG_(,##__VA_ARGS__, 8,7,6,5,4,3,2,1,0)
#define EJIT_NARG_(...) EJIT_ARG_N(__VA_ARGS__)
#define EJIT_ARG_N(_0,_1,_2,_3,_4,_5,_6,_7,_8,N,...) N

#define EJIT_STR(x) #x
#define EJIT_STR_ALL_0()
#define EJIT_STR_ALL_1(a)       EJIT_STR(a)
#define EJIT_STR_ALL_2(a, ...)  EJIT_STR(a), EJIT_STR_ALL_1(__VA_ARGS__)
/* ... 以此类推到 EJIT_STR_ALL_8 ... */
#define EJIT_SELECT(_1,_2,_3,_4,_5,_6,_7,_8,NAME,...) NAME
#define EJIT_STR_ALL(...) EJIT_SELECT(__VA_ARGS__, \
    EJIT_STR_ALL_8, EJIT_STR_ALL_7, EJIT_STR_ALL_6, EJIT_STR_ALL_5, \
    EJIT_STR_ALL_4, EJIT_STR_ALL_3, EJIT_STR_ALL_2, EJIT_STR_ALL_1, \
    EJIT_STR_ALL_0)(__VA_ARGS__)

/* ---------- dim：本体 + 属性（各恰好一次） ---------- */
#define DEFINE_DIM(dim_name) \
  __attribute__((ejit_dim_decl(#dim_name))) \
  static int EJIT_CAT(ejit_dim_decl_anchor_, __COUNTER__) \
      __attribute__((unused));

#define DEFINE_DIM_TYPE(dim_name, dim_type) \
  __attribute__((ejit_dim_type(#dim_name, #dim_type))) \
  static int EJIT_CAT(ejit_dim_type_anchor_, __COUNTER__) \
      __attribute__((unused));

#define DEFINE_DIM_SPEC(dim_name, dim_spec) \
  __attribute__((ejit_dim_spec(#dim_name, dim_spec))) \
  static int EJIT_CAT(ejit_dim_spec_anchor_, __COUNTER__) \
      __attribute__((unused));

/* ---------- period：本体 + 组成 dim 列表（空列表 = 同名 1:1 sugar） ---------- */
#define DEFINE_PERIOD(period_name, ...) \
  EJIT_PERIOD_IMPL(EJIT_NARG(__VA_ARGS__))(period_name, __VA_ARGS__)
#define EJIT_PERIOD_IMPL(n) EJIT_CAT(EJIT_PERIOD_IMPL_, n)
#define EJIT_PERIOD_IMPL_0(p) \
  __attribute__((ejit_period_decl(#p))) \
  static int EJIT_CAT(ejit_period_decl_anchor_, __COUNTER__) \
      __attribute__((unused));
#define EJIT_PERIOD_IMPL_1(p, ...) EJIT_PERIOD_IMPL_N(p, __VA_ARGS__)
/* ... 2..8 同 EJIT_PERIOD_IMPL_1 ... */
#define EJIT_PERIOD_IMPL_N(p, ...) \
  __attribute__((ejit_period_decl(#p, EJIT_STR_ALL(__VA_ARGS__)))) \
  static int EJIT_CAT(ejit_period_decl_anchor_, __COUNTER__) \
      __attribute__((unused));

/* ---------- fold：可增量追加；operand 可缺省（→ 该 dim 的 spec） ---------- */
#define DEFINE_FOLD(...) EJIT_FOLD_SELECT(EJIT_NARG(__VA_ARGS__))(__VA_ARGS__)
#define EJIT_FOLD_SELECT(n) EJIT_CAT(DEFINE_FOLD_, n)
#define DEFINE_FOLD_3(period_name, dim_name, fold_op) \
  __attribute__((ejit_period_fold(#period_name, #dim_name, fold_op))) \
  static int EJIT_CAT(ejit_fold_decl_anchor_, __COUNTER__) \
      __attribute__((unused));
#define DEFINE_FOLD_4(period_name, dim_name, fold_op, operand) \
  __attribute__((ejit_period_fold(#period_name, #dim_name, fold_op, operand))) \
  static int EJIT_CAT(ejit_fold_decl_anchor_, __COUNTER__) \
      __attribute__((unused));

/* ---------- 特化折叠（L3）：dim 级声明；enable 开关；折叠参数由编译器从 period fold 推导（§3.4bis） ---------- */
#define DEFINE_DIM_SPEC_FOLD(dim_name, enable) \
  __attribute__((ejit_dim_spec_fold(#dim_name, enable))) \
  static int EJIT_CAT(ejit_spec_fold_anchor_, __COUNTER__) \
      __attribute__((unused));
```

> 实现注：`EJIT_STR_ALL_2..8`、`EJIT_PERIOD_IMPL_1..8` 为机械展开，clang 已验证 `##__VA_ARGS__`（GNU 扩展）与 `__COUNTER__`；C++20 下可改用 `__VA_OPT__`。

### 3.3 属性定义（Attr.td 签名）

| 属性 | 参数 | 约束（Sema 强制） |
|---|---|---|
| `ejit_dim_decl` | `StringArgument dim` | 每 dim 恰好一次 |
| `ejit_dim_type` | `StringArgument dim, StringArgument type` | 每 dim 恰好一次；type ∈ 白名单 `{half-static, dynamic}`（预留 `static`） |
| `ejit_dim_spec` | `StringArgument dim, IntArgument spec` | 每 dim 恰好一次；spec ∈ (0, 256] |
| `ejit_period_decl` | `StringArgument period, VariadicStringArgument dims` | 每 period 恰好一次；dims 均已声明；空 dims = 同名 1:1 sugar |
| `ejit_period_fold` | `StringArgument period, StringArgument dim, EnumArgument op, Optional IntArgument operand` | 每 (period, dim) 至多一条；period 必须声明且包含该 dim；operand 缺省 → Sema 填入该 dim 的 spec |
| `ejit_dim_spec_fold` | `StringArgument dim, BoolArgument enable` | 每 dim 恰好一次（D6）；dim 必须已声明；enable=true 时该 dim 必须存在非 identity 的 MOD fold 声明（C14/C15），operand 推导见 3.4bis |

折叠 op 枚举：`EJIT_FOLD_IDENTITY=0 / EJIT_FOLD_MOD=1 / EJIT_FOLD_DIV=2 / EJIT_FOLD_SHR=3 / EJIT_FOLD_AND=4`。

### 3.4 缺省规则（D7 细化）

1. **fold 整行缺省**：period 的某 dim 无 `DEFINE_FOLD` 行 → 变换为 identity。
2. **operand 缺省**：`DEFINE_FOLD(p, d, EJIT_FOLD_MOD)` → operand 取该 dim 的 spec。
3. **Sema 期填实**：fold 与 dim 声明同在公共头文件、同一 TU 可见，Sema 收集后直接把缺省 operand 补成 spec 数值写入元数据，运行时零推断；spec 不可见时直接报错（与 D1 严格模式一致）。
4. 观察：对合法值域（dim 值 < spec）而言，`v % spec ≡ v`——缺省 operand 的语义是"折叠机制统一走取模路径，不配置时退化为恒等"，行为安全。

### 3.4bis 特化折叠参数推导（D9 细化）

1. **operand = LCM**：AOT 收集该 dim 在所有 period 上的**非 identity** fold 声明（缺省 operand 按 3.4 填 spec 后），取各 operand 的最小公倍数作为特化 key 折叠模数。理由：entry 函数假设可能依赖**所有** period 的全局变量（不做访问闭包分析）——特化 key 折叠后相同必须蕴含**每个** period 的折叠值都相同，模数必须是各 operand 的公倍数，LCM 是最小安全值。
2. **op 仅支持 MOD**（C15）：identity（无 fold 声明）不参与推导；DIV/SHR/AND 或 op 不一致 → 编译错误。
3. **enable=false**：显式关闭 L3（等同无声明效果）；声明本身仍受 D6"恰好一次"约束，重复声明（无论取值）报错。
4. **运行时零推断**：LCM 在编译期算好写入元数据（§5），wrapper 插桩与 JIT 编译期直接消费，运行时无推导。
5. **正确性**：key 折叠相同 ⟹ instanceId 相同 ⟹ 对每个 period，替换常量 `instanceId mod operand` 相同，且等于实例真实折叠值 `slot mod operand`（因 operand | LCM）⟹ 替换常量与运行期语义一致，**无条件安全**（无需访问模式证明）。

### 3.5 端到端示例

```c
/* ejit_period_defs.h —— 所有使用 ejit 标记的 TU 必须包含 */
DEFINE_DIM(dim1)   DEFINE_DIM_TYPE(dim1, half-static)  DEFINE_DIM_SPEC(dim1, 16)
DEFINE_DIM(dim2)   DEFINE_DIM_TYPE(dim2, dynamic)      DEFINE_DIM_SPEC(dim2, 64)
DEFINE_DIM(dim3)   DEFINE_DIM_TYPE(dim3, dynamic)      DEFINE_DIM_SPEC(dim3, 32)

DEFINE_PERIOD(period1,  dim1)
DEFINE_PERIOD(period2,  dim2)
DEFINE_PERIOD(period13, dim1, dim3)
DEFINE_PERIOD(period23, dim2, dim3)

DEFINE_FOLD(period23, dim3, EJIT_FOLD_MOD, 10)
DEFINE_DIM_SPEC_FOLD(dim3, true)          /* L3：dim3 特化 key 折叠，operand 推导 = LCM(10) = 10 */

/* ---- 使用处：与现状完全一致 ---- */
__attribute__((ejit_period_arr("period1")))  struct Cfg1  g_cfg1[N1];
__attribute__((ejit_period_arr("period13"))) struct Cfg13 g_cfg13[N13];
__attribute__((ejit_period_arr("period23"))) struct Cfg23 g_cfg23[N23];

__attribute__((ejit_entry))
void process(int a __attribute__((ejit_dim("dim1"))),
             int b __attribute__((ejit_dim("dim2"))),
             int c __attribute__((ejit_dim("dim3")))) { ... }

__attribute__((ejit_period_lc("period13")))
void on_period13(int cell __attribute__((ejit_dim("dim1"))),
                 int sub  __attribute__((ejit_dim("dim3")))) { ... }
```

### 3.6 兼容与迁移

- 旧代码迁移成本 = 公共头文件加声明行：`DEFINE_DIM(cell)` + `DEFINE_DIM_TYPE(cell, ?)` + `DEFINE_DIM_SPEC(cell, ?)` + `DEFINE_PERIOD(cell)`（空列表 sugar）。
- 开放项：是否引入"同名隐式声明"让旧代码零迁移——倾向不引入，保持 D6"恰好一次"的单一规则。

### 3.7 period 数组的三种形态与 pattern 判定（D10 细化）

period P = (d1..dk)，各维折叠值域 **D_i = min(spec_i, fold operand_i)**（与 O-1 缓存规格一致）。三种合法容器形态：

| 形态 | 声明 | 维度 | 实例索引 |
|---|---|---|---|
| a 逐维 | `Data g[D1][D2]...[Dk]` | 每维 = 对应 dim 折叠值域 | C 行优先线性化（天然 == 实例索引） |
| b 展平 | `Data g[D]`，D = Π D_i | 总槽 = 实例空间 | 显式线性化表达式（如 `cell*10 + slot%10`） |
| c 嵌套成员 | 外层 `Data g[D1]` + 成员数组 `dim2_var[D2]`（§3.8） | 每层各自 = 折叠值域 | 分层实例索引 |

**嵌值判定规则（v0.4.2 值级判定）**：

1. 嵌值候选：访问下标替换后**可常量化**——`g[cell]` 由参数替换、`g[cell][slot%10]` 由折叠表达式替换命中（`urem(形参, fold operand)` 形态，全量化替换）→ GEP 全常量 → 数组线性下标 L 可算。
2. 嵌值资格（唯一判据，值级）：`L == 本次调用实例 cellIdx`（该全局所属 period 的实例索引）。`g[cell+1]` → L ≠ cellIdx → 不嵌，保持运行期读取；常量下标 `g[5]` 不再单独排除——与复核实例相等时嵌值且安全（v0.4.2）。
3. **判定时机（v0.4.2 修正）**：判定在**替换后**的关卡（JIT，StructFieldPass）做——格子坐标与复核实例坐标都是常量，直接比较。（v0.4.1 曾写"必须替换前判定"，错把"无法区分来源"推成"无法判定安全性"；安全性只看值，不看来源。）
4. **安全论证**：允许嵌值 ⟺ 访问指向的格子 == 本次调用的实例（复核覆盖的实例）。实例可**重新激活并重写数据**，而复核只覆盖本次调用 dims 推导的实例；嵌值与复核覆盖同一组格子 ⟹ 数据一变即版本提升 → 复核发现 → 重编译，永远一致。`g[cell+1]` 对应实例 (1) 不在 f_0（cell=0）的复核范围 (0) 内——实例 (1) 重激活重写时 f_0 无感知，数据过期。**该限制为无条件正确性要求**（实例生命周期与 type 无关）；"嵌入实例集复核"（O-11）是补偿优化而非替代。完整推演见 [EJIT_PATTERN_CONSTANT_MODEL.md](EJIT_PATTERN_CONSTANT_MODEL.md)。
5. b 形态展平索引：替换后由关卡值比较判定（`g[cell*10+slot%10]` → 58 == cellIdx ✓；`g[cell*10+5]` → 55 ≠ cellIdx ✗）；其他算子（/、`<<`、函数调用）→ 折叠替换不命中 → 保持运行期计算（advisory warning，C21）。
6. 取地址：`&g[...]` 元素地址在**同函数内**传递（SSA 拷贝/PHI）后解引用的 load 继续应用——JIT 编译期 direct GEP 全常量路径已支持（索引替换后全常量，`EJitStructFieldPass.cpp:230-269`）；L 检查作用于替换后 GEP 链（含经指针的链），无需新机制；**跨函数调用传递 → 放弃**（O-10）。
7. slot 值本身永不常量（spec_fold 语义）：`printf(cell, slot)` 不应用；只有访问下标中的折叠表达式固化为折叠常量。

维度一致性检查（**warning**，不阻止编译；动态指针数组跳过检查）：常量数组每维尺寸 == 对应 dim 折叠值域（a/c），b 形态总槽 == Π 折叠值域；维度实现独立——period 维度由声明推导、数组维度由用户声明，检查点比对。

### 3.8 成员 period（D11 细化）

结构体成员复用 `ejit_period_arr`（Subjects 扩展 FieldDecl；Sema 按语境分流——成员需外层容器检查）：

```c
struct Data {
    may_const_attr int xxx;
    __attribute__((ejit_period_arr("period2"))) int dim2_var[D2];  /* 成员 period 容器 */
} g[D1];   /* 外层 period1 容器（分层激活：cell 变化只失效外层，slot 变化只失效成员） */
```

- **注册**：成员不注册独立全局；绑定 = (外层数组类型, 成员名, 字节偏移) 编码进外层 `ejit_period_arr` 元数据的**成员描述表**（编译期常量偏移）。
- **版本复核**：复合访问 `g[cell].dim2_var[slot%10].may_const` 需**双 period 版本校验**（period1(cell) + period2(slot%10)）；cache entry 版本快照含嵌套 period 实例。
- **约束（违反 → error）**：嵌套限一层（成员数组元素不能再含 period 标记成员，C19）；成员 period 的 dim 与外层 period 的 dim 不重叠（正交性，C20）。
- **替代方案（非侵入，备选）**：声明层绑定宏 `DEFINE_PERIOD_MEMBER(period2, Data, dim2_var)`——struct 定义不动（第三方类型），AOT 按类型名+成员名关联；当成员属性无法加在类型上时使用。

## 4. Sema 校验矩阵

| # | 场景 | 结果 |
|---|---|---|
| C1 | `ejit_dim("X")` 参数 / `ejit_period_arr("P")` / `ejit_period_lc("P")`：名字未声明 | **错误** |
| C2 | dim 本体重复声明（两条 `DEFINE_DIM(dim1)`） | **错误**（D6） |
| C3 | dim 缺 type / type 重复 | **错误**（D6） |
| C4 | dim 缺 spec / spec 重复 | **错误**（D6） |
| C5 | type 不在白名单 | **错误** |
| C6 | spec ≤ 0 或 > 256 | **错误** |
| C7 | period 重复声明；组成 dim 未声明 | **错误** |
| C8 | fold：period 未声明 / period 不含该 dim / (period,dim) 重复 | **错误** |
| C9 | fold 缺省 operand 时该 dim 的 spec 不可见 | **错误**（Sema 期填实，见 3.4） |
| C10 | fold op 枚举非法 | **错误**（EnumArgument 自动） |
| C11 | 既有检查全部保留：函数 dim 数 ≤4、整型参数、lc 必须有同名 dim 参数、period 数组大小 ≤100、数组大小 vs 实例空间一致性 | 不变 |
| C12 | `ejit_dim_spec_fold` 的 dim 未声明 | **错误** |
| C13 | 同一 dim 重复 spec_fold 声明（含 enable 取值不同） | **错误**（D6） |
| C14 | spec_fold dim 在所有 period 上均无非 identity fold 声明（operand 无法推导） | **错误** |
| C15 | 相关 period fold 的 op 非 MOD（DIV/SHR/AND） | **错误**（L3 仅支持 MOD） |
| C16 | 推导 LCM > 256（instanceId 值域上界） | **错误** |
| C17 | （advisory）spec_fold dim 的函数体中无匹配 (MOD, operand) 形态的访问 | warning，不阻止（保守正确） |
| C18 | 数组每维尺寸 / 展平总槽与 period 折叠值域不一致（动态指针跳过） | **warning**（D10） |
| C19 | 嵌套 period 超过一层（成员数组元素含 period 标记成员） | **error**（D11） |
| C20 | 成员 period 的 dim 与外层 period 的 dim 重叠 | **error**（D11） |
| C21 | （advisory）非 dim 派生的 period 访问：展平索引含 {+, ×} 之外算子或独立常量偏置、dim 派生变换（`g[cell+1]`）、常量下标（`g[5]`）、非 dim 下标 | warning，不阻止；嵌值资格由关卡值级判定（§3.7 规则 2/3/5）——格子 == 复核实例时仍嵌值 |

新增诊断命名沿用 `err_ejit_*` 风格（`err_ejit_dim_redecl` / `err_ejit_dim_missing_type` / `err_ejit_dim_dup_type` / `err_ejit_dim_missing_spec` / `err_ejit_dim_dup_spec` / `err_ejit_bad_dim_type` / `err_ejit_bad_dim_spec` / `err_ejit_fold_*` 等）。

## 5. IR 元数据编码草案

载体全局上发射 `!ejit.metadata`（复用 `emitEjitGlobalMetadata` 机制，逐 TU 发射）：

```llvm
!ejit.metadata = distinct !{
  !{!"ejit_dim_decl",    !"dim3"},
  !{!"ejit_dim_type",    !"dim3", !"dynamic"},
  !{!"ejit_dim_spec",    !"dim3", i32 32},
  !{!"ejit_period_decl", !"period23", !"dim2", !"dim3"},
  !{!"ejit_period_fold", !"period23", !"dim3", i8 1, i32 10}   ; op=MOD, operand 已填实
  !{!"ejit_dim_spec_fold", !"dim3", i1 true, i32 10}   ; enable + LCM 编译期算好
}
```

- 缺省 operand 在 Sema 期填实（3.4），元数据永远存实际数值，运行时零推断。
- 特化折叠（L3）：`ejit_dim_spec_fold` 子节点存 enable 与推导好的 LCM；EJitOptimizer 从 bitcode 中的声明元数据解析替换规则（PASS1 需保留声明载体进 bitcode，见 §6）。
- 使用处元数据**完全不变**（`ejit_period_arr_ind` / `ejit_period_arr` 等照旧）——D4 的"下游编码不改"由此成立。
- 标签常量在 `EJitCommon.h` 增加 `TAG_EJIT_DIM_DECL` / `TAG_EJIT_DIM_TYPE` / `TAG_EJIT_DIM_SPEC` / `TAG_EJIT_PERIOD_DECL` / `TAG_EJIT_PERIOD_FOLD`。

## 6. 注册表与运行时影响预览

（本阶段只定标记；以下为影响面定位，type 的应用语义见 §7。）

| 组件 | 影响 |
|---|---|
| 注册表 | 新增 `EJIT_REG_PERIOD_DEF = 8`（沿用 5/6/7 的加法模式；40B ABI 不变）。name1=period 名，ptr=dim/fold 描述表，size=表项数；跨 TU 重复条目按名去重、冲突报错 |
| 命名空间 | `DimRegistry`（dim 名 → dimId + type 编号 + spec）与 `PeriodRegistry`（period 名 → periodId + 组成/折叠表）分离；type 字符串 → u8 编号（`static=0/half-static=1/dynamic=2`，预留）加速运行期计算 |
| 缓存尺寸（O-1 已决） | **spec = dim 值域大小**（取值 ∈ [0, spec)，C6 边界维持）。缓存规格：该 dim 有 L3 特化折叠 → 折叠值域 `min(spec, LCM)`（dim < spec 且折叠值 < LCM ⟹ 折叠值 < min(spec, LCM)，裸 LCM 在 spec < LCM 时浪费槽位）；无 L3 → 按 spec。替换写死的 `EJIT_ICACHE_DIM_SIZE=16` 与 SwitchController 的 256 |
| SwitchController | `enabled_/version_` 粒度从 (dimType, instance) 改为 **(periodId, 实例索引)**；实例索引 = 折叠后元组按声明顺序线性化 |
| 查找/版本复核 | wrapper 传原始 dims 不变；cache 层经运行时映射表把 dims 折叠成各 period 实例再比对版本；cache entry 存储 per-period 版本快照 |
| wrapper（PASS3） | spec_fold dim 实参插入 `urem %LCM`（一处算术）：icache GEP 下标、bucket cache key、编译请求 instanceId 三处统一取折叠值；普通 dim 照旧 |
| 编译/JIT | 非 L3 dim 照旧（参数替换，特殊化 key = 原始元组）；`EJitOptimizer` 对 spec_fold dim 改为：**参数不替换** + 模式匹配 (MOD, operand) 形态的折叠表达式并替换为 `ConstantInt(instanceId mod operand)` → InstCombine 折叠 GEP → `EJitStructFieldPass` 走全常量路径（**零改动**）；编译请求结构零改动（dims 全传，完整性校验照过） |
| PASS4 lc 插桩 | cellIdx 改为"组合实例"：声明元数据在同 TU 可见时内联计算（mod/div/shr 是廉价指令）；不可见时编译报错（严格模式）。`ejit_activate/deactivate` 签名保持 `(name, cellIdx)`，cellIdx 语义升级为实例索引 |
| icache | spec_fold dim 的 GEP 下标用折叠值（槽位自动共享）；维度尺寸改用 spec（O-1/O-9 联动） |
| PASS2 | period 数组注册携带组合信息（来自声明元数据，按名关联） |
| 数组编码（D10/D11） | `ejit_period_arr` 元数据扩展：完整维度列表（现状只最外层 count）+ 成员 period 描述表（类型, 成员名, 字节偏移）；`reAnnotateMayConst` 兜底匹配器升级为 pattern 判定（支持多动态索引，现状只允许一个）；JIT 期 direct GEP 全常量路径已覆盖取地址再解引用，无需新机制 |
| 嵌值安全（v0.4.2） | **关卡值级判定**（§3.7 规则 3/4）：JIT 期 StructFieldPass 新增检查——GEP 全常量 ⟹ 数组线性下标 L（段级，覆盖三种形态）⟹ `L == cellIdx(该 period)` 才嵌值；clang/PASS1/偏移兜底**零改动**。**现状洞**：`g[cell+1]` 类访问参数替换后 GEP 全常量即嵌值，且无版本复核跟踪——本修订同时是现状修复 |

## 7. 后续命题与开放问题

**命题 P1 —— type 的应用语义（D8 细化，暂缓）**：

- 分层回退模型：Tier-2 全特化（所有 dim 常量化）→ Tier-1 半特化（仅 half-static 常量化，dynamic 保留运行期实参）→ AOT。Tier-1 复用同一编译管线，仅配置"替换哪些参数"。
- 失效钩子：dynamic 数据运行期可变 ⇒ 现状"实例数据一次性写入"假设对 dynamic 不成立，写入路径需要版本提升接口；half-static 仍近似一次写入。
- Tier-1 是否需要独立 icache 层。
- 是否引入第三种 `static` type（编译期恒定）。

**开放问题**：

| 编号 | 问题 |
|---|---|
| O-1 | ~~spec 语义~~ **已决（v0.3.1）**：spec = 值域大小；缓存规格按折叠值域（见 §6） |
| O-2 | 隐式 1:1 声明（3.6）：倾向不引入 |
| O-3 | 折叠算子集合 `%N /N >>S &M` 是否覆盖业务；是否需要 dim 间线性组合 |
| O-4 | 各注册表容量：dim 数、period 数、每 period 实例空间上限 |
| O-5 | 跨 TU 一致性：以"声明头文件 + 运行时冲突检测"为准，还是需要链接级校验 |
| O-6 | 属性命名 `ejit_dim_decl/ejit_dim_type/ejit_dim_spec/ejit_period_decl/ejit_period_fold` 为暂定名 |
| O-7 | FOR_EACH/NARG 宏机制在 clang 全版本验证；period 组成 dim 数上限（现按 8） |
| O-8 | ~~L3 是否扩展 AND 等 op~~ **已决（v0.3.1）**：L3 仅支持 MOD，不扩展 |
| O-9 | ~~spec_fold dim 的缓存维度~~ **已决（v0.3.1）**：按折叠值域 `min(spec, LCM)`，见 O-1/§6 |
| O-10 | 取地址跨函数传递（指针参数/返回值携带 period 标签）：本轮不做，函数内直接传递已支持 |
| O-11 | （可选优化）嵌入实例集复核：把被放弃嵌值的实例集（如 `g[cell+1]` 的实例 (1)）纳入特化版本复核范围，恢复其嵌值资格——本轮不做（§3.7 规则 4 的关卡检查为无条件正确性要求，O-11 仅为补偿优化） |

## 8. 术语对照

| 术语 | 含义 |
|---|---|
| dim（维度） | 绑定到函数参数的身份；由 name/type/spec 三条声明构成；决定特殊化空间 |
| period（时间窗） | 由 1..N 个 dim 组成；实例空间为折叠后元组；使能/失效/版本粒度 |
| fold / 折叠 | dim 值 → period 实例分量的多对一变换（缺省恒等；MOD operand 缺省 = dim spec） |
| 特化折叠 spec_fold（L3） | dim 级声明（`DEFINE_DIM_SPEC_FOLD(dim, enable)`）：该 dim 不参与特化 key 的原始取值；编译期参数不替换、折叠表达式（MOD 形态）替换为折叠常量；operand = 相关 period fold 的 LCM |
| pattern 访问 | 索引成分 ∈ {dim 直用, dim 的 fold 表达式, 常量} 的 period 数组访问；满足才应用常量假设，否则整条访问保守不应用 |
| 实例索引 | 折叠后元组按声明顺序行优先线性化；数组线性下标 == 实例索引（D10 三种形态一致） |
| type | dim 的稳定性分类（half-static / dynamic，预留 static）——应用语义见命题 P1 |
| spec | dim 的值域/缓存尺寸参数：决定 icache 维度与状态表尺寸 |
| 实例 instance | period 的一个具体取值组合；对应 period 数组的一个下标 |
| 特殊化 specialization | 针对原始 dim 元组编译出的 JIT 代码版本 |
