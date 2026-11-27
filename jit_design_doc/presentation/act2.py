# -*- coding: utf-8 -*-
"""Act 2 — 机制：它到底怎么工作"""
from svgkit import svg, T, TL, RECT, PATH, LINE, arrow, pkt, box, lane, hv, vh, code, dspy
from data import M

ACT = 2


def s9_annotations():
    src = '''struct CellCfg {
  EJIT_PERIOD_CONST uint32_t cellType;   // 窗口内不变
  EJIT_PERIOD_CONST uint32_t cellId;     // 窗口内不变
  uint32_t trafficLoad;                  // 真变量：每次调用都可能不同
};

// 每个数组元素 = 一个独立的时间窗实例
EJIT_GROUP(cell) struct CellCfg g_cellCfg[16];

EJIT_ENTRY
void process_cell(EJIT_DIM(cell) uint8_t cellIdx)
{
  // JIT 时 cellType 已知 → 分支被折叠掉
  if (g_cellCfg[cellIdx].cellType == 0xFD)
    g_cellCfg[cellIdx].trafficLoad += 10;
  else
    g_cellCfg[cellIdx].trafficLoad += 20;
}

EJIT_GUARD(cell)
void update_cell_config(EJIT_DIM(cell) uint8_t cellIdx)
{
  g_cellCfg[cellIdx].cellType = 0xFD;    // 入口 deactivate、出口 activate
}'''
    c = code(src, 'c', 'sm', marks={2: (1, 'ice'), 3: (1, 'ice'), 8: (2, 'ice'), 11: (3, 'ice'), 10: (4, 'ice'), 21: (3, 'ice'), 20: (5, 'red')},
             cap='ejit_test/ejit_attr_test.c（节选）', maxw=692)
    ann = [
        ('EJIT_PERIOD_CONST', 'ice', '1', '字段：窗口内可视为常量。<code>volatile</code> 除外；位域暂不支持。'),
        ('EJIT_GROUP(name)', 'ice', '2', '全局变量：属于哪个时间窗。<code>"static"</code> 永远激活；数组每个元素是一个实例。'),
        ('EJIT_DIM(name)', 'ice', '3', '整型参数：特化维度，= 时间窗数组的下标。每个函数最多 4 个。'),
        ('EJIT_ENTRY', 'violet', '4', '函数：值得特化的入口。不支持递归；副作用须幂等（首次调用走 AOT）。'),
        ('EJIT_GUARD(name)', 'red', '5', '函数：会修改窗口数据。编译器在入口插 deactivate、每个 return 前插 activate。'),
    ]
    rows = ''.join(f'''<div class="ann step" data-step="{st}"><span class="mono-tag {col}">{tag}</span><p>{desc}</p></div>''' for tag, col, st, desc in ann)
    body = f'''<div class="grid g-52-48" style="gap:26px">
<div data-anim="left">{c}</div>
<div data-anim="right" style="display:flex;flex-direction:column;gap:6px;justify-content:center">{rows}
<p class="small step" data-step="5" style="margin:12px 4px 0">另有 <code>EJIT_FREE_DIM</code> / <code>EJIT_BOUND_PTR</code>（第三幕）。定义 <code>EJIT_DISABLE</code> 可让 7 个宏全部展开为空 —— <b>同一份源码，带 JIT 和不带 JIT 两种构建</b>。</p></div>
</div>'''
    notes = '''
<p>用户面就是 5 个宏。按 → 逐个出现，左边代码对应行会亮。</p>
<p>最值得强调的是 <code>EJIT_GUARD</code>：它是整个安全模型的唯一入口。业务要改窗口里的数据，必须经过一个 guard 函数；编译器替你在前后插上 deactivate/activate，忘了标会被 Sema 警告。</p>
<p>补一句 <code>EJIT_DISABLE</code>：这是给产品做 A/B 的开关，标注全消失，代码照常编译运行。</p>
'''
    return dict(act=ACT, name='annotations', kicker='第二幕 · 用户视角', title='用户的全部工作：7 个宏里的 5 个', body=body, steps=5, notes=notes)


