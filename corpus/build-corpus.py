#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Builds the measurement corpora as HARD LINKS — zero extra disk, same bytes.

    python3 corpus/build-corpus.py            # all corpora
    python3 corpus/build-corpus.py gif logg   # only the named ones

The log corpus is the one exception to the hard links: log files are
WRITTEN while a measurement runs, and a corpus that grows between packing
and unpacking makes every comparison meaningless. The logs are therefore
copied once, and files touched within the last fifteen minutes are skipped
entirely.

The corpus directory names (kod, png, foto, gif, film, arkiv, logg) and
the target paths below are frozen: the committed manifests and published
reports refer to them.
"""
import os, shutil, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
HOME = Path.home()
SKIP = {'node_modules', '.git', 'venv', '.venv', '__pycache__', '.cache',
        'dist', 'build', '.next', '.nuxt', 'target', 'coverage', '.data', 'var'}
FRESH = 900          # seconds: newer than this = may be being written right now


def collect(dest_root: Path, source: Path, suffixes, skip=SKIP, copy=False, fresh=True,
            byte_cap=None, stride=None) -> int:
    """Links (or copies) files under *source* that end in *suffixes*.

    byte_cap stops once that many bytes are collected — files are taken in
    sorted order so the selection is the same every time the corpus is
    rebuilt. stride=(start, step) takes every *step*-th file starting at
    *start*, to spread a selection across a whole archive instead of taking
    its beginning.
    """
    if not source.is_dir(): return 0
    candidates = []
    for root, dirs, files in os.walk(source):
        dirs[:] = sorted(d for d in dirs if d not in skip)
        for f in sorted(files):
            if not f.lower().endswith(suffixes): continue
            src = Path(root) / f
            if not src.is_file() or src.is_symlink(): continue
            candidates.append(src)
    candidates.sort()
    if stride:
        start, step = stride
        candidates = candidates[start::step]
    n = total = 0
    now = time.time()
    for src in candidates:
        try: st = src.stat()
        except OSError: continue
        if copy and fresh and now - st.st_mtime < FRESH: continue
        if byte_cap is not None and total + st.st_size > byte_cap: continue
        dest = dest_root / src.relative_to(source)
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            if copy: shutil.copy2(src, dest)
            else: os.link(src, dest)
        except FileExistsError: pass
        except OSError:
            try: shutil.copy2(src, dest)
            except OSError: continue
        n += 1; total += st.st_size
    return n


CODE = ('.mjs','.js','.ts','.jsx','.tsx','.py','.rs','.go','.c','.h','.html','.css',
        '.md','.json','.sh','.sql','.yml','.yaml','.toml','.svg')
GIF = ('.gif',)
VIDEO = ('.mp4','.webm')
ARCHIVES = ('.zip','.tar.gz','.tgz','.tar')
LOGS = ('.log','.log.1','.jsonl','syslog.1','kern.log','auth.log','dpkg.log')

# Each entry: (target, source, suffixes, extra keyword args for collect()).
# Target paths are frozen — manifests and reports refer to them.
PLAN = {
 'kod': [(f'kod/{p}', HOME/p, CODE, {}) for p in
         ('lostconnection','medvind','alltid','pocka','hermes','ting','symbios','fishy','keyup','masterlife','webscoute','ullabella','niclas','blendfarm','aix-pro','hak','frokenhoger','eystrasalt')],
 'png': [('png/hemsogrisen', HOME/'Downloads/Hemsogrisen_44_PNG_bilder', ('.png',), {}),
         ('png/skarmdumpar', HOME/'Pictures/Screenshots', ('.png',), {}),
         ('png/niclas',      HOME/'niclas/.data', ('.png',), {}),
         ('png/hak',         HOME/'hak/film/filmer', ('.png',), {}),
         ('png/ting-paritet',HOME/'ting/var/paritet', ('.png',), {})],
 'foto':[('foto/pecka-2018-12', HOME/'pecka-bilder/2018/2018-12', ('.jpg','.jpeg'), {}),
         ('foto/pecka-2017-06', HOME/'pecka-bilder/2017/2017-06', ('.jpg','.jpeg'), {}),
         ('foto/lc-klipp',      HOME/'lostconnection/data/klipp', ('.jpg',), {})],

 # --- GIF: everything the machine has. Animated GIFs are LZW-packed one
 # frame at a time, and the six demo GIFs in various node_modules are exact
 # copies of each other — precisely the duplicate pattern a real archive has.
 'gif': [('gif/pecka-bilder', HOME/'pecka-bilder',      GIF, {}),
         ('gif/pecka-stage',  HOME/'pecka-stage',       GIF, {}),
         ('gif/pecka-drive',  HOME/'pecka-drive',       GIF, {}),
         ('gif/pecka-tjanst', HOME/'pecka-tjanster',    GIF, {}),
         ('gif/bevarat',      HOME/'bevarat-fran-pecka',GIF, {}),
         ('gif/webb-demo',    HOME/'hemsogrisen',       GIF, {'skip': set()}),
         ('gif/grisspel',     HOME/'grisspel',          GIF, {'skip': set()}),
         ('gif/hamtat',       HOME/'Downloads',         GIF, {})],

 # --- VIDEO: three different kinds of video, so nobody can say the result
 # hinges on a single encoder. TikTok h264, own rip, VP9 screen capture.
 'film':[('film/lc-klipp',   HOME/'lostconnection/data', ('.mp4',),
                             {'byte_cap': 220*1024*1024, 'stride': (0, 7)}),
         ('film/hemsogrisen',HOME/'hemsogrisen-rip/tiktok', ('.mp4',), {}),
         ('film/skarmfilm',  HOME/'Videos/Screencasts', ('.webm',),
                             {'byte_cap': 140*1024*1024}),
         ('film/klippdemo',  HOME/'Downloads', ('.mp4',), {'byte_cap': 40*1024*1024})],

 # --- ARCHIVES: zip and tar.gz, i.e. packed inside packed. Two of the
 # pairs are bit-identical copies (Hemsögrisen's PNG zip downloaded twice,
 # the SYMBIOS starter in two places) — dedup must find them, compression
 # must not.
 'arkiv':[('arkiv/hamtat',  HOME/'Downloads', ('.zip','.tar.gz','.tgz'),
                            {'byte_cap': 180*1024*1024}),
          ('arkiv/drive',   HOME/'pecka-drive', ('.zip',),
                            {'byte_cap': 60*1024*1024}),
          ('arkiv/starter', HOME/'claude', ('.zip',), {}),
          ('arkiv/rot',     HOME, ('.zip','.tar.gz'), {'skip': set(os.listdir(HOME))}),
          ('arkiv/crm',     HOME/'aix-pro', ('.tar.gz',), {})],

 # --- LOGS: real log files, copied so they stand still. Syslog is the most
 # repetitive text a machine produces; the agent transcripts are JSONL with
 # long shared prefixes. Both kinds are included.
 'logg':[('logg/system',    Path('/var/log'), ('syslog.1','kern.log','auth.log','dpkg.log','bootstrap.log'),
                            {'copy': True, 'skip': set()}),
         ('logg/app',        HOME/'.config/Claude/logs', ('.log',), {'copy': True}),
         ('logg/hermes',     HOME/'.hermes/logs', ('.log',), {'copy': True}),
         ('logg/lc-hamtning',HOME/'lostconnection', ('.log',), {'copy': True}),
         ('logg/pecka-import',HOME/'pecka-import', ('.log',), {'copy': True}),
         ('logg/transkript', HOME/'.claude/projects/-home-hugo-medvind', ('.jsonl',),
                             {'copy': True, 'byte_cap': 40*1024*1024}),
         ('logg/transkript-llmgw', HOME/'.claude/projects/-home-hugo-llmgw', ('.jsonl',),
                             {'copy': True, 'byte_cap': 10*1024*1024})],
}

# --- freezing the corpus ------------------------------------------------------
# Hard links save disk, but they make the corpus LIVE: the file in
# corpus/kod is the same inode as the file in the working tree, so a saved
# document in another tab changes the corpus mid-measurement. It happened
# on 2026-11-18 — three files in corpus/kod/ullabella changed between
# packing and comparison, and the measurement reported "DIFFERS" on an
# archive that was flawless. The log corpus has always been copied for
# exactly that reason (see the header); with --copy it applies to all.
# (--kopiera is kept as an alias: published reports refer to it.)
FREEZE = '--copy' in sys.argv or '--kopiera' in sys.argv
chosen = [a for a in sys.argv[1:] if not a.startswith('--')] or list(PLAN)
for corpus in chosen:
    if corpus not in PLAN:
        print(f'unknown corpus: {corpus}', file=sys.stderr); sys.exit(1)
    shutil.rmtree(HERE/corpus, ignore_errors=True)
    for target, source, suffixes, extra in PLAN[corpus]:
        collect(HERE/target, source, suffixes,
                **({**extra, 'copy': True, 'fresh': False} if FREEZE else extra))
    files = [p for p in (HERE/corpus).rglob('*') if p.is_file()]
    nbytes = sum(p.stat().st_size for p in files)
    print(f'{corpus:<6} {len(files):>6} files  {nbytes/1048576:>8.1f} MiB')
