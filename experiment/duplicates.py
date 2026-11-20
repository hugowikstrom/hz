#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""How much of each corpus is exact duplicates?

    python3 experiment/duplicates.py        # writes results/duplicates.json

The number is needed to read the tables right. nvz has two kinds of win
that look the same in a size column: the dedup, which removes bytes that
exist twice, and the compression, which makes the remaining bytes smaller.
A corpus where half the content is copies yields an impressive ratio
without the packer having done anything at all — and conversely: on a
corpus without duplicates the row 'nvz utan knep' is identical to
'nvz normal' for a reason.
"""
import hashlib, json
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPORA = ('kod', 'png', 'foto', 'gif', 'film', 'arkiv', 'logg', 'silesia', 'enwik8')

out = {}
for name in CORPORA:
    top = ROOT / 'corpus' / name
    if not top.is_dir(): continue
    seen, raw_bytes, duplicated, files = set(), 0, 0, 0
    for p in sorted(top.rglob('*')):
        if not p.is_file() or p.is_symlink(): continue
        h = hashlib.blake2b(digest_size=16)
        with open(p, 'rb') as f:
            for piece in iter(lambda: f.read(1 << 20), b''): h.update(piece)
        s = p.stat().st_size
        raw_bytes += s; files += 1
        if h.digest() in seen: duplicated += s
        else: seen.add(h.digest())
    # The JSON keys are frozen: report/evaluation.py reads them by name.
    out[name] = dict(filer=files, rabyte=raw_bytes, dubblettbyte=duplicated,
                     andel=round(100 * duplicated / max(raw_bytes, 1), 1), unika=len(seen))
    print(f'{name:<8} {files:>6} files  {raw_bytes/1048576:>8.1f} MiB  '
          f'duplicates {duplicated/1048576:>7.1f} MiB ({out[name]["andel"]} %)', flush=True)

(ROOT / 'results' / 'duplicates.json').write_text(json.dumps(out, indent=1, ensure_ascii=False))
