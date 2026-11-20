#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# The 44-edge-case suite: builds a tree of empty files, duplicates,
# symlinks (relative, broken, to directories), newline and non-ASCII file
# names, odd permissions and 1985/2031 timestamps; packs it in every mode
# and demands every byte, permission and timestamp back. Also tests the
# gzip surface contract (exit statuses, -c/-k/-r, stdin) and, when built,
# the GPU part.
#
#   bash tests/test-all.sh
#
# None of the edge cases is decorative. Every line below corresponds to
# something that actually breaks in archivers: empty files that look like
# missing ones, links that point out of the tree, file names with embedded
# newlines, files that are exact copies of each other (which dedup must
# hand back as two real files, not as a link), and permissions that
# silently become 644.
set -u
root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf -- "$work"' EXIT
failed=0
pass() { printf '  \033[32mok\033[0m    %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m  %s\n' "$1"; failed=$((failed+1)); }

printf '\nnvpack test suite\n\n'

# --- 1. the building blocks on their own ------------------------------------
printf 'Building blocks\n'

bits=$work/bits; mkdir -p "$bits"
head -c 3000000 /dev/urandom > "$bits/random.bin"
find "$root/src" -name '*.c' -o -name '*.py' | xargs cat > "$bits/text.txt" 2>/dev/null

if "$root/src/hzcm" pack < "$bits/text.txt" | "$root/src/hzcm" unpack | cmp -s - "$bits/text.txt"
then pass "nvcm: text round trip"; else fail "nvcm: text round trip"; fi

if "$root/src/hzcm" pack < "$bits/random.bin" | "$root/src/hzcm" unpack | cmp -s - "$bits/random.bin"
then pass "nvcm: incompressible random data"; else fail "nvcm: incompressible random data"; fi

: > "$bits/empty"
if "$root/src/hzcm" pack < "$bits/empty" | "$root/src/hzcm" unpack | cmp -s - "$bits/empty"
then pass "nvcm: empty stream"; else fail "nvcm: empty stream"; fi

jpg="$(find "$root/corpus/foto" -name '*.jpg' 2>/dev/null | head -1)"
if [ -n "$jpg" ]; then
    if "$root/src/hzjpg" pack < "$jpg" | "$root/src/hzjpg" unpack | cmp -s - "$jpg"
    then pass "nvjpg: baseline JPEG round trip"; else fail "nvjpg: baseline JPEG"; fi
fi

# A PNG with no known deflate engine must be refused, not stored wrong.
python3 - "$root" <<'PY' && pass "png: unknown engine refused instead of guessed" || fail "png: unknown engine"
import sys, zlib, struct, pathlib
sys.path.insert(0, sys.argv[1] + '/src')
import png
# A PNG whose IDAT was packed with a stream no zlib setting reproduces
# (here: raw deflate with an atypical level combination via two
# concatenated streams).
raw = bytes(range(256)) * 64
c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, 0)
stream = c.compress(raw[:100]) + c.flush(zlib.Z_FULL_FLUSH)
c2 = zlib.compressobj(1, zlib.DEFLATED, 15, 9, 0)
stream += c2.compress(raw[100:]) + c2.flush()
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
blob = (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0))
        + chunk(b'IDAT', stream) + chunk(b'IEND', b''))
p = pathlib.Path(sys.argv[1]) / 'tests' / '.tmp.png'
p.write_bytes(blob)
try:
    r = png.unfold(str(p))             # the module's API name
finally:
    p.unlink()
sys.exit(0 if r is None else 1)
PY

