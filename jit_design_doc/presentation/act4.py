# -*- coding: utf-8 -*-
"""Act 4 — 诚实：数字、坏消息、没做完的"""
import math
from svgkit import svg, T, TL, RECT, PATH, LINE, arrow, box, code, dspy
from data import M

ACT = 4


def s26_opener():
    html = f'''
<div class="opener">
  <div class="num">04</div>
  <div class="kicker" data-anim="up">第四幕 · 诚实</div>
  <h2 class="display" data-anim="up">{dspy('我们做的是测量，')}<br>{dspy('不是信仰。')}</h2>
  <p class="thesis" data-anim="up">接下来的数字，<b style="color:var(--ink)">没有一个是为了好看</b>。<br>同一个系统：有的内核快 4 倍，有的慢 2.5 倍，还有那个 47 万倍的笑话。</p>
  <p data-anim="up" style="font:400 18px/1.6 var(--sans);color:var(--ink-3);margin:40px 0 0;max-width:1120px">{M['build']}。全部是 x86 主机微基准，<b style="color:var(--ink-2)">不是板端数据</b>；板端收益基线至今没有。</p>
</div>'''
    notes = '''
<p>语气放平。这一幕是整场演讲我最想让人记住的部分：我们自己的数字，包括难看的。</p>
<p>先把口径说清楚：x86 主机、微基准、共享服务器。板端数据一个都没有。</p>
'''
    return dict(act=ACT, name='opener4', cls='bleed', html=html, notes=notes)


def s27_speedup():
    out = code('''=== EJIT Performance Benchmark ===
cellIdx=3  iter=20000000  warmup=5

--- Pure AOT (无 attribute, 无 wrapper 开销) ---
  result=2000000000  time=92.7 ms

--- JIT Optimized (激活时间窗) ---
  result=2000000000  time=0.0 ms

  JIT vs Pure AOT 加速: 474369.6x
  Interproc JIT vs Pure AOT 加速: 0.4x''', 'txt', 'sm', hl={5: 'amber', 8: 'ice', 10: 'ice', 11: 'red'}, maxw=600, cap='ejit_test/ejit_perf_bench · 真实输出')
    fn = code('''compute_cell:               ; 特化后 · 3 条指令 · 12 字节
  mov  w0, #37888           ; ← 低 16 位
  movk w0, #30517, lsl #16  ; ← 高 16 位
  ret''', 'asm', 'lg', hl={2: 'ice', 3: 'ice'}, maxw=620, cap='JIT 机器码 · 全文（AArch64 后端）')
    html = f'''
<div style="position:absolute;left:120px;top:60px;width:1360px">
  <div class="kicker" data-anim="up">第四幕 · 先看一个好的</div>
  <div style="display:flex;align-items:flex-end;gap:28px;margin-top:4px" data-anim="fade">
    <div class="mono" style="font:800 176px/1 var(--mono);letter-spacing:-.05em;color:var(--ink)" data-count="{M['official_speedup_num']}" data-dec="1" data-dur="2200">0</div>
    <div style="font:800 96px/1 var(--mono);color:var(--ice);padding-bottom:22px">×</div>
    <div style="font:700 30px/1.35 var(--sans);color:var(--ink-2);padding-bottom:30px;max-width:420px">官方 benchmark 打印出来的加速比。<br><span style="color:var(--ink-3)">另两次运行：486,596×、1,430,164×</span></div>
  </div>
  <div class="grid g-58-42" style="gap:30px;margin-top:30px">
    <div class="step" data-step="1">{out}</div>
    <div><div class="step" data-step="2">{fn}</div>
      <div class="card pad-lg rule-red step" data-step="3" style="margin-top:16px"><div class="card-h">它衡量的是什么</div>
      <p class="small" style="color:var(--ink-2)">2000 万次循环，在常量已知之后被 LLVM 证明是闭式：<code>100 × 4 ÷ 4 × 20M</code>。于是整个函数只剩 <b>mov + movk + ret</b>（12 字节：AArch64 一条指令装不下 32 位常数）。<br><br>这个数字衡量的是"循环消失了"，不是"EJIT 有多快"。三次运行给出 {M['official_speedup_runs']} —— 因为<b>分母是时钟分辨率的噪声</b>。</p></div>
    </div>
  </div>
  <p class="step" data-step="3" style="font:600 30px/1.4 var(--sans);color:var(--ink);margin:26px 0 0">当一个加速比有六位数，该怀疑的不是编译器，<em style="font-style:normal;color:var(--red)">是 benchmark</em>。</p>
</div>'''
    notes = '''
<p>先给大家看一个好数字：47 万倍。官方 benchmark 自己打印的。让数字滚一会儿。</p>
<p>按 → 看左边的原始输出和右边的"全文"。JIT 之后的函数只有 3 条指令：mov 装进低 16 位、movk 补上高 16 位，得到 2,000,000,000，返回 —— AArch64 一条指令装不下 32 位常数，所以是两条。2000 万次循环没有了，因为常量已知之后 LLVM 证明了这个循环有闭式解。</p>
<p>按 → 出现结论。再补一刀：同一个 benchmark 三次运行给出 47 万、49 万、143 万 —— 一个"加速比"能在三次之间差 3 倍，是因为分母是时钟分辨率。同一份输出的最后一行，才是诚实的那一个：Interproc JIT vs Pure AOT，0.4 倍。</p>
'''
    return dict(act=ACT, name='speedup', cls='bleed', html=html, steps=3, notes=notes)


