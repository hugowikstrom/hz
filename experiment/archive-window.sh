#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The hypothesis test behind report section 4.2: block partitioning vs
# codec choice on the archives-of-archives corpus.
#
# Why does nvz lose on a corpus of zip and tar.gz files?
#
#   bash experiment/archive-window.sh
#
# The measurement showed that tar.zst -22 --ultra --long=31 takes the
# archive corpus from 392 to 243 MiB while nvz stops at 291. The hypothesis
# is that the win lies not in the codec but in the WINDOW: zstd gets to see
# the whole stream in one sweep and finds matches hundreds of megabytes
# apart, while nvz packs independent blocks of at most 192 MiB with
# --long=27 (a 128 MiB window) inside each of them.
#
# Four runs separate the two explanations:
#
#   1. zstd -19 WITHOUT --long   — small window, same codec as nvz picks
#   2. zstd -19 --long=27        — nvpack's window, the whole stream
#   3. zstd -19 --long=31        — the largest window
#   4. nvz --codec zstd22        — the best codec, nvpack's blocks
#
# If 1 lands near nvz and 2-3 far below it, the window is what decides, and
# the block size is then a design limit and not a codec choice.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
corpus="$root/corpus/arkiv"
work="${NVPACK_SCRATCH:-$root/.scratch}/window"
mkdir -p "$work"
raw=$(du -sb "$corpus" | cut -f1)
# The log's path is frozen: report/evaluation.py embeds it verbatim.
log="$root/results/archive-window.log"
exec > >(tee "$log") 2>&1
printf 'archive corpus: %s MiB raw\n\n' "$((raw/1048576))"

measure() {
    local name="$1"; shift
    local out="$work/probe.bin"
    rm -f "$out"
    local t0 t1
    t0=$(date +%s.%N)
    ( cd "$root/corpus" && tar -cf - arkiv ) | "$@" > "$out"
    t1=$(date +%s.%N)
    local b; b=$(stat -c%s "$out")
    # awk and not printf: bc writes a decimal point, and printf in a Swedish
    # locale wants a comma. With set -e the whole script died on that
    # disagreement.
    awk -v n="$name" -v b="$b" -v t0="$t0" -v t1="$t1" \
        'BEGIN { printf "  %-34s %8.1f MiB  %6.1f s\n", n, b/1048576, t1-t0 }'
    rm -f "$out"
}

measure 'zstd -19 (default window)'    zstd -19 -T0 -q -c
measure "zstd -19 --long=27 (nvpack's)" zstd -19 -T0 --long=27 -q -c
measure 'zstd -19 --long=31'           zstd -19 -T0 --long=31 -q -c
measure 'zstd -22 --ultra --long=31'   zstd -22 --ultra -T0 --long=31 -q -c

for codec in zstd19 zstd22; do
    archive="$work/nv-$codec.nv"; rm -f "$archive"
    t0=$(date +%s.%N)
    "$root/src/hz" pack "$corpus" "$archive" -q --codec "$codec"
    t1=$(date +%s.%N)
    awk -v n="nvz --codec $codec" -v b="$(stat -c%s "$archive")" -v t0="$t0" -v t1="$t1" \
        'BEGIN { printf "  %-34s %8.1f MiB  %6.1f s\n", n, b/1048576, t1-t0 }'

    printf '\n'; "$root/src/hz" list "$archive" | sed 's/^/    /'; printf '\n'
    rm -f "$archive"
done
