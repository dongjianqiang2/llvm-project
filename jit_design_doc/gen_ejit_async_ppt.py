#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 EJIT 异步编译 (Taskpool) 单页 PPT。风格与 gen_ejit_progress_ppt.py 一致。"""
from datetime import datetime
from pathlib import Path
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

# ───────────────── 通用汇报风格配色(蓝灰系) ─────────────────
PRIMARY      = RGBColor(0x2C, 0x52, 0x82)
ACCENT       = RGBColor(0x31, 0x82, 0xCE)
DARK         = RGBColor(0x2D, 0x37, 0x48)
SLATE        = RGBColor(0x4A, 0x55, 0x68)
GRAY         = RGBColor(0x59, 0x59, 0x59)
GRAY_L       = RGBColor(0x8C, 0x8C, 0x8C)
BG_GRAY      = RGBColor(0xF5, 0xF7, 0xFA)
BORDER       = RGBColor(0xD9, 0xDE, 0xE6)
PRIM_L       = RGBColor(0xEB, 0xF2, 0xFA)
WHITE        = RGBColor(0xFF, 0xFF, 0xFF)
GREEN        = RGBColor(0x38, 0xA1, 0x6F)
ORANGE       = RGBColor(0xDD, 0x6B, 0x20)

prs = Presentation()
prs.slide_width  = Inches(13.333)
prs.slide_height = Inches(7.5)
SW, SH = prs.slide_width, prs.slide_height
BLANK = prs.slide_layouts[6]
FONT = "微软雅黑"


def slide(): return prs.slides.add_slide(BLANK)

def rect(s, x, y, w, h, color, line=None, line_w=0.5):
    sp = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, x, y, w, h)
    sp.fill.solid(); sp.fill.fore_color.rgb = color
    if line is not None:
        sp.line.color.rgb = line; sp.line.width = Pt(line_w)
    else:
        sp.line.fill.background()
    sp.shadow.inherit = False
    return sp

def txt(s, x, y, w, h, runs, align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP, spacing=None):
    tb = s.shapes.add_textbox(x, y, w, h)
    tf = tb.text_frame; tf.word_wrap = True; tf.vertical_anchor = anchor
    for i, para in enumerate(runs):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        if spacing: p.space_after = Pt(spacing)
        for (t, sz, col, b, f) in para:
            r = p.add_run(); r.text = t
            r.font.size = Pt(sz); r.font.color.rgb = col
            r.font.bold = b; r.font.name = f
    return tb

def card(s, x, y, w, h, bar=PRIMARY):
    rect(s, x, y, w, h, WHITE, line=BORDER, line_w=0.75)
    rect(s, x, y, Inches(0.07), h, bar)

def arrow(s, x, y, w, h, color=ACCENT, shape=MSO_SHAPE.UP_DOWN_ARROW):
    sp = s.shapes.add_shape(shape, x, y, w, h)
    sp.fill.solid(); sp.fill.fore_color.rgb = color
    sp.line.fill.background()
    sp.shadow.inherit = False
    return sp

