"""Small SVG/HTML authoring kit for the EJIT deck build (build-time helpers only)."""
import html as _html
import re

SERIF_CHARS = set()


def esc(s):
    return _html.escape(str(s), quote=True)


def dspy(*texts):
    """Register glyphs used by serif display text (for the serif font subset). Returns the text."""
    for text in texts:
        for ch in re.sub(r'<[^>]+>', '', text):
            SERIF_CHARS.add(ch)
    return texts[0] if len(texts) == 1 else ''.join(texts)


def svg(content, w=1408, h=560, cls='', vb=None, extra=''):
    vb = vb or f'0 0 {w} {h}'
    c = f' {cls}' if cls else ''
    return (f'<svg class="svg{c}" viewBox="{vb}" width="{w}" height="{h}" preserveAspectRatio="xMinYMin meet" '
            f'xmlns="http://www.w3.org/2000/svg" {extra}>{content}</svg>')


def T(x, y, s, cls='', anchor=None, size=None, weight=None, extra=''):
    a = []
    if anchor:
        a.append(f'text-anchor="{anchor}"')
    if size:
        a.append(f'font-size="{size}"')
    if weight:
        a.append(f'font-weight="{weight}"')
    if extra:
        a.append(extra)
    c = f' class="{cls}"' if cls else ''
    return f'<text x="{x}" y="{y}"{(" " + " ".join(a)) if a else ""}{c}>{esc(s)}</text>'


def TS(x, y, spans, anchor=None, size=15, cls='', extra=''):
    """text with tspans: spans = [(text, cls|None)]"""
    a = [f'text-anchor="{anchor}"'] if anchor else []
    if size:
        a.append(f'font-size="{size}"')
    if extra:
        a.append(extra)
    c = f' class="{cls}"' if cls else ''
    out = []
    for t, sc in spans:
        out.append(f'<tspan{" " if sc else ""}{("class=" + chr(34) + sc + chr(34)) if sc else ""}>{esc(t)}</tspan>')
    return f'<text x="{x}" y="{y}"{(" " + " ".join(a)) if a else ""}{c}>{"".join(out)}</text>'


def _zh(s):
    return any('一' <= ch <= '鿿' for ch in s)


def TL(x, y, lines, cls='', anchor=None, size=15, lh=None, weight=None):
    """multi-line text block; lines = [str | (str, cls)]"""
    lh = lh or round(size * 1.42, 1)
    c = f' class="{cls}"' if cls else ''
    at = ([f'text-anchor="{anchor}"'] if anchor else []) + ([f'font-weight="{weight}"'] if weight else [])
    out = [f'<text x="{x}" y="{y}" font-size="{size}"{c}{(" " + " ".join(at)) if at else ""}>']
    for i, ln in enumerate(lines):
        t, sc = (ln, None) if isinstance(ln, str) else ln
        a = [f'x="{x}" dy="{lh}"'] if i else []
        if sc:
            a.append(f'class="{sc}"')
        elif _zh(t):
            a.append('class="zh"')
        out.append(f'<tspan {" ".join(a)}>{esc(t)}</tspan>')
    out.append('</text>')
    return ''.join(out)


def RECT(x, y, w, h, cls='box', rx=12, extra=''):
    return f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" class="{cls}" {extra}/>'


def CIRCLE(cx, cy, r, cls='', extra=''):
    c = f' class="{cls}"' if cls else ''
    return f'<circle cx="{cx}" cy="{cy}" r="{r}"{c} {extra}/>'


def LINE(x1, y1, x2, y2, cls='ln', extra=''):
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" class="{cls}" {extra}/>'


def PATH(d, cls='wire', extra='', pid=None):
    i = f' id="{pid}"' if pid else ''
    return f'<path d="{d}" class="{cls}"{i} {extra}/>'


def arrow(d, color='ink', dash=False, extra='', both=False, pid=None):
    cls = 'wire' + ('-' + color if color != 'ink' else '') + (' dash' if dash else '')
    i = f' id="{pid}"' if pid else ''
    e = extra + (' marker-start="url(#ah-%s")' % color if both else '')
    return f'<path d="{d}" class="{cls}"{i} marker-end="url(#ah-{color})" {e}/>'


