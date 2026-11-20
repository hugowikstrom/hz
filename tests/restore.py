#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Reconstruction test: packs a corpus, unpacks it and demands every byte,
every permission and every timestamp back.

    python3 tests/restore.py gif arkiv logg film
    python3 tests/restore.py --damage gif       # damage tests only

Why a separate test when the measurement harness already runs `diff -rq`?
Because diff answers a different question. diff compares the contents of
files present on both sides; it says nothing about permissions, times,
empty directories, or whether a duplicate came back as two real files or
as one hard link. And it leaves no receipt: after a clean diff you know it
went well, but you have nothing to show. Here a fingerprint list is
computed instead — SHA-256 per file plus metadata — before and after, and
the lists must be identical.

The damage tests cover the other half of "can nvz reconstruct the data":
a packer that silently hands back the WRONG data is worse than one that
refuses.

The JSON field names in results/restoration.json are frozen — the report
generators read them.
"""
from __future__ import annotations
import hashlib, json, os, shutil, stat, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HZ = str(ROOT / 'src' / 'hz')
SCRATCH = Path(os.environ.get('NVPACK_SCRATCH', ROOT / '.scratch')) / 'aterskapning'


def fingerprint(root: Path) -> dict:
    """Everything about the tree that an unpack must give back."""
    entries = {}
    for p in sorted(root.rglob('*')):
        rel = str(p.relative_to(root))
        st = p.lstat()
        if stat.S_ISLNK(st.st_mode):
            entries[rel] = {'kind': 'link', 'target': os.readlink(p)}
        elif p.is_dir():
            entries[rel] = {'kind': 'dir', 'mode': stat.S_IMODE(st.st_mode)}
        else:
            h = hashlib.sha256()
            with open(p, 'rb') as f:
                for piece in iter(lambda: f.read(1 << 20), b''):
                    h.update(piece)
            entries[rel] = {'kind': 'file', 'bytes': st.st_size, 'mode': stat.S_IMODE(st.st_mode),
                            'mtime_ns': st.st_mtime_ns, 'sha256': h.hexdigest(),
                            'links': st.st_nlink}
    return entries


def compare(before: dict, after: dict) -> list[str]:
    deviations = []
    for rel in sorted(set(before) | set(after)):
        a, b = before.get(rel), after.get(rel)
        if a is None: deviations.append(f'extra in the unpack: {rel}'); continue
        if b is None: deviations.append(f'missing after unpack: {rel}'); continue
        for key in a:
            # The hard-link count is not compared directly: the corpora ARE
            # hard links into the home directory, and an unpack must give
            # back independent files. The requirement is instead that the
            # unpack leaves no links behind.
            if key == 'links':
                if b.get('links', 1) != 1:
                    deviations.append(f'{rel}: unpacked file has {b["links"]} hard links')
                continue
            if a[key] != b.get(key):
                deviations.append(f'{rel}: {key} {a[key]!r} -> {b.get(key)!r}')
    return deviations


def run(args, expect_failure=False):
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode and not expect_failure:
        raise RuntimeError(f'{" ".join(args[:3])}: {r.stderr[-1500:]}')
    return r


def test_corpus(name: str, mode: str = 'normal') -> dict:
    corpus = ROOT / 'corpus' / name
    SCRATCH.mkdir(parents=True, exist_ok=True)
    archive = SCRATCH / f'{name}-{mode}.nv'
    out = SCRATCH / f'upp-{name}'
    archive.unlink(missing_ok=True)
    shutil.rmtree(out, ignore_errors=True)

    before = fingerprint(corpus)
    rabyte = sum(p['bytes'] for p in before.values() if p['kind'] == 'file')
    # Packed without -q: the lines about dedup and about which codec each
    # block got are half the answer to WHY a corpus compresses as it does.
    t0 = time.monotonic()
    r = run([HZ, 'pack', str(corpus), str(archive), '--mode', mode])
    packsek = time.monotonic() - t0
    pack_output = [line.strip() for line in r.stdout.splitlines() if line.strip()]
    out.mkdir(parents=True)
    t0 = time.monotonic(); run([HZ, 'unpack', str(archive), str(out), '-q'])
    uppacksek = time.monotonic() - t0
    after = fingerprint(out / name)
    deviations = compare(before, after)

    # `list` must report as many entries as the tree has — directories and
    # links included. If that does not hold, the register does not carry the
    # whole tree, even if the files that DID come back happened to be real.
    listing = run([HZ, 'list', str(archive)]).stdout
    files_before = sum(1 for p in before.values() if p['kind'] == 'file')
    listed_entries = None
    for line in listing.splitlines():
        if ' entries,' in line:
            listed_entries = int(line.split(' entries,')[0].split()[-1])
    # +1: the corpus's own root directory is an entry in the archive but
    # not in the fingerprint.
    entries_before = len(before) + 1
    if listed_entries != entries_before:
        deviations.append(f'list: {listed_entries} entries in the register, {entries_before} in the tree')

    # The field names below are frozen: report/evaluation.py reads them.
    res = dict(korpus=name, filer=files_before, rabyte=rabyte,
               arkivbyte=archive.stat().st_size, packsek=round(packsek, 2),
               uppacksek=round(uppacksek, 2), avvikelser=deviations,
               identisk=not deviations, poster=entries_before,
               listade_poster=listed_entries, packutdata=pack_output,
               listning=listing.strip().splitlines())
    res['läge'] = mode
    shutil.rmtree(out, ignore_errors=True)
    archive.unlink(missing_ok=True)
    return res


def test_damage(name: str) -> list[dict]:
    """An archive with a piece destroyed may fail — but never return wrong data."""
    corpus = ROOT / 'corpus' / name
    SCRATCH.mkdir(parents=True, exist_ok=True)
    orig = SCRATCH / f'skada-{name}.nv'
    orig.unlink(missing_ok=True)
    run([HZ, 'pack', str(corpus), str(orig), '-q'])
    clean = fingerprint(corpus)
    data = orig.read_bytes()
    results = []

    cases = [
        ('magic replaced',        lambda b: b'XXXXXX\x01' + b[7:]),
        ('byte in a block body',  lambda b: b[:len(b)//3] + bytes([b[len(b)//3] ^ 0x40]) + b[len(b)//3+1:]),
        ('last kilobyte gone',    lambda b: b[:-1024]),
        ('register pointer zeroed', lambda b: b[:-16] + b'\x00'*16),
        ('one byte in the register', lambda b: b[:-64] + bytes([b[-64] ^ 0xff]) + b[-63:]),
    ]
    for label, damage in cases:
        broken = SCRATCH / 'trasig.nv'
        broken.write_bytes(damage(data))
        out = SCRATCH / 'upp-trasig'
        shutil.rmtree(out, ignore_errors=True); out.mkdir(parents=True)
        r = run([HZ, 'unpack', str(broken), str(out), '-q'], expect_failure=True)
        target = out / name
        if r.returncode != 0:
            outcome, passed = 'refused', True
        elif not target.exists():
            outcome, passed = 'produced nothing', True
        else:
            deviations = compare(clean, fingerprint(target))
            if deviations:
                outcome, passed = f'RETURNED WRONG DATA ({len(deviations)} deviations)', False
            else:
                outcome, passed = 'unharmed (the damage sat in slack)', True
        # Frozen field names — report/evaluation.py reads them.
        results.append({'skada': label, 'utfall': outcome, 'godkänt': passed,
                        'felkod': r.returncode})
        shutil.rmtree(out, ignore_errors=True)
        broken.unlink(missing_ok=True)
    orig.unlink(missing_ok=True)
    return results


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    damage_only = '--damage' in sys.argv or '--skada' in sys.argv
    corpora = args or ['gif', 'arkiv', 'logg', 'film']
    outfile = ROOT / 'results' / 'restoration.json'
    data = json.loads(outfile.read_text()) if outfile.exists() else {}
    data.setdefault('korpusar', {}); data.setdefault('skada', {})

    if not damage_only:
        for name in corpora:
            print(f'{name}: computing fingerprints and packing...', flush=True)
            r = test_corpus(name)
            data['korpusar'][name] = r
            ratio = r['rabyte'] / r['arkivbyte'] if r['arkivbyte'] else 0
            print(f'  {r["filer"]} files  {r["rabyte"]/1048576:.1f} -> '
                  f'{r["arkivbyte"]/1048576:.1f} MiB (x{ratio:.2f})  '
                  f'pack {r["packsek"]:.1f}s  unpack {r["uppacksek"]:.1f}s  '
                  f'{"ALL RESTORED" if r["identisk"] else "DEVIATIONS"}', flush=True)
            for a in r['avvikelser'][:10]: print('    ', a)
            outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))

    name = corpora[0]
    print(f'damage tests on {name}:', flush=True)
    data['skada'][name] = test_damage(name)
    for p in data['skada'][name]:
        print(f'  {p["skada"]:<26} {p["utfall"]:<40} {"ok" if p["godkänt"] else "FAIL"}')
    outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))


if __name__ == '__main__':
    main()
