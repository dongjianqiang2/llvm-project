# -*- coding: utf-8 -*-
"""Act 3 — 疯狂：工程上最野的部分"""
from svgkit import svg, T, TL, RECT, PATH, LINE, arrow, pkt, box, lane, hv, vh, rounded_poly, code, dspy
from data import M

ACT = 3


def s18_opener():
    html = f'''
<div class="opener">
  <div class="num">03</div>
  <div class="kicker" data-anim="up">第三幕 · 疯狂</div>
  <h2 class="display" data-anim="up">{dspy('现在，疯狂的部分。')}</h2>
  <p class="thesis" data-anim="up">一个编译器要在<b style="color:var(--ink)">没有操作系统的核</b>上活下来，<br>它周围的每一样东西都得自己造。</p>
  <p data-anim="up" style="font:400 20px/1.6 var(--sans);color:var(--ink-3);margin:40px 0 0;max-width:1100px">接下来每一页，都对应第一幕那张表里的一个"✕"。它们的共同点是：<b style="color:var(--ink-2)">没有一个是为了跑得更快</b>，全是为了"能跑"。</p>
</div>'''
    notes = '''
<p>语气在这里要变。前两幕是在解释一个想法；这一幕是在展示一堆"不该存在但存在了"的工程物。</p>
<p>提醒观众：接下来每一页对应第一幕"六关"里的一关。</p>
'''
    return dict(act=ACT, name='opener3', cls='bleed', html=html, notes=notes)


def s19_trim():
    W, H = 740, 566
    rows = [('53 个 .a', 117, 80), ('extract', 99, 46), ('gc-merge', 59, 30), ('ejit.o', 37, None), ('链接后', 21, 12)]
    sc = 4.1
    o = []
    o.append(RECT(0, 0, 11, 11, 'solid-amber', 2) + T(19, 10, 'x86-64 · 实测', 't-2', size=13.5))
    o.append(RECT(190, 0, 11, 11, 'solid-ice', 2) + T(209, 10, 'aarch64 · 末项为预估', 't-2', size=13.5))
    for i, (lab, x86, a64) in enumerate(rows):
        y = 56 + i * 100
        d = .2 + i * .14
        o.append(T(0, y + 22, lab, 't-ink b700' + (' zh' if any('一' <= c <= '鿿' for c in lab) else ''), size=16))
        o.append(f'<g data-anim="grow" style="--d:{d:.2f}s">')
        o.append(RECT(150, y, x86 * sc, 30, 'solid-amber', 4))
        o.append(T(150 + x86 * sc + 12, y + 22, f'{x86} MB', 't-ink b700', size=15))
        o.append('</g>')
        if a64 is not None:
            o.append(f'<g data-anim="grow" style="--d:{d + .07:.2f}s">')
            last = i == len(rows) - 1
            o.append(RECT(150, y + 34, a64 * sc, 30, 'band-ice' if last else 'solid-ice', 4, 'stroke="var(--ice)" stroke-width="1.4"' + (' stroke-dasharray="5 4"' if last else '')))
            o.append(T(150 + a64 * sc + 12, y + 56, f'{"≈" if last else ""}{a64} MB', 't-2 b700', size=15))
            o.append('</g>')
    o.append(LINE(150, 40, 150, 530, 'ln-ink'))
    o.append(T(0, H - 4, 'lipo 三步：extract（只留被引用的 .o）→ gc-merge（ld -r --gc-sections --entry=ejit_init）→ merge', 'zh t-3', size=13))
    chart = svg(''.join(o), W, H)
    cut = ['DWARF · CodeView', 'GlobalISel', 'ObjCARC', 'Win / Wasm / AIX EH', 'SME / SVE / AdvSIMD pass', '非 greedy 寄存器分配器',
           'MachineVerifier', '汇编文本打印', 'MachinePipeliner', '非 ELF writer', 'x86 依赖（AArch64 上）', 'GVNHoist / NewGVN', 'AutoFDO', 'MIR 序列化', 'RuntimeDyld fallback']
    chips = ''.join(f'<span class="mono-tag ink" style="font-size:13.5px;font-weight:500">{c}</span>' for c in cut)
    right = f'''
<div class="card pad-lg rule-violet" style="flex:1"><div class="card-h">EJIT_TRIM_LLVM_BACKEND：砍掉 15 类东西</div>
  <div style="display:flex;flex-wrap:wrap;gap:8px;margin-top:4px">{chips}</div>
  <p class="small" style="font-size:18px;color:var(--ink-2);margin-top:14px">剩下：<b>SelectionDAG + FastISel · RAGreedy · ELF</b>。</p></div>
<div class="grid g2" style="gap:16px;flex:1">
  <div class="card pad-lg rule-lime"><div class="card-h">怎么证明没砍坏</div>
    <p class="small" style="font-size:18px;color:var(--ink-2)"><b>llc -O2</b>：1793 个输入，逐字节一致<br><b>opt O2 / O3</b>：266 个输入，一致<br>loadable <b>19.57 → 18.51 MB</b>（−1.06 MB）</p>
    <p class="small" style="color:var(--ink-3);margin-top:10px">commit 90b0b02e5227</p></div>
  <div class="card pad-lg rule-ice"><div class="card-h">EJIT 自己</div>
    <p class="small" style="font-size:18px;color:var(--ink-2)"><code>libLLVMEJIT.a</code> ≈ 0.5 – 1.5 MB。<br>其余全是 LLVM。<br><br>PGO 组件再加 <b>+640 KB</b>。</p></div>
</div>'''
    body = f'<div class="grid g-52-48" style="gap:30px"><div data-anim="left">{chart}</div><div class="step" data-step="1" style="display:flex;flex-direction:column;gap:16px">{right}</div></div>'
    notes = '''
<p>117 MB 的 LLVM 怎么进 2 MB 的固件？答案很朴素：砍。lipo 三步把 53 个静态库压成一个 ejit.o，再靠 EJIT_TRIM_LLVM_BACKEND 在源码层砍掉 15 类用不到的东西。</p>
<p>按 → 出现右边。最值得念的是"怎么证明没砍坏"：不是看代码，是拿 1793 个输入跑 llc 对比字节。这是整个项目我最信得过的一个数字。</p>
<p>诚实说明：aarch64 的最后一项（≈12 MB）是文档里的预估，不是实测。</p>
'''
    return dict(act=ACT, name='trim', kicker='第三幕 · 减法', title='把 LLVM 塞进固件：117 MB 是怎么瘦下来的', body=body, steps=1, notes=notes)


