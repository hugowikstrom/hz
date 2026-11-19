#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Mäter nvjpg över ett urval: storlek, bit-exakthet, hastighet.
set -u
skrap=/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/jp; mkdir -p $skrap
in=0; ut=0; n=0; fel=0; t0=$(date +%s.%N)
while read -r f; do
    ./src/nvjpg packa < "$f" > $skrap/a.nvj 2>/dev/null || { echo "RÅFALL: $f"; continue; }
    ./src/nvjpg packaupp < $skrap/a.nvj > $skrap/a.jpg 2>/dev/null
    cmp -s "$f" $skrap/a.jpg || { echo "SKILJER: $f"; fel=$((fel+1)); }
    in=$((in+$(stat -c%s "$f"))); ut=$((ut+$(stat -c%s $skrap/a.nvj))); n=$((n+1))
done
t1=$(date +%s.%N)
awk -v i=$in -v u=$ut -v n=$n -v f=$fel -v t=$(echo "$t1-$t0"|bc) 'BEGIN{
  printf "%d filer  %.1f MB -> %.1f MB  (%+.1f%%)  fel: %d  %.1fs  %.1f MB/s\n", n, i/1e6, u/1e6, -100*(i-u)/i, f, t, i/1e6/t}'
