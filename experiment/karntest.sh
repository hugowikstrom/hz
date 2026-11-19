#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The kernel test: the trial Stefan Alfredsson asked for in the public
# thread on 2026-11-17.
#
#   download the Linux kernel, checksum it, pack it with nvz, compare the
#   size against kernel.org's own tar.xz, unpack and checksum again.
#
# The point is that the corpus is not ours: the most reviewed source tree
# in the world, fetched from a third party, with a checksum we did not set.
#
#   bash experiment/karntest.sh [workdir]
#
# The result is written as JSON to resultat/karntest.json and as a table to
# standard out.
set -euo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HZ="$ROOT/src/hz"
WORK="${1:-${NVPACK_SKRAP:-$ROOT/.skrap}/karntest}"
VER="${KARNA_VERSION:-6.12}"
URL="https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$VER.tar.xz"
mkdir -p "$WORK"; cd "$WORK"

t() { local s=$(date +%s); "$@" >/dev/null 2>&1; echo $(( $(date +%s) - s )); }

[ -f linux.tar.xz ] || { echo "fetching $URL"; curl -sSLo linux.tar.xz "$URL"; }
DL=$(stat -c%s linux.tar.xz)
md5sum linux.tar.xz > downloaded.md5

if [ ! -d kernel ]; then echo "extracting the tarball"; mkdir -p kernel; tar -xf linux.tar.xz -C kernel; fi
FILES=$(find kernel -type f | wc -l)
RAW=$(du -sb kernel | cut -f1)

echo "manifest before ($FILES files)"
( cd kernel && find . -type f -print0 | sort -z | xargs -0 -P "$(nproc)" -n 200 md5sum ) | sort -k2 > before.md5

echo "nvz --mode normal"; TN=$(t "$HZ" pack kernel nvz-normal.nvz --mode normal -q); SN=$(stat -c%s nvz-normal.nvz)
echo "nvz --mode small";  TS=$(t "$HZ" pack kernel nvz-small.nvz  --mode small  -q); SS=$(stat -c%s nvz-small.nvz)
echo "tar.xz -9e";        TX=$(t bash -c 'tar -cf - kernel | xz -9e -T0 -c > ref.tar.xz'); SX=$(stat -c%s ref.tar.xz)
echo "tar.zst -19";       TZ=$(t bash -c 'tar -cf - kernel | zstd -19 --long=27 -T0 -q -o ref.tar.zst -f'); SZ=$(stat -c%s ref.tar.zst)
echo "7z LZMA2 -mx=9, 512 MiB dictionary, solid"
rm -f ref.7z; T7=$(t 7z a -t7z -m0=LZMA2 -mx=9 -md=512m -ms=on -mmt="$(nproc)" ref.7z kernel); S7=$(stat -c%s ref.7z)

echo "unpacking nvz-small and checking with md5sum -c"
rm -rf out; mkdir out; TU=$(t "$HZ" unpack nvz-small.nvz out)
CHECK=$( cd out/kernel && md5sum -c "$WORK/before.md5" 2>&1 | grep -c ': OK$' || true )
BAD=$( cd out/kernel && md5sum -c "$WORK/before.md5" 2>&1 | grep -c ': FAILED' || true )
( cd out/kernel && find . -type f -print0 | sort -z | xargs -0 -P "$(nproc)" -n 200 md5sum ) | sort -k2 > after.md5
SAME=$(cmp -s before.md5 after.md5 && echo yes || echo NO)

mkdir -p "$ROOT/resultat"
cat > "$ROOT/resultat/karntest.json" <<JSON
{"version":"$VER","nedladdad_tar_xz":$DL,"rabyte":$RAW,"filer":$FILES,
 "nvz_normal":{"byte":$SN,"packtid":$TN},"nvz_liten":{"byte":$SS,"packtid":$TS},
 "tar_xz_9e":{"byte":$SX,"packtid":$TX},"tar_zst_19":{"byte":$SZ,"packtid":$TZ},
 "lzma2_extrem_7z":{"byte":$S7,"packtid":$T7},
 "uppacktid_liten":$TU,"md5_ok":$CHECK,"md5_fel":$BAD,"manifest_lika":"$SAME"}
JSON

pct() { python3 -c "print(f'{($1-$2)/$2*100:+.1f} %')"; }
echo
echo "linux-$VER: $RAW bytes in $FILES files"
printf '%-24s %14s %10s %12s\n' packer bytes "vs dl" "pack time"
printf '%-24s %14d %10s %12s\n' "kernel.org tar.xz" "$DL" "0.0 %" "-"
printf '%-24s %14d %10s %11ds\n' "tar.xz -9e (local)" "$SX" "$(pct "$SX" "$DL")" "$TX"
printf '%-24s %14d %10s %11ds\n' "tar.zst -19 --long" "$SZ" "$(pct "$SZ" "$DL")" "$TZ"
printf '%-24s %14d %10s %11ds\n' "7z LZMA2 -mx=9 512M" "$S7" "$(pct "$S7" "$DL")" "$T7"
printf '%-24s %14d %10s %11ds\n' "nvz --mode normal" "$SN" "$(pct "$SN" "$DL")" "$TN"
printf '%-24s %14d %10s %11ds\n' "nvz --mode small" "$SS" "$(pct "$SS" "$DL")" "$TS"
echo
echo "unpack (small): ${TU}s · md5sum -c: $CHECK OK, $BAD FAILED · manifests identical: $SAME"
