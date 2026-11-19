#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Dominance: is nvz better AND faster than the best established tool?

    python3 experiment/dominans.py [corpus...]

The report's tables compare size and time separately. The harder question
was raised on 2026-11-18: better than the established tools, everywhere —
faster and at least as good. That demands beating the SAME opponent on
both axes at once.

The script measures nothing itself; it reads resultat/<corpus>.json, which
matning/matning.py fills. The opponent per corpus is whichever established
packer produced the smallest archive, with its own measured pack time. Of
nvz's modes, the smallest of those that dominate is counted; if none
dominates, the smallest is still shown, so the row says how close it came
and on which axis it fell.
"""
import json, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPORA = ['kod', 'png', 'foto', 'gif', 'film', 'arkiv', 'logg', 'silesia', 'enwik8']


def main():
    chosen = sys.argv[1:] or CORPORA
    print(f"{'corpus':<10} {'best established':<24} {'bytes':>13} {'sec':>6} | "
          f"{'nvz':<8} {'bytes':>13} {'sec':>6} | outcome")
    dominated = total = 0
    for k in chosen:
        f = ROOT / 'resultat' / f'{k}.json'
        if not f.exists():
            continue
        m = json.loads(f.read_text())['matningar']
        established = {n: v for n, v in m.items() if not n.startswith('nvz')}
        nvz = {n: v for n, v in m.items() if n.startswith('nvz') and n != 'nvz utan knep'}
        if not established or not nvz:
            continue
        bn, bv = min(established.items(), key=lambda kv: kv[1]['byte'])
        bb, bs = bv['byte'], bv['packsek']
        dom = [(v['byte'], v['packsek'], n) for n, v in nvz.items()
               if v['byte'] <= bb and v['packsek'] <= bs]
        cand = [(v['byte'], v['packsek'], n) for n, v in nvz.items()]
        b, t, label = min(dom) if dom else min(cand)
        total += 1
        if dom:
            outcome, dominated = 'DOMINATES', dominated + 1
        elif b <= bb:
            outcome = 'smaller, but slower'
        elif t <= bs:
            outcome = 'faster, but larger'
        else:
            outcome = 'worse on both'
        spaced = lambda n: f'{n:,}'.replace(',', ' ')
        print(f'{k:<10} {bn:<24} {spaced(bb):>13} {bs:>6.0f} | '
              f'{label.replace("nvz ", ""):<8} {spaced(b):>13} {t:>6.0f} | '
              f'{outcome} ({(b-bb)/bb*100:+.2f} % size, {(t-bs)/bs*100:+.0f} % time)')
    print(f'\ndominates on {dominated} of {total} corpora')


if __name__ == '__main__':
    main()