def s10_metadata():
    ir = code('''@g_cells = external global [16 x %struct.CellCfg], !ejit.metadata !0

define i32 @check_cell(i8 noundef zeroext %0) !ejit.metadata !9 {
  %2 = zext i8 %0 to i64
  %3 = getelementptr inbounds nuw [16 x %struct.CellCfg], ptr @g_cells, i64 0, i64 %2
  %4 = load i32, ptr %3, !ejit.period_const !17      ; mode      ← 标注挂在这
  %5 = getelementptr inbounds nuw i8, ptr %3, i64 4
  %6 = load i32, ptr %5, !ejit.period_const !17      ; threshold ← 标注挂在这
  %7 = getelementptr inbounds nuw i8, ptr %3, i64 8
  %8 = load i32, ptr %7                              ; gain      ← 真变量：没有标注
  %9  = icmp eq i32 %4, 1
  %10 = select i1 %9, i32 20, i32 5
  %11 = icmp eq i32 %4, 0
  %12 = select i1 %11, i32 10, i32 %10
  %13 = mul nsw i32 %6, %12
  %14 = add nsw i32 %13, %8
  ret i32 %14
}

!0  = distinct !{!1, !2, !3}
!1  = !{!"ejit_group_indexed", !"cell", i32 16}     ; g_cells 是 cell 时间窗 · 16 个实例
!2  = !{!"ejit_period_const_field", i32 0}          ; 偏移 0 的字段窗口内不变
!3  = !{!"ejit_period_const_field", i32 4}          ; 偏移 4 的字段窗口内不变
!9  = distinct !{!10, !11}
!10 = !{!"ejit_entry"}                              ; 这是 EJIT_ENTRY
!11 = !{!"ejit_dim", !"cell", i32 0}                ; 参数 0 是 cell 维度
!17 = !{}''', 'll', 'xs', hl={6: 'ice', 8: 'ice', 10: 'amber'}, cap='真实 dump · ejit_dump_test.c · 略去 tbaa/align', maxw=777)
    body = f'''<div class="grid g-58-42" style="gap:26px"><div data-anim="left">{ir}</div>
<div data-anim="right">
  <div class="card pad-lg rule-ice" style="margin-bottom:16px"><div class="card-h">标注变成了什么</div>
    <ul class="ul">
      <li class="li"><b>load 级</b>：<code>!ejit.period_const</code> 直接挂在 load 指令上 —— 仿照 volatile 的传递链，但用 metadata 而不是指令位</li>
      <li class="li"><b>GV 级</b>：<code>!ejit.metadata</code> 记录数组属于哪个窗口、哪些字节偏移是常量字段</li>
      <li class="li"><b>软标注</b>：任何 pass 丢掉它，只会少一次优化，不会错</li>
    </ul></div>
  <div class="card pad-lg rule-violet"><div class="card-h">为了保住它，我们动了两处核心 LLVM</div>
    <p class="small" style="color:var(--ink-2)">instruction metadata 很脆，任何重写 load 的优化都可能把它丢掉。于是：</p>
    <ul class="ul">
      <li class="li"><span class="k">PASS1</span>在<b>一切优化之前</b>抽 bitcode，再按 GV 偏移表把丢掉的标注挂回去</li>
      <li class="li"><span class="k">IR</span><code>FixedMetadataKinds.def</code>：给它一个固定编号（42）</li>
      <li class="li"><span class="k">Local.cpp</span>复制 load 时保留它（<code>copyMetadataForLoad</code>）</li>
    </ul></div>
</div></div>'''
    notes = '''
<p>左边是从 <code>ejit_dump_test.c</code> 真实 dump 出来的 IR，只去掉了 tbaa/align 噪声。两条 period_const load、一条普通 load，一目了然。</p>
<p>关键设计是"软标注"：metadata 不是语义，丢了不会错。这也是我们敢把 LLVM 的整套优化流水线直接搬过来跑的原因——它不认识这个标注，顶多忽略它。</p>
<p>右边卡片是给编译器同学的彩蛋：为了不让标注在优化里丢失，核心 LLVM 加了一个固定 metadata kind，并在复制 load 的地方多保留一行。整个项目对核心 LLVM 的"语义性"改动就这么点，其余都是 <code>#ifdef</code> 裁剪。</p>
'''
    return dict(act=ACT, name='metadata', kicker='第二幕 · 标注的落地', title='Clang 把属性变成 metadata，挂在 load 上', body=body, notes=notes)


def s11_aot_pipeline():
    W, H = 1408, 504
    y, h = 0, 210
    zh = lambda s: ' zh' if any('一' <= c <= '鿿' for c in s) else ''
    nodes = [
        (0, 210, '', 'Clang CodeGen', ['属性 → IR + metadata'], 'box'),
        (241, 300, 'PASS1', 'ejit-register-bitcode', ['在一切优化之前', '抽调用闭包 · 预优化 · 序列化'], 'box-ice'),
        (572, 300, 'PASS2–4', 'ejit-aot-module', ['注册时间窗 · 插 wrapper', 'guard：deactivate / activate'], 'box-ice'),
        (903, 226, '', 'O2 / O3', ['标准流水线', '顺手优化 wrapper', '的 fallback 路径'], 'box'),
        (1160, 248, '', '可执行文件', ['.text + @__ejit_bitcode', '+ 注册表段'], 'box-ink'),
    ]

    def node(x, w, tag, title, lines, cls):
        ts, ls = 20, 15
        th, lh = ts * 1.3, ls * 1.5
        total = (24 if tag else 0) + th + len(lines) * lh
        cy = y + (h - total) / 2
        o = [RECT(x, y, w, h, cls, 14)]
        if tag:
            o.append(T(x + w / 2, cy + 13, tag, 't-ice up b700', anchor='middle', size=12.5))
            cy += 28
        o.append(T(x + w / 2, cy + th * 0.78, title, 't-ink b700' + zh(title), anchor='middle', size=ts))
        cy += th
        for i, ln in enumerate(lines):
            o.append(T(x + w / 2, cy + lh * (i + 0.74), ln, 'zh t-3', anchor='middle', size=ls))
        return ''.join(o)

    out = []
    for i, (x, w, tag, title, lines, cls) in enumerate(nodes):
        out.append(f'<g class="step plain" data-step="{1 if i < 2 else 2}">')
        out.append(node(x, w, tag, title, lines, cls))
        out.append('</g>')
    for i in range(len(nodes) - 1):
        x1, x2 = nodes[i][0] + nodes[i][1], nodes[i + 1][0]
        out.append(f'<g class="step plain" data-step="{1 if i == 0 else 2}">{arrow(f"M{x1 + 8} {y + h / 2} H{x2 - 10}", "ink", extra="stroke-width=\"2\"")}</g>')
    why = [
        (391, ['为什么排最前：load 级 metadata', '是 instruction metadata，', '任何优化都可能把它丢掉']),
        (722, ['为什么在 O2 之前：让 O2 把', 'wrapper 的 fallback 也优化了；', 'noinline 在 inliner 之前生效']),
    ]
    for cx, lines in why:
        for j, ln in enumerate(lines):
            out.append(T(cx, h + 34 + j * 26, ln, ('zh t-3' if j < 2 else 'zh t-amber b700'), anchor='middle', size=15))
    out.append(LINE(0, h + 116, 1408, h + 116, 'ln'))

    def chip(x, y, text, w, cls='box-ice', tc='t-ink'):
        return RECT(x, y, w, 58, cls, 10) + T(x + w / 2, y + 35, text, tc, anchor='middle', size=15, weight=700)
    y1, y2 = h + 142, h + 242
    out.append(T(0, y1 + 35, '非 LTO', 'zh t-ink b700', size=16))
    out.append(chip(120, y1, 'PASS1', 92) + chip(228, y1, 'LowerFreeDim', 140) + chip(384, y1, 'PASS2–4', 116)
               + chip(516, y1, '———————————   O2 / O3   ———————————', 452, 'box-ink') + chip(984, y1, 'codegen', 112, 'box'))
    out.append(T(1128, y1 + 35, '最常见的情况', 'zh t-3', size=14.5))
    out.append(T(0, y2 + 35, 'LTO', 't-ink b700', size=16))
    out.append(chip(120, y2, 'PASS1', 92) + chip(228, y2, 'O2 pre-link', 150, 'box-ink') + chip(394, y2, 'PASS1′', 92)
               + chip(502, y2, 'O2 post-link', 170, 'box-ink') + chip(688, y2, 'LowerFreeDim', 140) + chip(844, y2, 'PASS2–4', 116) + chip(976, y2, 'codegen', 112, 'box'))
    out.append(T(1120, y2 + 35, 'post-link 再各跑一次', 'zh t-3', size=14.5))
    body = svg(''.join(out), W, H) + '''
<p class="p step" data-step="3" style="font-size:24px;margin-top:20px">PASS1 抽出的是<b>还没被任何优化碰过</b>的 bitcode；wrapper 留在函数入口。<b>所有特化决策，都推迟到窗口打开之后。</b></p>'''
    notes = '''
<p>AOT 侧一共就做两件事：备份一份干净的 bitcode（PASS1），以及在函数入口插一段探针（PASS2–4）。其它全是 LLVM 原有流水线。</p>
<p>两个"为什么"值得讲：PASS1 排在最前，是因为 load 级 metadata 经不起优化；PASS2–4 排在 O2 前面（非 LTO），是故意的——这样 O2 会把探针的 fallback 路径一起优化掉，而且 noinline 属性能在 inliner 之前生效，防止 wrapper 被内联进调用者。</p>
<p>下面两条是精确的 pass 顺序，给编译器同学看。挤在中间的 LowerFreeDim 是 EJIT_FREE_DIM 的下推（PassBuilderPipelines.cpp:1679，无开关、永远跑）；LTO 路径下 post-link 还会各再跑一次。</p>
'''
    return dict(act=ACT, name='aot-pipeline', kicker='第二幕 · AOT 编译期', title='两阶段 AOT：先备份 bitcode，再插探针，然后才轮到 O2', body=body, steps=3, notes=notes)