def pkt(path_id, run, dur=900, delay=0, hold=0, back=False, rev=False, r=7):
    """packet dot riding a path; visible from step `run` (data-step) and looping."""
    e = f' data-path="{path_id}" data-dur="{dur}" data-delay="{delay}" data-hold="{hold}"'
    if back:
        e += ' data-back="1"'
    if rev:
        e += ' data-rev="1"'
    # The step reveal sits on a <g> wrapper and the motion on the circle inside:
    # a CSS `transform:none` on .step would override the circle's SVG transform attribute.
    head = f'<g class="step plain" data-step="{run}" style="opacity:0">' if run else '<g>'
    return f'{head}<circle class="pkt" r="{r}"{e}/></g>'


def hv(x1, y1, x2, y2, r=12):
    """horizontal, then vertical, then horizontal (Z shape) with rounded corners"""
    s1 = 1 if x2 > x1 else -1
    s2 = 1 if y2 > y1 else -1
    if abs(y2 - y1) < 2 * r:
        return f'M{x1} {y1} H{x2}'
    return (f'M{x1} {y1} H{x2 - r * s1} Q{x2} {y1} {x2} {y1 + r * s2} '
            f'V{y2 - r * s2} Q{x2} {y2} {x2 - r * s1} {y2}')


def vh(x1, y1, x2, y2, r=12):
    s1 = 1 if y2 > y1 else -1
    s2 = 1 if x2 > x1 else -1
    if abs(x2 - x1) < 2 * r:
        return f'M{x1} {y1} V{y2}'
    return f'M{x1} {y1} V{y2 - r * s1} Q{x1} {y2} {x1 + r * s2} {y2} H{x2}'


def rounded_poly(pts, r=12, close=False):
    """rounded-corner polyline through pts=[(x,y),...] using arcs"""
    if len(pts) < 3:
        return 'M' + ' L'.join(f'{x} {y}' for x, y in pts)
    d = f'M{pts[0][0]} {pts[0][1]}'
    for i in range(1, len(pts) - 1):
        (x0, y0), (x1, y1), (x2, y2) = pts[i - 1], pts[i], pts[i + 1]
        v1 = (x1 - x0, y1 - y0)
        v2 = (x2 - x1, y2 - y1)
        l1 = max(1e-6, (v1[0] ** 2 + v1[1] ** 2) ** 0.5)
        l2 = max(1e-6, (v2[0] ** 2 + v2[1] ** 2) ** 0.5)
        rr = min(r, l1 / 2, l2 / 2)
        a = (x1 - v1[0] / l1 * rr, y1 - v1[1] / l1 * rr)
        b = (x1 + v2[0] / l2 * rr, y1 + v2[1] / l2 * rr)
        d += f' L{a[0]:.1f} {a[1]:.1f} Q{x1} {y1} {b[0]:.1f} {b[1]:.1f}'
    d += f' L{pts[-1][0]} {pts[-1][1]}'
    return d


def box(x, y, w, h, title=None, lines=None, cls='box', rx=12, tcls='t-ink', tsize=15, lsize=12.5,
        lcls='t-3', align='middle', tx=None, ty=None):
    """node box with title + optional description lines (centered by default)."""
    out = [RECT(x, y, w, h, cls, rx)]
    cx = x + w / 2 if align == 'middle' else x + 16
    anchor = 'middle' if align == 'middle' else None
    lines = lines or []
    th, lh = tsize * 1.3, lsize * 1.45
    total = (th if title else 0) + len(lines) * lh
    base = (y + h / 2 - total / 2 + th * 0.76) if ty is None else ty
    tc = (tcls + (' zh' if title and _zh(title) else '')).strip()
    if title:
        out.append(T(tx if tx is not None else cx, base, title, tc, anchor=anchor, size=tsize, weight=700))
    if lines:
        ls = [(l, (lcls if isinstance(l, str) else (l[1] or lcls)) + (' zh' if _zh(l if isinstance(l, str) else l[0]) else '')) for l in lines]
        out.append(TL(tx if tx is not None else cx, base + th * 0.5 + lh * 0.72, ls, anchor=anchor, size=lsize, lh=lh))
    return ''.join(out)


