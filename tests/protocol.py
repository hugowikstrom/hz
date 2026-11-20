#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reconstruction protocol: pack, unpack, and prove with md5 that every
file came back exactly.

    python3 tests/protocol.py                # all corpora
    python3 tests/protocol.py logg kod       # only the named ones

Each corpus gets one full round trip in the default mode (small):

  1. md5 manifest BEFORE: one line per file, in `md5sum` format, saved to
     results/protocol/<corpus>-fore.md5
  2. pack to a temporary archive; the archive's size and md5 are recorded
  3. unpack into an empty directory
  4. md5 manifest AFTER, saved next to it: <corpus>-efter.md5
  5. line-by-line comparison — and in addition `md5sum -c` runs the BEFORE
     manifest against the UNPACKED tree, so the check is done by an
     independent tool and not by the same code that wrote the manifests

The manifests are committed to the repo. The point is that anyone can
inspect them and re-run the check themselves:

    cd <unpacked tree> && md5sum -c <corpus>-fore.md5

The protocol (results/protocol/protocol.json) carries everything the
report shows. Its field names and the manifest file names are frozen —
report/protocol-report.py reads them.
"""
import hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HZ = ROOT / 'src' / 'hz'
OUT = ROOT / 'results' / 'protocol'
SCRATCH = Path(os.environ.get('NVPACK_SCRATCH', ROOT / '.scratch')) / 'protocol'

CORPORA = ['kod', 'gif', 'foto', 'arkiv', 'logg', 'png', 'film', 'silesia', 'enwik8']


def md5_file(p: Path) -> str:
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for piece in iter(lambda: f.read(1 << 20), b''):
            h.update(piece)
    return h.hexdigest()


def manifest(root: Path) -> list[str]:
    """`md5sum` lines for every file under root, in stable order."""
    lines = []
    for p in sorted(root.rglob('*')):
        if p.is_file() and not p.is_symlink():
            lines.append(f'{md5_file(p)}  {p.relative_to(root)}')
    return lines


def run(args):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{" ".join(str(a) for a in args[:3])}: {r.stderr[-800:]}')
    return r


def record_corpus(name: str) -> dict:
    corpus = ROOT / 'corpus' / name
    SCRATCH.mkdir(parents=True, exist_ok=True)
    OUT.mkdir(parents=True, exist_ok=True)
    archive = SCRATCH / f'{name}.nv'
    out = SCRATCH / f'upp-{name}'
    archive.unlink(missing_ok=True)
    shutil.rmtree(out, ignore_errors=True)

    print(f'{name}: manifest before ...', flush=True)
    before = manifest(corpus)
    rabyte = sum(p.stat().st_size for p in corpus.rglob('*') if p.is_file())
    (OUT / f'{name}-fore.md5').write_text('\n'.join(before) + '\n')

    print(f'{name}: packing (mode small) ...', flush=True)
    t0 = time.monotonic()
    run([HZ, 'pack', str(corpus), str(archive), '-q'])
    packsek = round(time.monotonic() - t0, 1)
    arkivbyte = archive.stat().st_size
    archive_md5 = md5_file(archive)

    print(f'{name}: unpacking ...', flush=True)
    t0 = time.monotonic()
    run([HZ, 'unpack', str(archive), str(out), '-q'])
    uppsek = round(time.monotonic() - t0, 1)

    print(f'{name}: manifest after ...', flush=True)
    out_corpus = out / name
    after = manifest(out_corpus)
    (OUT / f'{name}-efter.md5').write_text('\n'.join(after) + '\n')

    equal = before == after
    # Independent check: the system's md5sum against the before manifest.
    r = subprocess.run(['md5sum', '-c', '--quiet', str(OUT / f'{name}-fore.md5')],
                       cwd=out_corpus, capture_output=True, text=True)
    md5sum_ok = (r.returncode == 0)

    deviations = [f for f, e in zip(before, after) if f != e][:5] if not equal else []
    # Frozen field names — report/protocol-report.py reads them.
    res = dict(
        korpus=name, filer=len(before), rabyte=rabyte,
        arkivbyte=arkivbyte, arkiv_md5=archive_md5,
        packsek=packsek, uppacksek=uppsek,
        manifest_fore_md5=hashlib.md5('\n'.join(before).encode()).hexdigest(),
        manifest_efter_md5=hashlib.md5('\n'.join(after).encode()).hexdigest(),
        identiska=equal, md5sum_c_ok=md5sum_ok,
        exempel=before[:3], avvikelser=deviations,
    )
    shutil.rmtree(out, ignore_errors=True)
    archive.unlink(missing_ok=True)
    status = 'IDENTICAL' if (equal and md5sum_ok) else 'DIFFERS!'
    print(f'{name}: {len(before)} files, manifest {res["manifest_fore_md5"][:12]}… -> '
          f'{res["manifest_efter_md5"][:12]}…  {status}', flush=True)
    return res


def main():
    chosen = sys.argv[1:] or CORPORA
    outfile = OUT / 'protocol.json'
    OUT.mkdir(parents=True, exist_ok=True)
    data = json.loads(outfile.read_text()) if outfile.exists() else {}
    data['maskin'] = 'Ryzen AI 9 HX 470, 24 threads, NVMe'
    data['nvz'] = subprocess.run([HZ, '--version'], capture_output=True,
                                 text=True).stdout.strip()
    data['datum'] = time.strftime('%Y-%m-%d %H:%M')
    data.setdefault('korpusar', {})
    for name in chosen:
        data['korpusar'][name] = record_corpus(name)
        outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))
    all_ok = all(k['identiska'] and k['md5sum_c_ok'] for k in data['korpusar'].values())
    print('\nCONCLUSION:', 'all corpora bit-exact' if all_ok else 'DEVIATIONS PRESENT')


if __name__ == '__main__':
    main()
