#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The three flaws from Ted Lyngmo's code review of 2026-11-18 — as
# regression tests. Every check is GREEN when the flaw is fixed.
#
#   bash prov/brister.sh        ("brister" = flaws; the published name stays)
#
# The review is the most valuable feedback the project has received, and
# the point of keeping it here is that the three flaws shared one cause:
# the packer's own self-check guards the wrong boundary. `nvz test` and the
# reconstruction suites pack and unpack ON THE SAME MACHINE, in a
# successful flow, with readable files and free disk. None of them could
# see any of this.
set -uo pipefail
export LC_ALL=C
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HZ="$ROOT/src/hz"
WORK="$(mktemp -d /tmp/nvz-flaws-XXXXXX)"
failed=0
cleanup() { chmod -R u+rwX "$WORK" 2>/dev/null; rm -rf "$WORK"; }
trap cleanup EXIT

ok()  { printf '  \033[32mok\033[0m    %s\n' "$1"; }
bad() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; failed=1; }

echo "flaw 1: the PNG stream is verified against its CRC, and the recipe carries the engine fingerprint"
# The recipe is four zlib parameters, and they do NOT determine the output
# stream — that depends on the implementation. The test patches the recipe
# in the register (same string length) and tries both outcomes: a recipe
# whose truth lies in the search space must be rescued, one that does not
# must be REFUSED, never corrupted.
mkdir -p "$WORK/images"
n=0
while IFS= read -r f; do cp "$f" "$WORK/images/" 2>/dev/null && n=$((n+1)); [ $n -ge 120 ] && break; done \
    < <(find "$ROOT/korpus/png" -name '*.png' 2>/dev/null)
if [ "$n" -eq 0 ]; then
    echo "        (skipped: korpus/png missing)"
else
    "$HZ" pack "$WORK/images" "$WORK/p.nvz" --mode normal --codec zstd3 -q
    python3 - "$WORK" <<'PY'
import re, subprocess, sys
work = sys.argv[1]
d = open(f'{work}/p.nvz','rb').read()
pos = int.from_bytes(d[-16:-8],'little')
packed = d[pos:len(d)-16]
xz = packed[:6] == b'\xfd7zXZ\x00'
raw = subprocess.run(['xz','-dc'] if xz else ['zstd','-dqc'], input=packed, capture_output=True).stdout
m = re.search(rb'(\d+),(\d+),(\d+),(\d+),z[0-9.]+-[0-9a-f]{8}', raw)
if not m:
    open(f'{work}/no-recipe','w').write('1'); raise SystemExit(0)
level, window = int(m.group(1)), int(m.group(2))
def emit(newraw, target):
    packed2 = subprocess.run(['xz','-9e','-T1','-c'] if xz else ['zstd','-19','-q','-c'],
                             input=newraw, capture_output=True).stdout
    open(target,'wb').write(d[:pos] + packed2 + pos.to_bytes(8,'little') + len(newraw).to_bytes(8,'little'))
old = m.group(0)
other_level = b'%d' % (1 if level != 1 else 2) + old[len(m.group(1)):]
emit(raw.replace(old, other_level, 1), f'{work}/wrong-level.nvz')
other_window = b'%d,%d' % (level, 11 if window != 11 else 12) + old[len(m.group(1))+1+len(m.group(2)):]
emit(raw.replace(old, other_window, 1), f'{work}/wrong-window.nvz')
PY
    if [ -f "$WORK/no-recipe" ]; then
        echo "        (skipped: no PNG in the sample could be unfolded)"
    else
        rm -rf "$WORK/uA"
        if "$HZ" unpack "$WORK/wrong-level.nvz" "$WORK/uA" -q 2>/dev/null &&
           diff -r "$WORK/images" "$WORK/uA/images" >/dev/null 2>&1; then
            ok "a recipe that fails to reproduce the stream is re-searched and rescued bit-exact"
        else
            bad "the fallback search does not find the right parameters"
        fi
        rm -rf "$WORK/uB"
        output=$("$HZ" unpack "$WORK/wrong-window.nvz" "$WORK/uB" 2>&1); status=$?
        if [ $status -ne 0 ] && grep -q "deflate engine" <<<"$output"; then
            ok "an impossible recipe is REFUSED with the engine fingerprints named — never silent corruption"
        else
            bad "an impossible recipe gave status $status instead of a refusal"
        fi
    fi
fi

echo "flaw 2: write failures are errors, not silent data loss"
printf 'data data data\n' > "$WORK/d.txt"
"$HZ" -c "$WORK/d.txt" > /dev/full 2>/dev/null
[ $? -ne 0 ] && ok "-c to /dev/full exits nonzero (gzip gives 1)" \
             || bad "-c to /dev/full gave 0"

mkdir -p "$WORK/full"; head -c 3000000 /dev/urandom | base64 > "$WORK/full/big.txt"
( cd "$WORK/full" && md5sum big.txt > before.md5 )
( cd "$WORK/full" && ulimit -f 200 && "$HZ" big.txt >/dev/null 2>&1 )
if [ -f "$WORK/full/big.txt" ] && ( cd "$WORK/full" && md5sum -c before.md5 >/dev/null 2>&1 ); then
    ok "the original survives an aborted archive (file-size limit), md5 unchanged"
else
    bad "the original was lost when the archive could not be finished"
fi
[ -f "$WORK/full/big.txt.hz" ] && bad "a truncated archive was left behind" \
                               || ok "the half-written archive was cleaned away"

echo "flaw 3: an unreadable file does not stop the rest"
mkdir -p "$WORK/several"; cd "$WORK/several"
printf 'aaa\n' > a.txt; printf 'bbb\n' > b.txt; printf 'ccc\n' > c.txt; chmod 000 b.txt
output=$("$HZ" a.txt b.txt c.txt 2>&1); status=$?
chmod 644 b.txt
if [ $status -eq 1 ] && [ -f a.txt.hz ] && [ -f c.txt.hz ] && [ -f b.txt ]; then
    ok "a warning, the rest are packed, exit status 1 — gzip's contract, no core dump"
else
    bad "exit status $status (134 = SIGABRT), a.txt.hz=$([ -f a.txt.hz ] && echo yes || echo no) c.txt.hz=$([ -f c.txt.hz ] && echo yes || echo no)"
fi

echo
[ $failed -eq 0 ] && echo "All three flaws remain fixed." || echo "SOMETHING HAS REGRESSED."
exit $failed
