#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Measurement harness: run a set of packers over a corpus and record the result.

Every packer gets the same corpus and is measured on three numbers: size,
pack time, unpack time.  Output goes to results/<corpus>.json so the tables
can be rebuilt without repacking.  Everything written lands in the scratch
directory, never in the corpus.
"""
import hashlib, json, os, re, shutil, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCRATCH = Path(os.environ.get('NVPACK_SCRATCH', '/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/nvz-matning'))

# Each entry: name -> (pack command, unpack command).  {c}=corpus directory,
# {cd}=its parent, {cn}=its basename, {a}=archive file, {u}=unpack directory.
# Commands run in a shell; the corpus is always packed by directory name,
# never by absolute path.  The keys are measurement identifiers stored in
# results/*.json — frozen, do not rename.  The hz subcommands and flags
# (packa, packa-upp, --läge, ...) are the tool's actual CLI.
PACKERS = {
 'tar (okomprimerad)': ("tar -C {cd} -cf {a} {cn}", "tar -C {u} -xf {a}"),
 'tar.gz -6':          ("tar -C {cd} -cf - {cn} | gzip -6 > {a}", "tar -C {u} -xzf {a}"),
 'tar.gz -9 (pigz)':   ("tar -C {cd} -cf - {cn} | pigz -9 -p 24 > {a}", "tar -C {u} -xzf {a}"),
 'tar.bz2 -9':         ("tar -C {cd} -cf - {cn} | bzip2 -9 > {a}", "tar -C {u} -xjf {a}"),
 'tar.zst -3 (dagens)':("tar -C {cd} -cf - {cn} | zstd -3 -T0 -q -o {a}", "zstd -dq -c {a} | tar -C {u} -xf -"),
 'tar.zst -19 --long': ("tar -C {cd} -cf - {cn} | zstd -19 -T0 --long=27 -q -o {a}", "zstd -dq --long=27 -c {a} | tar -C {u} -xf -"),
 'tar.zst -22 --ultra':("tar -C {cd} -cf - {cn} | zstd -22 --ultra -T0 --long=31 -q -o {a}", "zstd -dq --long=31 -c {a} | tar -C {u} -xf -"),
 'tar.xz -9e':         ("tar -C {cd} -cf - {cn} | xz -9e -T0 -c > {a}", "xz -dc -T0 {a} | tar -C {u} -xf -"),
 'nvz snabb':       ("{nv} packa {c} {a} -q --läge snabb", "{nv} packa-upp {a} {u} -q"),
 'nvz normal':      ("{nv} packa {c} {a} -q --läge normal", "{nv} packa-upp {a} {u} -q"),
 'nvz liten':       ("{nv} packa {c} {a} -q --läge liten", "{nv} packa-upp {a} {u} -q"),
 'nvz utan knep':   ("{nv} packa {c} {a} -q --läge normal --utan-dedup --utan-omvandling",
                        "{nv} packa-upp {a} {u} -q"),
}

def run(cmd):
    t = time.monotonic()
    r = subprocess.run(['bash','-o','pipefail','-c',cmd], capture_output=True, text=True)
    if r.returncode: raise RuntimeError(f"{cmd}\n{r.stderr[-2000:]}")
    return time.monotonic() - t

def corpus_bytes(c: Path):
    return sum(p.stat().st_size for p in c.rglob('*') if p.is_file())


def fingerprint(c: Path) -> str:
    """Cheap fingerprint of the corpus: path, size and mtime per file.

    THE CORPUS CAN CHANGE MID-MEASUREMENT.  corpus/build-corpus.py hard-links
    (os.link) instead of copying, so nearly every file in kod, png, foto,
    gif, film and arkiv is the SAME inode as the file in the live working
    tree.  If someone saves a document in another window in the middle of a
    run, the corpus changes under nvz's feet and the recursive comparison
    afterwards reports "DIFFERS" — an error that looks like a correctness
    bug but is a measurement artifact.  It happened on 2026-11-18: three
    files in corpus/kod/ullabella changed between packing and comparison.

    The fingerprint reads no file contents, only directory entries, so it
    costs a few milliseconds.  If it differs before and after, the run is
    invalid and must be redone — not interpreted."""
    h = hashlib.blake2b(digest_size=16)
    for q in sorted(c.rglob('*')):
        try:
            st = q.lstat()
        except OSError:
            continue
        h.update(f'{q.relative_to(c)}|{st.st_size}|{st.st_mtime_ns}\n'.encode())
    return h.hexdigest()

def measure(corpus: Path, name, verify=True):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    slug = re.sub(r'[^a-z0-9]+', '-', name.lower()).strip('-')
    archive = SCRATCH / f'prov-{slug}.ark'
    unpack = SCRATCH / 'upp'
    fields = dict(c=str(corpus), cd=str(corpus.parent), cn=corpus.name, a=str(archive),
                  u=str(unpack), nv=str(ROOT/'src'/'hz'))
    pack_cmd, unpack_cmd = (c.format(**fields) for c in PACKERS[name])
    before = fingerprint(corpus)
    archive.unlink(missing_ok=True)
    tp = run(pack_cmd)
    size = archive.stat().st_size
    shutil.rmtree(unpack, ignore_errors=True); unpack.mkdir(parents=True)
    tu = run(unpack_cmd)
    equal = None
    if verify:
        r = subprocess.run(['diff','-rq','--no-dereference',str(corpus), str(unpack/corpus.name)],
                           capture_output=True, text=True)
        equal = (r.returncode == 0)
        if not equal: print('   MISMATCH:', r.stdout[:500], file=sys.stderr)
    after = fingerprint(corpus)
    stable = (before == after)
    if not stable:
        print(f'   CORPUS CHANGED DURING THE MEASUREMENT ({name}) — the run is invalid.\n'
              f'   The corpus is hard-linked to a live working tree; freeze it before measuring:\n'
              f'   python3 corpus/build-corpus.py --kopiera', file=sys.stderr)
    shutil.rmtree(unpack, ignore_errors=True); archive.unlink(missing_ok=True)
    # The dict keys are JSON field names in results/*.json — frozen.
    return dict(packare=name, byte=size, packsek=round(tp,2), uppacksek=round(tu,2),
                identisk=equal, korpus_stabil=stable)

def main():
    corpus = ROOT/'corpus'/sys.argv[1]
    chosen = sys.argv[2:] or list(PACKERS)
    raw = corpus_bytes(corpus)
    outfile = ROOT/'results'/f'{corpus.name}.json'
    data = json.loads(outfile.read_text()) if outfile.exists() else {}
    data['corpus'] = corpus.name
    data['rabyte'] = raw
    data['filer'] = sum(1 for p in corpus.rglob('*') if p.is_file())
    data.setdefault('matningar', {})
    print(f'{corpus.name}: {raw/1048576:.1f} MiB in {data["filer"]} files')
    for name in chosen:
        try:
            r = measure(corpus, name)
        except Exception as e:
            print(f'  {name:<22} ERROR: {e}'); continue
        data['matningar'][name] = r
        ratio = raw / r['byte']
        print(f'  {name:<22} {r["byte"]/1048576:>9.2f} MiB  x{ratio:>5.2f}  '
              f'pack {r["packsek"]:>7.1f}s  unpack {r["uppacksek"]:>6.1f}s  '
              f'{"exact" if r["identisk"] else "DIFFERS"}')
        outfile.parent.mkdir(exist_ok=True)
        outfile.write_text(json.dumps(data, indent=1, ensure_ascii=False))

if __name__ == '__main__': main()
