# -*- coding: utf-8 -*-
"""Act 0 — 开场"""
from svgkit import svg, T, RECT, PATH, arrow, code, dspy


def s1_title():
    html = '''
<canvas class="canvas hero" data-hero="title"></canvas>
<div style="position:absolute;left:120px;top:84px;width:1360px">
  <div class="kicker" data-anim="up"><b>EJIT</b>&nbsp;· EmbeddedJIT · 时间窗常量 × 运行时特化</div>
  <h1 class="display" style="font-size:104px;line-height:1.18;margin:6px 0 0" data-anim="up">把运行时，<br>变成编译期。</h1>
  <p style="font:400 24px/1.5 var(--sans);color:var(--ink-2);margin:22px 0 0;max-width:1100px" data-anim="up">一个长在裸核固件里的 JIT 编译器 —— 以及，我们如何在一颗没有
  <code style="font:700 .9em/1 var(--mono);color:var(--ink);background:var(--bg-3);padding:3px 7px;border-radius:6px;white-space:nowrap">std::thread</code> 的核上，手搓了一整个并发运行时。</p>
</div>'''
    dspy('把运行时，变成编译期。')
    notes = '''
<p><b>开场（30 秒）</b>：先别管 JIT。看屏幕下方这条信号线：在大部分时间里它上蹿下跳，编译器对它一无所知，只能老老实实每次从内存读；但在某几段时间里，它是平的——值不变。EJIT 做的事只有一件：<b>在那段平的时间里，把运行时的值当成编译期常量，重新编一份代码。</b></p>
<p>这条线今天会反复出现：窗口打开（activate）→ 特化代码接管；窗口关上（deactivate）→ 立刻回到 AOT，什么都不会坏。</p>
<p>自我介绍后直接翻页。不要在这里解释任何术语；下一页先给约束，让观众先觉得"这不可能"。</p>
'''
    return dict(act=0, name='title', cls='bleed', live='hero', html=html, notes=notes)


def s2_crazy():
    kpis = '''
<div class="grid g4" style="margin-top:46px">
  <div class="kpi c-amber" data-anim="up" style="--d:.25s"><div class="l">RAM 预算</div><div class="v" style="font-size:44px">100<small>KB</small>–2<small>MB</small></div><div class="n">整个进程的内存，不是编译器的</div><div class="src">SPEC4 §6 目标硬件</div></div>
  <div class="kpi c-amber" data-anim="up" style="--d:.37s"><div class="l">操作系统</div><div class="v">0</div><div class="n">裸核 SRE · aarch64_be · 无文件系统 · 无 dlsym</div><div class="src">EJIT_SRE_TASKPOOL §1.2</div></div>
  <div class="kpi c-amber" data-anim="up" style="--d:.49s"><div class="l">C++ 线程库</div><div class="v">0</div><div class="n">禁用 &lt;atomic&gt; · std::thread · std::mutex · future</div><div class="src">EJIT_SRE_TASKPOOL §1.2</div></div>
  <div class="kpi c-amber" data-anim="up" style="--d:.61s"><div class="l">首次编译延迟</div><div class="v" style="font-size:50px">&lt;100<small>ms</small></div><div class="n">软实时目标：可容忍一次，不能每次</div><div class="src">SPEC4 §10</div></div>
</div>
<p class="p step" data-step="1" style="margin-top:34px;font-size:23px">而我们要塞进去的东西叫 <b>LLVM</b>：裁剪前 <b>53 个静态库，117 MB</b>。<span style="color:var(--ink-3)">（EJIT_LIBRARY_TRIMMING §2.1）</span></p>'''
    html = f'''
<div class="stmt" style="padding-top:12px">
  <div class="kicker" data-anim="up">开场 · 先看约束</div>
  <h2 class="display" style="font-size:84px;line-height:1.24;margin:0" data-anim="up">{dspy('在这颗核上，')}<em>{dspy('连文件都没有')}</em>{dspy('。')}<br>{dspy('我们放了一个编译器进去。')}</h2>
  {kpis}
</div>'''
    notes = '''
<p>先把"疯狂"摆到桌面上。这不是服务器 JIT 的世界：没有操作系统、没有文件系统、没有 <code>std::thread</code>，整个进程只有 100 KB 到 2 MB 内存。我们却要把 LLVM 的 OrcJIT + JITLink 放进去，还要求它失败时永远能退回 AOT。</p>
<p>按 → 出现最后一行。停顿一下，让观众消化"117 MB vs 2 MB"。然后说：所以这个项目一半的工作量，其实是在做"减法"和"手搓"。第三幕会讲。</p>
<p>数字口径：RAM 与延迟是 SPEC4 的目标硬件假设；线程库禁用清单来自 taskpool 设计文档；117 MB 是裁剪前 x86_64 静态库总量。</p>
'''
    return dict(act=0, name='crazy', trans='rise', html=html, steps=1, notes=notes)