def s28_range():
    rows = [
        ('stream 内核', '每样本读 3 项配置、含数据相关分支', M['stream_aot_ns'], M['stream_jit_ns'], 'ns / 样本', 'ice'),
        ('官方 interproc 内核', 'callee 是 noinline 的 static 函数', M['ip_aot_ms'], M['ip_jit_ms'], 'ms / 2000 万次', 'red'),
        ('tiny 函数 · 命中', 'EJIT_ENTRY，单小区，inline cache 命中', M['tiny_pure_ns'], M['tiny_hit1_ns'], 'ns / 次调用', 'red'),
        ('tiny 函数 · 未激活', 'EJIT_ENTRY，但窗口没开（fallback）', M['tiny_pure_ns'], M['tiny_miss_ns'], 'ns / 次调用', 'red'),
    ]
    W, H = 900, 360
    cx, k = 600, 140
    o = [LINE(cx, 10, cx, H - 42, 'ln-ink')]
    for v in (0.1, 0.25, 0.5, 1, 2, 4):
        x = cx + math.log2(v) * k
        o.append(LINE(x, H - 42, x, H - 35, 'ln-ink'))
        lab = f'{v:g}×'
        o.append(T(x, H - 13, lab, 't-3 b500' + (' t-ink b700' if v == 1 else ''), anchor='middle', size=13.5))
    cards = []
    for i, (name, what, aot, jit, unit, col) in enumerate(rows):
        y = 20 + i * 90
        r = aot / jit
        lg = math.log2(r)
        x0 = cx + (0 if lg >= 0 else lg * k)
        w = abs(lg) * k
        o.append(f'<g class="step" data-step="{i + 1}">')
        o.append(RECT(x0, y, w, 32, f'solid-{col}', 4))
        # square the end that sits on the baseline
        sq_x = cx - 4 if lg < 0 else cx
        o.append(RECT(sq_x, y, 4, 32, f'solid-{col}', 0))
        end = x0 + w + 12 if lg >= 0 else x0 - 12
        o.append(T(end, y + 23, f'{r:.2f}×', f't-{col} b700', anchor='start' if lg >= 0 else 'end', size=16))
        o.append('</g>')
        cards.append((i, col, name, what, aot, jit, unit, r))
    chart = svg(''.join(o), W, H)
    cd = ''.join(f'''<div class="step right" data-step="{i + 1}" style="display:flex;gap:14px;align-items:flex-start;padding:14px 0;border-bottom:1px solid var(--line)">
  <span class="mono-tag {col}" style="margin-top:3px;min-width:74px;text-align:center">{r:.2f}×</span>
  <p style="margin:0;font:400 15.5px/1.55 var(--sans);color:var(--ink-2)"><b style="color:var(--ink)">{name}</b><br>{what}<br><span class="mono" style="font-size:13.5px">{aot:g} → {jit:g} {unit}</span></p></div>''' for i, col, name, what, aot, jit, unit, r in cards)
    why = '''
<div class="grid g3 step" data-step="4" style="margin-top:26px">
  <div class="card pad-sm rule-ice"><div class="card-h">快的那一个</div><p class="small" style="color:var(--ink-2)">收益几乎全部来自"分支 → csneg（条件反选）"：常量已知后，LLVM 敢推测了。<b>对 EJIT 最有利的形态。</b></p></div>
  <div class="card pad-sm rule-red"><div class="card-h">慢的那一个（一）</div><p class="small" style="color:var(--ink-2)">callee 是 noinline 的 static 函数 → bitcode 瘦身把它<b>外链化</b>了，特化止步于调用边界；调用还多过一跳 GOT 桩。benchmark 源码注释说"会折成常量"，实测没有。</p></div>
  <div class="card pad-sm rule-red"><div class="card-h">慢的那一个（二）</div><p class="small" style="color:var(--ink-2)">探针本身比函数贵。<b>别给小函数标 EJIT_ENTRY</b> —— 尤其别在窗口没开的时候调用它。</p></div>
</div>'''
    body = f'<div class="grid" style="grid-template-columns:900px 1fr;gap:28px"><div data-anim="left"><div class="code-cap" style="margin-bottom:12px"><span>加速比 = AOT 耗时 ÷ JIT 耗时 · 对数轴</span><b><span style="color:var(--red)">← 更慢</span> · 1× 为基准 · <span style="color:var(--ice)">更快 →</span></b></div>{chart}</div><div data-anim="right">{cd}</div></div>{why}'
    notes = '''
<p>同一台机器、同一套工具链、四种内核。横轴是对数的，中间是 1 倍。按 → 逐条出现。</p>
<p>第一种快 4.2 倍，是上一幕看过的 stream 内核。第二种是官方 benchmark 的 interproc 用例：JIT 版比 AOT 慢 2.5 倍。原因是 callee 被标了 noinline，瘦身阶段把它外链化了，特化根本进不去；而每次调用还得多过一跳 GOT 桩。benchmark 的源码注释说"phase 1d 会让它折成常量"，实测并没有。</p>
<p>后两种是同一个两行的小函数：命中时 16.6 纳秒，没激活时 69 纳秒，而普通调用只要 5.4。探针比函数本身贵。</p>
<p>按 → 三张卡是结论。这页如果只能带走一句话：<b>EJIT_ENTRY 是有价的，只给体量够大、调用够热的函数。</b></p>
'''
    return dict(act=ACT, name='range', kicker='第四幕 · 收益的范围', title='同一个系统，同一个工具链：4.2 倍，和 0.4 倍', body=body, steps=4, notes=notes)


