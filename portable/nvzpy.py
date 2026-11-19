#!/usr/bin/env python3
"""nvzpy — the portable nvz: pure standard-library Python, slow but correct.

    python3 nvzpy.py pack   <dir> <archive.nv>
    python3 nvzpy.py unpack <archive.nv> <target-dir>
    python3 nvzpy.py list   <archive.nv>
    python3 nvzpy.py test   <archive.nv>

WHY THIS EXISTS. The real nvz is a Linux binary, fully optimized: libzstd
and liblzma in process, fork-based workers, its own context-mixing codec.
None of that exists on a stock macOS or Windows machine. This file is the
other end of the trade: nothing but the Python standard library (lzma,
zlib, hashlib), so it runs wherever Python 3.8+ runs — and it is therefore
slower and its archives are somewhat larger. That is the price, and it is
paid on purpose.

WHAT IT GUARANTEES, exactly:

  * Archives IT writes are ordinary nvz v3 archives restricted to the
    codecs {ra, xz9e, xz9e-j0}, with no transforms. The Linux nvz unpacks
    them bit for bit, and so does this tool itself.
  * Archives the Linux nvz writes WITH `--portable` use the same
    restriction and unpack here bit for bit.
  * Any other nvz archive is read as far as its codecs allow: ra and xz
    blocks always; zstd blocks if the optional `zstandard` pip package is
    installed; nvcm/nvlz blocks and PNG/JPEG-transformed objects are
    refused with a message that says exactly what to do (repack with
    --portable), never with wrong data.

WHAT IT DOES NOT DO: the content-aware transforms (PNG recipes depend on
the deflate engine, nvjpg is C), the zstd codecs, the context mixer, and
the gzip-style surface. This is the rowing boat, not the ship — but it
floats everywhere.

Windows notes: symbolic links are stored if the OS lets us read them and
recreated if the OS lets us create them (developer mode); otherwise they
are skipped with a warning and exit status 2. Permission bits beyond
read-only have no meaning there and are stored/applied best-effort.
"""
import hashlib
import io
import lzma
import os
import stat as stat_mod
import struct
import sys
import time

MAGIC = b"NVPACK\x01"
REG2 = b"NVZREG2\x00"
BLOCK_CAP = 192 << 20
OPAQUE_CAP = 16 << 30
DICT_CAP = 256 << 20          # python builds the dict in RAM; stay modest
PROBE = 4 << 20

# Format strings (Swedish, frozen — see the language note in src/cpp/nvz.hpp).
TEXT_EXTS = {
    ".txt", ".md", ".json", ".jsonl", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx",
    ".py", ".rs", ".go", ".c", ".h", ".cpp", ".hpp", ".java", ".sh", ".bash", ".zsh",
    ".sql", ".css", ".scss", ".html", ".htm", ".xml", ".svg", ".yml", ".yaml",
    ".toml", ".ini", ".cfg", ".conf", ".service", ".log", ".csv", ".tsv", ".po",
    ".pot", ".map", ".lock", ".patch", ".diff", ".rst", ".tex", ".webmanifest"}
OPAQUE_EXTS = {
    ".mp4", ".m4a", ".m4v", ".mov", ".mkv", ".webm", ".mp3", ".ogg", ".opus",
    ".flac", ".webp", ".avif", ".heic", ".gif", ".zip", ".gz", ".tgz", ".bz2",
    ".xz", ".zst", ".7z", ".rar", ".br", ".woff", ".woff2", ".pdf", ".jxl",
    ".wasm", ".deb", ".rpm", ".whl", ".jar", ".apk", ".odt", ".ods", ".docx",
    ".xlsx", ".pptx"}


class NvzError(Exception):
    pass


def blake16(data):
    return hashlib.blake2b(data, digest_size=16).digest()


# --- codecs -------------------------------------------------------------------

def xz_filters(n, pb):
    """LZMA2 preset 9e with a dictionary that covers the block (the same
    insight as in the C++ codec: a repeat further back than the dictionary
    cannot be found, and the sort puts exactly such repeats in the block)."""
    d = 1 << 20
    while d < n and d < DICT_CAP:
        d <<= 1
    return [{"id": lzma.FILTER_LZMA2,
             "preset": 9 | lzma.PRESET_EXTREME,
             "dict_size": d,
             "pb": pb}]