# --- 2. the whole tree of edge cases -----------------------------------------
printf '\nEdge cases in the tree\n'
t="$work/tree"
mkdir -p "$t/empty-dir" "$t/deep/one/two/three/four" "$t/names"
: > "$t/zero-byte"
printf 'a' > "$t/one-byte"
printf 'same content\n' > "$t/dup-a"
printf 'same content\n' > "$t/dup-b"
printf 'same content\n' > "$t/deep/one/dup-c"
head -c 200000 /dev/urandom > "$t/random.bin"
head -c 1000000 /dev/zero > "$t/zeros.bin"
seq 1 50000 > "$t/numbers.txt"
printf '#!/bin/sh\necho hi\n' > "$t/script.sh"; chmod 755 "$t/script.sh"
chmod 600 "$t/one-byte"; chmod 444 "$t/zero-byte"
printf 'secret' > "$t/locked"; chmod 400 "$t/locked"
ln -s ../dup-a "$t/deep/link-relative"
ln -s /does/not/exist "$t/link-broken"
ln -s . "$t/link-dir"
printf 'x' > "$t/names/with spaces and ÅÄÖ.txt"
printf 'y' > "$t/names/$(printf 'new\nline')"
printf 'z' > "$t/names/-starts-with-dash"
printf 'q' > "$t/names/.hidden"
cp "$jpg" "$t/photo.jpg" 2>/dev/null || true
find "$root/corpus/png" -name '*.png' 2>/dev/null | head -2 | while read -r f; do cp "$f" "$t/image-$(basename "$f")"; done
touch -d '1985-03-14 12:34:56.123456789' "$t/numbers.txt"
touch -d '2031-01-01 00:00:00' "$t/one-byte"
chmod 700 "$t/empty-dir"

# The gpu mode is tried only if the gpu part is built AND the device
# answers. A machine without vulkan must run the rest of the suite without
# seeing a single failure.
modes="fast normal small"
if [ -x "$root/src/hzgpu" ] && "$root/src/hzgpu" device --quiet >/dev/null 2>&1; then
    modes="$modes gpu"
fi

for mode in $modes; do
    ark="$work/test-$mode.nv"; out="$work/out-$mode"
    if ! "$root/src/hz" pack "$t" "$ark" -q --mode "$mode" 2>"$work/err.txt"; then
        fail "mode $mode: packing failed"; sed 's/^/        /' "$work/err.txt"; continue
    fi
    mkdir -p "$out"
    if ! "$root/src/hz" unpack "$ark" "$out" -q 2>"$work/err.txt"; then
        fail "mode $mode: unpacking failed"; sed 's/^/        /' "$work/err.txt"; continue
    fi
    if diff -rq --no-dereference "$t" "$out/$(basename "$t")" > "$work/diff.txt" 2>&1; then
        pass "mode $mode: contents identical ($(stat -c%s "$ark") bytes)"
    else
        fail "mode $mode: contents differ"; head -5 "$work/diff.txt" | sed 's/^/        /'
    fi
    # Permissions and times are compared separately: diff does not see them.
    ( cd "$t" && find . -printf '%P %m %T@\n' | sort ) > "$work/a.txt"
    ( cd "$out/$(basename "$t")" && find . -printf '%P %m %T@\n' | sort ) > "$work/b.txt"
    if cmp -s "$work/a.txt" "$work/b.txt"; then
        pass "mode $mode: permissions and times identical"
    else
        fail "mode $mode: permissions or times differ"
        diff "$work/a.txt" "$work/b.txt" | head -5 | sed 's/^/        /'
    fi
    # Duplicates must come back as real files, not as hard links.
    if [ "$(stat -c%h "$out/$(basename "$t")/dup-a")" = 1 ]; then
        pass "mode $mode: dedup returns independent files"
    else
        fail "mode $mode: dedup left hard links behind"
    fi
done

# The Swedish verbs (packa, packa-upp, lista, prova, --läge) are a permanent
# alias surface: published measurements and old scripts call them, so this
# one round exercises the aliases ON PURPOSE — everything else in the suite
# speaks English to the binary.
if "$root/src/hz" packa "$t" "$work/alias.nv" -q --läge snabb 2>/dev/null \
   && mkdir -p "$work/out-alias" \
   && "$root/src/hz" packa-upp "$work/alias.nv" "$work/out-alias" -q 2>/dev/null \
   && diff -rq --no-dereference "$t" "$work/out-alias/$(basename "$t")" >/dev/null 2>&1 \
   && "$root/src/hz" lista "$work/alias.nv" >/dev/null 2>&1
then pass "Swedish aliases: packa/packa-upp/lista round trip"
else fail "Swedish aliases: the permanent alias surface broke"; fi