def s29_probe():
    vals = [('普通 AOT 调用', M['tiny_pure_ns'], 'ink'), ('IC 命中 · 单小区', M['tiny_hit1_ns'], 'ice'), ('IC 命中 · 16 小区轮转', M['tiny_hit16_ns'], 'ice'),
            ('1/8 命中 · 其余未激活', M['tiny_mixed_ns'], 'amber'), ('全部未激活', M['tiny_miss_ns'], 'amber')]
    W, H = 700, 344
    sc = 600 / 70
    o = []
    for i, (lab, v, col) in enumerate(vals):
        y = 18 + i * 62
        o.append(T(0, y + 17, lab, 'zh t-2 b500', size=14.5))
        bx = 250
        o.append(f'<rect x="{bx}" y="{y}" width="{v * sc:.1f}" height="24" rx="4" class="solid-{col}" data-anim="grow" style="--d:{.15 + i * .1:.2f}s;transform-box:fill-box;transform-origin:left center"/>')
        o.append(T(bx + v * sc + 10, y + 18, f'{v:g}', 't-ink b700', size=15))
    o.append(LINE(bx, 6, bx, H - 34, 'ln-ink'))
    o.append(T(bx, H - 10, 'ns / 次调用（tiny 函数，2000 万次取中位数）', 'zh t-3', size=13))
    chart = svg(''.join(o), W, H)
    asm = code('''f:                      // AArch64 命中路径
  adrp x8, __ejit_icache_fn_f
  add  x8, x8, :lo12:__ejit_icache_fn_f
  cmp  w0, #16                 // idx < 16 ?
  add  x8, x8, w0, uxtw #3     // cell[idx]
  adrp x9, .Lf.icache_miss
  add  x9, x9, :lo12:.Lf.icache_miss
  csel x8, x8, x9, lo          // 域外 → 哨兵
  ldr  x1, [x8]                // 里面是谁？
  br   x1                      // musttail 尾跳''', 'asm', 'xs', maxw=390)
    body = f'''<div class="grid g-50-50" style="gap:34px"><div data-anim="left">{chart}
<p class="step" data-step="1" style="font:500 17px/1.5 var(--sans);color:var(--ink-2);margin:14px 0 0;padding-left:16px;border-left:3px solid var(--red)">同一个两行的小函数。命中时 <b>+{M['tiny_hit1_ns'] - M['tiny_pure_ns']:.0f} ns</b>，没激活时 <b>+{M['tiny_miss_ns'] - M['tiny_pure_ns']:.0f} ns</b>。三条探针指令解释不了这个数 —— 怀疑是跨页间接跳转的代价，但本机 <code>perf</code> 被禁用，<b>原因未证实</b>。</p></div>
<div data-anim="right">
<div class="grid g3 step" data-step="1" style="gap:14px">
  <div class="kpi c-ice" style="padding:14px 18px"><div class="l">aarch64 · 命中查询</div><div class="v" style="font-size:40px">9.0<small>ns</small></div><div class="n">NO_RECLAIM 配置 · perf stat · 1 亿次</div></div>
  <div class="kpi c-amber" style="padding:14px 18px"><div class="l">带读令牌</div><div class="v" style="font-size:40px">20.2<small>ns</small></div><div class="n">两次 RMW 约占 11 ns</div></div>
  <div class="kpi c-ink" style="padding:14px 18px;--kc:var(--ink-3)"><div class="l">目标</div><div class="v" style="font-size:40px">&lt;100</div><div class="n">条指令；现约 105</div></div>
</div>
<div class="card pad-lg rule-ice step" data-step="1" style="margin-top:18px"><div class="card-h">目标板上的命中路径（AArch64 · llc 真实输出）</div>
<div class="grid g-72-28" style="gap:18px;align-items:center">{asm}
<div class="kpi c-ice" style="padding:14px 18px"><div class="l">机器指令</div><div class="v" style="font-size:48px">10</div><div class="n">没有一条条件分支；域外下标用 <code>csel</code> 分流</div></div></div></div>
<div class="card pad-lg rule-lime step" data-step="1" style="margin-top:10px"><div class="card-h">不知道该标谁？有工具</div>
<p class="small" style="color:var(--ink-2);font-size:15.5px"><code>ejit_print_period_const_ranking()</code>：按"每单位时间消掉了多少次 period_const load"给每个 entry 排名。排名靠后的，把标注拿掉。</p></div>
</div></div>'''
    notes = '''
<p>探针到底多贵？左边是 x86 上同一个两行函数的五种情形。命中时 16.6 纳秒，比普通调用多 11 纳秒；没激活时 69 纳秒。</p>
<p>按 → 右边是目标板上的真实汇编：十条指令，没有一条条件分支。aarch64 主机上这条查询路径是 9 纳秒（不带读令牌的配置）。</p>
<p>诚实说明：x86 上那 11 纳秒我解释不了——指令数对不上。本机 perf 被禁用，只能存疑。</p>
<p>最后一张卡是实用建议：运行时自带一个排名工具，按"每单位时间消掉了多少次常量 load"排序。排名靠后的 entry，应该把标注拿掉。</p>
'''
    return dict(act=ACT, name='probe', kicker='第四幕 · 探测的代价', title='探针不是免费的：+11 ns，哪怕一切顺利', body=body, steps=1, notes=notes)


