#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Summerar nvjpg-storleken över ett fast urval. Används för modelltrimning.
set -u
in=0; ut=0; n=0
while read -r f; do
    s=$(./src/nvjpg packa < "$f" 2>/dev/null | wc -c) || continue
    [ "$s" -gt 0 ] || continue
    in=$((in+$(stat -c%s "$f"))); ut=$((ut+s)); n=$((n+1))
done
awk -v i=$in -v u=$ut -v n=$n 'BEGIN{printf "%3d filer  %8.3f MB -> %8.3f MB   %+.2f%%\n", n, i/1e6, u/1e6, -100*(i-u)/i}'
