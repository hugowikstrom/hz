#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Probe: can a PNG's IDAT zlib stream be recreated bit for bit with zlib?"""
import sys, zlib, struct, pathlib, collections

def idat(p):
    b = p.read_bytes()
    if b[:8] != b'\x89PNG\r\n\x1a\n': return None
    i, chunks = 8, []
    while i < len(b):
        ln = struct.unpack('>I', b[i:i+4])[0]; ctype = b[i+4:i+8]
        if ctype == b'IDAT': chunks.append(b[i+8:i+8+ln])
        i += 12 + ln
        if ctype == b'IEND': break
    return b''.join(chunks)

def recipe(stream):
    """Find the (level, wbits, memLevel, strategy) that yields exactly the same bytes."""
    try: raw = zlib.decompress(stream)
    except Exception: return None, 0
    attempts = 0
    for level in (9,6,7,8,5,4,3,2,1):
        for strategy in (0,1,2,3,4):
            for mem in (8,9):
                attempts += 1
                c = zlib.compressobj(level, zlib.DEFLATED, 15, mem, strategy)
                out = c.compress(raw) + c.flush()
                if out == stream: return (level,15,mem,strategy), attempts
    return None, attempts

if __name__ == '__main__':
    stats = collections.Counter(); hits = 0; total = 0; raw_bytes=0; stream_bytes=0
    for arg in sys.argv[1:]:
        p = pathlib.Path(arg)
        s = idat(p)
        if not s: continue
        total += 1
        r, n = recipe(s)
        stats[str(r)] += 1
        if r: hits += 1; stream_bytes += len(s); raw_bytes += len(zlib.decompress(s))
        print(f"{p.name[:44]:46} {len(s):>9}  {'HIT '+str(r) if r else 'MISS'}  ({n} attempts)")
    print(f"\n{hits}/{total} reproducible.  IDAT {stream_bytes/1e6:.1f} MB -> raw {raw_bytes/1e6:.1f} MB")
    for k,v in stats.most_common(): print(f"  {v:>4}  {k}")
