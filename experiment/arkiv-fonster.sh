#!/usr/bin/env bash
# [EN] The hypothesis test behind report section 4.2: block partitioning vs
# codec choice on the archives-of-archives corpus.
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
# Varför förlorar nvz på en korpus av zip- och tar.gz-filer?
#
#   bash experiment/arkiv-fonster.sh
#
# Mätningen visade att tar.zst -22 --ultra --long=31 tar arkivkorpusen från
# 392 till 243 MiB medan nvz stannar på 291. Hypotesen är att vinsten inte
# ligger i kodeken utan i FÖNSTRET: zstd får se hela strömmen i ett svep och
# hittar matchningar hundratals megabyte isär, medan nvz packar oberoende
# block på högst 192 MiB med --long=27 (128 MiB fönster) inuti.
#
# Fyra körningar skiljer de två förklaringarna åt:
#
#   1. zstd -19 UTAN --long     — litet fönster, samma kodek som nvz väljer
#   2. zstd -19 --long=27       — nvpacks fönster, hela strömmen
#   3. zstd -19 --long=31       — största fönstret
#   4. nvz --kodek zstd22    — bästa kodek, nvpacks block
#
# Om 1 landar nära nvz och 2–3 långt under är det fönstret som avgör, och
# då är blockstorleken en designgräns och inte ett kodekval.
set -euo pipefail
rot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
k="$rot/korpus/arkiv"
skrap="${NVPACK_SKRAP:-$rot/.skrap}/fonster"
mkdir -p "$skrap"
ra=$(du -sb "$k" | cut -f1)
logg="$rot/resultat/arkiv-fonster.log"
exec > >(tee "$logg") 2>&1
printf 'arkivkorpus: %s MiB rå\n\n' "$((ra/1048576))"

mät() {
    local namn="$1"; shift
    local ut="$skrap/prov.bin"
    rm -f "$ut"
    local t0 t1
    t0=$(date +%s.%N)
    ( cd "$rot/korpus" && tar -cf - arkiv ) | "$@" > "$ut"
    t1=$(date +%s.%N)
    local b; b=$(stat -c%s "$ut")
    # awk och inte printf: bc skriver decimalpunkt, och printf i svensk lokal
    # vill ha komma. Med set -e dog hela skriptet på den oenigheten.
    awk -v n="$namn" -v b="$b" -v t0="$t0" -v t1="$t1" \
        'BEGIN { printf "  %-34s %8.1f MiB  %6.1f s\n", n, b/1048576, t1-t0 }'
    rm -f "$ut"
}

mät 'zstd -19 (standardfönster)'   zstd -19 -T0 -q -c
mät 'zstd -19 --long=27 (nvpacks)' zstd -19 -T0 --long=27 -q -c
mät 'zstd -19 --long=31'           zstd -19 -T0 --long=31 -q -c
mät 'zstd -22 --ultra --long=31'   zstd -22 --ultra -T0 --long=31 -q -c

for kodek in zstd19 zstd22; do
    ark="$skrap/nv-$kodek.nv"; rm -f "$ark"
    t0=$(date +%s.%N)
    "$rot/src/nvz" packa "$k" "$ark" -q --kodek "$kodek"
    t1=$(date +%s.%N)
    awk -v n="nvz --kodek $kodek" -v b="$(stat -c%s "$ark")" -v t0="$t0" -v t1="$t1" \
        'BEGIN { printf "  %-34s %8.1f MiB  %6.1f s\n", n, b/1048576, t1-t0 }'

    printf '\n'; "$rot/src/nvz" lista "$ark" | sed 's/^/    /'; printf '\n'
    rm -f "$ark"
done
