#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""RETIRED. This is the original Python implementation of the tool
(named nvz then, hz since 2026-11-19), kept as the v1 archive-format
reference: the C++ implementation reads its archives bit for bit, and the
test suite holds the two to the same bytes. It is built by nothing and
called by nothing. The file was born Swedish; it was translated to English
on 2026-11-20 — comments, identifiers and messages only, with every format
byte (register keys, block class names, codec names, magic, suffix) left
untouched, because those bytes ARE the v1 format.

nvz — a content-aware archiver.

The command line is gzip's — same flags, same default behavior:

    nvz file                     # -> file.nv, original gone
    nvz -d file.nv               # -> file, archive gone
    nvz -c file > out.nv         # to stdout
    nvz -l ark.nv                # compressed, uncompressed, ratio
    nvz -t ark.nv                # unpack and compare against the register
    nvz -k -9 directory          # the whole tree to directory.nv

A DIRECTORY as operand is packed into ONE archive of the tree. That is the
only place the surface differs from gzip, and it is deliberate: a directory
is what nvz is built for. The source tree is always left in place. With -r
gzip's literal thing is done instead — one file at a time — and then the
gain BETWEEN files is lost.

The subcommands remain; benchmark/ and tests/ called them:

    nvz pack <directory> <archive.nv>
    nvz unpack <archive.nv> <destdir>
    nvz list <archive.nv>
    nvz test <directory>         # pack, unpack, compare bit for bit

WHY IT EXISTS. `tar | zstd` is two programs that know nothing about each
other. tar lays the files down in whatever order the directory tree happens
to have them and passes on a single byte stream; zstd sees that stream
through a window and never learns that file 4000 is an exact copy of file
12, or that the 9 MB of "noise" it just gave up on is in fact a deflate
stream that can be unfolded.

nvz knows five things tar does not:

  1. IDENTICAL FILES ARE STORED ONCE. Content is hashed; duplicates become
     pointers. tar stores every copy, and zstd's window only finds the
     copies that happen to sit near each other in the stream.

  2. SIMILAR FILES ARE PLACED NEXT TO EACH OTHER. Entries are sorted by
     class and suffix before packing, so all the JavaScript sits in one run
     and all the screenshots in another. Compression then gets to aim at
     similarities that in directory order lay hundreds of megabytes apart.

  3. PNG IS UNFOLDED AND REPACKED. A PNG is already deflate-packed, and
     deflate from 1996 is a bad shell to keep around your image data. nvz
     unfolds the IDAT stream into raw filter rows and saves the RECIPE that
     lets exactly the same bytes be deflated back. See png.py.

  4. JPEG IS RECODED WITHOUT TOUCHING A PIXEL. The Huffman layer is swapped
     for an adaptive arithmetic coder with models for how coefficients
     actually look next to each other. The image is the same, byte for
     byte, when unpacked. See nvjpg.c.

  5. ALREADY-PACKED DATA IS LEFT IN PEACE. Video, audio and zip archives
     are not pushed through an expensive compressor that finds nothing
     anyway. No saving in bytes, but a large saving in time — and time is
     why today's archives run zstd -3 instead of -19.

EVERY TRANSFORM IS PROVEN AT PACK TIME. Both the PNG and the JPEG path
stitch their result back together and compare with the original before it
is accepted. If it does not match, the file is stored raw. An archiver may
have transforms it cannot handle; it may never BELIEVE it handled one.

THE FORMAT. The archive is blocks + register.

    "NVPACK\\x01" | block 0 | block 1 | ... | register (zstd) | <u64 offset, u64 length>

