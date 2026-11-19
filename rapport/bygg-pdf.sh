#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Renderar tryckversionen till PDF och kontrollerar resultatet.
#
#   bash rapport/bygg-pdf.sh
#
# Två kontroller körs efteråt, och båda har en historia:
#
#   TYPE 3 — Chrome bäddar in varje typsnitt som kommit in via @font-face som en
#   Type 3-font, och sådana underkänner tryckerier. Därför refererar tryck.html
#   bara familjenamn på typsnitt som finns installerade i systemet. Kontrollen
#   finns för att felet inte syns på skärmen, bara i pdffonts.
#
#   SIDANTAL — sidorna är byggda som fasta A4-rutor med overflow: hidden. Blir
#   en sida för full klipps innehållet tyst bort. Antalet sidor i PDF:en måste
#   därför stämma exakt med antalet sidor i HTML:en, och varje sida granskas
#   som bild efteråt.
set -euo pipefail
rot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
ut="$rot/rapport/nvz-rapport.pdf"

python3 "$rot/rapport/bygg-tryck.py"
vantat=$(grep -c 'class="sida"' "$rot/rapport/tryck.html")

printf '  overfyllnad ... '
svar=$(google-chrome --headless --disable-gpu --no-sandbox --virtual-time-budget=4000 \
       --dump-dom "file://$rot/rapport/tryck.html" 2>/dev/null \
       | grep -o 'OVERFYLLD:[^<]*\|ALLA-SIDOR-RYMS' | head -1)
if [ "$svar" = "ALLA-SIDOR-RYMS" ]; then echo "ok"; else echo "$svar"; exit 1; fi

google-chrome --headless --disable-gpu --no-sandbox --no-pdf-header-footer \
    --virtual-time-budget=10000 \
    --print-to-pdf="$ut" "file://$rot/rapport/tryck.html" 2>/dev/null

printf '\n%s\n' "$ut"
printf '  %s\n' "$(du -h "$ut" | cut -f1)"

sidor=$(pdfinfo "$ut" | awk '/^Pages:/ {print $2}')
printf '  %s sidor (vantat %s) ... ' "$sidor" "$vantat"
[ "$sidor" = "$vantat" ] && echo "ok" || { echo "FEL — innehåll kan ha klippts"; exit 1; }

printf '  typsnitt ... '
if pdffonts "$ut" | grep -q 'Type 3'; then
    echo "TYPE 3 HITTAD:"; pdffonts "$ut"; exit 1
else
    echo "ok, inga Type 3"
fi
pdffonts "$ut" | sed -n '1,2p;3,$p' | sed 's/^/      /'
