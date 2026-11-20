# SPDX-License-Identifier: GPL-2.0-only
# Sums the gain from PNG unfolding over one directory: how many files the
# probe's recipe search hits, and how large the raw stream becomes.
import os, sys, zlib, pathlib
from importlib.machinery import SourceFileLoader
m = SourceFileLoader('s', str(pathlib.Path(__file__).with_name('png-probe.py'))).load_module()
files = sorted(pathlib.Path(sys.argv[1]).glob('*.png'))
raw = bytearray(); orig = 0; hits = 0
for p in files:
    b = p.read_bytes(); orig += len(b)
    s = m.idat(p)
    if s:
        r, _ = m.recipe(s)
        if r: raw += zlib.decompress(s); hits += 1; continue
    raw += b
print(f"{hits}/{len(files)} hits, original {orig/1e6:.1f} MB, raw stream {len(raw)/1e6:.1f} MB")
open(os.path.join(os.environ.get('NVPACK_SCRATCH', '/tmp'), 'raw.bin'), 'wb').write(raw)