def s20_pool():
    left = f'''
<div class="step" data-step="1" style="margin-bottom:14px"><div class="card pad-sm rule-violet"><div class="card-h">① 开池</div>
<p class="small" style="color:var(--ink-2)"><code>SRE_MemDbgAlloc</code> 一块 <b>2 MiB</b>、对齐；一次 <code>split_2m_to_4k</code> → <b>512 个 4 KiB 页</b>。</p></div></div>
<div class="step" data-step="2" style="margin-bottom:14px"><div class="card pad-sm rule-amber"><div class="card-h">② 写（RW）</div>
<p class="small" style="color:var(--ink-2)">bump 分配，<b>每个特化至少独占一页</b>。池子保持可写 —— <code>allocate</code> 绝不 <code>enable_ex</code>。</p></div></div>
<div class="step" data-step="3" style="margin-bottom:14px"><div class="card pad-sm rule-ice"><div class="card-h">③ 封（RX）</div>
<p class="small" style="color:var(--ink-2)">finalize 逐页 <code>enable_ex</code>；任一页失败，整个 finalize 失败。<code>sealAndSyncCache</code> 清 D-cache、失效 I-cache。</p></div></div>
<div class="step" data-step="4"><div class="card pad-sm rule-red"><div class="card-h">④ 代价</div>
<p class="small" style="color:var(--ink-2)"><code>compute_cell</code> 特化后 <b>6 字节</b>，独占 <b>4096 字节</b>。封过的池子永不复用；代码永不释放。</p></div></div>'''
    legend = ('<div style="display:flex;gap:18px;align-items:center;font:500 13px/1 var(--mono);color:var(--ink-2);margin-bottom:10px">'
              '<span><i class="sw" style="background:var(--ink-4)"></i>空闲</span><span><i class="sw" style="background:var(--amber-mark)"></i>RW · 正在写</span>'
              '<span><i class="sw" style="background:var(--ice-mark)"></i>RX · 已封固</span><span><i class="sw" style="background:transparent;border:2px solid var(--red)"></i>6 B 的函数</span></div>')
    right = f'''<div class="step" data-step="1">{legend}<svg class="svg" viewBox="0 0 960 416" style="width:960px;height:416px"><g class="pool-grid"></g></svg>
<p class="small" style="color:var(--ink-3);margin-top:10px">一个 2 MiB 池 = 512 × 4 KiB（全部画出）。生产预设另有 <code>.text.ejit</code> 固定池 16 MiB（≈ 7 池）；Tier-1 插桩代码放远端动态池。</p></div>'''
    quote = '''<p class="step" data-step="4" style="font:500 17px/1.5 var(--sans);color:var(--ink-2);margin:16px 0 0;padding-left:16px;border-left:3px solid var(--red)">"<b>enable_rw 必须同时清执行位</b> —— 只翻 AP 位，会留下一个 RWX 的写窗口。"<span style="color:var(--ink-3)"> · EJIT_SRE_CODE_POOL §14.2</span></p>'''
    body = f'<div class="grid g-35-65" style="gap:30px"><div>{left}</div><div>{right}</div></div>{quote}'
    notes = '''
<p>裸核上没有 mmap，没有 mprotect。可执行内存要靠平台接口一页一页地要。按 → 分四步。</p>
<p>① 开一块 2 MiB，切成 512 个 4 KiB 页。② 写代码的时候整块是 RW —— 写入期间绝不同时可执行。③ finalize 时逐页封成 RX，同时清 D-cache、失效 I-cache。④ 代价：每个特化至少占一页。那个 6 字节的函数，独占 4096 字节。</p>
<p>最下面那句引言是血泪教训：把页改回可写时必须同时清掉执行位，否则就是一个 RWX 窗口。</p>
'''
    return dict(act=ACT, name='pool', kicker='第三幕 · W^X', title='写完就封：一个 6 字节的函数，独占 4096 字节', body=body, steps=4, live='pool', notes=notes)


