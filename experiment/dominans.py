#!/usr/bin/env python3
"""Dominans: är nvz bättre OCH snabbare än det bästa etablerade verktyget?

    python3 experiment/dominans.py [korpus...]

Rapportens tabeller jämför storlek och tid var för sig. Den hårdare frågan
restes 2026-11-18: *bättre än det etablerade, överallt — snabbare och minst
lika bra.* Den kräver att nvz slår SAMMA motståndare på båda axlarna
samtidigt.

Skriptet mäter ingenting självt; det läser resultat/<korpus>.json, som
matning/matning.py fyller. Motståndaren per korpus är den av de etablerade
packarna som gav minsta arkiv, med sin egen uppmätta packtid. Av nvz:s lägen
räknas det minsta av dem som dominerar; dominerar inget läge redovisas ändå
det minsta, så raden visar hur nära det kom och på vilken axel det föll.
"""
import json, sys
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
KORPUSAR = ['kod', 'png', 'foto', 'gif', 'film', 'arkiv', 'logg', 'silesia', 'enwik8']


def main():
    valda = sys.argv[1:] or KORPUSAR
    print(f"{'korpus':<10} {'bästa etablerade':<24} {'byte':>13} {'sek':>6} | "
          f"{'nvz':<8} {'byte':>13} {'sek':>6} | utfall")
    dom_antal = tot = 0
    for k in valda:
        f = ROT / 'resultat' / f'{k}.json'
        if not f.exists():
            continue
        m = json.loads(f.read_text())['matningar']
        etabl = {n: v for n, v in m.items() if not n.startswith('nvz')}
        nvz = {n: v for n, v in m.items() if n.startswith('nvz') and n != 'nvz utan knep'}
        if not etabl or not nvz:
            continue
        mn, mv = min(etabl.items(), key=lambda kv: kv[1]['byte'])
        mb, ms = mv['byte'], mv['packsek']
        kand = [(v['byte'], v['packsek'], n) for n, v in nvz.items()]
        dom = [x for x in kand if x[0] <= mb and x[1] <= ms]
        b, t, l = min(dom) if dom else min(kand)
        tot += 1
        if dom:
            utfall, dom_antal = 'DOMINERAR', dom_antal + 1
        elif b <= mb:
            utfall = 'mindre, men långsammare'
        elif t <= ms:
            utfall = 'snabbare, men större'
        else:
            utfall = 'sämre på båda'
        mellanslag = lambda n: f'{n:,}'.replace(',', ' ')
        print(f'{k:<10} {mn:<24} {mellanslag(mb):>13} {ms:>6.0f} | '
              f'{l.replace("nvz ", ""):<8} {mellanslag(b):>13} {t:>6.0f} | '
              f'{utfall} ({(b-mb)/mb*100:+.2f} % storlek, {(t-ms)/ms*100:+.0f} % tid)')
    print(f'\ndominerar på {dom_antal} av {tot} korpusar')


if __name__ == '__main__':
    main()