# --- 2b. the GPU part (nvgpu and the nvlz codec) -----------------------------
# Three things are tested, and they are the three that make the gpu path
# usable or not: the hash is EXACTLY the Python side's (otherwise dedup
# points wrong), the matcher's answers are identical on gpu and cpu
# (otherwise the timing comparison means nothing), and an nvlz archive can
# be unpacked WITHOUT a gpu — an archive must never be hostage to a
# graphics driver.
printf '\nThe GPU part\n'
if [ ! -x "$root/src/hzgpu" ]; then
    printf '  --    skipped: src/hzgpu is not built (make gpu)\n'
elif ! "$root/src/hzgpu" device --quiet >/dev/null 2>&1; then
    printf '  --    skipped: no vulkan device answers here\n'
else
    if "$root/src/hzgpu" selftest --quiet > "$work/gputest.txt" 2>&1
    then pass "hzgpu selftest: test vectors, gpu==cpu, matcher==cpu"
    else fail "hzgpu selftest failed"; sed 's/^/        /' "$work/gputest.txt" | head -8; fi

    # The hash against python's own hashlib, on real files from src/.
    find "$root/src" -maxdepth 1 -type f | sort > "$work/gpufiles.txt"
    "$root/src/hzgpu" hash --quiet < "$work/gpufiles.txt" > "$work/gpuhash.txt" 2>/dev/null
    if python3 - "$work/gpuhash.txt" <<'PYEOF'
import hashlib, sys
bad = 0
for line in open(sys.argv[1]):
    h, _, path = line.rstrip('\n').partition('  ')
    with open(path, 'rb') as f:
        if hashlib.blake2b(f.read(), digest_size=16).hexdigest() != h:
            print('differs:', path); bad += 1
sys.exit(1 if bad else 0)
PYEOF
    then pass "nvgpu hash: bit-exact vs hashlib.blake2b(digest_size=16)"
    else fail "nvgpu hash: deviates from hashlib"; fi

    # The codec round trip on text, random data and the empty stream.
    for name in text.txt random.bin empty; do
        if "$root/src/hzgpu" pack --quiet < "$bits/$name" \
           | "$root/src/hzgpu" unpack | cmp -s - "$bits/$name"
        then pass "nvlz: $name round trip"
        else fail "nvlz: $name round trip"; fi
    done

    # Data larger than one chunk (32 MiB) — the chunk boundary is where a
    # packer tends to drop a byte.
    cat "$bits/text.txt" "$bits/text.txt" "$bits/text.txt" "$bits/text.txt" \
        "$bits/text.txt" "$bits/text.txt" > "$work/big.bin" 2>/dev/null
    if [ "$(stat -c%s "$work/big.bin")" -gt 1000 ]; then
        if "$root/src/hzgpu" pack --quiet < "$work/big.bin" \
           | "$root/src/hzgpu" unpack | cmp -s - "$work/big.bin"
        then pass "nvlz: $(( $(stat -c%s "$work/big.bin") / 1048576 )) MiB across the chunk boundary"
        else fail "nvlz: multiple chunks"; fi
    fi

    # Unpacking must survive entirely without vulkan. VK_ICD_FILENAMES
    # pointed at an empty file makes the loader find no driver at all.
    "$root/src/hzgpu" pack --quiet < "$bits/text.txt" > "$work/without.nvlz"
    if VK_ICD_FILENAMES=/dev/null VK_DRIVER_FILES=/dev/null \
       "$root/src/hzgpu" unpack < "$work/without.nvlz" | cmp -s - "$bits/text.txt"
    then pass "nvlz: unpacking without a vulkan driver"
    else fail "nvlz: unpacking requires a gpu — the archive is hostage"; fi

    # And packing must be able to fall back to the cpu with the same format.
    if VK_ICD_FILENAMES=/dev/null VK_DRIVER_FILES=/dev/null \
       "$root/src/hzgpu" pack --cpu --quiet < "$bits/text.txt" \
       | "$root/src/hzgpu" unpack | cmp -s - "$bits/text.txt"
    then pass "nvlz: packing without a gpu (cpu matcher, same format)"
    else fail "nvlz: the cpu fallback does not pack"; fi

    # A whole archive with the codec forced, and the register must say nvlz.
    if "$root/src/hz" pack "$t" "$work/nvlz.nv" -q --codec nvlz 2>"$work/err.txt" \
       && "$root/src/hz" unpack "$work/nvlz.nv" "$work/out-nvlz" -q 2>>"$work/err.txt" \
       && diff -rq --no-dereference "$t" "$work/out-nvlz/$(basename "$t")" >/dev/null 2>&1
    then pass "nvz --codec nvlz: archive round trip"
    else fail "nvz --codec nvlz: archive does not round-trip"
         head -3 "$work/err.txt" | sed 's/^/        /'; fi
    if "$root/src/hz" list "$work/nvlz.nv" 2>/dev/null | grep -q hzlz
    then pass "nvz list: blocks reported as nvlz"
    else fail "nvz list: the codec is missing from the register"; fi