def s12_wrapper():
    hot = code('''define i32 @f(i8 zeroext %idx) {
  %in   = icmp ult i8 %idx, 16
  %i    = zext nneg i8 %idx to i64
  %slot = getelementptr [16 x ptr], ptr @__ejit_icache_fn_f,
                         i64 0, i64 %i        ; 我的 cell
  %p    = select i1 %in, ptr %slot,
          ptr @f.icache_miss                 ; 域外 → 不可变的 miss 哨兵
  %fn   = load atomic ptr, ptr %p monotonic  ; 里面是谁？
  %r    = musttail call i32 %fn(i8 zeroext %idx)
  ret i32 %r                                ; 跳过去，不建栈帧
}''', 'll', 'sm', hl={2: 'ice', 3: 'ice', 4: 'ice', 5: 'ice', 6: 'ice', 7: 'ice', 8: 'ice', 9: 'ice'}, cap='热路径 · 真实 IR（clang -O2，改名）', maxw=654)
    miss = code('''define internal i32 @f_miss(i8 zeroext %idx) noinline {
  %fi = load i32, ptr @__ejit_funcidx_f   ; init 时填的稠密 funcIndex
  br i1 (icmp eq %fi, -1), label %fallback, label %call
call:                            ; 慢路径才有栈帧
  %dims = alloca [1 x { i32, i32 }]              ; {dimType, instanceId}
  store (load @__ejit_dimtype_cell), ptr %dims
  store (zext i8 %idx to i32), ptr %dims[0].1
  %st  = call @ejit_taskpool_compile_or_get(%fi, %dims, 1, %fn, %b)
  %p   = load ptr, ptr %fn
  %hit = and (eq %st, 0), (ne %p, null)
  br i1 %hit, label %dispatch, label %fallback
dispatch:                        ; 命中共享缓存，还没进 cell
  %r   = call %p(%idx)
  call @ejit_taskpool_release_read(%b)            ; 用完归还读令牌
  ret %r
fallback:
  …原函数体…                       ; 没编好 / 没激活：就是普通 AOT
  ret %r.aot
}''', 'll', 'sm', hl={6: 'amber', 7: 'amber', 8: 'amber'}, cap='慢路径 · f_miss（真实 IR 的节选改写）', maxw=654)
    # shape of one call: one basic block, plus an out-of-line cold path
    Wc, Hc = 688, 184
    c = [RECT(0, 0, Wc, 60, 'box-ice', 12),
         T(20, 26, '热路径 · 一个基本块：读 cell[idx]，读到谁就跳谁', 'zh t-ink b700', size=15),
         T(20, 48, 'cmp → add → csel → ldr → br · 没有分支，没有栈帧', 'zh t-3', size=13),
         arrow('M145 64 V110', 'ink'), T(159, 93, '哨兵（还没编译）', 'zh t-3', size=12.5),
         arrow('M543 64 V110', 'ice'), T(529, 93, 'musttail 尾跳 · 不建栈帧', 'zh t-ice b500', anchor='end', size=12.5),
         RECT(0, 118, 290, 60, 'box-amber', 12),
         T(16, 143, 'f_miss · noinline · 冷', 'zh t-ink b700', size=15),
         T(16, 164, '拼维度 → compile_or_get', 'zh t-3', size=13),
         arrow('M294 148 H394', 'amber', dash=True), T(344, 138, '编译 → 发布', 'zh t-amber b500', anchor='middle', size=12),
         RECT(398, 118, 290, 60, 'box-ice', 12),
         T(414, 143, '特化代码 f[3, 0]', 'zh t-ink b700', size=15),
         T(414, 164, '常量已经折进去 · 下一次就命中', 'zh t-3', size=13)]
    cfg = svg(''.join(c), Wc, Hc, extra='style="margin-top:14px"')
    body = f'''<div class="grid g2" style="gap:26px"><div class="step" data-step="1">{miss}</div><div class="step" data-step="2">{hot}{cfg}</div></div>
<div class="grid g3 step" data-step="3" style="margin-top:14px">
  <div class="card pad-sm rule-ice"><div class="card-h">一个基本块，没有分支</div><p class="small" style="color:var(--ink-2)">≤ 2 维时 cell 表在<b>定义时</b>就预填 <code>&amp;f_miss</code>，读出来永远可调用。域外下标不判跳，走 <code>select</code>。</p></div>
  <div class="card pad-sm rule-ice"><div class="card-h">3–4 维：判空 + expect</div><p class="small" style="color:var(--ink-2)">表太大（16³ / 16⁴ 个重定位），改成零初始化 + 判空，分派到 dispatch / miss。</p></div>
  <div class="card pad-sm rule-amber"><div class="card-h">热路径不拿读令牌</div><p class="small" style="color:var(--ink-2)">探针里塞不进别的东西。安全性来自另一条规则：<b>特化代码永不释放</b>（drain ≠ reclaim）。</p></div>
</div>'''
    notes = '''
<p>这一页是"一次调用到底发生了什么"的真实答案。按 → 先看慢路径，再看热路径。</p>
<p>慢路径 <code>f_miss</code> 是 noinline 的：funcIndex 守卫、拼维度、问运行时要函数指针、命中就调并归还读令牌、否则跑原函数体。它在第一次调用、以及没激活的时候才会被走到。</p>
<p>热路径只有一个基本块：算 cell 地址、（域外的用 select 换成 miss 哨兵）、原子读、musttail 尾调用。没有判空、没有分支、没有栈帧。能用这种"无脑跳"的前提，是 cell 表定义时就填好了 &amp;f_miss —— 读到谁就跳谁。</p>
<p>右下那张小图是整件事的形状：上面一条是热路径，往左下落到慢路径，往右下直接跳进特化代码；慢路径编译完把结果发布进 cell，下一次调用就从右边走了。</p>
<p>第三步三张卡是设计取舍：为什么不拿读令牌？因为热路径已经塞不进别的东西。代价是特化代码永远不能释放 —— 如果哪天要释放，就得把 inline cache 关掉。这是有意为之的"用空间换确定性"。</p>
'''
    return dict(act=ACT, name='wrapper', kicker='第二幕 · 一次调用', title='wrapper 的真身：一个基本块的热路径，一个 noinline 的慢路径', body=body, steps=3, notes=notes)


