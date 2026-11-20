#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Measures nvjpg over a selection: size, bit exactness, speed.
set -u
work="${NVPACK_SCRATCH:-/tmp}/nvjpg-test"; mkdir -p "$work"
in=0; out=0; n=0; errors=0; t0=$(date +%s.%N)
while read -r f; do
    ./src/hzjpg pack < "$f" > "$work/a.nvj" 2>/dev/null || { echo "RAW FALLBACK: $f"; continue; }
    ./src/hzjpg unpack < "$work/a.nvj" > "$work/a.jpg" 2>/dev/null
    cmp -s "$f" "$work/a.jpg" || { echo "DIFFERS: $f"; errors=$((errors+1)); }
    in=$((in+$(stat -c%s "$f"))); out=$((out+$(stat -c%s "$work/a.nvj"))); n=$((n+1))
done
t1=$(date +%s.%N)
awk -v i=$in -v u=$out -v n=$n -v f=$errors -v t=$(echo "$t1-$t0"|bc) 'BEGIN{
  printf "%d files  %.1f MB -> %.1f MB  (%+.1f%%)  errors: %d  %.1fs  %.1f MB/s\n", n, i/1e6, u/1e6, -100*(i-u)/i, f, t, i/1e6/t}'
