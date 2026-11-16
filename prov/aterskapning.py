#!/usr/bin/env python3
"""Återskapningsprov: packar en korpus, packar upp den och kräver tillbaka
varje byte, varje rättighet och varje tidsstämpel.

    python3 prov/aterskapning.py gif arkiv logg film
    python3 prov/aterskapning.py --skada gif        # bara skadeproven

Varför ett eget prov när mätharnesset redan kör `diff -rq`? För att diff
svarar på en annan fråga. diff jämför innehåll i de filer som finns på båda
sidor; den säger ingenting om rättigheter, tider, tomma kataloger eller om
en dubblett kom tillbaka som två riktiga filer eller som en hård länk. Och
den ger inget kvitto: efter en lyckad diff vet man att det gick bra, men man
har inget att visa. Här räknas i stället en avtryckslista — SHA-256 per fil
plus metadata — före och efter, och listorna ska vara identiska.

Skadeproven prövar den andra halvan av frågan "kan nvz återskapa datat":
en packare som tyst lämnar tillbaka FEL data är värre än en som vägrar.
"""
from __future__ import annotations
import hashlib, json, os, shutil, stat, subprocess, sys, time
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
NV = str(ROT / 'src' / 'nvz')
SKRAP = Path(os.environ.get('NVPACK_SKRAP', ROT / '.skrap')) / 'aterskapning'


def avtryck(rot: Path) -> dict:
    """Allt om trädet som en uppackning måste ge tillbaka."""
    poster = {}
    for p in sorted(rot.rglob('*')):
        rel = str(p.relative_to(rot))
        st = p.lstat()
        if stat.S_ISLNK(st.st_mode):
            poster[rel] = {'typ': 'länk', 'mål': os.readlink(p)}
        elif p.is_dir():
            poster[rel] = {'typ': 'katalog', 'läge': stat.S_IMODE(st.st_mode)}
        else:
            h = hashlib.sha256()
            with open(p, 'rb') as f:
                for bit in iter(lambda: f.read(1 << 20), b''):
                    h.update(bit)
            poster[rel] = {'typ': 'fil', 'byte': st.st_size, 'läge': stat.S_IMODE(st.st_mode),
                           'mtid_ns': st.st_mtime_ns, 'sha256': h.hexdigest(),
                           'länkar': st.st_nlink}
    return poster


def jamfor(fore: dict, efter: dict) -> list[str]:
    avvikelser = []
    for rel in sorted(set(fore) | set(efter)):
        a, b = fore.get(rel), efter.get(rel)
        if a is None: avvikelser.append(f'extra i uppackningen: {rel}'); continue
        if b is None: avvikelser.append(f'saknas efter uppackning: {rel}'); continue
        for nyckel in a:
            # Antalet hårda länkar jämförs inte rakt: korpusarna ÄR hårdlänkar
            # in i hemkatalogen, och en uppackning ska ge fristående filer.
            # Kravet är i stället att uppackningen inte lämnar länkar efter sig.
            if nyckel == 'länkar':
                if b.get('länkar', 1) != 1:
                    avvikelser.append(f'{rel}: uppackad fil har {b["länkar"]} hårda länkar')
                continue
            if a[nyckel] != b.get(nyckel):
                avvikelser.append(f'{rel}: {nyckel} {a[nyckel]!r} -> {b.get(nyckel)!r}')
    return avvikelser


def kor(args, vantar_fel=False):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode and not vantar_fel:
        raise RuntimeError(f'{" ".join(args[:3])}: {r.stderr[-1500:]}')
    return r


def prova_korpus(namn: str, lage: str = 'normal') -> dict:
    korpus = ROT / 'korpus' / namn
    SKRAP.mkdir(parents=True, exist_ok=True)
    arkiv = SKRAP / f'{namn}-{lage}.nv'
    upp = SKRAP / f'upp-{namn}'
    arkiv.unlink(missing_ok=True)
    shutil.rmtree(upp, ignore_errors=True)

    fore = avtryck(korpus)
    rabyte = sum(p['byte'] for p in fore.values() if p['typ'] == 'fil')
    # Packas utan -q: raderna om dedup och om vilken kodek varje block fick
    # är halva svaret på VARFÖR en korpus komprimerar som den gör.
    t0 = time.monotonic()
    r = kor([NV, 'packa', str(korpus), str(arkiv), '--läge', lage])
    packsek = time.monotonic() - t0
    packutdata = [rad.strip() for rad in r.stdout.splitlines() if rad.strip()]
    upp.mkdir(parents=True)
    t0 = time.monotonic(); kor([NV, 'packa-upp', str(arkiv), str(upp), '-q'])
    uppacksek = time.monotonic() - t0
    efter = avtryck(upp / namn)
    avvikelser = jamfor(fore, efter)

    # `lista` ska redovisa lika många poster som trädet har — kataloger och
    # länkar inräknade. Stämmer inte det bär registret inte hela trädet,
    # även om de filer som KOM tillbaka råkade vara riktiga.
    listad = kor([NV, 'lista', str(arkiv)]).stdout
    filer_fore = sum(1 for p in fore.values() if p['typ'] == 'fil')
    listade_poster = None
    for rad in listad.splitlines():
        if ' poster,' in rad:
            listade_poster = int(rad.split(' poster,')[0].split()[-1])
    # +1: korpusens egen rotkatalog är en post i arkivet men inte i avtrycket.
    poster_fore = len(fore) + 1
    if listade_poster != poster_fore:
        avvikelser.append(f'lista: {listade_poster} poster i registret, {poster_fore} i trädet')

    res = dict(korpus=namn, läge=lage, filer=filer_fore, rabyte=rabyte,
               arkivbyte=arkiv.stat().st_size, packsek=round(packsek, 2),
               uppacksek=round(uppacksek, 2), avvikelser=avvikelser,
               identisk=not avvikelser, poster=poster_fore,
               listade_poster=listade_poster, packutdata=packutdata,
               listning=listad.strip().splitlines())
    shutil.rmtree(upp, ignore_errors=True)
    arkiv.unlink(missing_ok=True)
    return res


