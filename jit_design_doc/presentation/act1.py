# -*- coding: utf-8 -*-
"""Act 1 — 问题：为什么需要第三条路"""
from svgkit import svg, T, TL, RECT, PATH, LINE, arrow, code, dspy
from data import M

ACT = 1


def s5_flat():
    W, H = 1408, 468
    x0, x1 = 368, 1372
    ch = [
        ('g_boardCfg.boardType', 'static 窗口', 1, [(0, .05, 0, ''), (.05, 1, 1, 'UBBPg')]),
        ('g_cellCfg[3].cellType', 'cell 窗口 · 实例 3', 0, [(0, .2, 0, 'FDD'), (.2, .64, 1, '0xFD'), (.64, 1, 0, '0xEC')]),
        ('g_cellCfg[3].cellId', '', 0, [(0, .2, 0, '—'), (.2, .64, 1, '42'), (.64, 1, 0, '7')]),
        ('g_scfg[3].mode', '', 0, [(0, .64, 1, '0'), (.64, 1, 0, '1')]),
    ]
    out = []
    lane_h = 96
    wa, wb = x0 + .22 * (x1 - x0), x0 + .62 * (x1 - x0)
    # window band
    out.append(f'<g class="step plain" data-step="1">')
    out.append(RECT(wa, 18, wb - wa, lane_h * 4 - 8, 'band-ice', 8))
    out.append(PATH(f'M{wa} 18 V{18 + lane_h * 4 - 8}', 'wire-ice'))
    out.append(PATH(f'M{wb} 18 V{18 + lane_h * 4 - 8}', 'wire-ice'))
    out.append(T(wa + 14, 8, '窗口：ejit_activate("cell", 3)  …  这 4 个值全是平的', 't-ice b700', size=14))
    out.append('</g>')
    for i, (name, sub, kind, segs) in enumerate(ch):
        top = 40 + i * lane_h
        hi, lo = top + 16, top + 66
        out.append(T(8, top + 30, name, 't-ink b500', size=14))
        if sub:
            out.append(T(8, top + 52, sub, 't-3', size=12))
        out.append(LINE(x0 - 14, lo + 12, x1, lo + 12, 'ln'))
        d = ''
        for (a, b, lv, _) in segs:
            xa, xb = x0 + a * (x1 - x0), x0 + b * (x1 - x0)
            y = hi if lv else lo
            if not d:
                d = f'M{xa:.1f} {y:.1f}'
            else:
                d += f' V{y:.1f}'
            d += f' H{xb:.1f}'
        out.append(PATH(d, 'wire', extra='stroke-width="2.2" stroke-linejoin="round"'))
        for (a, b, lv, lab) in segs:
            if not lab:
                continue
            xa, xb = x0 + a * (x1 - x0), x0 + b * (x1 - x0)
            y = (hi if lv else lo) - 10
            inside = a >= .22 and b <= .62
            out.append(T((xa + xb) / 2, y, lab, 't-2 b700' if not inside else 't-ice b700', anchor='middle', size=13))
    # change points
    for t, lab, cls in ((.05, '上电初始化', 'ln-ink'), (.64, 'reconfig：deactivate → 改 → activate', 'wire-red dash')):
        x = x0 + t * (x1 - x0)
        out.append('<g class="step plain" data-step="1">' if t == .64 else '<g>')
        out.append(PATH(f'M{x:.1f} 30 V{40 + lane_h * 4 - 20}', cls))
        out.append(T(x + (8 if t == .05 else -8), 40 + lane_h * 4 - 4, lab, 'zh t-red b700' if t == .64 else 'zh t-3', anchor='start' if t == .05 else 'end', size=13))
        out.append('</g>')
    out.append(T(x1, 40 + lane_h * 4 - 4, 't →', 't-3', anchor='end', size=13))
    body = svg(''.join(out), W, H) + '''
<div class="cols step" data-step="2" style="margin-top:8px">
  <div class="card tint-ice pad-sm" style="flex:1.2"><div class="card-h"><span class="sw" style="background:var(--ice)"></span>对编译器来说</div>
  <p class="small" style="color:var(--ink-2);font-size:16px">这 4 个全局变量在 AOT 那一刻"可能随时会变"。于是每次循环都要从内存读、每次都要判——<b>哪怕它们整整 40% 的时间里一动不动</b>。</p></div>
  <div class="card pad-sm" style="flex:1"><div class="card-h"><span class="sw" style="background:var(--red)"></span>对业务来说</div>
  <p class="small" style="color:var(--ink-2);font-size:16px">改配置是一件<b>大事</b>：要停业务、要重新建链。它发生在清楚的边界上，业务自己知道。</p></div>
</div>'''
    notes = '''
<p>这是基带开发者最熟悉的一张图。单板类型上电就定了；小区类型、小区 ID 在建链之后不变；调度模式在重配之前不变。它们不是 C 意义上的 <code>const</code>——会变——但变化只发生在清晰的边界上。</p>
<p>按 → 出现蓝色窗口。窗口里所有通道都是平的。问题在于：<b>编译器在 AOT 的时候看不见这个窗口</b>，它只能假设最坏情况。</p>
<p>这一页只建立一个直觉："不变"是有时间范围的。不要在这里讲实现。</p>
'''
    return dict(act=ACT, name='flat', kicker='第一幕 · 问题', title='配置不是常量。但在它自己那段时间里，它是。', body=body, steps=2, notes=notes)


