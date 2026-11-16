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
from rapport import tal

MIB = 1048576
NYA = {'gif': 'Bilder (GIF)', 'film': 'Film (h264 och VP9)',
       'arkiv': 'Arkiv (zip och tar.gz)', 'logg': 'Loggfiler'}
ALLA = {'kod': 'Programkod', 'png': 'Bilder (PNG)', 'foto': 'Foton (JPEG)', **NYA,
        'silesia': 'Silesia (arkivnivå)', 'enwik8': 'enwik8 (arkivnivå)'}


def jattetabell() -> str:
    """Hela mätmatrisen i en tabell: packare som rader, korpusar som
    kolumner. Två paneler — storlek och packtid — eftersom en cell som bär
    båda blir oläslig. Minsta storlek och kortaste tid per kolumn i fet."""
    korpusar = [(k, r.läs(k)) for k in ALLA]
    korpusar = [(k, d) for k, d in korpusar if d]
    namnrad = {'kod': 'kod', 'png': 'png', 'foto': 'foto', 'gif': 'gif',
               'film': 'film', 'arkiv': 'arkiv', 'logg': 'logg',
               'silesia': 'silesia', 'enwik8': 'enwik8'}

    def panel(varde, fetast, enhet, rubrik):
        ut = [f'**{rubrik}**', '',
              '| packare | ' + ' | '.join(namnrad.get(k, k) for k, _ in korpusar) + ' |',
              '|:--|' + '--:|' * len(korpusar)]
        bast = {k: fetast([varde(m) for m in d['matningar'].values() if varde(m) is not None])
                for k, d in korpusar}
        for namn in r.ORDNING:
            celler = []
            for k, d in korpusar:
                m = d['matningar'].get(namn)
                if not m: celler.append('—'); continue
                v = varde(m)
                c = tal(v, 1) if enhet == 'MiB' else f'{v:.0f}'
                if v == bast[k]: c = f'**{c}**'
                celler.append(c)
            fet = namn.startswith('nvz')
            rad = f'**{namn}**' if fet else namn
            ut.append(f'| {rad} | ' + ' | '.join(celler) + ' |')
        ut.append('')
        ut.append(f'*{enhet} per korpus; minsta värde per kolumn i fet. '
                  f'Råstorlekar: ' + ', '.join(
                      f'{namnrad.get(k, k)} {tal(d["rabyte"]/MIB, 0)}'
                      for k, d in korpusar) + ' MiB.*')
        return '\n'.join(ut)

    return (panel(lambda m: m['byte']/MIB, min, 'MiB', 'Arkivstorlek') + '\n\n' +
            panel(lambda m: m['packsek'], min, 's', 'Packtid') + '\n\n' +
            panel(lambda m: m['uppacksek'], min, 's', 'Uppacktid'))


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
        def procent(x):
            return ('+' if x >= 0 else '−') + tal(abs(x), 1) + ' %'
        rader.append((
            rubrik,
            tal(d["rabyte"]/MIB, 0),
            tal(idag["byte"]/MIB, 1),
            tal(bast_klassisk["byte"]/MIB, 1) + ' (' + bk.replace("tar.", "").replace(" --long", "").replace(" --ultra", "") + ')',
            tal(nv["byte"]/MIB, 1) + ' (' + nk.split()[1] + ')',
            procent(100*(nv["byte"]-idag["byte"])/idag["byte"]),
            procent(100*(nv["byte"]-bast_klassisk["byte"])/bast_klassisk["byte"]),
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
        ut.append(f'| {namn} | {v["filer"]:,} | {v["unika"]:,} | '.replace(',', ' ') +
                  f'{tal(v["rabyte"]/MIB, 1)} | {tal(v["dubblettbyte"]/MIB, 1)} | '
                  f'{tal(v["andel"], 1)} % |')
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
                f'| {namn} | ' + f'{v["byte"]:,}'.replace(',', ' ') +
                f' | {tal(v["kvot"], 3)}× | {tal(v["bpb"], 3)} | '
                f'{tal(rå/MIB/max(v["packsek"],1e-9), 1)} | '
                f'{tal(rå/MIB/max(v["uppacksek"],1e-9), 1)} | '
                f'{"ja" if v["exakt"] else "NEJ"} |')
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
        mottext = ('+' if mot >= 0 else '−') + tal(abs(mot), 1) + ' %'
        ut.append(f'| `{namn}` | {OM.get(namn, "")} | ' + f'{råbyte:,}'.replace(',', ' ') +
                  ' | ' + ' | '.join(celler) + f' | {mottext} |')
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
    rader = ['Vår egen mätning: **nvcm gör enwik8 på ' + f'{nv["byte"]:,}'.replace(',', ' ') +
             f' byte, {tal(nv["bpb"], 3)} bits/byte**']
    egna = {k: v for k, v in s.items() if k != 'nvcm'}
    if egna:
        bast = min(egna.values(), key=lambda v: v['byte'])
        bastnamn = next(k for k, v in egna.items() if v is bast)
        d_proc = 100 * (nv['byte'] - bast['byte']) / bast['byte']
        rader.append(f'— {tal(abs(d_proc), 1)} % {"mindre" if d_proc < 0 else "större"} än '
                     f'`{bastnamn}` på samma maskin (' + f'{bast["byte"]:,}'.replace(',', ' ') +
                     f' byte, {tal(bast["bpb"], 3)} bpb).')
    takt = tal(nv["byte"]*8/100_000_000)
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
            return tal(rå/s, 0) if s >= 0.05 else '>2 000'
        ut.append(f'| {rubrik} | {fart("nvz normal")} | '
                  f'{fart("nvz normal", "uppacksek")} | {fart("tar.zst -3 (dagens)")} | '
                  f'{fart("tar.zst -19 --long")} | {fart("tar.xz -9e")} |')
    return '\n'.join(ut) + '\n\n*MiB per sekund, rådata genom väggklockan.*'


def main():
    md = ROT / 'rapport' / 'bred-provning.md'
    r.infoga(md, oversikt(), 'ÖVERSIKT')
    r.infoga(md, jattetabell(), 'MATRIS')
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
