"""Replace same-named slide functions in act*.py with the defs from the given file.
usage: python3 splice.py fix_x.py"""
import glob
import pathlib
import re
import sys

HERE = str(pathlib.Path(__file__).resolve().parent) + '/'
src = open(sys.argv[1], encoding='utf-8').read()
for part in re.split(r'(?m)^(?=def \w+\()', src):
    m = re.match(r'def (\w+)\(', part)
    if not m:
        continue
    name, new = m.group(1), part.rstrip('\n') + '\n'
    for f in sorted(glob.glob(HERE + 'act*.py')):
        s = open(f, encoding='utf-8').read()
        mm = re.search(r'(?ms)^def ' + name + r'\(.*?(?=^def |\Z)', s)
        if mm:
            open(f, 'w', encoding='utf-8').write(s[:mm.start()] + new + '\n\n' + s[mm.end():])
            print('spliced', name, '->', f.rsplit('/', 1)[1])
