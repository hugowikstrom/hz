#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds nvjpg with the given model choices and measures on the same selection.
set -u
work="${NVPACK_SCRATCH:-/tmp}"
for choice in "$@"; do
    gcc -O2 -w $choice -o "$work/nvjpg-t" src/hzjpg.c || { echo "$choice: BUILD ERROR"; continue; }
    cp "$work/nvjpg-t" src/hzjpg.trim
    printf '%-34s ' "${choice:-base}"
    sed 's|./src/hzjpg|./src/hzjpg.trim|' experiment/jpeg-size.sh > "$work/size.sh"
    bash "$work/size.sh" < experiment/selection-jpeg.txt
done
rm -f src/hzjpg.trim
