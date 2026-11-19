#!/usr/bin/env python3
# [EN] Counts duplicate bytes per corpus — the numbers behind the dedup rows.
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
"""Hur stor del av varje korpus är exakta dubbletter?

    python3 experiment/dubbletter.py        # skriver resultat/dubbletter.json

Talet behövs för att läsa tabellerna rätt. nvz har två sorters vinst som
ser likadana ut i en storlekskolumn: dedupen, som tar bort bytes som finns
två gånger, och komprimeringen, som gör kvarvarande bytes mindre. En korpus
där halva innehållet är kopior ger en imponerande kvot utan att packaren
gjort något alls — och tvärtom: på en korpus utan dubbletter är raden
'nvz utan knep' identisk med 'nvz normal' av en anledning.
"""
import hashlib, json
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
KORPUSAR = ('kod', 'png', 'foto', 'gif', 'film', 'arkiv', 'logg', 'silesia', 'enwik8')

ut = {}
for namn in KORPUSAR:
    rot = ROT / 'korpus' / namn
    if not rot.is_dir(): continue
    sedda, rabyte, dubbelt, filer = set(), 0, 0, 0
    for p in sorted(rot.rglob('*')):
        if not p.is_file() or p.is_symlink(): continue
        h = hashlib.blake2b(digest_size=16)
        with open(p, 'rb') as f:
            for bit in iter(lambda: f.read(1 << 20), b''): h.update(bit)
        s = p.stat().st_size
        rabyte += s; filer += 1
        if h.digest() in sedda: dubbelt += s
        else: sedda.add(h.digest())
    ut[namn] = dict(filer=filer, rabyte=rabyte, dubblettbyte=dubbelt,
                    andel=round(100 * dubbelt / max(rabyte, 1), 1), unika=len(sedda))
    print(f'{namn:<8} {filer:>6} filer  {rabyte/1048576:>8.1f} MiB  '
          f'dubbletter {dubbelt/1048576:>7.1f} MiB ({ut[namn]["andel"]} %)', flush=True)

(ROT / 'resultat' / 'dubbletter.json').write_text(json.dumps(ut, indent=1, ensure_ascii=False))