def s30_icache():
    W, H = 1408, 300
    x0, k, cap = 330, 21, 32          # 0 KiB at x0, 21 px per KiB, 32 KiB L1 I-cache
    xc = x0 + cap * k
    o = [T(0, 24, '指令缓存压力 = 小区数 × 每份代码大小', 'zh t-ink b700', size=17),
                  RECT(x0, 44, cap * k, 214, 'band-ice', 8),
         RECT(xc, 44, (48 - cap) * k, 214, 'band-red', 8),
         LINE(xc, 34, xc, 258, 'ln-ink', 'stroke-dasharray="5 4"'),
         T(xc, 24, 'L1 I-cache · 32 KiB（Cortex-A 典型值）', 'zh t-ink b700', anchor='middle', size=14),
         T(x0 + 14, 64, '装得下', 'zh t-3', size=13),
         T(xc + 14, 64, '装不下', 'zh t-red b700', size=13)]
    o.append(T(0, 284, '示意：容量与体积均为文档估计值，非实测', 'zh t-3', size=13))
    for v in (0, 8, 16, 24, 32, 40, 48):
        x = x0 + v * k
        o.append(LINE(x, 258, x, 264, 'ln-ink'))
        o.append(T(x, 284, f'{v}' + (' KiB' if v == 48 else ''), 't-3', anchor='middle', size=13))
    # one cell: 2–5 KiB
    yA = 106
    o.append(T(0, yA - 2, '单小区的热点路径', 'zh t-ink b700', size=16))
    o.append(T(0, yA + 20, '一份特化代码，2 – 5 KiB', 'zh t-3', size=13))
    o.append(f'<rect x="{x0 + 2 * k}" y="{yA - 14}" width="{3 * k}" height="32" rx="4" class="solid-ice" data-anim="grow" style="transform-box:fill-box;transform-origin:left center"/>')
    o.append(T(x0 + 5 * k + 14, yA + 8, '装得下 · 收益全额', 'zh t-ice b700', size=14.5))
    # eight cells × 5 KiB
    yB = 204
    o.append(T(0, yB - 2, '8 个小区 × 5 KiB', 'zh t-ink b700', size=16))
    o.append(T(0, yB + 20, '每个小区一份，≈ 40 KiB', 'zh t-3', size=13))
    for i in range(8):
        x = x0 + i * 5 * k
        over = (i + 1) * 5 > cap
        o.append(f'<rect x="{x}" y="{yB - 14}" width="{5 * k - 3}" height="32" rx="4" class="solid-{"red" if over else "amber"}" data-anim="grow" style="--d:{.25 + i * .06:.2f}s;transform-box:fill-box;transform-origin:left center"/>')
        o.append(T(x + (5 * k - 3) / 2, yB + 8, str(i), 't-ink b700', anchor='middle', size=13))
    o.append(T(x0 + 8 * 5 * k + 6, yB + 8, '超出', 'zh t-red b700', size=14.5))
    chart = svg(''.join(o), W, H)
    cards = '''
<div class="grid g4" style="gap:16px">
  <div class="card pad-lg rule-amber"><div class="card-h">A · 合一份</div><p class="small" style="font-size:17px;color:var(--ink-2)">配置相同的小区共用一份代码：份数从小区数 N 降到不同配置数 K。</p><p style="margin:14px 0 0"><span class="st todo">方案 · 未实现</span></p></div>
  <div class="card pad-lg rule-amber"><div class="card-h">B · 放得近</div><p class="small" style="font-size:17px;color:var(--ink-2)">同小区的代码连续排放；dispatcher 聚到 <code>.text.ejit_dispatch</code>，MissFn 进 <code>.text.unlikely</code>。</p><p style="margin:14px 0 0"><span class="st warn">已实现 · 默认关 · 待上板</span></p></div>
  <div class="card pad-lg rule-amber"><div class="card-h">C · 取得早</div><p class="small" style="font-size:17px;color:var(--ink-2)">切换小区前，把目标小区的热代码预取进 L2。</p><p style="margin:14px 0 0"><span class="st todo">方案 · 未实现</span></p></div>
  <div class="card pad-lg rule-lime"><div class="card-h">测量优先</div>
  <p class="small" style="font-size:17px;color:var(--ink-2)">PMU：<code>L1I_CACHE_REFILL / L1I_CACHE</code>。miss 率 <b>&lt; 1%</b> 算健康，<b>&gt; 5%</b> 才值得投入 —— 先测出是"挤占、超限、还是切换"，再决定做 A、B、C 里的哪一个。<b>这一页没有任何实测数字。</b></p></div>
</div>'''
    body = f'<div data-anim="left">{chart}</div><div class="step" data-step="1" style="margin-top:22px">{cards}</div>'
    notes = '''
<p>单小区的时候，热点路径只有 2–5 KB，L1 I-cache 轻松装下。但小区一多，每个小区一份特化代码，8 个小区就是 8 份。i-cache 的压力等于小区数乘以每份代码大小。</p>
<p>按 → 出现三个方案：合一份是治本（份数跟配置数走，不跟小区数走），放得近治挤占，取得早治切换。诚实状态：放得近已经实现但默认关着、等上板；另外两个还是方案。</p>
<p>最右边那张卡最重要：先测。没有 PMU 基线之前，这页上的所有数字都是估计。</p>
'''
    return dict(act=ACT, name='icache', kicker='第四幕 · 小区一多', title='多小区的代价：代码份数跟着小区数走', body=body, steps=1, notes=notes)