def compress(codec, data):
    if codec == "ra":
        return data
    if codec == "xz9e":
        return lzma.compress(data, format=lzma.FORMAT_XZ,
                             check=lzma.CHECK_CRC64,
                             filters=xz_filters(len(data), 2))
    if codec == "xz9e-j0":
        return lzma.compress(data, format=lzma.FORMAT_XZ,
                             check=lzma.CHECK_CRC64,
                             filters=xz_filters(len(data), 0))
    raise NvzError("nvzpy never writes codec %r" % codec)


def decompress(codec, data):
    if codec == "ra":
        return data
    if codec.startswith("xz9e"):
        return lzma.decompress(data)      # the stream carries its own filters
    if codec.startswith("zstd"):
        try:
            import zstandard
        except ImportError:
            raise NvzError(
                "this archive has a %s block; install the optional package "
                "(`pip install zstandard`) or repack it on Linux with "
                "`nvz pack --portable`" % codec)
        return zstandard.ZstdDecompressor(max_window_size=1 << 31).decompress(data)
    if codec in ("nvcm", "nvz", "nvlz"):
        raise NvzError(
            "this archive has a %s block, which only the Linux nvz can "
            "decode; repack it there with `nvz pack --portable`" % codec)
    raise NvzError("unknown codec: %s" % codec)


# --- the register, v2/v3 binary -----------------------------------------------

class Reader:
    def __init__(self, data):
        self.d, self.i = data, 0

    def take(self, n):
        if self.i + n > len(self.d):
            raise NvzError("the register is truncated")
        out = self.d[self.i:self.i + n]
        self.i += n
        return out

    def u8(self):  return self.take(1)[0]
    def u32(self): return struct.unpack("<I", self.take(4))[0]
    def u64(self): return struct.unpack("<Q", self.take(8))[0]
    def i64(self): return struct.unpack("<q", self.take(8))[0]
    def s(self):   return self.take(self.u32()).decode("utf-8")


class Writer:
    def __init__(self):
        self.b = bytearray()

    def u8(self, v):  self.b.append(v)
    def u32(self, v): self.b += struct.pack("<I", v)
    def u64(self, v): self.b += struct.pack("<Q", v)
    def i64(self, v): self.b += struct.pack("<q", v)
    def raw(self, v): self.b += v

    def s(self, v):
        e = v.encode("utf-8")
        self.u32(len(e))
        self.raw(e)


def read_register(path):
    size = os.path.getsize(path)
    if size < len(MAGIC) + 16:
        raise NvzError("not an nvz archive")
    with open(path, "rb") as f:
        if f.read(len(MAGIC)) != MAGIC:
            raise NvzError("not an nvz archive")
        f.seek(-16, io.SEEK_END)
        reg_off, raw_len = struct.unpack("<QQ", f.read(16))
        if reg_off + 16 > size:
            raise NvzError("the register points outside the archive")
        f.seek(reg_off)
        packed = f.read(size - 16 - reg_off)
    if packed[:6] == b"\xfd7zXZ\x00":
        raw = lzma.decompress(packed)
    else:
        raw = decompress("zstd19", packed)      # zstd magic or bust
    if raw[:1] == b"{":
        raise NvzError("v1 (Python/JSON) archives are read by the Linux nvz, "
                       "not by nvzpy")
    if raw[:8] != REG2:
        raise NvzError("unknown register format")
    r = Reader(raw[8:])
    reg = {"version": r.u32(), "root": r.s(), "flat": r.u8() != 0}
    if reg["version"] >= 3:
        reg["created"] = time.strftime("%Y-%m-%dT%H:%M:%S",
                                       time.localtime(r.i64()))
    else:
        reg["created"] = r.s()
    blocks = []
    for _ in range(r.u32()):
        blocks.append({"index": r.u32(), "cls": r.s(), "codec": r.s(),
                       "off": r.u64(), "clen": r.u64(), "raw_len": r.u64()})
    reg["blocks"] = blocks
    nobj = r.u32()
    hash_len = 4 if reg["version"] >= 3 else 16
    reg["hash_len"] = hash_len
    objects = [{"hash": r.take(hash_len), "size": r.u64()} for _ in range(nobj)]
    for ob in objects:
        ob["blk"], ob["off"], ob["len"] = r.u32(), r.u64(), r.u64()
        ob["transform"] = ""
    for _ in range(r.u32()):
        ob = objects[r.u32()]
        ob["transform"], ob["recipe"] = r.s(), r.s()
        ob["rblk"], ob["roff"], ob["rlen"] = r.u32(), r.u64(), r.u64()
    reg["objects"] = objects
    entries = []
    for _ in range(r.u32()):
        e = {"path": r.s(), "kind": chr(r.u8()), "mode": r.u32(),
             "mtime_ns": r.i64(), "target": "", "obj": -1}
        if e["kind"] == "l":
            e["target"] = r.s()
        if e["kind"] == "f":
            e["obj"] = r.i64()
        entries.append(e)
    reg["entries"] = entries
    return reg


