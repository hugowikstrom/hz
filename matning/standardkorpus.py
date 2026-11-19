#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# [EN] Codec-level measurements on the standard corpora (Silesia, enwik8),
# single-threaded, against published literature numbers.
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
"""Standardbenchmark: Silesia och enwik8, mätt som andra packare mäts.

    python3 matning/standardkorpus.py silesia enwik8

Alla tal i repots övriga tabeller kommer ur Hugos egna filer. Det gör dem
ärliga men ojämförbara: ingen annan har samma foton. Den här mätningen kör i
stället de två korpusar hela facket använder —

  SILESIA   12 filer, 211 938 580 byte. Standarden i lzbench och i zstds och
            brotlis egna jämförelser. Redovisas som summa över filerna, och
            per fil, för att en packare kan vinna på text och förlora på
            databasdumpar.
  ENWIK8    de första 100 000 000 byten av engelska Wikipedia. Standarden i
            Large Text Compression Benchmark, där resultatet alltid anges i
            BITS PER BYTE och inte i procent.

Två regler skiljer den här mätningen från repots arkivtabeller, och båda
finns för att talen ska gå att jämföra utåt:

  EN TRÅD. zstd och xz får -T1. Alla publicerade tal på de här korpusarna är
  enkeltrådiga, och nvcm har ingen flertrådsläge att ställa upp med.
  EN FIL. Ingen tar, ingen dedup, ingen sortering — bara kodeken mot filen,
  som i lzbench. nvz som ARKIVPACKARE mäts separat med matning.py.

Varje körning kontrolleras genom att packa upp igen och jämföra SHA-256.
"""
import hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
SKRAP = Path(os.environ.get('NVPACK_SKRAP', ROT / '.skrap')) / 'standard'
NVZ = ROT / 'src' / 'nvcm'

# namn -> (packkommando, uppackkommando). {i}=infil, {o}=utfil.
KODEKAR = {
 'gzip -9':            ('gzip -9 -c {i} > {o}',               'gzip -dc {i} > {o}'),
 'bzip2 -9':           ('bzip2 -9 -c {i} > {o}',              'bzip2 -dc {i} > {o}'),
 'zstd -19':           ('zstd -19 -T1 -q -c {i} > {o}',       'zstd -dq -c {i} > {o}'),
 'zstd -22 --ultra':   ('zstd -22 --ultra -T1 -q -c {i} > {o}','zstd -dq -c {i} > {o}'),
 'brotli -q 11':       ('brotli -q 11 -c {i} > {o}',          'brotli -dc {i} > {o}'),
 'xz -9e':             ('xz -9e -T1 -c {i} > {o}',            'xz -dc {i} > {o}'),
 'nvcm':               (f'{NVZ} packa < {{i}} > {{o}}',      f'{NVZ} packaupp < {{i}} > {{o}}'),
}


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for bit in iter(lambda: f.read(1 << 20), b''):
            h.update(bit)
    return h.hexdigest()


def kor(cmd: str) -> float:
    t = time.monotonic()
    r = subprocess.run(['bash', '-o', 'pipefail', '-c', cmd], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{cmd}\n{r.stderr[-800:]}')
    return time.monotonic() - t


def mat_fil(fil: Path, kodek: str, avtryck: str) -> dict:
    SKRAP.mkdir(parents=True, exist_ok=True)
    packad, aterskapad = SKRAP / 'p.bin', SKRAP / 'a.bin'
    pack, uppack = KODEKAR[kodek]
    for f in (packad, aterskapad): f.unlink(missing_ok=True)
    ct = kor(pack.format(i=str(fil), o=str(packad)))
    cb = packad.stat().st_size
    dt = kor(uppack.format(i=str(packad), o=str(aterskapad)))
    lika = sha(aterskapad) == avtryck
    for f in (packad, aterskapad): f.unlink(missing_ok=True)
    rab = fil.stat().st_size
    return dict(kodek=kodek, byte=cb, packsek=round(ct, 2), uppacksek=round(dt, 2),
                kvot=round(rab / cb, 3), bpb=round(cb * 8 / rab, 4),
                packmibs=round(rab / 1048576 / max(ct, 1e-9), 1),
                uppackmibs=round(rab / 1048576 / max(dt, 1e-9), 1), exakt=lika)


def main():
    valda = sys.argv[1:] or ['silesia', 'enwik8']
    for korpus in valda:
        kat = ROT / 'korpus' / korpus
        filer = sorted(p for p in kat.iterdir() if p.is_file())
        utfil = ROT / 'resultat' / f'standard-{korpus}.json'
        data = json.loads(utfil.read_text()) if utfil.exists() else {}
        data['korpus'] = korpus
        data['rabyte'] = sum(p.stat().st_size for p in filer)
        data.setdefault('filer', {})
        print(f'\n{korpus}: {data["rabyte"]:,} byte i {len(filer)} filer'.replace(',', ' '))
        avtryck = {p.name: sha(p) for p in filer}
        for kodek in KODEKAR:
            if not shutil.which(kodek.split()[0]) and kodek != 'nvcm':
                print(f'  {kodek:<18} saknas på maskinen'); continue
            summa = sekp = seku = 0
            allt_exakt = True
            for p in filer:
                try:
                    r = mat_fil(p, kodek, avtryck[p.name])
                except Exception as e:
                    print(f'  {kodek:<18} {p.name}: FEL {e}'); allt_exakt = False; continue
                data['filer'].setdefault(p.name, {})[kodek] = r
                summa += r['byte']; sekp += r['packsek']; seku += r['uppacksek']
                allt_exakt &= r['exakt']
                utfil.parent.mkdir(exist_ok=True)
                utfil.write_text(json.dumps(data, indent=1, ensure_ascii=False))
            rab = data['rabyte']
            print(f'  {kodek:<18} {summa:>12,} byte  x{rab/summa:>6.3f}  '
                  f'{summa*8/rab:>5.3f} bpb  pack {rab/1048576/max(sekp,1e-9):>6.1f} MiB/s  '
                  f'upp {rab/1048576/max(seku,1e-9):>7.1f} MiB/s  '
                  f'{"exakt" if allt_exakt else "AVVIKELSE"}'.replace(',', ' '), flush=True)
            data.setdefault('summor', {})[kodek] = dict(
                byte=summa, packsek=round(sekp, 2), uppacksek=round(seku, 2),
                kvot=round(rab / summa, 3), bpb=round(summa * 8 / rab, 4), exakt=allt_exakt)
            utfil.write_text(json.dumps(data, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
