#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Builds the macOS and Windows packages of the portable tool into dist/.
#
#   bash packaging/build-portable.sh
#
# Three artifacts, all wrapping portable/nvzpy.py (pure stdlib Python):
#
#   nvz-$V.pyz          zipapp, runs anywhere:  python3 nvz-2.0.pyz pack ...
#   nvz-$V-macos.tar.gz nvz script + install.sh (-> /usr/local/bin or ~/.local/bin)
#   nvz-$V-windows.zip  nvzpy.py + nvz.cmd + install.ps1 (-> %LOCALAPPDATA%\Programs\nvz)
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
(cd dist && sha256sum nvz-$V.pyz nvz-$V-macos.tar.gz nvz-$V-windows.zip nvz-$V.tar.gz 2>/dev/null > SHA256SUMS)
cat dist/SHA256SUMS
