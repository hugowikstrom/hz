#!/usr/bin/env python3
"""The JPEG transform: the shell around the nvjpg binary.

nvjpg does the work (see hzjpg.c). Here its output is split into two parts
that want different treatment in the archive:

    REST   — the JPEG header verbatim: markers, quantization tables,
             Huffman tables and EXIF. A hundred images out of the same
             camera have near-identical headers, so they are packed
             together with zstd and shrink to almost nothing.

    BODY   — the arithmetically coded coefficient stream. It is already as
             close to random as a byte stream gets; pushing it through yet
             another compressor is wasted time. It is stored raw.

Every transform is proved in both directions before it is accepted. The
cost is that packing does roughly double work on photos; the price is
worth it, because the alternative is an archive that looks done and isn't.
"""
from __future__ import annotations

import os, shutil, struct, subprocess
from pathlib import Path

def _find(name: str) -> str:
    neighbor = Path(__file__).resolve().parent / name
    if neighbor.is_file() and os.access(neighbor, os.X_OK):
        return str(neighbor)
    return shutil.which(name) or str(neighbor)


NVJPG = _find('nvjpg')


def unfold(path: str):
    """Returns {'rest', 'body'} or None if the file should be stored raw."""
    with open(path, 'rb') as f:
        orig = f.read()
    r = subprocess.run([NVJPG, 'pack'], input=orig, capture_output=True)
    if r.returncode != 0 or r.stdout[:4] != b'NVJ1':
        return None                       # progressive, broken, or not recodable
    d = r.stdout
    head_len, tail_len, _scan_len = struct.unpack('<III', d[4:16])
    cut = 18 + head_len + tail_len
    if len(d) >= len(orig):
        return None                       # no gain — store raw
    rest, body = d[:cut], d[cut:]
    if rebuild(rest, body) != orig:
        return None                       # should not happen; store it raw then
    return dict(rest=rest, body=body)


def rebuild(rest: bytes, body: bytes) -> bytes:
    r = subprocess.run([NVJPG, 'unpack'], input=rest + body, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('nvjpg unpack: ' + r.stderr.decode()[:300])
    return r.stdout
