#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds the macOS and Windows packages of the portable tool into dist/.
#
#   bash packaging/build-portable.sh
#
# Three artifacts, all wrapping portable/hzpy.py (pure stdlib Python):
#
#   hz-$V.pyz          zipapp, runs anywhere:  python3 hz-2.0.pyz pack ...
#   hz-$V-macos.tar.gz hz script + install.sh (-> /usr/local/bin or ~/.local/bin)
#   hz-$V-windows.zip  hzpy.py + hz.cmd + install.ps1 (-> %LOCALAPPDATA%\Programs\hz)
#
# Deterministic: file timestamps inside the archives are fixed to the
# release date, entries are sorted, so a rebuild of the same commit is
# byte-identical and the published SHA256SUMS stays true.
set -euo pipefail
cd "$(dirname "$0")/.."
V=2.0
STAMP="2026-11-19 00:00:00"
mkdir -p dist
python3 packaging/build-portable.py "$V" "$STAMP"
(cd dist && sha256sum hz-$V.pyz hz-$V-macos.tar.gz hz-$V-windows.zip hz-$V.tar.gz 2>/dev/null > SHA256SUMS)
cat dist/SHA256SUMS
