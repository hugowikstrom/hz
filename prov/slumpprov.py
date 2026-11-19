#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Randomized test: builds random trees, packs, unpacks, demands identity.

Why this exists beyond prova-allt.sh: that suite tests the edge cases
someone THOUGHT OF. This one tests combinations nobody did — random file
sizes, file kinds, duplicate patterns, permissions, times, names and
symlinks. Every round is a full round trip: pack, unpack, compare every
byte, every permission and every timestamp, plus the archive's own check
(-t). It earns its keep: it caught the build-dependency fault that made
`nvz -t` crash while `nvz pack` worked, on its first round.

  python3 prov/slumpprov.py [rounds] [--seed N]     (--frö works too)

A failing round prints its seed. The same seed gives the same tree, so the
fault reproduces exactly.
"""
import os, random, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NVZ = ROOT / 'src' / 'nvz'
MODES = ['fast', 'normal', 'small']

# Real PNGs and JPEGs from the corpus, if present — the transforms are the
# part with the most to break, and synthetic data never touches them.
def templates():
    out = []
    for pattern, cap in (('korpus/png/**/*.png', 40), ('korpus/foto/**/*.jpg', 40)):
        out += [p for p in list(ROOT.glob(pattern))[:cap]
                if p.is_file() and p.stat().st_size < 4 << 20]
    return out


def build_tree(root: Path, r: random.Random, images):
    root.mkdir(parents=True)
    dirs = [root]
    for _ in range(r.randint(0, 6)):
        d = r.choice(dirs) / r.choice(['a', 'deep', 'name with spaces', 'åäö', '.hidden'])
        d.mkdir(parents=True, exist_ok=True)
        dirs.append(d)
    earlier = []
    for i in range(r.randint(1, 40)):
        d = r.choice(dirs)
        name = r.choice([f'f{i}', f'f{i}.txt', f'f{i}.png', f'f{i}.jpg', f'-dash{i}',
                         f'åäö{i}.md', f'new\nline{i}', f'f{i}.gz'])
        p = d / name
        kind = r.choice(['empty', 'zeros', 'random', 'text', 'duplicate', 'image', 'onebyte'])
        if kind == 'image' and images:
            shutil.copy(r.choice(images), p)
        elif kind == 'duplicate' and earlier:
            shutil.copy(r.choice(earlier), p)
        elif kind == 'empty':
            p.write_bytes(b'')
        elif kind == 'onebyte':
            p.write_bytes(bytes([r.randrange(256)]))
        elif kind == 'zeros':
            p.write_bytes(bytes(r.randint(1, 1 << 18)))
        elif kind == 'random':
            p.write_bytes(bytes(r.randrange(256) for _ in range(r.randint(1, 1 << 14))))
        else:
            line = (f'line {r.randrange(1000)} with text\n' * r.randint(1, 400)).encode()
            p.write_bytes(line)
        earlier.append(p)
        os.chmod(p, r.choice([0o644, 0o600, 0o400, 0o755, 0o444]))
        os.utime(p, (r.randint(0, 2 << 30), r.randint(0, 2 << 30)))
    for _ in range(r.randint(0, 4)):          # symlinks: relative, broken, to dirs
        d = r.choice(dirs)
        target = r.choice(['../' + r.choice(earlier).name, '/does/not/exist', '.'])
        try: (d / f'link{r.randrange(10000)}').symlink_to(target)
        except OSError: pass


def one_round(seed: int, work: Path, images) -> str | None:
    r = random.Random(seed)
    tree, up = work / 'tree', work / 'up'
    ark = work / 'test.nv'
    for p in (tree, up):
        shutil.rmtree(p, ignore_errors=True)
    ark.unlink(missing_ok=True)
    build_tree(tree, r, images)
    mode = r.choice(MODES)
    k = subprocess.run([NVZ, 'pack', str(tree), str(ark), '-q', f'--mode={mode}'],
                       capture_output=True, text=True)
    if k.returncode: return f'pack returned {k.returncode}: {k.stderr[:200]}'
    k = subprocess.run([NVZ, '-t', str(ark)], capture_output=True, text=True)
    if k.returncode: return f'-t returned {k.returncode}: {k.stderr[:200]}'
    up.mkdir(parents=True)
    k = subprocess.run([NVZ, 'unpack', str(ark), str(up), '-q'], capture_output=True, text=True)
    if k.returncode: return f'unpack returned {k.returncode}: {k.stderr[:200]}'
    k = subprocess.run(['diff', '-rq', '--no-dereference', str(tree), str(up / tree.name)],
                       capture_output=True, text=True)
    if k.returncode: return f'content differs: {k.stdout[:300]}'
    for a, b in ((tree, up / tree.name),):            # permissions and times
        for p in sorted(a.rglob('*')):
            q = b / p.relative_to(a)
            sa, sb = p.lstat(), q.lstat()
            if (sa.st_mode, int(sa.st_mtime)) != (sb.st_mode, int(sb.st_mtime)):
                return f'mode/time differs on {p.relative_to(a)}'
    return None


def main():
    rounds = int(sys.argv[1]) if len(sys.argv) > 1 and sys.argv[1].isdigit() else 50
    start = 1
    for flag in ('--seed', '--frö'):
        if flag in sys.argv:
            start = int(sys.argv[sys.argv.index(flag) + 1])
    images = templates()
    failed = 0
    with tempfile.TemporaryDirectory(prefix='nvz-rand-') as d:
        for i in range(rounds):
            seed = start + i
            fault = one_round(seed, Path(d), images)
            if fault:
                failed += 1
                print(f'  FAIL  seed {seed}: {fault}', flush=True)
            elif (i + 1) % 10 == 0:
                print(f'  {i+1}/{rounds} rounds ok', flush=True)
    print(f'\n{rounds - failed}/{rounds} random trees came back bit-exact'
          f'{" — ALL" if not failed else ""}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