def s31_pgo_calib():
    W, H = 1408, 250
    o = []
    o.append(box(0, 70, 250, 110, '12 个真实闭包', ['EJIT 测试程序'], 'box'))
    o.append(box(310, 70, 250, 110, 'AOT 预内联之后', ['剩余 callee = 0'], 'box-ink'))
    o.append(box(620, 70, 250, 110, 'JIT PGO 内联', ['无事可做'], 'box-red'))
    o.append(box(930, 70, 250, 110, 'MachineBlockPlacement', ['唯一还有事做的'], 'box-ice'))
    for x in (250, 560, 870):
        o.append(arrow(f'M{x + 6} 125 H{x + 54}', 'ink'))
    o.append(T(745, 46, 'Stage 3a：no-op', 't-red b700', anchor='middle', size=14))
    o.append(T(1055, 46, 'Stage 1：唯一的收益来源', 't-ice b700', anchor='middle', size=14))
    o.append(T(1250, 125, '→', 't-3 b700', anchor='middle', size=22))
    o.append(T(1250, 150, '', 't-3', size=12))
    o.append(T(1396, 112, '热路径布局', 'zh t-2 b700', anchor='end', size=14))
    o.append(T(1396, 136, '分支方向翻转', 'zh t-2', anchor='end', size=13))
    o.append(T(1396, 158, '冷块外提', 'zh t-2', anchor='end', size=13))
    tbl = '''
<table class="tbl tight" style="font-size:17px;margin-top:6px">
<thead><tr><th style="width:30%">PGO 的阶段</th><th style="width:30%">预期</th><th>在真实 EJIT 闭包上</th></tr></thead>
<tbody>
<tr><td>内联（ModuleInliner）</td><td>按热度补内联</td><td><span class="st bad">no-op</span> AOT 预内联已经把 callee 全部消化掉</td></tr>
<tr><td>块布局（MachineBlockPlacement）</td><td>热路径顺排</td><td><span class="st ok">有效</span> 构造用例：taken 99% → fall-through；"收益真实但非数量级"</td></tr>
<tr><td>间接调用提升（ICP）</td><td>按目标特化</td><td><span class="st warn">有限</span> 真实闭包里间接调用很少</td></tr>
<tr><td>代价</td><td>—</td><td>运行时 +640 KB · 插桩 · 多一次重编译</td></tr>
</tbody></table>
<p class="p" style="font-size:20px;margin-top:18px">所以 <b>go / no-go 至今没有拍板</b>："MBP 的收益，是否配得上 +640 KB"。设计文档里自己写着：等真机数据。中途还有过一次<b style="color:var(--red)">方向反转</b>（v0.10）——推翻了前两个版本"非内联版常优于内联版"的假设。</p>'''
    body = svg(''.join(o), W, H) + tbl
    notes = '''
<p>PGO 做出来之后，我们拿 12 个真实的 EJIT 程序校准了一遍，结论很尴尬：AOT 预内联已经把所有 callee 消化了，JIT 的 PGO 内联阶段无事可做，是个 no-op。</p>
<p>真正还有事做的只剩后端的块布局：分支方向翻转、冷块外提。一个构造出来的用例里，taken 99% 的分支从跳转变成了 fall-through。但文档自己的措辞是"收益真实，但不是数量级"。</p>
<p>所以 go/no-go 到今天没有拍板：这点收益配不配得上 640 KB 和一次额外的重编译。中途还推翻过一次自己的假设。这些我们都写进了文档。</p>
'''
    return dict(act=ACT, name='pgo-calib', kicker='第四幕 · 校准', title='PGO 的内联阶段是个 no-op —— 而且我们是量出来的', body=body, notes=notes)


