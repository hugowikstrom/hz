#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# [EN] The GPU measurement series: bandwidth ceiling, kernel-vs-CPU parity,
# and the nvlz codec timings behind the README GPU section.
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
"""Mäter vad gpu-läget är värt — och skriver svaret till resultat/gpu.json.

Frågan som skulle besvaras: går packningen snabbare med ett gpu-läge? Den
frågan går inte att svara på med en åsikt, så det här skriptet mäter de fyra
nivåer svaret bor på:

  1. TAKET. Hur fort kan gpu:n läsa data jämfört med cpu:n? Grafiken är
     integrerad och delar RAM med processorn, så ett enda svep över data kan
     aldrig gå fortare än den kvoten. Allt annat är under det taket.
  2. KÄRNAN. Samma arbete, samma svar, på gpu och på cpu: blake2b-hashen som
     dedupen använder, och lz77-matchningen som är det enda brett parallella
     steget i en packare. Svaren jämförs bit för bit — en tid utan identiskt
     svar är ingen mätning.
  3. KODEKEN. nvlz (matchning på gpu, kodning på cpu) mot zstd-kurvan och xz
     på exakt samma block. Det är här det avgörs: en gpu-kärna som vinner mot
     min egen cpu-kod kan ändå förlora mot zstd, som gör ett smartare jobb.
  4. HELA ARKIVET. nvz i läge normal mot läge gpu på en riktig korpus.

Kör: python3 matning/gpu-matning.py [korpus]   (standard: korpus/kod)
"""
import json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
SKRAP = Path(os.environ.get('NVPACK_SKRAP', ROT / '.skrap')) / 'gpu-matning'
NVGPU = ROT / 'src' / 'nvgpu'
HZ = ROT / 'src' / 'hz'
BLOCK_MIB = 16            # så stort block kodekarna jämförs på


def kör(cmd, pipefail=True, **kw):
    """pipefail=False behövs när ett rör avsiktligt avbryts: `tar | head -c`
    ger tar ett SIGPIPE, och det är inte ett fel utan hela poängen."""
    skal = ['bash'] + (['-o', 'pipefail'] if pipefail else []) + ['-c', cmd]
    r = subprocess.run(skal, capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f'{cmd}\n{r.stderr[-1500:]}')
    return r


def tid(cmd):
    t = time.monotonic()
    r = kör(cmd)
    return time.monotonic() - t, r


def bäst_av(cmd, varv=2):
    """Snabbaste av n körningar. Den långsammaste är alltid någon annans last."""
    return min(tid(cmd)[0] for _ in range(varv))


def tal(text, mönster, grupp=1):
    m = re.search(mönster, text)
    return float(m.group(grupp).replace(',', '.')) if m else None


# --- 1. taket -------------------------------------------------------------

def taket():
    r = kör(f'{NVGPU} band 1024')
    u = r.stdout
    return dict(
        gpu_mibs=tal(u, r'gpu läser delat minne\s+[\d.]+ s\s+(\d+)'),
        cpu_mibs=tal(u, r'cpu läser vanligt minne\s+[\d.]+ s\s+(\d+)'),
        cpu_delat_mibs=tal(u, r'cpu läser delat minne\s+[\d.]+ s\s+(\d+)'),
    )


# --- 2. kärnorna ----------------------------------------------------------

def hashen(korpusar):
    ut = {}
    for k in korpusar:
        lista = SKRAP / f'filer-{k.name}.txt'
        kör(f'find {k} -type f | sort > {lista}')
        filer = sum(1 for _ in open(lista))
        byte = sum(p.stat().st_size for p in k.rglob('*') if p.is_file())
        # Cachen värms först: annars mäts disken, inte hashen.
        kör(f'cat $(head -200 {lista}) > /dev/null || true', pipefail=False)
        g = SKRAP / 'h.gpu'; c = SKRAP / 'h.cpu'
        tg = bäst_av(f'{NVGPU} hasha --tyst < {lista} > {g} 2>/dev/null')
        tc = bäst_av(f'{NVGPU} hasha --tyst --cpu < {lista} > {c} 2>/dev/null')
        lika = kör(f'cmp -s {g} {c} && echo ja || echo nej').stdout.strip() == 'ja'
        ut[k.name] = dict(filer=filer, mib=round(byte / 2**20, 1),
                          gpu_sek=round(tg, 3), cpu_sek=round(tc, 3),
                          gpu_mibs=round(byte / 2**20 / tg), cpu_mibs=round(byte / 2**20 / tc),
                          identiska=lika)
    return ut


def matcharen(block: Path):
    ut = {}
    for kedja in (16, 64, 256, 4096):
        r = kör(f'{NVGPU} matcher {block} --tak {BLOCK_MIB} --kedja {kedja} --nog 96')
        u = r.stdout
        ut[str(kedja)] = dict(
            cpu_sek=tal(u, r'cpu-sökning.*?([\d.]+) s'),
            gpu_sek=tal(u, r'gpu-sökning\s+([\d.]+) s'),
            gpu_kopiering_sek=tal(u, r'gpu-kopiering in\s+([\d.]+) s'),
            avvikelser=int(tal(u, r'avvikelser mot cpu\s+(\d+)') or 0),
        )
    return ut


