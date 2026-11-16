#!/usr/bin/env python3
"""Fyller tabellerna i rapport/bred-provning.md ur resultat/*.json.

    python3 rapport/bred-provning.py

Prosan i rapporten är skriven för hand; varje tal i den kommer härifrån.
Två block fylls: mättabellerna per korpus och återskapningstabellen.
"""
import json, sys
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROT / 'matning'))
import rapport as r

MIB = 1048576
NYA = {'gif': 'Bilder (GIF)', 'film': 'Film (h264 och VP9)',
       'arkiv': 'Arkiv (zip och tar.gz)', 'logg': 'Loggfiler'}
ALLA = {'kod': 'Programkod', 'png': 'Bilder (PNG)', 'foto': 'Foton (JPEG)', **NYA}


def mattabeller() -> str:
    delar = []
    for namn, rubrik in NYA.items():
        d = r.läs(namn)
        if not d: continue
        rå = d['rabyte']
        huvud = (f'### {rubrik} — {rå/MIB:,.1f} MiB i {d["filer"]:,} filer'
                 .replace(',', ' '))
        delar.append(huvud + '\n\n' + r.tabell(d))
    return '\n\n'.join(delar)


def oversikt() -> str:
    """En rad per korpus: vad nvz tar hem, och vad det kostar i tid."""
    rader = []
    for namn, rubrik in ALLA.items():
        d = r.läs(namn)
        if not d: continue
        m = d['matningar']
        idag = m.get('tar.zst -3 (dagens)')
        bast_klassisk = min((m[k] for k in m if not k.startswith('nvz')
                             and k != 'tar (okomprimerad)'), key=lambda x: x['byte'])
        nv = min((m[k] for k in m if k.startswith('nvz') and k != 'nvz utan knep'),
                 key=lambda x: x['byte'])
        bk = next(k for k in m if m[k] is bast_klassisk)
        nk = next(k for k in m if m[k] is nv)
        rader.append((
            rubrik,
            f'{d["rabyte"]/MIB:,.0f}'.replace(',', ' '),
            f'{idag["byte"]/MIB:,.1f}'.replace(',', ' '),
            f'{bast_klassisk["byte"]/MIB:,.1f} ({bk.replace("tar.", "").replace(" --long", "").replace(" --ultra", "")})'.replace(',', ' '),
            f'{nv["byte"]/MIB:,.1f} ({nk.split()[1]})'.replace(',', ' '),
            f'{100*(nv["byte"]-idag["byte"])/idag["byte"]:+.1f} %',
            f'{100*(nv["byte"]-bast_klassisk["byte"])/bast_klassisk["byte"]:+.1f} %',
            f'{nv["packsek"]:.0f} / {bast_klassisk["packsek"]:.0f} s',
        ))
    rubrik = ('korpus', 'rå MiB', 'dagens MiB', 'bästa klassiska MiB', 'bästa nvz MiB',
              'mot dagens', 'mot bästa klassiska', 'packtid nvz/klassisk')
    ut = ['| ' + ' | '.join(rubrik) + ' |',
          '|' + '|'.join([':--'] + ['--:'] * 7) + '|']
    for rad in rader:
        ut.append('| ' + ' | '.join(rad) + ' |')
    return '\n'.join(ut)


def aterskapning() -> str:
    f = ROT / 'resultat' / 'aterskapning.json'
    if not f.exists(): return '*(inga återskapningsprov körda)*'
    d = json.loads(f.read_text())
    ut = ['| korpus | filer | poster i registret | SHA-256 lika | läge och tider lika'
          ' | hårda länkar kvar | avvikelser |',
          '|:--|--:|--:|:-:|:-:|:-:|:--|']
    for namn, v in d['korpusar'].items():
        poster = ('ja' if v['listade_poster'] == v['poster'] else
                  f'NEJ ({v["listade_poster"]} mot {v["poster"]})')
        avv = 'inga' if v['identisk'] else '; '.join(v['avvikelser'][:3])
        ut.append(f'| {namn} | {v["filer"]:,} | {poster} | '.replace(',', ' ') +
                  f'{"ja" if v["identisk"] else "NEJ"} | '
                  f'{"ja" if v["identisk"] else "NEJ"} | nej | {avv} |')
    delar = ['\n'.join(ut)]
    if d.get('skada'):
        for namn, prov in d['skada'].items():
            delar.append(f'Skadeprov på arkivet från korpusen **{namn}**:\n\n'
                         '| skada | utfall | godkänt |\n|:--|:--|:-:|\n' +
                         '\n'.join(f'| {p["skada"]} | {p["utfall"]} | '
                                   f'{"ja" if p["godkänt"] else "NEJ"} |' for p in prov))
    return '\n\n'.join(delar)