def s32_todo():
    rows = [
        ('bad', '产品级收益基线', '板端 PMU、真实业务闭环。到今天为止，所有"快多少"都是主机微基准。'),
        ('todo', '跨核真机验证', '跨核 I/D-cache 一致性 · 真实 core id 与选举 · 共享 section 同 VA · SRE_TaskDelete 真 join · worker 栈峰值。'),
        ('todo', '多小区 i-cache 消减', '合一份 / 放得近 / 取得早：一个默认关、两个没做。等 PMU 基线。'),
        ('bad', '2 个集成测试是红的', 'closure_slim（"no JIT entries"）；volatile（cacheHits 断言 —— inline cache 命中不再计入，是测试过时，功能断言全过）。'),
        ('warn', '2 个 VP 多核测试本机没跑', '需要 EJIT_SRE_PGO_VALUE_PROFILE=ON 的运行时构建。'),
        ('warn', '补充套件的历史红', 'LFD 文档记录 token 模式 162 例中 22 失败；HEAD 上未复核。'),
        ('todo', 'SPEC 里的"未来扩展"', '函数指针特化 · 自定义标量时间窗 · 多时间窗隐式依赖 · 源码级调试。'),
        ('ok', '已知并接受', '旧版编译的二进制与新运行时不兼容（配置布局变了）—— 升级就整体重编。'),
    ]
    trs = ''.join(f'<tr><td style="width:36px"><span class="st {st}"></span></td><td style="width:27%"><b>{t}</b></td><td>{d}</td></tr>' for st, t, d in rows)
    body = f'''<table class="tbl air" style="font-size:17.5px"><tbody>{trs}</tbody></table>
<p class="p" style="font-size:20px;margin-top:16px;color:var(--ink-3)">测试状态口径：{M['it']}（31 个目标，{M['build']}）。</p>'''
    notes = '''
<p>这是今天 HEAD 上的真实待办。不要跳过任何一行，但也不要逐字念 —— 每行一句话。</p>
<p>两行红的是集成测试：closure_slim 报"没有 JIT entry"，是真问题；volatile 那个是测试断言过时了（inline cache 命中不再走运行时查询，计数器自然不涨），功能断言全过。</p>
<p>最后一行是"已知并接受"：新旧版本不兼容，我们选择不兼容而不是背一堆历史包袱。</p>
'''
    return dict(act=ACT, name='todo', kicker='第四幕 · 账本', title='没做完的、待真机的、以及我们选择不做的', body=body, notes=notes)


