#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The reality check: nvz against the live 2.9 GB archive this project was
# started for.
#
# Measures nvz against today's archive pipeline on a REAL project directory.
#
#   bash experiment/real-archive.sh ~/lostconnection
#
# The corpora in corpus/ are clean: only code, only PNG, only photos. A real
# project is mixed, and the mix decides what is actually gained. This
# experiment exists so that the number from reality stands next to the
# number from the lab — even when reality is more boring.
set -u
target="${1:?directory}"
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="${NVPACK_SCRATCH:-$root/.scratch}/real"
mkdir -p "$work"

size() { stat -c%s "$1"; }
sec() { date +%s.%N; }
show() { awk -v n="$1" -v b="$2" -v t="$3" -v r="$4" \
  'BEGIN{printf "  %-22s %9.2f MiB   %6.1f s   %+.2f %%\n", n, b/1048576, t, 100*(b-r)/r}'; }

printf '\n%s\n' "$target"
printf '  %s in %s files\n\n' "$(du -sh "$target" | cut -f1)" "$(find "$target" -type f | wc -l)"

t0=$(sec)
tar -C "$(dirname "$target")" -cf - "$(basename "$target")" | zstd -3 -T0 -q -o "$work/today.zst"
t1=$(sec)
base=$(size "$work/today.zst")
show "tar.zst -3 (today's)" "$base" "$(echo "$t1-$t0"|bc)" "$base"

for mode in normal small; do
    t0=$(sec)
    "$root/src/hz" pack "$target" "$work/nv-$mode.nv" -q --mode "$mode" || continue
    t1=$(sec)
    show "nvz $mode" "$(size "$work/nv-$mode.nv")" "$(echo "$t1-$t0"|bc)" "$base"
done

printf '\n  checking that everything comes back ... '
rm -rf "$work/out"; mkdir -p "$work/out"
"$root/src/hz" unpack "$work/nv-normal.nv" "$work/out" -q
if diff -rq --no-dereference "$target" "$work/out/$(basename "$target")" > "$work/diff.txt" 2>&1; then
    printf 'bit for bit identical\n\n'
else
    printf 'DEVIATIONS:\n'; head -5 "$work/diff.txt"; printf '\n'
fi
rm -rf "$work/out" "$work"/*.nv "$work/today.zst"
