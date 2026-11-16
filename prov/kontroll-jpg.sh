#!/usr/bin/env bash
# Packar och packar upp VARJE jpeg i en katalog och kräver identiska bytes.
set -u
rot="${1:?katalog}"
export NV="$(cd "$(dirname "$0")/.." && pwd)/src/nvjpg"
kontroll() {
    f="$1"; t=$(mktemp -d)
    if ! "$NV" packa < "$f" > "$t/a.nvj" 2>/dev/null; then echo "RÅFALL $f"; rm -rf "$t"; return; fi
    "$NV" packaupp < "$t/a.nvj" > "$t/a.jpg" 2>/dev/null
    if cmp -s "$f" "$t/a.jpg"; then
        printf 'OK %s %s\n' "$(stat -c%s "$f")" "$(stat -c%s "$t/a.nvj")"
    else echo "SKILJER $f"; fi
    rm -rf "$t"
}
export -f kontroll
find "$rot" -type f \( -iname '*.jpg' -o -iname '*.jpeg' \) -print0 \
  | xargs -0 -P 24 -I{} bash -c 'kontroll "$@"' _ {} \
  | awk '/^OK/{n++; i+=$2; u+=$3} /^SKILJER/{s++; print} /^RÅFALL/{r++; print}
         END{printf "\n%d bit-exakta, %d skiljer, %d råfall\n%.1f MB -> %.1f MB  %+.2f%%\n", n, s+0, r+0, i/1e6, u/1e6, -100*(i-u)/i}'