fi

# rans: the interleaved entropy codec round-trips, and the register says so.
if "$root/src/hz" pack "$t" "$work/rans.hz" -q --codec rans \
   && "$root/src/hz" unpack "$work/rans.hz" "$work/out-rans" -q \
   && diff -rq --no-dereference "$t" "$work/out-rans/$(basename "$t")" >/dev/null 2>&1 \
   && "$root/src/hz" list "$work/rans.hz" | grep -q rans
then pass "rans: bit-exact round trip, codec in the register"
else fail "rans: round trip"; fi

# --- 3. a damaged archive must refuse, not guess -----------------------------
printf '\nDamaged archives\n'
ark="$work/test-normal.nv"
cp "$ark" "$work/broken.nv"
printf 'BAD' | dd of="$work/broken.nv" bs=1 seek=2 conv=notrunc status=none
if "$root/src/hz" unpack "$work/broken.nv" "$work/out-broken" -q 2>/dev/null; then
    fail "damaged magic was accepted"
else
    pass "damaged magic is refused"
fi

# --- 4. the gzip surface -----------------------------------------------------
# The surface is gzip's, and what is tested here are exactly those promises:
# the original is replaced, the round trip is bit-exact, the flags mean what
# they mean in gzip, and the exit status separates error (1) from warning
# (2). The tests run in their own directory, because the whole point of the
# surface is that it writes into the directory it stands in.
printf '\nThe command line gzip surface\n'

nv="$root/src/hz"
y="$work/surface"; mkdir -p "$y/tree/sub"
python3 - "$y" <<'PYEOF'
import os, random, sys
y = sys.argv[1]; random.seed(11)
lines = ''.join(f'def f{i}(x):\n    return x * {i}\n' for i in range(300))
open(f'{y}/tree/code.py','w').write(lines)
open(f'{y}/tree/sub/copy.py','w').write(lines)           # duplicate: dedup must see it
open(f'{y}/tree/random.bin','wb').write(bytes(random.randrange(256) for _ in range(20000)))
open(f'{y}/alone.txt','w').write('hey ho\n' * 1500)
os.symlink('code.py', f'{y}/tree/link.py')
PYEOF
chmod 640 "$y/alone.txt"; touch -d '2021-03-04 05:06:07' "$y/alone.txt"
sha="$(sha256sum "$y/alone.txt" | cut -d' ' -f1)"
perms="$(stat -c '%a %Y' "$y/alone.txt")"
cp -a "$y/tree" "$y/tree-orig"

# One file: the original must be gone, and the archive must inherit its
# time and permissions.
( cd "$y" && "$nv" -q alone.txt )
if [ ! -e "$y/alone.txt" ] && [ -f "$y/alone.txt.hz" ]; then
    pass "nvz file: original replaced by file.hz"
else fail "nvz file: the original was not replaced"; fi
if [ "$(stat -c '%a %Y' "$y/alone.txt.hz")" = "$perms" ]; then
    pass "nvz file: the archive inherited time and permissions"
else fail "nvz file: the archive got different times than the file"; fi

( cd "$y" && "$nv" -dq alone.txt.hz )
if [ "$(sha256sum "$y/alone.txt" | cut -d' ' -f1)" = "$sha" ] \
   && [ "$(stat -c '%a %Y' "$y/alone.txt")" = "$perms" ] && [ ! -e "$y/alone.txt.hz" ]; then
    pass "nvz -d: bit-exact back, archive gone"
else fail "nvz -d: the file did not come back unchanged"; fi

