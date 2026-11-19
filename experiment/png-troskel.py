#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Var går gränsen för när PNG-utveckling lönar sig?

Hypotes: det avgörande talet är UPPBLÅSNINGEN — hur många gånger större den
utvecklade rådatan är än IDAT-strömmen den låg i. En PNG där deflate redan
klämt ihop 100:1 har inget slack kvar att ta; en där den bara nådde 3:1 är
fotografisk och har massor.

Försöket sorterar ett urval filer i uppblåsningshinkar och packar varje hink
på båda sätten. Tröskeln läses sedan av ur tabellen i stället för att gissas.
"""
import subprocess, sys, tempfile, zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'src'))
import png

HINKAR = [(0, 2), (2, 4), (4, 8), (8, 16), (16, 32), (32, 1 << 30)]


def zstd(sokvag: Path) -> int:
    ut = sokvag.with_suffix('.z')
    subprocess.run(['zstd', '-19', '--long=27', '-T0', '-q', '-f', '-o', str(ut), str(sokvag)], check=True)
    return ut.stat().st_size


def urval(rot: Path, per_katalog=14):
    """Jämnt spridda filer ur varje katalog, så att inget verktyg dominerar."""
    per = {}
    for p in sorted(rot.rglob('*.png')):
        per.setdefault(p.parent, []).append(p)
    ut = []
    for kat, filer in per.items():
        steg = max(1, len(filer) // per_katalog)
        ut += filer[::steg][:per_katalog]
    return ut


def main():
    rot = Path(sys.argv[1])
    filer = urval(rot)
    print(f'{len(filer)} filer i urvalet')
    with tempfile.TemporaryDirectory(dir='/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad') as d:
        d = Path(d)
        strömmar = {}
        for lo, hi in HINKAR:
            strömmar[(lo, hi)] = [open(d / f'orig-{lo}', 'wb'), open(d / f'ra-{lo}', 'wb'), 0]
        missar = 0
        for p in filer:
            r = png.veckla_ut(str(p))
            if not r:
                missar += 1
                continue
            b = p.read_bytes()
            idat = len(b) - len(r['skelett'])
            kvot = len(r['ra']) / max(idat, 1)
            for (lo, hi), (fo, fr, _) in strömmar.items():
                if lo <= kvot < hi:
                    fo.write(b)
                    fr.write(r['skelett']); fr.write(r['ra'])
                    strömmar[(lo, hi)][2] += 1
                    break
        for v in strömmar.values(): v[0].close(); v[1].close()

        print(f'{missar} filer gick inte att utveckla (okänd deflate-motor)\n')
        print(f'{"uppblåsning":<14}{"filer":>6}{"original":>11}{"utvecklat":>11}'
              f'{"zstd19 orig":>13}{"zstd19 utv":>12}{"vinst":>9}')
        for (lo, hi), (_, _, n) in strömmar.items():
            if not n: continue
            o, r = d / f'orig-{lo}', d / f'ra-{lo}'
            zo, zr = zstd(o), zstd(r)
            namn = f'{lo}–{hi}x' if hi < 1 << 30 else f'{lo}x+'
            print(f'{namn:<14}{n:>6}{o.stat().st_size/1e6:>10.1f}M{r.stat().st_size/1e6:>10.1f}M'
                  f'{zo/1e6:>12.2f}M{zr/1e6:>11.2f}M{-100*(zr-zo)/zo:>8.1f}%')


if __name__ == '__main__':
    main()