def s33_scale():
    gone = ['EJitSyncCompiler（同步编译）', 'EJitAsyncCompiler', '私有 taskpool', 'LRU 缓存 EJitCache', '双引擎', '函数体计时',
            '固定 arity wrapper 入口', 'ejit_activate_array']
    chips = ''.join(f'<span class="mono-tag ink" style="text-decoration:line-through;text-decoration-color:var(--red);text-decoration-thickness:2px;font-weight:500">{c}</span>' for c in gone)
    html = f'''<div style="padding-top:2px">
  <div class="kicker" data-anim="up">第四幕 · 规模</div>
  <h2 class="title" data-anim="up" style="margin-bottom:22px">两万五千行，七个属性，一颗牙齿都没动的 LLVM</h2>
  <div class="grid g4" style="gap:18px">
    <div class="kpi c-ice" data-anim="up" style="padding:20px 24px"><div class="l">EJIT 代码</div><div class="v" style="font-size:64px" data-count="25138" data-dur="1200">0</div><div class="n">行：运行时 15.4K · 头文件 6.8K · AOT pass 2.8K</div></div>
    <div class="kpi c-ice" data-anim="up" style="--d:.1s;padding:20px 24px"><div class="l">最大的单文件</div><div class="v" style="font-size:64px" data-count="3719" data-dur="1200">0</div><div class="n">EJitSharedTaskPool.cpp —— 一个手搓的并发运行时</div></div>
    <div class="kpi c-violet" data-anim="up" style="--d:.2s;padding:20px 24px"><div class="l">Clang 侧</div><div class="v" style="font-size:64px">7<small>属性</small></div><div class="n">SemaEJIT.cpp + CGEJIT.cpp ≈ 1.5K 行</div></div>
    <div class="kpi c-violet" data-anim="up" style="--d:.3s;padding:20px 24px"><div class="l">核心 LLVM</div><div class="v" style="font-size:64px" data-count="77" data-dur="900">0</div><div class="n">个文件，几乎全是 TRIM 开关；lld 1 个文件 +7/−1</div></div>
  </div>
  <div class="grid g3" style="gap:18px;margin-top:18px">
    <div class="kpi c-lime" data-anim="up" style="--d:.4s;padding:16px 24px"><div class="l">gtest · 今天</div><div class="v" style="font-size:44px">272 · 52 · 169</div><div class="n">EJITTests（另 2 skipped）· CodePool · SharedTaskPool，全绿</div></div>
    <div class="kpi c-lime" data-anim="up" style="--d:.5s;padding:16px 24px"><div class="l">lit · 今天</div><div class="v" style="font-size:44px">55 / 55 · 28 / 28</div><div class="n">AOT pass lit · clang CodeGen/Sema lit</div></div>
    <div class="kpi c-amber" data-anim="up" style="--d:.6s;padding:16px 24px"><div class="l">集成测试 · 今天</div><div class="v" style="font-size:44px">27 过 · 2 红</div><div class="n">31 个目标里另有 2 个需要 VP 构建配置</div></div>
  </div>
  <p class="p step" data-step="1" style="font-size:21px;margin:22px 0 10px">还有一组数字是往下的 —— 这个月删掉的：</p>
  <div class="step" data-step="1" style="display:flex;flex-wrap:wrap;gap:10px">{chips}</div>
</div>'''
    notes = '''
<p>规模数字，快速带过。两万五千行 EJIT 自研代码，最大的单文件是那个手搓的并发运行时。</p>
<p>值得强调的是右下：对核心 LLVM 的改动 77 个文件，几乎全是编译开关；lld 只动了一个文件七行。clang 侧只有七个属性。</p>
<p>最后按 →：往下走的那组数字。这个月我们删掉了同步编译、私有 taskpool、LRU 缓存、函数体计时。少掉的代码，是最不会出 bug 的代码。</p>
'''
    return dict(act=ACT, name='scale', html=html, steps=1, notes=notes)


def slides():
    return [s26_opener(), s27_speedup(), s28_range(), s29_probe(), s30_icache(), s31_pgo_calib(), s32_todo(), s33_scale()]