def s13_binary():
    W, H = 640, 540
    rows = [
        ('.text', 'AOT 代码：每个 EJIT_ENTRY = 探针 + f_miss（含原函数体）', 'box'),
        ('.rodata', '字符串、常量', 'box'),
        ('.ejit_bitcode', '@__ejit_bitcode · BITCODE / SYMBOL 注册表', 'box-ice'),
        ('.ejit_group', 'PERIOD_ARRAY / LIFECYCLE / FUNCINDEX / ICACHE 注册表', 'box-ice'),
        ('.data · .bss', '__ejit_dimtype_cell · __ejit_funcidx_f · __ejit_icache_fn_f[16]', 'box-amber'),
        ('.init_array', 'ejit_auto_register（ctor 路径；裸核没有 ctor → 走静态表遍历）', 'box-violet'),
    ]
    out = []
    y = 0
    for name, desc, cls in rows:
        out.append(RECT(0, y, W, 70, cls, 8))
        out.append(T(18, y + 30, name, 't-ink b700', size=15))
        out.append(T(18, y + 54, desc, 'zh t-3', size=13))
        out.append(RECT(0, y, 4, 70, {'box-ice': 'solid-ice', 'box-amber': 'solid-amber', 'box-violet': 'solid-violet'}.get(cls, 'solid-ink'), 2))
        y += 82
    out.append(T(0, H - 4, '单函数样例：bitcode 2720 B · 4 入口 demo：6548 B · 注册表约 190 B / 入口', 'zh t-3', size=13))
    left = svg(''.join(out), W, H)
    reg = code('''@__ejit_bitcode = internal constant [2720 x i8] c"BC\\C0\\DE…"

@.ejit.registry.bitcode = private constant [...] [
  { i32 0, ptr @"f",       ptr null, ptr @__ejit_bitcode, i64 2720 },
  { i32 3, ptr @"g_cells", ptr null, ptr @g_cells,        i64 16   },
  ...], section ".ejit_bitcode"          ; 0 = BITCODE · 3 = SYMBOL

@.ejit.registry.funcindex = private constant [...] [
  { i32 6, ptr @"f", ptr null, ptr @__ejit_funcidx_f, i64 0 },
  ...], section ".ejit_group"            ; 6 = FUNCINDEX

@llvm.global_ctors = appending global [... {
  i32 65535, ptr @ejit_auto_register, ptr null }]''', 'll', 'sm', cap='真实输出 · 一个 EJIT_ENTRY 的模块', maxw=734)
    body = f'''<div class="grid g-45-55" style="gap:28px"><div data-anim="left">{left}</div><div data-anim="right">{reg}
<div class="card pad-lg rule-violet" style="margin-top:18px"><div class="card-h">两套注册路径</div>
<p class="small" style="color:var(--ink-2);font-size:15.5px">有构造函数的平台走 <code>ejit_auto_register</code>（优先级 65535，最后跑）。裸核没有 ctor：<code>ejit_init</code> 改为遍历 <code>.ejit_*</code> 段里的静态表，边界靠链接脚本给出 <code>__start_ / __stop_</code>。表是纯 POD，每项 <code>{{type, name, _, addr, size}}</code>。</p></div></div></div>'''
    notes = '''
<p>二进制里多了什么？三样东西：一段 bitcode（在 <code>.ejit_bitcode</code> 段）、几张很小的注册表（<code>.ejit_group</code> 段），以及几个 init 时才填的全局变量。</p>
<p>注册表是 POD 数组。这样设计是为了裸核：没有构造函数、没有动态加载，运行时只要知道段的起止地址，遍历一遍就把所有东西注册完了。</p>
<p>诚实成本：每个 EJIT_ENTRY 的逻辑在 Flash 里存了两份（AOT 机器码 + bitcode）。bitcode 很小（每条指令几个字节的量级），真正贵的是 AOT 函数体本来就在。</p>
'''
    return dict(act=ACT, name='binary', kicker='第二幕 · 产物', title='二进制里多了什么：一段 bitcode、几张表、几个槽', body=body, notes=notes)


