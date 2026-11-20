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
SRC = (ROOT / "portable" / "hzpy.py").read_bytes()
LIC = (ROOT / "LICENSE").read_bytes()

README_MAC = f"""hyperZip {V} (hz) — portable archiver for macOS
=====================================

Needs: python3 (macOS: xcode-select --install, or python.org).

Install:   ./install.sh        (puts `hz` in /usr/local/bin if writable,
                                otherwise ~/.local/bin)
Or run in place:   ./hz pack <dir> <archive.hz>

Usage:
  hz pack   <dir> <archive.hz>
  hz unpack <archive.hz> <target-dir>
  hz list   <archive.hz>
  hz test   <archive.hz>

Archives are ordinary hZ v3 archives (codecs ra/xz9e), bit-exact and
interchangeable with the optimized Linux hz (its `--portable` mode).
License: GPL-2.0-only, see LICENSE. https://nattviken.com/projekt/hz/
"""

README_WIN = f"""hyperZip {V} (hz) — portable archiver for Windows
=======================================

Needs: Python 3.8+ from python.org or the Microsoft Store.

Install:  right-click install.ps1 -> Run with PowerShell
          (copies hz to %LOCALAPPDATA%\\Programs\\hz and adds it to PATH)
Or run in place:  hz.cmd pack <dir> <archive.hz>

Usage:
  hz pack   <dir> <archive.hz>
  hz unpack <archive.hz> <target-dir>
  hz list   <archive.hz>
  hz test   <archive.hz>

Symbolic links need Windows developer mode; otherwise they are skipped
with a warning (exit status 2), never silently wrong.

Archives are ordinary hZ v3 archives (codecs ra/xz9e), bit-exact and
interchangeable with the optimized Linux hz (its `--portable` mode).
License: GPL-2.0-only, see LICENSE.txt. https://nattviken.com/projekt/hz/
"""

INSTALL_SH = """#!/bin/sh
# Installs hz. /usr/local/bin if writable, else ~/.local/bin.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
command -v python3 >/dev/null || { echo "python3 is required (xcode-select --install)"; exit 1; }
dst=/usr/local/bin
[ -w "$dst" ] || dst="$HOME/.local/bin"
mkdir -p "$dst"
install -m 755 "$here/hz" "$dst/hz"
echo "installed: $dst/hz"
case ":$PATH:" in *":$dst:"*) ;; *) echo "note: add $dst to PATH";; esac
"""

NVZ_CMD = '@echo off\r\nwhere py >nul 2>nul && (py -3 "%~dp0hzpy.py" %*) || (python "%~dp0hzpy.py" %*)\r\n'

INSTALL_PS1 = '\r\n'.join("""# Installs hz for the current user and puts it on PATH.
$dst = Join-Path $env:LOCALAPPDATA 'Programs\\hz'
New-Item -ItemType Directory -Force -Path $dst | Out-Null
Copy-Item -Force "$PSScriptRoot\\hzpy.py","$PSScriptRoot\\hz.cmd","$PSScriptRoot\\LICENSE.txt" $dst
$path = [Environment]::GetEnvironmentVariable('Path', 'User')
if ($path -notlike "*$dst*") {
    [Environment]::SetEnvironmentVariable('Path', "$path;$dst", 'User')
    Write-Host "hz installed in $dst and added to PATH (open a new terminal)."
} else {
    Write-Host "hz installed in $dst."
}""".split('\n')) + '\r\n'

# --- zipapp: python3 hz-2.0.pyz <cmd> ... ---------------------------------
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    zi = zipfile.ZipInfo("__main__.py", date_time=ZDATE)
    zi.external_attr = 0o644 << 16
    z.writestr(zi, SRC)
pyz = DIST / f"hz-{V}.pyz"
pyz.write_bytes(b"#!/usr/bin/env python3\n" + buf.getvalue())
pyz.chmod(0o755)

# --- macOS tarball -----------------------------------------------------------
def tadd(tf, name, data, mode):
    ti = tarfile.TarInfo(f"hz-{V}-macos/{name}")
    ti.size, ti.mtime, ti.mode = len(data), int(T), mode
    ti.uname = ti.gname = "root"
    tf.addfile(ti, io.BytesIO(data))

# "w:gz" stamps the current time into the gzip header; an explicit
# GzipFile with mtime=T keeps rebuilds byte-identical.
import gzip
gz = gzip.GzipFile(DIST / f"hz-{V}-macos.tar.gz", "wb", compresslevel=9, mtime=int(T))
with tarfile.open(fileobj=gz, mode="w") as tf:
    tadd(tf, "hz", SRC, 0o755)
    tadd(tf, "install.sh", INSTALL_SH.encode(), 0o755)
    tadd(tf, "README.txt", README_MAC.encode(), 0o644)
    tadd(tf, "LICENSE", LIC, 0o644)
gz.close()

# --- Windows zip -------------------------------------------------------------
with zipfile.ZipFile(DIST / f"hz-{V}-windows.zip", "w", zipfile.ZIP_DEFLATED) as z:
    for name, data in [("hzpy.py", SRC), ("hz.cmd", NVZ_CMD.encode()),
                       ("install.ps1", INSTALL_PS1.encode()),
                       ("README.txt", README_WIN.encode().replace(b"\n", b"\r\n")),
                       ("LICENSE.txt", LIC.replace(b"\n", b"\r\n"))]:
        zi = zipfile.ZipInfo(f"hz-{V}-windows/{name}", date_time=ZDATE)
        zi.external_attr = 0o644 << 16
        z.writestr(zi, data)

print("built:", ", ".join(p.name for p in sorted(DIST.iterdir())))
