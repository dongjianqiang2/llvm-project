#!/usr/bin/env python3
"""Contact sheets: sheet.py <shots_dir> <out_dir> [slides per sheet] [cols]"""
import sys
import pathlib
from PIL import Image, ImageDraw

src = pathlib.Path(sys.argv[1])
out = pathlib.Path(sys.argv[2])
per = int(sys.argv[3]) if len(sys.argv) > 3 else 4
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 2
only = sys.argv[5].split(',') if len(sys.argv) > 5 else None
out.mkdir(parents=True, exist_ok=True)
for f in out.glob('sheet_*.png'):
    f.unlink()
files = sorted(src.glob('s[0-9][0-9]*.png'), key=lambda p: (int(p.name[1:3]), p.name))
if only:
    files = [f for f in files if f.name[1:3] in only]
W, H = 1600, 900
tw, th = W // cols, H // cols
rows = (per + cols - 1) // cols
for k in range(0, len(files), per):
    chunk = files[k:k + per]
    sheet = Image.new('RGB', (tw * cols, th * rows + 0), (40, 40, 40))
    d = ImageDraw.Draw(sheet)
    for j, f in enumerate(chunk):
        im = Image.open(f).convert('RGB').resize((tw, th), Image.LANCZOS)
        x, y = (j % cols) * tw, (j // cols) * th
        sheet.paste(im, (x, y))
        d.rectangle([x, y, x + 58, y + 20], fill=(0, 0, 0))
        d.text((x + 6, y + 4), f.name[:-4], fill=(255, 220, 80))
    name = out / f"sheet_{chunk[0].name[1:3]}_{chunk[-1].name[1:3]}.png"
    sheet.save(name)
    print(name)