def s14_loop():
    W, H = 1408, 510
    o = []
    o.append(lane(0, 0, 1408, 156, '业务核 Core 0..N', 'amber', '· 调用方，永远不等'))
    o.append(lane(0, 176, 1408, 156, '共享段 EJIT_SHARED_SECTION', 'ice', '· 全 POD + 原子变量，所有核同 VA 映射'))
    o.append(lane(0, 352, 1408, 156, 'Worker 核', 'violet', '· 唯一的编译者，CAS 选出来的'))
    o.append(box(24, 46, 150, 64, '调用 f(3)', None, 'box'))
    o.append(box(226, 46, 250, 64, 'probe', ['查 cell 表 / 共享缓存'], 'box'))
    o.append(box(560, 16, 380, 52, '入队 compile_or_get', ['funcIndex CAS 去重 · 队满/重复就回滚'], 'box-amber', tsize=14, lsize=12))
    o.append(box(560, 84, 380, 54, '先跑 AOT 函数体', ['调用方立刻返回，不等编译'], 'box-amber', tsize=14, lsize=12))
    o.append(box(1080, 12, 304, 56, '命中 → 直跳特化代码', None, 'box-ice', tsize=14))
    o.append(box(226, 226, 250, 60, '结果缓存', ['32 桶 × 16 槽 · 每桶一把 RwLock'], 'box'))
    o.append(box(560, 226, 170, 60, 'SwitchController', ['enabled + version'], 'box', tsize=13, lsize=11.5))
    o.append(box(770, 226, 370, 60, 'TaskQueue', ['Vyukov 环 1024 槽 · 多 producer 抢 head'], 'box'))
    o.append(box(1080, 396, 304, 60, 'pollOne 出队', ['空转就 yield'], 'box'))
    o.append(box(810, 396, 220, 60, '检查点 1', ['version 不对就丢弃'], 'box', tsize=14))
    o.append(box(500, 396, 262, 60, 'JIT 编译', ['bitcode → PASS6 → LLVM → 机器码'], 'box-violet', tsize=14))
    o.append(box(226, 396, 226, 60, '检查点 2 → publish', ['等读者退场再写'], 'box-ice', tsize=13.5))
    wires = [
        ('p-a1', 'M174 78 H220', 'ink', False), ('p-a3', 'M476 78 H518 V111 H554', 'amber', False),
        ('p-a4', 'M476 64 H518 V42 H554', 'amber', True), ('p-a2', 'M351 46 V40 H1074', 'ice', False),
        ('p-x1', 'M900 68 V220', 'amber', False), ('p-x2', 'M1120 286 V330 H1232 V390', 'ink', False),
        ('p-c1', 'M1080 426 H1036', 'ink', False), ('p-c2', 'M810 426 H768', 'violet', False), ('p-c3', 'M500 426 H458', 'violet', False),
        ('p-x3', 'M339 396 V292', 'ice', False), ('p-x4', 'M339 226 V168 Q339 156 351 156 V116', 'ice', True),
        ('p-v', 'M645 286 V330 H920 V390', 'red', True),
    ]
    for pid, d, col, dash in wires:
        o.append(arrow(d, col, dash, pid=pid, extra='stroke-width="1.5"'))
    o.append(T(355, 30, '命中', 't-ice b700', size=12))
    o.append(T(520, 100, 'miss', 't-amber b700', size=12))
    o.append(T(1196, 348, 'version 校验', 't-red b700', anchor='middle', size=12))
    o.append(T(345, 186, '回填 cell · 写缓存', 't-ice b700', size=12))
    o.append(pkt('p-a1', 1, 500) + pkt('p-a3', 1, 600, 250) + pkt('p-a1', 4, 500) + pkt('p-a2', 4, 1100, 300))
    o.append(pkt('p-a4', 2, 500) + pkt('p-x1', 2, 700, 400) + pkt('p-x2', 2, 1100, 900) + pkt('p-c1', 2, 300, 1900) + pkt('p-c2', 2, 400, 2100) + pkt('p-c3', 2, 700, 2400))
    o.append(pkt('p-x3', 3, 900, 400) + pkt('p-x4', 3, 900, 1400))
    caps = [
        (1, '<b>第 1 次调用</b>：probe 未命中 → 入队（同一个 (函数, 实例) 在队列里只会有一份）→ 调用方<b>立刻</b>去跑 AOT 函数体。'),
        (2, '<b>worker 取任务</b>：先核对 version，再编译。此时业务核在正常干活，没有任何停顿。'),
        (3, '<b>发布</b>：再核对一次 version（期间可能已经 deactivate），等桶里的读者全部退出，才写缓存、回填 cell。'),
        (4, '<b>第 2 次调用</b>：probe 直接命中，一条 musttail call 跳进特化代码。整个过程调用方只多付了一次 AOT。'),
    ]
    caphtml = ''.join(
        f'<p class="cap step plain" data-step="{st}"' + (f' data-until="{st + 1}"' if st < 4 else '') +
        f' style="position:absolute;left:0;top:6px;font:400 17px/1.4 var(--sans);color:var(--ink-2);margin:0">{txt}</p>'
        for st, txt in caps)
    lat = f'''<div class="grid g3" data-anim="fade" style="--d:.5s;gap:16px;margin-top:0">
  <div class="kpi c-violet" style="padding:12px 18px"><div class="l">入队 → 特化可用 · 进程内第一次</div><div class="v" style="font-size:34px">{M['lat_first_ms'][0]:.0f}–{M['lat_first_ms'][1]:.0f}<small>ms</small></div></div>
  <div class="kpi c-violet" style="padding:12px 18px"><div class="l">之后每个特化</div><div class="v" style="font-size:34px">{M['lat_tiny_ms'][0]:.0f}–{M['lat_warm_ms'][1]:.0f}<small>ms</small></div></div>
  <div class="kpi c-ink" style="padding:12px 18px;--kc:var(--ink-3)"><div class="l">目标（SPEC4 §10）</div><div class="v" style="font-size:34px">&lt;100<small>ms</small></div><div class="n">x86 主机实测，n = 8 取中位数</div></div>
</div>'''
    body = svg(''.join(o), W, H) + '<div style="position:relative;height:32px">' + caphtml + '</div>' + lat
    notes = '''
<p>这是运行时的全貌，三条泳道：业务核、跨核共享段、worker 核。按 → 分四步走完一个完整闭环。</p>
<p>① 第一次调用：查不到，入队，然后<b>马上</b>跑 AOT 原函数体 —— 调用方永远不等编译。② worker 从队列里取出来，先核对 version 再编译。③ 编完再核对一次 version，等桶里读者都退出才发布。④ 第二次调用命中，直跳。</p>
<p>两个检查点是整套并发正确性的核心：deactivate 让 version 加一，于是队列里的旧任务、编到一半的旧结果，都会在检查点被丢弃。业务核那边什么都不用管。</p>
'''
    return dict(act=ACT, name='loop', kicker='第二幕 · 运行时闭环', title='从第一次调用到命中：一张图看完', body=body, steps=4, live='flows', notes=notes)


