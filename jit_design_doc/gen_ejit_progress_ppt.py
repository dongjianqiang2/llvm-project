#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""生成 EJIT 进展汇报 PPT —— 通用汇报风格(文件名按当前日期动态命名)。

2026-09 更新: 补充 Q3(07~09 月) 深化与加固期进展 —— icache 多版本/共享表、
在线 PGO 双 tier、bound_ptr/free_dim、闭包瘦身、验证器/计时/审计、诊断体系、
后端裁剪增强; 时间轴延长至 09.15, 共 5 页。
"""
from datetime import datetime
from pathlib import Path
import glob
import subprocess
from pptx import Presentation
from pptx.util import Inches, Pt
from pptx.dml.color import RGBColor
from pptx.enum.text import PP_ALIGN, MSO_ANCHOR
from pptx.enum.shapes import MSO_SHAPE

# ───────────────── 通用汇报风格配色(蓝灰系·舒适)─────────────────
PRIMARY      = RGBColor(0x2C, 0x52, 0x82)   # 深蓝 主强调(标题/竖条/数据)
ACCENT       = RGBColor(0x31, 0x82, 0xCE)   # 钢蓝 次强调
DARK         = RGBColor(0x2D, 0x37, 0x48)   # 深蓝灰 表头
SLATE        = RGBColor(0x4A, 0x55, 0x68)   # 中深灰蓝 层名块
GRAY         = RGBColor(0x59, 0x59, 0x59)   # 中灰 正文
GRAY_L       = RGBColor(0x8C, 0x8C, 0x8C)   # 浅灰 副文本
BG_GRAY      = RGBColor(0xF5, 0xF7, 0xFA)   # 浅蓝灰背景
BORDER       = RGBColor(0xD9, 0xDE, 0xE6)   # 边框灰蓝
ROW_ALT      = RGBColor(0xF7, 0xF9, 0xFC)   # 表格行交替
PRIM_L       = RGBColor(0xEB, 0xF2, 0xFA)   # 浅蓝(仅★核心价值)
WHITE        = RGBColor(0xFF, 0xFF, 0xFF)
BAD_RED      = RGBColor(0xC0, 0x4E, 0x4E)   # 拉完了等级

# 状态徽章色(柔和, 仅小色块使用)
GREEN        = RGBColor(0x38, 0xA1, 0x6F)
YELLOW       = RGBColor(0xD6, 0x9E, 0x2E)
ORANGE       = RGBColor(0xDD, 0x6B, 0x20)
STATUS_COLOR = {"✓": GREEN, "⚙": YELLOW, "△": ORANGE}

prs = Presentation()
prs.slide_width  = Inches(13.333)
prs.slide_height = Inches(7.5)
SW, SH = prs.slide_width, prs.slide_height
BLANK = prs.slide_layouts[6]
FONT = "微软雅黑"

REPO_ROOT = Path(__file__).resolve().parents[1]
BASE_REF_CANDIDATES = ("release/21.x", "origin/release/21.x")


def git(*args):
    return subprocess.check_output(["git", *args], cwd=REPO_ROOT, text=True)


def resolve_base_ref():
    for ref in BASE_REF_CANDIDATES:
        try:
            subprocess.check_output(
                ["git", "rev-parse", "--verify", ref],
                cwd=REPO_ROOT,
                text=True,
                stderr=subprocess.DEVNULL,
            )
            return ref
        except subprocess.CalledProcessError:
            pass
    return BASE_REF_CANDIDATES[0]


def _add_count(value):
    return 0 if value == "-" else int(value)


def collect_work_stats():
    """统计当前分支相对 release/21.x 的全量增量。"""
    base_ref = resolve_base_ref()
    range_spec = f"{base_ref}...HEAD"
    numstat = git(
        "diff", "--numstat", range_spec, "--", ".",
        ":(exclude)jit_design_doc/*.pptx",
    )
    stats = {
        "added": 0,
        "code_added": 0,
        "source_added": 0,
        "test_added": 0,
        "doc_added": 0,
        "design_docs": 0,
        "commits": 0,
    }
    source_exts = {
        ".c", ".cc", ".cpp", ".cxx", ".h", ".hpp", ".inc", ".td",
        ".def", ".cmake", ".py", ".sh", ".ld", ".ll",
    }
    for line in numstat.splitlines():
        added_s, _deleted_s, path = line.split("\t")[:3]
        added = _add_count(added_s)
        path = path.replace("\\", "/")
        suffix = Path(path).suffix
        is_test = path.startswith("ejit_test/") or "/test/" in path or "/unittests/" in path
        is_doc = path.startswith("jit_design_doc/") or path == "CLAUDE.md" or path.endswith("README.md")
        is_code = suffix in source_exts or path.endswith("CMakeLists.txt")
        stats["added"] += added
        if is_code or (is_test and not path.endswith("README.md")):
            stats["code_added"] += added
        if is_code and not is_doc:
            stats["source_added"] += added
        if is_test:
            stats["test_added"] += added
        if is_doc:
            stats["doc_added"] += added

    doc_files = git(
        "diff", "--name-only", range_spec, "--", "jit_design_doc",
        ":(exclude)jit_design_doc/*.pptx",
    )
    stats["design_docs"] = sum(1 for p in doc_files.splitlines() if p.endswith(".md"))
    stats["commits"] = int(git("rev-list", "--count", f"{base_ref}..HEAD").strip())
    return stats


def count_tests():
    """动态统计四套测试用例数量。"""
    def n(rel):
        return len(glob.glob(str(REPO_ROOT / rel)))
    return {
        "lit": n("llvm/test/Transforms/EmbeddedJIT/*.ll"),
        "clang": n("clang/test/CodeGen/ejit_*.c") + n("clang/test/Sema/ejit_*.cpp"),
        "gtest": n("llvm/unittests/ExecutionEngine/EJIT/*Test.cpp"),
        "integ": n("ejit_test/*.c"),
    }


WORK_STATS = collect_work_stats()
TEST_STATS = count_tests()


def fmt_k(n):
    return f"{n / 1000:.1f}K" if n >= 10000 else f"{n:,}"

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
    """统一白底 + 细灰边框 + 左侧主色竖条。"""
    rect(s, x, y, w, h, WHITE, line=BORDER, line_w=0.75)
    rect(s, x, y, Inches(0.07), h, bar)

def header(s, title, subtitle=None):
    rect(s, 0, 0, SW, Inches(0.07), PRIMARY)
    rect(s, 0, Inches(0.07), Inches(0.45), Inches(0.55), PRIMARY)
    txt(s, Inches(0.6), Inches(0.12), Inches(11.0), Inches(0.5),
        [[(title, 23, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
    if subtitle:
        txt(s, Inches(0.6), Inches(0.58), Inches(12.4), Inches(0.26),
            [[(subtitle, 10.5, GRAY, False, FONT)]])
    rect(s, 0, Inches(0.88), SW, Inches(0.015), BORDER)

def item_runs(items, sz=9):
    """[(名称, 状态[, 名称颜色]), ...] → 名称+状态徽章 runs。"""
    runs = []
    for it in items:
        name, st = it[0], it[1]
        col = it[2] if len(it) > 2 else DARK
        runs.append((name, sz, col, True, FONT))
        runs.append((st, sz, STATUS_COLOR.get(st, GREEN), True, FONT))
        runs.append(("   ", sz, GRAY, False, FONT))
    return runs

def v_arrow(s, cx, y):
    txt(s, cx - Inches(0.1), y, Inches(0.2), Inches(0.12),
        [[("▼", 9, GRAY_L, True, FONT)]], align=PP_ALIGN.CENTER)

# ════════════════════════════════════════════════════════════════
# 第 1 页：整体架构完成情况
# ════════════════════════════════════════════════════════════════
def page1():
    s = slide()
    header(s, "EJIT 整体架构与完成情况",
           "EmbeddedJIT · 时间窗常量 + 运行时特化 · 2026-05 ~ 2026-09")

    lx = Inches(0.35); lw = Inches(8.55)

    # 范式条 — 深蓝底白字
    ly = Inches(0.93)
    rect(s, lx, ly, lw, Inches(0.38), PRIMARY)
    txt(s, lx, ly, lw, Inches(0.38),
        [[("① 范式: 时间窗常量 + 运行时特化", 11.5, WHITE, True, FONT),
          ("    ✓", 11.5, RGBColor(0x8E,0xE3,0xB8), True, FONT)]],
        align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
    v_arrow(s, lx + lw/2, ly + Inches(0.38))

    # 源码层 — 深灰底
    yy = ly + Inches(0.50)
    rect(s, lx, yy, lw, Inches(0.48), SLATE)
    txt(s, lx + Inches(0.15), yy + Inches(0.03), lw - Inches(0.3), Inches(0.22),
        [[("源码层  ", 11, WHITE, True, FONT),
          ("② Clang 8 属性标注面", 10.5, WHITE, True, FONT),
          ("    ✓", 11, RGBColor(0x8E,0xE3,0xB8), True, FONT)]])
    txt(s, lx + Inches(0.15), yy + Inches(0.26), lw - Inches(0.3), Inches(0.2),
        [[("ejit_entry / ejit_period / ejit_period_arr / ejit_dim / ejit_may_const / "
           "ejit_period_lc / ejit_bound_ptr / ejit_free_dim",
           8, RGBColor(0xC8,0xC8,0xC8), False, FONT)]])
    v_arrow(s, lx + lw/2, yy + Inches(0.48))

    # AOT 编译期 — 白底竖条
    yy = ly + Inches(1.10)
    card(s, lx, yy, lw, Inches(0.88))
    txt(s, lx + Inches(0.18), yy + Inches(0.03), lw - Inches(0.3), Inches(0.24),
        [[("AOT 编译期 (两阶段 · 5 Pass)", 11, DARK, True, FONT)]])
    txt(s, lx + Inches(0.18), yy + Inches(0.28), lw - Inches(0.3), Inches(0.26),
        [[("O2前:  ", 9, DARK, True, FONT)] +
         item_runs([("③ 两阶段bitcode提取(entry-first+闭包瘦身)", "✓"),
                    ("④ may_const双重保留+预优化", "✓"),
                    ("⑤ 协调器PASS5", "✓")])])
    txt(s, lx + Inches(0.18), yy + Inches(0.56), lw - Inches(0.3), Inches(0.26),
        [[("O2后:  ", 9, DARK, True, FONT)] +
         item_runs([("⑥ Period注册", "✓"),
                    ("⑦ 单函数Wrapper+稠密索引(嵌套entry边界)", "✓"),
                    ("⑧ 生命周期护栏", "✓"),
                    ("⑨ 双轨注册+指纹", "✓")])])
    v_arrow(s, lx + lw/2, yy + Inches(0.88))

    # 运行时特化引擎 — 白底竖条
    yy = ly + Inches(2.10)
    card(s, lx, yy, lw, Inches(1.24))
    txt(s, lx + Inches(0.18), yy + Inches(0.03), lw - Inches(0.3), Inches(0.24),
        [[("运行时特化引擎", 11, DARK, True, FONT),
          ("  业务调用 → wrapper → 命中缓存跳转 / 失败 fallback AOT", 8.5, GRAY_L, False, FONT)]])
    txt(s, lx + Inches(0.18), yy + Inches(0.28), lw - Inches(0.3), Inches(0.24),
        [item_runs([("⑩ 缓存LRU", "✓"), ("→ ⑪ 调度器", "✓"), ("→ ⑫ Bitcode加载", "✓"),
                    ("→ ⑬ 优化流水线", "✓"), ("→ ⑭ PASS6字段替换", "✓", PRIMARY),
                    ("→ ⑮ OrcJIT(JITLink+relax stub)", "✓")])])
    txt(s, lx + Inches(0.18), yy + Inches(0.52), lw - Inches(0.3), Inches(0.24),
        [[("代码内存:  ", 9, DARK, True, FONT)] +
         item_runs([("⑯ EJitCodePool (2MiB固定语义池 + 4K-seal + rodata折叠 + Tier-1远池)", "✓")])])
    txt(s, lx + Inches(0.18), yy + Inches(0.76), lw - Inches(0.3), Inches(0.24),
        [[("命中加速:  ", 9, DARK, True, FONT)] +
         item_runs([("㉗ 多版本内联缓存 (frame-less 4指令命中)", "✓"),
                    ("→ ㉘ 共享分区表 (跨核原位清零失效)", "✓")])])
    txt(s, lx + Inches(0.18), yy + Inches(1.00), lw - Inches(0.3), Inches(0.24),
        [[("在线反馈:  ", 9, DARK, True, FONT)] +
         item_runs([("㉙ PGO双tier (Tier-1采样→Tier-2特化)", "✓"),
                    ("+ ㉚ 值剖析", "⚙"),
                    ("+ ㉛ 冷回收", "✓")])])
    v_arrow(s, lx + lw/2, yy + Inches(1.24))

    # 异步 Taskpool — 白底竖条
    yy = ly + Inches(3.46)
    card(s, lx, yy, lw, Inches(0.56))
    txt(s, lx + Inches(0.18), yy + Inches(0.03), lw - Inches(0.3), Inches(0.22),
        [[("异步编译调度 / Taskpool", 11, DARK, True, FONT)]])
    txt(s, lx + Inches(0.18), yy + Inches(0.26), lw - Inches(0.3), Inches(0.26),
        [item_runs([("⑰ 单worker调度", "✓"),
                    ("⑱ 跨核共享单worker (fixed-core pinning)", "△"),
                    ("⑲ 无锁队列+去重+分桶+查询性能模型", "✓"),
                    ("⑳ version失效", "✓")])])

    # 横切支柱 — 白底竖条
    yy = ly + Inches(4.04)
    card(s, lx, yy, lw, Inches(0.80))
    txt(s, lx + Inches(0.18), yy + Inches(0.03), lw - Inches(0.3), Inches(0.22),
        [[("横切支柱 (贯穿所有层)", 11, DARK, True, FONT)]])
    txt(s, lx + Inches(0.18), yy + Inches(0.26), lw - Inches(0.3), Inches(0.24),
        [item_runs([("㉑ 稠密槽注册表", "✓"), ("㉒ 注册暂存+冻结", "✓"),
                    ("㉓ SRE平台抽象", "△"), ("㉔ 并发原语层", "✓")])])
    txt(s, lx + Inches(0.18), yy + Inches(0.52), lw - Inches(0.3), Inches(0.24),
        [item_runs([("㉕ LLVM裁剪三层 (53.a 117MB→单ejit.o 32MB)", "✓"),
                    ("㉖ BareMetal裸核 (+aarch64_be大端)", "✓")])])

    # 深化与加固 — 白底竖条 (Q3)
    yy = ly + Inches(4.86)
    card(s, lx, yy, lw, Inches(0.74), bar=ACCENT)
    txt(s, lx + Inches(0.18), yy + Inches(0.03), lw - Inches(0.3), Inches(0.22),
        [[("深化与加固 (Q3 · 07~09月)", 11, DARK, True, FONT),
          ("  从「能跑」到「快而可观测」", 8.5, GRAY_L, False, FONT)]])
    txt(s, lx + Inches(0.18), yy + Inches(0.26), lw - Inches(0.3), Inches(0.22),
        [item_runs([("㉜ bound_ptr维度绑定指针", "✓"), ("㉝ free_dim", "✓"),
                    ("㉞ may_const收益排序", "✓"), ("㉟ bitcode闭包瘦身", "✓")])])
    txt(s, lx + Inches(0.18), yy + Inches(0.50), lw - Inches(0.3), Inches(0.22),
        [item_runs([("㊱ 替换验证器", "⚙"), ("㊲ 函数体周期计时", "⚙"),
                    ("㊳ 分支剖析审计", "⚙"), ("㊴ 诊断体系", "✓"),
                    ("㊵ 后端裁剪增强", "✓")])])

    # ── 右：完成情况统计 ──
    rx = Inches(9.05); ry = Inches(0.95); rw = Inches(3.95)
    rect(s, rx, ry, rw, Inches(0.40), SLATE)
    txt(s, rx, ry, rw, Inches(0.40),
        [[("完成情况统计", 13, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)

    status_cards = [
        (Inches(1.45), GREEN, "34", "✓ 已实现使用", "代码完成并接入"),
        (Inches(2.35), YELLOW, "4", "⚙ 默认OFF实验", "验证器/计时/审计/值剖析"),
        (Inches(3.25), ORANGE, "2", "△ 代码完成待真机", "待 aarch64 SRE多核验证"),
    ]
    for sy, bar, num, label, sub in status_cards:
        card(s, rx, sy, rw, Inches(0.80), bar=bar)
        txt(s, rx + Inches(0.18), sy, Inches(0.9), Inches(0.80),
            [[(num, 26, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, rx + Inches(1.0), sy + Inches(0.10), rw - Inches(1.15), Inches(0.32),
            [[(label, 11.5, bar, True, FONT)]])
        txt(s, rx + Inches(1.0), sy + Inches(0.44), rw - Inches(1.15), Inches(0.28),
            [[(sub, 8.5, GRAY, False, FONT)]])

    # 总技术点红条
    sy3 = Inches(4.15)
    rect(s, rx, sy3, rw, Inches(0.45), PRIMARY)
    txt(s, rx + Inches(0.12), sy3, rw - Inches(0.24), Inches(0.45),
        [[("40 个技术点 · 覆盖 6 个域 · 8 属性 · 7 Pass", 11, WHITE, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)

    # 工作量数据(含测试)
    sy4 = sy3 + Inches(0.55)
    txt(s, rx, sy4, rw, Inches(0.26),
        [[("▎工作量数据 (相对 release/21.x)", 11, DARK, True, FONT)]])
    work = [
        (fmt_k(WORK_STATS["added"]), "全部增量"),
        (fmt_k(WORK_STATS["code_added"]), "代码增量"),
        (fmt_k(WORK_STATS["test_added"]), "测试/验证"),
        (str(WORK_STATS["commits"]), "分支提交"),
        (str(WORK_STATS["design_docs"]), "设计文档"),
        ("aarch64_be", "SRE裸核"),
    ]
    wy = sy4 + Inches(0.32)
    for i, (v, k) in enumerate(work):
        col = i % 2; row = i // 2
        x = rx + col * Inches(2.03)
        y = wy + row * Inches(0.48)
        rect(s, x, y, Inches(1.93), Inches(0.44), BG_GRAY, line=BORDER, line_w=0.5)
        txt(s, x + Inches(0.12), y, Inches(1.0), Inches(0.44),
            [[(v, 12, PRIMARY, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, x + Inches(1.0), y, Inches(0.92), Inches(0.44),
            [[(k, 8, GRAY, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)

    # ── 底部时间轴(05~09 开发节奏) ──
    tl_y = Inches(6.60)
    node_start = Inches(1.35); node_end = Inches(12.80)
    line_y = tl_y + Inches(0.50)
    rect(s, node_start, line_y, node_end - node_start, Inches(0.035), PRIMARY)
    milestones = [
        ("05.03", "立项"),
        ("05.04", "AOT主体"),
        ("06.16", "demo打通"),
        ("06.27", "共享worker"),
        ("07.05", "诊断+裁剪"),
        ("07.28", "icache"),
        ("08.14", "在线PGO"),
        ("08.20", "验证器+瘦身"),
        ("08.26", "bound-ptr"),
        ("09.02", "语义池+seal"),
        ("09.09", "PGO冷恢复"),
        ("09.15", "free_dim收官"),
    ]
    n = len(milestones)
    seg = (node_end - node_start) / (n - 1)
    for i, (date, label) in enumerate(milestones):
        cx = node_start + seg * i
        txt(s, cx - Inches(0.52), tl_y, Inches(1.04), Inches(0.24),
            [[(label, 7.5, DARK, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.BOTTOM)
        txt(s, cx - Inches(0.52), tl_y + Inches(0.25), Inches(1.04), Inches(0.18),
            [[(date, 8, PRIMARY, True, FONT)]], align=PP_ALIGN.CENTER)
        dot = s.shapes.add_shape(MSO_SHAPE.OVAL, cx - Inches(0.055), line_y - Inches(0.04), Inches(0.11), Inches(0.11))
        dot.fill.solid(); dot.fill.fore_color.rgb = PRIMARY
        dot.line.color.rgb = WHITE; dot.line.width = Pt(1.2)
        dot.shadow.inherit = False

# ════════════════════════════════════════════════════════════════
# 第 2 页：详细技术项 · 核心架构 (05~06 搭建期)
# ════════════════════════════════════════════════════════════════
def page2():
    s = slide()
    header(s, "EJIT 详细技术项 · 核心架构",
           "26 个技术点 · 6 个域 · 架构搭建期 (2026-05 ~ 2026-06) · 实现状态 · 关键设计决策")

    data = [
        ("①", "时间窗常量+运行时特化", "全局", "✓", "立身之本: 时间窗内数据当编译期常量特化, 失败回退AOT"),
        ("②", "Clang 属性标注面", "前端", "✓", "6基础属性 Attr.td→SemaEJIT→CGEJIT 全链路; may_const软标注丢失安全"),
        ("③", "两阶段 bitcode 提取嵌入 PASS1", "AOT", "✓", "O2前提取调用图闭包, embed @__ejit_bitcode; entry定义排前+闭包瘦身(Q3)"),
        ("④", "may_const 双重保留 + AOT预优化", "AOT", "✓", "LLVM层kind保留 + GV偏移回退(v1.7) + reAnnotate; PASS1↔PASS6纽带"),
        ("⑤", "AOT 协调器 PASS5", "AOT", "✓", "调度PASS2→3→4 + hasAnyEjitMetadata快速返回 + 一致性诊断"),
        ("⑥", "Period/静态变量注册 PASS2", "AOT", "✓", "生成 ejit_register_period_array / static_var 注册调用"),
        ("⑦", "单函数 Wrapper + 稠密索引 PASS3", "AOT", "✓", "名字哈希→稠密funcIndex(防碰撞); 嵌套entry特化边界保留; JIT失败fall原函数体"),
        ("⑧", "时间窗生命周期护栏 PASS4", "AOT", "✓", "ejit_period_lc 函数入口deactivate / 出口activate, 同序配对"),
        ("⑨", "双轨运行时注册体系", "AOT", "✓", "global_ctors构造器 + __ejit_registry_*[]静态表(裸核); 指纹重建+强制静态注册"),
        ("⑩", "编译缓存 LRU", "运行时", "✓", "iterator内嵌O(1); cacheKey=funcIdx<<32|dims; 三重上限+period失效"),
        ("⑪", "编译调度器 CompileDriver", "运行时", "✓", "热路径单哈希查缓存, miss走compileCold(解码/验证/编译/入缓存)"),
        ("⑫", "Bitcode 加载器 ModuleLoader", "运行时", "✓", "funcIdx稠密键(非funcName); 缓存period元数据; 幂等/冲突拒绝"),
        ("⑬", "JIT 优化流水线 EJitOptimizer", "运行时", "✓", "参数替换→InstCombine→PASS6→L1/L2/L3; Inline禁用(AOT预内联)"),
        ("⑭", "结构体字段常量替换 PASS6 ★", "运行时", "✓", "核心价值: may_const load→GEP算offset→读内存→构造Constant; 三模式"),
        ("⑮", "OrcJIT 引擎 EJitOrcEngine", "运行时", "✓", "LLJIT+JITLink替代MCJIT; per-cacheKey JITDylib隔离; Large code model; relax分支stub"),
        ("⑯", "SRE 机器码内存池 EJitCodePool", "运行时", "✓", "2MiB固定语义池+near-hot 4K-seal+rodata折叠exec段; Tier-1远池; callback注入可单测"),
        ("⑰", "纯异步单 worker 调度", "taskpool", "✓", "producer入队即返回fallback, 平台task上单worker轮询; CompileCallback解耦"),
        ("⑱", "跨核共享单 worker taskpool", "taskpool", "△", "POD共享blob+CAS选举owner; generation代际隔离; fixed-core pinning; 代码✓待真机验证"),
        ("⑲", "无锁队列+去重+分桶缓存", "taskpool", "✓", "Vyukov MPSC; 32桶隔离rehash; commit gate锁内校验; 查询路径性能模型+bench实测"),
        ("⑳", "SwitchController version 失效", "taskpool", "✓", "逐实例version单调; toggle后三层全懒失效; 8×256二维数组零分配"),
        ("㉑", "稠密槽注册表管理", "横切", "✓", "注册期单线程分配稠密槽(lifecycle[0,8)/funcIndex[0,4096)), 无锁"),
        ("㉒", "注册暂存 + 双路径 + 冻结", "横切", "✓", "构造期暂存→init消费; 构造器/静态表双路径; taskpool下冻结注册"),
        ("㉓", "SRE 平台抽象层", "横切", "△", "头抽象+链接择一; 平台符号无weak fallback(缺失即链接错); host✓ SRE待接入"),
        ("㉔", "并发原语层", "横切", "✓", "EJitAtomic(__atomic_*)/EJitRwLock(双变量)/EJitIpcLock(短临界区)"),
        ("㉕", "LLVM 裁剪三层体系", "横切", "✓", "源码宏排除+EJitPassBuilder+lipo.py; 53.a(117MB)→单ejit.o 32MB; 后端裁剪增强(Q3)"),
        ("㉖", "BareMetal 裸核运行时", "横切", "✓", "EJIT_FREESTANDING; no-op std头+POSIX桩库; std::mutex→BareMetalMutex; aarch64_be大端"),
    ]

    cols = [("#", 0.42), ("技术点", 3.35), ("层", 0.85), ("状态", 0.62), ("关键设计决策", 7.05)]
    tx = Inches(0.3); ty = Inches(1.05)
    cx = tx
    for name, w in cols:
        rect(s, cx, ty, Inches(w), Inches(0.36), DARK)
        txt(s, cx, ty, Inches(w), Inches(0.36),
            [[(name, 11, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(w)

    ry = ty + Inches(0.36)
    rh = Inches(0.197)
    for i, (no, name, layer, st, note) in enumerate(data):
        is_star = "★" in name
        bg = PRIM_L if is_star else (ROW_ALT if i % 2 == 0 else WHITE)
        cx = tx
        for name_, w in cols:
            rect(s, cx, ry, Inches(w), rh, bg, line=BORDER, line_w=0.25)
            cx += Inches(w)
        cx = tx
        txt(s, cx, ry, Inches(cols[0][1]), rh, [[(no, 9.5, PRIMARY, True, FONT)]],
            align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[0][1])
        txt(s, cx + Inches(0.05), ry, Inches(cols[1][1]) - Inches(0.1), rh,
            [[(name, 9.3, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[1][1])
        layer_color = {"全局":PRIMARY,"前端":DARK,"AOT":DARK,"运行时":DARK,
                       "taskpool":DARK,"横切":GRAY}.get(layer, GRAY)
        txt(s, cx, ry, Inches(cols[2][1]), rh, [[(layer, 8.5, layer_color, True, FONT)]],
            align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[2][1])
        rect(s, cx + Inches(0.12), ry + Inches(0.025), Inches(0.34), rh - Inches(0.05), STATUS_COLOR[st])
        txt(s, cx + Inches(0.12), ry + Inches(0.025), Inches(0.34), rh - Inches(0.05),
            [[(st, 10, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[3][1])
        note_color = PRIMARY if is_star else GRAY
        txt(s, cx + Inches(0.05), ry, Inches(cols[4][1]) - Inches(0.1), rh,
            [[(note, 8.6, note_color, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        ry += rh

    # 底部图例
    legy = ry + Inches(0.1)
    rect(s, Inches(0.3), legy, Inches(12.7), Inches(0.4), BG_GRAY, line=BORDER, line_w=0.5)
    txt(s, Inches(0.5), legy, Inches(12.5), Inches(0.4),
        [[("图例:  ", 10, DARK, True, FONT),
          ("✓", 11, GREEN, True, FONT),(" 已实现使用(24)   ", 9.5, GRAY, False, FONT),
          ("△", 11, ORANGE, True, FONT),(" 代码完成待真机验证(2)   ", 9.5, GRAY, False, FONT),
          ("    ★ 核心价值;  Q3 深化项(㉗~㊵)见下页", 9.5, PRIMARY, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)

# ════════════════════════════════════════════════════════════════
# 第 3 页：深化与加固技术项 (07~09)
# ════════════════════════════════════════════════════════════════
def page3_deep():
    s = slide()
    header(s, "EJIT 深化与加固技术项 (Q3)",
           "14 个技术点 · 2026-07 ~ 2026-09 · 从「能跑」到「快而可观测」 · 3 大主线")

    data = [
        ("㉗", "多版本内联缓存 icache ★", "运行时", "✓", "[D]^numDims 每身份一槽; frame-less命中路径(load+musttail,4指令); NO_RECLAIM免版本校验"),
        ("㉘", "icache 共享分区表", "运行时", "✓", "cell表移入跨核共享段; period toggle原位清零跨核失效; 无探针状态/无排空握手"),
        ("㉙", "在线 PGO 分层编译 ★", "运行时", "✓", "Tier-1临时采样→Tier-2带profile特化重编译; 并发暂存+SRE加固; Tier-2分批发布"),
        ("㉚", "PGO 值剖析", "运行时", "⚙", "3类site: 间接调用目标/memop大小/循环界; guard特化+通用fallback; 默认OFF"),
        ("㉛", "PGO 冷回收", "运行时", "✓", "超时回收无进展采样槽+suppression表(128); 生命周期边界自动恢复; 状态<512KiB"),
        ("㉜", "维度绑定指针 bound_ptr", "前端+AOT", "✓", "EJIT_BOUND_PTR绑定EJIT_DIM参数; 无需全局数组名; 多指针借用+指针facts特化"),
        ("㉝", "free_dim 自由维度", "前端+AOT", "✓", "标注may_const数据不依赖的索引; GEP全常量折叠; 解决non-const-offset不可寻址"),
        ("㉞", "may_const 收益排序", "运行时", "✓", "按活跃站点/运行时影响排序; 请求转发owner核; 聚焦高价值特化"),
        ("㉟", "bitcode 闭包瘦身", "AOT", "✓", "内联后外链化: 大闭包helper外部化+显式注册; 与finline-hint标注驱动哲学一致"),
        ("㊱", "may_const 替换验证器", "运行时", "⚙", "EJIT_VERIFY_SUBSTITUTION运行时校验替换正确性; 默认OFF零开销"),
        ("㊲", "函数体周期计时", "运行时", "⚙", "AOT/JIT body cycles对比+wrapper开销; musttail保形; -ejit-function-body-timing"),
        ("㊳", "分支剖析审计", "运行时", "⚙", "复用Tier-1计数器; 95%/60%偏置统计; 找无优化机会标注; CMake默认OFF(aarch64_be预设开启)"),
        ("㊴", "诊断体系", "横切", "✓", "日志级别OFF/INFO/VERBOSE/DEBUG; IR/ASM dump; print_compiled; SRE安全节流"),
        ("㊵", "LLVM 后端裁剪增强", "横切", "✓", "MachineVerifier/文本asm/MCInstPrinter/指令名表/per-CPU调度表 strip"),
    ]

    cols = [("#", 0.42), ("技术点", 2.75), ("层", 0.85), ("状态", 0.62), ("关键设计决策", 7.65)]
    tx = Inches(0.3); ty = Inches(1.05)
    cx = tx
    for name, w in cols:
        rect(s, cx, ty, Inches(w), Inches(0.36), DARK)
        txt(s, cx, ty, Inches(w), Inches(0.36),
            [[(name, 11, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(w)

    ry = ty + Inches(0.36)
    rh = Inches(0.26)
    for i, (no, name, layer, st, note) in enumerate(data):
        is_star = "★" in name
        bg = PRIM_L if is_star else (ROW_ALT if i % 2 == 0 else WHITE)
        cx = tx
        for name_, w in cols:
            rect(s, cx, ry, Inches(w), rh, bg, line=BORDER, line_w=0.25)
            cx += Inches(w)
        cx = tx
        txt(s, cx, ry, Inches(cols[0][1]), rh, [[(no, 9.5, PRIMARY, True, FONT)]],
            align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[0][1])
        txt(s, cx + Inches(0.05), ry, Inches(cols[1][1]) - Inches(0.1), rh,
            [[(name, 9.3, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[1][1])
        layer_color = {"前端+AOT": ACCENT, "AOT": DARK, "运行时": DARK, "横切": GRAY}.get(layer, GRAY)
        txt(s, cx, ry, Inches(cols[2][1]), rh, [[(layer, 8.5, layer_color, True, FONT)]],
            align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[2][1])
        rect(s, cx + Inches(0.12), ry + Inches(0.035), Inches(0.34), rh - Inches(0.07), STATUS_COLOR[st])
        txt(s, cx + Inches(0.12), ry + Inches(0.035), Inches(0.34), rh - Inches(0.07),
            [[(st, 10, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        cx += Inches(cols[3][1])
        note_color = PRIMARY if is_star else GRAY
        txt(s, cx + Inches(0.05), ry, Inches(cols[4][1]) - Inches(0.1), rh,
            [[(note, 8.4, note_color, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        ry += rh

    # ── Q3 三大主线小结 ──
    ry2 = ry + Inches(0.12)
    themes = [
        ("性能主线", PRIMARY, "icache 4指令命中路径 · 在线PGO双tier · 固定语义代码池"),
        ("特化面扩展", ACCENT, "bound_ptr / free_dim 标注驱动 · 收益排序 · bitcode闭包瘦身"),
        ("质量护栏", GREEN, "替换验证器 · 函数体计时 · 分支审计 · 诊断体系"),
    ]
    tw = Inches(4.1); gap = Inches(0.15)
    for i, (t, c, d) in enumerate(themes):
        x = Inches(0.3) + i * (tw + gap)
        card(s, x, ry2, tw, Inches(0.95), bar=c)
        txt(s, x + Inches(0.18), ry2 + Inches(0.07), tw - Inches(0.3), Inches(0.28),
            [[(t, 10.5, DARK, True, FONT)]])
        txt(s, x + Inches(0.18), ry2 + Inches(0.38), tw - Inches(0.3), Inches(0.5),
            [[(d, 8.3, GRAY, False, FONT)]])

    # ── 底部: 图例 + 测试体系 ──
    legy = ry2 + Inches(1.1)
    rect(s, Inches(0.3), legy, Inches(12.7), Inches(0.5), BG_GRAY, line=BORDER, line_w=0.5)
    t = TEST_STATS
    txt(s, Inches(0.5), legy, Inches(12.5), Inches(0.5),
        [[("图例:  ", 10, DARK, True, FONT),
          ("✓", 11, GREEN, True, FONT),(" 已实现使用(34)   ", 9.5, GRAY, False, FONT),
          ("⚙", 11, YELLOW, True, FONT),(" 默认OFF实验(4)   ", 9.5, GRAY, False, FONT),
          ("△", 11, ORANGE, True, FONT),(" 待真机验证(2)   ", 9.5, GRAY, False, FONT),
          ("    测试体系:  ", 10, DARK, True, FONT),
          (f"{t['lit']} AOT lit · {t['clang']} Clang lit · {t['gtest']} gtest · {t['integ']} 集成C用例",
           9.5, PRIMARY, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)

# ════════════════════════════════════════════════════════════════
# 第 4 页：EJIT 后续工作规划
# ════════════════════════════════════════════════════════════════
def page4_roadmap():
    s = slide()
    header(s, "EJIT 后续工作规划", "从代码完成 → 真机验证 → 性能收益 → 可交付工程化")

    bx = Inches(0.35); by = Inches(0.98); bw = Inches(12.65)
    rect(s, bx, by, bw, Inches(0.5), PRIMARY)
    txt(s, bx, by, bw, Inches(0.5),
        [[("目标: 形成可复现的 EJIT 闭环证据链 —— ", 11.5, WHITE, True, FONT),
          ("真机可跑", 11.5, RGBColor(0xCF,0xE0,0xF5), True, FONT),
          (" / ", 11.5, WHITE, True, FONT),
          ("收益可量化", 11.5, RGBColor(0xCF,0xE0,0xF5), True, FONT),
          (" / ", 11.5, WHITE, True, FONT),
          ("交付可裁剪", 11.5, RGBColor(0xCF,0xE0,0xF5), True, FONT)]],
        align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)

    # Q3 已交付条
    q3y = Inches(1.60)
    card(s, bx, q3y, bw, Inches(0.52), bar=GREEN)
    txt(s, bx + Inches(0.18), q3y, bw - Inches(0.3), Inches(0.52),
        [[("Q3 已交付 (07~09):  ", 10.5, DARK, True, FONT),
          ("在线PGO双tier · icache多版本+共享表 · bound_ptr+free_dim · bitcode闭包瘦身 · "
           "替换验证器/计时/审计 · 诊断体系+后端裁剪增强", 9.5, GREEN, True, FONT)]],
        anchor=MSO_ANCHOR.MIDDLE)

    # ── 三阶段路线图 ──
    phases = [
        ("近期：验证闭环", "2~4 周", GREEN, [
            ("SRE/AArch64 真机", "共享taskpool/icache/在线PGO多核验证, 裸核链接"),
            ("代码池真机", "4K-seal / near-hot语义池边界、发布与fallback行为"),
            ("诊断闭环", "IR/ASM dump + 函数体计时定位真机热点"),
            ("业务场景", "bound_ptr/free_dim 覆盖真实配置结构"),
        ]),
        ("中期：收益量化", "1~2 月", ACCENT, [
            ("Benchmark 体系", "PGO Tier-2加速比 / icache命中率 / 编译耗时"),
            ("调参策略", "L1/L2/L3 pipeline、缓存上限、PGO阈值+冷回收timeout"),
            ("值剖析评估", "开启代价 vs 收益, 决定是否默认化"),
            ("裁剪体积", "单 ejit.o 继续瘦身: PassBuilder/Target/JITLink 收敛"),
        ]),
        ("远期：工程交付", "季度", PRIMARY, [
            ("平台抽象固化", "host/SRE/bare-metal 三套配置稳定化与文档化"),
            ("诊断与可观测", "trace/dump/错误码/统计 counters 统一输出"),
            ("安全护栏", "注册冻结、符号白名单、内存权限切换与越界保护"),
            ("论文/专利材料", "时间窗常量+运行时特化+在线PGO+裁剪JIT体系化总结"),
        ]),
    ]

    x0 = Inches(0.35); y0 = Inches(2.28); gap = Inches(0.2)
    colw = (Inches(12.65) - gap * 2) / 3
    for idx, (title, span, color, items) in enumerate(phases):
        x = x0 + idx * (colw + gap)
        rect(s, x, y0, colw, Inches(0.42), color)
        txt(s, x + Inches(0.12), y0, colw - Inches(0.24), Inches(0.42),
            [[(title, 12, WHITE, True, FONT), ("  ·  ", 10, WHITE, False, FONT),
              (span, 10, RGBColor(0xE8,0xF2,0xFF), True, FONT)]],
            anchor=MSO_ANCHOR.MIDDLE)
        cy = y0 + Inches(0.55)
        for no, (name, desc) in enumerate(items, 1):
            card(s, x, cy, colw, Inches(0.72), bar=color)
            rect(s, x + Inches(0.16), cy + Inches(0.14), Inches(0.34), Inches(0.34), color)
            txt(s, x + Inches(0.16), cy + Inches(0.14), Inches(0.34), Inches(0.34),
                [[(str(no), 9.5, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
            txt(s, x + Inches(0.58), cy + Inches(0.07), colw - Inches(0.75), Inches(0.26),
                [[(name, 10.5, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
            txt(s, x + Inches(0.58), cy + Inches(0.34), colw - Inches(0.75), Inches(0.32),
                [[(desc, 8.3, GRAY, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
            cy += Inches(0.8)

    # ── 底部交付物 ──
    dy = Inches(6.18)
    txt(s, Inches(0.35), dy - Inches(0.28), Inches(12.65), Inches(0.25),
        [[("▎阶段性交付物", 12.5, DARK, True, FONT)]])
    deliverables = [
        ("真机验证报告", "SRE 多核 / 裸核 / fallback"),
        ("性能收益表", "命中率 / 耗时 / 加速比"),
        ("裁剪构建包", "LLVMEJIT.a / 单 ejit.o"),
        ("可复现 Demo", "脚本 + 测试 + 文档"),
    ]
    dw = Inches(3.05)
    for i, (name, desc) in enumerate(deliverables):
        x = Inches(0.35) + i * Inches(3.2)
        rect(s, x, dy, dw, Inches(0.72), BG_GRAY, line=BORDER, line_w=0.5)
        rect(s, x, dy, Inches(0.06), Inches(0.72), PRIMARY)
        txt(s, x + Inches(0.18), dy + Inches(0.06), dw - Inches(0.3), Inches(0.28),
            [[(name, 10.5, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, x + Inches(0.18), dy + Inches(0.36), dw - Inches(0.3), Inches(0.28),
            [[(desc, 8.5, GRAY, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)

# ════════════════════════════════════════════════════════════════
# 第 5 页：编译器的技术规划与畅想 (AI × 传统编译器)
# ════════════════════════════════════════════════════════════════
def page5_ai():
    s = slide()
    header(s, "编译器的技术规划与畅想",
           "AI × 传统编译器 · 用 AI 改造编译器本身 · 18 方向总评榜已归档 (ai4compiler 分支, 2026-06)")

    # 定位条
    bx = Inches(0.35); by = Inches(0.98); bw = Inches(12.65)
    rect(s, bx, by, bw, Inches(0.5), PRIMARY)
    txt(s, bx, by, bw, Inches(0.5),
        [[("用 AI 改造传统编译器本身（而非用编译器服务 AI）· 两大落点：", 11.5, WHITE, True, FONT),
          ("① 产出更优代码（性能）", 11.5, RGBColor(0xCF,0xE0,0xF5), True, FONT),
          ("   ② 自身/产物更小更快（小型化）", 11.5, RGBColor(0xCF,0xE0,0xF5), True, FONT)]],
        align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)

    # ── 左栏: 渐进落地 9 方向(含总评榜等级) ──
    lx = Inches(0.35); ly = Inches(1.58); lw = Inches(6.25)
    txt(s, lx, ly, lw, Inches(0.28),
        [[("▎渐进落地方向 (9) · 总评榜等级: 夯/顶级/人上人/NPC/拉完了", 11.5, DARK, True, FONT)]])

    tier_color = {"夯": PRIMARY, "顶级": ACCENT, "人上人": GREEN, "NPC": GRAY, "拉完了": BAD_RED}
    perf = [
        ("1", "学习型 Inliner 可解释化", "NPC", "蒸馏MLGO黑盒→决策树规则, 可审计/可移植"),
        ("2", "LLM Superoptimizer ★", "顶级", "LLM propose + Alive2 verify, 重写10-30条指令"),
        ("3", "神经代价模型", "人上人", "替代RegAlloc/Scheduler启发式(异构硬件撑不住)"),
        ("4", "PGO 无 profile 化", "NPC", "GNN预测分支频次, 让PGO民主化"),
        ("5", "自动发掘 Combine 规则 ★", "人上人", "扫手写汇编diff, 自动产真实LLVM patch"),
    ]
    size = [
        ("6", "学习型 Size Opt", "NPC", "-Oz继承者, MLGO换size reward"),
        ("7", "LTO学习型死代码消除", "拉完了", "误删=crash; 仅退化为冷代码后置"),
        ("8", "AI辅助Pass删除/瘦身", "NPC", "定制PassManager + 裁剪编译器binary"),
        ("9", "IR 学习型压缩", "拉完了", "解压须快于传输; 仅zstd字典有ROI"),
    ]
    iy = ly + Inches(0.30)
    txt(s, lx, iy, lw, Inches(0.22),
        [[("性能优化（让编译器产出更优代码）", 10, ACCENT, True, FONT)]])
    iy += Inches(0.24)
    for no, name, tier, desc in perf:
        card(s, lx, iy, lw, Inches(0.37), bar=ACCENT)
        txt(s, lx + Inches(0.16), iy, Inches(0.28), Inches(0.37),
            [[(no, 10, PRIMARY, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, lx + Inches(0.44), iy, Inches(2.1), Inches(0.37),
            [[(name, 9, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, lx + Inches(2.56), iy, Inches(0.62), Inches(0.37),
            [[(tier, 7.8, tier_color[tier], True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, lx + Inches(3.2), iy, lw - Inches(3.3), Inches(0.37),
            [[(desc, 7.8, GRAY, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        iy += Inches(0.40)
    iy += Inches(0.03)
    txt(s, lx, iy, lw, Inches(0.22),
        [[("小型化（编译器自身/产物更小更快）", 10, ACCENT, True, FONT)]])
    iy += Inches(0.24)
    for no, name, tier, desc in size:
        card(s, lx, iy, lw, Inches(0.37), bar=ACCENT)
        txt(s, lx + Inches(0.16), iy, Inches(0.28), Inches(0.37),
            [[(no, 10, PRIMARY, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, lx + Inches(0.44), iy, Inches(2.1), Inches(0.37),
            [[(name, 9, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, lx + Inches(2.56), iy, Inches(0.62), Inches(0.37),
            [[(tier, 7.8, tier_color[tier], True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, lx + Inches(3.2), iy, lw - Inches(3.3), Inches(0.37),
            [[(desc, 7.8, GRAY, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        iy += Inches(0.40)

    # ── 右栏: 颠覆性畅想精选 ──
    rx = Inches(6.8); ry = Inches(1.58); rw = Inches(6.2)
    txt(s, rx, ry, rw, Inches(0.28),
        [[("▎颠覆性畅想 (精选 8) · 触及编译器根基假设", 11.5, DARK, True, FONT)]])

    radical = [
        ("夯", "自演化编译器", "编译时持续学习优化规则并固化进下版本, 传统编译器5年内或被淘汰", PRIMARY),
        ("夯", "Verified-by-Default", "AI自动生成性质证明, 代码默认带形式化证书(无UB/data race)", PRIMARY),
        ("顶级", "端到端可微编译器", "整条pipeline离散决策连续松弛, 端到端梯度优化运行时间", ACCENT),
        ("顶级", "程序综合替代编译", "编译器重写算法本身(O(n²)→O(n log n)), I/O等价+profile验证", ACCENT),
        ("顶级", "神经符号混合 IR", "IR节点可为神经嵌入, 中间阶段「模糊思考」后塌缩成符号", ACCENT),
        ("人上人", "跨层联合优化", "打破编译器/OS/硬件分层抽象, AI学跨层隐性接口", GREEN),
        ("人上人", "多模态编译器", "读代码+注释+commit+issue, 理解「程序意图」作优化信号", GREEN),
        ("人上人", "编译器即 OS", "取消编译时/运行时边界, 持续重编译热点, 多版本按场景dispatch", GREEN),
    ]
    iy = ry + Inches(0.30)
    for tier, name, desc, c in radical:
        card(s, rx, iy, rw, Inches(0.52), bar=c)
        rect(s, rx + Inches(0.14), iy + Inches(0.08), Inches(0.5), Inches(0.36), c)
        txt(s, rx + Inches(0.14), iy + Inches(0.08), Inches(0.5), Inches(0.36),
            [[(tier, 7.5, WHITE, True, FONT)]], align=PP_ALIGN.CENTER, anchor=MSO_ANCHOR.MIDDLE)
        txt(s, rx + Inches(0.74), iy + Inches(0.03), rw - Inches(0.9), Inches(0.24),
            [[(name, 10, DARK, True, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        txt(s, rx + Inches(0.74), iy + Inches(0.27), rw - Inches(0.9), Inches(0.22),
            [[(desc, 7.8, GRAY, False, FONT)]], anchor=MSO_ANCHOR.MIDDLE)
        iy += Inches(0.56)

    # ── 底部: 推荐切入路径 + EJIT 结合 ──
    bby = Inches(6.55)
    rect(s, bx, bby, bw, Inches(0.88), BG_GRAY, line=BORDER, line_w=0.5)
    rect(s, bx, bby, Inches(0.07), Inches(0.88), PRIMARY)
    txt(s, bx + Inches(0.18), bby + Inches(0.04), bw - Inches(0.3), Inches(0.26),
        [[("▎推荐切入路径 (总评榜结论)", 10.5, DARK, True, FONT)]])
    txt(s, bx + Inches(0.18), bby + Inches(0.28), bw - Inches(0.3), Inches(0.28),
        [[("① 快速出成果建声誉 → 自动挖Combine规则(每周真实LLVM patch)    "
           "② 顶级且能毕业 → LLM Superoptimizer(Alive2兜底)    "
           "③ 长跑 → 自演化 / Verified-by-Default", 8.8, GRAY, False, FONT)]])
    txt(s, bx + Inches(0.18), bby + Inches(0.56), bw - Inches(0.3), Inches(0.28),
        [[("EJIT 载体: 在线PGO/收益排序已是数据驱动编译工程实例; "
           "AIMV(AI多版本 · clang driver + MCP链路)早期集成已打通", 8.8, PRIMARY, True, FONT)]])

page1()
page2()
page3_deep()
page4_roadmap()
page5_ai()

_today = datetime.now().strftime("%Y%m%d")
out = REPO_ROOT / "jit_design_doc" / f"EJIT进展_{_today}.pptx"
prs.save(out)
print("已生成:", out)
print("共 5 页 (通用汇报风格)")
print(f"统计: {WORK_STATS['commits']} 提交, 增量 {fmt_k(WORK_STATS['added'])} 行, "
      f"设计文档 {WORK_STATS['design_docs']} 篇")
print(f"测试: {TEST_STATS['lit']} AOT lit / {TEST_STATS['clang']} Clang lit / "
      f"{TEST_STATS['gtest']} gtest / {TEST_STATS['integ']} 集成用例")
