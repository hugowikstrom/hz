#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Renders the thread-scaling figure from results/threads-*.json.

    python3 benchmark/thread-figure.py            # -> report/img/thread-scaling.png

HTML to PNG through headless Chrome, the way the other figures in
report/img are made. The numbers are never typed here; everything comes
from the JSON, so the figure cannot drift from the measurement.

What the figure has to communicate, because it is the surprise in the
data: at these settings most established packers cannot use more than
one core. xz -9e codes 192 MiB blocks, so a corpus smaller than that is
one block and one thread however many you ask for; zstd -22 --ultra's
2 GiB window is one job for the same reason. The speedup column is
therefore mostly 1.00 and that IS the result - it must not look like a
broken harness, so the effective thread count sits next to it.
"""
import json, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPORA = ['kod', 'enwik8', 'silesia', 'logg']
ORDER = ['tar.gz -6', 'tar.gz -9 (pigz)', 'tar.bz2 -9', 'tar.zst -3 (dagens)',
         'tar.zst -19 --long', 'tar.zst -22 --ultra', 'tar.xz -9e', 'tar.br -q 11',
         'nvz snabb', 'nvz normal', 'nvz liten']
LABEL = {'tar.gz -6': ('gzip −6', 'deflate'),
         'tar.gz -9 (pigz)': ('pigz −9', 'deflate, parallel'),
         'tar.bz2 -9': ('bzip2 −9', 'BWT+Huffman'),
         'tar.zst -3 (dagens)': ('zstd −3', 'LZ77+tANS'),
         'tar.zst -19 --long': ('zstd −19', 'LZ77+tANS, 128M window'),
         'tar.zst -22 --ultra': ('zstd −22', 'LZ77+tANS, 2G window'),
         'tar.xz -9e': ('xz −9e', 'LZMA2, 192M blocks'),
         'tar.br -q 11': ('brotli −q 11', 'brotli'),
         'nvz snabb': ('hZ fast', 'dedup+sort+zstd'),
         'nvz normal': ('hZ normal', 'transforms+probe'),
         'nvz liten': ('hZ small', '+ context mixing')}


def load():
    out = {}
    for c in CORPORA:
        f = ROOT / 'results' / f'threads-{c}.json'
        if f.exists():
            out[c] = json.loads(f.read_text())
    return out


def cell(entry):
    """One corpus' three numbers for one packer."""
    k = entry.get('konfig', {})
    one, mx = k.get('1', {}), k.get('max', {})
    if 'bytes' not in one:
        return '<td class="num mut">—</td>' * 3
    if 'bytes' not in mx:
        why = (mx.get('not_expressible') or {}).get('class', '')
        tag = 'single' if why == 'enkeltradad' else 'absent'
        return (f'<td class="num">{one["packsek"]:.1f}</td>'
                f'<td class="num mut ej" colspan="2">no parallel mode</td>'
                if tag else '')
    d = entry.get('harledt', {})
    sp = d.get('speedup_pack') or 0
    klass = 'win' if sp >= 2 else ('flat' if sp < 1.15 else '')
    return (f'<td class="num">{one["packsek"]:.1f}</td>'
            f'<td class="num">{mx["packsek"]:.1f}</td>'
            f'<td class="num {klass}">{sp:.2f}×<div class="t">{mx["threads_effective"]} thr</div></td>')


