#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Measures what the gpu mode is worth — and writes the answer to results/gpu.json.

The question to be answered: does packing go faster with a gpu mode?  That
question cannot be answered with an opinion, so this script measures the
four levels the answer lives on:

  1. THE CEILING.  How fast can the gpu read data compared to the cpu?  The
     graphics are integrated and share RAM with the processor, so a single
     sweep over the data can never beat that ratio.  Everything else sits
     under that ceiling.
  2. THE KERNEL.  Same work, same answer, on gpu and on cpu: the blake2b
     hash the dedup uses, and the lz77 matching that is the only broadly
     parallel step in a packer.  The answers are compared bit for bit — a
     timing without an identical answer is not a measurement.
  3. THE CODEC.  nvlz (matching on gpu, coding on cpu) against the zstd
     curve and xz on exactly the same block.  This is where it is decided:
     a gpu kernel that beats my own cpu code can still lose to zstd, which
     does a smarter job.
  4. THE WHOLE ARCHIVE.  nvz in mode normal against mode gpu on a real corpus.

Run: python3 benchmark/gpu.py [corpus]   (default: corpus/kod)
"""
import json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = Path(os.environ.get('NVPACK_SCRATCH', ROOT / '.scratch')) / 'gpu-matning'
NVGPU = ROOT / 'src' / 'nvgpu'
HZ = ROOT / 'src' / 'hz'
BLOCK_MIB = 16            # size of the block the codecs are compared on


def run(cmd, pipefail=True, **kw):
    """pipefail=False is needed when a pipe is cut short on purpose:
    `tar | head -c` gives tar a SIGPIPE, and that is not an error but the
    whole point."""
    shell = ['bash'] + (['-o', 'pipefail'] if pipefail else []) + ['-c', cmd]
    r = subprocess.run(shell, capture_output=True, text=True, **kw)
    if r.returncode:
        raise RuntimeError(f'{cmd}\n{r.stderr[-1500:]}')
    return r


def timed(cmd):
    t = time.monotonic()
    r = run(cmd)
    return time.monotonic() - t, r


def best_of(cmd, rounds=2):
    """Fastest of n runs.  The slowest is always somebody else's load."""
    return min(timed(cmd)[0] for _ in range(rounds))


def number(text, pattern, group=1):
    m = re.search(pattern, text)
    return float(m.group(group).replace(',', '.')) if m else None


# --- 1. the ceiling ---------------------------------------------------------

def ceiling():
    # The patterns match nvgpu's output, which speaks Swedish — frozen.
    r = run(f'{NVGPU} band 1024')
    u = r.stdout
    return dict(
        gpu_mibs=number(u, r'gpu reads shared memory\s+[\d.]+ s\s+(\d+)'),
        cpu_mibs=number(u, r'cpu reads plain memory\s+[\d.]+ s\s+(\d+)'),
        cpu_delat_mibs=number(u, r'cpu reads shared memory\s+[\d.]+ s\s+(\d+)'),
    )


# --- 2. the kernels ---------------------------------------------------------

def hashing(corpora):
    out = {}
    for c in corpora:
        listing = SCRATCH / f'filer-{c.name}.txt'
        run(f'find {c} -type f | sort > {listing}')
        files = sum(1 for _ in open(listing))
        size = sum(p.stat().st_size for p in c.rglob('*') if p.is_file())
        # Warm the cache first: otherwise the disk is measured, not the hash.
        run(f'cat $(head -200 {listing}) > /dev/null || true', pipefail=False)
        g = SCRATCH / 'h.gpu'; h = SCRATCH / 'h.cpu'
        tg = best_of(f'{NVGPU} hasha --tyst < {listing} > {g} 2>/dev/null')
        tc = best_of(f'{NVGPU} hasha --tyst --cpu < {listing} > {h} 2>/dev/null')
        equal = run(f'cmp -s {g} {h} && echo yes || echo no').stdout.strip() == 'yes'
        out[c.name] = dict(filer=files, mib=round(size / 2**20, 1),
                           gpu_sek=round(tg, 3), cpu_sek=round(tc, 3),
                           gpu_mibs=round(size / 2**20 / tg), cpu_mibs=round(size / 2**20 / tc),
                           identiska=equal)
    return out


def matcher(block: Path):
    out = {}
    for chain in (16, 64, 256, 4096):
        r = run(f'{NVGPU} matcher {block} --tak {BLOCK_MIB} --kedja {chain} --nog 96')
        u = r.stdout
        out[str(chain)] = dict(
            cpu_sek=number(u, r'cpu search.*?([\d.]+) s'),
            gpu_sek=number(u, r'gpu search\s+([\d.]+) s'),
            gpu_kopiering_sek=number(u, r'gpu copy in\s+([\d.]+) s'),
            avvikelser=int(number(u, r'mismatches against cpu\s+(\d+)') or 0),
        )
    return out


