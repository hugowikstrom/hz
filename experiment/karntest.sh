#!/usr/bin/env bash
# Kärntestet: det prov Stefan Alfredsson begärde i tråden 2026-11-17.
#
#   hämta linuxkärnan, kontrollsumma den, packa med nvz, jämför storleken mot
#   kernel.orgs egen tar.xz, packa upp och kontrollsumma igen.
#
# Poängen är att korpusen inte är vår: den är världens mest granskade
# källkodsträd, hämtat från tredje part, med en checksumma vi inte satt själva.
#
#   bash experiment/karntest.sh [arbetskatalog]
#
# Resultatet skrivs som JSON till resultat/karntest.json och som tabell till
# standard ut.
set -euo pipefail
export LC_ALL=C
ROT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVZ="$ROT/src/nvz"
ARB="${1:-${NVPACK_SKRAP:-$ROT/.skrap}/karntest}"
VER="${KARNA_VERSION:-6.12}"
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"
mkdir -p "$ARB"; cd "$ARB"

tid() { local s=$(date +%s); "$@" >/dev/null 2>&1; echo $(( $(date +%s) - s )); }

[ -f linux.tar.xz ] || { echo "hämtar $URL"; curl -sSLo linux.tar.xz "$URL"; }
NED=$(stat -c%s linux.tar.xz)
md5sum linux.tar.xz > nedladdad.md5

if [ ! -d karna ]; then echo "packar upp tarbollen"; mkdir -p karna; tar -xf linux.tar.xz -C karna; fi
FILER=$(find karna -type f | wc -l)
RA=$(du -sb karna | cut -f1)

echo "manifest före ($FILER filer)"
( cd karna && find . -type f -print0 | sort -z | xargs -0 -P "$(nproc)" -n 200 md5sum ) | sort -k2 > fore.md5

echo "nvz --läge normal"; TN=$(tid "$NVZ" packa karna nvz-normal.nvz --läge normal -q); SN=$(stat -c%s nvz-normal.nvz)
echo "nvz --läge liten";  TL=$(tid "$NVZ" packa karna nvz-liten.nvz  --läge liten  -q); SL=$(stat -c%s nvz-liten.nvz)
echo "tar.xz -9e";        TX=$(tid bash -c 'tar -cf - karna | xz -9e -T0 -c > ref.tar.xz'); SX=$(stat -c%s ref.tar.xz)
echo "tar.zst -19";       TZ=$(tid bash -c 'tar -cf - karna | zstd -19 --long=27 -T0 -q -o ref.tar.zst -f'); SZ=$(stat -c%s ref.tar.zst)

echo "7z LZMA2 -mx=9, 512 MiB ordbok, solid"
rm -f ref.7z; T7=$(tid 7z a -t7z -m0=LZMA2 -mx=9 -md=512m -ms=on -mmt="$(nproc)" ref.7z karna); S7=$(stat -c%s ref.7z)

echo "packar upp nvz-liten och kontrollerar med md5sum -c"
rm -rf ut; mkdir ut; TU=$(tid "$NVZ" packa-upp nvz-liten.nvz ut)
KONTROLL=$( cd ut/karna && md5sum -c "$ARB/fore.md5" 2>&1 | grep -c ': OK$' || true )
FEL=$( cd ut/karna && md5sum -c "$ARB/fore.md5" 2>&1 | grep -c ': FAILED' || true )
( cd ut/karna && find . -type f -print0 | sort -z | xargs -0 -P "$(nproc)" -n 200 md5sum ) | sort -k2 > efter.md5
LIKA=$(cmp -s fore.md5 efter.md5 && echo ja || echo NEJ)

mkdir -p "$ROT/resultat"
cat > "$ROT/resultat/karntest.json" <<JSON
{"version":"$VER","nedladdad_tar_xz":$NED,"rabyte":$RA,"filer":$FILER,
 "nvz_normal":{"byte":$SN,"packtid":$TN},"nvz_liten":{"byte":$SL,"packtid":$TL},
 "tar_xz_9e":{"byte":$SX,"packtid":$TX},"tar_zst_19":{"byte":$SZ,"packtid":$TZ},
 "lzma2_extrem_7z":{"byte":$S7,"packtid":$T7},
 "uppacktid_liten":$TU,"md5_ok":$KONTROLL,"md5_fel":$FEL,"manifest_lika":"$LIKA"}
JSON

p() { python3 -c "print(f'{($1-$2)/$2*100:+.1f} %')"; }
echo
echo "linux-$VER: $RA byte i $FILER filer"
printf '%-24s %14s %10s %12s\n' packare byte "mot ned" packtid
printf '%-24s %14d %10s %12s\n' "kernel.org tar.xz" "$NED" "0,0 %" "-"
printf '%-24s %14d %10s %11ds\n' "tar.xz -9e (lokalt)" "$SX" "$(p "$SX" "$NED")" "$TX"
printf '%-24s %14d %10s %11ds\n' "tar.zst -19 --long" "$SZ" "$(p "$SZ" "$NED")" "$TZ"
printf '%-24s %14d %10s %11ds\n' "7z LZMA2 -mx=9 512M" "$S7" "$(p "$S7" "$NED")" "$T7"
printf '%-24s %14d %10s %11ds\n' "nvz --läge normal" "$SN" "$(p "$SN" "$NED")" "$TN"
printf '%-24s %14d %10s %11ds\n' "nvz --läge liten" "$SL" "$(p "$SL" "$NED")" "$TL"
echo
echo "uppackning (liten): ${TU}s · md5sum -c: $KONTROLL OK, $FEL FAILED · manifest identiska: $LIKA"
