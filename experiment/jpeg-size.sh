#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Sums the nvjpg size over a fixed selection. Used for model tuning.
set -u
in=0; out=0; n=0
while read -r f; do
    s=$(./src/hzjpg pack < "$f" 2>/dev/null | wc -c) || continue
    [ "$s" -gt 0 ] || continue
    in=$((in+$(stat -c%s "$f"))); out=$((out+s)); n=$((n+1))
done
awk -v i=$in -v u=$out -v n=$n 'BEGIN{printf "%3d files  %8.3f MB -> %8.3f MB   %+.2f%%\n", n, i/1e6, u/1e6, -100*(i-u)/i}'
