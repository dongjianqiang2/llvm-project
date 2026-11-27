#!/usr/bin/env python3
"""Assemble the EJIT talk: slides (Python) + css + js + subset fonts -> one HTML file.

usage: python3 build.py [out.html]
env:   EJIT_TALK_FONTS  dir with NotoSansSC.ttf, NotoSerifSC.ttf, JetBrainsMono-{Regular,Medium,Bold,ExtraBold}.ttf
       EJIT_TALK_PY     dir providing fontTools (optional; site-packages is used if it is installed there)
"""
import importlib
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).parent
FONTS = pathlib.Path(os.environ.get('EJIT_TALK_FONTS', '/tmp/pptools/fonts'))
OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else HERE.parent / 'EJIT_TALK.html')
EXTRA_PY = os.environ.get('EJIT_TALK_PY', '/tmp/pptools/py')
sys.path.insert(0, str(HERE))
if os.path.isdir(EXTRA_PY):
    sys.path.insert(0, EXTRA_PY)
import svgkit  # noqa: E402

BUILD = HERE / '.build'          # subset fonts + char lists (intermediates, not for commit)
BUILD.mkdir(exist_ok=True)

MODS = ['act0', 'act1', 'act2', 'act3', 'act4', 'act5', 'appx']


def collect():
    slides = []
    for m in MODS:
        mod = importlib.import_module(m)
        importlib.reload(mod)
        slides += mod.slides()
    return slides


def wrap(sl):
    act = sl.get('act', 0)
    steps = sl.get('steps', 0)
    attrs = [f'data-act="{act}"', f'data-name="{sl.get("name", "")}"']
    if steps:
        attrs.append(f'data-steps="{steps}"')
    if sl.get('trans'):
        attrs.append(f'data-in="{sl["trans"]}"')
    if sl.get('live'):
        attrs.append(f'data-live="{sl["live"]}"')
    if sl.get('actname'):
        attrs.append(f'data-actname="{sl["actname"]}"')
    cls = 'slide ' + sl.get('cls', '')
    notes = sl.get('notes', '')
    notes_html = f'<aside class="notes">{notes}</aside>' if notes else ''
    if sl.get('html') is not None:
        return f'<section class="{cls}" {" ".join(attrs)}>{sl["html"]}{notes_html}</section>'
    kicker = sl.get('kicker', '')
    kick = f'<div class="kicker">{kicker}</div>' if kicker else ''
    title = f'<h2 class="title">{sl["title"]}</h2>' if sl.get('title') else ''
    sub = f'<p class="sub">{sl["sub"]}</p>' if sl.get('sub') else ''
    head = f'<header class="s-head">{kick}{title}{sub}</header>' if (kick or title or sub) else ''
    return f'<section class="{cls}" {" ".join(attrs)}>{head}<div class="s-body">{sl.get("body", "")}</div>{notes_html}</section>'


