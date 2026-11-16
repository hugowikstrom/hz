#!/usr/bin/env python3
"""Bygger resultattabellerna ur resultat/*.json.

Siffror som skrivs av för hand blir fel förr eller senare — oftast när något
mäts om och bara hälften av texten uppdateras. Därför finns den här: tabellen
i README och i rapporten kommer alltid ur den senaste mätningen.

    python3 matning/rapport.py            # markdown till stdout
    python3 matning/rapport.py --text     # samma sak som ren text
"""
import json, sys
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
ORDNING = ['tar (okomprimerad)', 'tar.gz -6', 'tar.gz -9 (pigz)', 'tar.bz2 -9',
           'tar.zst -3 (dagens)', 'tar.zst -19 --long', 'tar.zst -22 --ultra',
           'tar.xz -9e', 'nvz snabb', 'nvz utan knep', 'nvz normal',
           'nvz liten']
KORPUS = {'kod': 'Programkod', 'png': 'Bilder (PNG)', 'foto': 'Foton (JPEG)',
          'gif': 'Bilder (GIF)', 'film': 'Film (h264/VP9)', 'arkiv': 'Arkiv (zip/tar.gz)',
          'logg': 'Loggfiler', 'symbios3d': 'Blandat projektträd (symbios-3d)',
          'silesia': 'Silesia (standardkorpus)', 'enwik8': 'enwik8 (standardkorpus)'}
MIB = 1048576


def tal(x, d=2):
    """Svenskt talformat: decimalkomma, mellanslag som tusenavskiljare.
    Tabellerna är svenska — då är 27.4 fel och 27,4 rätt."""
    return f'{x:,.{d}f}'.replace(',', ' ').replace('.', ',')


def läs(namn):
    f = ROT / 'resultat' / f'{namn}.json'
    return json.loads(f.read_text()) if f.exists() else None


def tabell(data, md=True):
    rå = data['rådata'] if 'rådata' in data else data['rabyte']
    mät = data['matningar']
    bas = mät.get('tar.zst -3 (dagens)', {}).get('byte')
    rader = []
    # Minsta arkiv och kortaste packtid markeras, så att kolumnjämförelsen
    # går att göra med blicken i stället för med huvudräkning.
    med = [m for namn in ORDNING if (m := mät.get(namn))]
    minsta = min((m['byte'] for m in med), default=None)
    snabbast = min((m['packsek'] for m in med), default=None)
    for namn in ORDNING:
        m = mät.get(namn)
        if not m: continue
        mot = (('+' if m['byte'] >= bas else '−') +
               tal(abs(100*(m['byte']-bas)/bas), 1) + ' %') if bas else '—'
        storlek = tal(m['byte']/MIB)
        if m['byte'] == minsta: storlek = f'**{storlek}**'
        ptid = f'{m["packsek"]:.0f} s'
        if m['packsek'] == snabbast: ptid = f'**{ptid}**'
        rader.append((namn, storlek,
                      tal(rå/m['byte']) + '×', mot,
                      ptid, f'{m["uppacksek"]:.0f} s',
                      'ja' if m.get('identisk') else 'NEJ'))
    rubrik = ('packare', 'storlek MiB', 'kvot', 'mot dagens', 'packtid', 'uppacktid', 'exakt')
    if md:
        ut = ['| ' + ' | '.join(rubrik) + ' |',
              '|' + '|'.join([':--'] + ['--:'] * 5 + [':-:']) + '|']
        for r in rader:
            fet = r[0].startswith('nvz')
            ut.append('| ' + ' | '.join(f'**{c}**' if fet and i == 0 else c
                                        for i, c in enumerate(r)) + ' |')
        return '\n'.join(ut)
    bredd = [max(len(str(x[i])) for x in [rubrik] + rader) for i in range(7)]
    ut = ['  '.join(h.ljust(b) for h, b in zip(rubrik, bredd))]
    ut.append('  '.join('-' * b for b in bredd))
    for r in rader:
        ut.append('  '.join(c.ljust(b) if i == 0 else c.rjust(b)
                            for i, (c, b) in enumerate(zip(r, bredd))))
    return '\n'.join(ut)


def infoga(sökväg: Path, text: str, markör='TABELLER'):
    """Byter ut allt mellan markörerna i en fil mot färska tabeller."""
    START, SLUT = f'<!-- {markör} -->', f'<!-- SLUT {markör} -->'
    gammal = sökväg.read_text()
    i, j = gammal.index(START), gammal.index(SLUT)
    sökväg.write_text(gammal[:i + len(START)] + '\n\n' + text + '\n\n' + gammal[j:])


def main():
    md = '--text' not in sys.argv
    delar = []
    for namn, rubrik in KORPUS.items():
        d = läs(namn)
        if not d: continue
        rå = d['rabyte']
        huvud = (f'### {rubrik} — {tal(rå/MIB, 1)} MiB i {d["filer"]:,} filer'
                 .replace(',', ' ')) if md else \
                f'{rubrik} — {tal(rå/MIB, 1)} MiB i {d["filer"]} filer'
        delar.append(huvud + '\n\n' + tabell(d, md))
    text = '\n\n'.join(delar)
    if '--in-i-readme' in sys.argv:
        infoga(ROT / 'README.md', text)
        print('README.md uppdaterad')
    else:
        print(text)


if __name__ == '__main__':
    main()
