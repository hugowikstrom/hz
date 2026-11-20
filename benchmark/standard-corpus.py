#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Standard benchmark: Silesia and enwik8, measured the way other packers are.

    python3 benchmark/standard-corpus.py silesia enwik8

Every number in the repo's other tables comes from Hugo's own files.  That
makes them honest but incomparable: nobody else has the same photos.  This
measurement instead runs the two corpora the whole field uses —

  SILESIA   12 files, 211 938 580 bytes.  The standard in lzbench and in
            zstd's and brotli's own comparisons.  Reported as a sum over the
            files, and per file, because a packer can win on text and lose
            on database dumps.
  ENWIK8    the first 100 000 000 bytes of English Wikipedia.  The standard
            in the Large Text Compression Benchmark, where the result is
            always stated in BITS PER BYTE, not in percent.

Two rules separate this measurement from the repo's archive tables, and both
exist so the numbers can be compared externally:

  ONE THREAD.  zstd and xz get -T1.  All published numbers on these corpora
  are single-threaded, and nvcm has no multi-threaded mode to field.
  ONE FILE.  No tar, no dedup, no sorting — just the codec against the file,
  as in lzbench.  nvz as an ARCHIVE PACKER is measured separately with
  measure.py.

Every run is verified by unpacking again and comparing SHA-256.
"""
import hashlib, json, os, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = Path(os.environ.get('NVPACK_SCRATCH', ROOT / '.scratch')) / 'standard'
NVCM = ROOT / 'src' / 'nvcm'

# name -> (pack command, unpack command).  {i}=input file, {o}=output file.
# The keys are codec identifiers stored in results/standard-*.json — frozen.
# nvcm's subcommands (packa, packaupp) are the tool's actual CLI.
CODECS = {
 'gzip -9':            ('gzip -9 -c {i} > {o}',               'gzip -dc {i} > {o}'),
 'bzip2 -9':           ('bzip2 -9 -c {i} > {o}',              'bzip2 -dc {i} > {o}'),
 'zstd -19':           ('zstd -19 -T1 -q -c {i} > {o}',       'zstd -dq -c {i} > {o}'),
 'zstd -22 --ultra':   ('zstd -22 --ultra -T1 -q -c {i} > {o}','zstd -dq -c {i} > {o}'),
 'brotli -q 11':       ('brotli -q 11 -c {i} > {o}',          'brotli -dc {i} > {o}'),
 'xz -9e':             ('xz -9e -T1 -c {i} > {o}',            'xz -dc {i} > {o}'),
 'nvcm':               (f'{NVCM} packa < {{i}} > {{o}}',      f'{NVCM} packaupp < {{i}} > {{o}}'),
}


def sha(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def run(cmd: str) -> float:
    t = time.monotonic()
    r = subprocess.run(['bash', '-o', 'pipefail', '-c', cmd], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f'{cmd}\n{r.stderr[-800:]}')
    return time.monotonic() - t


def measure_file(path: Path, codec: str, digest: str) -> dict:
    SCRATCH.mkdir(parents=True, exist_ok=True)
    packed, restored = SCRATCH / 'p.bin', SCRATCH / 'a.bin'
    pack, unpack = CODECS[codec]
    for f in (packed, restored): f.unlink(missing_ok=True)
    ct = run(pack.format(i=str(path), o=str(packed)))
    cb = packed.stat().st_size
    dt = run(unpack.format(i=str(packed), o=str(restored)))
    equal = sha(restored) == digest
    for f in (packed, restored): f.unlink(missing_ok=True)
    raw = path.stat().st_size
    # The dict keys are JSON field names in results/standard-*.json — frozen.
    return dict(kodek=codec, byte=cb, packsek=round(ct, 2), uppacksek=round(dt, 2),
                kvot=round(raw / cb, 3), bpb=round(cb * 8 / raw, 4),
                packmibs=round(raw / 1048576 / max(ct, 1e-9), 1),
                uppackmibs=round(raw / 1048576 / max(dt, 1e-9), 1), exakt=equal)


def main():
    chosen = sys.argv[1:] or ['silesia', 'enwik8']
    for corpus in chosen:
        cdir = ROOT / 'corpus' / corpus
        files = sorted(p for p in cdir.iterdir() if p.is_file())
        outfile = ROOT / 'results' / f'standard-{corpus}.json'
        data = json.loads(outfile.read_text()) if outfile.exists() else {}
        data['corpus'] = corpus
        data['rabyte'] = sum(p.stat().st_size for p in files)
        data.setdefault('filer', {})
        print(f'\n{corpus}: {data["rabyte"]:,} bytes in {len(files)} files'.replace(',', ' '))
        digests = {p.name: sha(p) for p in files}
        for codec in CODECS:
            if not shutil.which(codec.split()[0]) and codec != 'nvcm':
                print(f'  {codec:<18} missing on this machine'); continue
            total = secp = secu = 0
            all_exact = True
            for p in files:
                try:
                    r = measure_file(p, codec, digests[p.name])
                except Exception as e:
                    print(f'  {codec:<18} {p.name}: ERROR {e}'); all_exact = False; continue
                data['filer'].setdefault(p.name, {})[codec] = r
                total += r['byte']; secp += r['packsek']; secu += r['uppacksek']
                all_exact &= r['exakt']
                outfile.parent.mkdir(exist_ok=True)
                outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))
            raw = data['rabyte']
            print(f'  {codec:<18} {total:>12,} bytes  x{raw/total:>6.3f}  '
                  f'{total*8/raw:>5.3f} bpb  pack {raw/1048576/max(secp,1e-9):>6.1f} MiB/s  '
                  f'unpack {raw/1048576/max(secu,1e-9):>7.1f} MiB/s  '
                  f'{"exact" if all_exact else "MISMATCH"}'.replace(',', ' '), flush=True)
            data.setdefault('summor', {})[codec] = dict(
                byte=total, packsek=round(secp, 2), uppacksek=round(secu, 2),
                kvot=round(raw / total, 3), bpb=round(total * 8 / raw, 4), exakt=all_exact)
            outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