def assemble():
    slides = collect()
    css = (HERE / 'base.css').read_text() + (HERE / 'fx.css').read_text()
    js = (HERE / 'deck.js').read_text() + (HERE / 'live.js').read_text()
    body = ''.join(wrap(s) for s in slides)
    html = f'''<!DOCTYPE html>
<html lang="zh-CN" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="color-scheme" content="dark light">
<title>把运行时，变成编译期。· EJIT</title>
<style>
{css}
</style>
</head>
<body>
<div id="viewport" class="graticule">
<div id="stage">
{svgkit.defs()}
<div id="slides">
{body}
</div>
<div class="hud"><div class="hud-brand"><span class="dot"></span>EJIT<span class="hud-act"></span></div><div class="hud-num"></div><div class="hud-tl"></div></div>
<div id="wipe"></div><div id="shut-t"></div><div id="shut-b"></div><div id="flash"></div>
</div>
</div>
<div id="ovhint">点击跳转 · Esc / O 返回</div>
<div id="controls">
<button id="c-prev" title="上一页 (←)"><svg viewBox="0 0 24 24"><path d="M15 6l-6 6 6 6"/></svg></button>
<button id="c-next" title="下一页 (→)"><svg viewBox="0 0 24 24"><path d="M9 6l6 6-6 6"/></svg></button>
<button id="c-ov" title="总览 (O)"><svg viewBox="0 0 24 24"><path d="M4 4h6v6H4zM14 4h6v6h-6zM4 14h6v6H4zM14 14h6v6h-6z"/></svg></button>
<button id="c-nt" title="备注 (N)"><svg viewBox="0 0 24 24"><path d="M5 6h14M5 12h14M5 18h8"/></svg></button>
<button id="c-th" title="主题 (T)"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="8"/><path d="M12 4a8 8 0 0 1 0 16z" fill="currentColor"/></svg></button>
<button id="c-fs" title="全屏 (F)"><svg viewBox="0 0 24 24"><path d="M4 9V4h5M20 9V4h-5M4 15v5h5M20 15v5h-5"/></svg></button>
</div>
<div id="notes"></div>
<div id="toast"></div>
<div id="help" class="overlay"><div class="in"><h2>键盘与手势</h2><div class="keys">
<div class="row"><span>下一页 / 下一步</span><span><kbd>→</kbd><kbd>↓</kbd><kbd>Space</kbd><kbd>Enter</kbd></span></div>
<div class="row"><span>上一页 / 上一步</span><span><kbd>←</kbd><kbd>↑</kbd><kbd>Backspace</kbd></span></div>
<div class="row"><span>总览</span><span><kbd>O</kbd><kbd>Esc</kbd></span></div>
<div class="row"><span>演讲者视图（新窗口）</span><span><kbd>P</kbd></span></div>
<div class="row"><span>备注抽屉</span><span><kbd>N</kbd></span></div>
<div class="row"><span>深色 / 浅色</span><span><kbd>T</kbd></span></div>
<div class="row"><span>全屏</span><span><kbd>F</kbd></span></div>
<div class="row"><span>黑屏 / 白屏</span><span><kbd>B</kbd><kbd>W</kbd></span></div>
<div class="row"><span>动效开关</span><span><kbd>M</kbd></span></div>
<div class="row"><span>计时归零（演讲者视图）</span><span><kbd>R</kbd></span></div>
<div class="row"><span>首页 / 末页</span><span><kbd>Home</kbd><kbd>End</kbd></span></div>
<div class="row"><span>触控</span><span>左右滑动 · 左/右半屏点击</span></div>
<div class="row"><span>打印 / PDF</span><span>Ctrl+P（每页一张，自动展开全部步骤）</span></div>
<div class="row"><span>本帮助</span><span><kbd>?</kbd><kbd>F1</kbd></span></div>
</div><p style="color:var(--ink-3);font-size:14px;margin-top:22px">按 <kbd>Esc</kbd> 关闭</p></div></div>
<div id="pbar"><span class="t">00:00</span><span class="n"></span><span class="sp"></span><button data-cmd="prev">◀ 上一页</button><button data-cmd="next">下一页 ▶</button><button data-cmd="blackout">黑屏</button><button data-cmd="whiteout">白屏</button></div>
<div id="pnotes"></div>
<script>
{js}
window.__deckBoot();
document.querySelectorAll('#pbar button').forEach(b => b.onclick = () => {{
  if (window.opener) window.opener.postMessage({{ ejit: true, type: 'cmd', name: b.dataset.cmd }}, '*');
  if (b.dataset.cmd === 'blackout' || b.dataset.cmd === 'whiteout') {{ document.documentElement.classList.toggle(b.dataset.cmd); }};
}});
</script>
</body>
</html>
'''
    return html, slides


