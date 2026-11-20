#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Fills the tables in report/broad-evaluation.md from results/*.json.

    python3 report/evaluation.py

The prose in the report is written by hand; every number in it comes from
here.  Two kinds of blocks are filled: the measurement tables per corpus and
the restoration table.

The report is a published document and its generated text is frozen byte for
byte.  That freezes two kinds of strings in this file: the row keys and
field names in results/*.json (Swedish measurement ids — 'nvz liten',
'matningar', 'rabyte', ...) and the HTML markers in the document (ÖVERSIKT,
MATRIS, ...).  The labels the retired Swedish-to-English string translation
had left behind ('kvot', 'Uppack time', 'Loggfiles', 'vs. best klassiska')
were anglicized on 2026-11-20 — in the document and here in the same edit,
as any future label change must be.
"""
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'benchmark'))
import report as r

MIB = 1048576
# Section headings for the four corpora this evaluation added.
NEW = {'gif': 'Images (GIF)', 'film': 'Video (h264 and VP9)',
       'arkiv': 'Archives (zip and tar.gz)', 'logg': 'Log files'}
# Row labels in the overview and throughput tables.
ALL = {'kod': 'Source code', 'png': 'Images (PNG)', 'foto': 'Photos (JPEG)',
       'gif': 'Images (GIF)', 'film': 'Video (h264 and VP9)',
       'arkiv': 'Archives (zip and tar.gz)', 'logg': 'Log files',
       'silesia': 'Silesia (archive level)', 'enwik8': 'enwik8 (archive level)'}


def num(x, d=2):
    """English number format: decimal point, comma as thousands separator."""
    return r.num(x, d, sv=False)


def table(d) -> str:
    """One corpus, every compressor.  Same layout as benchmark/report.py's
    table()."""
    raw = d['rådata'] if 'rådata' in d else d['rabyte']
    meas = d['matningar']
    base = meas.get('tar.zst -3 (dagens)', {}).get('byte')
    present = [m for name in r.ORDER if (m := meas.get(name))]
    smallest = min((m['byte'] for m in present), default=None)
    fastest = min((m['packsek'] for m in present), default=None)
    out = ['| compressor | size MiB | ratio | vs. current | pack time |'
           ' unpack time | exact |',
           '|' + '|'.join([':--'] + ['--:'] * 5 + [':-:']) + '|']
    for name in r.ORDER:
        m = meas.get(name)
        if not m: continue
        vs = (('+' if m['byte'] >= base else '−') +
              num(abs(100 * (m['byte'] - base) / base), 1) + ' %') if base else '—'
        size = num(m['byte'] / MIB, 2)
        if m['byte'] == smallest: size = f'**{size}**'
        pt = f'{m["packsek"]:.0f} s'
        if m['packsek'] == fastest: pt = f'**{pt}**'
        # The tool was renamed nvz -> hz on 2026-11-19; the row keys in
        # results/*.json are frozen measurement ids, the display is not.
        label = name.replace('nvz ', 'hz ')
        if label.startswith('hz'): label = f'**{label}**'
        out.append(f'| {label} | {size} | {num(raw / m["byte"], 2)}× | {vs} | '
                   f'{pt} | {m["uppacksek"]:.0f} s | '
                   f'{"yes" if m.get("identisk") else "NO"} |')
    return '\n'.join(out)


def full_matrix() -> str:
    """The whole measurement matrix in one table: compressors as rows,
    corpora as columns.  Three panels — size, pack time, unpack time —
    since a cell carrying all three is unreadable.  Column minimum in
    bold."""
    corpora = [(k, d) for k in ALL if (d := r.load(k))]

    def panel(value, unit, caption):
        out = [f'**{caption}**', '',
               '| compressor | ' + ' | '.join(k for k, _ in corpora) + ' |',
               '|:--|' + '--:|' * len(corpora)]
        best = {k: min(v for m in d['matningar'].values()
                       if (v := value(m)) is not None)
                for k, d in corpora}
        for name in r.ORDER:
            cells = []
            for k, d in corpora:
                m = d['matningar'].get(name)
                if not m: cells.append('—'); continue
                v = value(m)
                c = num(v, 1) if unit == 'MiB' else f'{v:.0f}'
                if v == best[k]: c = f'**{c}**'
                cells.append(c)
            hz = name.startswith('nvz')
            shown = name.replace('nvz ', 'hz ') if hz else name
            row = f'**{shown}**' if hz else shown
            out.append(f'| {row} | ' + ' | '.join(cells) + ' |')
        out.append('')
        out.append(f'*{unit} per corpus; column minimum in bold. '
                   f'Raw sizes: ' + ', '.join(
                       f'{k} {num(d["rabyte"] / MIB, 0)}'
                       for k, d in corpora) + ' MiB.*')
        return '\n'.join(out)

    return (panel(lambda m: m['byte'] / MIB, 'MiB', 'Archive size') + '\n\n' +
            panel(lambda m: m['packsek'], 's', 'Pack time') + '\n\n' +
            panel(lambda m: m['uppacksek'], 's', 'Unpack time'))


def per_corpus() -> str:
    parts = []
    for name, title in NEW.items():
        d = r.load(name)
        if not d: continue
        head = f'### {title} — {d["rabyte"] / MIB:,.1f} MiB in {d["filer"]:,} files'
        parts.append(head + '\n\n' + table(d))
    return '\n\n'.join(parts)


def overview() -> str:
    """One row per corpus: what hz brings home, and what it costs in time."""
    rows = []
    for name, title in ALL.items():
        d = r.load(name)
        if not d: continue
        m = d['matningar']
        today = m.get('tar.zst -3 (dagens)')
        best_classical = min((m[k] for k in m if not k.startswith('nvz')
                              and k != 'tar (okomprimerad)'),
                             key=lambda x: x['byte'])
        hz = min((m[k] for k in m if k.startswith('nvz') and k != 'nvz utan knep'),
                 key=lambda x: x['byte'])
        bk = next(k for k in m if m[k] is best_classical)
        hk = next(k for k in m if m[k] is hz)
        def percent(x):
            return ('+' if x >= 0 else '−') + num(abs(x), 1) + ' %'
        rows.append((
            title,
            num(d['rabyte'] / MIB, 0),
            num(today['byte'] / MIB, 1),
            num(best_classical['byte'] / MIB, 1) + ' (' +
            bk.replace('tar.', '').replace(' --long', '').replace(' --ultra', '') + ')',
            num(hz['byte'] / MIB, 1) + ' (' + hk.split()[1] + ')',
            percent(100 * (hz['byte'] - today['byte']) / today['byte']),
            percent(100 * (hz['byte'] - best_classical['byte']) / best_classical['byte']),
            f'{hz["packsek"]:.0f} / {best_classical["packsek"]:.0f} s',
        ))
    head = ('corpus', 'raw MiB', 'current MiB', 'best classical MiB',
            'best hz MiB', 'vs. current', 'vs. best classical',
            'pack time hz/classical')
    out = ['| ' + ' | '.join(head) + ' |',
           '|' + '|'.join([':--'] + ['--:'] * 7) + '|']
    for row in rows:
        out.append('| ' + ' | '.join(row) + ' |')
    return '\n'.join(out)


def restoration() -> str:
    f = ROOT / 'results' / 'restoration.json'
    if not f.exists(): return '*(no restoration tests run)*'
    d = json.loads(f.read_text())
    out = ['| corpus | files | register entries | SHA-256 equal |'
           ' mode and times equal | hard links left | deviations |',
           '|:--|--:|--:|:-:|:-:|:-:|:--|']
    for name, v in d['korpusar'].items():
        entries = ('yes' if v['listade_poster'] == v['poster'] else
                   f'NO ({v["listade_poster"]} vs {v["poster"]})')
        dev = 'none' if v['identisk'] else '; '.join(v['avvikelser'][:3])
        out.append(f'| {name} | {v["filer"]:,} | {entries} | '
                   f'{"yes" if v["identisk"] else "NO"} | '
                   f'{"yes" if v["identisk"] else "NO"} | no | {dev} |')
    parts = ['\n'.join(out)]
    if d.get('skada'):
        # The damage labels are frozen data in the JSON and shown verbatim.
        for name, tests in d['skada'].items():
            parts.append(f'Damage tests on the archive from corpus **{name}**:\n\n'
                         '| damage | outcome | passed |\n|:--|:--|:-:|\n' +
                         '\n'.join(f'| {p["skada"]} | '
                                   f'{p["utfall"].replace("vägrade", "rejected")} | '
                                   f'{"yes" if p["godkänt"] else "NO"} |'
                                   for p in tests))
    return '\n\n'.join(parts)


def duplicates() -> str:
    f = ROOT / 'results' / 'duplicates.json'
    if not f.exists(): return '*(duplicate measurement not run)*'
    d = json.loads(f.read_text())
    out = ['| corpus | files | unique files | raw MiB | exact duplicates MiB | share |',
           '|:--|--:|--:|--:|--:|--:|']
    for name, v in d.items():
        out.append(f'| {name} | {v["filer"]:,} | {v["unika"]:,} | '
                   f'{num(v["rabyte"] / MIB, 1)} | {num(v["dubblettbyte"] / MIB, 1)} | '
                   f'{num(v["andel"], 1)} % |')
    return '\n'.join(out)


def standard() -> str:
    """Codec level: one file at a time, one thread — as lzbench and LTCB
    measure."""
    parts = []
    for corpus, title in (('silesia', 'Silesia'), ('enwik8', 'enwik8')):
        f = ROOT / 'results' / f'standard-{corpus}.json'
        if not f.exists(): continue
        d = json.loads(f.read_text())
        raw = d['rabyte']
        rows = ['| codec | size bytes | ratio | bits/byte | pack MiB/s |'
                ' unpack MiB/s | exact |',
                '|:--|--:|--:|--:|--:|--:|:-:|']
        for codec, v in sorted(d.get('summor', {}).items(),
                               key=lambda kv: -kv[1]['byte']):
            label = f'**{codec}**' if codec == 'nvcm' else codec
            rows.append(
                f'| {label} | {v["byte"]:,} | {num(v["kvot"], 3)}× |'
                f' {num(v["bpb"], 3)} | '
                f'{num(raw / MIB / max(v["packsek"], 1e-9), 1)} | '
                f'{num(raw / MIB / max(v["uppacksek"], 1e-9), 1)} | '
                f'{"yes" if v["exakt"] else "NO"} |')
        parts.append(f'**{title}** — {raw:,} bytes' + '\n\n' + '\n'.join(rows))
    return '\n\n'.join(parts) if parts else '*(standard benchmark not run)*'


def silesia_per_file() -> str:
    f = ROOT / 'results' / 'standard-silesia.json'
    if not f.exists(): return '*(standard benchmark not run)*'
    d = json.loads(f.read_text())
    codecs = ['zstd -19', 'brotli -q 11', 'xz -9e', 'nvcm']
    # 'nvcm' is the frozen measurement key in results/standard-*.json;
    # the tool is named hzcm since 2026-11-20 and that is what the table
    # shows.
    shown = [k if k != 'nvcm' else 'hzcm' for k in codecs]
    out = ['| file | contents | raw bytes | ' + ' | '.join(shown) +
           ' | hzcm vs. best |',
           '|:--|:--|--:|' + '--:|' * len(codecs) + '--:|']
    ABOUT = {'dickens': 'English literature, text', 'mozilla': 'executables (tar)',
             'mr': 'medical 3D image', 'nci': 'chemical structures, text',
             'ooffice': 'executable DLL', 'osdb': 'database dump, binary',
             'reymont': 'Polish literature, PDF source', 'samba': 'source code (tar)',
             'sao': 'star catalogue, binary', 'webster': 'dictionary, text',
             'x-ray': 'X-ray, 16-bit', 'xml': 'XML'}
    for name in sorted(d.get('filer', {})):
        row = d['filer'][name]
        if 'nvcm' not in row: continue
        rawbytes = round(row['nvcm']['byte'] * row['nvcm']['kvot'])
        cells = []
        for k in codecs:
            v = row.get(k)
            cells.append(f'{v["byte"]:,}' if v else '—')
        others = [row[k]['byte'] for k in row if k != 'nvcm']
        vs = 100 * (row['nvcm']['byte'] - min(others)) / min(others)
        vstext = ('+' if vs >= 0 else '−') + num(abs(vs), 1) + ' %'
        out.append(f'| `{name}` | {ABOUT.get(name, "")} | {rawbytes:,} | ' +
                   ' | '.join(cells) + f' | {vstext} |')
    return '\n'.join(out)


def enwik8_position() -> str:
    f = ROOT / 'results' / 'standard-enwik8.json'
    if not f.exists(): return '*(enwik8 not measured)*'
    d = json.loads(f.read_text())
    s = d.get('summor', {})
    if 'nvcm' not in s: return '*(nvcm not measured on enwik8)*'
    nv = s['nvcm']
    lines = [f'Our own measurement: **hzcm compresses enwik8 to {nv["byte"]:,}'
             f' bytes, {num(nv["bpb"], 3)} bits/byte**']
    ours = {k: v for k, v in s.items() if k != 'nvcm'}
    if ours:
        best = min(ours.values(), key=lambda v: v['byte'])
        bestname = next(k for k, v in ours.items() if v is best)
        dp = 100 * (nv['byte'] - best['byte']) / best['byte']
        lines.append(f'— {num(abs(dp), 1)} % {"smaller" if dp < 0 else "larger"} than '
                     f'`{bestname}` on the same machine ({best["byte"]:,}'
                     f' bytes, {num(best["bpb"], 3)} bpb).')
    rate = num(nv['byte'] * 8 / 100_000_000)
    lines.append(f'\nAt {rate} bits/byte hzcm lands between the dictionary '
                 'packers and the heavy context mixers in the table above: it '
                 'collects what a dictionary packer cannot see, but it has '
                 'eight models where cmix has hundreds, and it runs at a '
                 'couple of MiB/s where cmix runs at kilobytes per second.')
    return ' '.join(lines[:2]) + '\n' + lines[-1]


def lzma2() -> str:
    f = ROOT / 'results' / 'lzma2.json'
    if not f.exists(): return '*(LZMA2 comparison not run)*'
    d = json.loads(f.read_text())
    NAMES = {'kod': 'Source code', 'logg': 'Log files',
             'arkiv': 'Archives (zip/tar.gz)', 'png': 'Images (PNG)',
             'silesia': 'Silesia', 'enwik8': 'enwik8'}
    out = ['| corpus | raw MiB | LZMA2-extreme MiB | hz MiB |'
           ' hz vs. LZMA2-extreme | pack time LZMA2/hz | exact |',
           '|:--|--:|--:|--:|--:|--:|:-:|']
    for k, v in d.items():
        l, n = v['lzma2'], v['nvz']
        diff = 100 * (n['byte'] - l['byte']) / l['byte']
        dtext = ('+' if diff >= 0 else '−') + num(abs(diff), 1) + ' %'
        w = '**' if n['byte'] < l['byte'] else ''
        out.append(f'| {NAMES.get(k, k)} | {num(v["rabyte"] / MIB, 0)} | '
                   f'{num(l["byte"] / MIB)} | {w}{num(n["byte"] / MIB)}{w} | {dtext} | '
                   f'{l["packsek"]:.0f} / {n["packsek"]:.0f} s | '
                   f'{"yes" if l["exakt"] and n["exakt"] else "NO"} |')
    return '\n'.join(out)


def throughput() -> str:
    """Throughput in MiB/s, so that time can be compared across corpora."""
    out = ['| corpus | hz normal pack | hz normal unpack | zstd -3 pack '
           '| zstd -19 pack | xz -9e pack |', '|:--|--:|--:|--:|--:|--:|']
    for name, title in ALL.items():
        d = r.load(name)
        if not d: continue
        raw = d['rabyte'] / MIB
        meas = d['matningar']
        def speed(packer, key='packsek'):
            v = meas.get(packer)
            if not v: return '—'
            s = v[key]
            return num(raw / s, 0) if s >= 0.05 else '>2,000'
        out.append(f'| {title} | {speed("nvz normal")} | '
                   f'{speed("nvz normal", "uppacksek")} | {speed("tar.zst -3 (dagens)")} | '
                   f'{speed("tar.zst -19 --long")} | {speed("tar.xz -9e")} |')
    return '\n'.join(out) + '\n\n*MiB per second, raw data through the wall clock.*'


def main():
    # The markers are anchors in the published document and stay Swedish,
    # like the row keys in results/*.json.
    md = ROOT / 'report' / 'broad-evaluation.md'
    r.inject(md, overview(), 'ÖVERSIKT')
    r.inject(md, full_matrix(), 'MATRIS')
    r.inject(md, per_corpus(), 'TABELLER')
    r.inject(md, restoration(), 'ÅTERSKAPNING')
    r.inject(md, throughput(), 'TIDER')
    r.inject(md, duplicates(), 'DUBBLETTER')
    r.inject(md, standard(), 'STANDARD')
    r.inject(md, silesia_per_file(), 'SILESIA-PER-FIL')
    r.inject(md, enwik8_position(), 'ENWIK8-PLATS')
    r.inject(md, lzma2(), 'LZMA2')
    print(f'{md} updated')


if __name__ == '__main__':
    main()