def s6_aot():
    c = code('''EJIT_ENTRY
int64_t stream_kernel(EJIT_DIM(cell) uint8_t ci,
                      const int16_t *x, int n) {
  const StreamCfg *c = &g_scfg[ci];
  int64_t acc = 0;
  for (int i = 0; i < n; i++) {
    int v = x[i];
    if (c->mode == 0) {                 // ← 配置
      if (v > c->thresh)                // ← 配置
        acc += (int64_t)v * c->gain_q;  // ← 配置
      else
        acc -= v;
    } else {
      acc += (int64_t)v * v;
    }
  }
  return acc;
}''', 'c', 'xs', boxes={8: [('c->mode', 'amber')], 9: [('c->thresh', 'amber')], 10: [('c->gain_q', 'amber')]}, cap='C · 用户视角')
    a = code('''.loop:                                 ; clang -O2 真实输出 · AArch64 · 标签已改名
  ldrsh w12, [x19], #2                 ; v = *x++
  cbnz  w9, .square                    ; ← mode ≠ 0 ?  每个样本都判
  ldr   w13, [x8, #8]                  ; ← thresh：每个样本都从内存读
  sxtw  x11, w12
  cmp   w13, w12                       ; v > thresh ?
  b.ge  .below                         ; ← 依赖数据：分支预测的噩梦
  ldrsw x12, [x8, #4]                  ; ← gain_q：每个样本都从内存读
  smaddl x0, w12, x11, x0             ; acc += gain_q × v
  b     .check
.below:                                ; v ≤ thresh：acc −= v
  sub   x0, x0, x11
  b     .check
.check:                                ; （mode ≠ 0 的 v×v 一路也汇到这里）
  subs  x10, x10, #1
  b.eq  .done                          ; 没数完就直落回 .loop''', 'asm', 'sm', cap='AOT（目标板）· 每个样本做的事')
    facts = '''
<div class="grid g3 step" data-step="1" style="margin-top:20px">
  <div class="kpi c-amber" style="padding:16px 20px"><div class="l">内存读取 / 样本</div><div class="v" style="font-size:44px">2</div><div class="n">thresh、gain_q —— 值没变，也要读</div></div>
  <div class="kpi c-amber" style="padding:16px 20px"><div class="l">条件跳转 / 样本</div><div class="v" style="font-size:44px">3</div><div class="n">其中 <b>b.ge</b> 依赖数据，随机输入下几乎次次预测失败</div></div>
  <div class="kpi c-amber" style="padding:16px 20px"><div class="l">每个样本的代价</div><div class="v" style="font-size:44px" data-count="{M['stream_aot_ns']:.1f}" data-dec="1" data-delay="300">0</div><div class="n">ns · x86 主机实测（见第四幕）</div></div>
</div>'''
    body = f'<div class="grid g-42-58" style="gap:22px"><div data-anim="left">{c}</div><div data-anim="right">{a}</div></div>{facts}'
    notes = '''
<p>左边是用户写的代码，右边是这条循环在目标板（AArch64）后端的真实汇编：clang -O2 产出 IR，llc -O2 编出这一段。三处读配置的地方，用户一眼就懂。</p>
<p>重点念右边的注释：mode 每个样本判一次、thresh 和 gain 每个样本各从内存读一次、还有一条依赖数据的跳转。编译器没错——它<b>不知道</b>这些值在窗口内不会变，所以只能这么写。注意 <code>mode</code> 分支是可预测的，真正贵的是那条随机的 <code>b.ge</code>：在乱序大核上，一次预测失败就是十几二十个周期。</p>
<p>按 → 出现三张数字卡。第三个数字（3.9 ns/样本）先卖个关子："这个数字我们在第四幕会回头再看"。</p>
'''
    return dict(act=ACT, name='aot', kicker='第一幕 · AOT 的困境', title='编译器不知道的事，它只能每次都问内存', body=body, steps=1, notes=notes)


