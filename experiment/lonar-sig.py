#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Lönar sig PNG-utvecklingen? Jämför original-bytes mot utvecklad-och-ompackad."""
import sys, subprocess, tempfile, time, concurrent.futures as fut
from pathlib import Path
sys.path.insert(0, 'src'); import png

def packa(data: bytes, extra=()):
    with tempfile.TemporaryDirectory() as d:
        i = Path(d)/'i'; i.write_bytes(data)
        subprocess.run(['zstd','-19','--long=27','-T0','-q','-f','-o',str(Path(d)/'o'),str(i),*extra], check=True)
        return (Path(d)/'o').stat().st_size

def main():
  for katalog in sys.argv[1:]:
      filer = sorted(Path(katalog).rglob('*.png'))
      orig = b''.join(p.read_bytes() for p in filer)
      t0=time.monotonic()
      with fut.ProcessPoolExecutor(24) as pool:
          svar = list(pool.map(png.veckla_ut, [str(p) for p in filer], chunksize=2))
      tid = time.monotonic()-t0
      traff = [s for s in svar if s]
      ra = b''.join(s['ra'] for s in traff) + b''.join(s['skelett'] for s in traff)
      miss = b''.join(p.read_bytes() for p,s in zip(filer,svar) if not s)
      a = packa(orig)
      b = packa(ra) + (packa(miss) if miss else 0)
      print(f'{katalog:<28} {len(filer):>4} filer {len(orig)/1e6:>8.1f} MB  traff {len(traff)}/{len(filer)}'
            f'  ra {len(ra)/1e6:>9.1f} MB')
      print(f'{"":28} zstd19 på original {a/1e6:>8.2f} MB   via utveckling {b/1e6:>8.2f} MB'
            f'   {"VINST" if b<a else "FÖRLUST"} {100*(a-b)/a:+.1f}%   utveckling {tid:.1f}s')


if __name__ == '__main__':
    main()
