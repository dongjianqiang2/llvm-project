#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 EJIT 异步编译 (Taskpool) 单页 PPT —— 华为汇报风格(白底+品牌红点缀+灰阶)。
右侧整体方案为原生可编辑流程图: 节点=圆角矩形(文字内嵌), 连线=带箭头 connector。
"""
from datetime import datetime
from pathlib import Path
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE, MSO_CONNECTOR
from pptx.oxml.ns import qn

# ───────────────── 华为风格配色: 白底 · 品牌红点缀 · 灰阶正文 ─────────────────
RED       = RGBColor(0xC7, 0x00, 0x0B)   # 华为品牌红(仅细条/边框点缀)
DARK      = RGBColor(0x26, 0x26, 0x26)   # 近黑 标题
GRAY      = RGBColor(0x59, 0x59, 0x59)   # 中灰 正文
GRAY_L    = RGBColor(0x8C, 0x8C, 0x8C)   # 浅灰 副文本
BG_GRAY   = RGBColor(0xF2, 0xF3, 0xF5)   # 浅灰底
LANE_BG   = RGBColor(0xF7, 0xF8, 0xFA)   # 泳道底
BORDER    = RGBColor(0xD9, 0xD9, 0xD9)   # 边框灰
NODE_LINE = RGBColor(0x75, 0x75, 0x75)   # 节点边框
ARROW     = RGBColor(0x59, 0x59, 0x59)   # 连线
WHITE     = RGBColor(0xFF, 0xFF, 0xFF)
GREEN     = RGBColor(0x3A, 0x7D, 0x5B)   # 柔和绿 ✓
ORANGE    = RGBColor(0xC0, 0x56, 0x21)   # 柔和橙 △

prs = Presentation()
prs.slide_width  = Inches(13.333)
prs.slide_height = Inches(7.5)
SW, SH = prs.slide_width, prs.slide_height
BLANK = prs.slide_layouts[6]
FONT = "微软雅黑"

IN = Inches


def slide(): return prs.slides.add_slide(BLANK)

def rect(s, x, y, w, h, color, line=None, line_w=0.5):
    sp = s.shapes.add_shape(MSO_SHAPE.RECTANGLE, IN(x), IN(y), IN(w), IN(h))
    sp.fill.solid(); sp.fill.fore_color.rgb = color
    if line is not None:
        sp.line.color.rgb = line; sp.line.width = Pt(line_w)
    else:
        sp.line.fill.background()
    sp.shadow.inherit = False
    return sp

def txt(s, x, y, w, h, runs, align=PP_ALIGN.LEFT, anchor=MSO_ANCHOR.TOP):
    tb = s.shapes.add_textbox(IN(x), IN(y), IN(w), IN(h))
    tf = tb.text_frame; tf.word_wrap = True; tf.vertical_anchor = anchor
    for i, para in enumerate(runs):
        p = tf.paragraphs[0] if i == 0 else tf.add_paragraph()
        p.alignment = align
        for (t, sz, col, b, f) in para:
            r = p.add_run(); r.text = t
            r.font.size = Pt(sz); r.font.color.rgb = col
            r.font.bold = b; r.font.name = f
    return tb

def card(s, x, y, w, h, bar=RED):
    rect(s, x, y, w, h, WHITE, line=BORDER, line_w=0.75)
    rect(s, x, y, 0.05, h, bar)

def bullets(s, x, y, w, items, size, gap):
    """items: (lead, body) 或 (lead, body, lead_color)"""
    yy = y
    for it in items:
        lead, body = it[0], it[1]
        lc = it[2] if len(it) > 2 else DARK
        txt(s, x, yy, w, gap,
            [[(lead, size, lc, True, FONT),
              (body, size, GRAY, False, FONT)]])
        yy += gap

def node(s, x, y, w, h, title, details=(), accent=False, blank=False,
         shape=MSO_SHAPE.ROUNDED_RECTANGLE, title_sz=8.5, det_sz=7.2,
         title_color=None, det_color=None):
    """流程图节点: 文字内嵌于 shape (PowerPoint 中可直接编辑)。
    accent=红框强调; blank=虚线边框留白(待填)。"""
    sp = s.shapes.add_shape(shape, IN(x), IN(y), IN(w), IN(h))
    try: sp.adjustments[0] = 0.10
    except Exception: pass
    sp.fill.solid(); sp.fill.fore_color.rgb = WHITE
    sp.line.color.rgb = RED if accent else NODE_LINE
    sp.line.width = Pt(1.25 if accent else 1.0)
    if blank:
        ln = sp.line._get_or_add_ln()
        ln.append(ln.makeelement(qn('a:prstDash'), {'val': 'dash'}))
    sp.shadow.inherit = False
    tf = sp.text_frame; tf.word_wrap = True
    tf.vertical_anchor = MSO_ANCHOR.MIDDLE
    tf.margin_left = tf.margin_right = IN(0.03)
    tf.margin_top = tf.margin_bottom = IN(0.02)
    p = tf.paragraphs[0]; p.alignment = PP_ALIGN.CENTER
    r = p.add_run(); r.text = title
    r.font.size = Pt(title_sz); r.font.bold = True
    r.font.color.rgb = title_color or DARK; r.font.name = FONT
    for d in details:
        p2 = tf.add_paragraph(); p2.alignment = PP_ALIGN.CENTER
        r2 = p2.add_run(); r2.text = d
        r2.font.size = Pt(det_sz); r2.font.bold = False
        r2.font.color.rgb = det_color or (GRAY_L if blank else GRAY); r2.font.name = FONT
    return sp

def connect(s, x1, y1, x2, y2, color=ARROW, width=1.25, dashed=False):
    """原生 connector + 箭头 (triangle tailEnd), 可在 PowerPoint 中编辑。"""
    conn = s.shapes.add_connector(MSO_CONNECTOR.STRAIGHT,
                                  IN(x1), IN(y1), IN(x2), IN(y2))
    conn.line.color.rgb = color; conn.line.width = Pt(width)
    try: conn.shadow.inherit = False
    except Exception: pass
    ln = conn.line._get_or_add_ln()
    if dashed:
        ln.append(ln.makeelement(qn('a:prstDash'), {'val': 'dash'}))
    ln.append(ln.makeelement(qn('a:tailEnd'),
                             {'type': 'triangle', 'w': 'med', 'len': 'med'}))
    return conn

def label(s, x, y, t, w=0.6, align=PP_ALIGN.LEFT, color=GRAY, sz=7.5, bold=True):
    txt(s, x, y, w, 0.2, [[(t, sz, color, bold, FONT)]], align=align)

def lane(s, x, y, w, h, title):
    """泳道: 浅灰底 + 红色小方块 + 加粗标题。"""
    rect(s, x, y, w, h, LANE_BG, line=BORDER, line_w=0.75)
    rect(s, x + 0.10, y + 0.075, 0.05, 0.15, RED)
    txt(s, x + 0.22, y + 0.03, w - 0.3, 0.2,
        [[(title, 9.5, DARK, True, FONT)]])


# ════════════════════════════════════════════════════════════════
# 单页: EJIT 异步编译
# ════════════════════════════════════════════════════════════════
def page():
    s = slide()

    # ── 标题区: 白底 + 顶部红条 + 黑标题 + 短红下划线 ──
    rect(s, 0, 0, 13.333, 0.055, RED)
    txt(s, 0.6, 0.14, 12.0, 0.42,
        [[("EJIT 异步编译 · Taskpool 调度", 22, DARK, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)
    rect(s, 0.62, 0.60, 0.55, 0.028, RED)
    txt(s, 0.6, 0.66, 12.4, 0.24,
        [[("纯异步 · 单 Worker · 无锁并发 —— SRE 裸核 (aarch64_be) 无 C++ 线程库场景 · 2026.06 – 2026.11",
           10.5, GRAY, False, FONT)]])
    rect(s, 0, 0.94, 13.333, 0.012, BORDER)

    # ══════════ 左栏: 问题背景 / 关键技术 / 进展 ══════════
    lx = 0.35; lw = 5.35

    y = 1.06; h = 1.46
    card(s, lx, y, lw, h)
    txt(s, lx + 0.18, y + 0.04, lw, 0.28,
        [[("问题背景", 12, DARK, True, FONT)]])
    bullets(s, lx + 0.18, y + 0.36, lw - 0.32, [
        ("同步阻塞  ", "miss 时调用方全程等待 bitcode→特化→OrcJIT 编译, 空闲核无法参与"),
        ("无去重  ", "多调用方并发请求同一函数, 各自重复编译, 浪费 CPU"),
        ("裸核约束  ", "SRE aarch64_be 无 C++ 线程库 (thread/mutex/atomic), 仅原子变量+栅栏+task 原语"),
    ], size=8.8, gap=0.35)

    y = 2.64; h = 2.58
    card(s, lx, y, lw, h)
    txt(s, lx + 0.18, y + 0.04, lw, 0.28,
        [[("关键技术", 12, DARK, True, FONT)]])
    bullets(s, lx + 0.18, y + 0.36, lw - 0.32, [
        ("纯异步模型  ", "producer 入队即返回 fallback; 单 worker 跑在 EJitSreTask 平台 task (host/SRE 链接择一)"),
        ("无锁 MPSC 队列  ", "Vyukov 环形队列 1024 槽, 多 producer CAS 抢槽; 单 consumer 是无锁正确性硬约束"),
        ("Dedup 去重  ", "funcIndex 直接数组下标, CAS 占位 O(1); 冲突/队满 → 自动回滚 + fallback"),
        ("32 桶分桶缓存  ", "每桶独立 RwLock: 读 tryRead 不等待 / 写 spin 等读者退场; rehash 只影响单桶"),
        ("读计数外提  ", "lookup 返回 {fnPtr, bucket}, 调用方用完 release_read → 旧代码安全释放"),
        ("Version 懒失效  ", "toggle→version++ 单调递增: cache miss / 队列丢弃 / 不 publish; worker 双检查点挡竞态"),
    ], size=8.4, gap=0.368)

    y = 5.34; h = 2.0
    card(s, lx, y, lw, h)
    txt(s, lx + 0.18, y + 0.04, lw, 0.28,
        [[("进展", 12, DARK, True, FONT)]])
    bullets(s, lx + 0.18, y + 0.33, lw - 0.32, [
        ("✓  ", "单 worker 纯异步调度 + C ABI (compile_or_get / release_read / stats)", GREEN),
        ("✓  ", "跨核共享单 worker: POD 共享 blob + CAS owner 选举, 生产 preset 钉核 6", GREEN),
        ("✓  ", "4K 页粒度跨核 seal (v5 ABI): slot 携带代码范围 + executableCoreMask 记忆", GREEN),
        ("✓  ", "host 测试 EJITTaskPool / SharedTaskPool Tests: 注入式核 ID, 无真实线程", GREEN),
        ("△  ", "待真机: 跨核 I/D-cache 一致性 · SRE_TaskDelete 真 join · worker 栈峰值实测", ORANGE),
    ], size=8.4, gap=0.33)

    # ══════════ 右栏: 整体方案 (可编辑流程图, 顺时针环形数据流) ══════════
    rx = 5.95; rw = 7.05

    # 分区标题条
    rect(s, rx, 1.00, rw, 0.36, BG_GRAY, line=BORDER, line_w=0.5)
    rect(s, rx, 1.00, 0.05, 0.36, RED)
    txt(s, rx + 0.15, 1.00, rw - 0.3, 0.36,
        [[("整体方案 · 纯异步单 Worker 调度 (producer 入队即返回 → worker 编译 → cache 跨核共享)",
           10.5, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)

    # ── 泳道 1: 业务核 ──
    lane(s, rx, 1.44, rw, 1.98, "业务核 Core 0..N · Producer (AOT wrapper 调用)")
    # 第一行: 正常路径
    node(s, rx + 0.12, 1.78, 1.24, 0.64, "① 调用入口",
         ("compile_or_get(funcIdx, dims)",))
    node(s, rx + 1.58, 1.78, 1.30, 0.64, "② 实例开关检查",
         ("禁用 → 直接 fallback",))
    node(s, rx + 3.10, 1.78, 1.52, 0.64, "③ 缓存查询 tryRead",
         ("不等待 · 失败即 miss",))
    node(s, rx + 4.88, 1.78, 1.90, 0.64, "④ 命中 → 执行特化代码",
         ("fnPtr(args) + release_read",), accent=True)
    connect(s, rx + 1.38, 2.10, rx + 1.56, 2.10)
    connect(s, rx + 2.92, 2.10, rx + 3.08, 2.10)
    connect(s, rx + 4.66, 2.10, rx + 4.86, 2.10)
    # 第二行: miss 路径
    node(s, rx + 4.20, 2.70, 1.70, 0.56, "miss → tryEnqueue 去重入队",
         ("dedup CAS + queue push", "队满/冲突回滚 · 立即返回 fallback"))
    connect(s, rx + 4.42, 2.42, rx + 4.72, 2.70)
    label(s, rx + 3.94, 2.47, "miss", w=0.42)

    # ── 泳道 2: 跨核共享状态 ──
    lane(s, rx, 3.64, rw, 1.36,
         "跨核共享状态 · EJIT_SHARED_SECTION (全 POD + EJitAtomic · 同 VA 映射)")
    node(s, rx + 0.30, 3.98, 2.20, 0.84, "结果缓存 · 32 桶",
         ("每桶独立 RwLock (tryRead / spin 写)",
          "version 快照逐实例比对 · 固定槽 POD"))
    node(s, rx + 2.70, 3.98, 2.00, 0.84, "SwitchController",
         ("enabled + version 单调递增",
          "三层懒失效 · Off / Async 模式"))
    node(s, rx + 4.90, 3.98, 2.02, 0.84, "任务队列 TaskQueue",
         ("Vyukov MPSC 无锁环形队列 (1024 槽)",
          "Dedup: CAS 占位去重 · 满即回滚"))
    connect(s, rx + 5.30, 3.26, rx + 5.30, 3.98)          # 入队
    label(s, rx + 5.38, 3.42, "入队", w=0.5)
    connect(s, rx + 2.35, 3.98, rx + 3.30, 2.42, dashed=True)  # 命中读(上行)
    label(s, rx + 2.70, 3.32, "命中读", w=0.6)

    # ── 泳道 3: Worker 核 ──
    lane(s, rx, 5.22, rw, 1.42, "Worker 核")
    node(s, rx + 5.44, 5.56, 1.40, 0.70, "④ pollOne 出队",
         ("空闲 yield 让出",))
    node(s, rx + 3.86, 5.56, 1.28, 0.70, "⑤ 检查点 1",
         ("version 失配即丢弃",))
    node(s, rx + 2.10, 5.56, 1.52, 0.70, "⑥ JIT 编译",
         ("bitcode → 参数特化 → OrcJIT",), accent=True)
    node(s, rx + 0.28, 5.56, 1.60, 0.70, "⑦ 检查点2 → publish",
         ("spin 等读者=0 · release",))
    connect(s, rx + 5.42, 5.91, rx + 5.18, 5.91)
    connect(s, rx + 3.84, 5.91, rx + 3.64, 5.91)
    connect(s, rx + 2.08, 5.91, rx + 1.90, 5.91)
    connect(s, rx + 5.90, 4.82, rx + 5.90, 5.56)          # 出队(下行)
    label(s, rx + 5.98, 5.00, "出队", w=0.5)
    connect(s, rx + 1.15, 5.56, rx + 1.15, 4.82)          # publish(上行)
    label(s, rx + 1.23, 5.02, "publish", w=0.72)
    connect(s, rx + 4.40, 4.82, rx + 4.40, 5.56, dashed=True)  # version(下行)
    label(s, rx + 3.30, 5.02, "version 校验", w=1.02, align=PP_ALIGN.RIGHT)
    txt(s, rx + 0.28, 6.34, rw - 0.5, 0.22,
        [[("EJitSreTask 平台 task (生产 preset 钉核 6) · EJitWorker 循环: pollOne → 空闲 yield · 软停止 + join 退出",
           7.5, GRAY_L, False, FONT)]])

    # ── 底部: 平台抽象层脚注 ──
    rect(s, rx, 6.74, rw, 0.56, BG_GRAY, line=BORDER, line_w=0.5)
    rect(s, rx, 6.74, 0.05, 0.56, RED)
    txt(s, rx + 0.16, 6.78, rw - 0.3, 0.5,
        [[("平台抽象层: EJitAtomic (__atomic_* 内建, 免 <atomic>) · EJitSreTask (host: std::thread / SRE: 平台 task 原语)",
           8, GRAY, False, FONT)],
         [("跨核执行权限: enable_ex + split_2m_to_4k 4K 页粒度 seal · executableCoreMask 记忆 (v5 ABI)",
           8, GRAY, False, FONT)]])


# ════════════════════════════════════════════════════════════════
# 第二页: 多小区 iCache Miss 消减方案 (6187/6188/6189 留白)
# ════════════════════════════════════════════════════════════════
def page2():
    s = slide()

    # ── 标题区 ──
    rect(s, 0, 0, 13.333, 0.055, RED)
    txt(s, 0.6, 0.14, 12.0, 0.42,
        [[("多小区 iCache Miss 消减方案", 22, DARK, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)
    rect(s, 0.62, 0.60, 0.55, 0.028, RED)
    txt(s, 0.6, 0.66, 12.4, 0.24,
        [[("多小区特化版本并存 × I-cache 容量/冲突 —— 6187 / 6188 / 6189 cache 与核间共享情况留白待填 · 2026.11",
           10.5, GRAY, False, FONT)]])
    rect(s, 0, 0.94, 13.333, 0.012, BORDER)

    # ══════════ 左栏: 问题背景 / 关键技术 / 进展 ══════════
    lx = 0.35; lw = 5.35

    y = 1.06; h = 1.46
    card(s, lx, y, lw, h)
    txt(s, lx + 0.18, y + 0.04, lw, 0.28,
        [[("问题背景", 12, DARK, True, FONT)]])
    bullets(s, lx + 0.18, y + 0.36, lw - 0.32, [
        ("多小区并存  ", "每小区一个生命周期实例, 同一函数按小区特化出多版本, 版本数随活跃小区数增长"),
        ("容量非瓶颈  ", "单热点路径 2–5 KB << L1 I 32–64 KB; 风险在多版本叠加的 set 冲突与换入换出"),
        ("布局半成品  ", "dispatcher 聚类与 cold 隔离已做, 多小区版本在代码池内的排布未按 cache 友好组织"),
    ], size=8.8, gap=0.35)

    y = 2.64; h = 2.58
    card(s, lx, y, lw, h)
    txt(s, lx + 0.18, y + 0.04, lw, 0.28,
        [[("关键技术 (已具备的消减基础)", 12, DARK, True, FONT)]])
    bullets(s, lx + 0.18, y + 0.36, lw - 0.32, [
        ("多版本 Inline Cache  ", "(funcIndex × dims) 直接索引, 每身份一槽, 多小区各自直跳特化版本"),
        ("跨核共享 cell 表  ", "表入共享段, toggle 置零即跨核失效; 各核命中同一份, 无每核回收会合"),
        ("Dispatcher 聚类  ", "独立 .text.ejit_dispatch 段 64B 对齐, 单 line 容 1–4 个, 空间局部性命中 (PR #124)"),
        ("Cold 隔离  ", "miss 函数体 → .text.unlikely, 减少热路径 cache 污染 (PR #124)"),
        ("PMU 测量优先  ", "L1I_CACHE_REFILL / L1I_CACHE 定基线; <1% 健康, >5% 才需投入 (待上板)"),
        ("占用观测  ", "slot dump 整表 icache 占用报告, 支撑多小区版本体量评估"),
    ], size=8.4, gap=0.368)

    y = 5.34; h = 2.0
    card(s, lx, y, lw, h)
    txt(s, lx + 0.18, y + 0.04, lw, 0.28,
        [[("进展 / 待填", 12, DARK, True, FONT)]])
    bullets(s, lx + 0.18, y + 0.33, lw - 0.32, [
        ("✓  ", "dispatcher 聚类 + cold miss 隔离已合入 (PR #124), 待上板验证", GREEN),
        ("✓  ", "多版本 Inline Cache + 跨核共享 cell 表已实现", GREEN),
        ("✓  ", "slot dump 整表 icache 占用报告已合入", GREEN),
        ("□  ", "6187 / 6188 / 6189 cache 参数 (容量/相联度/映射) —— 待填", GRAY_L),
        ("□  ", "是否核间共享及对应失效/布局策略 —— 待确认", GRAY_L),
    ], size=8.4, gap=0.33)

    # ══════════ 右栏: 整体方案 (消减框架, cache 部分留白) ══════════
    rx = 5.95; rw = 7.05

    rect(s, rx, 1.00, rw, 0.36, BG_GRAY, line=BORDER, line_w=0.5)
    rect(s, rx, 1.00, 0.05, 0.36, RED)
    txt(s, rx + 0.15, 1.00, rw - 0.3, 0.36,
        [[("整体方案 · 消减框架 (6187 / 6188 / 6189 cache 与核间共享留白待填)",
           10.5, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)

    # ── 泳道 1: 多小区特化版本来源 ──
    lane(s, rx, 1.44, rw, 1.30, "多小区特化版本来源 (业务核 → taskpool → 代码池)")
    node(s, rx + 0.12, 1.78, 1.50, 0.62, "小区 0..N 并发调用",
         ("每小区独立生命周期",))
    node(s, rx + 1.82, 1.78, 1.55, 0.62, "IC 探测 per-identity",
         ("命中直跳 · miss 入队",))
    node(s, rx + 3.57, 1.78, 1.50, 0.62, "taskpool 编译多版本",
         ("worker 核特化",))
    node(s, rx + 5.27, 1.78, 1.60, 0.62, ".text.ejit 多版本驻留",
         ("代码池 near-hot 64B 对齐",))
    connect(s, rx + 1.62, 2.09, rx + 1.80, 2.09)
    connect(s, rx + 3.37, 2.09, rx + 3.55, 2.09)
    connect(s, rx + 5.07, 2.09, rx + 5.25, 2.09)
    connect(s, rx + 6.05, 2.40, rx + 6.05, 2.92, dashed=True)   # 驻留 → I-cache
    label(s, rx + 4.40, 2.56, "多版本驻留进入 I-cache", w=1.60, align=PP_ALIGN.RIGHT)

    # ── 泳道 2: I-cache 现状 (6187/6188/6189 留白) ──
    lane(s, rx, 2.92, rw, 2.16, "I-cache 现状 · 6187 / 6188 / 6189 (参数待填)")
    blank_fields = ("容量: ＿＿＿ 相联度: ＿",
                    "行大小: ＿ 服务核: ＿＿",
                    "多小区版本占用: ＿＿")
    node(s, rx + 0.12, 3.24, 2.20, 1.10, "6187 cache",
         blank_fields, blank=True, title_sz=9.5)
    node(s, rx + 2.44, 3.24, 2.20, 1.10, "6188 cache",
         blank_fields, blank=True, title_sz=9.5)
    node(s, rx + 4.76, 3.24, 2.16, 1.10, "6189 cache",
         blank_fields, blank=True, title_sz=9.5)
    # 是否核间共享: 决策菱形 + 两条留白分支
    node(s, rx + 0.30, 4.40, 1.45, 0.66, "核间共享?",
         shape=MSO_SHAPE.DIAMOND, title_sz=8)
    node(s, rx + 2.15, 4.24, 4.75, 0.40, "若核间共享: ＿＿＿＿＿＿＿",
         blank=True, title_sz=7.5, title_color=GRAY)
    node(s, rx + 2.15, 4.80, 4.75, 0.40, "若每核私有: ＿＿＿＿＿＿＿",
         blank=True, title_sz=7.5, title_color=GRAY)
    connect(s, rx + 1.75, 4.58, rx + 2.13, 4.44)
    connect(s, rx + 1.75, 4.88, rx + 2.13, 4.98)

    # ── 泳道 3: 消减手段 (分层) ──
    lane(s, rx, 5.26, rw, 1.42, "消减手段 · 分层推进")
    node(s, rx + 0.12, 5.62, 1.48, 0.66, "Tier 1 链接偏移验证",
         ("非 2^n 偏移规避冲突",))
    node(s, rx + 1.76, 5.62, 1.58, 0.66, "Tier 2 聚类 + 隔离 ✓",
         ("dispatcher 段 / .text.unlikely",))
    node(s, rx + 3.50, 5.62, 1.52, 0.66, "Tier 3 高阶手段",
         ("PGO 排布 · cache 着色",))
    node(s, rx + 5.18, 5.62, 1.74, 0.66, "多小区布局策略",
         ("＿＿＿＿＿ 待基线后定",), blank=True)
    connect(s, rx + 1.60, 5.95, rx + 1.74, 5.95)
    connect(s, rx + 3.34, 5.95, rx + 3.48, 5.95)
    connect(s, rx + 5.02, 5.95, rx + 5.16, 5.95)
    connect(s, rx + 6.05, 5.20, rx + 6.05, 5.60, dashed=True)   # 共享情况/基线 → 选型
    label(s, rx + 4.30, 5.26, "基线与共享情况 → 选型", w=1.70, align=PP_ALIGN.RIGHT)
    txt(s, rx + 0.28, 6.36, rw - 0.5, 0.22,
        [[("Tier 1/2 为通用手段 · Tier 3 与多小区布局策略需 6187 / 6188 / 6189 基线数据支撑",
           7.5, GRAY_L, False, FONT)]])

    # ── 底部脚注 ──
    rect(s, rx, 6.78, rw, 0.52, BG_GRAY, line=BORDER, line_w=0.5)
    rect(s, rx, 6.78, 0.05, 0.52, RED)
    txt(s, rx + 0.16, 6.82, rw - 0.3, 0.46,
        [[("测量优先: PMU L1I_CACHE_REFILL / L1I_CACHE 建基线 —— miss 率 <1% 健康 / >5% 需投入", 8, GRAY, False, FONT)],
         [("待补: 6187 / 6188 / 6189 cache 实测 miss 基线 · 核间共享确认 · 多小区版本布局选型", 8, GRAY, False, FONT)]])


page()
page2()

_today = datetime.now().strftime("%Y%m%d")
out = Path(__file__).resolve().parent / f"EJIT异步编译_{_today}.pptx"
prs.save(out)
print("已生成:", out)