Each block is an independently packed run of object bodies, with its own
codec. The independence is deliberate: blocks are packed in parallel on all
cores, and an unpack that only wants one file only has to touch one block.
"""
from __future__ import annotations

import argparse, concurrent.futures as fut, hashlib, json, os, shutil, stat, struct
import subprocess, sys, tempfile, threading, time
from pathlib import Path

# png.py and jpeg.py expose unfold/rebuild (born as veckla_ut/återskapa in
# the Swedish v1 sources); the archive bytes they produce are unchanged.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import png as png_tr
import jpeg as jpeg_tr

MAGIC = b'NVPACK\x01'
VERSION = 1
BLOCK_CAP = 192 * 1024 * 1024     # raw size per block before a new one is begun
PROBE_BYTES = 4 * 1024 * 1024     # how much of a block the codec choice probes on

# How many times larger than its IDAT stream a PNG may become before the
# unfold is declined. The cap is NOT a profitability bound —
# experiment/png-threshold.py measured the matter and disproved that guess:
# the unfold won in every class, and most of all (−40 %) for images blown
# up more than 32 times. It is logical in hindsight. An image deflate has
# already squeezed 50:1 is a screenshot with large flat areas, and when
# fifty of THOSE lie unfolded in the same block a modern packer finds the
# similarities BETWEEN the images — something deflate could never see
# through its 32 kB window, one file at a time.
#
# The cap therefore exists only as a guard rail against unreasonable
# resource use: a single 50 MB screenshot must not become several GB of
# working data.
PNG_INFLATION_CAP = 150


# ---------------------------------------------------------------------------
# Classification
# ---------------------------------------------------------------------------
# A suffix is a guess, magic bytes are an answer. The class decides three
# things: which transform is tried, which block the entry lands in, and
# which codec the block gets.

TEXT_SUFFIXES = {'.txt','.md','.json','.jsonl','.js','.mjs','.cjs','.ts','.tsx','.jsx','.py','.rs',
 '.go','.c','.h','.cpp','.hpp','.java','.sh','.bash','.zsh','.sql','.css','.scss','.html','.htm',
 '.xml','.svg','.yml','.yaml','.toml','.ini','.cfg','.conf','.service','.log','.csv','.tsv',
 '.po','.pot','.map','.lock','.patch','.diff','.rst','.tex','.webmanifest'}

OPAQUE_SUFFIXES = {'.mp4','.m4a','.m4v','.mov','.mkv','.webm','.mp3','.ogg','.opus','.flac',
 '.webp','.avif','.heic','.gif','.zip','.gz','.tgz','.bz2','.xz','.zst','.7z','.rar','.br',
 '.woff','.woff2','.pdf','.jxl','.wasm','.deb','.rpm','.whl','.jar','.apk','.odt','.ods',
 '.docx','.xlsx','.pptx'}


def classify(path: Path, head: bytes) -> str:
    if head[:8] == b'\x89PNG\r\n\x1a\n': return 'png'
    if head[:3] == b'\xff\xd8\xff':      return 'jpeg'
    suffix = path.suffix.lower()
    if suffix in OPAQUE_SUFFIXES: return 'opak'
    if suffix in TEXT_SUFFIXES: return 'text'
    # Unknown suffix: fewer than one byte in twenty outside the readable
    # range is enough to call something text. Text wins by sitting with text.
    sample = head[:4096]
    if sample and sum(b < 9 or (13 < b < 32) for b in sample) * 20 < len(sample):
        return 'text'
    return 'binar'


# Codec per class and mode. 'prova' (Swedish for probe, a v1 string) means
# the block itself gets to decide by packing a sample with every candidate.
#
#   fast    — like today's archive scripts: zstd -3, no transforms. Included
#             to be able to measure against it, and for archives that must
#             be quick.
#   normal  — transforms on, zstd -19 or xz depending on which wins.
#   small   — text goes through nvcm. It packs a couple of MB/s, so it only
#             gets the blocks where it earns it: text, source code, PNG
#             skeletons and JPEG headers. Unfolded image data is too
#             voluminous for it.
#
# Note that not even video and zip archives are listed as 'ra' but as
# 'prova'. Assuming that "already packed" cannot be packed further cost
# 150 MiB in the first measurement of the PNG corpus: the screenshots that
# could NOT be unfolded were stored raw, and nvz then missed that four
# thousand screenshots of the same web page resemble EACH OTHER, even when
# their deflate streams are impenetrable one by one. The probe costs a
# couple of seconds per block and answers 'ra' on its own when the
# assumption holds. The only exception is jpeg-kropp, the jpeg body class:
# that is our own arithmetic stream, and it IS noise.
MODES = {
 'fast':   {'text':'zstd3','binar':'zstd3','png-ra':'zstd3','png-rest':'zstd3',
            'jpeg-rest':'zstd3','jpeg-kropp':'ra','jpeg':'ra','opak':'ra','png':'zstd3'},
 'normal': {'text':'prova','binar':'prova','png-ra':'prova','png-rest':'zstd19',
            'jpeg-rest':'zstd19','jpeg-kropp':'ra','jpeg':'prova','opak':'prova','png':'prova'},
 'small':  {'text':'prova-cm','binar':'prova-cm','png-ra':'prova','png-rest':'prova-cm',
            'jpeg-rest':'prova-cm','jpeg-kropp':'ra','jpeg':'prova','opak':'prova','png':'prova'},
 # gpu — like normal, but nvlz also gets to compete for every block. nvlz is
 # nvpack's own codec where the match search runs on the graphics card.
 # READ THE MEASUREMENT IN THE README BEFORE USING IT: on this machine it
 # wins no block, and the mode remains so the measurement can be redone.
 'gpu':    {'text':'prova-gpu','binar':'prova-gpu','png-ra':'prova-gpu','png-rest':'prova-gpu',
            'jpeg-rest':'prova-gpu','jpeg-kropp':'ra','jpeg':'prova','opak':'prova','png':'prova-gpu'},
}
def helper_binary(name: str) -> str:
    """Find nvcm/nvjpg/nvgpu next to the script, else in PATH.

    Next to it first, so a checked-out tree uses its own builds and not
    something installed. PATH as fallback, so nvz can run as ONE file
    (zipapp) where there is no directory to sit next to."""
    sibling = Path(__file__).resolve().parent / name
    if sibling.is_file() and os.access(sibling, os.X_OK):
        return str(sibling)
    return shutil.which(name) or str(sibling)


NVCM = helper_binary('nvcm')
NVGPU = helper_binary('nvgpu')

# nvcm takes half a gigabyte of model tables per block it packs. Blocks run
# in parallel, and without this gate twenty-four of them can start at once
# and take the machine's memory with them.
NVCM_GATE = threading.Semaphore(4)

# There is ONE graphics chip. Two simultaneous nvlz packs do not share it,
# they queue at the driver — and every extra concurrent kernel raises the
# risk that the watchdog kills the whole context. Two is enough to hide the
# gather time behind the kernel time.
NVLZ_GATE = threading.Semaphore(2)


def gpu_available() -> bool:
    """Can nvlz pack here? The answer is cached: it costs a process start
    to answer, and the question is asked once per block."""
    if gpu_available.answer is None:
        try:
            r = subprocess.run([NVGPU, 'device', '--quiet'], capture_output=True, timeout=30)
            gpu_available.answer = r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            gpu_available.answer = False
    return gpu_available.answer


gpu_available.answer = None


# ---------------------------------------------------------------------------
# Codecs
# ---------------------------------------------------------------------------

def _run(args, stdin=None):
    r = subprocess.run(args, input=stdin, capture_output=True)
    if r.returncode: raise RuntimeError(f'{args[0]}: {r.stderr.decode()[:400]}')
    return r.stdout


def pack_block(raw: Path, out: Path, codec: str, threads: int) -> str:
    """Packs a block's raw data. Returns the codec actually used."""
    if codec in ('prova', 'prova-cm', 'prova-gpu'):
        codec = choose_codec(raw, with_cm=(codec == 'prova-cm'),
                             with_gpu=(codec == 'prova-gpu'))
    if codec == 'ra':
        os.replace(raw, out)
    elif codec == 'zstd3':
        _run(['zstd','-3',f'-T{threads}','-q','-f','-o',str(out),str(raw)])
    elif codec == 'nvcm':
        with NVCM_GATE, open(raw,'rb') as i, open(out,'wb') as u:
            r = subprocess.run([NVCM,'pack'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvcm: '+r.stderr.decode()[:300])
    elif codec == 'nvlz':
        with NVLZ_GATE, open(raw,'rb') as i, open(out,'wb') as u:
            r = subprocess.run([NVGPU,'pack','--quiet'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvgpu: '+r.stderr.decode()[:300])
    elif codec == 'zstd19':
        _run(['zstd','-19','--long=27',f'-T{threads}','-q','-f','-o',str(out),str(raw)])
    elif codec == 'zstd22':
        _run(['zstd','-22','--ultra','--long=31',f'-T{threads}','-q','-f','-o',str(out),str(raw)])
    elif codec == 'xz9e':
        with open(out,'wb') as f:
            r = subprocess.run(['xz','-9e',f'-T{threads}','-c',str(raw)], stdout=f, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('xz: '+r.stderr.decode()[:400])
    else:
        raise ValueError(codec)
    return codec


def unpack_block(packed: Path, out: Path, codec: str):
    if codec == 'ra':
        os.replace(packed, out)
    elif codec in ('nvcm', 'nvz'):      # 'nvz' = archives from the hours when the codec bore that name
        with NVCM_GATE, open(packed,'rb') as i, open(out,'wb') as u:
            r = subprocess.run([NVCM,'unpack'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvcm: '+r.stderr.decode()[:300])
    elif codec == 'nvlz':
        # The unpack is pure cpu. An archive must not be hostage to a
        # graphics driver: whoever packed had a gpu, whoever unpacks may
        # have none.
        with open(packed,'rb') as i, open(out,'wb') as u:
            r = subprocess.run([NVGPU,'unpack'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvgpu: '+r.stderr.decode()[:300])
    elif codec.startswith('zstd'):
        window = '--long=31' if codec == 'zstd22' else '--long=27'
        _run(['zstd','-d',window,'-q','-f','-o',str(out),str(packed)])
    elif codec == 'xz9e':
        with open(out,'wb') as f:
            r = subprocess.run(['xz','-dc','-T0',str(packed)], stdout=f, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('xz: '+r.stderr.decode()[:400])
    else:
        raise ValueError(codec)


def probe_size(probe: Path) -> int:
    return probe.stat().st_size


def choose_codec(raw: Path, with_cm=False, with_gpu=False) -> str:
    """Tries a sample with every candidate and picks the winner.

    The sample is taken at EIGHT places spread through the block, not as the
    first megabytes. A block is sorted by suffix, so the first megabytes are
    a single file type and say nothing about the rest — the first
    measurement of this choice let zstd beat xz on a block where xz was 4 %
    better.
    """
    size = raw.stat().st_size
    if size == 0: return 'ra'
    with tempfile.TemporaryDirectory() as d:
        probe = Path(d)/'probe'
        parts, part_size = 8, PROBE_BYTES // 8
        with open(raw,'rb') as f, open(probe,'wb') as u:
            if size <= PROBE_BYTES:
                u.write(f.read())
            else:
                for i in range(parts):
                    f.seek(i * (size - part_size) // (parts - 1))
                    u.write(f.read(part_size))
        z, x = Path(d)/'z', Path(d)/'x'
        _run(['zstd','-19','--long=27','-T1','-q','-f','-o',str(z),str(probe)])
        zb = z.stat().st_size
        with open(x,'wb') as f:
            subprocess.run(['xz','-9e','-T1','-c',str(probe)], stdout=f, stderr=subprocess.DEVNULL)
        xb = x.stat().st_size or zb
        cb = None
        if with_cm:
            with open(probe,'rb') as f:
                r = subprocess.run([NVCM,'pack'], stdin=f, capture_output=True)
            if r.returncode == 0: cb = len(r.stdout)
        gb = None
        if with_gpu and gpu_available():
            with NVLZ_GATE, open(probe,'rb') as f:
                r = subprocess.run([NVGPU,'pack','--quiet'], stdin=f, capture_output=True)
            if r.returncode == 0: gb = len(r.stdout)
        best = min(x for x in (zb, xb, cb, gb) if x is not None)

        # Three outcomes, not two. Only distinguishing "compressible" from
        # "not" cost 0.06 % on a real archive: 2.9 GB of video packs half a
        # percent, which is under the threshold for being worth an expensive
        # codec — but half a percent of 2.9 GB is fourteen megabytes, and a
        # cheap codec takes them in a few seconds. Today's `tar | zstd -3`
        # took them; nvz threw them away.
        if best > probe_size(probe) * 0.998:
            return 'ra'          # under a fifth of a percent: not worth a single second
        if best > probe_size(probe) * 0.98:
            return 'zstd3'       # meager but almost free to take
        if cb is not None and cb < min(zb, xb) * 0.98:
            return 'nvcm'
        # nvlz needs a margin for the same reason as nvcm: it is more
        # expensive than zstd and should only be chosen when it brings home
        # something real.
        if gb is not None and gb < min(zb, xb) * 0.98:
            return 'nvlz'
        # xz and zstd -19 take about the same time on this machine, so the
        # margin only has to cover the sample's noise.
        return 'xz9e' if xb < zb * 0.995 else 'zstd19'


# ---------------------------------------------------------------------------
# Transforms (run in worker processes)
# ---------------------------------------------------------------------------

def _transform(task):
    """(class, path) -> (transform, {'rest','body',...}) or (None, None)."""
    cls, path = task
    try:
        if cls == 'png':
            r = png_tr.unfold(path, PNG_INFLATION_CAP)
            return ('png', r) if r else (None, None)
        if cls == 'jpeg':
            r = jpeg_tr.unfold(path)
            return ('jpeg', r) if r else (None, None)
    except Exception:
        return (None, None)           # a broken file must be stored, not fell the archive
    return (None, None)


# ---------------------------------------------------------------------------
# Packing
# ---------------------------------------------------------------------------

def hash_file(path: Path, chunk=1 << 20):
    """Returns (hash, first kilobytes). The head is used for classification,
    so the file only has to be read once."""
    h = hashlib.blake2b(digest_size=16)
    head = b''
    with open(path,'rb') as f:
        while d := f.read(chunk):
            if not head: head = d[:4096]
            h.update(d)
    return h.hexdigest(), head


def read_tree(root: Path):
    """Walks the tree once and extracts entries in stable order.

    Two traps sit here, and the test suite hit both:

    A SYMLINK TO A DIRECTORY shows up among os.walk's directories, not
    among its files. Look for links only among the files and it vanishes
    from the archive entirely — silently, without error, and with nothing
    missing until someone tries to use what was unpacked.

    THE DIRECTORIES' OWN TIME is data too. Without it every directory gets
    the unpack day's date, and an archive that moves all timestamps to
    today is not a faithful copy.
    """
    entries = []
    skipped = 0
    for dirpath, dirs, files in os.walk(root, followlinks=False):
        dirs.sort(); files.sort()
        k = Path(dirpath)
        st = k.lstat()
        entries.append(dict(p=str(k.relative_to(root)) if k != root else '', t='d',
                            m=st.st_mode & 0o7777, mt=st.st_mtime_ns))
        linked = [d for d in dirs if (k/d).is_symlink()]
        for d in linked:
            entries.append(dict(p=str((k/d).relative_to(root)), t='l', mal=os.readlink(k/d),
                                mt=(k/d).lstat().st_mtime_ns))
        if linked:
            dirs[:] = [d for d in dirs if d not in set(linked)]
        for name in files:
            s = k/name
            st = s.lstat()
            rel = str(s.relative_to(root))
            if stat.S_ISLNK(st.st_mode):
                entries.append(dict(p=rel, t='l', mal=os.readlink(s), mt=st.st_mtime_ns))
            elif stat.S_ISREG(st.st_mode):
                entries.append(dict(p=rel, t='f', m=st.st_mode & 0o7777,
                                    mt=st.st_mtime_ns, s=st.st_size))
            else:
                skipped += 1           # pipes, sockets, devices — nothing a file copy owns
    if skipped:
        print(f'  warning: {skipped} special files (pipes/sockets/devices) were skipped',
              file=sys.stderr)
    return entries


def pack(root: Path, archive: Path, *, mode='small', dedup=True, transform=None, jobs=None,
         quiet=False, codec=None, flat=False):
    t0 = time.monotonic()
    jobs = jobs or min(24, os.cpu_count() or 4)
    codec_table = MODES[mode]
    if transform is None: transform = (mode != 'fast')
    root = root.resolve()
    say = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))

    entries = read_tree(root)
    files = [p for p in entries if p['t'] == 'f']
    raw_bytes = sum(p['s'] for p in files)
    say(f'  {len(entries)} entries, {raw_bytes/1048576:.1f} MiB, mode {mode}')

    # --- 1. Hash and classify ------------------------------------------------
    # Hashing is I/O-bound and blake2b releases the GIL, so threads suffice.
    with fut.ThreadPoolExecutor(jobs) as pool:
        results = list(pool.map(lambda p: hash_file(root/p['p']), files))
    for p, (h, head) in zip(files, results):
        p['h'] = h
        p['cls'] = classify(root/p['p'], head)

    # --- 2. Dedup -------------------------------------------------------------
    objects = {}
    for p in files:
        key = p['h'] if dedup else f'{p["h"]}:{p["p"]}'
        o = objects.get(key)
        if o is None:
            o = objects[key] = dict(h=p['h'], s=p['s'], cls=p['cls'], source=p['p'],
                                    suffix=Path(p['p']).suffix.lower(), id=len(objects))
        p['o'] = o['id']
    saved = raw_bytes - sum(o['s'] for o in objects.values())
    if dedup and saved:
        say(f'  dedup: {len(files)-len(objects)} duplicates, {saved/1048576:.1f} MiB')

    workdir = Path(tempfile.mkdtemp(prefix='nvz-', dir=archive.parent))
    try:
        # --- 3. Sort ----------------------------------------------------------
        # The sort is the whole point of item 2 in the module head: class
        # first, then suffix, then path. Files that resemble each other land
        # after each other in the stream, and the transformed follow their
        # class.
        ordered = sorted(objects.values(), key=lambda o: (o['cls'], o['suffix'], o['source']))
        # Renumber the objects after the sort. The register stores them as
        # parallel lists, and then an object's number MUST be its place in
        # the list — otherwise every entry points at the wrong object. (The
        # first version did not, and gave an archive where every file got
        # someone else's content. The bug is invisible at pack time, only at
        # unpack.)
        renumbered = {o['id']: i for i, o in enumerate(ordered)}
        for o in ordered: o['id'] = renumbered[o['id']]
        for p in files: p['o'] = renumbered[p['o']]

        blocks = []
        open_blocks = {}

        def new_block(cls):
            b = dict(i=len(blocks), cls=cls, codec=codec_table.get(cls,'prova'),
                     stream=workdir/f'b{len(blocks)}', raw_len=0)
            b['f'] = open(b['stream'],'wb')
            blocks.append(b)
            return b

        def add(cls, data: bytes):
            b = open_blocks.get(cls)
            if b is None or b['raw_len'] >= BLOCK_CAP:
                if b is not None: b['f'].close(); b.pop('f')
                b = open_blocks[cls] = new_block(cls)
            off = b['raw_len']; b['f'].write(data); b['raw_len'] += len(data)
            return b['i'], off, len(data)

        # --- 4. Transform and place in blocks ----------------------------------
        # pool.map keeps the order, so the sort above survives even when the
        # answers come back in a jumble. buffersize keeps memory down: an
        # unfolded PNG can be a hundred times its file size, and 24 of those
        # in flight at once is enough to fill the machine.
        tasks = [(o['cls'] if transform else '', str(root/o['source'])) for o in ordered]
        counter = {'png': 0, 'jpeg': 0}
        with fut.ProcessPoolExecutor(jobs) as pool:
            for o, (tr, r) in zip(ordered, pool.map(_transform, tasks, chunksize=1,
                                                    buffersize=jobs)):
                if r:
                    counter[tr] += 1
                    o['tr'] = tr
                    if tr == 'png': o['recipe'] = r['recipe']
                    o['b_rest'] = add(f'{tr}-rest', r['rest'])
                    o['b'] = add('png-ra' if tr == 'png' else 'jpeg-kropp', r['body'])
                else:
                    with open(root/o['source'],'rb') as f:
                        o['b'] = add(o['cls'], f.read())
        if transform:
            totals = {k: sum(1 for o in ordered if o['cls'] == k) for k in ('png','jpeg')}
            for k in ('png','jpeg'):
                if totals[k]:
                    say(f'  {k}: {counter[k]}/{totals[k]} transformed')
        for b in open_blocks.values():
            if 'f' in b: b['f'].close(); b.pop('f')
        say(f'  {len(blocks)} blocks')

        # --- 5. Pack the blocks in parallel ------------------------------------
        if codec:
            for b in blocks:
                if b['codec'] != 'ra': b['codec'] = codec
        threads = max(1, jobs // max(1, min(len(blocks), jobs)))

        def pack_one(b):
            out = workdir/f'c{b["i"]}'
            b['codec'] = pack_block(b['stream'], out, b['codec'], threads)
            b['packed'] = out
            b['stream'].unlink(missing_ok=True)
        with fut.ThreadPoolExecutor(min(jobs, max(1,len(blocks)))) as pool:
            list(pool.map(pack_one, blocks))

        # --- 6. Write the archive -----------------------------------------------
        with open(archive,'wb') as out:
            out.write(MAGIC)
            for b in blocks:
                b['off'] = out.tell()
                with open(b['packed'],'rb') as f: shutil.copyfileobj(f, out, 1<<20)
                b['clen'] = out.tell() - b['off']
                b['packed'].unlink()
            # The register sits in parallel lists, not in a list of dicts.
            # Same facts, but the key names appear once instead of once per
            # object, and numbers of the same kind land next to each other
            # where they pack well. On a tree of 6,000 small files the
            # difference is a couple of percent of the WHOLE archive.
            #
            # The offsets within a block are not stored at all but derived:
            # the objects were appended to the blocks in exactly this order,
            # so a running sum of the lengths gives the same numbers. That it
            # does is checked below — a register that points one byte wrong
            # is an unreadable archive.
            #
            # The key names are v1 format bytes, Swedish-born and frozen:
            # rot = root, platt = flat, skapad = created, kodek = codec,
            # rålen = raw length, klass = class, objekt = objects,
            # poster = entries, mal = link target.
            register = dict(
                version=VERSION, rot=root.name, platt=flat,
                skapad=time.strftime('%Y-%m-%dT%H:%M:%S'),
                block=[dict(i=b['i'], kodek=b['codec'], off=b['off'], clen=b['clen'],
                            rålen=b['raw_len'], klass=b['cls']) for b in blocks],
                objekt=dict(
                    blk=[o['b'][0] for o in ordered],
                    len=[o['b'][2] for o in ordered],
                    s=[o['s'] for o in ordered],
                    h=[o['h'][:16] for o in ordered],   # 64 bits suffice for a check
                    tr={str(o['id']): [o['tr'], o['b_rest'][0], o['b_rest'][2],
                                       o.get('recipe')] for o in ordered if 'tr' in o},
                ),
                poster=[{k:v for k,v in p.items() if k in ('p','t','m','mt','mal','o')}
                        for p in entries],
            )
            check_offsets(register, ordered)
            line = json.dumps(register, ensure_ascii=False).encode()
            regoff = out.tell()
            out.write(_run(['zstd','-19','-q','-c'], stdin=line))
            out.write(struct.pack('<QQ', regoff, len(line)))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    size = archive.stat().st_size
    say(f'  done: {size/1048576:.2f} MiB (x{raw_bytes/max(size,1):.2f}) in {time.monotonic()-t0:.1f}s')
    return size


# ---------------------------------------------------------------------------
# Unpacking
# ---------------------------------------------------------------------------

def derive_offsets(reg):
    """Derives every object's place in its block from the lengths.

    The order is the same as at pack time: for a transformed object the
    rest was appended first, then the body.
    """
    o = reg['objekt']
    running = {}
    out = []
    for i, (blk, ln) in enumerate(zip(o['blk'], o['len'])):
        row = o['tr'].get(str(i))
        if row:
            rblk, rlen = row[1], row[2]
            roff = running.get(rblk, 0); running[rblk] = roff + rlen
        off = running.get(blk, 0); running[blk] = off + ln
        out.append(((blk, off, ln), (row[1], roff, row[2]) if row else None))
    return out


def check_offsets(register, ordered):
    """Proves that the derived places are exactly the ones that were used."""
    for derived, o in zip(derive_offsets(register), ordered):
        if tuple(o['b']) != derived[0] or ('tr' in o) != (derived[1] is not None) \
           or ('tr' in o and tuple(o['b_rest']) != derived[1]):
            raise RuntimeError(f'the register would point wrong for {o["source"]}')


def read_register(archive: Path):
    with open(archive,'rb') as f:
        if f.read(len(MAGIC)) != MAGIC: raise SystemExit('Not an nvz archive.')
        f.seek(-16, os.SEEK_END)
        regoff, _rawlen = struct.unpack('<QQ', f.read(16))
        f.seek(regoff)
        packed = f.read(os.path.getsize(archive) - 16 - regoff)
    return json.loads(_run(['zstd','-d','-q','-c'], stdin=packed).decode())


def unpack(archive: Path, dest: Path, *, jobs=None, quiet=False):
    t0 = time.monotonic()
    jobs = jobs or min(24, os.cpu_count() or 4)
    say = (lambda *a: None) if quiet else (lambda *a: print(*a, flush=True))
    reg = read_register(archive)
    # A FLAT archive is an archive of a lone file (the gzip surface packs
    # that way). Its entries go directly into the destination, not into a
    # directory named after the tree — and the root entry's permissions and
    # time belong to the temporary directory the file was packed through,
    # so they must not be pasted onto the destination directory.
    flat = bool(reg.get('platt'))
    root = dest if flat else dest/reg['rot']
    root.mkdir(parents=True, exist_ok=True)

    workdir = Path(tempfile.mkdtemp(prefix='nvz-un-', dir=dest))
    try:
        # The blocks are unpacked in parallel into the working directory.
        # Keeping them on disk instead of in memory is deliberate: an
        # archive can be larger than the machine's RAM, and then the unpack
        # must not be what stops.
        def unpack_one(b):
            packed = workdir/f'c{b["i"]}'
            with open(archive,'rb') as f:
                f.seek(b['off']); left = b['clen']
                with open(packed,'wb') as u:
                    while left:
                        d = f.read(min(1<<20, left))
                        u.write(d); left -= len(d)
            out = workdir/f'b{b["i"]}'
            unpack_block(packed, out, b['kodek'])
            packed.unlink(missing_ok=True)
            return b['i'], out
        with fut.ThreadPoolExecutor(min(jobs, max(1,len(reg['block'])))) as pool:
            streams = dict(pool.map(unpack_one, reg['block']))

        # Directories first, in tree order, so the files have somewhere to go.
        for p in reg['poster']:
            if p['t'] == 'd': (root/p['p']).mkdir(parents=True, exist_ok=True)
        for p in reg['poster']:
            if p['t'] == 'l':
                s = root/p['p']; s.parent.mkdir(parents=True, exist_ok=True)
                s.unlink(missing_ok=True); os.symlink(p['mal'], s)
                # The link's OWN time, not the target's. utime without this
                # follows the link and changes a completely different file's
                # timestamp.
                if 'mt' in p: os.utime(s, ns=(p['mt'], p['mt']), follow_symlinks=False)

        paths = {}
        for p in reg['poster']:
            if p['t'] == 'f': paths.setdefault(p['o'], []).append(p)
        places = derive_offsets(reg)

        # os.pread instead of seek+read: several threads read from the same
        # block at once, and a shared file position would have given them
        # each other's bytes.
        fds = {i: os.open(s, os.O_RDONLY) for i, s in streams.items()}

        def build(nr):
            (bi, off, ln), rest_place = places[nr]
            body = os.pread(fds[bi], ln, off)
            if rest_place:
                tr, _, _, recipe = reg['objekt']['tr'][str(nr)]
                bi2, off2, ln2 = rest_place
                rest = os.pread(fds[bi2], ln2, off2)
                data = (png_tr.rebuild(rest, body, recipe) if tr == 'png'
                        else jpeg_tr.rebuild(rest, body))
            else:
                data = body
            for p in paths.get(nr, ()):
                s = root/p['p']
                with open(s,'wb') as u: u.write(data)
                os.chmod(s, p['m']); os.utime(s, ns=(p['mt'], p['mt']))

        with fut.ThreadPoolExecutor(jobs) as pool:
            list(pool.map(build, sorted(paths)))
        for fd in fds.values(): os.close(fd)

        # Directories last, and deepest first: writing a file in a directory
        # changes the directory's time, so it must be set after everything
        # in it is in place.
        for p in reversed(reg['poster']):
            if p['t'] == 'd':
                if flat and p['p'] == '': continue
                s = root/p['p']
                os.chmod(s, p['m'])
                if 'mt' in p: os.utime(s, ns=(p['mt'], p['mt']))
    finally:
        shutil.rmtree(workdir, ignore_errors=True)
    say(f'  unpacked in {time.monotonic()-t0:.1f}s')


# ---------------------------------------------------------------------------

def list_archive(archive: Path):
    reg = read_register(archive)
    print(f"nvz v{reg['version']}  root={reg['rot']}  created {reg['skapad']}")
    transformed = {}
    for v in reg['objekt']['tr'].values(): transformed[v[0]] = transformed.get(v[0], 0) + 1
    print(f"{len(reg['poster'])} entries, {len(reg['objekt']['blk'])} objects, "
          f"{len(reg['block'])} blocks" + (f", transformed: {transformed}" if transformed else ""))
    for b in reg['block']:
        print(f"  block {b['i']:>3}  {b['klass']:<11} {b['kodek']:<7} "
              f"{b['rålen']/1048576:>9.2f} -> {b['clen']/1048576:>8.2f} MiB  "
              f"x{b['rålen']/max(b['clen'],1):.2f}")


def selftest(root: Path) -> bool:
    """Packs, unpacks and compares. An archiver that cannot prove it gives
    back exactly the same bytes is a lossy compressor."""
    root = root.resolve()
    with tempfile.TemporaryDirectory(prefix='nvz-test-') as d:
        d = Path(d); ark = d/'probe.nv'
        pack(root, ark)
        unpack(ark, d/'out')
        errors = 0
        for p in root.rglob('*'):
            if p.is_symlink():
                b = d/'out'/root.name/p.relative_to(root)
                if not b.is_symlink() or os.readlink(b) != os.readlink(p):
                    print('LINK DIFFERS:', p); errors += 1
                continue
            if not p.is_file(): continue
            b = d/'out'/root.name/p.relative_to(root)
            if not b.exists() or b.read_bytes() != p.read_bytes():
                print('DIFFERS:', p); errors += 1
            elif b.stat().st_mode != p.stat().st_mode or b.stat().st_mtime_ns != p.stat().st_mtime_ns:
                print('METADATA DIFFERS:', p); errors += 1
        print('TEST FAILED' if errors else 'Test ok: every file identical bit for bit.')
        return errors == 0


# ---------------------------------------------------------------------------
# Single-file archives
#
# The gzip surface packs one file at a time, and pack() walks a directory
# tree. A lone file is no tree, so it is packed through a tree of one file:
# a temporary directory with a HARD LINK to the file. A hard link is the
# same inode — content, permissions and time are the file's own, nothing is
# copied, and on a large archive it costs zero bytes and zero seconds. The
# register is marked 'platt' (flat) so the unpack knows the entry goes
# directly into the destination.
# ---------------------------------------------------------------------------

def pack_file(file: Path, archive: Path, *, name=None, **kw) -> int:
    with tempfile.TemporaryDirectory(prefix='nvz-one-', dir=archive.parent) as d:
        staged = Path(d)/(name or file.name)
        try:
            os.link(file, staged)
        except OSError:
            shutil.copy2(file, staged)   # filesystem boundary: then it may cost
        return pack(Path(d), archive, flat=True, **kw)


def name_in_archive(reg) -> str | None:
    """The name the file had when packed, or None if the archive is a tree."""
    if not reg.get('platt'): return None
    for p in reg['poster']:
        if p['t'] == 'f': return p['p']
    return None


def unpack_file(archive: Path, dest: Path, *, jobs=None) -> None:
    """Unpacks a flat single-file archive to exactly the path dest.

    The file is unpacked in a temporary directory on the same filesystem
    and then MOVED into place. A move within a filesystem only renames the
    inode, so permissions and time that unpack already set come along
    unchanged — and the destination either does not exist at all or is
    complete, never half-written.
    """
    dest.parent.mkdir(parents=True, exist_ok=True)
    workdir = Path(tempfile.mkdtemp(prefix='nvz-one-', dir=dest.parent))
    try:
        unpack(archive, workdir, jobs=jobs, quiet=True)
        found = [x for x in workdir.rglob('*') if x.is_file() or x.is_symlink()]
        if len(found) != 1:
            raise RuntimeError(f'{archive}: flat archive with {len(found)} files')
        os.replace(found[0], dest)
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def test_archive(archive: Path, *, jobs=None) -> list:
    """Unpacks the archive and compares every file against the hash in the
    register.

    The register carries the blake2b prefix of every object, taken on the
    original before packing. Unpacking and rehashing is therefore a real
    test of the whole chain — blocks, codec, PNG recipe and JPEG repack —
    and not just a checksum of the archive file.
    """
    reg = read_register(archive)
    with tempfile.TemporaryDirectory(prefix='nvz-test-') as d:
        d = Path(d)
        unpack(archive, d, jobs=jobs, quiet=True)
        root = d if reg.get('platt') else d/reg['rot']
        errors = []
        for p in reg['poster']:
            s = root/p['p']
            if p['t'] == 'f':
                if not s.is_file() or s.is_symlink(): errors.append((p['p'], 'missing'))
                elif hash_file(s)[0][:16] != reg['objekt']['h'][p['o']]:
                    errors.append((p['p'], 'content differs'))
            elif p['t'] == 'l':
                if not s.is_symlink() or os.readlink(s) != p['mal']:
                    errors.append((p['p'], 'link differs'))
            elif p['t'] == 'd' and not s.is_dir():
                errors.append((p['p'] or '.', 'directory missing'))
    return errors


def unpacked_size(reg) -> int:
    """Bytes the unpack writes — before dedup, i.e. what you get back."""
    s = reg['objekt']['s']
    return sum(s[p['o']] for p in reg['poster'] if p['t'] == 'f')


# ---------------------------------------------------------------------------
# The command line
#
# The surface is gzip's, because gzip's surface is the one already in your
# fingers:
#
#     nvz file           -> file.nv, original gone
#     nvz -d file.nv     -> file, archive gone
#     nvz -c file > out.nv   nvz -l ark.nv   nvz -t ark.nv
#     nvz -9 -k dir          cat file | nvz > file.nv
#
# ONE deviation, and it is deliberate: gzip says "is a directory -- ignored"
# about a directory, but a directory is precisely what nvz is built for. A
# directory as operand is therefore packed into ONE archive of the whole
# tree, with dedup and sorting between the files — and the source tree is
# always left in place, for nvz must not be able to erase a tree. For
# gzip's literal behavior there is -r: then every file is packed on its
# own, inside the directory, exactly as gzip does, and the gain BETWEEN
# files is lost.
#
# The subcommands (pack, unpack, list, test) remain unchanged. The
# measurements in benchmark/ and the tests in tests/ called them, and a
# change of surface must not make old numbers unmeasurable.
# ---------------------------------------------------------------------------

SUFFIX = '.nv'

# gzip's nine digits against nvpack's three modes. -6 is gzip's default and
# normal is nvpack's, so the middle must be the middle.
LEVEL_MODE = {1:'fast', 2:'fast', 3:'fast', 4:'normal', 5:'normal',
              6:'normal', 7:'small', 8:'small', 9:'small'}

SUBCOMMANDS = ('pack', 'unpack', 'list', 'test')

# Long flags -> the short one they mean, or a name of their own for those
# only nvz has. Empty string = accepted and does nothing (gzip has them,
# and a script that passes them must not fall over).
LONG_FLAGS = {
    'stdout':'c', 'to-stdout':'c', 'decompress':'d', 'uncompress':'d',
    'force':'f', 'help':'h', 'keep':'k', 'list':'l', 'license':'L',
    'no-name':'n', 'name':'N', 'quiet':'q', 'silent':'q', 'recursive':'r',
    'suffix':'S', 'test':'t', 'verbose':'v', 'version':'V',
    'fast':'1', 'best':'9',
    'synchronous':'', 'rsyncable':'', 'ascii':'', 'no-time':'',
    'mode':'MODE', 'codec':'CODEC', 'jobs':'J',
    'no-dedup':'DEDUP', 'no-transform':'TRANS',
}
SHORT = 'acdfhklLnNqrtvV'          # without argument
SHORT_WITH_ARG = {'S':'S', 'j':'J'}

HELP = f"""Usage: nvz [OPTION]... [FILE]...
Packs and unpacks files and directory trees. Without options every FILE is
packed and the original replaced, exactly like gzip.

  -c, --stdout       write to standard output, leave the files alone
  -d, --decompress   unpack
  -f, --force        overwrite targets, follow links, repack already packed
  -h, --help         this help
  -k, --keep         keep the file that was packed
  -l, --list         show what the archive holds
  -L, --license      the terms
  -n, --no-name      on unpack: derive the name from the archive's filename (default)
  -N, --name         on unpack: use the name stored in the archive
  -q, --quiet         say nothing
  -r, --recursive    descend into directories and pack every file on its own
  -S, --suffix=.SUF  suffix for the archives (default {SUFFIX})
  -t, --test          test the archive: unpack and compare against the register
  -v, --verbose      tell what happens (-vv shows the packing in detail)
  -V, --version      version
  -1 .. -9           fast .. small: -1..-3 mode fast, -4..-6 normal, -7..-9 small
                     (no digit: small, i.e. the smallest nvz can do)
      --fast, --best same as -1 and -9

Only nvz has these:
  -j, --jobs=N       this many parallel jobs
      --mode=M       fast | normal | small (beats -1..-9; default small)
      --codec=K      force zstd3 | zstd19 | zstd22 | xz9e | nvcm | nvlz
      --no-dedup, --no-transform

A DIRECTORY as operand is packed into ONE archive of the whole tree — that
is what nvz is for, and it is the only place the surface differs from
gzip. The source tree is always left in place; nvz never erases a
directory. With -r exactly what gzip does is done instead, one file at a
time, and then the dedup and the sorting BETWEEN files — most of the gain —
are lost.

Without FILE (or with "-") standard input is read and standard output written.
The subcommands pack, unpack, list and test remain and work.
If you have a FILE named like a subcommand: write nvz -- pack.

Exit status: 0 all well, 1 error, 2 warning."""

LICENSE = """nvz — content-aware archiver.
Written by Hugo Wikström. Free to use, change and share.
No warranty: every transform is tried in both directions at pack time, but
the test is the code's own word, not a promise."""


def _error(text) -> int:
    print(f'nvz: {text}', file=sys.stderr)
    return 1


def _warn(o, text) -> int:
    if not o['q']: print(f'nvz: {text}', file=sys.stderr)
    return 2


def _long_flag(name: str):
    """Resolves a long flag, accepting abbreviations as gzip does."""
    if name in LONG_FLAGS: return LONG_FLAGS[name]
    hits = [k for k in LONG_FLAGS if k.startswith(name)]
    if not hits: raise SystemExit(_error(f"unknown option -- '{name}'"))
    if len({LONG_FLAGS[k] for k in hits}) > 1:
        raise SystemExit(_error(f"ambiguous option -- '{name}' ({', '.join(sorted(hits))})"))
    return LONG_FLAGS[hits[0]]


def parse_args(argv):
    o = dict(c=False, d=False, f=False, k=False, l=False, n=False, N=False,
             q=False, r=False, t=False, v=0, suffix=SUFFIX, mode=None,
             codec=None, jobs=None, dedup=True, transform=None)
    files, files_only, i = [], False, 0

    def apply(h, value):
        """Sets a flag. Returns True if the next argv entry was consumed."""
        if h == '':  return False
        if h in '123456789': o['mode'] = LEVEL_MODE[int(h)]; return False
        if h == 'v': o['v'] += 1; return False
        if h == 'q': o['q'] = True; o['v'] = 0; return False
        if h == 'a': return False
        if h == 'h': print(HELP); raise SystemExit(0)
        if h == 'V': print(f'nvz {VERSION}.0'); raise SystemExit(0)
        if h == 'L': print(LICENSE); raise SystemExit(0)
        if h == 'DEDUP': o['dedup'] = False; return False
        if h == 'TRANS': o['transform'] = False; return False
        if h in ('S', 'J', 'MODE', 'CODEC'):
            consumed = False
            if value is None:
                if i >= len(argv): raise SystemExit(_error(f'option requires a value'))
                value = argv[i]; consumed = True
            if h == 'S':
                if not value.strip('.'): raise SystemExit(_error(f"invalid suffix '{value}'"))
                o['suffix'] = value if value.startswith('.') else '.' + value
            elif h == 'J':
                if not value.isdigit(): raise SystemExit(_error(f"-j takes a number, not '{value}'"))
                o['jobs'] = max(1, int(value))
            elif h == 'MODE':
                if value not in MODES:
                    raise SystemExit(_error(f"unknown mode '{value}' (choose {'|'.join(MODES)})"))
                o['mode'] = value
            else:
                if value not in ('zstd3','zstd19','zstd22','xz9e','nvcm','nvlz'):
                    raise SystemExit(_error(f"unknown codec '{value}'"))
                o['codec'] = value
            return consumed
        o[h] = True
        return False

    while i < len(argv):
        a = argv[i]; i += 1
        if files_only or a == '-' or not a.startswith('-'):
            files.append(a); continue
        if a == '--':
            files_only = True; continue
        if a.startswith('--'):
            name, value = a[2:], None
            if '=' in name: name, value = name.split('=', 1)
            if apply(_long_flag(name), value): i += 1
            continue
        j = 1
        while j < len(a):
            ch = a[j]; j += 1
            if ch in SHORT_WITH_ARG:
                value = a[j:] or None; j = len(a)
                if apply(SHORT_WITH_ARG[ch], value): i += 1
            elif ch in '123456789' or ch in SHORT:
                apply(ch, None)
            else:
                raise SystemExit(_error(f"unknown option -- '{ch}'\nTry nvz --help."))
    return o, files


def _packkw(o):
    return dict(mode=o['mode'] or 'normal', dedup=o['dedup'], transform=o['transform'],
                jobs=o['jobs'], codec=o['codec'], quiet=o['v'] < 2)


def _register(o, p: Path):
    try:
        return read_register(p)
    except SystemExit as e:
        _warn(o, f'{p}: {e}' if str(e) else f'{p}: not an nvz archive'); return None
    except Exception as e:
        _warn(o, f'{p}: unreadable archive ({e})'); return None


def _may_write(o, dest: Path) -> bool:
    """gzip's rule: if the target exists we ask at a terminal, otherwise we
    leave the file alone. A sweep packing a thousand files must not be able
    to overwrite anything just because nobody was watching."""
    if o['f'] or not os.path.lexists(dest): return True
    if sys.stdin.isatty() and sys.stderr.isatty():
        sys.stderr.write(f'nvz: {dest} already exists; overwrite (y/n)? ')
        sys.stderr.flush()
        if sys.stdin.readline().strip().lower()[:1] == 'y': return True
        sys.stderr.write('\tnot overwritten\n')
        return False
    _warn(o, f'{dest} already exists -- unchanged')
    return False


def _to_stdout(p: Path):
    with open(p, 'rb') as f:
        shutil.copyfileobj(f, sys.stdout.buffer, 1 << 20)
    sys.stdout.buffer.flush()


def _ratio(o, src: str, raw: int, packed: int, dest, replaced: bool):
    if o['v'] < 1 or o['q']: return
    pct = (1 - packed/raw) * 100 if raw else 0.0
    verb = 'replaced with' if replaced else 'written to'
    print(f'{src}:\t{pct:5.1f}% -- {verb} {dest}', file=sys.stderr)


def _tree_size(root: Path) -> int:
    total = 0
    for k, _, files in os.walk(root, followlinks=False):
        for n in files:
            try: total += os.lstat(os.path.join(k, n)).st_size
            except OSError: pass
    return total


# --- pack --------------------------------------------------------------------

def _pack_one_file(o, p: Path) -> int:
    if p.name.endswith(o['suffix']) and not o['f']:
        return _warn(o, f'{p} already has the suffix {o["suffix"]} -- unchanged')
    raw = p.stat().st_size
    if o['c']:
        with tempfile.TemporaryDirectory(prefix='nvz-out-') as d:
            ark = Path(d)/'out.nv'
            pack_file(p, ark, **_packkw(o))
            _ratio(o, str(p), raw, ark.stat().st_size, 'stdout', False)
            _to_stdout(ark)
        return 0
    out = Path(str(p) + o['suffix'])
    if not _may_write(o, out): return 2
    pack_file(p, out, **_packkw(o))
    st = p.stat()
    # The archive inherits the file's time and permissions. gzip does, and
    # it is what makes a packed directory readable as a directory: the
    # times are still there.
    os.chmod(out, st.st_mode & 0o7777)
    os.utime(out, ns=(st.st_atime_ns, st.st_mtime_ns))
    if not o['k']: p.unlink()
    _ratio(o, str(p), raw, out.stat().st_size, out, not o['k'])
    return 0


def _pack_tree(o, p: Path) -> int:
    raw = _tree_size(p) if o['v'] else 0
    if o['c']:
        with tempfile.TemporaryDirectory(prefix='nvz-out-') as d:
            ark = Path(d)/'out.nv'
            pack(p, ark, **_packkw(o))
            _ratio(o, str(p), raw, ark.stat().st_size, 'stdout', False)
            _to_stdout(ark)
        return 0
    out = Path(str(p) + o['suffix'])
    if not _may_write(o, out): return 2
    pack(p, out, **_packkw(o))
    _ratio(o, str(p), raw, out.stat().st_size, out, False)
    return 0


def _expand(o, files, wants_archives: bool):
    """Works out what is actually to be processed.

    With -r a directory is swapped for the files in it — gzip's behavior,
    one file at a time. Without -r the directory stands as its own operand,
    and the pack then makes ONE archive of the tree.

    In the sweep, files with the wrong suffix are skipped WITHOUT a word:
    gzip complains about each one, but a sweep is something you rerun, and
    an `nvz -r` that barks one line per already-packed file is unusable.
    Name a file yourself and you do get the warning — then the file was
    the point.
    """
    out = []
    for f in files:
        p = Path(f)
        if o['r'] and p.is_dir() and not p.is_symlink():
            for dirpath, dirs, names in os.walk(p, followlinks=False):
                dirs.sort(); names.sort()
                for n in names:
                    s = Path(dirpath)/n
                    if s.is_symlink(): continue
                    if n.endswith(o['suffix']) != wants_archives: continue
                    out.append(s)
        else:
            out.append(p)
    return out


def _pack_operand(o, p: Path) -> int:
    if not os.path.lexists(p):
        return _error(f'{p}: does not exist')
    if p.is_symlink():
        if not o['f']: return _warn(o, f'{p} is a symlink -- skipped')
        p = Path(os.path.realpath(p))
    if p.is_dir():
        return _pack_tree(o, p)
    if not p.is_file():
        return _warn(o, f'{p} is not a regular file -- skipped')
    links = p.stat().st_nlink
    if links > 1 and not (o['f'] or o['k'] or o['c']):
        return _warn(o, f'{p} has {links-1} other link -- unchanged')
    return _pack_one_file(o, p)


# --- unpack --------------------------------------------------------------------

def _dest_for(o, p: Path, reg) -> Path | None:
    """What name the unpack should get. Like gzip: derived from the
    archive's filename, or the one stored in the archive if -N is set."""
    stored = name_in_archive(reg)
    if o['N'] and stored: return p.parent/stored
    if p.name.endswith(o['suffix']):
        return p.parent/p.name[:-len(o['suffix'])]
    if stored: return p.parent/stored
    return None


def _unpack_operand(o, p: Path) -> int:
    if not p.is_file():
        # `nvz -d file` finds file.nv, exactly as gunzip finds file.gz.
        withsuf = Path(str(p) + o['suffix'])
        if os.path.lexists(p) or not withsuf.is_file():
            return _error(f'{p}: does not exist')
        p = withsuf
    # The suffix decides BEFORE the register is read: a file not named .nv
    # is not ours to unpack, and that is a warning (status 2) as in gzip —
    # not an error about the format.
    if not p.name.endswith(o['suffix']) and not (o['c'] or o['f']):
        return _warn(o, f'{p}: unknown suffix -- skipped')
    reg = _register(o, p)
    if reg is None: return 1
    if reg.get('platt'):
        dest = _dest_for(o, p, reg)
        if dest is None:
            return _warn(o, f'{p}: unknown suffix -- skipped')
        if o['c']:
            with tempfile.TemporaryDirectory(prefix='nvz-out-') as d:
                tmp = Path(d)/'f'
                unpack_file(p, tmp, jobs=o['jobs'])
                _to_stdout(tmp)
            return 0
        if dest.resolve() == p.resolve():
            return _error(f'{p}: the name in the archive is the archive\'s own name, '
                          'unpacking would overwrite it')
        if not _may_write(o, dest): return 2
        unpack_file(p, dest, jobs=o['jobs'])
        _ratio(o, str(p), unpacked_size(reg), p.stat().st_size, dest, not o['k'])
        if not o['k']: p.unlink()
        return 0
    # A tree. It cannot be poured into a byte stream, and it must not be
    # guessed into one — an archive of a thousand files is not a file.
    if o['c']:
        return _error(f'{p}: the archive holds a tree ({reg["rot"]}), '
                      'it cannot be written to stdout')
    root = p.parent/reg['rot']
    if not _may_write(o, root): return 2
    unpack(p, p.parent, jobs=o['jobs'], quiet=o['v'] < 2)
    _ratio(o, str(p), unpacked_size(reg), p.stat().st_size, root, not o['k'])
    if not o['k']: p.unlink()
    return 0


# --- list and test -------------------------------------------------------------

def _list_operand(o, p: Path, total: dict) -> int:
    reg = _register(o, p)
    if reg is None: return 1
    c = p.stat().st_size
    u = unpacked_size(reg)
    name = name_in_archive(reg) or reg['rot'] + '/'
    print(f'{c:>13} {u:>13} {(1-c/max(u,1))*100:6.1f}%  {name}')
    total['c'] += c; total['u'] += u; total['n'] += 1
    if o['v']:
        list_archive(p)
    return 0


def _test_operand(o, p: Path) -> int:
    reg = _register(o, p)
    if reg is None: return 1
    try:
        errors = test_archive(p, jobs=o['jobs'])
    except Exception as e:
        return _error(f'{p}: the test could not be run ({e})')
    if errors:
        for path, what in errors[:20]:
            print(f'nvz: {p}: {path}: {what}', file=sys.stderr)
        if len(errors) > 20:
            print(f'nvz: {p}: ... and {len(errors)-20} more', file=sys.stderr)
        return _error(f'{p}: TEST FAILED ({len(errors)} entries)')
    if o['v']: print(f'{p}:\tOK')
    return 0


# --- standard input and output ---------------------------------------------------

def _stream(o) -> int:
    """Without a file operand: stdin -> stdout, like gzip.

    The archive must be seekable (the register sits last and is read
    first), so the stream lands in a temporary file on the way. That is
    the difference from gzip, which really streams — and the reason is the
    format, not laziness: an archive with register and blocks is not a run
    of bytes readable from the front.
    """
    if not o['d'] and sys.stdout.isatty() and not o['f']:
        return _error('compressed data is not written to a terminal. '
                      'Use -f if you mean it, or name a file.')
    with tempfile.TemporaryDirectory(prefix='nvz-stream-') as d:
        d = Path(d)
        inside = d/'in'
        with open(inside, 'wb') as u:
            shutil.copyfileobj(sys.stdin.buffer, u, 1 << 20)
        if o['d'] or o['t'] or o['l']:
            reg = _register(o, inside)
            if reg is None: return 1
            if o['l']:
                total = dict(c=0, u=0, n=0)
                print(f'{"compressed":>13} {"uncompressed":>13}  ratio  name')
                return _list_operand(o, inside, total)
            if o['t']:
                return _test_operand(o, inside)
            if not reg.get('platt'):
                return _error('the archive holds a tree and cannot be '
                              'unpacked to stdout — give the archive as a file')
            out = d/'out'
            unpack_file(inside, out, jobs=o['jobs'])
            _to_stdout(out)
            return 0
        ark = d/'out.nv'
        # 'standard-in' is the stored entry name — v1 archive bytes, frozen.
        pack_file(inside, ark, name='standard-in', **_packkw(o))
        _to_stdout(ark)
        return 0


# ---------------------------------------------------------------------------

def main_gzip(argv) -> int:
    # -l and -t beat -d, exactly as in gzip: `nvz -dt ark.nv` tests.
    o, files = parse_args(argv)
    if not files:
        return _stream(o)

    status = 0
    if o['l']:
        total = dict(c=0, u=0, n=0)
        if not o['q']:
            print(f'{"compressed":>13} {"uncompressed":>13}  ratio  name')
        for p in _expand(o, files, True):
            status = max(status, _list_operand(o, p, total))
        if total['n'] > 1:
            print(f'{total["c"]:>13} {total["u"]:>13} '
                  f'{(1-total["c"]/max(total["u"],1))*100:6.1f}%  (total)')
        return status
    if o['t']:
        for p in _expand(o, files, True):
            status = max(status, _test_operand(o, p))
        return status
    if not o['d'] and o['c'] and sys.stdout.isatty() and not o['f']:
        return _error('compressed data is not written to a terminal. '
                      'Use -f if you mean it.')
    for p in _expand(o, files, bool(o['d'])):
        if str(p) == '-':
            status = max(status, _stream(o)); continue
        status = max(status, _unpack_operand(o, p) if o['d']
                     else _pack_operand(o, p))
    return status


def main_subcommands(argv) -> int:
    a = argparse.ArgumentParser(prog='nvz', description='Content-aware archiver')
    u = a.add_subparsers(dest='cmd', required=True)
    p = u.add_parser('pack'); p.add_argument('directory'); p.add_argument('archive')
    p.add_argument('-q','--quiet', action='store_true'); p.add_argument('-j','--jobs', type=int)
    p.add_argument('--no-dedup', action='store_true')
    p.add_argument('--no-transform', action='store_true')
    p.add_argument('--mode', choices=list(MODES), default='small')
    p.add_argument('--codec', choices=['zstd3','zstd19','zstd22','xz9e','nvcm','nvlz'])
    q = u.add_parser('unpack'); q.add_argument('archive'); q.add_argument('dest')
    q.add_argument('-q','--quiet', action='store_true'); q.add_argument('-j','--jobs', type=int)
    l = u.add_parser('list'); l.add_argument('archive')
    v = u.add_parser('test'); v.add_argument('directory')
    n = a.parse_args(argv)
    if n.cmd == 'pack':
        pack(Path(n.directory), Path(n.archive), mode=n.mode, dedup=not n.no_dedup,
             transform=False if n.no_transform else None,
             jobs=n.jobs, quiet=n.quiet, codec=n.codec)
    elif n.cmd == 'unpack':
        unpack(Path(n.archive), Path(n.dest), jobs=n.jobs, quiet=n.quiet)
    elif n.cmd == 'list':
        list_archive(Path(n.archive))
    elif n.cmd == 'test':
        return 0 if selftest(Path(n.directory)) else 1
    return 0


def main(argv=None) -> int:
    # The first word decides which surface it is. A file NAMED like a
    # subcommand is reached with `nvz -- pack`, exactly as a file named -f
    # is: that is what `--` is for.
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in SUBCOMMANDS:
        return main_subcommands(argv)
    return main_gzip(argv)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BrokenPipeError:
        os._exit(0)                     # `nvz -dc big.nv | head` is not an error
    except KeyboardInterrupt:
        sys.exit(130)
