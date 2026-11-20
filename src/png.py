#!/usr/bin/env python3
"""The PNG transform: unfold the deflate shell, and be able to stitch it back.

A PNG is a sequence of chunks whose IDAT contents together form ONE zlib
stream. The stream in turn holds filter rows — raw image data with one
filter byte at the head of each row. Those rows are the interesting part:
they are strongly patterned, and a modern packer that gets to see several
images' rows in the same block finds similarities deflate never could.

The problem is the way back. An archive must not hand back "a PNG with the
same picture" — it must hand back exactly the same bytes, or checksums,
signatures and git break. Deflate is not a canonical encoding: the same
input yields different output depending on level, memory level and
strategy. So we hunt for the RECIPE — the set of zlib parameters that
happens to produce precisely the stream the file already has — and store
it. If no recipe is found the file is stored raw.

The search is cheaper than it looks:

  * the first two bytes of the zlib header give away the window size (CMF)
    and roughly which level group the encoder used (FLEVEL in FLG). That
    cuts away most of it.
  * every candidate is compared INCREMENTALLY. Wrong parameters almost
    always diverge within the first few hundred kilobytes, and then the
    attempt is aborted at once instead of packing the whole image for
    nothing.
  * the recipe that worked most recently is tried first. Images out of the
    same tool share a recipe in practice, so from image two on the first
    guess is right.
"""
from __future__ import annotations

import struct, zlib

SIGNATURE = b'\x89PNG\r\n\x1a\n'
STEP = 1 << 18
_recent = []           # recipes that worked in this process, newest first


def _chunks(b: bytes):
    """Walks the PNG chunks. Returns None if the file is not well formed."""
    if b[:8] != SIGNATURE: return None
    i, out = 8, []
    n = len(b)
    while i + 8 <= n:
        (ln,) = struct.unpack('>I', b[i:i+4])
        kind = b[i+4:i+8]
        if i + 12 + ln > n: return None
        out.append((i, ln, kind))
        i += 12 + ln
        if kind == b'IEND': break
    if i != n: return None            # trailing bytes — store raw instead
    return out


def _candidates(stream: bytes):
    """Recipe candidates in the order they are worth trying."""
    cmf, flg = stream[0], stream[1]
    if (cmf & 0x0f) != 8 or (flg & 0x20): return []      # not deflate, or FDICT
    wbits = (cmf >> 4) + 8
    if not 9 <= wbits <= 15: return []
    flevel = flg >> 6
    group = {0: (1,), 1: (2,3,4,5), 2: (6,), 3: (7,8,9)}[flevel]
    # zlib derives FLEVEL from the level, so the group is a statement, not a
    # guess. Within the group: 9 and 6 are what tools actually use.
    levels = sorted(group, key=lambda n: (n not in (9, 6), -n))
    cand = [(n, wbits, m, s) for n in levels for m in (8, 9) for s in (0, 1, 2, 3, 4)]
    first = [r for r in _recent if r in cand]
    return first + [r for r in cand if r not in first]


def _matches(raw: bytes, target: bytes, recipe) -> bool:
    """Repacks the raw data and compares incrementally — aborts at the first divergence."""
    level, wbits, mem, strategy = recipe
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, mem, strategy)
    view = memoryview(target)
    out = bytearray()
    for i in range(0, len(raw), STEP):
        out += c.compress(raw[i:i+STEP])
        if len(out) > len(target) or view[:len(out)] != memoryview(out):
            return False
    out += c.flush()
    return len(out) == len(target) and view == memoryview(out)


def _redeflate(raw: bytes, recipe) -> bytes:
    level, wbits, mem, strategy = recipe
    c = zlib.compressobj(level, zlib.DEFLATED, wbits, mem, strategy)
    return c.compress(raw) + c.flush()


def unfold(path: str, inflation_cap: float = 1e9):
    """Returns {'recipe', 'rest', 'body'} or None if the file should be stored raw.

    `inflation_cap` is how many times larger the unfolded raw data may be
    than the IDAT stream for the unfold to be worth the trouble. A PNG that
    deflate already squeezed 50:1 is a screenshot with large flat areas;
    there is nothing left to take there, only pack time to lose.

    The result is always proved by stitching it back together and comparing
    with the original. What is returned is thereby proven reproducible.
    """
    with open(path, 'rb') as f:
        b = f.read()
    chunks = _chunks(b)
    if not chunks: return None
    idat = [(i, ln) for i, ln, kind in chunks if kind == b'IDAT']
    if not idat or sum(ln for _, ln in idat) < 512: return None   # too small to be worth anything

    stream = b''.join(b[i+8:i+8+ln] for i, ln in idat)
    try:
        raw = zlib.decompress(stream)
    except zlib.error:
        return None
    if len(raw) > len(stream) * inflation_cap:
        return None

    recipe = None
    for k in _candidates(stream):
        if _matches(raw, stream, k):
            recipe = k; break
    if recipe is None: return None
    if recipe in _recent: _recent.remove(recipe)
    _recent.insert(0, recipe); del _recent[4:]

    # The skeleton is the file without IDAT contents. Everything else —
    # signature, all other chunks, lengths and checksums — comes along
    # verbatim, so the stitching needs to know nothing about PNG semantics.
    skeleton = bytearray()
    last = 0
    for i, ln in idat:
        skeleton += b[last:i+8]
        last = i + 8 + ln
    skeleton += b[last:]

    if rebuild(bytes(skeleton), raw, recipe) != b:
        return None                                   # should not happen; store it raw then
    return dict(recipe=list(recipe), rest=bytes(skeleton), body=raw)


def rebuild(skeleton: bytes, raw: bytes, recipe) -> bytes:
    """Stitches the PNG back together: deflate the raw data with the recipe and fill in IDAT."""
    stream = _redeflate(raw, tuple(recipe))
    out = bytearray(skeleton[:8])
    i, j = 8, 0                         # i = read position in the skeleton, j = in the stream
    n = len(skeleton)
    while i + 8 <= n:
        head = skeleton[i:i+8]
        (ln,) = struct.unpack('>I', head[:4])
        kind = head[4:8]
        out += head
        if kind == b'IDAT':
            out += stream[j:j+ln]; j += ln
            out += skeleton[i+8:i+12]   # the checksum stayed behind in the skeleton
            i += 12
        else:
            out += skeleton[i+8:i+12+ln]
            i += 12 + ln
        if kind == b'IEND': break
    out += skeleton[i:]
    return bytes(out)
