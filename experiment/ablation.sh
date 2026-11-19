#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The ablation: how much comes from EACH trick on its own?
#
# The question came from Stefan Alfredsson in the public thread on
# 2026-11-17: does the win come merely from sorting files so that likeness
# lands inside the compressor's window — a known method (gwern.net/sort),
# no novelty — and does the effect then depend entirely on the window size?
#
# The test holds the COMPRESSOR CONSTANT and varies only what nvz does to
# the data first. With corpus `kod` (55 MiB) and xz -9e, the whole corpus
# fits inside the dictionary on every row, so the window size is not a free
# variable: what separates the rows is the ordering and nothing else.
#
#   bash experiment/ablation.sh [corpus] [codec]
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVZ="$ROOT/src/nvz"
CORPUS="${1:-kod}"
CODEC="${2:-xz9e}"
WORK="${NVPACK_SKRAP:-$ROOT/.skrap}/ablation"
mkdir -p "$WORK"
K="$ROOT/korpus/$CORPUS"
[ -d "$K" ] || { echo "missing korpus/$CORPUS"; exit 1; }
RAW=$(du -sb "$K" | cut -f1)

run() { # name, flags...
    local name="$1"; shift
    local s=$(date +%s)
    "$NVZ" pack "$K" "$WORK/$name.nvz" --mode normal --codec "$CODEC" -q "$@"
    local t=$(( $(date +%s) - s ))
    echo "$name $(stat -c%s "$WORK/$name.nvz") $t"
}

echo "ablation on korpus/$CORPUS ($RAW bytes), codec $CODEC, mode normal"
# The reference row must go through the SAME compressor as the nvz rows,
# or the table measures a compressor swap instead of a reordering.
case "$CODEC" in
  xz9e)   REF=(xz -9e -T0 -c) ;;
  zstd19) REF=(zstd -19 --long=27 -T0 -q -c) ;;
  zstd22) REF=(zstd -22 --ultra --long=31 -T0 -q -c) ;;
  zstd3)  REF=(zstd -3 -T0 -q -c) ;;
  *) echo "ablation: no tar reference for codec $CODEC"; exit 1 ;;
esac
S=$(date +%s); tar -cf - -C "$ROOT/korpus" "$CORPUS" | "${REF[@]}" > "$WORK/tar.ref"; TT=$(( $(date +%s) - S ))
echo "tar.ref $(stat -c%s "$WORK/tar.ref") $TT" > "$WORK/rows"
run none    --no-dedup --no-transform --no-sort --no-classing >> "$WORK/rows"
run class   --no-dedup --no-transform --no-sort               >> "$WORK/rows"
run sort    --no-dedup --no-transform                          >> "$WORK/rows"
run dedup   --no-transform                                     >> "$WORK/rows"
run all                                                        >> "$WORK/rows"

python3 - "$WORK/rows" "$RAW" "$CORPUS" <<'PY'
import sys
rows = [l.split() for l in open(sys.argv[1]) if l.strip()]
raw, corpus = int(sys.argv[2]), sys.argv[3]
names = {'tar.ref': 'tar in directory order (no nvz)',
         'none':  'nvz, all tricks off',
         'class': '+ one block per content class',
         'sort':  '+ sorting within the class',
         'dedup': '+ deduplication',
         'all':   '+ transforms (png/jpeg)'}
base = int(rows[0][1]); prev = None
print(f'\n| step | bytes | vs tar | the step itself | pack time |')
print('|:--|--:|--:|--:|--:|')
for n, b, t in rows:
    b = int(b)
    step = '—' if prev is None else f'{(b-prev)/prev*100:+.2f} %'
    print(f'| {names.get(n,n)} | {b:,} | {(b-base)/base*100:+.2f} % | {step} | {t} s |')
    prev = b
print(f'\nraw data {raw:,} bytes')
PY
