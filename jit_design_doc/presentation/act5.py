# -*- coding: utf-8 -*-
"""Act 5 — 收束 + 附录"""
from svgkit import svg, T, RECT, PATH, LINE, CIRCLE, code, dspy

ACT = 5


def s34_takeaways():
    def row(n, step, title, desc):
        return f'''<div class="step left" data-step="{step}" style="display:flex;gap:40px;align-items:flex-start;padding:34px 0;border-bottom:1px solid var(--line)">
  <div style="font:800 112px/0.9 var(--mono);color:transparent;-webkit-text-stroke:1.6px var(--ice);letter-spacing:-.05em;flex:none;width:176px">{n}</div>
  <div style="padding-top:6px"><div style="font:700 40px/1.25 var(--sans);color:var(--ink);margin-bottom:14px">{title}</div>
  <p style="margin:0;font:400 22px/1.6 var(--sans);color:var(--ink-2);max-width:1120px">{desc}</p></div></div>'''
    body = (row('01', 1, '时间窗是一种"类型"', '别问一个值<b>是不是</b>常量，问它<b>在哪段时间里</b>是常量。把这个窗口写进程序，编译器就能替你把"这段时间"变成编译期。')
            + row('02', 2, '软标注 + 永远可回退 = 敢于疯狂的安全网', 'metadata 丢了只会少优化；窗口关上只会回到 AOT；每一处失败都通向"不优化"，没有一处通向"错"。这是敢把 LLVM 塞进一颗裸核的前提。')
            + row('03', 3, '先测量', '47 万倍是笑话，0.4 倍是事实，PGO 的内联是 no-op。这些数字都出自我们自己之手，也都写进了设计文档。'))
    notes = '''
<p>如果只能带走三句话，就是这三句。按 → 逐条出现，每条停五秒，不展开。</p>
'''
    return dict(act=ACT, name='takeaways', kicker='第五幕 · 收束', title='三个可以带走的东西', body=body, steps=3, notes=notes)


def s35_timeline():
    ev = [
        ('04-11', 'SPEC4 需求规格', 1), ('05-09', '立项', 0), ('06-16', '端到端 demo 打通', 0), ('06-22', '跨核共享 taskpool', 0),
        ('07-22', 'Inline Cache 直跳', 0), ('08-03', '固定代码池 · 4K 封固', 0), ('08-25', 'bound_ptr 板级 demo', 0),
        ('09-15', 'free_dim 特化', 0), ('10-27', 'NEON 向量化', 1), ('11-18', '退役同步编译', 1), ('11-24', '统一属性词汇表', 1),
    ]
    W, H = 1408, 556
    y = 258
    o = [PATH(f'M0 {y} H1408', 'ln-ink draw', 'pathLength="1"')]
    n = len(ev)
    step = 1408 / (n - 1)
    for i, (d, lab, verified) in enumerate(ev):
        x = round(i * step)
        up = i % 2 == 0
        o.append(f'<g data-anim="fade" style="--d:{.25 + i * .09:.2f}s">')
        o.append(LINE(x, y - 16, x, y + 16, 'ln-ink'))
        o.append(CIRCLE(x, y, 11 if verified else 9, 'solid-ice' if verified else 'box-ink'))
        if up:
            o.append(LINE(x, y - 26, x, y - 66, 'ln'))
            o.append(T(x, y - 118, d, 't-ink b800', anchor='middle', size=24))
            o.append(T(x, y - 82, lab, 'zh t-2 b500', anchor='middle', size=18.5))
        else:
            o.append(LINE(x, y + 26, x, y + 66, 'ln'))
            o.append(T(x, y + 112, d, 't-ink b800', anchor='middle', size=24))
            o.append(T(x, y + 146, lab, 'zh t-2 b500', anchor='middle', size=18.5))
        o.append('</g>')
    o.append(T(1408, y + 234, '2026 →', 't-3 b500', anchor='end', size=14))
    o.append(CIRCLE(11, y + 229, 11, 'solid-ice') + T(32, y + 234, 'git 里查得到', 'zh t-3', size=14.5))
    o.append(CIRCLE(204, y + 229, 9, 'box-ink') + T(222, y + 234, '来自 9 月进展 PPT（仓库是浅克隆，更早的历史本地不可见）', 'zh t-3', size=14.5))
    body = svg(''.join(o), W, H) + '''
<p class="p" style="font-size:24px;margin-top:2px">七个多月。从一份 614 行的需求规格，到一个 25,138 行、能跑在裸核上的 JIT。<span style="color:var(--ink-3)">PR 编号已经走到 #274。</span></p>'''
    notes = '''
<p>时间线，20 秒。实心点是在 git 里能查到的；空心点来自 9 月的进展 PPT，因为仓库是浅克隆，更早的历史本地看不到。</p>
<p>最后三个节点值得停一下：10 月底打开 NEON 向量化；11 月 18 日删掉同步编译和私有 taskpool；11 月 24 日把散落的属性名统一成今天看到的这七个。</p>
'''
    return dict(act=ACT, name='timeline', kicker='第五幕 · 七个月', title='从一份规格，到一颗裸核上的 JIT', body=body, notes=notes)


def s36_end():
    html = '''
<canvas class="canvas hero" data-hero="end"></canvas>
<div style="position:absolute;left:120px;top:110px;width:1300px">
  <div class="kicker" data-anim="up"><b>EJIT</b>&nbsp;· 收束</div>
  <h1 class="display" style="font-size:92px;line-height:1.22;margin:10px 0 0" data-anim="up">窗口会关上。<br>代码会回退。<em style="font-style:normal;color:var(--ice)">什么都不会坏。</em></h1>
  <p style="font:400 24px/1.5 var(--sans);color:var(--ink-2);margin:28px 0 0" data-anim="up">谢谢。提问时间。</p>
  <p style="font:400 14px/1.6 var(--mono);color:var(--ink-3);margin:18px 0 0;max-width:1000px" data-anim="up">← → 翻页 · O 总览 · N 备注 · P 演讲者视图 · T 主题 · ? 帮助<br>数字出处与测量方法见附录 A2</p>
</div>'''
    dspy('窗口会关上。代码会回退。什么都不会坏。')
    notes = '''
<p>最后回到开场那条线。窗口总会关上——配置总会改。关上之后发生的事只有一件：下一次调用，回到 AOT。没有 deopt 的风暴，没有状态需要恢复。</p>
<p>"什么都不会坏"不是口号，是设计约束：SPEC 的第 8 节只有一句话——所有 JIT 失败场景必须 fallback 到 AOT，不能导致程序崩溃。</p>
<p>停在这里进 Q&A。附录里有 API 速查、测量方法、术语和源码索引。</p>
'''
    return dict(act=ACT, name='end', cls='bleed', live='hero', html=html, notes=notes)


def slides():
    return [s34_takeaways(), s35_timeline(), s36_end()]