def lane(x, y, w, h, label, color, sub=None):
    out = [RECT(x, y, w, h, 'box-3', 14)]
    out.append(RECT(x, y, 4, h, f'solid-{color}', 2))
    out.append(T(x + 20, y + 28, label, f'zh t-ink b700', size=15))
    if sub:
        out.append(T(x + 20 + len(label) * 16.5, y + 28, sub, 't-3', size=12.5))
    return ''.join(out)


def defs():
    m = []
    for c in ['ink', 'amber', 'ice', 'violet', 'red', 'lime']:
        col = 'var(--ink-3)' if c == 'ink' else f'var(--{c})'
        m.append(f'<marker id="ah-{c}" viewBox="0 0 10 10" refX="8.5" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse">'
                 f'<path d="M1.5 1.5 L9 5 L1.5 8.5 z" fill="{col}"/></marker>')
    return ('<svg width="0" height="0" style="position:absolute;width:0;height:0;overflow:hidden" aria-hidden="true"><defs>'
            + ''.join(m) + '</defs></svg>')


# ---------------------------------------------------------------- code highlighting
C_TYPES = (r'(?:uint8_t|uint16_t|uint32_t|uint64_t|int8_t|int16_t|int32_t|int64_t|unsigned|signed|void|char|short|int|long|'
           r'float|double|bool|size_t)')
C_KW = r'(?:if|else|for|while|return|struct|union|enum|typedef|static|const|volatile|inline|switch|case|break|continue|do|sizeof)'
RULES = {
    'c': [
        (r'//[^\n]*|/\*.*?\*/', 'c'),
        (r'EJIT_[A-Z_0-9]+', 'a'),
        (r'"(?:[^"\\]|\\.)*"', 's'),
        (C_KW, 'k'), (C_TYPES, 't'),
        (r'\b\d[xX]?[0-9a-fA-F]*[uUlL]*\b', 'n'),
        (r'\b[A-Za-z_]\w*(?=\s*\()', 'fn'),
    ],
    'll': [
        (r';[^\n]*', 'c'),
        (r'!ejit[\w.]*', 'm'),
        (r'![0-9]+|!tbaa|!llvm\.[\w.]+|!\{\}|!\{', 'p'),
        (r'"(?:[^"\\]|\\.)*"', 's'),
        (r'\b(?:define|declare|load|store|getelementptr|br|ret|icmp|select|phi|call|musttail|tail|switch|label|global|constant)\b', 'k'),
        (r'\b(?:ptr|void|i\d+|float|double|type)\b', 't'),
        (r'@[A-Za-z0-9_.$]+', 'g'),
        (r'\b(?:align|inbounds|nuw|nsw|zeroext|noundef|noinline|nounwind|to|atomic|monotonic|section|dso_local|external|internal|'
         r'local_unnamed_addr|distinct|immutable|dereferenceable|captures|writeonly|readonly|noalias|nocallback|nofree|nosync|'
         r'willreturn|memory|immarg|uwtable|range|preemptable)\b', 'p'),
        (r'\b\d+\b', 'n'),
        (r'\b[\w.]+(?=\()', 'fn'),
    ],
    'asm': [
        (r'//[^\n]*|;[^\n]*|#[^\d\n][^\n]*', 'c'),  # AArch64 immediates (#16) are not comments
        (r'^\s*(?:\.L\w+|[0-9a-f]+):', 'p'),
        (r'^\s*\.cfi\w+.*$', 'p'),
        (r'\b(?:mov|movsx|movsxd|movzbl|movslq|lea|add|sub|imul|imull|mul|div|divl|idiv|inc|dec|cmp|cmpl|test|testl|and|or|xor|shl|shr|sar|sal|'
         r'jmp|je|jne|jge|jl|jle|jg|jbe|ja|jae|jb|js|jns|call|callq|ret|retq|push|pushq|pop|popq|nop|cmovl|cmovle|cmovg|cmovge|cmove|cmovne|'
         r'cmovs|cmovns|sete|setne|cdqe|cdq|cqo|neg|not|adrp|csel|br|blr|stp|ldp|str|sxtw|uxtw)\b', 'k'),
        (r'\b(?:BYTE|WORD|DWORD|QWORD) PTR\b|:lo12:', 'p'),
        (r'%[a-z0-9]+|\b(?:[a-d]l|[a-d]x|[sd]i|[sb]p|sil|dil|[a-d]il|[rs][a-d]x|[rs][sd]i|[rs]bp|rsp|rip|[a-d][hx]|[sd]il|r\d+d?|'
         r'e[a-d]x|e[sd]i|e[sb]p|bpl|spl|[xw]\d+|sp|xzr|wzr)\b', 'reg'),
        (r'-?0x[0-9a-f]+|\b\d+\b', 'n'),
    ],
    'sh': [
        (r'#[^\n]*', 'c'),
        (r'"(?:[^"\\]|\\.)*"', 's'),
        (r'\bninja\b|\bclang\w*|\bEjit\w+|\bejit[_\w]*', 'k'),
        (r'(?<![\w-])-{1,2}[\w-]+', 't'),
    ],
    'txt': [],
}


