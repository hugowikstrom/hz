#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# zlib-ng/corpora: a test on data somebody else chose.
#
# The suggestion came from Jonas Danielsson in the public thread on
# 2026-11-17. The value lies in the selection not being ours: Calgary and
# Canterbury are the literature's old standard corpora and consist of
# SMALL files, which is exactly where nvz is weakest (report section 8).
# The test is not favourably chosen, and it is reported as it falls.
# As of 2026-11-19 this script is part of every full test run.
#
#   bash experiment/corpora.sh [workdir]
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HZ="$ROOT/src/hz"
WORK="${1:-${NVPACK_SCRATCH:-$ROOT/.scratch}/corpora}"
mkdir -p "$WORK"; cd "$WORK"
[ -d corpora ] || git clone --depth 1 -q https://github.com/zlib-ng/corpora corpora
rm -rf corpora/.git

printf '%-14s %9s %10s %10s %10s %10s %10s %10s %10s  %s\n' \
       corpus raw gzip-9 bzip2-9 zstd-19 xz-9e "hz fast" "hz normal" "hz small" roundtrip
for d in corpora/*/; do
    n=$(basename "$d")
    raw=$(du -sb "$d" | cut -f1)
    g=$(tar -cf - "$d" | gzip -9 -c | wc -c)
    b=$(tar -cf - "$d" | bzip2 -9 -c | wc -c)
    z=$(tar -cf - "$d" | zstd -19 --long=27 -T0 -q -c | wc -c)
    x=$(tar -cf - "$d" | xz -9e -T0 -c | wc -c)
    roundtrip=yes
    declare -A S
    for mode in fast normal small; do
        rm -f "$n-$mode.nvz"
        "$HZ" pack "$d" "$n-$mode.nvz" --mode "$mode" -q
        S[$mode]=$(stat -c%s "$n-$mode.nvz")
        rm -rf "out-$n"; "$HZ" unpack "$n-$mode.nvz" "out-$n" -q
        diff -r --no-dereference "$d" "out-$n/$n" >/dev/null 2>&1 || roundtrip="NO($mode)"
        rm -rf "out-$n"
    done
    printf '%-14s %9d %10d %10d %10d %10d %10d %10d %10d  %s\n' \
           "$n" "$raw" "$g" "$b" "$z" "$x" "${S[fast]}" "${S[normal]}" "${S[small]}" "$roundtrip"
done
