#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# When is the similarity sort worth something, and when is it worth zero?
#
# The ablation in experiment/ablation.sh measures the sort at ±0 on the
# corpora kod and png. That is not a fault in the sort — those corpora are
# not the case the sort exists for:
#
#   * kod through xz -9e: the whole corpus (58 MiB) fits in the dictionary
#     (64 MiB), so the compressor sees every file from every other file
#     anyway. Ordering cannot make a difference by construction.
#   * png: one single file type, so sorting on (class, extension, path)
#     yields the order directory order already had.
#   * kod through zstd -3 (2 MiB window): mixed file types AND a small
#     window, but a real project tree already keeps its likeness gathered
#     per directory — .py sits with .py in the same package. The sort then
#     breaks up locality that already existed, and measures slightly
#     NEGATIVE.
#
# The case the sort exists for is a tree where likeness is scattered. This
# script builds that case out of exactly the same data: the same 5 974
# files from korpus/kod, copied into 200 directories with random names, so
# directory order loses all locality while the content is unchanged.
#
#   bash experiment/sortering.sh
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HZ="$ROOT/src/hz"
WORK="${1:-${NVPACK_SKRAP:-$ROOT/.skrap}/sortering}"
TREE="$WORK/scattered"
mkdir -p "$WORK"

python3 - "$ROOT/korpus/kod" "$TREE" <<'PY'
import random, shutil, sys
from pathlib import Path
random.seed(20261118)                      # fixed seed: the tree reproduces
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
shutil.rmtree(dst, ignore_errors=True)
files = sorted(p for p in src.rglob('*') if p.is_file())
dirs = [dst / f'd{i:03d}' for i in range(200)]
for d in dirs: d.mkdir(parents=True)
for i, p in enumerate(files):
    shutil.copy2(p, random.choice(dirs) / f'{i:05d}{p.suffix}')
b = sum(p.stat().st_size for p in dst.rglob('*') if p.is_file())
print(f'  scattered tree: {len(files)} files, {b} bytes, 200 random directories')
PY

# Dedup and transforms are off throughout: what is measured is the
# ordering and nothing else. The codec is zstd -3 (2 MiB window) in one
# column and xz -9e (64 MiB dictionary) in the other, to show that the
# sort's value is a function of the window.
row() { # name, flags...
    local name="$1"; shift
    local a b
    "$HZ" pack "$TREE" "$WORK/a.nvz" --mode normal --codec zstd3 --no-dedup --no-transform -q "$@"
    a=$(stat -c%s "$WORK/a.nvz")
    "$HZ" pack "$TREE" "$WORK/b.nvz" --mode normal --codec xz9e  --no-dedup --no-transform -q "$@"
    b=$(stat -c%s "$WORK/b.nvz")
    printf '| %-34s | %11d | %11d |\n' "$name" "$a" "$b"
}

echo
echo '| ordering | zstd -3 (2 MiB window) | xz -9e (64 MiB dictionary) |'
echo '|:--|--:|--:|'
TZ=$(tar -cf - -C "$WORK" scattered | zstd -3 -T0 -q -c | wc -c)
TX=$(tar -cf - -C "$WORK" scattered | xz -9e -T0 -c | wc -c)
printf '| %-34s | %11d | %11d |\n' "tar in directory order (no nvz)" "$TZ" "$TX"
row "nvz without the sort" --no-sort --no-classing
row "nvz WITH the sort" --no-classing
echo
echo "Same data, same compressor; the only difference is the ordering."
