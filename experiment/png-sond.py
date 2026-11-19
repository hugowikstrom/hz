#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Sond: gar PNG:ers IDAT-zlibstrom att ateskapa bit-exakt med zlib?"""
import sys, zlib, struct, pathlib, collections

def idat(p):
    b = p.read_bytes()
    if b[:8] != b'\x89PNG\r\n\x1a\n': return None
    i, chunks = 8, []
    while i < len(b):
        ln = struct.unpack('>I', b[i:i+4])[0]; typ = b[i+4:i+8]
        if typ == b'IDAT': chunks.append(b[i+8:i+8+ln])
        i += 12 + ln
        if typ == b'IEND': break
    return b''.join(chunks)

def recept(strom):
    """Hitta (level, wbits, memLevel, strategy) som ger exakt samma bytes."""
    try: rad = zlib.decompress(strom)
    except Exception: return None, 0
    forsok = 0
    for level in (9,6,7,8,5,4,3,2,1):
        for strategy in (0,1,2,3,4):
            for mem in (8,9):
                forsok += 1
                c = zlib.compressobj(level, zlib.DEFLATED, 15, mem, strategy)
                ut = c.compress(rad) + c.flush()
                if ut == strom: return (level,15,mem,strategy), forsok
    return None, forsok

if __name__ == '__main__':
    stat = collections.Counter(); traff = 0; tot = 0; rabytes=0; strombytes=0
    for arg in sys.argv[1:]:
        p = pathlib.Path(arg)
        s = idat(p)
        if not s: continue
        tot += 1
        r, f = recept(s)
        stat[str(r)] += 1
        if r: traff += 1; strombytes += len(s); rabytes += len(zlib.decompress(s))
        print(f"{p.name[:44]:46} {len(s):>9}  {'TRAFF '+str(r) if r else 'MISS'}  ({f} forsok)")
    print(f"\n{traff}/{tot} reproducerbara.  IDAT {strombytes/1e6:.1f} MB -> ra {rabytes/1e6:.1f} MB")
    for k,v in stat.most_common(): print(f"  {v:>4}  {k}")
