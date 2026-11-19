#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Bygger nvcm med givna val och mäter på samma provbit.
set -u
prov=/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/m8.bin
for val in "$@"; do
    gcc -O2 -w $val -o /tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/nvcm-t src/hzcm.c || { echo "$val BYGGFEL"; continue; }
    t0=$(date +%s.%N)
    s=$(/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/nvcm-t packa < $prov | wc -c)
    t=$(echo "$(date +%s.%N)-$t0" | bc)
    awk -v v="${val:-grund}" -v s=$s -v t=$t -v i=$(stat -c%s $prov) \
      'BEGIN{printf "%-36s %9d  %.3f bpb  %5.1fs  %.2f MB/s\n", v, s, s*8/i, t, i/1e6/t}'
done
