#!/usr/bin/env bash
# Ablationen: hur mycket kommer från VARJE knep för sig?
#
# Frågan kom från Stefan Alfredsson i tråden 2026-11-17: uppstår vinsten bara
# av att filer sorteras så att likheter hamnar i kompressorns fönster — en
# känd metod (gwern.net/sort), ingen nyhet — och beror den i så fall helt på
# fönsterstorleken?
#
# Provet håller KOMPRESSORN KONSTANT (samma xz -9e, samma 64 MiB ordbok) och
# varierar bara vad nvz gör med datan innan. Korpusen `kod` är 55 MiB och
# ryms därmed HELT inom ordboken i varje steg, så fönsterstorleken är inte en
# fri variabel: det som skiljer raderna är ordningen och ingenting annat.
#
#   bash experiment/ablation.sh [korpus] [kodek]
set -euo pipefail
export LC_ALL=C
ROT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVZ="$ROT/src/nvz"
KORPUS="${1:-kod}"
KODEK="${2:-xz9e}"
ARB="${NVPACK_SKRAP:-$ROT/.skrap}/ablation"
mkdir -p "$ARB"
K="$ROT/korpus/$KORPUS"
[ -d "$K" ] || { echo "saknar korpus/$KORPUS"; exit 1; }
RA=$(du -sb "$K" | cut -f1)

kor() { # namn, flaggor...
    local namn="$1"; shift
    local s=$(date +%s)
    "$NVZ" packa "$K" "$ARB/$namn.nvz" --läge normal --kodek "$KODEK" -q "$@"
    local t=$(( $(date +%s) - s ))
    echo "$namn $(stat -c%s "$ARB/$namn.nvz") $t"
}

echo "ablation på korpus/$KORPUS ($RA byte), kodek $KODEK, läge normal"
# referens utan nvz alls: tar i katalogordning genom samma kompressor
# Referensraden måste gå genom SAMMA kompressor som nvz-raderna, annars mäter
# tabellen kompressorbyte i stället för omordning.
case "$KODEK" in
  xz9e)   REFKMD=(xz -9e -T0 -c) ;;
  zstd19) REFKMD=(zstd -19 --long=27 -T0 -q -c) ;;
  zstd22) REFKMD=(zstd -22 --ultra --long=31 -T0 -q -c) ;;
  zstd3)  REFKMD=(zstd -3 -T0 -q -c) ;;
  *) echo "ablation: ingen tar-referens för kodek $KODEK"; exit 1 ;;
esac
S=$(date +%s); tar -cf - -C "$ROT/korpus" "$KORPUS" | "${REFKMD[@]}" > "$ARB/tar.xz"; TT=$(( $(date +%s) - S ))
echo "tar.xz $(stat -c%s "$ARB/tar.xz") $TT" > "$ARB/rader"
kor inget   --utan-dedup --utan-omvandling --utan-sortering --utan-klassning >> "$ARB/rader"
kor klass   --utan-dedup --utan-omvandling --utan-sortering                  >> "$ARB/rader"
kor sort    --utan-dedup --utan-omvandling                                   >> "$ARB/rader"
kor dedup   --utan-omvandling                                                >> "$ARB/rader"
kor allt                                                                     >> "$ARB/rader"

python3 - "$ARB/rader" "$RA" "$KORPUS" <<'PY'
import sys
rader = [l.split() for l in open(sys.argv[1]) if l.strip()]
ra, korpus = int(sys.argv[2]), sys.argv[3]
namn = {'tar.xz': 'tar i katalogordning (ingen nvz)',
        'inget': 'nvz, alla knep av',
        'klass': '+ ett block per innehållsklass',
        'sort':  '+ sortering inom klassen',
        'dedup': '+ deduplicering',
        'allt':  '+ omvandlingar (png/jpeg)'}
bas = int(rader[0][1]); forra = None
print(f'\n| steg | byte | mot tar | steget självt | packtid |')
print('|:--|--:|--:|--:|--:|')
for n, b, t in rader:
    b = int(b)
    steg = '—' if forra is None else f'{(b-forra)/forra*100:+.2f} %'
    print(f'| {namn.get(n,n)} | {b:,} | {(b-bas)/bas*100:+.2f} % | {steg} | {t} s |'.replace(',', ' '))
    forra = b
print(f'\nrådata {ra:,} byte'.replace(',', ' '))
PY