def s7_why_not():
    rows = [
        ('profile 落盘 + 离线重编译', '没有文件系统', 'profile 只活在内存里'),
        ('去优化 deopt：推测错了就回退', '没有 deopt 基础设施，"推测必须一次正确"', '时间窗 + 失效，推测永远带 guard'),
        ('编译线程池、GC 线程', '没有 &lt;atomic&gt;、<code>std::thread</code>', '手搓原子、无锁队列、读写锁'),
        ('dlsym / dlopen 找符号', '裸核没有，而且必须避免', 'AOT 期自动注册每一个外部符号'),
        ('MB 级代码缓存，随用随取', '整个进程 100 KB – 2 MB', '2 MiB 池、4 KiB 页封固、永不 RWX'),
        ('解释器 / 基线编译预热', '软实时：停顿只能有一次', '第一次调用就是 AOT，编译全在后台'),
    ]
    trs = ''.join(
        f'<tr class="step" data-step="{i + 1}"><td>{a}</td><td style="color:var(--red)"><b>✕</b> {b}</td><td style="color:var(--ice)"><b>→</b> {c}</td></tr>'
        for i, (a, b, c) in enumerate(rows))
    body = f'''
<table class="tbl roomy" style="font-size:21px">
<thead><tr><th style="width:31%">服务器 JIT 的默认答案</th><th style="width:36%">这颗核上</th><th>EJIT 被迫给出的答案</th></tr></thead>
<tbody>{trs}</tbody></table>
<p class="p step" data-step="6" style="margin-top:30px;font-size:26px;line-height:1.5">没有一行是"优化"，每一行都是<b>重新发明</b>。这就是为什么这个项目有一半的代码不在编译器里。</p>'''
    notes = '''
<p>这页回答"为什么不直接拿 V8/JVM 的方案来"。逐行出现，每行只念左边和右边。</p>
<p>说最后一行的时候补一句：这些替代方案没有一个是为了快，全都是为了"在没有操作系统的世界里也能成立"。第三幕会把每一行展开。</p>
'''
    return dict(act=ACT, name='why-not', kicker='第一幕 · 为什么不能照搬', title='把服务器 JIT 搬过来？先过这六关', body=body, steps=6, notes=notes)


def s8_third():
    # three mini timelines: when does the compiler learn the value?
    def col(step, title, color, lines, events, know):
        W, H = 420, 160
        o = [PATH('M16 96 H404', 'ln-ink')]
        for x, lab, colr, anchor in events:
            o.append(PATH(f'M{x} 44 V104', f'wire-{colr}'))
            o.append(T(x + (-6 if anchor == 'end' else 6), 36, lab, f't-{colr} b700', anchor=anchor, size=12))
        o.append(T(16, 134, know, 'zh t-2', size=14))
        body = svg(''.join(o), W, H)
        return f'''<div class="col step" data-step="{step}">
  <div class="card pad-lg" style="height:100%"><div class="card-h"><span class="sw" style="background:var(--{color})"></span>{title}</div>
  <p class="small" style="color:var(--ink-2)">{lines}</p>{body}</div></div>'''
    c1 = col(0, 'AOT', 'amber', '编译发生在 t = 0 之前。配置值还不存在，只能假设"随时会变"。',
             [(40, '编译', 'ink', 'start'), (150, '上电', 'amber', 'start')], '编译时：值不存在')
    c2 = col(0, '传统 JIT', 'violet', '编译发生在第一次调用。值已知，但每个函数、每个取值都要来一遍，还要防它变。',
             [(200, '首次调用 → 编译', 'violet', 'start')], '编译时：值已知，但来得晚、来得贵')
    c3 = col(1, 'EJIT', 'ice', '编译发生在<b>窗口打开之后</b>。值已知，而且业务承诺它在窗口内不变。',
             [(150, 'activate', 'ice', 'start'), (240, '编译', 'ice', 'start'), (340, 'deactivate', 'red', 'start')],
             '编译时：值已知，且承诺不变 —— 刚好')
    html = f'''
<div class="stmt" style="justify-content:flex-start;padding-top:36px">
  <div class="kicker" data-anim="up">第一幕 · 第三条路</div>
  <h2 class="display" style="font-size:80px;line-height:1.24;margin:0" data-anim="up">{dspy('数据不是常量。')}<br>{dspy('但')}<em>{dspy('在一段时间里')}</em>{dspy('，它是。')}</h2>
  <p class="after" data-anim="up" style="margin-top:22px;font-size:24px">所以别问"它是不是常量"，问<b style="color:var(--ink)">"它在哪段时间里是常量"</b>。把这个窗口告诉编译器——这就是 EJIT 的全部赌注。</p>
  <div class="cols" style="margin-top:36px;gap:22px">{c1}{c2}{c3}</div>
</div>'''
    notes = '''
<p>停在这页久一点，这是整场演讲的核心一句话。</p>
<p>三个小时间轴：AOT 编译得太早，值还不存在；传统 JIT 编译得刚好，但它不知道值<b>会不会变</b>，所以需要 deopt、需要 profile、需要一堆基础设施；EJIT 多知道一件事——业务承诺"这个窗口里它不变"——于是整栋大楼都可以省掉。</p>
<p>按 → 让 EJIT 那一列出现。然后进入第二幕：这件事具体是怎么做到的。</p>
'''
    return dict(act=ACT, name='third', trans='zoom', html=html, steps=1, notes=notes)


def slides():
    return [s5_flat(), s6_aot(), s7_why_not(), s8_third()]
