#!/usr/bin/env python3
"""Slumpprov: bygger slumpmässiga träd, packar, packar upp, kräver identitet.

Varför det här finns utöver prova-allt.sh: den sviten provar kantfall jag
KOM PÅ. Det här provar kombinationer ingen kom på — slumpade filstorlekar,
filtyper, dubblettmönster, rättigheter, tider, namn och lägen. Varje runda är
en hel rundgång: packa, packa upp, jämföra varje byte, varje rättighet och
varje tidsstämpel, och dessutom köra arkivets egen kontroll (-t).

  python3 prov/slumpprov.py [rundor] [--frö N]

En runda som misslyckas skriver ut sitt frö. Samma frö ger samma träd, så
felet går att återskapa exakt.
"""
import filecmp, os, random, shutil, subprocess, sys, tempfile
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
NVZ = ROT / 'src' / 'nvz'
LÄGEN = ['snabb', 'normal', 'liten']

# Riktiga PNG:er och JPEG:ar ur korpusen, om den finns — omvandlingarna är
# den del som har mest att gå sönder, och de rörs inte av syntetisk data.
def förlagor():
    ut = []
    for mönster, tak in (('korpus/png/**/*.png', 40), ('korpus/foto/**/*.jpg', 40)):
        ut += [p for p in list(ROT.glob(mönster))[:tak] if p.is_file() and p.stat().st_size < 4 << 20]
    return ut


def bygg_träd(rot: Path, r: random.Random, bilder):
    rot.mkdir(parents=True)
    kataloger = [rot]
    for _ in range(r.randint(0, 6)):
        d = r.choice(kataloger) / r.choice(['a', 'djupt', 'namn med mellanslag', 'åäö', '.dold'])
        d.mkdir(parents=True, exist_ok=True)
        kataloger.append(d)
    tidigare = []
    for i in range(r.randint(1, 40)):
        d = r.choice(kataloger)
        namn = r.choice([f'f{i}', f'f{i}.txt', f'f{i}.png', f'f{i}.jpg', f'-streck{i}',
                         f'åäö{i}.md', f'ny\nrad{i}', f'f{i}.gz'])
        p = d / namn
        sort = r.choice(['tom', 'noll', 'slump', 'text', 'dubblett', 'bild', 'ettbyte'])
        if sort == 'bild' and bilder:
            shutil.copy(r.choice(bilder), p)
        elif sort == 'dubblett' and tidigare:
            shutil.copy(r.choice(tidigare), p)
        elif sort == 'tom':
            p.write_bytes(b'')
        elif sort == 'ettbyte':
            p.write_bytes(bytes([r.randrange(256)]))
        elif sort == 'noll':
            p.write_bytes(bytes(r.randint(1, 1 << 18)))
        elif sort == 'slump':
            p.write_bytes(bytes(r.randrange(256) for _ in range(r.randint(1, 1 << 14))))
        else:
            rad = (f'rad {r.randrange(1000)} med text\n' * r.randint(1, 400)).encode()
            p.write_bytes(rad)
        tidigare.append(p)
        os.chmod(p, r.choice([0o644, 0o600, 0o400, 0o755, 0o444]))
        os.utime(p, (r.randint(0, 2 << 30), r.randint(0, 2 << 30)))
    for _ in range(r.randint(0, 4)):          # symlänkar: relativa, trasiga, kataloger
        d = r.choice(kataloger)
        mål = r.choice(['../' + r.choice(tidigare).name, '/finns/inte', '.'])
        try: (d / f'lank{r.randrange(10000)}').symlink_to(mål)
        except OSError: pass


def runda(frö: int, arbete: Path, bilder) -> str | None:
    r = random.Random(frö)
    träd, upp = arbete / 'trad', arbete / 'upp'
    ark = arbete / 'prov.nv'
    for p in (träd, upp):
        shutil.rmtree(p, ignore_errors=True)
    ark.unlink(missing_ok=True)
    bygg_träd(träd, r, bilder)
    läge = r.choice(LÄGEN)
    k = subprocess.run([NVZ, 'packa', str(träd), str(ark), '-q', f'--läge={läge}'],
                       capture_output=True, text=True)
    if k.returncode: return f'packningen gav {k.returncode}: {k.stderr[:200]}'
    k = subprocess.run([NVZ, '-t', str(ark)], capture_output=True, text=True)
    if k.returncode: return f'-t gav {k.returncode}: {k.stderr[:200]}'
    upp.mkdir(parents=True)
    k = subprocess.run([NVZ, 'packa-upp', str(ark), str(upp), '-q'], capture_output=True, text=True)
    if k.returncode: return f'uppackningen gav {k.returncode}: {k.stderr[:200]}'
    k = subprocess.run(['diff', '-rq', '--no-dereference', str(träd), str(upp / träd.name)],
                       capture_output=True, text=True)
    if k.returncode: return f'innehållet skiljer: {k.stdout[:300]}'
    for a, b in ((träd, upp / träd.name),):            # rättigheter och tider
        for p in sorted(a.rglob('*')):
            q = b / p.relative_to(a)
            sa, sb = p.lstat(), q.lstat()
            if (sa.st_mode, int(sa.st_mtime)) != (sb.st_mode, int(sb.st_mtime)):
                return f'rättighet/tid skiljer på {p.relative_to(a)}'
    return None


def main():
    rundor = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 50
    start = int(sys.argv[sys.argv.index('--frö') + 1]) if '--frö' in sys.argv else 1
    bilder = förlagor()
    fel = 0
    with tempfile.TemporaryDirectory(prefix='nvz-slump-') as d:
        for i in range(rundor):
            frö = start + i
            brist = runda(frö, Path(d), bilder)
            if brist:
                fel += 1
                print(f'  FEL  frö {frö}: {brist}', flush=True)
            elif (i + 1) % 10 == 0:
                print(f'  {i+1}/{rundor} rundor ok', flush=True)
    print(f'\n{rundor - fel}/{rundor} slumpade träd kom tillbaka bit-exakt'
          f'{" — ALLA" if not fel else ""}')
    return 1 if fel else 0


if __name__ == '__main__':
    sys.exit(main())