def _tok(line, rules):
    out, pos = [], 0
    if not rules:
        return [(line, None)]
    pat = re.compile('|'.join(f'(?P<g{n}>{r})' for n, (r, _) in enumerate(rules)))
    for m in pat.finditer(line):
        if m.start() > pos:
            out.append((line[pos:m.start()], None))
        out.append((m.group(0), rules[int(m.lastgroup[1:])][1]))
        pos = m.end()
    if pos < len(line):
        out.append((line[pos:], None))
    return out


CODE_WIDTHS = []


def _w(line, size):
    w = 0.0
    for ch in line:
        w += size if ord(ch) > 0x2E7F else size * 0.6
    return w


def code(text, lang='c', cls='', hl=None, boxes=None, cap=None, marks=None, maxw=None):
    """highlighted code block. hl={lineno: color}; boxes={lineno: [(substr, color)]} (first match);
    marks={lineno: (step, color)}: line that lights up when the slide reaches `step`."""
    hl, boxes, marks = hl or {}, boxes or {}, marks or {}
    rules = RULES[lang]
    out = []
    for n, ln in enumerate(text.strip('\n').split('\n'), 1):
        spans = _tok(ln, rules)
        ranges = []
        for sub, color in boxes.get(n, []):
            p = ln.find(sub)
            if p >= 0:
                ranges.append((p, p + len(sub), color))
        line_html, pos = '', 0
        for text_, c in spans:
            s0, s1 = pos, pos + len(text_)
            pos = s1
            bcls = next((col for a, b, col in ranges if a < s1 and b > s0), None)
            inner = esc(text_)
            if c and bcls:
                line_html += f'<span class="{c}"><span class="box {bcls}">{inner}</span></span>'
            elif bcls:
                line_html += f'<span class="box {bcls}">{inner}</span>'
            elif c:
                line_html += f'<span class="{c}">{inner}</span>'
            else:
                line_html += inner
        if n in hl:
            line_html = f'<span class="hl {hl[n]}">{line_html}</span>'
        if n in marks:
            st_, col_ = marks[n]
            line_html = f'<span class="hlr {col_}" data-step="{st_}">{line_html}</span>'
        # callout comments (marked with an arrow) speak in the accent colour
        for a, b in (('<span class="c">; ←', '<span class="cm">; ←'), ("<span class=\"c\">// ←", '<span class="cm">// ←'),
                     ('<span class="c">; →', '<span class="cm">; →'), ("<span class=\"c\">// →", '<span class="cm">// →')):
            line_html = line_html.replace(a, b)
        out.append(line_html)
    size = {'sm': 15, 'xs': 13.5, 'lg': 19}.get(cls.strip(), 17)
    wmax = max(_w(l, size) for l in text.strip('\n').split('\n'))
    CODE_WIDTHS.append((cap or lang, cls.strip() or 'md', round(wmax), len(text.strip('\n').split('\n')), maxw))
    classes = 'code ' + cls + (' hl-pad' if cap else '')
    caphtml = f'<span class="cap">{esc(cap)}</span>' if cap else ''
    return f'<pre class="{classes}">{caphtml}' + '\n'.join(out) + '</pre>'