def dubbletter() -> str:
    f = ROT / 'resultat' / 'dubbletter.json'
    if not f.exists(): return '*(dubblettmätningen inte körd)*'
    d = json.loads(f.read_text())
    ut = ['| korpus | filer | unika filer | rå MiB | exakta dubbletter MiB | andel |',
          '|:--|--:|--:|--:|--:|--:|']
    for namn, v in d.items():
        ut.append(f'| {namn} | {v["filer"]:,} | {v["unika"]:,} | {v["rabyte"]/MIB:,.1f} | '
                  f'{v["dubblettbyte"]/MIB:,.1f} | {v["andel"]:.1f} % |'.replace(',', ' '))
    return '\n'.join(ut)


def standard() -> str:
    """Kodeknivå: en fil i taget, en tråd — som lzbench och LTCB mäter."""
    delar = []
    for korpus, rubrik in (('silesia', 'Silesia'), ('enwik8', 'enwik8')):
        f = ROT / 'resultat' / f'standard-{korpus}.json'
        if not f.exists(): continue
        d = json.loads(f.read_text())
        rå = d['rabyte']
        rader = ['| kodek | storlek byte | kvot | bits/byte | pack MiB/s | upp MiB/s | exakt |',
                 '|:--|--:|--:|--:|--:|--:|:-:|']
        for kodek, v in sorted(d.get('summor', {}).items(), key=lambda kv: -kv[1]['byte']):
            fet = kodek == 'nvcm'
            namn = f'**{kodek}**' if fet else kodek
            rader.append(
                f'| {namn} | {v["byte"]:,} | {v["kvot"]:.3f}× | {v["bpb"]:.3f} | '
                f'{rå/MIB/max(v["packsek"],1e-9):,.1f} | {rå/MIB/max(v["uppacksek"],1e-9):,.1f} | '
                f'{"ja" if v["exakt"] else "NEJ"} |'.replace(',', ' '))
        delar.append(f'**{rubrik}** — {rå:,} byte'.replace(',', ' ') + '\n\n' + '\n'.join(rader))
    return '\n\n'.join(delar) if delar else '*(standardbenchmarken inte körd)*'


def silesia_per_fil() -> str:
    f = ROT / 'resultat' / 'standard-silesia.json'
    if not f.exists(): return '*(standardbenchmarken inte körd)*'
    d = json.loads(f.read_text())
    kodekar = ['zstd -19', 'brotli -q 11', 'xz -9e', 'nvcm']
    ut = ['| fil | innehåll | rå byte | ' + ' | '.join(kodekar) + ' | nvcm mot bäst |',
          '|:--|:--|--:|' + '--:|' * len(kodekar) + '--:|']
    OM = {'dickens': 'engelsk litteratur, text', 'mozilla': 'körbara filer (tar)',
          'mr': 'medicinsk 3D-bild', 'nci': 'kemiska strukturer, text',
          'ooffice': 'körbar DLL', 'osdb': 'databasdump, binär',
          'reymont': 'polsk litteratur, PDF-källa', 'samba': 'källkod (tar)',
          'sao': 'stjärnkatalog, binär', 'webster': 'ordbok, text',
          'x-ray': 'röntgenbild, 16-bitars', 'xml': 'XML'}
    for namn in sorted(d.get('filer', {})):
        rad = d['filer'][namn]
        if 'nvcm' not in rad: continue
        rå = next(iter(rad.values()))
        råbyte = round(rad['nvcm']['byte'] * rad['nvcm']['kvot'])
        celler = []
        for k in kodekar:
            v = rad.get(k)
            celler.append(f'{v["byte"]:,}'.replace(',', ' ') if v else '—')
        andra = [rad[k]['byte'] for k in rad if k != 'nvcm']
        mot = 100 * (rad['nvcm']['byte'] - min(andra)) / min(andra)
        ut.append(f'| `{namn}` | {OM.get(namn, "")} | ' + f'{råbyte:,}'.replace(',', ' ') +
                  ' | ' + ' | '.join(celler) + f' | {mot:+.1f} % |')
    return '\n'.join(ut)


