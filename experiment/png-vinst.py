# SPDX-License-Identifier: GPL-2.0-only
import sys, zlib, struct, pathlib, subprocess, tempfile, os
sys.path.insert(0,'.')
from importlib.machinery import SourceFileLoader
m = SourceFileLoader('s','png-sond.py').load_module()
filer = sorted(pathlib.Path(sys.argv[1]).glob('*.png'))
ra = bytearray(); orig = 0; traff=0
for p in filer:
    b = p.read_bytes(); orig += len(b)
    s = m.idat(p)
    if s:
        r,_ = m.recept(s)
        if r: ra += zlib.decompress(s); traff+=1; continue
    ra += b
print(f"{traff}/{len(filer)} traff, original {orig/1e6:.1f} MB, ra-strom {len(ra)/1e6:.1f} MB")
open('/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/ra.bin','wb').write(ra)
