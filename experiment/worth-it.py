#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Does PNG unfolding pay off? Compares original bytes against unfolded-and-repacked."""
import sys, subprocess, tempfile, time, concurrent.futures as fut
from pathlib import Path
sys.path.insert(0, 'src'); import png


def pack(data: bytes, extra=()):
    with tempfile.TemporaryDirectory() as d:
        i = Path(d)/'i'; i.write_bytes(data)
        subprocess.run(['zstd','-19','--long=27','-T0','-q','-f','-o',str(Path(d)/'o'),str(i),*extra], check=True)
        return (Path(d)/'o').stat().st_size


def main():
    for directory in sys.argv[1:]:
        files = sorted(Path(directory).rglob('*.png'))
        orig = b''.join(p.read_bytes() for p in files)
        t0 = time.monotonic()
        with fut.ProcessPoolExecutor(24) as pool:
            # src/png.py: unfold returns {'recipe', 'rest', 'body'} —
            # recipe, skeleton, raw body.
            answers = list(pool.map(png.unfold, [str(p) for p in files], chunksize=2))
        elapsed = time.monotonic() - t0
        hits = [s for s in answers if s]
        raw = b''.join(s['body'] for s in hits) + b''.join(s['rest'] for s in hits)
        missed = b''.join(p.read_bytes() for p, s in zip(files, answers) if not s)
        a = pack(orig)
        b = pack(raw) + (pack(missed) if missed else 0)
        print(f'{directory:<28} {len(files):>4} files {len(orig)/1e6:>8.1f} MB  hits {len(hits)}/{len(files)}'
              f'  raw {len(raw)/1e6:>9.1f} MB')
        print(f'{"":28} zstd19 on original {a/1e6:>8.2f} MB   via unfolding {b/1e6:>8.2f} MB'
              f'   {"GAIN" if b<a else "LOSS"} {100*(a-b)/a:+.1f}%   unfolding {elapsed:.1f}s')


if __name__ == '__main__':
    main()
