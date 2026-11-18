#!/usr/bin/env bash
# zlib-ng/corpora: ett prov på testdata som någon annan har valt.
#
# Förslaget kom från Jonas Danielsson i tråden 2026-11-17. Värdet ligger i att
# urvalet inte är vårt: Calgary och Canterbury är litteraturens gamla
# standardkorpusar och består av SMÅ filer, vilket är precis det område där
# nvz är svagast (fast överhuvud, rapportens avsnitt 8 punkt 3). Provet är
# alltså inte gynnsamt valt, och det redovisas som det är.
#
#   bash experiment/corpora.sh [arbetskatalog]
set -euo pipefail
export LC_ALL=C
ROT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVZ="$ROT/src/nvz"
ARB="${1:-${NVPACK_SKRAP:-$ROT/.skrap}/corpora}"
mkdir -p "$ARB"; cd "$ARB"
[ -d corpora ] || git clone --depth 1 -q https://github.com/zlib-ng/corpora corpora
rm -rf corpora/.git

tid() { local s=$(date +%s%N); "$@" >/dev/null 2>&1; echo $(( ($(date +%s%N) - s) / 1000000 )); }

printf '%-14s %9s %10s %10s %10s %10s %10s %10s %10s  %s\n' \
       korpus rå gzip-9 bzip2-9 zstd-19 xz-9e "nvz snabb" "nvz normal" "nvz liten" rundgång
for d in corpora/*/; do
    n=$(basename "$d")
    ra=$(du -sb "$d" | cut -f1)
    g=$(tar -cf - "$d" | gzip -9 -c | wc -c)
    b=$(tar -cf - "$d" | bzip2 -9 -c | wc -c)
    z=$(tar -cf - "$d" | zstd -19 --long=27 -T0 -q -c | wc -c)
    x=$(tar -cf - "$d" | xz -9e -T0 -c | wc -c)
    rundgang=ja
    declare -A S
    for lage in snabb normal liten; do
        rm -f "$n-$lage.nvz"
        "$NVZ" packa "$d" "$n-$lage.nvz" --läge "$lage" -q
        S[$lage]=$(stat -c%s "$n-$lage.nvz")
        rm -rf "ut-$n"; "$NVZ" packa-upp "$n-$lage.nvz" "ut-$n" -q
        diff -r --no-dereference "$d" "ut-$n/$n" >/dev/null 2>&1 || rundgang="NEJ($lage)"
        rm -rf "ut-$n"
    done
    printf '%-14s %9d %10d %10d %10d %10d %10d %10d %10d  %s\n' \
           "$n" "$ra" "$g" "$b" "$z" "$x" "${S[snabb]}" "${S[normal]}" "${S[liten]}" "$rundgang"
done
