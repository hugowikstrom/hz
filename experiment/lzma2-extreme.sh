#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# LZMA2 in its strongest form against nvz — the opponent from report
# section 3.6.
#
#   bash experiment/lzma2-extreme.sh [corpus ...]
#
# xz -9e is already in every table, but xz is LZMA2 with a 64 MiB
# dictionary. 7-Zip lets the format loose: here -mx=9 runs with a 512 MiB
# dictionary and a solid archive — the dictionary then covers the WHOLE of
# every corpus except film, which is the same playing field as nvz's large
# blocks. The BCJ2 filter on the code corpus as well? No — no filters: the
# comparison concerns LZMA2 itself.
#
# Every run is a full round trip: pack, unpack, diff -r against the source.
# The results are written to results/lzma2.json.
set -euo pipefail
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="${NVPACK_SCRATCH:-$root/.scratch}/lzma2"
mkdir -p "$work"
out="$root/results/lzma2.json"
corpora=("$@")
[ $# -eq 0 ] && corpora=(kod logg arkiv png silesia enwik8)

python3 - "$out" init <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
if not p.exists(): p.write_text('{}')
PY

for k in "${corpora[@]}"; do
  src="$root/corpus/$k"
  raw=$(du -sb "$src" | cut -f1)
  echo "=== $k ($((raw/1048576)) MiB) ==="

  # --- 7z LZMA2 -mx=9, 512 MiB dictionary, solid ---------------------------
  archive="$work/$k.7z"; rm -f "$archive"
  t0=$(date +%s.%N)
  7z a -t7z -m0=lzma2 -mx=9 -md=512m -ms=on -mmt=on -bso0 -bsp0 "$archive" "$src" >/dev/null
  t1=$(date +%s.%N)
  b7=$(stat -c%s "$archive")
  unp="$work/out-$k"; rm -rf "$unp"; mkdir -p "$unp"
  t2=$(date +%s.%N)
  7z x -bso0 -bsp0 -o"$unp" "$archive" >/dev/null
  t3=$(date +%s.%N)
  if diff -rq "$src" "$unp/$k" >/dev/null; then ok7=true; else ok7=false; fi
  rm -rf "$unp" "$archive"

  # --- nvz, the default mode ------------------------------------------------
  nvarchive="$work/$k.nv"; rm -f "$nvarchive"
  t4=$(date +%s.%N)
  "$root/src/hz" pack "$src" "$nvarchive" -q
  t5=$(date +%s.%N)
  bnv=$(stat -c%s "$nvarchive")
  unp="$work/out-$k"; rm -rf "$unp"; mkdir -p "$unp"
  t6=$(date +%s.%N)
  "$root/src/hz" unpack "$nvarchive" "$unp" -q
  t7=$(date +%s.%N)
  if diff -rq "$src" "$unp/$k" >/dev/null; then oknv=true; else oknv=false; fi
  rm -rf "$unp" "$nvarchive"

  # The JSON keys are frozen: the report reads them by name.
  python3 - "$out" "$k" "$raw" "$b7" "$ok7" "$t0" "$t1" "$t2" "$t3" \
                 "$bnv" "$oknv" "$t4" "$t5" "$t6" "$t7" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text())
(k, raw, b7, ok7, t0, t1, t2, t3, bnv, oknv, t4, t5, t6, t7) = sys.argv[2:]
d[k] = dict(rabyte=int(raw),
            lzma2=dict(byte=int(b7), exakt=ok7 == 'true',
                       packsek=round(float(t1)-float(t0), 1),
                       uppacksek=round(float(t3)-float(t2), 1)),
            nvz=dict(byte=int(bnv), exakt=oknv == 'true',
                     packsek=round(float(t5)-float(t4), 1),
                     uppacksek=round(float(t7)-float(t6), 1)))
p.write_text(json.dumps(d, indent=1))
print(f"  lzma2-extreme {int(b7)/1048576:9.2f} MiB  {float(t1)-float(t0):6.1f}s  {'exact' if ok7=='true' else 'DIFFERS'}")
print(f"  nvz           {int(bnv)/1048576:9.2f} MiB  {float(t5)-float(t4):6.1f}s  {'exact' if oknv=='true' else 'DIFFERS'}")
delta = (int(bnv)-int(b7))/int(b7)*100
print(f"  nvz vs lzma2-extreme: {delta:+.1f} %")
PY
done
echo "LZMA2-DONE"
