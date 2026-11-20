#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds a binary .deb of hyperZip for Ubuntu/Debian amd64 into dist/.
#
#   bash packaging/build-deb.sh
#
# The package installs /usr/bin/{hz,hzcm,hzjpg} and their man pages —
# the compiled, ready-to-run Linux tool, as opposed to the portable
# Python edition the macOS/Windows packages carry. Built from the
# working tree's binaries: run make first (the script refuses stale or
# missing binaries rather than silently packaging them).
set -euo pipefail
cd "$(dirname "$0")/.."
V=2.0
ARCH=$(dpkg --print-architecture)
# make -q asks the build system itself whether the binaries are current.
make -q src/hz src/hzcm src/hzjpg 2>/dev/null \
    || { echo "binaries missing or out of date — run make"; exit 1; }
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
make install DESTDIR="$STAGE" PREFIX=/usr >/dev/null
gzip -9n "$STAGE"/usr/share/man/man1/*.1
mkdir -p "$STAGE/usr/share/doc/hz" "$STAGE/DEBIAN"
cp README.md "$STAGE/usr/share/doc/hz/"
cat > "$STAGE/usr/share/doc/hz/copyright" <<EOF
hyperZip (hz) — Copyright (C) 2026 Hugo Wikström <hugo.wikstrom@gmail.com>
License: GPL-2.0-only. On Debian systems the full text is in
/usr/share/common-licenses/GPL-2.
EOF
cat > "$STAGE/DEBIAN/control" <<EOF
Package: hz
Version: $V
Architecture: $ARCH
Maintainer: Hugo Wikström <hugo.wikstrom@gmail.com>
Depends: libzstd1, liblzma5
Section: utils
Priority: optional
Homepage: https://nattviken.com/en/projects/hz/
Description: hyperZip - content-aware archiver that knows what it is packing
 hyperZip (command: hz) classifies its input and applies content-specific
 lossless transforms - deduplication, similarity sorting, PNG unfolding
 with zlib recipe search, lossless JPEG recoding and context-mixing
 compression - before choosing a codec per block by probing a sample.
 Everything is bit-exact; whatever cannot be reproduced exactly is
 stored raw.
EOF
mkdir -p dist
dpkg-deb --build --root-owner-group "$STAGE" "dist/hz_${V}_${ARCH}.deb" >/dev/null
ls -l "dist/hz_${V}_${ARCH}.deb"