def s15_pass6():
    W, H = 640, 392
    o = []
    o.append(T(0, 16, '@g_cells : [16 x CellCfg]', 't-ink b700', size=14))
    o.append(T(0, 36, 'struct CellCfg { i32 mode; i32 threshold; i32 gain; }', 't-3', size=12))
    x0, y0, ew, eh = 0, 56, 124, 96
    for i in range(4):
        x = x0 + i * (ew + 10)
        last = i == 3
        cls = 'box-ice' if last else 'box'
        o.append(RECT(x, y0, ew, eh, cls, 8))
        o.append(T(x + 10, y0 + 22, f'[{i}]', 't-3 b700', size=12))
        for j, f in enumerate(('mode @0', 'threshold @4', 'gain @8')):
            yy = y0 + 44 + j * 20
            hl = last and j == 1
            o.append(T(x + 10, yy, f, 't-amber b700' if hl else ('t-ice b700' if (last and j == 0) else 't-2'), size=12))
        if last:
            o.append(RECT(x + 2, y0 + 29, ew - 4, 20, 'band-amber', 4))
    o.append(T(0, y0 + eh + 28, '… 共 16 个实例，cellIdx 已替换为常量 3', 'zh t-3', size=12.5))
    o.append(RECT(0, 208, W, 64, 'box-ink', 8))
    o.append(T(14, 234, 'addr = &g_cells + 3 × sizeof(CellCfg) + offsetof(threshold)', 't-ink b500', size=13.5))
    o.append(T(14, 256, '      = &g_cells + 3 × 12 + 4     →   *(int32_t *)addr = 13', 't-amber b700', size=13.5))
    o.append(arrow('M320 272 V300', 'amber'))
    o.append(T(0, 326, '这一步发生在 JIT 编译期间、worker 核上：', 'zh t-2', size=13.5))
    o.append(T(0, 350, '直接读进程内存。不是解释执行，不是猜，是读真值。', 'zh t-ink b700', size=13.5))
    o.append(T(0, 384, '地址必须能沿常量 GEP 链回溯到根全局变量；回溯不到就跳过这条 load。', 'zh t-3', size=12.5))
    left = svg(''.join(o), W, H)
    before = code('''; 替换前 · bitcode 里（idx 已经是常量 3）
%mode = load i32,
    ptr getelementptr inbounds nuw (
        [16 x %struct.CellCfg], ptr @g_cells,
        i64 0, i64 3, i32 0),
    !ejit.period_const !17''', 'll', 'sm', hl={2: 'amber', 5: 'amber'}, cap='EJitStructFieldPass 之前', maxw=734)
    after = code('''; 替换后
%mode = i32 0

; 不是一个"更聪明的 load"，而是一个 ConstantInt。
; 从这一行起，后面的每个 pass 都可以把 mode 当成 0。''', 'll', 'sm', hl={2: 'ice'}, cap='EJitStructFieldPass 之后', maxw=734)
    rules = '''
<div class="card pad-lg rule-amber" style="margin-top:18px"><div class="card-h">边界：每一款都是"宁可不优化，也不冒进"</div>
<ul class="ul">
  <li class="li tight">只认 load 上的 <code>!ejit.period_const</code>；<code>volatile</code> 永远不折</li>
  <li class="li tight">支持整型、浮点、指针；位域暂不支持</li>
  <li class="li tight">替换失败 → 跳过这条 load → 行为与 AOT 完全一致</li>
</ul></div>'''
    body = f'''<div class="grid g-45-55" style="gap:28px"><div data-anim="left">{left}</div>
<div data-anim="right">{before}<div class="step" data-step="1">{after}</div>{rules}</div></div>'''
    notes = '''
<p>这是整个系统的"魔法"所在，但说完你会发现它一点也不魔法：PASS6 在编译期，按 GEP 链算出地址，直接从进程内存里把值读出来，然后 <code>replaceAllUsesWith(ConstantInt::get(...))</code>。</p>
<p>按 → 看替换后的 IR：load 没了，变成了一个普通的 LLVM 常量。从这里开始，EJIT 的工作就结束了 —— 剩下全是 LLVM 自己的事。</p>
<p>右下角三条边界很重要：这是一个"宁可不优化也不能错"的 pass。所有失败路径都通向"跳过这条 load"。</p>
'''
    return dict(act=ACT, name='pass6', kicker='第二幕 · 魔法 pass', title='PASS6：编译的时候，去进程内存里把真值读出来', body=body, steps=1, notes=notes)


