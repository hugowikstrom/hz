#!/usr/bin/env python3
"""Återskapningsprotokoll: packa, packa upp, och bevisa med md5 att varje
fil kom tillbaka exakt.

    python3 prov/protokoll.py                # alla korpusar
    python3 prov/protokoll.py logg kod       # bara de nämnda

För varje korpus görs en hel rundgång med standardläget (`liten`):

  1. md5-manifest FÖRE: en rad per fil, i `md5sum`-format, sparad i
     resultat/protokoll/<korpus>-fore.md5
  2. packning till ett temporärt arkiv; arkivets storlek och md5 bokförs
  3. uppackning till en tom katalog
  4. md5-manifest EFTER, sparad bredvid: <korpus>-efter.md5
  5. jämförelse rad för rad — och dessutom körs `md5sum -c` med
     FÖRE-manifestet mot det UPPACKADE trädet, så att kontrollen görs av
     ett oberoende verktyg och inte av samma kod som skrev manifesten

Manifesten checkas in i repot. Poängen är att vem som helst kan granska
dem och köra om kontrollen själv:

    cd <uppackat träd> && md5sum -c <korpus>-fore.md5

Protokollet (resultat/protokoll/protokoll.json) bär allt rapporten visar.
"""
import hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
NVZ = ROT / 'src' / 'nvz'
UT = ROT / 'resultat' / 'protokoll'
SKRAP = Path(os.environ.get('NVPACK_SKRAP', ROT / '.skrap')) / 'protokoll'

KORPUSAR = ['kod', 'gif', 'foto', 'arkiv', 'logg', 'png', 'film', 'silesia', 'enwik8']


def md5_fil(p: Path) -> str:
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for bit in iter(lambda: f.read(1 << 20), b''):
            h.update(bit)
    return h.hexdigest()


def manifest(rot: Path) -> list[str]:
    """`md5sum`-rader för varje fil under rot, i stabil ordning."""
    rader = []
    for p in sorted(rot.rglob('*')):
        if p.is_file() and not p.is_symlink():
            rader.append(f'{md5_fil(p)}  {p.relative_to(rot)}')
    return rader


def kor(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{" ".join(str(a) for a in args[:3])}: {r.stderr[-800:]}')
    return r


def protokollfora(namn: str) -> dict:
    korpus = ROT / 'korpus' / namn
    SKRAP.mkdir(parents=True, exist_ok=True)
    UT.mkdir(parents=True, exist_ok=True)
    arkiv = SKRAP / f'{namn}.nv'
    upp = SKRAP / f'upp-{namn}'
    arkiv.unlink(missing_ok=True)
    shutil.rmtree(upp, ignore_errors=True)

    print(f'{namn}: manifest före ...', flush=True)
    fore = manifest(korpus)
    rabyte = sum(p.stat().st_size for p in korpus.rglob('*') if p.is_file())
    (UT / f'{namn}-fore.md5').write_text('\n'.join(fore) + '\n')

    print(f'{namn}: packar (läge liten) ...', flush=True)
    t0 = time.monotonic()
    kor([NVZ, 'packa', str(korpus), str(arkiv), '-q'])
    packsek = round(time.monotonic() - t0, 1)
    arkivbyte = arkiv.stat().st_size
    arkivmd5 = md5_fil(arkiv)

    print(f'{namn}: packar upp ...', flush=True)
    t0 = time.monotonic()
    kor([NVZ, 'packa-upp', str(arkiv), str(upp), '-q'])
    uppsek = round(time.monotonic() - t0, 1)

    print(f'{namn}: manifest efter ...', flush=True)
    uppkorpus = upp / namn
    efter = manifest(uppkorpus)
    (UT / f'{namn}-efter.md5').write_text('\n'.join(efter) + '\n')

    lika = fore == efter
    # Oberoende kontroll: systemets md5sum mot före-manifestet.
    r = subprocess.run(['md5sum', '-c', '--quiet', str(UT / f'{namn}-fore.md5')],
                       cwd=uppkorpus, capture_output=True, text=True)
    md5sum_ok = (r.returncode == 0)

    avvikelser = [f for f, e in zip(fore, efter) if f != e][:5] if not lika else []
    res = dict(
        korpus=namn, filer=len(fore), rabyte=rabyte,
        arkivbyte=arkivbyte, arkiv_md5=arkivmd5,
        packsek=packsek, uppacksek=uppsek,
        manifest_fore_md5=hashlib.md5('\n'.join(fore).encode()).hexdigest(),
        manifest_efter_md5=hashlib.md5('\n'.join(efter).encode()).hexdigest(),
        identiska=lika, md5sum_c_ok=md5sum_ok,
        exempel=fore[:3], avvikelser=avvikelser,
    )
    shutil.rmtree(upp, ignore_errors=True)
    arkiv.unlink(missing_ok=True)
    status = 'IDENTISKT' if (lika and md5sum_ok) else 'AVVIKER!'
    print(f'{namn}: {len(fore)} filer, manifest {res["manifest_fore_md5"][:12]}… -> '
          f'{res["manifest_efter_md5"][:12]}…  {status}', flush=True)
    return res


def main():
    valda = sys.argv[1:] or KORPUSAR
    utfil = UT / 'protokoll.json'
    UT.mkdir(parents=True, exist_ok=True)
    data = json.loads(utfil.read_text()) if utfil.exists() else {}
    data['maskin'] = 'Ryzen AI 9 HX 470, 24 trådar, NVMe'
    data['nvz'] = subprocess.run([NVZ, '--version'], capture_output=True,
                                 text=True).stdout.strip()
    data['datum'] = time.strftime('%Y-%m-%d %H:%M')
    data.setdefault('korpusar', {})
    for namn in valda:
        data['korpusar'][namn] = protokollfora(namn)
        utfil.write_text(json.dumps(data, indent=1, ensure_ascii=False))
    alla = all(k['identiska'] and k['md5sum_c_ok'] for k in data['korpusar'].values())
    print('\nSLUTSATS:', 'alla korpusar bitexakta' if alla else 'AVVIKELSER FINNS')


if __name__ == '__main__':
    main()
