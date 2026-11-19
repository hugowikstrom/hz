#!/usr/bin/env python3
# [EN] Builds the measurement corpora from live data on this machine.
# Hard-links by default to save disk; --kopiera freezes the corpus with
# real copies (a corpus that moves mid-measurement invalidates the run).
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
"""Bygger testkorpusarna som HÅRDLÄNKAR — noll extra diskutrymme, samma bytes.

    python3 korpus/bygg-korpus.py            # alla korpusar
    python3 korpus/bygg-korpus.py gif logg   # bara de nämnda

Loggkorpusen är det enda undantaget från hårdlänkarna: loggfiler SKRIVS
medan mätningen pågår, och en korpus som växer mellan packning och
uppackning gör varje jämförelse meningslös. Därför kopieras loggarna en
gång, och filer som rörts den senaste kvarten hoppas över helt.
"""
import os, shutil, sys, time
from pathlib import Path

HIT = Path(__file__).resolve().parent
HEM = Path.home()
HOPPA = {'node_modules', '.git', 'venv', '.venv', '__pycache__', '.cache',
         'dist', 'build', '.next', '.nuxt', 'target', 'coverage', '.data', 'var'}
FARSKT = 900          # sekunder: nyare än så här = kan skrivas just nu


def samla(mal: Path, kalla: Path, andelser, hoppa=HOPPA, kopiera=False, farskt=True,
          bytetak=None, flytt=None) -> int:
    """Länkar (eller kopierar) filer under *kalla* som slutar på *andelser*.

    bytetak stoppar när så många byte samlats — filerna tas i sorterad
    ordning så att urvalet blir detsamma varje gång korpusen byggs om.
    flytt=(tag, av) tar var *av*:e fil med start på *tag*, för att kunna
    sprida ett urval över ett helt arkiv i stället för att ta början av det.
    """
    if not kalla.is_dir(): return 0
    kandidater = []
    for rot, kataloger, filer in os.walk(kalla):
        kataloger[:] = sorted(d for d in kataloger if d not in hoppa)
        for f in sorted(filer):
            if not f.lower().endswith(andelser): continue
            kall = Path(rot) / f
            if not kall.is_file() or kall.is_symlink(): continue
            kandidater.append(kall)
    kandidater.sort()
    if flytt:
        tag, av = flytt
        kandidater = kandidater[tag::av]
    n = summa = 0
    nu = time.time()
    for kall in kandidater:
        try: st = kall.stat()
        except OSError: continue
        if kopiera and farskt and nu - st.st_mtime < FARSKT: continue
        if bytetak is not None and summa + st.st_size > bytetak: continue
        dest = mal / kall.relative_to(kalla)
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            if kopiera: shutil.copy2(kall, dest)
            else: os.link(kall, dest)
        except FileExistsError: pass
        except OSError:
            try: shutil.copy2(kall, dest)
            except OSError: continue
        n += 1; summa += st.st_size
    return n


KOD = ('.mjs','.js','.ts','.jsx','.tsx','.py','.rs','.go','.c','.h','.html','.css',
       '.md','.json','.sh','.sql','.yml','.yaml','.toml','.svg')
GIF = ('.gif',)
FILM = ('.mp4','.webm')
ARKIV = ('.zip','.tar.gz','.tgz','.tar')
LOGG = ('.log','.log.1','.jsonl','syslog.1','kern.log','auth.log','dpkg.log')