def s21_threads():
    def card(step, title, desc):
        return f'''<div class="step left" data-step="{step}" style="display:flex;gap:18px;align-items:flex-start;padding:17px 0;border-bottom:1px solid var(--line)">
  <span class="mono-tag violet" style="margin-top:2px">{title}</span><p style="margin:0;font:400 17px/1.55 var(--sans);color:var(--ink-2)">{desc}</p></div>'''
    items = [
        (1, 'EJitAtomic', '只用 <code>__atomic_*</code> 内建，不 include <code>&lt;atomic&gt;</code>；memory order 全部显式写出。'),
        (2, 'MPSC 环', 'Vyukov 有界环 <b>1024 槽</b>。producer <code>CAS(enqueuePos)</code> 抢槽，写完 <code>storeRelease</code> 发布。'),
        (3, '去重', '<code>inFlight[funcIndex]</code> 存 generation，<code>CAS(0→gen)</code> 占位，O(1)。同一个函数在队列里最多一份。'),
        (4, '32 桶缓存', '32 桶 × 16 槽的 POD，没有哈希表分配。读 <code>tryRead</code> 不等待；写者举旗后自旋，等读者归零。'),
        (5, 'version', 'U32 单调递增。worker 过两个检查点，发布前在桶写锁里再校验一次（commit gate）。'),
    ]
    left = ''.join(card(*it) for it in items)
    body = f'''<div class="grid" style="grid-template-columns:700px 1fr;gap:0"><div data-anim="left">{left}
<p class="step" data-step="5" style="font:500 18px/1.55 var(--sans);color:var(--ink-2);margin:20px 0 0">"单 consumer 是 Vyukov 无锁正确性的<b>硬约束</b>"：多一个消费者，就会出现"幽灵空"伪信号。</p></div></div>
<canvas class="canvas ring"></canvas>'''
    notes = '''
<p>没有 <code>std::thread</code>，没有 <code>&lt;atomic&gt;</code>，没有 <code>std::mutex</code>。但我们需要：多个业务核同时提交编译请求、一个 worker 核消费、所有核共享一份结果缓存，还不能有锁。</p>
<p>右边是仿真：三个 producer（业务核）往一个 1024 槽的环里抢位；同一个函数被两个核同时提交时，第二个会被去重弹回（红色的那个）；唯一的 consumer（worker）从另一头取。</p>
<p>按 → 逐个出现五个部件。最后一句话最重要：单 consumer 不是偷懒，是 Vyukov 队列无锁正确性的前提。</p>
'''
    return dict(act=ACT, name='threads', kicker='第三幕 · 并发', title='没有 std::thread？那就手搓一个并发运行时', body=body, steps=5, live='ring', notes=notes)


