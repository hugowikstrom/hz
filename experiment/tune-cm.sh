#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds nvcm with the given choices and measures on the same test slice.
# The slice is NVCM_SAMPLE (default $NVPACK_SCRATCH/m8.bin): a fixed cut, so
# that rows from different runs stay comparable.
set -u
work="${NVPACK_SCRATCH:-/tmp}"
sample="${NVCM_SAMPLE:-$work/m8.bin}"
for choice in "$@"; do
    gcc -O2 -w $choice -o "$work/nvcm-t" src/hzcm.c || { echo "$choice BUILD ERROR"; continue; }
    t0=$(date +%s.%N)
    s=$("$work/nvcm-t" pack < "$sample" | wc -c)
    t=$(echo "$(date +%s.%N)-$t0" | bc)
    awk -v v="${choice:-base}" -v s=$s -v t=$t -v i=$(stat -c%s "$sample") \
      'BEGIN{printf "%-36s %9d  %.3f bpb  %5.1fs  %.2f MB/s\n", v, s, s*8/i, t, i/1e6/t}'
done