def fonster() -> str:
    f = ROT / 'resultat' / 'arkiv-fonster.log'
    if not f.exists(): return '*(fönsterexperimentet inte kört)*'
    rader = [r for r in f.read_text().splitlines() if r.strip()]
    return ('Experimentet `experiment/arkiv-fonster.sh` skiljer de två '
            'förklaringarna åt — är det kodeken eller fönstret?\n\n```\n' +
            '\n'.join(rader) + '\n```')


def enwik8_plats() -> str:
    f = ROT / 'resultat' / 'standard-enwik8.json'
    if not f.exists(): return '*(enwik8 inte mätt)*'
    d = json.loads(f.read_text())
    s = d.get('summor', {})
    if 'nvcm' not in s: return '*(nvcm inte mätt på enwik8)*'
    nv = s['nvcm']
    rader = [f'Vår egen mätning: **nvcm gör enwik8 på {nv["byte"]:,} byte, '
             f'{nv["bpb"]:.3f} bits/byte**'.replace(',', ' ')]
    egna = {k: v for k, v in s.items() if k != 'nvcm'}
    if egna:
        bast = min(egna.values(), key=lambda v: v['byte'])
        bastnamn = next(k for k, v in egna.items() if v is bast)
        d_proc = 100 * (nv['byte'] - bast['byte']) / bast['byte']
        rader.append(f'— {abs(d_proc):.1f} % {"mindre" if d_proc < 0 else "större"} än '
                     f'`{bastnamn}` på samma maskin ({bast["byte"]:,} byte, '
                     f'{bast["bpb"]:.3f} bpb).'.replace(',', ' '))
    takt = f'{nv["byte"]*8/100_000_000:.2f}'
    rader.append(f'\nMed {takt} bits/byte hamnar nvcm mellan ordbokspackarna '
                 'och de tunga kontextblandarna i tabellen ovan: den tar hem det '
                 'en ordbokspackare inte kan se, men den har åtta modeller där '
                 'cmix har hundratals, och den kör i ett par MiB/s där cmix kör '
                 'i kilobyte per sekund.')
    return ' '.join(rader[:2]) + '\n' + rader[-1]


def tider() -> str:
    """Genomströmning i MiB/s, för att tid ska gå att jämföra mellan korpusar."""
    ut = ['| korpus | nvz normal pack | nvz normal upp | zstd -3 pack '
          '| zstd -19 pack | xz -9e pack |', '|:--|--:|--:|--:|--:|--:|']
    for namn, rubrik in ALLA.items():
        d = r.läs(namn)
        if not d: continue
        rå = d['rabyte'] / MIB
        m = d['matningar']
        def fart(packare, nyckel='packsek'):
            v = m.get(packare)
            if not v: return '—'
            s = v[nyckel]
            return f'{rå/s:,.0f}'.replace(',', ' ') if s >= 0.05 else '>2 000'
        ut.append(f'| {rubrik} | {fart("nvz normal")} | '
                  f'{fart("nvz normal", "uppacksek")} | {fart("tar.zst -3 (dagens)")} | '
                  f'{fart("tar.zst -19 --long")} | {fart("tar.xz -9e")} |')
    return '\n'.join(ut) + '\n\n*MiB per sekund, rådata genom väggklockan.*'


def main():
    md = ROT / 'rapport' / 'bred-provning.md'
    r.infoga(md, oversikt(), 'ÖVERSIKT')
    r.infoga(md, mattabeller(), 'TABELLER')
    r.infoga(md, aterskapning(), 'ÅTERSKAPNING')
    r.infoga(md, tider(), 'TIDER')
    r.infoga(md, dubbletter(), 'DUBBLETTER')
    r.infoga(md, standard(), 'STANDARD')
    r.infoga(md, silesia_per_fil(), 'SILESIA-PER-FIL')
    r.infoga(md, enwik8_plats(), 'ENWIK8-PLATS')
    print(f'{md} uppdaterad')


if __name__ == '__main__':
    main()