def subset(html):
    import fontTools
    text = set(html) | set(svgkit.SERIF_CHARS)
    # add digits/latin/punct that JS may synthesize (timers, counters) and common CJK fallbacks
    text |= set('0123456789:/.-%×+ ()·—…✓△□✕▶')
    chars = ''.join(sorted(c for c in text if ord(c) >= 0x20 and c not in '\x7f'))
    (BUILD / 'chars.txt').write_text(chars, encoding='utf-8')
    serif = ''.join(sorted(svgkit.SERIF_CHARS | set('0123456789,.:;!?()「」“”‘’—…·×→ ')))
    (BUILD / 'serif_chars.txt').write_text(serif, encoding='utf-8')
    py = sys.executable
    env = {'PYTHONPATH': EXTRA_PY + (os.pathsep + os.environ['PYTHONPATH'] if os.environ.get('PYTHONPATH') else ''), 'PATH': '/usr/bin:/bin'}

    def run(args):
        subprocess.run([py] + args, check=True, env=env, cwd=str(HERE))

    common = ['--flavor=woff2', '--layout-features=*', '--no-hinting', '--desubroutinize', '--name-IDs=1,2', '--notdef-outline']
    outs = {}
    run(['-m', 'fontTools.subset', str(FONTS / 'NotoSansSC.ttf'), f'--text-file={BUILD / "chars.txt"}', '--output-file=' + str(BUILD / 'sans.woff2')] + common)
    serif_src = FONTS / 'NotoSerifSC.ttf'
    if not serif_src.exists():
        serif_src = FONTS / 'NotoSansSC.ttf'
    run(['-m', 'fontTools.subset', str(serif_src), f'--text-file={BUILD / "serif_chars.txt"}', '--output-file=' + str(BUILD / 'serif.woff2')] + common)
    for w, key in (('Regular', 'MONO_R'), ('Medium', 'MONO_M'), ('Bold', 'MONO_B'), ('ExtraBold', 'MONO_X')):
        run(['-m', 'fontTools.subset', str(FONTS / f'JetBrainsMono-{w}.ttf'), f'--text-file={BUILD / "chars.txt"}', '--output-file=' + str(BUILD / f'mono_{w}.woff2')] + common)
        outs[key] = b64(BUILD / f'mono_{w}.woff2')
    outs['SANS'] = b64(BUILD / 'sans.woff2')
    outs['SERIF'] = b64(BUILD / 'serif.woff2')
    return outs, chars


def b64(p):
    import base64
    return base64.b64encode(pathlib.Path(p).read_bytes()).decode('ascii')


def coverage(html, chars):
    """report characters no embedded font can draw (in sans or mono scope)"""
    import fontTools
    from fontTools.ttLib import TTFont
    res = {}
    for name, path in (('sans', BUILD / 'sans.woff2'), ('serif', BUILD / 'serif.woff2'), ('mono', BUILD / 'mono_Regular.woff2')):
        f = TTFont(str(path))
        res[name] = set(f.getBestCmap().keys())
    sans_m = set(c for c in chars if ord(c) not in res['sans'] and ord(c) not in res['mono'])
    serif_missing = set(c for c in svgkit.SERIF_CHARS if ord(c) not in res['serif'])
    return sans_m, serif_missing


def main():
    html, slides = assemble()
    fonts, chars = subset(html)
    for k, v in fonts.items():
        html = html.replace(f'__{k}_B64__', v)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(html, encoding='utf-8')
    miss, smiss = coverage(html, chars)
    sizes = {p.name: p.stat().st_size for p in sorted(BUILD.glob('*.woff2'))}
    print(f'slides={len(slides)}  bytes={len(html.encode())}  fonts={sizes}')
    print('missing glyphs (sans/mono):', ''.join(sorted(miss)) or 'none')
    print('missing glyphs (serif):', ''.join(sorted(smiss)) or 'none')
    print('code blocks (cap, size, est_width_px, lines, maxw):')
    for cw in svgkit.CODE_WIDTHS:
        flag = '  <-- OVER' if cw[4] and cw[2] > cw[4] else ''
        print('   ', cw, flag)
    for i, s in enumerate(slides, 1):
        print(f'{i:02d} act{s.get("act", 0)} steps={s.get("steps", 0)} in={s.get("trans", "-"):7} {s.get("name", "")}')


if __name__ == '__main__':
    main()
