#!/usr/bin/env bash
# Packs and unpacks EVERY jpeg in a directory and demands identical bytes.
set -u
root="${1:?directory}"
export NV="$(cd "$(dirname "$0")/.." && pwd)/src/nvjpg"
check() {
    f="$1"; t=$(mktemp -d)
    if ! "$NV" pack < "$f" > "$t/a.nvj" 2>/dev/null; then echo "RAWCASE $f"; rm -rf "$t"; return; fi
    "$NV" unpack < "$t/a.nvj" > "$t/a.jpg" 2>/dev/null
    if cmp -s "$f" "$t/a.jpg"; then
        printf 'OK %s %s\n' "$(stat -c%s "$f")" "$(stat -c%s "$t/a.nvj")"
    else echo "DIFFERS $f"; fi
    rm -rf "$t"
}
export -f check
find "$root" -type f \( -iname '*.jpg' -o -iname '*.jpeg' \) -print0 \
  | xargs -0 -P 24 -I{} bash -c 'check "$@"' _ {} \
  | awk '/^OK/{n++; i+=$2; u+=$3} /^DIFFERS/{s++; print} /^RAWCASE/{r++; print}
         END{printf "\n%d bit-exact, %d differ, %d raw cases\n%.1f MB -> %.1f MB  %+.2f%%\n", n, s+0, r+0, i/1e6, u/1e6, -100*(i-u)/i}'