def s22_cross_core():
    W, H = 1408, 560
    o = []
    # cores
    for i in range(8):
        x = 44 + i * 168
        cls = 'box-violet' if i == 6 else 'box'
        o.append(f'<g class="core core-{i}">')
        o.append(RECT(x, 4, 128, 62, cls, 10))
        o.append(T(x + 64, 31, f'Core {i}', 't-ink b700', anchor='middle', size=17))
        o.append(T(x + 64, 52, 'worker' if i == 6 else 'ejit_init()', 't-violet b700' if i == 6 else 't-3', anchor='middle', size=13.5))
        o.append('</g>')
    # shared blob
    o.append(RECT(300, 150, 808, 168, 'box-ice', 14))
    o.append(T(324, 182, 'EJIT_SHARED_SECTION · 共享 blob', 't-ice b700', size=17))
    o.append(T(324, 206, '所有核映射到同一个虚拟地址（same-VA）；__atomic_* 直接作用在字段上', 'zh t-2', size=14.5))
    fields = ['magic · abiVersion · size', 'initState', 'TaskQueue · inFlight[]', '32 桶结果缓存', 'SwitchController · version', 'executableCoreMask']
    for k, f in enumerate(fields):
        fx = 324 + (k % 3) * 262
        fy = 236 + (k // 3) * 40
        o.append(RECT(fx, fy, 244, 32, 'box', 6) + T(fx + 122, fy + 21, f, 't-2 b500', anchor='middle', size=13.5))
    # arrows core -> blob
    for i in range(8):
        x = 44 + i * 168 + 64
        col = 'violet' if i == 6 else 'ink'
        o.append(arrow(f'M{x} 66 V144', col, i != 6, extra='stroke-width="1.4"' if i != 6 else 'stroke-width="2.2"'))
    o.append(T(44 + 6 * 168 + 64 + 12, 112, 'CAS 赢了：成为 owner', 'zh t-violet b700', size=13))
    o.append(T(44 + 0 * 168 + 64 + 12, 112, '输家：校验 magic / version / size 后 attach', 'zh t-3', size=13))
    # state machine
    o.append(T(300, 356, 'initState', 't-3 b700', size=13))
    sm = [('Uninitialized', 396), ('Initializing', 640), ('Ready', 884)]
    for k, (s, sx) in enumerate(sm):
        o.append(RECT(sx, 338, 190 if k < 2 else 110, 30, 'box', 6) + T(sx + (95 if k < 2 else 55), 358, s, 't-ink b700', anchor='middle', size=13))
    o.append(arrow('M590 353 H634', 'ink'))
    o.append(arrow('M834 353 H878', 'ink'))
    o.append(T(612, 332, 'CAS', 't-3', anchor='middle', size=12))
    o.append(T(856, 332, 'storeRelease', 't-3', anchor='middle', size=12))
    o.append(T(1020, 358, 'peer 见到 Initializing 只做有限次 yield，绝不创建第二个 worker', 'zh t-3', size=12.5))
    # static asserts (step 2)
    o.append('<g class="step plain" data-step="2">')
    o.append(RECT(300, 386, 808, 40, 'box-ink', 8))
    o.append(T(316, 412, 'static_assert：standard-layout · trivially destructible · trivially default constructible · alignof == 64', 't-2 b500', size=13.5))
    o.append(T(1124, 412, '→ 零 .init_array', 'zh t-amber b700', size=14))
    o.append('</g>')
    # executable core mask (step 3)
    o.append('<g class="step plain" data-step="3">')
    o.append(RECT(300, 444, 808, 104, 'box-amber', 12))
    o.append(T(324, 472, '同 VA ≠ 同执行权限：enable_ex 翻的是调用核自己的 stage-1 页表', 'zh t-amber b700', size=15.5))
    o.append(T(324, 496, '每个核得亲自为自己的页表封固一次；做过的核记在 executableCoreMask 里，之后走快路径', 'zh t-2', size=13.5))
    for i in range(8):
        bx = 324 + i * 48
        on = i == 6
        o.append(RECT(bx, 506, 40, 28, 'box-violet' if on else 'box', 6) + T(bx + 20, 525, str(i), ('t-violet b700' if on else 't-3'), anchor='middle', size=13))
    o.append(T(724, 525, 'bit i = 核 i 已对该页 enable_ex', 'zh t-3', size=12.5))
    o.append('</g>')
    body = svg(''.join(o), W, H) + '''
<p class="p step" data-step="3" style="font-size:19px;margin-top:2px"><b>待真机</b>：真实 core id 与跨核选举、共享 section 同 VA、跨核 I/D-cache 一致性 —— host 上只能 mock，文档自己写着"未在本机验证"。</p>'''
    notes = '''
<p>多核怎么办？八个核都会调 <code>ejit_init</code>，但只能有一个 worker。做法是一段 CAS：谁把 initState 从 Uninitialized 改成 Initializing，谁就是 owner，由它把共享 blob 铺好，最后 <code>storeRelease(Ready)</code>。其他核看到 Initializing 只会让出几次，绝不会"等不及自己再建一个"。</p>
<p>按 → 第二步：共享 blob 必须是纯 POD，连构造函数都不能有 —— 否则后启动的核一跑构造函数，就把正在运行的队列清零了。</p>
<p>第三步是最反直觉的一条：同 VA ≠ 同执行权限。<code>enable_ex</code> 改的是调用核自己的页表，所以每个核都得自己封一遍。这些都还没上真机验证。</p>
'''
    return dict(act=ACT, name='cross-core', kicker='第三幕 · 跨核', title='八个核，一个 worker：用 CAS 选出来的独裁者', body=body, steps=3, notes=notes)


def s23_frontier():
    W, H = 1408, 372
    steps = [
        ('标量 / 数组全局', 'EJIT_GROUP + PERIOD_CONST', '索引必须是常量'),
        ('指针全局 + bounds', '显式 bounds 或分配器 lookup', '对象存活 · 同 VA · 无并发写'),
        ('EJIT_FREE_DIM 参数', '下标只进地址运算', '值不随该下标变（用户断言）'),
        ('局部 free dim', '局部变量也能当维度', '只读 · 不可取地址'),
        ('EJIT_BOUND_PTR', '指针参数绑定到维度', '≤ 8 个 / 入口 · 借用不拷贝'),
        ('anchored witness', 'bounds 未知也敢折', '到所选元素的前缀可读 · 值均匀'),
    ]
    o = []
    n = len(steps)
    sw = (1408 - (n - 1) * 14) / n
    for i, (title, what, pre) in enumerate(steps):
        x = i * (sw + 14)
        h = 56 + i * 40
        y = 300 - h
        o.append(f'<g class="step pop" data-step="{i + 1}">')
        o.append(RECT(x, y, sw, h, 'box-ice' if i < 4 else 'box-violet', 10))
        o.append(T(x + sw / 2, y + 26, f'{i}', 't-3 b700', anchor='middle', size=12))
        o.append(T(x + sw / 2, y + 50, title, ('t-ink b700 zh' if any('一' <= c <= '鿿' for c in title) else 't-ink b700'), anchor='middle', size=14.5))
        o.append(T(x + sw / 2, y + h - 22, what, 'zh t-3', anchor='middle', size=12))
        o.append(T(x + sw / 2, 322, pre, 'zh t-amber b500', anchor='middle', size=12.5))
        o.append('</g>')
    o.append(LINE(0, 300.5, 1408, 300.5, 'ln-ink'))
    o.append(T(0, 356, '每往外一步，解锁一种新的访问形态 —— 也多一条必须成立的前提', 'zh t-3', size=13))
    o.append(T(1408, 356, '越往右越新', 'zh t-3', anchor='end', size=13))
    body = svg(''.join(o), W, H) + '''
<div class="grid g-60-40" style="gap:26px;margin-top:4px">
  <div class="card pad-lg rule-amber step" data-step="5"><div class="card-h">最惊险的一步：witness</div>
    <p class="small" style="color:var(--ink-2)">bounds 未知时，PASS6 算地址代入的代表值是 <b>0</b>。它<b>只用于地址运算，绝不写进 IR</b>——参数、GEP、store 全保持真值。为什么这么小心？设计文档原话：把 slot→0 写进 IR，会"send all twenty TTIs' stores into element <code>unit*5 + 0</code>"——<b style="color:var(--red)">Silent memory corruption, not a missed optimization.</b></p></div>
  <div class="card pad-lg rule-red step" data-step="6"><div class="card-h">安全底线</div>
    <p class="quote" style="font-size:22px;padding-top:0;padding-bottom:0">Failure must degrade to <em style="font-style:normal;color:var(--red)">no optimization</em>, never to a <em style="font-style:normal;color:var(--red)">wrong address</em>.</p>
    <p class="small" style="color:var(--ink-3);margin-top:10px">EJIT_FREE_DIM §3 · 360 case 矩阵：3 入口 × 5 case × 2 opt × 3 tier × 2 slot × 2 字节序</p></div>
</div>'''
    notes = '''
<p>特化的边界是一寸一寸往外推的。按 → 爬楼梯：从"索引必须是常量"，到指针 + bounds，到 free_dim，到局部变量，到 bound_ptr，一直到 bounds 未知也敢折的 anchored witness。</p>
<p>每一步的代价写在台阶下面：多一条必须成立的前提。到最后一步，前提已经变成了"部署契约"——到所选元素的前缀可读、值均匀。这些是编译器证明不了、只能靠部署保证的东西，所以文档里反复写"successful optimization does not prove the deployment contract"。</p>
<p>witness 那张卡是全场最惊险的地方：算地址时把下标当成 0，但绝不能让这个 0 漏进 IR。漏进去就是 20 个 TTI 的 store 全写进同一个元素——静默的内存踩踏。</p>
'''
    return dict(act=ACT, name='frontier', kicker='第三幕 · 得寸进尺', title='特化的边界，一寸一寸往外推', body=body, steps=6, notes=notes)


def s24_pgo():
    W, H = 1408, 470
    o = []
    # center: memory
    o.append(RECT(554, 150, 300, 150, 'box-ice', 16))
    o.append(T(704, 190, '内存里的 profile', 'zh t-ice b700', anchor='middle', size=16))
    o.append(T(704, 216, '__profc / __profd → addRecord', 't-2', anchor='middle', size=12.5))
    o.append(T(704, 238, '→ InMemoryFileSystem', 't-2', anchor='middle', size=12.5))
    o.append(T(704, 260, '/ejit.prof', 't-ink b700', anchor='middle', size=13))
    # nodes
    o.append(box(0, 24, 330, 84, 'Tier-0 · AOT', ['特化之前的 fallback'], 'box'))
    o.append(box(0, 346, 330, 84, 'Tier-1 · 特化 + 计数', ['轻优化 · PGOInstrumentationGen', '边计数器落 RW 段'], 'box-violet', tsize=15))
    o.append(box(1078, 346, 330, 84, 'Tier-2 · PGOUse', ['!prof → 分支概率', 'MachineBlockPlacement 热路径布局'], 'box-ice', tsize=15))
    o.append(box(1078, 24, 330, 84, '发布', ['批量发布 · 回填 cell', '之后跑的是 Tier-2'], 'box'))
    # paths
    o.append(arrow('M165 108 V340', 'ink', pid='p1'))
    o.append(T(178, 230, 'ejit_init_pgo()', 'zh t-3', size=13))
    o.append(T(178, 250, 'opt-in，默认关', 'zh t-3', size=13))
    o.append(arrow('M334 388 H548', 'violet', pid='p2'))
    o.append(T(441, 376, '64 次命中', 't-violet b700', anchor='middle', size=13))
    o.append(T(441, 412, 'CAS 封顶的采样', 'zh t-3', anchor='middle', size=12))
    o.append(arrow('M860 388 H1072', 'ice', pid='p3'))
    o.append(T(966, 376, '重编译', 'zh t-3', anchor='middle', size=13))
    o.append(arrow('M1243 342 V114', 'ink', pid='p4'))
    o.append(T(1256, 230, 'admission：并发 profile 数', 'zh t-3', size=13))
    o.append(T(1256, 250, '默认 1（范围 1–16）', 'zh t-3', size=13))
    o.append(pkt('p1', 1, 1400, 0, 0) + pkt('p2', 2, 1100, 300, 0) + pkt('p3', 3, 1100, 300, 0) + pkt('p4', 4, 1100, 300, 0))
    body = svg(''.join(o), W, H) + '''
<div class="grid g3 step" data-step="4" style="margin-top:6px">
  <div class="kpi c-violet" style="padding:14px 20px"><div class="l">触发阈值</div><div class="v" style="font-size:40px">64</div><div class="n">次缓存命中后入队 Tier-2</div></div>
  <div class="kpi c-ice" style="padding:14px 20px"><div class="l">自研代码</div><div class="v" style="font-size:40px">≈80<small>行</small></div><div class="n">内存合成 indexed profile，零文件 I/O</div></div>
  <div class="kpi c-amber" style="padding:14px 20px"><div class="l">运行时增量</div><div class="v" style="font-size:40px">+640<small>KB</small></div><div class="n">约占 12 MB 运行时的 5%（独立 probe 实测）</div></div>
</div>'''
    notes = '''
<p>没有文件系统，怎么做 PGO？答案是 profile 根本不落盘。按 → 走一圈：Tier-1 是带边计数器的特化版；命中 64 次之后入队；worker 直接读计数器，在内存里合成一份 indexed profile，挂到一个内存文件系统的 <code>/ejit.prof</code> 上，LLVM 的 PGOInstrumentationUse 以为自己在读文件。</p>
<p>整个内存合成部分自研代码只有 80 行左右。这是"借用 LLVM 二十年积累"的又一个例子。</p>
<p>注意这是 opt-in：普通 <code>ejit_init</code> 不走 PGO。至于值不值，下一幕给结论。</p>
'''
    return dict(act=ACT, name='pgo', kicker='第三幕 · 没有 PGO 的零件', title='没有文件系统的 PGO：profile 只活在内存里', body=body, steps=4, live='flows', notes=notes)


def s25_deopt():
    zh = lambda s: ' zh' if any('一' <= c <= '鿿' for c in s) else ''
    W2, H2 = 656, 168
    b = [T(0, 14, '双缓冲：generation 最低位选半区', 'zh t-3 b500', size=13.5)]
    for k, (lab, active) in enumerate((('gen 0 · 写 A', 0), ('flip → drain', None), ('gen 1 · 写 B', 1))):
        x = k * 234
        for half in (0, 1):
            on = active == half
            b.append(RECT(x + half * 94, 32, 90, 74, 'box-violet' if on else 'box', 8))
            b.append(T(x + half * 94 + 45, 77, 'AB'[half], 't-violet b700' if on else 't-3 b700', anchor='middle', size=19))
        b.append(T(x + 94, 132, lab, 'zh ' + ('t-3' if active is None else 't-2 b500'), anchor='middle', size=13.5))
        if k < 2:
            b.append(arrow(f'M{x + 198} 69 H{x + 226}', 'ink'))
    b.append(T(0, 160, 'writers == 0 才复制并清零退役半区；采集器没有 CAS、没有锁。', 'zh t-3', size=13))
    buf = svg(''.join(b), W2, H2)
    left = f'''
<div class="card pad-lg rule-violet" style="margin-bottom:16px"><div class="card-h">Value Profiling：三类 site</div>
<ul class="ul">
  <li class="li tight">间接调用的目标</li>
  <li class="li tight">动态访存的大小（memop size）</li>
  <li class="li tight">循环边界（自研；官方 value kind 没有这个，走 side table）</li>
</ul>
<p class="small" style="color:var(--ink-2);margin-top:8px">每核一个 K-way heavy-hitter 表（K = 2 · 64 sites / 核 · 32 核 ≈ 200 KB）。<b>无 CAS、无锁、无跨核写共享行。</b></p></div>
<div style="margin:16px 0 18px">{buf}</div>
<div class="grid g3" style="gap:14px">
  <div class="kpi c-violet" style="padding:22px 18px 20px"><div class="l">样本下限</div><div class="v" style="font-size:48px">≥100</div></div>
  <div class="kpi c-violet" style="padding:22px 18px 20px"><div class="l">置信下限</div><div class="v" style="font-size:48px">≥99%</div></div>
  <div class="kpi c-amber" style="padding:22px 18px 20px"><div class="l">默认</div><div class="v" style="font-size:48px">OFF</div></div>
</div>'''
    right = code('''
; Tier-2：带 guard 的推测（示意）
if (bound == 64) {          ; guard：icmp eq Bound, V
  for (i = 0; i < 64; i++)  ; 热克隆：边界成了常量
    hot_body(i);            ;   → 可展开 / 可向量化
} else {
  generic_loop(bound);      ; 冷回退：原样保留
}
''', 'c', 'lg', hl={2: 'ice', 3: 'lime', 6: 'amber'}, maxw=700)
    # what "no deopt" means: wrong guess never leaves compiled code
    W3, H3 = 716, 156
    c = []
    lanes = [('V8 / HotSpot', '推测编译的热代码', '猜错了', 'deopt：退回解释器', '恢复解释器状态，再跑一遍', 'box', 'red', 'box-red'),
             ('EJIT · 裸核', '带 guard 的热克隆', 'guard 不匹配', '语义一致的冷路径', '原样保留的 generic_loop(bound)', 'box-ice', 'amber', 'box-amber')]
    for r, (lab, a, mid, bt, bs, acl, col, bcl) in enumerate(lanes):
        y = r * 86
        c.append(T(0, y + 34, lab, 't-3 b700' + zh(lab), size=13.5))
        c.append(RECT(130, y, 210, 60, acl, 10) + T(235, y + 36, a, 'zh t-ink b700', anchor='middle', size=14.5))
        c.append(arrow(f'M344 {y + 30} H438', col, dash=True) + T(391, y + 19, mid, 'zh t-' + col + ' b500', anchor='middle', size=12))
        c.append(RECT(448, y, 268, 60, bcl, 10))
        c.append(T(582, y + 26, bt, 'zh t-ink b700', anchor='middle', size=14.5))
        c.append(T(582, y + 47, bs, 'zh t-3', anchor='middle', size=12.5))
    cmp_svg = svg(''.join(c), W3, H3, extra='style="margin-top:20px"')
    body = f'''<div class="grid g-48-52" style="gap:28px"><div data-anim="left">{left}</div>
<div data-anim="right">{right}
<div class="step" data-step="1"><div class="card pad-lg rule-red" style="margin-top:18px"><div class="card-h">为什么必须带 guard</div>
<p class="small" style="color:var(--ink-2)">"EJIT <b>无 deoptimization</b>，绝不无条件推测。"（EJIT_VALUE_PROFILE §7.2）V8 猜错了可以去优化回到解释器；我们猜错了只有一条路——<b style="color:var(--red)">执行错的代码</b>。所以每次推测都是"带护栏的热路径 + 语义一致的冷回退"，而且默认关闭。</p></div>{cmp_svg}</div></div></div>'''
    notes = '''
<p>知道了值，还想知道频度。Value Profiling 记录三类"重复出现的值"：间接调用跳到哪、memcpy 多大、循环跑几圈。记录器是每核一份的 heavy-hitter 表，没有锁，也没有跨核共享行。</p>
<p>按 → 看右边。推测的模板永远是这个样子：先比较、再走热克隆、错了走原样的冷回退。V8 可以赌，赌错了 deopt；我们没有这条路，所以 guard 不可省略。</p>
<p>下面那张对照图把差别画出来了：V8 猜错可以退回解释器；我们猜错只能落在一段语义一样的冷代码上，永远不离开编译好的代码。</p>
<p>诚实说明：这个功能默认关着（编译开关），板端 demo 的结果文档里没有记录。我们不伪造收益。</p>
'''
    return dict(act=ACT, name='deopt', kicker='第三幕 · 没有退路的推测', title='没有 deopt 的世界：每一次推测都必须一次对', body=body, steps=1, notes=notes)


def slides():
    return [s18_opener(), s19_trim(), s20_pool(), s21_threads(), s22_cross_core(), s23_frontier(), s24_pgo(), s25_deopt()]