def s16_llvm_does_it():
    pre = code('''define i32 @check_cell(i8 zeroext %idx) {
  %c  = getelementptr [16 x %struct.CellCfg],
       ptr @g_cells, i64 0, i64 %idx
  %mode      = load i32, ptr %c,   !ejit.period_const !17
  %threshold = load i32, ptr %c+4, !ejit.period_const !17
  %gain      = load i32, ptr %c+8
  %s1 = select i1 (eq %mode, 1), i32 20, i32 5
  %s2 = select i1 (eq %mode, 0), i32 10, i32 %s1
  %m  = mul nsw i32 %threshold, %s2
  %r  = add nsw i32 %m, %gain
  ret i32 %r
}''', 'll', hl={4: 'ice', 5: 'ice', 6: 'amber'}, cap='替换前 · cell = 3：mode = 0，threshold = 13', maxw=590)
    post = code('''define i32 @check_cell(i8 zeroext %idx) {
  %gain = load i32, ptr
      getelementptr (i8, ptr @g_cells, i64 44)
  %r = add nsw i32 %gain, 130
  ret i32 %r
}''', 'll', hl={4: 'ice'}, cap='替换后 · 真实 dump', maxw=700)

    # JIT pipeline, two rows of five; violet = EJIT's own pass
    W, H = 1408, 136
    rows = [
        ['preReplacePeriodIndices', 'InstCombine', 'EJitStructFieldPass', 'IPSCCP', 'InstCombine'],
        ['EJitStructFieldPass', 'O1 / O2 / O3 函数简化', 'EJitStructFieldPass', 'InstCombine · SCCP · SimplifyCFG · ADCE', '向量化 · AArch64'],
    ]
    o = []
    ends = []
    for r, row in enumerate(rows):
        y = r * 82
        widths = [sum(15 if ord(ch) > 0x2E7F else 9 for ch in s) for s in row]
        extra = (W - 40 - sum(widths) - 4 * 52) / 5
        x = 0
        for i, s in enumerate(row):
            w = widths[i] + extra
            ej = 'EJit' in s
            o.append(f'<g class="step plain d{(r * 5 + i) % 4 + 1}" data-step="2">')
            o.append(RECT(x, y, w, 52, 'box-violet' if ej else 'box', 10))
            o.append(T(x + w / 2, y + 32, s, ('t-violet' if ej else 't-ink') + ' b700' + (' zh' if any('一' <= ch <= '鿿' for ch in s) else ''), anchor='middle', size=15))
            o.append('</g>')
            if i < 4:
                o.append(f'<g class="step plain" data-step="2">{arrow(f"M{x + w + 6} {y + 26} H{x + w + 46}", "ink")}</g>')
            x += w + 52
        ends.append(x - 52)
    o.append(f'<g class="step plain" data-step="2">{arrow(f"M{ends[0] + 4} 26 H{W - 10} V67 H14 V{74}", "ink", dash=True)}</g>')
    pipe = svg(''.join(o), W, H, extra='style="margin-top:8px"')

    def sq(n, col):
        return ''.join(f'<i style="display:inline-block;width:30px;height:22px;border-radius:4px;background:var(--{col}-mark);margin-right:6px"></i>' for _ in range(n))

    ops = f'''<div class="step" data-step="3" style="margin-top:26px">
  <div class="code-cap" style="margin-bottom:12px"><span>IR 指令</span><b><span style="color:var(--amber)">8 条</span> → <span style="color:var(--ice)">2 条</span></b></div>
  <div style="display:grid;grid-template-columns:1fr auto;gap:14px 18px;align-items:center;font:500 15px/1.3 var(--mono)">
    <span style="color:var(--ink-2)">1 gep · 3 load · 2 select · 1 mul · 1 add</span><span style="white-space:nowrap">{sq(8, 'amber')}</span>
    <span style="color:var(--ink-2)">1 load · 1 add</span><span style="white-space:nowrap">{sq(2, 'ice')}</span>
  </div></div>'''
    body = f'''<div class="grid g-46-54" style="gap:28px"><div data-anim="left">{pre}</div>
<div data-anim="right"><div class="step" data-step="3">{post}{ops}</div></div></div>
<div class="step" data-step="2"><div class="code-cap" style="margin-top:22px;margin-bottom:2px;text-transform:none"><span>JIT 流水线 · <b style="color:var(--violet)">紫色是 EJIT 自己的 pass</b>，其余全是 LLVM 原装</span><b>没有 Inline：AOT 预内联已经把 callee 消化掉了</b></div>{pipe}</div>
<p class="p step" data-step="3" style="font-size:29px;margin-top:28px"><b>我们不写优化。我们只是把真相告诉 LLVM，剩下的它来做。</b></p>'''
    notes = '''
<p>左边的 IR 就是上一页那张图编译进去的样子；右边是真实 dump 出来的最终 IR。八条指令变成两条：三次 load、两个 select、一次乘法，连那个算地址的 gep 都没了，只剩一次 load 加一次 add。</p>
<p>按 → 出现流水线。值得注意的：EJIT 自己的 pass 在整条流水线里跑了三轮 —— 参数替换之后一轮、IPSCCP 之后一轮、函数简化之后还有一轮（展开会暴露出新的常量索引 GEP）。其余全是 LLVM 原装的 pass。注意没有 Inline：AOT 预内联已经把 callee 消化掉了。</p>
<p>最后按 →：右边的指令数对比，和那句话。那句话是这一幕的总结，也是我对这个项目最满意的一句话。</p>
'''
    return dict(act=ACT, name='llvm-does-it', kicker='第二幕 · 剩下的交给 LLVM', title='一旦常量成立，LLVM 二十年积累的优化全部免费', body=body, steps=3, notes=notes)


