#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Deterministic package builder for the portable tool. Called by
build-portable.sh; takes <version> <timestamp>."""
import io, os, stat, sys, tarfile, time, zipfile
from pathlib import Path

V, STAMP = sys.argv[1], sys.argv[2]
T = time.mktime(time.strptime(STAMP, "%Y-%m-%d %H:%M:%S"))
ZDATE = tuple(time.localtime(T))[:6]
ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "dist"
SRC = (ROOT / "portable" / "nvzpy.py").read_bytes()
LIC = (ROOT / "LICENSE").read_bytes()

README_MAC = f"""nvz {V} — portable archiver for macOS
=====================================

Needs: python3 (macOS: xcode-select --install, or python.org).

Install:   ./install.sh        (puts `nvz` in /usr/local/bin if writable,
                                otherwise ~/.local/bin)
Or run in place:   ./nvz pack <dir> <archive.nv>

Usage:
  nvz pack   <dir> <archive.nv>
  nvz unpack <archive.nv> <target-dir>
  nvz list   <archive.nv>
  nvz test   <archive.nv>

Archives are ordinary nvz v3 archives (codecs ra/xz9e), bit-exact and
interchangeable with the optimized Linux nvz (`--portable` mode there).
License: GPL-2.0-only, see LICENSE. https://nattviken.com/projekt/nvz/
"""

README_WIN = f"""nvz {V} — portable archiver for Windows
=======================================

Needs: Python 3.8+ from python.org or the Microsoft Store.

Install:  right-click install.ps1 -> Run with PowerShell
          (copies nvz to %LOCALAPPDATA%\\Programs\\nvz and adds it to PATH)
Or run in place:  nvz.cmd pack <dir> <archive.nv>

Usage:
  nvz pack   <dir> <archive.nv>
  nvz unpack <archive.nv> <target-dir>
  nvz list   <archive.nv>
  nvz test   <archive.nv>

Symbolic links need Windows developer mode; otherwise they are skipped
with a warning (exit status 2), never silently wrong.

Archives are ordinary nvz v3 archives (codecs ra/xz9e), bit-exact and
interchangeable with the optimized Linux nvz (`--portable` mode there).
License: GPL-2.0-only, see LICENSE.txt. https://nattviken.com/projekt/nvz/
"""

INSTALL_SH = """#!/bin/sh
# Installs nvz. /usr/local/bin if writable, else ~/.local/bin.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
command -v python3 >/dev/null || { echo "python3 is required (xcode-select --install)"; exit 1; }
dst=/usr/local/bin
[ -w "$dst" ] || dst="$HOME/.local/bin"
mkdir -p "$dst"
install -m 755 "$here/nvz" "$dst/nvz"
echo "installed: $dst/nvz"
case ":$PATH:" in *":$dst:"*) ;; *) echo "note: add $dst to PATH";; esac
"""

NVZ_CMD = '@echo off\r\nwhere py >nul 2>nul && (py -3 "%~dp0nvzpy.py" %*) || (python "%~dp0nvzpy.py" %*)\r\n'

INSTALL_PS1 = '\r\n'.join("""# Installs nvz for the current user and puts it on PATH.
$dst = Join-Path $env:LOCALAPPDATA 'Programs\\nvz'
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item -Force "$PSScriptRoot\\nvzpy.py","$PSScriptRoot\\nvz.cmd","$PSScriptRoot\\LICENSE.txt" $dst
$path = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($path -notlike "*$dst*") {
    [Environment]::SetEnvironmentVariable('Path', "$path;$dst", 'User')
    Write-Host "nvz installed in $dst and added to PATH (open a new terminal)."
} else {
    Write-Host "nvz installed in $dst."
}""".split('\n')) + '\r\n'

# --- zipapp: python3 nvz-2.0.pyz <cmd> ... ---------------------------------
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    zi = zipfile.ZipInfo("__main__.py", date_time=ZDATE)
    zi.external_attr = 0o644 << 16
    z.writestr(zi, SRC)
pyz = DIST / f"nvz-{V}.pyz"
pyz.write_bytes(b"#!/usr/bin/env python3\n" + buf.getvalue())
pyz.chmod(0o755)

# --- macOS tarball -----------------------------------------------------------
def tadd(tf, name, data, mode):
    ti = tarfile.TarInfo(f"nvz-{V}-macos/{name}")
    ti.size, ti.mtime, ti.mode = len(data), int(T), mode
    ti.uname = ti.gname = "root"
    tf.addfile(ti, io.BytesIO(data))

# "w:gz" stamps the current time into the gzip header; an explicit
# GzipFile with mtime=T keeps rebuilds byte-identical.
import gzip
gz = gzip.GzipFile(DIST / f"nvz-{V}-macos.tar.gz", "wb", compresslevel=9, mtime=int(T))
with tarfile.open(fileobj=gz, mode="w") as tf:
    tadd(tf, "nvz", SRC, 0o755)
    tadd(tf, "install.sh", INSTALL_SH.encode(), 0o755)
    tadd(tf, "README.txt", README_MAC.encode(), 0o644)
    tadd(tf, "LICENSE", LIC, 0o644)
gz.close()

# --- Windows zip -------------------------------------------------------------
with zipfile.ZipFile(DIST / f"nvz-{V}-windows.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for name, data in [("nvzpy.py", SRC), ("nvz.cmd", NVZ_CMD.encode()),
                       ("install.ps1", INSTALL_PS1.encode()),
                       ("README.txt", README_WIN.encode().replace(b"\n", b"\r\n")),
                       ("LICENSE.txt", LIC.replace(b"\n", b"\r\n"))]:
        zi = zipfile.ZipInfo(f"nvz-{V}-windows/{name}", date_time=ZDATE)
        zi.external_attr = 0o644 << 16
        z.writestr(zi, data)

print("built:", ", ".join(p.name for p in sorted(DIST.iterdir())))
