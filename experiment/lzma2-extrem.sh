#!/usr/bin/env bash
# LZMA2 i sin starkaste form mot nvz.
#
#   bash experiment/lzma2-extrem.sh [korpus ...]
#
# xz -9e finns redan i alla tabeller, men xz är LZMA2 med 64 MiB ordbok.
# 7-Zip släpper lös formatet: här körs -mx=9 med 512 MiB ordbok och solid
# arkiv — ordboken täcker då HELA varje korpus utom film, vilket är samma
# spelplan som nvz:s stora block. Dessutom BCJ2-filtret på kodkorpusen?
# Nej — inga filter: jämförelsen gäller LZMA2 självt.
#
# Varje körning är en hel rundgång: packa, packa upp, diff -r mot källan.
# Resultaten skrivs till resultat/lzma2.json.
set -euo pipefail
rot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
skrap="${NVPACK_SKRAP:-$rot/.skrap}/lzma2"
mkdir -p "$skrap"
ut="$rot/resultat/lzma2.json"
korpusar=("${@:-kod logg arkiv png silesia enwik8}")
[ $# -eq 0 ] && korpusar=(kod logg arkiv png silesia enwik8)

python3 - "$ut" init <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1])
if not p.exists(): p.write_text('{}')
PY

for k in "${korpusar[@]}"; do
  kat="$rot/korpus/$k"
  ra=$(du -sb "$kat" | cut -f1)
  echo "=== $k ($((ra/1048576)) MiB) ==="

  # --- 7z LZMA2 -mx=9, 512 MiB ordbok, solid ------------------------------
  ark="$skrap/$k.7z"; rm -f "$ark"
  t0=$(date +%s.%N)
  7z a -t7z -m0=lzma2 -mx=9 -md=512m -ms=on -mmt=on -bso0 -bsp0 "$ark" "$kat" >/dev/null
  t1=$(date +%s.%N)
  b7=$(stat -c%s "$ark")
  upp="$skrap/upp-$k"; rm -rf "$upp"; mkdir -p "$upp"
  t2=$(date +%s.%N)
  7z x -bso0 -bsp0 -o"$upp" "$ark" >/dev/null
  t3=$(date +%s.%N)
  if diff -rq "$kat" "$upp/$k" >/dev/null; then ok7=true; else ok7=false; fi
  rm -rf "$upp" "$ark"

  # --- nvz, standardläget --------------------------------------------------
  arknv="$skrap/$k.nv"; rm -f "$arknv"
  t4=$(date +%s.%N)
  "$rot/src/nvz" packa "$kat" "$arknv" -q
  t5=$(date +%s.%N)
  bnv=$(stat -c%s "$arknv")
  upp="$skrap/upp-$k"; rm -rf "$upp"; mkdir -p "$upp"
  t6=$(date +%s.%N)
  "$rot/src/nvz" packa-upp "$arknv" "$upp" -q
  t7=$(date +%s.%N)
  if diff -rq "$kat" "$upp/$k" >/dev/null; then oknv=true; else oknv=false; fi
  rm -rf "$upp" "$arknv"

  python3 - "$ut" "$k" "$ra" "$b7" "$ok7" "$t0" "$t1" "$t2" "$t3" \
                 "$bnv" "$oknv" "$t4" "$t5" "$t6" "$t7" <<'PY'
import json, sys, pathlib
p = pathlib.Path(sys.argv[1]); d = json.loads(p.read_text())
(_, k, ra, b7, ok7, t0, t1, t2, t3, bnv, oknv, t4, t5, t6, t7) = sys.argv
d[k] = dict(rabyte=int(ra),
            lzma2=dict(byte=int(b7), exakt=ok7 == 'true',
                       packsek=round(float(t1)-float(t0), 1),
                       uppacksek=round(float(t3)-float(t2), 1)),
            nvz=dict(byte=int(bnv), exakt=oknv == 'true',
                     packsek=round(float(t5)-float(t4), 1),
                     uppacksek=round(float(t7)-float(t6), 1)))
p.write_text(json.dumps(d, indent=1))
v = d[k]
print(f"  lzma2-extrem {int(b7)/1048576:9.2f} MiB  {float(t1)-float(t0):6.1f}s  {'exakt' if ok7=='true' else 'SKILJER'}")
print(f"  nvz          {int(bnv)/1048576:9.2f} MiB  {float(t5)-float(t4):6.1f}s  {'exakt' if oknv=='true' else 'SKILJER'}")
skillnad = (int(bnv)-int(b7))/int(b7)*100
print(f"  nvz mot lzma2-extrem: {skillnad:+.1f} %")
PY
done
echo "KLART-LZMA2"