def header(s, title, subtitle=None):
    rect(s, 0, 0, SW, Inches(0.07), PRIMARY)
    rect(s, 0, Inches(0.07), Inches(0.45), Inches(0.55), PRIMARY)
    txt(s, Inches(0.6), Inches(0.12), Inches(12.0), Inches(0.5),
        [[(title, 23, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
    if subtitle:
        txt(s, Inches(0.6), Inches(0.58), Inches(12.4), Inches(0.26),
            [[(subtitle, 10.5, GRAY, False, FONT)]])
    rect(s, 0, Inches(0.88), SW, Inches(0.015), BORDER)

def bullets(s, x, y, w, items, size, gap):
    """items: (lead, body) 或 (lead, body, lead_color)"""
    yy = y
    for it in items:
        lead, body = it[0], it[1]
        lc = it[2] if len(it) > 2 else DARK
        txt(s, x, yy, w, Inches(gap),
            [[(lead, size, lc, True, FONT),
              (body, size, GRAY, False, FONT)]])
        yy += Inches(gap)

def flowbox(s, x, y, w, h, title, title_bg, lines, title_sz=10, body_sz=8.3, lh=0.26):
    rect(s, x, y, w, h, WHITE, line=BORDER, line_w=0.75)
    rect(s, x, y, w, Inches(0.32), title_bg)
    txt(s, x + Inches(0.1), y, w - Inches(0.2), Inches(0.32),
        [[(title, title_sz, WHITE, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
    yy = y + Inches(0.38)
    for runs in lines:
        txt(s, x + Inches(0.12), yy, w - Inches(0.24), Inches(lh), [runs])
        yy += Inches(lh)


# ════════════════════════════════════════════════════════════════
# 单页: EJIT 异步编译
# ════════════════════════════════════════════════════════════════
def page():
    s = slide()
    header(s, "EJIT 异步编译 · Taskpool 调度",
           "纯异步 · 单 Worker · 无锁并发 —— SRE 裸核 (aarch64_be) 无 C++ 线程库场景 · 2026.06 – 2026.11")

    # ── 左栏: 问题背景 / 关键技术 / 进展 ──
    lx = Inches(0.35); lw = Inches(5.35)

    # 1. 问题背景
    y = Inches(1.0); h = Inches(1.50)
    card(s, lx, y, lw, h)
    txt(s, lx + Inches(0.18), y + Inches(0.04), lw, Inches(0.3),
        [[("▎问题背景", 12.5, DARK, True, FONT)]])
    bullets(s, lx + Inches(0.18), y + Inches(0.36), lw - Inches(0.32), [
        ("同步阻塞  ", "miss 时调用方全程等待 bitcode→特化→OrcJIT 编译, 空闲核无法参与"),
        ("无去重  ", "多调用方并发请求同一函数, 各自重复编译, 浪费 CPU"),
        ("裸核约束  ", "SRE aarch64_be 无 C++ 线程库 (thread/mutex/atomic), 仅原子变量+栅栏+task 原语"),
    ], size=8.8, gap=0.37)

    # 2. 关键技术
    y = Inches(2.62); h = Inches(2.62)
    card(s, lx, y, lw, h)
    txt(s, lx + Inches(0.18), y + Inches(0.04), lw, Inches(0.3),
        [[("▎关键技术", 12.5, DARK, True, FONT)]])
    bullets(s, lx + Inches(0.18), y + Inches(0.36), lw - Inches(0.32), [
        ("纯异步模型  ", "producer 入队即返回 fallback; 单 worker 跑在 EJitSreTask 平台 task (host/SRE 链接择一)"),
        ("无锁 MPSC 队列  ", "Vyukov 环形队列 1024 槽, 多 producer CAS 抢槽; 单 consumer 是无锁正确性硬约束"),
        ("Dedup 去重  ", "funcIndex 直接数组下标, CAS 占位 O(1); 冲突/队满 → 自动回滚 + fallback"),
        ("32 桶分桶缓存  ", "每桶独立 RwLock: 读 tryRead 不等待 / 写 spin 等读者退场; rehash 只影响单桶"),
        ("读计数外提  ", "lookup 返回 {fnPtr, bucket}, 调用方用完 release_read → 旧代码安全释放"),
        ("Version 懒失效  ", "toggle→version++ 单调递增: cache miss / 队列丢弃 / 不 publish; worker 双检查点挡竞态"),
    ], size=8.4, gap=0.375)

    # 3. 进展
    y = Inches(5.36); h = Inches(1.99)
    card(s, lx, y, lw, h)
    txt(s, lx + Inches(0.18), y + Inches(0.03), lw, Inches(0.3),
        [[("▎进展", 12.5, DARK, True, FONT)]])
    bullets(s, lx + Inches(0.18), y + Inches(0.33), lw - Inches(0.32), [
        ("✓  ", "单 worker 纯异步调度 + C ABI (compile_or_get / release_read / stats)", GREEN),
        ("✓  ", "跨核共享单 worker: POD 共享 blob + CAS owner 选举, 生产 preset 钉核 6", GREEN),
        ("✓  ", "4K 页粒度跨核 seal (v5 ABI): slot 携带代码范围 + executableCoreMask 记忆", GREEN),
        ("✓  ", "host 测试 EJITTaskPool / SharedTaskPool Tests: 注入式核 ID, 无真实线程", GREEN),
        ("△  ", "待真机: 跨核 I/D-cache 一致性 · SRE_TaskDelete 真 join · worker 栈峰值实测", ORANGE),
    ], size=8.4, gap=0.33)

    # ── 右栏: 整体方案 ──
    rx = Inches(5.95); rw = Inches(7.05)
    rect(s, rx, Inches(1.0), rw, Inches(0.4), PRIMARY)
    txt(s, rx, Inches(1.0), rw, Inches(0.4),
        [[("整体方案 · 纯异步单 Worker 调度 (producer 入队 → worker 编译 → cache 共享)", 11.5, WHITE, True, FONT)]],
        align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)

    # Box A: 业务核 producer
    ax = rx; aw = Inches(3.30)
    flowbox(s, ax, Inches(1.55), aw, Inches(2.25), "业务核 Core 0..N (producer)", SLATE, [
        [("ejit_taskpool_compile_or_get()", 8.3, PRIMARY, True, FONT)],
        [("① 开关检查: 实例禁用 → fallback", 8.3, GRAY, False, FONT)],
        [("② cache tryRead 命中 → fnPtr(args)", 8.3, GRAY, False, FONT)],
        [("    用完 release_read(bucket)", 8.3, GRAY_L, False, FONT)],
        [("③ miss → tryEnqueue 去重入队", 8.3, GRAY, False, FONT)],
        [("    (dedup CAS + queue push)", 8.3, GRAY_L, False, FONT)],
        [("→ 立即返回 fallback, 不阻塞", 8.3, DARK, True, FONT)],
    ])

    # Box B: worker 核
    bx = rx + Inches(3.55); bw = Inches(3.50)
    flowbox(s, bx, Inches(1.55), bw, Inches(2.25), "Worker 核 · EJitSreTask (生产钉核 6)", PRIMARY, [
        [("EJitWorker 循环: pollOne, 空闲 yield", 8.3, GRAY, False, FONT)],
        [("④ tryDequeue → 检查点1: version 失配即丢弃", 8.3, GRAY, False, FONT)],
        [("⑤ 编译: CompileCallback → bitcode 加载", 8.3, GRAY, False, FONT)],
        [("    → 参数替换 / 特化 → OrcJIT(JITLink)", 8.3, GRAY, False, FONT)],
        [("⑥ 检查点2: 失配丢弃, 不 publish", 8.3, GRAY, False, FONT)],
        [("⑦ cache publish (write spin 等读者=0)", 8.3, GRAY, False, FONT)],
        [("    + release(funcIndex) 释放占位", 8.3, GRAY_L, False, FONT)],
    ])

    # 箭头 + 标注
    ay = Inches(3.84); ah = Inches(0.58)
    arrow(s, ax + aw / 2 - Inches(0.14), ay, Inches(0.28), ah)
    txt(s, ax + aw / 2 + Inches(0.18), ay + Inches(0.1), Inches(1.5), Inches(0.2),
        [[("miss 入队 · 命中读", 8, ACCENT, True, FONT)]])
    arrow(s, bx + bw / 2 - Inches(0.14), ay, Inches(0.28), ah)
    txt(s, bx + bw / 2 - Inches(1.62), ay + Inches(0.1), Inches(1.4), Inches(0.2),
        [[("出队 · publish", 8, ACCENT, True, FONT)]], align=PP_ALIGN.RIGHT)

    # Box C: 跨核共享状态
    cy = Inches(4.50); ch = Inches(1.80)
    rect(s, rx, cy, rw, ch, BG_GRAY, line=BORDER, line_w=0.75)
    rect(s, rx, cy, rw, Inches(0.30), DARK)
    txt(s, rx + Inches(0.1), cy, rw - Inches(0.2), Inches(0.30),
        [[("跨核共享状态  (EJIT_SHARED_SECTION · 全 POD + EJitAtomic, 同 VA 映射)", 9.5, WHITE, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)
    sub_w = (rw - Inches(0.4)) / 3
    subs = [
        ("TaskQueue", [
            ("Vyukov MPSC 无锁环形队列", GRAY),
            ("Dedup: CAS 占位去重", GRAY),
            ("队满/冲突 → 回滚 fallback", GRAY_L),
        ]),
        ("Cache · 32 桶", [
            ("每桶独立 RwLock", GRAY),
            ("tryRead 不等待 / 写 spin", GRAY),
            ("version 快照逐实例比对", GRAY),
            ("固定槽 POD (共享构建)", GRAY_L),
        ]),
        ("SwitchController", [
            ("enabled + version 单调递增", GRAY),
            ("三层懒失效: cache / 队列", GRAY),
            ("/ in-flight 编译中", GRAY_L),
            ("Off / Async 全局模式", GRAY),
        ]),
    ]
    for i, (name, lines) in enumerate(subs):
        sx = rx + Inches(0.12) + i * (sub_w + Inches(0.08))
        rect(s, sx, cy + Inches(0.38), sub_w, ch - Inches(0.48), WHITE, line=BORDER, line_w=0.5)
        txt(s, sx + Inches(0.1), cy + Inches(0.42), sub_w - Inches(0.2), Inches(0.24),
            [[(name, 9.5, PRIMARY, True, FONT)]])
        yy = cy + Inches(0.68)
        for t, c in lines:
            txt(s, sx + Inches(0.1), yy, sub_w - Inches(0.2), Inches(0.22),
                [[(t, 7.8, c, False, FONT)]])
            yy += Inches(0.23)

    # Box D: 平台抽象层
    dy = Inches(6.48); dh = Inches(0.87)
    card(s, rx, dy, rw, dh, bar=SLATE)
    txt(s, rx + Inches(0.18), dy + Inches(0.05), rw - Inches(0.3), Inches(0.26),
        [[("SRE 平台抽象层 (头文件抽象 + 链接时择一)", 10, DARK, True, FONT)]])
    txt(s, rx + Inches(0.18), dy + Inches(0.34), rw - Inches(0.3), Inches(0.5),
        [[("EJitAtomic (__atomic_* 内建, 不依赖 <atomic>)  ·  EJitSreTask (host: std::thread / SRE: 平台 task 原语, 软停+join)  ·  ", 8.3, GRAY, False, FONT)],
         [("enable_ex + split_2m_to_4k: 非 owner 核 4K 页粒度执行权限准备 (v5, executableCoreMask 记忆)", 8.3, GRAY, False, FONT)]])


page()

_today = datetime.now().strftime("%Y%m%d")
out = Path(__file__).resolve().parent / f"EJIT异步编译_{_today}.pptx"
prs.save(out)
print("已生成:", out)