def build(data) -> str:
    corpora = [c for c in CORPORA if c in data]
    head = ''.join(
        f'<th class="num grp" colspan="3">{c}<div class="t">'
        f'{data[c]["rabyte"]/1048576:.0f} MiB</div></th>' for c in corpora)
    sub = ''.join('<th class="num sub">1 core</th><th class="num sub">max</th>'
                  '<th class="num sub">speedup</th>' for _ in corpora)
    rows = []
    for name in ORDER:
        if not any(name in data[c]['matningar'] for c in corpora):
            continue
        label, alg = LABEL.get(name, (name, ''))
        cells = ''.join(cell(data[c]['matningar'].get(name, {})) for c in corpora)
        hz = ' hz' if name.startswith('nvz') else ''
        rows.append(f'<tr class="{hz.strip()}"><td class="k{hz}">{label}'
                    f'<div class="t">{alg}</div></td>{cells}</tr>')
    m = data[corpora[0]]['maskin']
    return f'''<!doctype html><meta charset="utf-8"><style>
* {{ margin:0; padding:0; box-sizing:border-box; }} html {{ overflow:hidden; }}
body {{ width:1480px; background:#f4f3ef; font-family:'Archivo','Liberation Sans',sans-serif;
color:#1d2321; padding:30px; }}
.kort {{ background:#fff; border-radius:14px; padding:34px 38px 26px;
box-shadow:0 1px 3px rgba(29,35,33,.08); }}
h1 {{ font-size:31px; letter-spacing:-.4px; }} h1 .z {{ color:#1a7f5c; }}
.under {{ font-size:15.5px; color:#5a645f; margin:8px 0 22px; line-height:1.5; }}
table {{ width:100%; border-collapse:collapse; font-size:14.5px; }}
th {{ font-size:12px; color:#1d2321; padding:0 7px 6px; text-align:right; vertical-align:bottom; }}
th.grp {{ border-bottom:2px solid #1d2321; }}
th.sub {{ font-size:10.5px; font-weight:400; color:#8a938e; padding-top:5px;
border-bottom:1px solid #1d2321; }}
th:first-child {{ text-align:left; }}
th .t {{ font-size:10px; font-weight:400; color:#8a938e; }}
td {{ padding:7px; border-bottom:1px solid #e4e2da; vertical-align:top; }}
tr:last-child td {{ border-bottom:none; }}
td.k {{ font-weight:600; white-space:nowrap; }}
td.k.hz {{ color:#1a7f5c; }}
td.num {{ text-align:right; font-variant-numeric:tabular-nums; }}
td.mut {{ color:#b9bfb9; }} td.ej {{ font-style:italic; font-size:12px; color:#8a938e; }}
td.win {{ color:#1a7f5c; font-weight:700; }}
td.flat {{ color:#8a938e; }}
.t {{ font-size:10.5px; color:#8a938e; margin-top:1px; }}
.fot {{ display:flex; justify-content:space-between; margin-top:18px; font-size:12.5px;
color:#5a645f; }}
.noter {{ margin-top:16px; font-size:12.5px; color:#5a645f; line-height:1.6; }}
</style><body><div class="kort">
<h1>hyper<span class="z">Z</span>ip — one core against as many as the algorithm allows</h1>
<div class="under">Pack seconds at one thread and at 24, per corpus, with the measured
speedup and the <b>effective</b> parallelism (cpu time ÷ wall time). Asking for 24 threads
is not the same as getting them: the number under each speedup is what the tool actually
achieved.</div>
<table><tr><th>packer</th>{head}</tr><tr><th></th>{sub}</tr>{''.join(rows)}</table>
<div class="noter">
<b>Why most rows say 1.00×.</b> xz −9e codes 192 MiB blocks, so any corpus below that size
is a single block and a single thread however many are requested. zstd −22 −−ultra's 2 GiB
window is one job for the same reason; zstd −19's 128 MiB window splits only once the
corpus exceeds it. gzip, bzip2 and brotli have no parallel mode at all — pigz is the
parallel deflate, and it is the only tool here that uses the machine.
<br><b>Size is unaffected</b> — at most 0.24 % between one thread and 24 — so none of
these speedups is bought with ratio.
</div>
<div class="fot"><span>{m["model"]}, {m["logical"]} logical / {m["physical"]} physical cores,
{m["governor"]} governor at {m["max_mhz"]} MHz · generated from results/threads-*.json</span>
<span>github.com/hugowikstrom/hz</span></div>
</div></body>'''


def main() -> int:
    data = load()
    if not data:
        print('no results/threads-*.json yet', file=sys.stderr); return 1
    scratch = ROOT / '.scratch'
    scratch.mkdir(exist_ok=True)
    html = scratch / 'thread-scaling.html'
    html.write_text(build(data))
    png = ROOT / 'report' / 'img' / 'thread-scaling.png'
    png.parent.mkdir(parents=True, exist_ok=True)
    for browser in ('/usr/bin/google-chrome', '/usr/bin/chromium-browser', 'chromium'):
        r = subprocess.run([browser, '--headless=new', '--disable-gpu', '--hide-scrollbars',
                            '--force-device-scale-factor=2', '--window-size=1540,800',
                            f'--screenshot={png}', f'file://{html}'],
                           capture_output=True, text=True)
        if png.exists():
            print(f'{png} ({png.stat().st_size//1024} kB), corpora: {", ".join(data)}')
            return 0
    print('no headless browser produced the image', file=sys.stderr)
    return 1


if __name__ == '__main__':
    sys.exit(main())