def s3_roadmap():
    acts = [
        ('开场', '约束 · 一分钟协议', 'ice'),
        ('问题', 'AOT 不知道 · JIT 来不及', 'amber'),
        ('机制', '标注 · bitcode · wrapper · PASS6 · 机器码', 'ice'),
        ('疯狂', '固件里的 LLVM · 手搓并发 · 跨核 · PGO', 'violet'),
        ('诚实', '收益 · 代价 · 没做完的', 'lime'),
        ('收束', '三个带走的东西', 'ice'),
    ]
    W, H = 1408, 560
    x0, x1, y = 8, 1400, 292
    out = [PATH(f'M{x0} {y} H{x1}', 'ln-ink')]
    for tx in range(x0, x1 + 1, 88):
        out.append(PATH(f'M{tx} {y - 6} V{y + 6}', 'ln-ink'))
    n, gap = len(acts), 24
    w = (x1 - x0 - gap * (n - 1)) / n
    for i, (name, desc, col) in enumerate(acts):
        x = x0 + i * (w + gap)
        d = f'--d:{0.15 + i * 0.12:.2f}s'
        out.append(f'<g data-anim="up" style="{d}">')
        out.append(RECT(x, y - 108, w, 216, f'band-{col}', 12))
        out.append(PATH(f'M{x} {y - 108} V{y + 108}', f'wire-{col}'))
        out.append(PATH(f'M{x + w} {y - 108} V{y + 108}', f'wire-{col}'))
        out.append(T(x + w / 2, y - 28, name, f'zh t-{col} b700', anchor='middle', size=42))
        items = desc.split(' · ')
        k = (len(items) + 1) // 2
        l1, l2 = ' · '.join(items[:k]), ' · '.join(items[k:])
        out.append(T(x + w / 2, y + 48, l1, 'zh t-2', anchor='middle', size=14.5))
        if l2:
            out.append(T(x + w / 2, y + 74, l2, 'zh t-2', anchor='middle', size=14.5))
        out.append(T(x + w / 2, y - 128, f'0{i}', 't-3 b700', anchor='middle', size=14))
        out.append('</g>')
    px = x0 + w / 2
    out.append(f'<g class="float"><path d="M{px} {y - 150} V{y - 116}" class="wire"/><circle cx="{px}" cy="{y - 156}" r="6" class="solid-ink"/>'
               f'<text x="{px + 14}" y="{y - 152}" class="zh t-ink b700" font-size="14">现在</text></g>')
    out.append(T(x1, y + 166, 't  →', 't-3', anchor='end', size=14))
    out.append(T(x0, y + 166, '每一幕 = 时间线上的一个窗口', 'zh t-3', size=14))
    body = svg(''.join(out), W, H) + '''
<p class="p" style="font-size:22px;margin-top:-6px">今天的讲述方式和 EJIT 的工作方式一样：<b>一次只打开一个窗口</b>。窗口关上，这一幕作废，下一幕重新来。</p>'''
    notes = '''
<p>路线图页，20 秒带过。提醒观众底部那条细线就是进度条：当前幕会亮。如果时间不够，第四幕"诚实"也不能跳——那是这场演讲里最有价值的部分。</p>
'''
    return dict(act=0, name='roadmap', kicker='开场 · 路线图', title='六个窗口，一次只开一个', body=body, notes=notes)


C1 = '''EJIT_GROUP(cell) CellCfg g_cellCfg[16];
EJIT_ENTRY
void process_cell(EJIT_DIM(cell) uint8_t idx) {
  if (g_cellCfg[idx].cellType == 0xFD) ...'''
