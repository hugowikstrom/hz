#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bygger återskapningsprotokollet som rapport ur resultat/protokoll/.

    python3 rapport/protokoll-rapport.py     # skriver rapport/aterskapningsprotokoll.md

Allt i rapporten kommer ur protokoll.json och manifesten — inga tal skrivs
för hand. Manifesten ligger incheckade bredvid, så varje rad går att
kontrollera med `md5sum -c` av vem som helst.
"""
import json
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
P = ROT / 'resultat' / 'protokoll'
MIB = 1048576

NAMN = {'kod': 'Programkod', 'gif': 'Bilder (GIF)', 'foto': 'Foton (JPEG)',
        'arkiv': 'Arkiv (zip/tar.gz)', 'logg': 'Loggfiler', 'png': 'Bilder (PNG)',
        'film': 'Film', 'silesia': 'Silesia', 'enwik8': 'enwik8'}


def tal(x, d=1):
    return f'{x:,.{d}f}'.replace(',', ' ').replace('.', ',')


def main():
    d = json.loads((P / 'protokoll.json').read_text())
    k = d['korpusar']
    alla_ok = all(v['identiska'] and v['md5sum_c_ok'] for v in k.values())

    rader = []
    for namn, v in k.items():
        rader.append(
            f"| {NAMN.get(namn, namn)} | {v['filer']:,} | {tal(v['rabyte']/MIB)} | "
            f"{tal(v['arkivbyte']/MIB)} | `{v['manifest_fore_md5']}` | "
            f"`{v['manifest_efter_md5']}` | "
            f"{'**LIKA**' if v['identiska'] else '**AVVIKER**'} | "
            f"{'ja' if v['md5sum_c_ok'] else 'NEJ'} |".replace(',', ' '))

    exempel = []
    for namn in ('kod', 'foto', 'logg'):
        if namn not in k: continue
        exempel.append(f"**{NAMN[namn]}** — tre första raderna ur "
                       f"[`{namn}-fore.md5`](../resultat/protokoll/{namn}-fore.md5):\n\n```")
        exempel += k[namn]['exempel']
        exempel.append('```\n')

    totfiler = sum(v['filer'] for v in k.values())
    totbyte = sum(v['rabyte'] for v in k.values())

    md = f'''# Återskapningsprotokoll — nvz

**Varje fil md5-summerad före packning och efter uppackning, på
{totfiler:,} filer och {tal(totbyte/MIB/1024, 2)} GiB över {len(k)} korpusar.
Utfall: {"samtliga manifest identiska, och `md5sum -c` bekräftar" if alla_ok
else "AVVIKELSER FUNNA — se tabellen"}.**

*{d['datum']} · {d['maskin']} · {d['nvz']} · läge `liten` (standard) ·
manifesten ligger i [`resultat/protokoll/`](../resultat/protokoll/) och
protokollet genereras av [`prov/protokoll.py`](../prov/protokoll.py)*

## Metod

För varje korpus, i denna ordning:

1. **Manifest före:** md5 beräknas för varje fil och skrivs i
   `md5sum`-format till `<korpus>-fore.md5`.
2. **Packning** med `nvz packa` i standardläget `liten`; arkivets storlek
   och md5 bokförs.
3. **Uppackning** med `nvz packa-upp` till en tom katalog.
4. **Manifest efter:** samma beräkning på det uppackade trädet, till
   `<korpus>-efter.md5`.
5. **Jämförelse** rad för rad — och därtill körs systemets `md5sum -c`
   med *före*-manifestet mot det *uppackade* trädet, så att kontrollen
   görs av ett oberoende verktyg och inte av samma kod som skrev
   manifesten.

Manifestens egen md5 (en summa över alla filsummor) är det som visas i
tabellen: är den lika före och efter är varje enskild fil lika, eftersom
en enda avvikande fil ändrar manifestsumman.

## Protokoll

| korpus | filer | rå MiB | arkiv MiB | manifest-md5 före | manifest-md5 efter | utfall | `md5sum -c` |
|:--|--:|--:|--:|:--|:--|:-:|:-:|
{chr(10).join(rader)}

## Stickprov ur manifesten

Manifesten är vanliga `md5sum`-filer — här är början av tre, ordagrant:

{chr(10).join(exempel)}

## Kontrollera själv

Hela kedjan går att köra om på egen maskin:

```bash
git clone https://github.com/hugowikstrom/nvz && cd nvz && make
python3 korpus/bygg-korpus.py          # eller peka på egna kataloger
python3 prov/protokoll.py
```

Eller granska bara ett befintligt manifest mot ett uppackat träd:

```bash
nvz packa min-katalog arkiv.nv
nvz packa-upp arkiv.nv /tmp/ut
cd /tmp/ut/min-katalog && md5sum -c /sökväg/till/manifest-fore.md5
```

## Noteringar

- md5 används här för att det är formatet folk känner igen och kan köra
  med `md5sum -c`. Packarens egen uppackningskontroll jämför därutöver
  blake2b-128 mot arkivregistret för varje fil, och den tidigare
  rapporten verifierar även rättigheter, tidsstämplar på nanosekunden,
  symlänkar och katalogposter — se
  [bred-provning](bred-provning.md), avsnitt 5.
- Arkiven i protokollet är temporära; deras md5 i `protokoll.json` finns
  för spårbarhet men arkivstorleken är inte deterministisk mellan
  körningar (se bred-provning, avsnitt 2.2).
'''
    ut = ROT / 'rapport' / 'aterskapningsprotokoll.md'
    ut.write_text(md)
    print(f'{ut} skriven ({len(k)} korpusar, alla ok: {alla_ok})')


if __name__ == '__main__':
    main()
