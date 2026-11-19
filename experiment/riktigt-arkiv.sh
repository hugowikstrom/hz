#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# [EN] The reality check: nvz against the live 2.9 GB archive this project
# was started for.
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
# Mäter nvz mot dagens arkivpipeline på en RIKTIG projektkatalog.
#
#   bash experiment/riktigt-arkiv.sh ~/lostconnection
#
# Korpusarna i korpus/ är rena: bara kod, bara PNG, bara foton. Ett riktigt
# projekt är blandat, och blandningen avgör vad man faktiskt tjänar. Det här
# försöket finns för att siffran i verkligheten ska stå bredvid siffran i
# labbet — även när verkligheten är tråkigare.
set -u
malkat="${1:?katalog}"
rot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
skrap="${NVPACK_SKRAP:-/tmp/claude-1000/-home-hugo/23274567-06f8-47a5-80d4-1071e7e104fd/scratchpad/riktigt}"
mkdir -p "$skrap"

storlek() { stat -c%s "$1"; }
sek() { date +%s.%N; }
visa() { awk -v n="$1" -v b="$2" -v t="$3" -v r="$4" \
  'BEGIN{printf "  %-22s %9.2f MiB   %6.1f s   %+.2f %%\n", n, b/1048576, t, 100*(b-r)/r}'; }

printf '\n%s\n' "$malkat"
printf '  %s i %s filer\n\n' "$(du -sh "$malkat" | cut -f1)" "$(find "$malkat" -type f | wc -l)"

t0=$(sek)
tar -C "$(dirname "$malkat")" -cf - "$(basename "$malkat")" | zstd -3 -T0 -q -o "$skrap/dagens.zst"
t1=$(sek)
bas=$(storlek "$skrap/dagens.zst")
visa "tar.zst -3 (dagens)" "$bas" "$(echo "$t1-$t0"|bc)" "$bas"

for lage in normal liten; do
    t0=$(sek)
    "$rot/src/hz" packa "$malkat" "$skrap/nv-$lage.nv" -q --läge "$lage" || continue
    t1=$(sek)
    visa "nvz $lage" "$(storlek "$skrap/nv-$lage.nv")" "$(echo "$t1-$t0"|bc)" "$bas"
done

printf '\n  kontrollerar att allt kommer tillbaka ... '
rm -rf "$skrap/upp"; mkdir -p "$skrap/upp"
"$rot/src/hz" packa-upp "$skrap/nv-normal.nv" "$skrap/upp" -q
if diff -rq --no-dereference "$malkat" "$skrap/upp/$(basename "$malkat")" > "$skrap/diff.txt" 2>&1; then
    printf 'bit för bit identiskt\n\n'
else
    printf 'AVVIKELSER:\n'; head -5 "$skrap/diff.txt"; printf '\n'
fi
rm -rf "$skrap/upp" "$skrap"/*.nv "$skrap/dagens.zst"