def write_register(reg):
    w = Writer()
    w.raw(REG2)
    w.u32(3)
    w.s(reg["root"])
    w.u8(1 if reg["flat"] else 0)
    w.i64(int(time.time()))
    w.u32(len(reg["blocks"]))
    for b in reg["blocks"]:
        w.u32(b["index"]); w.s(b["cls"]); w.s(b["codec"])
        w.u64(b["off"]); w.u64(b["clen"]); w.u64(b["raw_len"])
    w.u32(len(reg["objects"]))
    for ob in reg["objects"]:
        w.raw(ob["hash"][:4]); w.u64(ob["size"])
    for ob in reg["objects"]:
        w.u32(ob["blk"]); w.u64(ob["off"]); w.u64(ob["len"])
    w.u32(0)                      # no transformed objects, ever, in nvzpy
    w.u32(len(reg["entries"]))
    for e in reg["entries"]:
        w.s(e["path"]); w.u8(ord(e["kind"]))
        w.u32(e["mode"]); w.i64(e["mtime_ns"])
        if e["kind"] == "l":
            w.s(e["target"])
        if e["kind"] == "f":
            w.i64(e["obj"])
    return bytes(w.b)


# --- the tree -----------------------------------------------------------------

def classify(path, head):
    if head[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if head[:3] == b"\xff\xd8\xff":
        return "jpeg"
    ext = os.path.splitext(path)[1].lower()
    if ext in OPAQUE_EXTS:
        return "opak"
    if ext in TEXT_EXTS:
        return "text"
    probe = head[:4096]
    odd = sum(1 for b in probe if b < 9 or 13 < b < 32)
    if probe and odd * 20 < len(probe):
        return "text"
    return "binar"


def scan_tree(root):
    """Breadth first, names sorted — the same order every time gives the
    same archive every time, like the C++ scan."""
    entries, skipped = [], 0
    queue = [""]
    while queue:
        rel = queue.pop(0)
        full = os.path.join(root, rel.replace("/", os.sep)) if rel else root
        st = os.lstat(full)
        entries.append({"path": rel, "kind": "d", "mode": st.st_mode & 0o7777,
                        "mtime_ns": st.st_mtime_ns, "target": "", "obj": -1})
        subdirs = []
        for name in sorted(os.listdir(full)):
            r = rel + "/" + name if rel else name
            f = os.path.join(full, name)
            try:
                st = os.lstat(f)
            except OSError:
                continue
            if stat_mod.S_ISLNK(st.st_mode):
                try:
                    target = os.readlink(f)
                except OSError:
                    skipped += 1
                    continue
                entries.append({"path": r, "kind": "l", "mode": 0o777,
                                "mtime_ns": st.st_mtime_ns,
                                "target": target.replace(os.sep, "/"),
                                "obj": -1})
            elif stat_mod.S_ISDIR(st.st_mode):
                subdirs.append(r)
            elif stat_mod.S_ISREG(st.st_mode):
                entries.append({"path": r, "kind": "f",
                                "mode": st.st_mode & 0o7777,
                                "mtime_ns": st.st_mtime_ns, "target": "",
                                "obj": -1, "size": st.st_size})
            else:
                skipped += 1
        queue[0:0] = subdirs
    return entries, skipped


def set_metadata(path, mode, mtime_ns, is_link):
    if is_link:
        try:
            os.utime(path, ns=(mtime_ns, mtime_ns), follow_symlinks=False)
        except (NotImplementedError, OSError):
            pass
        return
    try:
        os.chmod(path, mode)
    except OSError:
        pass                      # permissions are a wish, notably on Windows
    os.utime(path, ns=(mtime_ns, mtime_ns))


# --- pack ---------------------------------------------------------------------

def choose_codec(block_data):
    """ra / xz9e / xz9e-j0 — the portable candidate set. The probe is the
    same idea as the C++ one: eight spots spread through the block, and pb
    settled on a small contiguous sample."""
    n = len(block_data)
    if n == 0:
        return "ra"
    if n <= PROBE:
        sample = block_data
    else:
        part = PROBE // 8
        sample = b"".join(
            block_data[i * (n - part) // 7:i * (n - part) // 7 + part]
            for i in range(8))
    small = sample[:512 << 10]
    pb2 = len(lzma.compress(small, filters=xz_filters(len(small), 2),
                            format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64))
    pb0 = len(lzma.compress(small, filters=xz_filters(len(small), 0),
                            format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64))
    codec = "xz9e-j0" if pb0 < pb2 else "xz9e"
    probe_out = len(compress(codec, sample))
    if probe_out > len(sample) * 0.998:
        return "ra"
    return codec


def pack(root, archive, quiet=False, flat=False):
    root = os.path.abspath(root)
    say = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    entries, skipped = scan_tree(root)
    if skipped:
        print("  warning: %d special files or unreadable links skipped"
              % skipped, file=sys.stderr)
    files = [e for e in entries if e["kind"] == "f"]
    raw_bytes = sum(e["size"] for e in files)
    say("  %d entries, %.1f MiB (nvzpy, portable codecs only)"
        % (len(entries), raw_bytes / 1048576))

    # Hash, classify, dedup.
    objects, seen = [], {}
    for e in files:
        data = open(os.path.join(root, e["path"].replace("/", os.sep)), "rb").read()
        h = blake16(data)
        if h in seen:
            e["obj"] = seen[h]
            continue
        ob = {"hash": h, "size": len(data), "cls": classify(e["path"], data),
              "ext": os.path.splitext(e["path"])[1].lower(),
              "source": e["path"]}
        ob["id"] = len(objects)
        seen[h] = ob["id"]
        objects.append(ob)
        e["obj"] = ob["id"]
    dups = len(files) - len(objects)
    if dups:
        say("  dedup: %d duplicates" % dups)

    # Sort: class, extension, large-first (an eighth of the cap), path.
    big = BLOCK_CAP // 8
    order = sorted(range(len(objects)), key=lambda i: (
        objects[i]["cls"], objects[i]["ext"],
        0 if objects[i]["size"] >= big else 1,
        -objects[i]["size"] if objects[i]["size"] >= big else 0,
        objects[i]["source"]))
    renum = {old: new for new, old in enumerate(order)}
    objects = [objects[i] for i in order]
    for e in files:
        e["obj"] = renum[e["obj"]]

    # Blocks.
    blocks, open_blocks = [], {}
    for ob in objects:
        cls = ob["cls"]
        cap = OPAQUE_CAP if cls == "opak" else BLOCK_CAP
        bi = open_blocks.get(cls)
        if bi is None or blocks[bi]["raw_len"] >= cap:
            bi = len(blocks)
            blocks.append({"index": bi, "cls": cls, "codec": "",
                           "raw_len": 0, "parts": []})
            open_blocks[cls] = bi
        b = blocks[bi]
        data = open(os.path.join(root, ob["source"].replace("/", os.sep)), "rb").read()
        if len(data) != ob["size"]:
            raise NvzError(ob["source"] + ": file changed during packing")
        ob["blk"], ob["off"], ob["len"] = bi, b["raw_len"], len(data)
        b["parts"].append(data)
        b["raw_len"] += len(data)
    say("  %d blocks" % len(blocks))

    # Compress and write.
    tmp = archive + ".part"
    try:
        with open(tmp, "wb") as out:
            out.write(MAGIC)
            pos = len(MAGIC)
            for b in blocks:
                raw = b"".join(b["parts"])
                b["parts"] = None
                b["codec"] = choose_codec(raw)
                packed = compress(b["codec"], raw)
                say("  block %d %s %s: %.2f -> %.2f MiB"
                    % (b["index"], b["cls"], b["codec"],
                       len(raw) / 1048576, len(packed) / 1048576))
                b["off"], b["clen"] = pos, len(packed)
                out.write(packed)
                pos += len(packed)
            reg = {"root": os.path.basename(root), "flat": flat,
                   "blocks": blocks, "objects": objects, "entries": entries}
            raw_reg = write_register(reg)
            out.write(compress("xz9e", raw_reg))
            out.write(struct.pack("<QQ", pos, len(raw_reg)))
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, archive)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise
    say("  done: %.2f MiB" % (os.path.getsize(archive) / 1048576))


# --- unpack -------------------------------------------------------------------

def unpack(archive, target, quiet=False):
    say = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    reg = read_register(archive)
    say("  %d entries, %d blocks" % (len(reg["entries"]), len(reg["blocks"])))
    for ob in reg["objects"]:
        if ob["transform"]:
            raise NvzError(
                "the archive holds PNG/JPEG-transformed objects, which only "
                "the Linux nvz can rebuild; repack with `nvz pack --portable`")
    with open(archive, "rb") as f:
        block_data = []
        for b in reg["blocks"]:
            f.seek(b["off"])
            data = decompress(b["codec"], f.read(b["clen"]))
            if len(data) != b["raw_len"]:
                raise NvzError("block %d: wrong length after decompression"
                               % b["index"])
            block_data.append(data)

    root_out = target if reg["flat"] else os.path.join(target, reg["root"])
    os.makedirs(target, exist_ok=True)
    status = 0
    for e in reg["entries"]:
        if e["kind"] != "d":
            continue
        if reg["flat"] and not e["path"]:
            continue
        os.makedirs(os.path.join(root_out, e["path"].replace("/", os.sep)) if e["path"]
                    else root_out, exist_ok=True)
    for e in reg["entries"]:
        if e["kind"] == "d":
            continue
        path = os.path.join(root_out, e["path"].replace("/", os.sep))
        if e["kind"] == "l":
            try:
                if os.path.lexists(path):
                    os.unlink(path)
                os.symlink(e["target"].replace("/", os.sep), path)
                set_metadata(path, 0, e["mtime_ns"], True)
            except OSError as ex:
                # Windows without developer mode lands here. A skipped link
                # with a warning beats a crashed unpack; status 2 says it.
                print("nvzpy: %s: could not create symlink (%s) -- skipped"
                      % (path, ex), file=sys.stderr)
                status = max(status, 2)
            continue
        ob = reg["objects"][e["obj"]]
        data = block_data[ob["blk"]][ob["off"]:ob["off"] + ob["len"]]
        if blake16(data)[:reg["hash_len"]] != ob["hash"][:reg["hash_len"]]:
            raise NvzError(path + ": content does not match the register")
        with open(path, "wb") as f:
            f.write(data)
        set_metadata(path, e["mode"], e["mtime_ns"], False)
    for e in reversed(reg["entries"]):
        if e["kind"] != "d":
            continue
        if reg["flat"] and not e["path"]:
            continue
        p = os.path.join(root_out, e["path"].replace("/", os.sep)) if e["path"] else root_out
        set_metadata(p, e["mode"], e["mtime_ns"], False)
    say("  done")
    return status


def list_archive(archive):
    reg = read_register(archive)
    print("nvz v%d  root=%s  created %s" % (reg["version"], reg["root"], reg["created"]))
    print("%d entries, %d objects, %d blocks"
          % (len(reg["entries"]), len(reg["objects"]), len(reg["blocks"])))
    for b in reg["blocks"]:
        ratio = b["raw_len"] / b["clen"] if b["clen"] else 0.0
        print("  block %3d  %-11s %-7s %9.2f -> %8.2f MiB  x%.2f"
              % (b["index"], b["cls"], b["codec"],
                 b["raw_len"] / 1048576, b["clen"] / 1048576, ratio))


def test_archive(archive):
    """Unpack to scratch and let the per-file hash check judge."""
    import shutil
    import tempfile
    d = tempfile.mkdtemp(prefix="nvzpy-test-")
    try:
        unpack(archive, d, quiet=True)
        print("%s: OK" % archive)
        return 0
    finally:
        shutil.rmtree(d, ignore_errors=True)


def main(argv):
    if len(argv) < 2 or argv[1] in ("-h", "--help"):
        print(__doc__.strip().split("\n\n")[0])
        return 0 if len(argv) >= 2 else 1
    cmd, args = argv[1], argv[2:]
    quiet = "-q" in args
    args = [a for a in args if a != "-q"]
    try:
        if cmd in ("pack", "packa") and len(args) == 2:
            pack(args[0], args[1], quiet=quiet)
            return 0
        if cmd in ("unpack", "packa-upp") and len(args) == 2:
            return unpack(args[0], args[1], quiet=quiet)
        if cmd in ("list", "lista") and len(args) == 1:
            list_archive(args[0])
            return 0
        if cmd in ("test", "prova") and len(args) == 1:
            return test_archive(args[0])
    except NvzError as e:
        print("nvzpy: %s" % e, file=sys.stderr)
        return 1
    print(__doc__.strip().split("\n\n")[0])
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