def prova_skada(namn: str) -> list[dict]:
    """Ett arkiv som fått en bit förstörd får ge fel — men aldrig fel data."""
    korpus = ROT / 'korpus' / namn
    SKRAP.mkdir(parents=True, exist_ok=True)
    orig = SKRAP / f'skada-{namn}.nv'
    orig.unlink(missing_ok=True)
    kor([NV, 'packa', str(korpus), str(orig), '-q'])
    rent = avtryck(korpus)
    data = orig.read_bytes()
    prov = []

    fall = [
        ('magin bytt',          lambda b: b'XXXXXX\x01' + b[7:]),
        ('byte i blockkropp',   lambda b: b[:len(b)//3] + bytes([b[len(b)//3] ^ 0x40]) + b[len(b)//3+1:]),
        ('sista kilobyten bort',lambda b: b[:-1024]),
        ('registerpekaren noll',lambda b: b[:-16] + b'\x00'*16),
        ('en byte i registret', lambda b: b[:-64] + bytes([b[-64] ^ 0xff]) + b[-63:]),
    ]
    for etikett, skada in fall:
        trasig = SKRAP / 'trasig.nv'
        trasig.write_bytes(skada(data))
        upp = SKRAP / 'upp-trasig'
        shutil.rmtree(upp, ignore_errors=True); upp.mkdir(parents=True)
        r = kor([NV, 'packa-upp', str(trasig), str(upp), '-q'], vantar_fel=True)
        mal = upp / namn
        if r.returncode != 0:
            utfall, godkant = 'vägrade', True
        elif not mal.exists():
            utfall, godkant = 'gav ingenting', True
        else:
            avvikelser = jamfor(rent, avtryck(mal))
            if avvikelser:
                utfall, godkant = f'LÄMNADE FEL DATA ({len(avvikelser)} avvikelser)', False
            else:
                utfall, godkant = 'oskadd (biten låg i slack)', True
        prov.append(dict(skada=etikett, utfall=utfall, godkänt=godkant,
                         felkod=r.returncode))
        shutil.rmtree(upp, ignore_errors=True)
        trasig.unlink(missing_ok=True)
    orig.unlink(missing_ok=True)
    return prov


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    bara_skada = '--skada' in sys.argv
    korpusar = args or ['gif', 'arkiv', 'logg', 'film']
    utfil = ROT / 'resultat' / 'aterskapning.json'
    data = json.loads(utfil.read_text()) if utfil.exists() else {}
    data.setdefault('korpusar', {}); data.setdefault('skada', {})

    if not bara_skada:
        for namn in korpusar:
            print(f'{namn}: räknar avtryck och packar...', flush=True)
            r = prova_korpus(namn)
            data['korpusar'][namn] = r
            kvot = r['rabyte'] / r['arkivbyte'] if r['arkivbyte'] else 0
            print(f'  {r["filer"]} filer  {r["rabyte"]/1048576:.1f} -> '
                  f'{r["arkivbyte"]/1048576:.1f} MiB (x{kvot:.2f})  '
                  f'pack {r["packsek"]:.1f}s  upp {r["uppacksek"]:.1f}s  '
                  f'{"ALLT TILLBAKA" if r["identisk"] else "AVVIKELSER"}', flush=True)
            for a in r['avvikelser'][:10]: print('    ', a)
            utfil.write_text(json.dumps(data, indent=1, ensure_ascii=False))

    namn = korpusar[0]
    print(f'skadeprov på {namn}:', flush=True)
    data['skada'][namn] = prova_skada(namn)
    for p in data['skada'][namn]:
        print(f'  {p["skada"]:<24} {p["utfall"]:<40} {"ok" if p["godkänt"] else "FEL"}')
    utfil.write_text(json.dumps(data, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