# --- 3. kodekarna ---------------------------------------------------------

KODEKAR = {
 'nvlz (gpu)':   (f'{NVGPU} packa --tyst < {{i}} > {{o}}',        f'{NVGPU} packaupp < {{o}} > {{a}}'),
 'nvlz (cpu)':   (f'{NVGPU} packa --cpu --tyst < {{i}} > {{o}}',  f'{NVGPU} packaupp < {{o}} > {{a}}'),
 'zstd -3':      ('zstd -3 -T24 -q -f -o {o} {i}',                'zstd -dq -f -o {a} {o}'),
 'zstd -6':      ('zstd -6 -T24 -q -f -o {o} {i}',                'zstd -dq -f -o {a} {o}'),
 'zstd -12':     ('zstd -12 -T24 -q -f -o {o} {i}',               'zstd -dq -f -o {a} {o}'),
 'zstd -19':     ('zstd -19 --long=27 -T24 -q -f -o {o} {i}',     'zstd -dq --long=27 -f -o {a} {o}'),
 'xz -9e':       ('xz -9e -T24 -c {i} > {o}',                     'xz -dc -T0 {o} > {a}'),
}


def kodekarna(block: Path):
    ut = {}
    rå = block.stat().st_size
    for namn, (pack, upp) in KODEKAR.items():
        o = SKRAP / 'ut.bin'; a = SKRAP / 'åter.bin'
        tp = bäst_av(pack.format(i=block, o=o))
        storlek = o.stat().st_size
        tu = bäst_av(upp.format(o=o, a=a, i=block))
        lika = kör(f'cmp -s {block} {a} && echo ja || echo nej').stdout.strip() == 'ja'
        ut[namn] = dict(byte=storlek, kvot=round(rå / storlek, 2),
                        packsek=round(tp, 2), uppacksek=round(tu, 2), identisk=lika)
    return ut


# --- 4. hela arkivet ------------------------------------------------------

def arkivet(korpus: Path):
    ut = {}
    for läge in ('normal', 'gpu'):
        ark = SKRAP / f'ark-{läge}.nv'
        upp = SKRAP / f'upp-{läge}'
        shutil.rmtree(upp, ignore_errors=True)
        ark.unlink(missing_ok=True)
        tp, _ = tid(f'{HZ} packa {korpus} {ark} -q --läge {läge}')
        upp.mkdir(parents=True)
        tu, _ = tid(f'{HZ} packa-upp {ark} {upp} -q')
        lika = kör(f'diff -rq --no-dereference {korpus} {upp}/{korpus.name} '
                   f'>/dev/null 2>&1 && echo ja || echo nej').stdout.strip() == 'ja'
        kodekar = kör(f'{HZ} lista {ark}').stdout
        ut[läge] = dict(byte=ark.stat().st_size, packsek=round(tp, 1), uppacksek=round(tu, 1),
                        identisk=lika,
                        kodekar=sorted(set(re.findall(r'\b(nvlz|xz9e|zstd19|zstd3|zstd22|nvcm|ra)\b',
                                                      kodekar))))
    return ut


def main():
    korpus = Path(sys.argv[1]) if len(sys.argv) > 1 else ROT / 'korpus' / 'kod'
    if not korpus.is_dir():
        raise SystemExit(f'ingen korpus på {korpus}')
    SKRAP.mkdir(parents=True, exist_ok=True)
    block = SKRAP / 'block.bin'
    if not block.exists() or block.stat().st_size < BLOCK_MIB * 2**20:
        kör(f'tar -C {korpus.parent} -cf - {korpus.name} 2>/dev/null '
            f'| head -c {BLOCK_MIB * 2**20} > {block}', pipefail=False)

    svar = dict(maskin=kör('lscpu | grep "Model name" | cut -d: -f2 | xargs').stdout.strip(),
                enhet=kör(f'{NVGPU} enhet | head -1 | cut -d: -f2 | xargs').stdout.strip(),
                last=open('/proc/loadavg').read().split()[0],
                block_mib=BLOCK_MIB, korpus=korpus.name)
    print('1/4 taket ...', flush=True);      svar['tak'] = taket()
    print('2/4 hashen ...', flush=True);     svar['hash'] = hashen([korpus, ROT / 'korpus' / 'png'])
    print('2/4 matcharen ...', flush=True);  svar['matchare'] = matcharen(block)
    print('3/4 kodekarna ...', flush=True);  svar['kodek'] = kodekarna(block)
    print('4/4 hela arkivet ...', flush=True); svar['arkiv'] = arkivet(korpus)

    mål = ROT / 'resultat' / 'gpu.json'
    mål.write_text(json.dumps(svar, ensure_ascii=False, indent=1) + '\n')
    print(f'skrev {mål}')


if __name__ == '__main__':
    main()
