#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Where does the threshold lie for PNG unfolding to pay off?

Hypothesis: the decisive number is the INFLATION — how many times larger
the unfolded raw data is than the IDAT stream it sat in. A PNG where
deflate has already squeezed 100:1 has no slack left to take; one where it
only reached 3:1 is photographic and has plenty.

The experiment sorts a selection of files into inflation buckets and packs
each bucket both ways. The threshold is then read off the table instead of
guessed.
"""
import subprocess, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'src'))
import png

BUCKETS = [(0, 2), (2, 4), (4, 8), (8, 16), (16, 32), (32, 1 << 30)]


def zstd(path: Path) -> int:
    out = path.with_suffix('.z')
    subprocess.run(['zstd', '-19', '--long=27', '-T0', '-q', '-f', '-o', str(out), str(path)], check=True)
    return out.stat().st_size


def selection(top: Path, per_dir=14):
    """Evenly spread files from every directory, so that no one tool dominates."""
    per = {}
    for p in sorted(top.rglob('*.png')):
        per.setdefault(p.parent, []).append(p)
    out = []
    for _, files in per.items():
        step = max(1, len(files) // per_dir)
        out += files[::step][:per_dir]
    return out


def main():
    top = Path(sys.argv[1])
    files = selection(top)
    print(f'{len(files)} files in the selection')
    with tempfile.TemporaryDirectory() as d:
        d = Path(d)
        streams = {}
        for lo, hi in BUCKETS:
            streams[(lo, hi)] = [open(d / f'orig-{lo}', 'wb'), open(d / f'raw-{lo}', 'wb'), 0]
        misses = 0
        for p in files:
            # src/png.py: unfold returns {'recipe', 'rest', 'body'} —
            # recipe, skeleton, raw body.
            r = png.unfold(str(p))
            if not r:
                misses += 1
                continue
            b = p.read_bytes()
            idat = len(b) - len(r['rest'])
            ratio = len(r['body']) / max(idat, 1)
            for (lo, hi), (fo, fr, _) in streams.items():
                if lo <= ratio < hi:
                    fo.write(b)
                    fr.write(r['rest']); fr.write(r['body'])
                    streams[(lo, hi)][2] += 1
                    break
        for v in streams.values(): v[0].close(); v[1].close()

        print(f'{misses} files could not be unfolded (unknown deflate engine)\n')
        print(f'{"inflation":<14}{"files":>6}{"original":>11}{"unfolded":>11}'
              f'{"zstd19 orig":>13}{"zstd19 unf":>12}{"gain":>9}')
        for (lo, hi), (_, _, n) in streams.items():
            if not n: continue
            o, r = d / f'orig-{lo}', d / f'raw-{lo}'
            zo, zr = zstd(o), zstd(r)
            name = f'{lo}–{hi}x' if hi < 1 << 30 else f'{lo}x+'
            print(f'{name:<14}{n:>6}{o.stat().st_size/1e6:>10.1f}M{r.stat().st_size/1e6:>10.1f}M'
                  f'{zo/1e6:>12.2f}M{zr/1e6:>11.2f}M{-100*(zr-zo)/zo:>8.1f}%')


if __name__ == '__main__':
    main()