# Varje post: (mål, källa, ändelser, extra flaggor till samla()).
PLAN = {
 'kod': [(f'kod/{p}', HEM/p, KOD, {}) for p in
         ('lostconnection','medvind','alltid','pocka','hermes','ting','symbios','fishy','keyup','masterlife','webscoute','ullabella','niclas','blendfarm','aix-pro','hak','frokenhoger','eystrasalt')],
 'png': [('png/hemsogrisen', HEM/'Downloads/Hemsogrisen_44_PNG_bilder', ('.png',), {}),
         ('png/skarmdumpar', HEM/'Pictures/Screenshots', ('.png',), {}),
         ('png/niclas',      HEM/'niclas/.data', ('.png',), {}),
         ('png/hak',         HEM/'hak/film/filmer', ('.png',), {}),
         ('png/ting-paritet',HEM/'ting/var/paritet', ('.png',), {})],
 'foto':[('foto/pecka-2018-12', HEM/'pecka-bilder/2018/2018-12', ('.jpg','.jpeg'), {}),
         ('foto/pecka-2017-06', HEM/'pecka-bilder/2017/2017-06', ('.jpg','.jpeg'), {}),
         ('foto/lc-klipp',      HEM/'lostconnection/data/klipp', ('.jpg',), {})],

 # --- GIF: allt maskinen har. Animerade GIF:ar är LZW-packade en bildruta
 # i taget, och de sex demo-GIF:arna i olika node_modules är exakta kopior
 # av varandra — precis det dubblettmönster ett riktigt arkiv har.
 'gif': [('gif/pecka-bilder', HEM/'pecka-bilder',      GIF, {}),
         ('gif/pecka-stage',  HEM/'pecka-stage',       GIF, {}),
         ('gif/pecka-drive',  HEM/'pecka-drive',       GIF, {}),
         ('gif/pecka-tjanst', HEM/'pecka-tjanster',    GIF, {}),
         ('gif/bevarat',      HEM/'bevarat-fran-pecka',GIF, {}),
         ('gif/webb-demo',    HEM/'hemsogrisen',       GIF, {'hoppa': set()}),
         ('gif/grisspel',     HEM/'grisspel',          GIF, {'hoppa': set()}),
         ('gif/hamtat',       HEM/'Downloads',         GIF, {})],

 # --- FILM: tre olika sorters film, för att ingen ska kunna säga att
 # resultatet hänger på en enda kodare. TikTok-h264, egen rip, VP9-skärmfilm.
 'film':[('film/lc-klipp',   HEM/'lostconnection/data', ('.mp4',),
                             {'bytetak': 220*1024*1024, 'flytt': (0, 7)}),
         ('film/hemsogrisen',HEM/'hemsogrisen-rip/tiktok', ('.mp4',), {}),
         ('film/skarmfilm',  HEM/'Videos/Screencasts', ('.webm',),
                             {'bytetak': 140*1024*1024}),
         ('film/klippdemo',  HEM/'Downloads', ('.mp4',), {'bytetak': 40*1024*1024})],

 # --- ARKIV: zip och tar.gz, alltså packat inuti packat. Två av paren är
 # bit-identiska kopior (Hemsögrisens PNG-zip hämtad två gånger, SYMBIOS-
 # startaren på två ställen) — dedupen ska hitta dem, komprimeringen inte.
 'arkiv':[('arkiv/hamtat',  HEM/'Downloads', ('.zip','.tar.gz','.tgz'),
                            {'bytetak': 180*1024*1024}),
          ('arkiv/drive',   HEM/'pecka-drive', ('.zip',),
                            {'bytetak': 60*1024*1024}),
          ('arkiv/starter', HEM/'claude', ('.zip',), {}),
          ('arkiv/rot',     HEM, ('.zip','.tar.gz'), {'hoppa': set(os.listdir(HEM))}),
          ('arkiv/crm',     HEM/'aix-pro', ('.tar.gz',), {})],

 # --- LOGG: riktiga loggfiler, kopierade så att de står stilla. Syslog är
 # den mest upprepade text en maskin producerar; agenttranskripten är
 # JSONL med långa gemensamma prefix. Båda sorterna finns med.
 'logg':[('logg/system',    Path('/var/log'), ('syslog.1','kern.log','auth.log','dpkg.log','bootstrap.log'),
                            {'kopiera': True, 'hoppa': set()}),
         ('logg/app',        HEM/'.config/Claude/logs', ('.log',), {'kopiera': True}),
         ('logg/hermes',     HEM/'.hermes/logs', ('.log',), {'kopiera': True}),
         ('logg/lc-hamtning',HEM/'lostconnection', ('.log',), {'kopiera': True}),
         ('logg/pecka-import',HEM/'pecka-import', ('.log',), {'kopiera': True}),
         ('logg/transkript', HEM/'.claude/projects/-home-hugo-medvind', ('.jsonl',),
                             {'kopiera': True, 'bytetak': 40*1024*1024}),
         ('logg/transkript-llmgw', HEM/'.claude/projects/-home-hugo-llmgw', ('.jsonl',),
                             {'kopiera': True, 'bytetak': 10*1024*1024})],
}

# --- att frysa korpusen -----------------------------------------------------
# Hårda länkar sparar disk, men de gör korpusen LEVANDE: filen i korpus/kod
# är samma inod som filen i arbetsträdet, så ett sparat dokument i en annan
# flik ändrar korpusen mitt i en mätning. Det hände 2026-11-18 — tre filer i
# korpus/kod/ullabella ändrades mellan packning och jämförelse, och
# mätningen rapporterade "SKILJER" på ett arkiv som var felfritt.
# Loggkorpusen har alltid kopierats av precis det skälet (se huvudet); med
# --kopiera gäller det alla.
FRYS = '--kopiera' in sys.argv
valda = [a for a in sys.argv[1:] if not a.startswith('--')] or list(PLAN)
for korpus in valda:
    if korpus not in PLAN:
        print(f'okänd korpus: {korpus}', file=sys.stderr); sys.exit(1)
    shutil.rmtree(HIT/korpus, ignore_errors=True)
    for mal, kalla, andelser, extra in PLAN[korpus]:
        samla(HIT/mal, kalla, andelser,
              **({**extra, 'kopiera': True, 'farskt': False} if FRYS else extra))
    filer = [p for p in (HIT/korpus).rglob('*') if p.is_file()]
    byte = sum(p.stat().st_size for p in filer)
    print(f'{korpus:<6} {len(filer):>6} filer  {byte/1048576:>8.1f} MiB')