C2 = '''ejit_init(&cfg);             // 或 ejit_init_pgo(&cfg)
// ... 写好 g_cellCfg[3] ...
ejit_activate("cell", 3);    // 窗口打开'''
C3 = '''process_cell(3);  // (1) AOT 函数体 + 入队，调用方不等
...               // worker：bitcode → 常量 → 机器码 → 发布
process_cell(3);  // (2) 命中：musttail 直跳特化代码'''
C4 = '''ejit_deactivate("cell", 3);    // 关窗：相关特化立刻作废
g_cellCfg[3].cellType = 0xEC;  // 改配置（EJIT_GUARD 函数）
ejit_activate("cell", 3);      // 重新打开 → 重新特化'''


def s4_minute():
    def card(step, x, y, tag, color, title, when, desc, snippet):
        return f'''<div class="step pop" data-step="{step}" style="position:absolute;left:{x}px;top:{y}px;width:600px;height:250px">
  <div class="card rule-{color}" style="height:100%;display:flex;flex-direction:column;padding:22px 28px">
    <div class="card-h"><span class="mono-tag {color}">{tag}</span>{title}<span style="font:500 13px/1 var(--mono);color:var(--ink-3);margin-left:auto">{when}</span></div>
    <p class="small" style="color:var(--ink-2);min-height:50px;font-size:15.5px;margin-bottom:12px">{desc}</p>
    {code(snippet, 'c', 'xs', maxw=500)}
  </div></div>'''
    c1 = card(1, 0, 0, '① 用户', 'ice', '标注', '写代码的时候',
              '标出"窗口内不变"的字段、它属于哪个时间窗，以及哪些函数值得特化。', C1)
    c2 = card(2, 808, 0, '② 业务', 'ice', '激活', '配置稳定之后',
              '业务确信配置不再变了，打开窗口。不用重编译，不用重启。', C2)
    c3 = card(3, 808, 306, '③ 运行时', 'violet', '特化', '后台 worker',
              '首次调用照常走 AOT；worker 把窗口内的值读出来、折成常量，编一份专用代码。', C3)
    c4 = card(4, 0, 306, '④ 业务', 'red', '失效', '要改配置的时候',
              '先关窗口。依赖它的特化代码全部作废，下一次调用自动回到 AOT。', C4)
    W, H = 1408, 556
    o = [f'<g class="step plain" data-step="2">{arrow("M610 125 H798", "ink")}{T(704, 112, "AOT 编译 · 链接", "zh t-3 b500", anchor="middle", size=13)}</g>']
    o.append(f'<g class="step plain" data-step="3">{arrow("M1108 256 V300", "violet")}{T(1122, 283, "第 1 次调用", "zh t-violet b700", size=13)}</g>')
    o.append(f'<g class="step plain" data-step="4">{arrow("M798 431 H610", "red")}{T(704, 419, "要改配置？先关窗口", "zh t-red b700", anchor="middle", size=13)}</g>')
    o.append(f'<g class="step plain" data-step="4">{arrow("M500 306 V278 H910 V256", "ice", True)}{T(705, 270, "重新 activate → 下一轮重新特化", "zh t-ice b700", anchor="middle", size=13)}</g>')
    body = (f'<div style="position:relative;height:556px">{svg("".join(o), W, H, extra="style=\"position:absolute;inset:0;pointer-events:none\"")}{c1}{c2}{c3}{c4}</div>'
            '<p class="p step" data-step="4" style="margin-top:20px;font-size:20px"><b>没有 deopt，没有解释器，没有预热停顿</b> —— 第一次调用永远是普通的 AOT 代码。'
            '<span style="color:var(--ink-3)">（同步编译已于 2026-11-18 退役：commit 792400b7cee7）</span></p>')
    notes = '''
<p>四步讲完 EJIT 的全部协议，它们构成一个环：标注 → 激活 → 特化 → 失效 → 再激活。按 → 逐个出现四张卡片和箭头。</p>
<p>强调两点：（1）用户只写标注，调用还是调原函数名，没有 API 入侵；（2）"先 deactivate 再改、改完再 activate"是唯一的契约——它取代了传统 JIT 里那一整套去优化（deopt）机制。</p>
<p>如果有架构背景的听众，可以补一句：async-only 是 11 月才定的，之前的同步模式已经整个删掉，代码更少、竞态也更少。</p>
'''
    return dict(act=0, name='minute', kicker='开场 · 一分钟版', title='EJIT 的全部协议：一个四步的环', body=body, steps=4, notes=notes)


def slides():
    return [s1_title(), s2_crazy(), s3_roadmap(), s4_minute()]