# -c and -k leave the file alone, and the stream must unpack again.
( cd "$y" && "$nv" -ckq alone.txt > "$work/via-stdout.hz" )
if [ -f "$y/alone.txt" ] && "$nv" -dcq "$work/via-stdout.hz" | cmp -s - "$y/alone.txt"; then
    pass "-c -k: file untouched, archive to stdout"
else fail "-c -k: the stream or the file is wrong"; fi

# No file name: stdin -> stdout, both directions.
if cat "$y/alone.txt" | "$nv" -9q | "$nv" -dq | cmp -s - "$y/alone.txt"; then
    pass "stdin -> stdout, round trip"
else fail "the pipe did not return the same bytes"; fi

# A directory as operand: ONE archive of the tree, and the tree is kept.
( cd "$y" && "$nv" -q tree )
if [ -f "$y/tree.hz" ] && [ -d "$y/tree" ]; then
    pass "directory: one archive of the tree, source tree kept"
else fail "directory: wrong outcome (archive missing or tree deleted)"; fi
if "$nv" -tq "$y/tree.hz"; then pass "-t: the archive is tested against the register"
else fail "-t: the test rejected a healthy archive"; fi
rm -rf "$y/tree"
( cd "$y" && "$nv" -dq tree.hz )
if diff -rq --no-dereference "$y/tree-orig" "$y/tree" >/dev/null 2>&1; then
    pass "-d on a tree archive: the whole tree back"
else fail "-d on a tree archive: the tree differs"; fi

# -r is gzip's literal behavior: one file at a time, and repeatable.
( cd "$y" && "$nv" -rq tree && "$nv" -rq tree )
if [ -f "$y/tree/code.py.hz" ] && [ ! -e "$y/tree/code.py" ] && [ -L "$y/tree/link.py" ]; then
    pass "-r: file by file, symlinks left alone"
else fail "-r: the sweep did not pack the way gzip does"; fi
( cd "$y" && "$nv" -rdq tree )
if diff -rq --no-dereference "$y/tree-orig" "$y/tree" >/dev/null 2>&1; then
    pass "-r -d: the sweep back bit for bit"
else fail "-r -d: the sweep did not return the tree"; fi

# Exit status: 1 is error, 2 is warning. Scripts hang on the difference.
"$nv" -q "$work/does-not-exist" 2>/dev/null; [ "$?" = 1 ] \
    && pass "exit status 1 when the file does not exist" || fail "wrong exit status for a missing file"
# The legacy suffix: archives from the nvz era are named .nv and must
# unpack with no byte changed.
cp "$work/via-stdout.hz" "$y/legacy.nv"
( cd "$y" && "$nv" -dq legacy.nv )
if [ -f "$y/legacy" ] && cmp -s "$y/legacy" "$y/alone.txt"; then
    pass ".nv archives from the nvz era unpack with the suffix clipped"
else fail "the .nv legacy suffix"; fi

( cd "$y" && "$nv" -q alone.txt && "$nv" -kq alone.txt.hz ) 2>/dev/null; [ "$?" = 2 ] \
    && pass "exit status 2 when the file is already packed" || fail "wrong exit status for an already packed file"
( cd "$y" && "$nv" -dq tree-orig/code.py ) 2>/dev/null; [ "$?" = 2 ] \
    && pass "exit status 2 on an unknown suffix" || fail "wrong exit status for an unknown suffix"
"$nv" --no-such-flag 2>/dev/null; [ "$?" = 1 ] \
    && pass "exit status 1 on an unknown flag" || fail "wrong exit status for an unknown flag"
"$nv" --l 2>/dev/null; [ "$?" = 1 ] \
    && pass "ambiguous abbreviation refused" || fail "ambiguous abbreviation accepted"

# A tree archive is not a byte stream and must not pretend to be one.
( cd "$y" && "$nv" -q tree && "$nv" -dcq tree.hz >/dev/null ) 2>/dev/null; [ "$?" = 1 ] \
    && pass "tree archive refuses to be written to stdout" || fail "a tree archive was poured into a stream"

printf '\n'
if [ "$failed" -eq 0 ]; then printf 'Everything passed.\n\n'; else printf '%d tests failed.\n\n' "$failed"; fi
exit $((failed > 0))
