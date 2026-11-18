#!/usr/bin/env bash
# När är likhetssorteringen värd något, och när är den värd noll?
#
# Ablationen i experiment/ablation.sh mäter sorteringen till ±0 på korpusarna
# kod och png. Det är inte ett fel i sorteringen — det är att de korpusarna
# inte är det fall sorteringen finns för:
#
#   * kod genom xz -9e: hela korpusen (58 MiB) ryms i ordboken (64 MiB), så
#     kompressorn ser varje fil från varje annan fil ändå. Ordningen kan då
#     per konstruktion inte göra någon skillnad.
#   * png: en enda filtyp, så sortering på (klass, ändelse, sökväg) ger samma
#     ordning som katalogordningen redan hade.
#   * kod genom zstd -3 (2 MiB fönster): blandade filtyper OCH litet fönster,
#     men ett riktigt projektträd har redan sina likheter samlade per katalog
#     — .py ligger med .py i samma paket. Sorteringen bryter då upp en
#     lokalitet som redan fanns, och mäter svagt NEGATIVT.
#
# Fallet sorteringen finns för är träd där likheterna ligger utspridda. Det
# här skriptet bygger det fallet av exakt samma data: samma 5 974 filer ur
# korpus/kod, kopierade till 200 slumpade kataloger med slumpade namn, så att
# katalogordningen förlorar all lokalitet medan innehållet är oförändrat.
#
#   bash experiment/sortering.sh
set -euo pipefail
export LC_ALL=C
ROT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVZ="$ROT/src/nvz"
ARB="${1:-${NVPACK_SKRAP:-$ROT/.skrap}/sortering}"
TRAD="$ARB/blandat"
mkdir -p "$ARB"

python3 - "$ROT/korpus/kod" "$TRAD" <<'PY'
import random, shutil, sys
from pathlib import Path
random.seed(20261118)                      # fast frö: trädet går att återskapa
src, dst = Path(sys.argv[1]), Path(sys.argv[2])
shutil.rmtree(dst, ignore_errors=True)
filer = sorted(p for p in src.rglob('*') if p.is_file())
kataloger = [dst / f'k{i:03d}' for i in range(200)]
for d in kataloger: d.mkdir(parents=True)
for i, p in enumerate(filer):
    shutil.copy2(p, random.choice(kataloger) / f'{i:05d}{p.suffix}')
b = sum(p.stat().st_size for p in dst.rglob('*') if p.is_file())
print(f'  blandat träd: {len(filer)} filer, {b} byte, 200 slumpade kataloger')
PY

# Dedup och omvandlingar är avstängda genomgående: det som ska mätas är
# ordningen, och ingenting annat. Kodeken är zstd -3 (fönster 2 MiB) i den
# ena spalten och xz -9e (ordbok 64 MiB) i den andra, för att visa att
# sorteringens värde är en funktion av fönstret.
rad() { # namn, flaggor...
    local namn="$1"; shift
    local a b
    "$NVZ" packa "$TRAD" "$ARB/a.nvz" --läge normal --kodek zstd3 --utan-dedup --utan-omvandling -q "$@"
    a=$(stat -c%s "$ARB/a.nvz")
    "$NVZ" packa "$TRAD" "$ARB/b.nvz" --läge normal --kodek xz9e  --utan-dedup --utan-omvandling -q "$@"
    b=$(stat -c%s "$ARB/b.nvz")
    printf '| %-34s | %11d | %11d |\n' "$namn" "$a" "$b"
}

echo
echo '| ordning | zstd -3 (2 MiB fönster) | xz -9e (64 MiB ordbok) |'
echo '|:--|--:|--:|'
TZ=$(tar -cf - -C "$ARB" blandat | zstd -3 -T0 -q -c | wc -c)
TX=$(tar -cf - -C "$ARB" blandat | xz -9e -T0 -c | wc -c)
printf '| %-34s | %11d | %11d |\n' "tar i katalogordning (ingen nvz)" "$TZ" "$TX"
rad "nvz utan sortering" --utan-sortering --utan-klassning
rad "nvz MED sortering" --utan-klassning
echo
echo "Samma data, samma kompressor, enda skillnaden är ordningen."
