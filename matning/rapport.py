#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Builds the result tables from resultat/*.json.

Numbers copied by hand go wrong sooner or later — usually when something is
re-measured and only half the text gets updated. Hence this: the tables in
the READMEs always come from the latest measurement.

    python3 matning/rapport.py              # English markdown to stdout
    python3 matning/rapport.py --sv         # Swedish markdown to stdout
    python3 matning/rapport.py --text       # plain text
    python3 matning/rapport.py --in-i-readme
        # inject English tables into README.md and Swedish into README.sv.md
"""
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# Row keys are measurement identifiers stored in resultat/*.json — frozen.
ORDER = ['tar (okomprimerad)', 'tar.gz -6', 'tar.gz -9 (pigz)', 'tar.bz2 -9',
         'tar.zst -3 (dagens)', 'tar.zst -19 --long', 'tar.zst -22 --ultra',
         'tar.xz -9e', 'nvz snabb', 'nvz utan knep', 'nvz normal',
         'nvz liten']
# Display names per language. The Swedish keys stay as the identifiers.
# The tool was renamed nvz -> hz on 2026-11-19; the row keys are frozen
# measurement identifiers and keep the old name, the display does not.
ROW_EN = {'tar (okomprimerad)': 'tar (uncompressed)',
          'tar.zst -3 (dagens)': 'tar.zst -3 (the baseline)',
          'nvz snabb': 'hz fast', 'nvz normal': 'hz normal',
          'nvz utan knep': 'hz, tricks off', 'nvz liten': 'hz small'}
ROW_SV = {'nvz snabb': 'hz snabb', 'nvz normal': 'hz normal',
          'nvz utan knep': 'hz utan knep', 'nvz liten': 'hz liten'}
CORPUS_SV = {'kod': 'Programkod', 'png': 'Bilder (PNG)', 'foto': 'Foton (JPEG)',
             'gif': 'Bilder (GIF)', 'film': 'Film (h264/VP9)',
             'arkiv': 'Arkiv (zip/tar.gz)', 'logg': 'Loggfiler',
             'symbios3d': 'Blandat projektträd (symbios-3d)',
             'silesia': 'Silesia (standardkorpus)',
             'enwik8': 'enwik8 (standardkorpus)'}
CORPUS_EN = {'kod': 'Source code', 'png': 'Images (PNG)', 'foto': 'Photos (JPEG)',
             'gif': 'Images (GIF)', 'film': 'Film (h264/VP9)',
             'arkiv': 'Archives (zip/tar.gz)', 'logg': 'Log files',
             'symbios3d': 'Mixed project tree (symbios-3d)',
             'silesia': 'Silesia (standard corpus)',
             'enwik8': 'enwik8 (standard corpus)'}
HEAD_SV = ('packare', 'storlek MiB', 'kvot', 'mot dagens', 'packtid', 'uppacktid', 'exakt')
HEAD_EN = ('packer', 'size MiB', 'ratio', 'vs baseline', 'pack', 'unpack', 'exact')
MIB = 1048576


def num(x, d=2, sv=True):
    """Swedish numbers use a decimal comma and spaces for thousands;
    English a decimal point and commas."""
    s = f'{x:,.{d}f}'
    return s.replace(',', ' ').replace('.', ',') if sv else s


def load(name):
    f = ROOT / 'resultat' / f'{name}.json'
    return json.loads(f.read_text()) if f.exists() else None


def table(data, sv, md=True):
    raw = data['rådata'] if 'rådata' in data else data['rabyte']
    meas = data['matningar']
    base = meas.get('tar.zst -3 (dagens)', {}).get('byte')
    rows = []
    # The smallest archive and the shortest pack time are bolded, so the
    # column comparison can be made by eye instead of by arithmetic.
    present = [m for name in ORDER if (m := meas.get(name))]
    smallest = min((m['byte'] for m in present), default=None)
    fastest = min((m['packsek'] for m in present), default=None)
    yes, no = ('ja', 'NEJ') if sv else ('yes', 'NO')
    for name in ORDER:
        m = meas.get(name)
        if not m: continue
        vs = (('+' if m['byte'] >= base else '−') +
              num(abs(100*(m['byte']-base)/base), 1, sv) + ' %') if base else '—'
        size = num(m['byte']/MIB, 2, sv)
        if m['byte'] == smallest: size = f'**{size}**'
        pt = f'{m["packsek"]:.0f} s'
        if m['packsek'] == fastest: pt = f'**{pt}**'
        label = ROW_SV.get(name, name) if sv else ROW_EN.get(name, name)
        rows.append((label, size,
                     num(raw/m['byte'], 2, sv) + '×', vs,
                     pt, f'{m["uppacksek"]:.0f} s',
                     yes if m.get('identisk') else no))
    head = HEAD_SV if sv else HEAD_EN
    if md:
        out = ['| ' + ' | '.join(head) + ' |',
               '|' + '|'.join([':--'] + ['--:'] * 5 + [':-:']) + '|']
        for r in rows:
            bold = r[0].startswith('hz')
            out.append('| ' + ' | '.join(f'**{c}**' if bold and i == 0 else c
                                         for i, c in enumerate(r)) + ' |')
        return '\n'.join(out)
    width = [max(len(str(x[i])) for x in [head] + rows) for i in range(7)]
    out = ['  '.join(h.ljust(b) for h, b in zip(head, width))]
    out.append('  '.join('-' * b for b in width))
    for r in rows:
        out.append('  '.join(c.ljust(b) if i == 0 else c.rjust(b)
                             for i, (c, b) in enumerate(zip(r, width))))
    return '\n'.join(out)


def inject(path: Path, text: str, marker='TABELLER'):
    """Replaces everything between the markers in a file with fresh tables."""
    START, END = f'<!-- {marker} -->', f'<!-- SLUT {marker} -->'
    old = path.read_text()
    i, j = old.index(START), old.index(END)
    path.write_text(old[:i + len(START)] + '\n\n' + text + '\n\n' + old[j:])


def build(sv, md=True):
    parts = []
    corpus_names = CORPUS_SV if sv else CORPUS_EN
    files_word = 'filer' if sv else 'files'
    for name, title in corpus_names.items():
        d = load(name)
        if not d: continue
        raw = d['rabyte']
        head = (f'### {title} — {num(raw/MIB, 1, sv)} MiB in {d["filer"]:,} {files_word}'
                if not sv else
                f'### {title} — {num(raw/MIB, 1, sv)} MiB i {d["filer"]:,} {files_word}'
                ).replace(',', ' ') if md else f'{title} — {num(raw/MIB, 1, sv)} MiB'
        parts.append(head + '\n\n' + table(d, sv, md))
    return '\n\n'.join(parts)


def main():
    md = '--text' not in sys.argv
    if '--in-i-readme' in sys.argv:
        inject(ROOT / 'README.md', build(sv=False))
        print('README.md updated (English)')
        sv_readme = ROOT / 'README.sv.md'
        if sv_readme.exists():
            inject(sv_readme, build(sv=True))
            print('README.sv.md updated (Swedish)')
    else:
        print(build(sv='--sv' in sys.argv, md=md))


if __name__ == '__main__':
    main()