# --- 3. the codecs ----------------------------------------------------------

# The keys are codec identifiers stored in results/gpu.json — frozen.
CODECS = {
 'nvlz (gpu)':   (f'{NVGPU} packa --tyst < {{i}} > {{o}}',        f'{NVGPU} packaupp < {{o}} > {{a}}'),
 'nvlz (cpu)':   (f'{NVGPU} packa --cpu --tyst < {{i}} > {{o}}',  f'{NVGPU} packaupp < {{o}} > {{a}}'),
 'zstd -3':      ('zstd -3 -T24 -q -f -o {o} {i}',                'zstd -dq -f -o {a} {o}'),
 'zstd -6':      ('zstd -6 -T24 -q -f -o {o} {i}',                'zstd -dq -f -o {a} {o}'),
 'zstd -12':     ('zstd -12 -T24 -q -f -o {o} {i}',               'zstd -dq -f -o {a} {o}'),
 'zstd -19':     ('zstd -19 --long=27 -T24 -q -f -o {o} {i}',     'zstd -dq --long=27 -f -o {a} {o}'),
 'xz -9e':       ('xz -9e -T24 -c {i} > {o}',                     'xz -dc -T0 {o} > {a}'),
}


def codecs(block: Path):
    out = {}
    raw = block.stat().st_size
    for name, (pack, unpack) in CODECS.items():
        o = SCRATCH / 'ut.bin'; a = SCRATCH / 'ater.bin'
        tp = best_of(pack.format(i=block, o=o))
        size = o.stat().st_size
        tu = best_of(unpack.format(o=o, a=a, i=block))
        equal = run(f'cmp -s {block} {a} && echo yes || echo no').stdout.strip() == 'yes'
        out[name] = dict(byte=size, kvot=round(raw / size, 2),
                         packsek=round(tp, 2), uppacksek=round(tu, 2), identisk=equal)
    return out


# --- 4. the whole archive ---------------------------------------------------

def archive(corpus: Path):
    out = {}
    for mode in ('normal', 'gpu'):
        ark = SCRATCH / f'ark-{mode}.nv'
        unpack = SCRATCH / f'upp-{mode}'
        shutil.rmtree(unpack, ignore_errors=True)
        ark.unlink(missing_ok=True)
        tp, _ = timed(f'{HZ} packa {corpus} {ark} -q --läge {mode}')
        unpack.mkdir(parents=True)
        tu, _ = timed(f'{HZ} packa-upp {ark} {unpack} -q')
        equal = run(f'diff -rq --no-dereference {corpus} {unpack}/{corpus.name} '
                    f'>/dev/null 2>&1 && echo yes || echo no').stdout.strip() == 'yes'
        listing = run(f'{HZ} lista {ark}').stdout
        out[mode] = dict(byte=ark.stat().st_size, packsek=round(tp, 1), uppacksek=round(tu, 1),
                         identisk=equal,
                         kodekar=sorted(set(re.findall(r'\b(nvlz|xz9e|zstd19|zstd3|zstd22|nvcm|ra)\b',
                                                       listing))))
    return out


def main():
    corpus = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'corpus' / 'kod'
    if not corpus.is_dir():
        raise SystemExit(f'no corpus at {corpus}')
    SCRATCH.mkdir(parents=True, exist_ok=True)
    block = SCRATCH / 'block.bin'
    if not block.exists() or block.stat().st_size < BLOCK_MIB * 2**20:
        run(f'tar -C {corpus.parent} -cf - {corpus.name} 2>/dev/null '
            f'| head -c {BLOCK_MIB * 2**20} > {block}', pipefail=False)

    # The dict keys are JSON field names in results/gpu.json — frozen.
    result = dict(maskin=run('lscpu | grep "Model name" | cut -d: -f2 | xargs').stdout.strip(),
                  enhet=run(f'{NVGPU} enhet | head -1 | cut -d: -f2 | xargs').stdout.strip(),
                  last=open('/proc/loadavg').read().split()[0],
                  block_mib=BLOCK_MIB, korpus=corpus.name)
    print('1/4 the ceiling ...', flush=True);       result['tak'] = ceiling()
    print('2/4 the hash ...', flush=True);          result['hash'] = hashing([corpus, ROOT / 'corpus' / 'png'])
    print('2/4 the matcher ...', flush=True);       result['matchare'] = matcher(block)
    print('3/4 the codecs ...', flush=True);        result['kodek'] = codecs(block)
    print('4/4 the whole archive ...', flush=True); result['arkiv'] = archive(corpus)

    target = ROOT / 'results' / 'gpu.json'
    target.write_text(json.dumps(result, ensure_ascii=False, indent=1) + '\n')
    print(f'wrote {target}')


if __name__ == '__main__':
    main()