def s17_machine_code():
    aot = code('''.loop:                                 ; AOT · clang -O2 · AArch64
  ldrsh w12, [x19], #2                 ; v = *x++
  cbnz  w9, .square                    ; mode ≠ 0 ?      ← 判 + 跳
  ldr   w13, [x8, #8]                  ; thresh           ← 读
  sxtw  x11, w12
  cmp   w13, w12                       ; v > thresh ?
  b.ge  .below                         ; 依赖数据          ← 跳（预测失败）
  ldrsw x12, [x8, #4]                  ; gain_q           ← 读
  smaddl x0, w12, x11, x0             ; acc += gain_q × v
  b     .check
.below:                                ; v ≤ thresh：
  sub   x0, x0, x11                    ;   acc −= v
  b     .check
.check:                                ; （mode ≠ 0 的 v×v 一路也汇到这里）
  subs  x10, x10, #1
  b.eq  .done                          ; 未完则直落回 .loop''', 'asm', 'sm', cap='每个样本：2 次配置读 · 3 条件跳转 · AArch64', maxw=654)
    jit = code('''.loop:                                 ; JIT · cell = 3 · 特化后 IR
  ldrsh x9, [x1], #2                   ; v = *x++
  lsl   x10, x9, #3                    ; v×8 ← thresh=100、gain_q=7 已折入
  cmp   w9, #100                       ; v > 100 ?
  sub   x10, x10, x9                   ; v×8 − v = v×7
  csneg x9, x10, x9, gt                ;   v > 100 ? v×7 : −v
  subs  x8, x8, #1
  add   x0, x9, x0
  b.ne  .loop''', 'asm', 'sm', hl={3: 'ice', 4: 'ice', 5: 'ice', 6: 'ice'}, cap='每个样本：0 次配置读 · 0 条数据相关跳转 · 8 条指令 · AArch64', maxw=654)
    facts = f'''
<div class="grid g3 step" data-step="1" style="margin-top:20px">
  <div class="kpi c-ice" style="padding:16px 20px"><div class="l">AOT → JIT · 每样本</div><div class="v" style="font-size:40px">{M['stream_aot_ns']:.1f}<small>ns</small> → {M['stream_jit_ns']:.2f}<small>ns</small></div><div class="n">x86 主机实测：随机输入 4096 样本 × 4000 轮，9 次取中位数</div></div>
  <div class="kpi c-ice" style="padding:16px 20px"><div class="l">加速比</div><div class="v" style="font-size:52px" data-count="{M['stream_aot_ns'] / M['stream_jit_ns']:.1f}" data-dec="1" data-post="×">0</div><div class="n">几乎全部来自 <b>分支 → csneg</b>（条件反选）：常量已知后，LLVM 敢推测了</div></div>
  <div class="kpi c-amber" style="padding:16px 20px"><div class="l">但请记住</div><div class="v" style="font-size:30px;line-height:1.25;padding-top:8px">这是最有利于我们的内核</div><div class="n">数据相关分支 + 每轮读配置。第四幕看另外三种。</div></div>
</div>'''
    body = f'<div class="grid g2" style="gap:26px"><div data-anim="left">{aot}</div><div class="step" data-step="1">{jit}</div></div>{facts}'
    notes = '''
<p>同一段 C 代码。左边 AOT，右边 JIT 特化后的版本。两边的汇编都出自 AArch64 后端的真实输出：AOT 侧 clang -O2 产出 IR，JIT 侧是运行时导出的特化后 IR，都用目标板的那套 llc -O2 编出来。</p>
<p>按 → 出现 JIT 版本：8 条指令，一次配置都不读，一条数据相关的跳转都没有。原本的 if-else 变成了 csneg（条件反选）—— 不是我们改的，是 LLVM 在常量已知之后自己敢推测了。</p>
<p>数字：3.97 → 0.94 ns/样本，约 4.2 倍 —— 这是 x86 主机上的实测耗时，与两边的代码形态分开看：形态是目标板的，量级是主机的。要马上泼冷水：这是对 EJIT 最有利的内核形态。第四幕会给你看另外三种。</p>
'''
    return dict(act=ACT, name='machine-code', kicker='第二幕 · 眼见为实', title='同一段 C，两份真实汇编', body=body, steps=1, notes=notes)


def slides():
    return [s9_annotations(), s10_metadata(), s11_aot_pipeline(), s12_wrapper(), s13_binary(), s14_loop(), s15_pass6(), s16_llvm_does_it(), s17_machine_code()]
