#!/usr/bin/env python3
"""Mätharness: kör en uppsättning packare över en korpus och bokför resultatet.

Varje packare får samma korpus och mäts på tre tal: storlek, packtid, uppacktid.
Utdata skrivs till resultat/<korpus>.json så att tabellerna kan byggas om utan
att packa om. Allt som skrivs hamnar i skräpkatalogen, aldrig i korpusen.
"""
import json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
SKRAP = Path(os.environ.get('NVPACK_SKRAP', '/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/nvz-matning'))

# Varje post: namn -> (packkommando, uppackkommando). {k}=korpuskatalog, {a}=arkivfil,
# {u}=uppackningskatalog. Kommandona körs i skal; korpusen packas alltid som
# katalognamnet, inte som absolut sökväg.
PACKARE = {
 'tar (okomprimerad)': ("tar -C {kf} -cf {a} {kn}", "tar -C {u} -xf {a}"),
 'tar.gz -6':          ("tar -C {kf} -cf - {kn} | gzip -6 > {a}", "tar -C {u} -xzf {a}"),
 'tar.gz -9 (pigz)':   ("tar -C {kf} -cf - {kn} | pigz -9 -p 24 > {a}", "tar -C {u} -xzf {a}"),
 'tar.bz2 -9':         ("tar -C {kf} -cf - {kn} | bzip2 -9 > {a}", "tar -C {u} -xjf {a}"),
 'tar.zst -3 (dagens)':("tar -C {kf} -cf - {kn} | zstd -3 -T0 -q -o {a}", "zstd -dq -c {a} | tar -C {u} -xf -"),
 'tar.zst -19 --long': ("tar -C {kf} -cf - {kn} | zstd -19 -T0 --long=27 -q -o {a}", "zstd -dq --long=27 -c {a} | tar -C {u} -xf -"),
 'tar.zst -22 --ultra':("tar -C {kf} -cf - {kn} | zstd -22 --ultra -T0 --long=31 -q -o {a}", "zstd -dq --long=31 -c {a} | tar -C {u} -xf -"),
 'tar.xz -9e':         ("tar -C {kf} -cf - {kn} | xz -9e -T0 -c > {a}", "xz -dc -T0 {a} | tar -C {u} -xf -"),
 'nvz snabb':       ("{nv} packa {k} {a} -q --läge snabb", "{nv} packa-upp {a} {u} -q"),
 'nvz normal':      ("{nv} packa {k} {a} -q --läge normal", "{nv} packa-upp {a} {u} -q"),
 'nvz liten':       ("{nv} packa {k} {a} -q --läge liten", "{nv} packa-upp {a} {u} -q"),
 'nvz utan knep':   ("{nv} packa {k} {a} -q --läge normal --utan-dedup --utan-omvandling",
                        "{nv} packa-upp {a} {u} -q"),
}

def kor(cmd):
    t = time.monotonic()
    r = subprocess.run(['bash','-o','pipefail','-c',cmd], capture_output=True, text=True)
    if r.returncode: raise RuntimeError(f"{cmd}\n{r.stderr[-2000:]}")
    return time.monotonic() - t

def korpusbyte(k: Path):
    return sum(p.stat().st_size for p in k.rglob('*') if p.is_file())

def matning(korpus: Path, namn, kontrollera=True):
    SKRAP.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r'[^a-z0-9]+', '-', namn.lower()).strip('-')
    arkiv = SKRAP / f'prov-{slug}.ark'
    upp = SKRAP / 'upp'
    fallt = dict(k=str(korpus), kf=str(korpus.parent), kn=korpus.name, a=str(arkiv),
                 u=str(upp), nv=str(ROT/'src'/'nvz'))
    pack, uppack = (c.format(**fallt) for c in PACKARE[namn])
    arkiv.unlink(missing_ok=True)
    tp = kor(pack)
    storlek = arkiv.stat().st_size
    shutil.rmtree(upp, ignore_errors=True); upp.mkdir(parents=True)
    tu = kor(uppack)
    lika = None
    if kontrollera:
        r = subprocess.run(['diff','-rq','--no-dereference',str(korpus), str(upp/korpus.name)],
                           capture_output=True, text=True)
        lika = (r.returncode == 0)
        if not lika: print('   AVVIKELSE:', r.stdout[:500], file=sys.stderr)
    shutil.rmtree(upp, ignore_errors=True); arkiv.unlink(missing_ok=True)
    return dict(packare=namn, byte=storlek, packsek=round(tp,2), uppacksek=round(tu,2), identisk=lika)

def main():
    korpus = ROT/'korpus'/sys.argv[1]
    valda = sys.argv[2:] or list(PACKARE)
    ra = korpusbyte(korpus)
    utfil = ROT/'resultat'/f'{korpus.name}.json'
    data = json.loads(utfil.read_text()) if utfil.exists() else {}
    data['korpus'] = korpus.name
    data['rabyte'] = ra
    data['filer'] = sum(1 for p in korpus.rglob('*') if p.is_file())
    data.setdefault('matningar', {})
    print(f'{korpus.name}: {ra/1048576:.1f} MiB i {data["filer"]} filer')
    for namn in valda:
        try:
            r = matning(korpus, namn)
        except Exception as e:
            print(f'  {namn:<22} FEL: {e}'); continue
        data['matningar'][namn] = r
        kvot = ra / r['byte']
        print(f'  {namn:<22} {r["byte"]/1048576:>9.2f} MiB  x{kvot:>5.2f}  '
              f'pack {r["packsek"]:>7.1f}s  upp {r["uppacksek"]:>6.1f}s  '
              f'{"exakt" if r["identisk"] else "SKILJER"}')
        utfil.parent.mkdir(exist_ok=True)
        utfil.write_text(json.dumps(data, indent=1, ensure_ascii=False))

if __name__ == '__main__': main()
