#!/usr/bin/env python3
"""RETIRED. This is the original Python implementation of nvz, kept
verbatim as the v1 archive-format reference: the C++ implementation reads
its archives bit for bit, and the test suite holds the two to the same
bytes. It is built by nothing and called by nothing, and it is the one
file in this repository that was deliberately left in Swedish when the
source went English on 2026-11-19 — translating retired reference code
only risks drift.

nvz — en innehållsmedveten arkivpackare.

Kommandoraden är gzips — samma flaggor, samma standardbeteende:

    nvz fil                      # -> fil.nv, originalet borta
    nvz -d fil.nv                # -> fil, arkivet borta
    nvz -c fil > ut.nv           # till standard ut
    nvz -l ark.nv                # packat, uppackat, kvot
    nvz -t ark.nv                # packa upp och jämför mot registret
    nvz -k -9 katalog            # hela trädet till katalog.nv

En KATALOG som operand packas till ETT arkiv av trädet. Det är enda stället
där ytan skiljer sig från gzip, och det är med flit: en katalog är vad nvz
är byggd för. Källträdet lämnas alltid kvar. Med -r görs gzips bokstavliga
sak i stället — en fil i taget — och då går vinsten MELLAN filerna förlorad.

Underkommandona finns kvar; matning/ och prov/ anropar dem:

    nvz packa <katalog> <arkiv.nv>
    nvz packa-upp <arkiv.nv> <målkatalog>
    nvz lista <arkiv.nv>
    nvz prova <katalog>          # packa, packa upp, jämför bit för bit

VARFÖR DEN FINNS. `tar | zstd` är två program som inte vet något om varandra.
tar lägger filerna i den ordning katalogträdet råkar ha dem och skickar vidare
en enda byteström; zstd ser den strömmen genom ett fönster och får aldrig veta
att fil 4000 är en exakt kopia av fil 12, eller att de 9 MB "slump" den just
gav upp på i själva verket är en deflate-ström som går att veckla ut.

nvz vet fem saker som tar inte vet:

  1. IDENTISKA FILER LAGRAS EN GÅNG. Innehållet hashas; dubbletter blir
     pekare. tar lagrar varje kopia, och zstds fönster hittar bara de kopior
     som råkar ligga nära varandra i strömmen.

  2. LIKA FILER LÄGGS BREDVID VARANDRA. Posterna sorteras på klass och
     ändelse före packning, så att all JavaScript ligger i en följd och alla
     skärmdumpar i en annan. Komprimeringen får då syfta på likheter som i
     katalogordning låg hundratals megabyte isär.

  3. PNG VECKLAS UT OCH PACKAS OM. En PNG är redan deflate-packad, och deflate
     från 1996 är ett dåligt skal att ha runt sin bilddata. nvz vecklar ut
     IDAT-strömmen till råa filterrader och sparar det RECEPT som gör att
     exakt samma bytes kan deflateras fram igen. Se png.py.

  4. JPEG KODAS OM UTAN ATT RÖRA EN PIXEL. Huffman-lagret byts mot en adaptiv
     aritmetisk kodare med modeller för hur koefficienter faktiskt ser ut
     bredvid varandra. Bilden är densamma, byte för byte, när den packas upp.
     Se nvjpg.c.

  5. REDAN PACKAT FÅR VARA I FRED. Film, ljud och zip-arkiv skickas inte genom
     en dyr komprimerare som ändå inte hittar något. Det är ingen besparing i
     byte men en stor besparing i tid — och tid är skälet till att dagens
     arkiv körs med zstd -3 i stället för -19.

VARJE OMVANDLING BEVISAS VID PACKNING. Både PNG- och JPEG-vägen syr ihop sitt
resultat igen och jämför med originalet innan det godtas. Går det inte ihop
lagras filen rå. En arkivpackare får ha omvandlingar den inte klarar; den får
aldrig TRO att den klarade.

FORMATET. Arkivet är block + register.

    "NVPACK\\x01" | block 0 | block 1 | ... | register (zstd) | <u64 offset, u64 längd>

Varje block är en oberoende packad följd av objektkroppar, med sin egen kodek.
Att blocken är oberoende är med flit: de packas parallellt på alla kärnor, och
en uppackning som bara vill ha en fil behöver bara röra ett block.
"""
from __future__ import annotations

import argparse, concurrent.futures as fut, hashlib, json, os, shutil, stat, struct
import subprocess, sys, tempfile, threading, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import png as png_omv
import jpeg as jpeg_omv

MAGI = b'NVPACK\x01'
VERSION = 1
BLOCKTAK = 192 * 1024 * 1024      # rå storlek per block innan ett nytt påbörjas
PROVBYTE = 4 * 1024 * 1024        # så mycket av ett block som kodekvalet provar på

# Hur många gånger större än sin IDAT-ström en PNG får bli innan utvecklingen
# avstås. Gränsen är INTE en lönsamhetsgräns — experiment/png-troskel.py mätte
# saken och motbevisade den gissningen: utvecklingen vann i varje klass, och
# allra mest (−40 %) för bilder som blåstes upp över 32 gånger. Det är logiskt
# i efterhand. En bild som deflate redan klämt 50:1 är ett skärmklipp med
# stora enfärgade ytor, och när femtio SÅDANA ligger utvecklade i samma block
# hittar en modern packare likheterna MELLAN bilderna — något deflate aldrig
# kunde se genom sitt 32 kB-fönster, en fil i taget.
#
# Taket finns därför bara som skyddsräcke mot orimlig resursåtgång: en enda
# 50 MB-skärmdump får inte bli flera GB arbetsdata.
PNG_UPPBLASNING_TAK = 150


# ---------------------------------------------------------------------------
# Klassificering
# ---------------------------------------------------------------------------
# Ändelsen är en gissning, magiska bytes är ett besked. Klassen avgör tre
# saker: vilken omvandling som provas, vilket block posten hamnar i, och
# vilken kodek blocket får.

TEXTANDELSER = {'.txt','.md','.json','.jsonl','.js','.mjs','.cjs','.ts','.tsx','.jsx','.py','.rs',
 '.go','.c','.h','.cpp','.hpp','.java','.sh','.bash','.zsh','.sql','.css','.scss','.html','.htm',
 '.xml','.svg','.yml','.yaml','.toml','.ini','.cfg','.conf','.service','.log','.csv','.tsv',
 '.po','.pot','.map','.lock','.patch','.diff','.rst','.tex','.webmanifest'}

OPAKA = {'.mp4','.m4a','.m4v','.mov','.mkv','.webm','.mp3','.ogg','.opus','.flac',
 '.webp','.avif','.heic','.gif','.zip','.gz','.tgz','.bz2','.xz','.zst','.7z','.rar','.br',
 '.woff','.woff2','.pdf','.jxl','.wasm','.deb','.rpm','.whl','.jar','.apk','.odt','.ods',
 '.docx','.xlsx','.pptx'}


def klassificera(sokvag: Path, huvud: bytes) -> str:
    if huvud[:8] == b'\x89PNG\r\n\x1a\n': return 'png'
    if huvud[:3] == b'\xff\xd8\xff':      return 'jpeg'
    ändelse = sokvag.suffix.lower()
    if ändelse in OPAKA: return 'opak'
    if ändelse in TEXTANDELSER: return 'text'
    # Okänd ändelse: färre än var tjugonde byte utanför det läsbara området
    # räcker för att kalla något text. Text vinner på att hamna bland text.
    prov = huvud[:4096]
    if prov and sum(b < 9 or (13 < b < 32) for b in prov) * 20 < len(prov):
        return 'text'
    return 'binar'


# Kodek per klass och läge. 'prova' betyder att blocket självt får avgöra
# genom att packa ett stickprov med varje kandidat.
#
#   snabb   — som dagens arkivskript: zstd -3, inga omvandlingar. Finns med
#             för att kunna mäta mot det, och för arkiv som ska gå fort.
#   normal  — omvandlingar på, zstd -19 eller xz beroende på vad som vinner.
#   liten   — text går genom nvcm. Den packar ett par MB/s, så den får bara
#             de block där den tar hem det: text, källkod, PNG-skelett och
#             JPEG-huvuden. Utvecklad bilddata är för voluminös för den.
#
# Lägg märke till att inte ens film och zip-arkiv står som 'ra' utan som
# 'prova'. Att anta att "redan packat" inte går att packa mer kostade 150 MiB
# i första mätningen av PNG-korpusen: de skärmdumpar som INTE gick att veckla
# ut lagrades rått, och då missade nvz att fyra tusen skärmdumpar av samma
# webbsida liknar VARANDRA, även när deras deflate-strömmar är ogenomträngliga
# var för sig. Provet kostar ett par sekunder per block och svarar 'ra' av sig
# självt när antagandet stämmer. Enda undantaget är jpeg-kropp: den är vår
# egen aritmetiska ström, och den ÄR brus.
LÄGEN = {
 'snabb':  {'text':'zstd3','binar':'zstd3','png-ra':'zstd3','png-rest':'zstd3',
            'jpeg-rest':'zstd3','jpeg-kropp':'ra','jpeg':'ra','opak':'ra','png':'zstd3'},
 'normal': {'text':'prova','binar':'prova','png-ra':'prova','png-rest':'zstd19',
            'jpeg-rest':'zstd19','jpeg-kropp':'ra','jpeg':'prova','opak':'prova','png':'prova'},
 'liten':  {'text':'prova-cm','binar':'prova-cm','png-ra':'prova','png-rest':'prova-cm',
            'jpeg-rest':'prova-cm','jpeg-kropp':'ra','jpeg':'prova','opak':'prova','png':'prova'},
 # gpu — som normal, men nvlz får också vara med och tävla om varje block.
 # nvlz är nvpacks egen kodek där matchningssökningen körs på grafikkortet.
 # LÄS MÄTNINGEN I README INNAN DU ANVÄNDER DEN: på den här maskinen vinner
 # den inget block, och läget finns kvar för att mätningen ska gå att göra om.
 'gpu':    {'text':'prova-gpu','binar':'prova-gpu','png-ra':'prova-gpu','png-rest':'prova-gpu',
            'jpeg-rest':'prova-gpu','jpeg-kropp':'ra','jpeg':'prova','opak':'prova','png':'prova-gpu'},
}
def hjälpbinär(namn: str) -> str:
    """Hitta nvcm/nvjpg/nvgpu bredvid skriptet, annars i PATH.

    Bredvid först, så att ett utcheckat träd använder sina egna byggen och
    inte något installerat. PATH som reserv, för att nvz ska gå att köra som
    EN fil (zipapp) där det inte finns någon katalog att ligga bredvid."""
    granne = Path(__file__).resolve().parent / namn
    if granne.is_file() and os.access(granne, os.X_OK):
        return str(granne)
    return shutil.which(namn) or str(granne)


NVCM = hjälpbinär('nvcm')
NVGPU = hjälpbinär('nvgpu')

# nvcm tar en halv gigabyte modelltabeller per block den packar. Blocken körs
# parallellt, och utan den här grinden kan tjugofyra av dem starta samtidigt
# och ta maskinens minne med sig.
NVCM_GRIND = threading.Semaphore(4)

# Det finns EN grafikkrets. Två nvlz-packningar samtidigt delar inte på den,
# de står i kö hos drivrutinen — och varje extra samtidig kärna ökar risken
# att vakthunden dödar hela kontexten. Två räcker för att gömma
# insamlingstiden bakom kärntiden.
NVLZ_GRIND = threading.Semaphore(2)


def gpu_finns() -> bool:
    """Går det att packa med nvlz här? Svaret cachas: det kostar ett
    processstart att svara, och frågan ställs en gång per block."""
    if gpu_finns.svar is None:
        try:
            r = subprocess.run([NVGPU, 'enhet', '--tyst'], capture_output=True, timeout=30)
            gpu_finns.svar = r.returncode == 0
        except (OSError, subprocess.SubprocessError):
            gpu_finns.svar = False
    return gpu_finns.svar


gpu_finns.svar = None


# ---------------------------------------------------------------------------
# Kodekar
# ---------------------------------------------------------------------------

def _kör(args, stdin=None):
    r = subprocess.run(args, input=stdin, capture_output=True)
    if r.returncode: raise RuntimeError(f'{args[0]}: {r.stderr.decode()[:400]}')
    return r.stdout


def packa_block(ra: Path, ut: Path, kodek: str, trådar: int) -> str:
    """Packar ett blocks rådata. Returnerar kodeken som faktiskt användes."""
    if kodek in ('prova', 'prova-cm', 'prova-gpu'):
        kodek = välj_kodek(ra, med_cm=(kodek == 'prova-cm'),
                           med_gpu=(kodek == 'prova-gpu'))
    if kodek == 'ra':
        os.replace(ra, ut)
    elif kodek == 'zstd3':
        _kör(['zstd','-3',f'-T{trådar}','-q','-f','-o',str(ut),str(ra)])
    elif kodek == 'nvcm':
        with NVCM_GRIND, open(ra,'rb') as i, open(ut,'wb') as u:
            r = subprocess.run([NVCM,'packa'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvcm: '+r.stderr.decode()[:300])
    elif kodek == 'nvlz':
        with NVLZ_GRIND, open(ra,'rb') as i, open(ut,'wb') as u:
            r = subprocess.run([NVGPU,'packa','--tyst'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvgpu: '+r.stderr.decode()[:300])
    elif kodek == 'zstd19':
        _kör(['zstd','-19','--long=27',f'-T{trådar}','-q','-f','-o',str(ut),str(ra)])
    elif kodek == 'zstd22':
        _kör(['zstd','-22','--ultra','--long=31',f'-T{trådar}','-q','-f','-o',str(ut),str(ra)])
    elif kodek == 'xz9e':
        with open(ut,'wb') as f:
            r = subprocess.run(['xz','-9e',f'-T{trådar}','-c',str(ra)], stdout=f, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('xz: '+r.stderr.decode()[:400])
    else:
        raise ValueError(kodek)
    return kodek


def packa_upp_block(packad: Path, ut: Path, kodek: str):
    if kodek == 'ra':
        os.replace(packad, ut)
    elif kodek in ('nvcm', 'nvz'):      # 'nvz' = arkiv från timmarna då kodeken hette så
        with NVCM_GRIND, open(packad,'rb') as i, open(ut,'wb') as u:
            r = subprocess.run([NVCM,'packaupp'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvcm: '+r.stderr.decode()[:300])
    elif kodek == 'nvlz':
        # Uppackningen är ren cpu. Ett arkiv ska inte vara gisslan hos en
        # grafikdrivrutin: den som packade hade en gpu, den som packar upp
        # kanske inte har någon.
        with open(packad,'rb') as i, open(ut,'wb') as u:
            r = subprocess.run([NVGPU,'packaupp'], stdin=i, stdout=u, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('nvgpu: '+r.stderr.decode()[:300])
    elif kodek.startswith('zstd'):
        fönster = '--long=31' if kodek == 'zstd22' else '--long=27'
        _kör(['zstd','-d',fönster,'-q','-f','-o',str(ut),str(packad)])
    elif kodek == 'xz9e':
        with open(ut,'wb') as f:
            r = subprocess.run(['xz','-dc','-T0',str(packad)], stdout=f, stderr=subprocess.PIPE)
            if r.returncode: raise RuntimeError('xz: '+r.stderr.decode()[:400])
    else:
        raise ValueError(kodek)


def storlek_av_prov(prov: Path) -> int:
    return prov.stat().st_size


def välj_kodek(ra: Path, med_cm=False, med_gpu=False) -> str:
    """Provar ett stickprov med varje kandidat och väljer den som vinner.

    Stickprovet tas på ÅTTA ställen spridda genom blocket, inte som de första
    megabyten. Ett block är sorterat efter ändelse, så de första megabyten är
    en enda filtyp och säger ingenting om resten — första mätningen av det här
    valet lät zstd vinna över xz på ett block där xz var 4 % bättre.
    """
    storlek = ra.stat().st_size
    if storlek == 0: return 'ra'
    with tempfile.TemporaryDirectory() as d:
        prov = Path(d)/'prov'
        delar, delstorlek = 8, PROVBYTE // 8
        with open(ra,'rb') as f, open(prov,'wb') as u:
            if storlek <= PROVBYTE:
                u.write(f.read())
            else:
                for i in range(delar):
                    f.seek(i * (storlek - delstorlek) // (delar - 1))
                    u.write(f.read(delstorlek))
        z, x = Path(d)/'z', Path(d)/'x'
        _kör(['zstd','-19','--long=27','-T1','-q','-f','-o',str(z),str(prov)])
        zb = z.stat().st_size
        with open(x,'wb') as f:
            subprocess.run(['xz','-9e','-T1','-c',str(prov)], stdout=f, stderr=subprocess.DEVNULL)
        xb = x.stat().st_size or zb
        cb = None
        if med_cm:
            with open(prov,'rb') as f:
                r = subprocess.run([NVCM,'packa'], stdin=f, capture_output=True)
            if r.returncode == 0: cb = len(r.stdout)
        gb = None
        if med_gpu and gpu_finns():
            with NVLZ_GRIND, open(prov,'rb') as f:
                r = subprocess.run([NVGPU,'packa','--tyst'], stdin=f, capture_output=True)
            if r.returncode == 0: gb = len(r.stdout)
        bäst = min(x for x in (zb, xb, cb, gb) if x is not None)

        # Tre utfall, inte två. Att bara skilja på "går att packa" och "går
        # inte" kostade 0,06 % på ett riktigt arkiv: 2,9 GB film packar sig
        # en halv procent, vilket är under tröskeln för att vara värt en dyr
        # kodek — men en halv procent av 2,9 GB är fjorton megabyte, och en
        # billig kodek tar dem på några sekunder. Dagens `tar | zstd -3` tog
        # dem; nvz slängde dem.
        if bäst > storlek_av_prov(prov) * 0.998:
            return 'ra'          # under en femtedels procent: inte värt en enda sekund
        if bäst > storlek_av_prov(prov) * 0.98:
            return 'zstd3'       # magert men nästan gratis att ta
        if cb is not None and cb < min(zb, xb) * 0.98:
            return 'nvcm'
        # nvlz behöver en marginal av samma skäl som nvcm: den är dyrare än
        # zstd och ska bara väljas när den tar hem något verkligt.
        if gb is not None and gb < min(zb, xb) * 0.98:
            return 'nvlz'
        # xz och zstd -19 tar ungefär lika lång tid på den här maskinen, så
        # marginalen behöver bara täcka stickprovets brus.
        return 'xz9e' if xb < zb * 0.995 else 'zstd19'


# ---------------------------------------------------------------------------
# Omvandlingar (körs i arbetsprocesser)
# ---------------------------------------------------------------------------

def _omvandla(uppgift):
    """(klass, sökväg) -> (omvandling, {'rest','kropp',...}) eller (None, None)."""
    klass, sokvag = uppgift
    try:
        if klass == 'png':
            r = png_omv.veckla_ut(sokvag, PNG_UPPBLASNING_TAK)
            return ('png', r) if r else (None, None)
        if klass == 'jpeg':
            r = jpeg_omv.veckla_ut(sokvag)
            return ('jpeg', r) if r else (None, None)
    except Exception:
        return (None, None)           # en trasig fil ska lagras, inte fälla arkivet
    return (None, None)


# ---------------------------------------------------------------------------
# Packning
# ---------------------------------------------------------------------------

def hasha(sokvag: Path, bit=1 << 20):
    """Returnerar (hash, första kilobyten). Huvudet används till klassningen,
    så filen behöver bara läsas en gång."""
    h = hashlib.blake2b(digest_size=16)
    huvud = b''
    with open(sokvag,'rb') as f:
        while d := f.read(bit):
            if not huvud: huvud = d[:4096]
            h.update(d)
    return h.hexdigest(), huvud


def läs_träd(rot: Path):
    """Går igenom trädet en gång och plockar fram poster i stabil ordning.

    Två fällor sitter här, och provsviten gick på båda:

    En SYMLÄNK TILL EN KATALOG dyker upp bland os.walks kataloger, inte bland
    dess filer. Letar man bara efter länkar bland filerna försvinner den helt
    ur arkivet — tyst, utan fel, och utan att något saknas förrän någon
    försöker använda det som packades upp.

    KATALOGERNAS EGEN TID är också data. Utan den får varje katalog
    uppackningsdagens datum, och ett arkiv som flyttar alla tidsstämplar till
    idag är inte en trogen kopia.
    """
    poster = []
    överhoppat = 0
    for katalog, kataloger, filer in os.walk(rot, followlinks=False):
        kataloger.sort(); filer.sort()
        k = Path(katalog)
        st = k.lstat()
        poster.append(dict(p=str(k.relative_to(rot)) if k != rot else '', t='d',
                           m=st.st_mode & 0o7777, mt=st.st_mtime_ns))
        länkade = [d for d in kataloger if (k/d).is_symlink()]
        for d in länkade:
            poster.append(dict(p=str((k/d).relative_to(rot)), t='l', mal=os.readlink(k/d),
                               mt=(k/d).lstat().st_mtime_ns))
        if länkade:
            kataloger[:] = [d for d in kataloger if d not in set(länkade)]
        for namn in filer:
            s = k/namn
            st = s.lstat()
            rel = str(s.relative_to(rot))
            if stat.S_ISLNK(st.st_mode):
                poster.append(dict(p=rel, t='l', mal=os.readlink(s), mt=st.st_mtime_ns))
            elif stat.S_ISREG(st.st_mode):
                poster.append(dict(p=rel, t='f', m=st.st_mode & 0o7777,
                                   mt=st.st_mtime_ns, s=st.st_size))
            else:
                överhoppat += 1        # rör, uttag, enheter — inget en fil-kopia äger
    if överhoppat:
        print(f'  varning: {överhoppat} specialfiler (rör/uttag/enheter) hoppades över',
              file=sys.stderr)
    return poster


def packa(rot: Path, arkiv: Path, *, läge='liten', dedup=True, omvandla=None, jobb=None,
          tyst=False, kodek=None, platt=False):
    t0 = time.monotonic()
    jobb = jobb or min(24, os.cpu_count() or 4)
    kodekar = LÄGEN[läge]
    if omvandla is None: omvandla = (läge != 'snabb')
    rot = rot.resolve()
    säg = (lambda *a: None) if tyst else (lambda *a: print(*a, flush=True))

    poster = läs_träd(rot)
    filer = [p for p in poster if p['t'] == 'f']
    råbyte = sum(p['s'] for p in filer)
    säg(f'  {len(poster)} poster, {råbyte/1048576:.1f} MiB, läge {läge}')

    # --- 1. Hasha och klassa ------------------------------------------------
    # Hashningen är I/O-bunden och blake2b släpper GIL:en, så trådar räcker.
    with fut.ThreadPoolExecutor(jobb) as pool:
        svar = list(pool.map(lambda p: hasha(rot/p['p']), filer))
    for p, (h, huvud) in zip(filer, svar):
        p['h'] = h
        p['klass'] = klassificera(rot/p['p'], huvud)

    # --- 2. Dedup -----------------------------------------------------------
    objekt = {}
    for p in filer:
        nyckel = p['h'] if dedup else f'{p["h"]}:{p["p"]}'
        o = objekt.get(nyckel)
        if o is None:
            o = objekt[nyckel] = dict(h=p['h'], s=p['s'], klass=p['klass'], kalla=p['p'],
                                      ändelse=Path(p['p']).suffix.lower(), id=len(objekt))
        p['o'] = o['id']
    sparat = råbyte - sum(o['s'] for o in objekt.values())
    if dedup and sparat:
        säg(f'  dedup: {len(filer)-len(objekt)} dubbletter, {sparat/1048576:.1f} MiB')

    arbete = Path(tempfile.mkdtemp(prefix='nvz-', dir=arkiv.parent))
    try:
        # --- 3. Sortera ----------------------------------------------------
        # Sorteringen är hela poängen med punkt 2 i modulhuvudet: klass först,
        # sedan ändelse, sedan sökväg. Filer som liknar varandra hamnar efter
        # varandra i strömmen, och de omvandlade följer med sin klass.
        lista = sorted(objekt.values(), key=lambda o: (o['klass'], o['ändelse'], o['kalla']))
        # Numrera om objekten efter sorteringen. Registret lagrar dem som
        # parallella listor, och då MÅSTE ett objekts nummer vara dess plats i
        # listan — annars pekar varje post på fel objekt. (Första versionen
        # gjorde inte det, och gav ett arkiv där varje fil fick någon annans
        # innehåll. Felet syns inte i packningen, bara i uppackningen.)
        nytt = {o['id']: i for i, o in enumerate(lista)}
        for o in lista: o['id'] = nytt[o['id']]
        for p in filer: p['o'] = nytt[p['o']]

        block = []
        öppet = {}

        def nytt_block(klass):
            b = dict(i=len(block), klass=klass, kodek=kodekar.get(klass,'prova'),
                     ström=arbete/f'b{len(block)}', rålen=0)
            b['f'] = open(b['ström'],'wb')
            block.append(b)
            return b

        def lägg(klass, data: bytes):
            b = öppet.get(klass)
            if b is None or b['rålen'] >= BLOCKTAK:
                if b is not None: b['f'].close(); b.pop('f')
                b = öppet[klass] = nytt_block(klass)
            off = b['rålen']; b['f'].write(data); b['rålen'] += len(data)
            return b['i'], off, len(data)

        # --- 4. Omvandla och lägg i block -----------------------------------
        # pool.map håller ordningen, så sorteringen ovan överlever även när
        # svaren kommer tillbaka huller om buller. buffersize håller nere
        # minnet: en utvecklad PNG kan vara hundra gånger sin filstorlek, och
        # 24 sådana i luften samtidigt räcker för att fylla maskinen.
        uppgifter = [(o['klass'] if omvandla else '', str(rot/o['kalla'])) for o in lista]
        räknare = {'png': 0, 'jpeg': 0}
        with fut.ProcessPoolExecutor(jobb) as pool:
            for o, (tr, r) in zip(lista, pool.map(_omvandla, uppgifter, chunksize=1,
                                                  buffersize=jobb)):
                if r:
                    räknare[tr] += 1
                    o['tr'] = tr
                    if tr == 'png': o['recept'] = r['recept']
                    o['b_rest'] = lägg(f'{tr}-rest', r['rest'])
                    o['b'] = lägg('png-ra' if tr == 'png' else 'jpeg-kropp', r['kropp'])
                else:
                    with open(rot/o['kalla'],'rb') as f:
                        o['b'] = lägg(o['klass'], f.read())
        if omvandla:
            antal = {k: sum(1 for o in lista if o['klass'] == k) for k in ('png','jpeg')}
            for k in ('png','jpeg'):
                if antal[k]:
                    säg(f'  {k}: {räknare[k]}/{antal[k]} omvandlade')
        for b in öppet.values():
            if 'f' in b: b['f'].close(); b.pop('f')
        säg(f'  {len(block)} block')

        # --- 5. Packa blocken parallellt ------------------------------------
        if kodek:
            for b in block:
                if b['kodek'] != 'ra': b['kodek'] = kodek
        trådar = max(1, jobb // max(1, min(len(block), jobb)))

        def packa_ett(b):
            ut = arbete/f'c{b["i"]}'
            b['kodek'] = packa_block(b['ström'], ut, b['kodek'], trådar)
            b['packad'] = ut
            b['ström'].unlink(missing_ok=True)
        with fut.ThreadPoolExecutor(min(jobb, max(1,len(block)))) as pool:
            list(pool.map(packa_ett, block))

        # --- 6. Skriv arkivet ------------------------------------------------
        with open(arkiv,'wb') as ut:
            ut.write(MAGI)
            for b in block:
                b['off'] = ut.tell()
                with open(b['packad'],'rb') as f: shutil.copyfileobj(f, ut, 1<<20)
                b['clen'] = ut.tell() - b['off']
                b['packad'].unlink()
            # Registret ligger i parallella listor, inte i en lista av
            # ordböcker. Samma uppgifter, men nyckelnamnen står en gång i
            # stället för en gång per objekt, och tal av samma slag hamnar
            # intill varandra där de packar bra. På ett träd med 6 000 små
            # filer är skillnaden ett par procent av HELA arkivet.
            #
            # Offseten i blocket lagras inte alls utan räknas fram: objekten
            # lades i blocken i exakt den här ordningen, så en löpande summa
            # av längderna ger samma tal. Att den gör det kontrolleras nedan —
            # ett register som pekar en byte fel är ett oläsbart arkiv.
            register = dict(
                version=VERSION, rot=rot.name, platt=platt,
                skapad=time.strftime('%Y-%m-%dT%H:%M:%S'),
                block=[dict(i=b['i'], kodek=b['kodek'], off=b['off'], clen=b['clen'],
                            rålen=b['rålen'], klass=b['klass']) for b in block],
                objekt=dict(
                    blk=[o['b'][0] for o in lista],
                    len=[o['b'][2] for o in lista],
                    s=[o['s'] for o in lista],
                    h=[o['h'][:16] for o in lista],   # 64 bitar räcker för kontroll
                    tr={str(o['id']): [o['tr'], o['b_rest'][0], o['b_rest'][2],
                                       o.get('recept')] for o in lista if 'tr' in o},
                ),
                poster=[{k:v for k,v in p.items() if k in ('p','t','m','mt','mal','o')}
                        for p in poster],
            )
            kontrollera_offsetar(register, lista)
            rad = json.dumps(register, ensure_ascii=False).encode()
            regoff = ut.tell()
            ut.write(_kör(['zstd','-19','-q','-c'], stdin=rad))
            ut.write(struct.pack('<QQ', regoff, len(rad)))
    finally:
        shutil.rmtree(arbete, ignore_errors=True)

    byte = arkiv.stat().st_size
    säg(f'  klart: {byte/1048576:.2f} MiB (x{råbyte/max(byte,1):.2f}) på {time.monotonic()-t0:.1f}s')
    return byte


# ---------------------------------------------------------------------------
# Uppackning
# ---------------------------------------------------------------------------

def härled_offsetar(reg):
    """Räknar fram varje objekts plats i sitt block ur längderna.

    Ordningen är densamma som vid packning: för ett omvandlat objekt lades
    först resten, sedan kroppen.
    """
    o = reg['objekt']
    löpande = {}
    ut = []
    for i, (blk, ln) in enumerate(zip(o['blk'], o['len'])):
        post = o['tr'].get(str(i))
        if post:
            rblk, rlen = post[1], post[2]
            roff = löpande.get(rblk, 0); löpande[rblk] = roff + rlen
        off = löpande.get(blk, 0); löpande[blk] = off + ln
        ut.append(((blk, off, ln), (post[1], roff, post[2]) if post else None))
    return ut


def kontrollera_offsetar(register, lista):
    """Bevisar att de härledda platserna är exakt de som användes."""
    for härledd, o in zip(härled_offsetar(register), lista):
        if tuple(o['b']) != härledd[0] or ('tr' in o) != (härledd[1] is not None) \
           or ('tr' in o and tuple(o['b_rest']) != härledd[1]):
            raise RuntimeError(f'registret skulle peka fel för {o["kalla"]}')


def läs_register(arkiv: Path):
    with open(arkiv,'rb') as f:
        if f.read(len(MAGI)) != MAGI: raise SystemExit('Inte ett nvz-arkiv.')
        f.seek(-16, os.SEEK_END)
        regoff, _rålen = struct.unpack('<QQ', f.read(16))
        f.seek(regoff)
        packat = f.read(os.path.getsize(arkiv) - 16 - regoff)
    return json.loads(_kör(['zstd','-d','-q','-c'], stdin=packat).decode())


def packa_upp(arkiv: Path, mål: Path, *, jobb=None, tyst=False):
    t0 = time.monotonic()
    jobb = jobb or min(24, os.cpu_count() or 4)
    säg = (lambda *a: None) if tyst else (lambda *a: print(*a, flush=True))
    reg = läs_register(arkiv)
    # Ett PLATT arkiv är ett arkiv av en ensam fil (gzip-ytan packar så). Dess
    # poster ska ligga direkt i målet, inte i en katalog med trädets namn —
    # och rotpostens rättigheter och tid hör till den tillfälliga katalog som
    # filen packades genom, så de får inte klistras på målkatalogen.
    platt = bool(reg.get('platt'))
    rot = mål if platt else mål/reg['rot']
    rot.mkdir(parents=True, exist_ok=True)

    arbete = Path(tempfile.mkdtemp(prefix='nvz-upp-', dir=mål))
    try:
        # Blocken packas upp parallellt till arbetskatalogen. Att hålla dem på
        # disk i stället för i minnet är med flit: ett arkiv kan vara större än
        # maskinens RAM, och då ska uppackningen inte vara det som stoppar.
        def upp(b):
            packad = arbete/f'c{b["i"]}'
            with open(arkiv,'rb') as f:
                f.seek(b['off']); kvar = b['clen']
                with open(packad,'wb') as u:
                    while kvar:
                        d = f.read(min(1<<20, kvar))
                        u.write(d); kvar -= len(d)
            ut = arbete/f'b{b["i"]}'
            packa_upp_block(packad, ut, b['kodek'])
            packad.unlink(missing_ok=True)
            return b['i'], ut
        with fut.ThreadPoolExecutor(min(jobb, max(1,len(reg['block'])))) as pool:
            strömmar = dict(pool.map(upp, reg['block']))

        # Kataloger först, i trädordning, så att filerna har någonstans att ta vägen.
        for p in reg['poster']:
            if p['t'] == 'd': (rot/p['p']).mkdir(parents=True, exist_ok=True)
        for p in reg['poster']:
            if p['t'] == 'l':
                s = rot/p['p']; s.parent.mkdir(parents=True, exist_ok=True)
                s.unlink(missing_ok=True); os.symlink(p['mal'], s)
                # Länkens EGEN tid, inte målets. utime utan detta följer länken
                # och ändrar en helt annan fils tidsstämpel.
                if 'mt' in p: os.utime(s, ns=(p['mt'], p['mt']), follow_symlinks=False)

        vägar = {}
        for p in reg['poster']:
            if p['t'] == 'f': vägar.setdefault(p['o'], []).append(p)
        platser = härled_offsetar(reg)

        # os.pread i stället för seek+read: flera trådar läser ur samma block
        # samtidigt, och en delad filposition hade gett dem varandras bytes.
        fdar = {i: os.open(s, os.O_RDONLY) for i, s in strömmar.items()}

        def bygg(nr):
            (bi, off, ln), rest_plats = platser[nr]
            kropp = os.pread(fdar[bi], ln, off)
            if rest_plats:
                tr, _, _, recept = reg['objekt']['tr'][str(nr)]
                bi2, off2, ln2 = rest_plats
                rest = os.pread(fdar[bi2], ln2, off2)
                data = (png_omv.återskapa(rest, kropp, recept) if tr == 'png'
                        else jpeg_omv.återskapa(rest, kropp))
            else:
                data = kropp
            for p in vägar.get(nr, ()):
                s = rot/p['p']
                with open(s,'wb') as u: u.write(data)
                os.chmod(s, p['m']); os.utime(s, ns=(p['mt'], p['mt']))

        with fut.ThreadPoolExecutor(jobb) as pool:
            list(pool.map(bygg, sorted(vägar)))
        for fd in fdar.values(): os.close(fd)

        # Katalogerna sist, och djupast först: att skriva en fil i en katalog
        # ändrar katalogens tid, så den måste sättas efter att allt i den är
        # på plats.
        for p in reversed(reg['poster']):
            if p['t'] == 'd':
                if platt and p['p'] == '': continue
                s = rot/p['p']
                os.chmod(s, p['m'])
                if 'mt' in p: os.utime(s, ns=(p['mt'], p['mt']))
    finally:
        shutil.rmtree(arbete, ignore_errors=True)
    säg(f'  uppackat på {time.monotonic()-t0:.1f}s')


# ---------------------------------------------------------------------------

def lista(arkiv: Path):
    reg = läs_register(arkiv)
    print(f"nvz v{reg['version']}  rot={reg['rot']}  skapad {reg['skapad']}")
    omv = {}
    for v in reg['objekt']['tr'].values(): omv[v[0]] = omv.get(v[0], 0) + 1
    print(f"{len(reg['poster'])} poster, {len(reg['objekt']['blk'])} objekt, "
          f"{len(reg['block'])} block" + (f", omvandlade: {omv}" if omv else ""))
    for b in reg['block']:
        print(f"  block {b['i']:>3}  {b['klass']:<11} {b['kodek']:<7} "
              f"{b['rålen']/1048576:>9.2f} -> {b['clen']/1048576:>8.2f} MiB  "
              f"x{b['rålen']/max(b['clen'],1):.2f}")


def prova(rot: Path) -> bool:
    """Packar, packar upp och jämför. En arkivpackare som inte kan bevisa att
    den ger tillbaka exakt samma bytes är en förlustkomprimerare."""
    rot = rot.resolve()
    with tempfile.TemporaryDirectory(prefix='nvz-prov-') as d:
        d = Path(d); ark = d/'prov.nv'
        packa(rot, ark)
        packa_upp(ark, d/'upp')
        fel = 0
        for p in rot.rglob('*'):
            if p.is_symlink():
                b = d/'upp'/rot.name/p.relative_to(rot)
                if not b.is_symlink() or os.readlink(b) != os.readlink(p):
                    print('LÄNK SKILJER:', p); fel += 1
                continue
            if not p.is_file(): continue
            b = d/'upp'/rot.name/p.relative_to(rot)
            if not b.exists() or b.read_bytes() != p.read_bytes():
                print('SKILJER:', p); fel += 1
            elif b.stat().st_mode != p.stat().st_mode or b.stat().st_mtime_ns != p.stat().st_mtime_ns:
                print('METADATA SKILJER:', p); fel += 1
        print('PROV MISSLYCKADES' if fel else 'Prov ok: varje fil bit för bit identisk.')
        return fel == 0


# ---------------------------------------------------------------------------
# Enfilsarkiv
#
# gzip-ytan packar en fil i taget, och packa() går igenom ett katalogträd. En
# ensam fil är inget träd, så den packas genom ett träd om en fil: en
# tillfällig katalog med en HÅRDLÄNK till filen. En hårdlänk är samma inod —
# innehåll, rättigheter och tid är filens egna, ingenting kopieras, och på ett
# stort arkiv kostar det noll byte och noll sekunder. Registret märks 'platt'
# så att uppackningen vet att posten ska läggas direkt i målet.
# ---------------------------------------------------------------------------

def packa_fil(fil: Path, arkiv: Path, *, namn=None, **kw) -> int:
    with tempfile.TemporaryDirectory(prefix='nvz-en-', dir=arkiv.parent) as d:
        mellan = Path(d)/(namn or fil.name)
        try:
            os.link(fil, mellan)
        except OSError:
            shutil.copy2(fil, mellan)   # annan filsystemsgräns: då får det kosta
        return packa(Path(d), arkiv, platt=True, **kw)


def namn_i_arkiv(reg) -> str | None:
    """Namnet filen hade när den packades, eller None om arkivet är ett träd."""
    if not reg.get('platt'): return None
    for p in reg['poster']:
        if p['t'] == 'f': return p['p']
    return None


def packa_upp_fil(arkiv: Path, mål: Path, *, jobb=None) -> None:
    """Packar upp ett platt enfilsarkiv till exakt sökvägen mål.

    Filen packas upp i en tillfällig katalog på samma filsystem och FLYTTAS
    sedan på plats. En flytt inom ett filsystem byter bara namn på inoden, så
    rättigheter och tid som packa_upp redan satt följer med oförändrade — och
    målet finns antingen inte alls eller färdigt, aldrig halvskrivet.
    """
    mål.parent.mkdir(parents=True, exist_ok=True)
    arbete = Path(tempfile.mkdtemp(prefix='nvz-en-', dir=mål.parent))
    try:
        packa_upp(arkiv, arbete, jobb=jobb, tyst=True)
        inne = [x for x in arbete.rglob('*') if x.is_file() or x.is_symlink()]
        if len(inne) != 1:
            raise RuntimeError(f'{arkiv}: platt arkiv med {len(inne)} filer')
        os.replace(inne[0], mål)
    finally:
        shutil.rmtree(arbete, ignore_errors=True)


def testa(arkiv: Path, *, jobb=None) -> list:
    """Packar upp arkivet och jämför varje fil mot hashen i registret.

    Registret bär blake2b-prefixet för varje objekt, taget på originalet före
    packningen. Att packa upp och hasha om är därför ett riktigt prov på hela
    kedjan — block, kodek, PNG-recept och JPEG-ompackning — och inte bara en
    kontrollsumma på arkivfilen.
    """
    reg = läs_register(arkiv)
    with tempfile.TemporaryDirectory(prefix='nvz-prov-') as d:
        d = Path(d)
        packa_upp(arkiv, d, jobb=jobb, tyst=True)
        rot = d if reg.get('platt') else d/reg['rot']
        fel = []
        for p in reg['poster']:
            s = rot/p['p']
            if p['t'] == 'f':
                if not s.is_file() or s.is_symlink(): fel.append((p['p'], 'saknas'))
                elif hasha(s)[0][:16] != reg['objekt']['h'][p['o']]:
                    fel.append((p['p'], 'innehållet skiljer'))
            elif p['t'] == 'l':
                if not s.is_symlink() or os.readlink(s) != p['mal']:
                    fel.append((p['p'], 'länken skiljer'))
            elif p['t'] == 'd' and not s.is_dir():
                fel.append((p['p'] or '.', 'katalog saknas'))
    return fel


def uppackad_storlek(reg) -> int:
    """Byte som uppackningen skriver — före dedup, alltså det man får tillbaka."""
    s = reg['objekt']['s']
    return sum(s[p['o']] for p in reg['poster'] if p['t'] == 'f')


# ---------------------------------------------------------------------------
# Kommandoraden
#
# Ytan är gzips, för att gzips yta är den man redan har i fingrarna:
#
#     nvz fil            -> fil.nv, originalet borta
#     nvz -d fil.nv      -> fil, arkivet borta
#     nvz -c fil > ut.nv   nvz -l ark.nv   nvz -t ark.nv
#     nvz -9 -k katalog    cat fil | nvz > fil.nv
#
# EN avvikelse, och den är med flit: gzip säger "is a directory -- ignored"
# om en katalog, men en katalog är precis vad nvz är byggd för. En katalog
# som operand packas därför till ETT arkiv av hela trädet, med dedup och
# sortering mellan filerna — och källträdet lämnas alltid kvar, för nvz
# ska inte kunna radera ett träd. Vill man ha gzips bokstavliga beteende
# finns -r: då packas varje fil för sig, i katalogen, precis som gzip gör,
# och då går vinsten MELLAN filerna förlorad.
#
# Underkommandona (packa, packa-upp, lista, prova) finns kvar oförändrade.
# Mätningarna i matning/ och proven i prov/ anropar dem, och ett byte av yta
# får inte göra gamla siffror omätbara.
# ---------------------------------------------------------------------------

SUFFIX = '.nv'

# gzips nio siffror mot nvpacks tre lägen. -6 är gzips standard och normal är
# nvpacks, så mitten ska vara mitten.
NIVÅ_LÄGE = {1:'snabb', 2:'snabb', 3:'snabb', 4:'normal', 5:'normal',
             6:'normal', 7:'liten', 8:'liten', 9:'liten'}

UNDERKOMMANDON = ('packa', 'packa-upp', 'lista', 'prova')

# Långa flaggor -> den korta de betyder, eller ett eget namn för dem som bara
# nvz har. Tomma strängen = tas emot och gör ingenting (gzip har dem, och
# ett skript som skickar dem ska inte stupa).
LÅNGA = {
    'stdout':'c', 'to-stdout':'c', 'decompress':'d', 'uncompress':'d',
    'force':'f', 'help':'h', 'keep':'k', 'list':'l', 'license':'L',
    'no-name':'n', 'name':'N', 'quiet':'q', 'silent':'q', 'recursive':'r',
    'suffix':'S', 'test':'t', 'verbose':'v', 'version':'V',
    'fast':'1', 'best':'9',
    'synchronous':'', 'rsyncable':'', 'ascii':'', 'no-time':'',
    'läge':'LÄGE', 'lage':'LÄGE', 'kodek':'KODEK', 'jobb':'J',
    'utan-dedup':'DEDUP', 'utan-omvandling':'OMV', 'tyst':'q',
}
KORTA = 'acdfhklLnNqrtvV'          # utan argument
KORTA_MED_ARG = {'S':'S', 'j':'J'}

HJÄLP = f"""Användning: nvz [FLAGGA]... [FIL]...
Packar och packar upp filer och katalogträd. Utan flaggor packas varje FIL
och originalet ersätts, precis som gzip.

  -c, --stdout       skriv till standard ut, lämna filerna i fred
  -d, --decompress   packa upp
  -f, --force        skriv över mål, följ länkar, packa om redan packat
  -h, --help         den här hjälpen
  -k, --keep         behåll filen som packades
  -l, --list         visa vad arkivet innehåller
  -L, --license      villkoren
  -n, --no-name      vid uppackning: härled namnet ur arkivets filnamn (standard)
  -N, --name         vid uppackning: använd namnet som står i arkivet
  -q, --quiet         säg ingenting
  -r, --recursive    gå ner i kataloger och packa varje fil för sig
  -S, --suffix=.SUF  ändelse på arkiven (standard {SUFFIX})
  -t, --test          prova arkivet: packa upp och jämför mot registret
  -v, --verbose      berätta vad som händer (-vv visar packningen i detalj)
  -V, --version      version
  -1 .. -9           snabbt .. litet: -1..-3 läge snabb, -4..-6 normal, -7..-9 liten
                     (utan siffra: liten, alltså det minsta nvz kan)
      --fast, --best samma som -1 och -9

Bara nvz har de här:
  -j, --jobb=N       så många parallella jobb
      --läge=L       snabb | normal | liten (slår -1..-9; standard liten)
      --kodek=K      tvinga zstd3 | zstd19 | zstd22 | xz9e | nvcm | nvlz
      --utan-dedup, --utan-omvandling

En KATALOG som operand packas till ETT arkiv av hela trädet — det är vad
nvz är till för, och det är enda stället där ytan skiljer sig från gzip.
Källträdet lämnas alltid kvar; nvz raderar aldrig en katalog. Med -r görs
i stället precis vad gzip gör, en fil i taget, och då förloras dedupen och
sorteringen MELLAN filerna, som är större delen av vinsten.

Utan FIL (eller med "-") läses standard in och skrivs standard ut.
Underkommandona packa, packa-upp, lista och prova finns kvar och fungerar.
Har du en FIL som heter som ett underkommando: skriv nvz -- packa.

Slutstatus: 0 allt väl, 1 fel, 2 varning."""

VILLKOR = """nvz — innehållsmedveten arkivpackare.
Skrivet av Hugo Wikström. Fritt att använda, ändra och sprida.
Ingen garanti: varje omvandling provas i båda riktningarna vid packning, men
provet är kodens eget ord, inte ett löfte."""


def _fel(text) -> int:
    print(f'nvz: {text}', file=sys.stderr)
    return 1


def _varna(o, text) -> int:
    if not o['q']: print(f'nvz: {text}', file=sys.stderr)
    return 2


def _lång(namn: str):
    """Löser ut en lång flagga, och godtar förkortningar som gzip gör."""
    if namn in LÅNGA: return LÅNGA[namn]
    träff = [k for k in LÅNGA if k.startswith(namn)]
    if not träff: raise SystemExit(_fel(f"okänd flagga -- '{namn}'"))
    if len({LÅNGA[k] for k in träff}) > 1:
        raise SystemExit(_fel(f"tvetydig flagga -- '{namn}' ({', '.join(sorted(träff))})"))
    return LÅNGA[träff[0]]


def tolka(argv):
    o = dict(c=False, d=False, f=False, k=False, l=False, n=False, N=False,
             q=False, r=False, t=False, v=0, suffix=SUFFIX, läge=None,
             kodek=None, jobb=None, dedup=True, omvandla=None)
    filer, bara_filer, i = [], False, 0

    def tillämpa(h, värde):
        """Sätter en flagga. Returnerar True om nästa argv-post åts upp."""
        if h == '':  return False
        if h in '123456789': o['läge'] = NIVÅ_LÄGE[int(h)]; return False
        if h == 'v': o['v'] += 1; return False
        if h == 'q': o['q'] = True; o['v'] = 0; return False
        if h == 'a': return False
        if h == 'h': print(HJÄLP); raise SystemExit(0)
        if h == 'V': print(f'nvz {VERSION}.0'); raise SystemExit(0)
        if h == 'L': print(VILLKOR); raise SystemExit(0)
        if h == 'DEDUP': o['dedup'] = False; return False
        if h == 'OMV': o['omvandla'] = False; return False
        if h in ('S', 'J', 'LÄGE', 'KODEK'):
            åt = False
            if värde is None:
                if i >= len(argv): raise SystemExit(_fel(f'flaggan saknar värde'))
                värde = argv[i]; åt = True
            if h == 'S':
                if not värde.strip('.'): raise SystemExit(_fel(f"ogiltig ändelse '{värde}'"))
                o['suffix'] = värde if värde.startswith('.') else '.' + värde
            elif h == 'J':
                if not värde.isdigit(): raise SystemExit(_fel(f"-j tar ett tal, inte '{värde}'"))
                o['jobb'] = max(1, int(värde))
            elif h == 'LÄGE':
                if värde not in LÄGEN:
                    raise SystemExit(_fel(f"okänt läge '{värde}' (välj {'|'.join(LÄGEN)})"))
                o['läge'] = värde
            else:
                if värde not in ('zstd3','zstd19','zstd22','xz9e','nvcm','nvlz'):
                    raise SystemExit(_fel(f"okänd kodek '{värde}'"))
                o['kodek'] = värde
            return åt
        o[h] = True
        return False

    while i < len(argv):
        a = argv[i]; i += 1
        if bara_filer or a == '-' or not a.startswith('-'):
            filer.append(a); continue
        if a == '--':
            bara_filer = True; continue
        if a.startswith('--'):
            namn, värde = a[2:], None
            if '=' in namn: namn, värde = namn.split('=', 1)
            if tillämpa(_lång(namn), värde): i += 1
            continue
        j = 1
        while j < len(a):
            ch = a[j]; j += 1
            if ch in KORTA_MED_ARG:
                värde = a[j:] or None; j = len(a)
                if tillämpa(KORTA_MED_ARG[ch], värde): i += 1
            elif ch in '123456789' or ch in KORTA:
                tillämpa(ch, None)
            else:
                raise SystemExit(_fel(f"okänd flagga -- '{ch}'\nFörsök med nvz --help."))
    return o, filer


def _packkw(o):
    return dict(läge=o['läge'] or 'normal', dedup=o['dedup'], omvandla=o['omvandla'],
                jobb=o['jobb'], kodek=o['kodek'], tyst=o['v'] < 2)


def _register(o, p: Path):
    try:
        return läs_register(p)
    except SystemExit as e:
        _varna(o, f'{p}: {e}' if str(e) else f'{p}: inte ett nvz-arkiv'); return None
    except Exception as e:
        _varna(o, f'{p}: oläsbart arkiv ({e})'); return None


def _får_skriva(o, mål: Path) -> bool:
    """gzips regel: finns målet redan frågar vi vid en terminal, annars låter
    vi filen vara. Ett svep som packar tusen filer ska inte kunna skriva över
    något bara för att ingen satt och tittade."""
    if o['f'] or not os.path.lexists(mål): return True
    if sys.stdin.isatty() and sys.stderr.isatty():
        sys.stderr.write(f'nvz: {mål} finns redan; skriva över (j/n)? ')
        sys.stderr.flush()
        if sys.stdin.readline().strip().lower()[:1] in ('j', 'y'): return True
        sys.stderr.write('\tej överskrivet\n')
        return False
    _varna(o, f'{mål} finns redan -- oförändrat')
    return False


def _till_stdout(p: Path):
    with open(p, 'rb') as f:
        shutil.copyfileobj(f, sys.stdout.buffer, 1 << 20)
    sys.stdout.buffer.flush()


def _kvot(o, från: str, rå: int, packat: int, till, ersatt: bool):
    if o['v'] < 1 or o['q']: return
    del_ = (1 - packat/rå) * 100 if rå else 0.0
    vad = 'ersatt med' if ersatt else 'skrivet till'
    print(f'{från}:\t{del_:5.1f}% -- {vad} {till}', file=sys.stderr)


def _trädstorlek(rot: Path) -> int:
    tot = 0
    for k, _, filer in os.walk(rot, followlinks=False):
        for n in filer:
            try: tot += os.lstat(os.path.join(k, n)).st_size
            except OSError: pass
    return tot


# --- packa -----------------------------------------------------------------

def _packa_fil(o, p: Path) -> int:
    if p.name.endswith(o['suffix']) and not o['f']:
        return _varna(o, f'{p} har redan ändelsen {o["suffix"]} -- oförändrad')
    rå = p.stat().st_size
    if o['c']:
        with tempfile.TemporaryDirectory(prefix='nvz-ut-') as d:
            ark = Path(d)/'ut.nv'
            packa_fil(p, ark, **_packkw(o))
            _kvot(o, str(p), rå, ark.stat().st_size, 'standard ut', False)
            _till_stdout(ark)
        return 0
    ut = Path(str(p) + o['suffix'])
    if not _får_skriva(o, ut): return 2
    packa_fil(p, ut, **_packkw(o))
    st = p.stat()
    # Arkivet ärver filens tid och rättigheter. gzip gör det, och det är vad
    # som gör en packad katalog läsbar som en katalog: tiderna är kvar.
    os.chmod(ut, st.st_mode & 0o7777)
    os.utime(ut, ns=(st.st_atime_ns, st.st_mtime_ns))
    if not o['k']: p.unlink()
    _kvot(o, str(p), rå, ut.stat().st_size, ut, not o['k'])
    return 0


def _packa_träd(o, p: Path) -> int:
    rå = _trädstorlek(p) if o['v'] else 0
    if o['c']:
        with tempfile.TemporaryDirectory(prefix='nvz-ut-') as d:
            ark = Path(d)/'ut.nv'
            packa(p, ark, **_packkw(o))
            _kvot(o, str(p), rå, ark.stat().st_size, 'standard ut', False)
            _till_stdout(ark)
        return 0
    ut = Path(str(p) + o['suffix'])
    if not _får_skriva(o, ut): return 2
    packa(p, ut, **_packkw(o))
    _kvot(o, str(p), rå, ut.stat().st_size, ut, False)
    return 0


def _utvidga(o, filer, vill_ha_arkiv: bool):
    """Räknar fram vad som faktiskt ska behandlas.

    Med -r byts en katalog mot filerna i den — gzips beteende, en fil i taget.
    Utan -r står katalogen kvar som sin egen operand, och packningen gör då
    ETT arkiv av trädet.

    I svepet hoppas filer med fel ändelse över UTAN ett ord: gzip klagar på
    var och en, men ett svep är något man kör om, och ett `nvz -r` som
    skäller en rad per redan packad fil är obrukbart. Namnger man en fil
    själv får man däremot varningen — då var det ju filen man menade.
    """
    ut = []
    for f in filer:
        p = Path(f)
        if o['r'] and p.is_dir() and not p.is_symlink():
            for katalog, kataloger, namn in os.walk(p, followlinks=False):
                kataloger.sort(); namn.sort()
                for n in namn:
                    s = Path(katalog)/n
                    if s.is_symlink(): continue
                    if n.endswith(o['suffix']) != vill_ha_arkiv: continue
                    ut.append(s)
        else:
            ut.append(p)
    return ut


def _packa_operand(o, p: Path) -> int:
    if not os.path.lexists(p):
        return _fel(f'{p}: finns inte')
    if p.is_symlink():
        if not o['f']: return _varna(o, f'{p} är en symlänk -- hoppas över')
        p = Path(os.path.realpath(p))
    if p.is_dir():
        return _packa_träd(o, p)
    if not p.is_file():
        return _varna(o, f'{p} är inte en vanlig fil -- hoppas över')
    länkar = p.stat().st_nlink
    if länkar > 1 and not (o['f'] or o['k'] or o['c']):
        return _varna(o, f'{p} har {länkar-1} annan länk -- oförändrad')
    return _packa_fil(o, p)


# --- packa upp -------------------------------------------------------------

def _mål_för(o, p: Path, reg) -> Path | None:
    """Vilket namn uppackningen ska få. Som gzip: härlett ur arkivets filnamn,
    eller det som står i arkivet om -N är satt."""
    lagrat = namn_i_arkiv(reg)
    if o['N'] and lagrat: return p.parent/lagrat
    if p.name.endswith(o['suffix']):
        return p.parent/p.name[:-len(o['suffix'])]
    if lagrat: return p.parent/lagrat
    return None


def _packa_upp_operand(o, p: Path) -> int:
    if not p.is_file():
        # `nvz -d fil` hittar fil.nv, precis som gunzip hittar fil.gz.
        med = Path(str(p) + o['suffix'])
        if os.path.lexists(p) or not med.is_file():
            return _fel(f'{p}: finns inte')
        p = med
    # Ändelsen avgör FÖRE registret läses: en fil som inte heter .nv är inte
    # vår att packa upp, och det är en varning (status 2) som i gzip — inte
    # ett fel om formatet.
    if not p.name.endswith(o['suffix']) and not (o['c'] or o['f']):
        return _varna(o, f'{p}: okänd ändelse -- hoppas över')
    reg = _register(o, p)
    if reg is None: return 1
    if reg.get('platt'):
        mål = _mål_för(o, p, reg)
        if mål is None:
            return _varna(o, f'{p}: okänd ändelse -- hoppas över')
        if o['c']:
            with tempfile.TemporaryDirectory(prefix='nvz-ut-') as d:
                tmp = Path(d)/'f'
                packa_upp_fil(p, tmp, jobb=o['jobb'])
                _till_stdout(tmp)
            return 0
        if mål.resolve() == p.resolve():
            return _fel(f'{p}: namnet i arkivet är arkivets eget namn, '
                        'uppackningen skulle skriva över det')
        if not _får_skriva(o, mål): return 2
        packa_upp_fil(p, mål, jobb=o['jobb'])
        _kvot(o, str(p), uppackad_storlek(reg), p.stat().st_size, mål, not o['k'])
        if not o['k']: p.unlink()
        return 0
    # Ett träd. Det går inte att hälla ut i en byteström, och det ska inte
    # heller gissas fram — ett arkiv av tusen filer är inte en fil.
    if o['c']:
        return _fel(f'{p}: arkivet innehåller ett träd ({reg["rot"]}), '
                    'det kan inte skrivas till standard ut')
    rot = p.parent/reg['rot']
    if not _får_skriva(o, rot): return 2
    packa_upp(p, p.parent, jobb=o['jobb'], tyst=o['v'] < 2)
    _kvot(o, str(p), uppackad_storlek(reg), p.stat().st_size, rot, not o['k'])
    if not o['k']: p.unlink()
    return 0


# --- lista och prova -------------------------------------------------------

def _lista_operand(o, p: Path, summa: dict) -> int:
    reg = _register(o, p)
    if reg is None: return 1
    c = p.stat().st_size
    u = uppackad_storlek(reg)
    namn = namn_i_arkiv(reg) or reg['rot'] + '/'
    print(f'{c:>13} {u:>13} {(1-c/max(u,1))*100:6.1f}%  {namn}')
    summa['c'] += c; summa['u'] += u; summa['n'] += 1
    if o['v']:
        lista(p)
    return 0


def _testa_operand(o, p: Path) -> int:
    reg = _register(o, p)
    if reg is None: return 1
    try:
        fel = testa(p, jobb=o['jobb'])
    except Exception as e:
        return _fel(f'{p}: provet gick inte att köra ({e})')
    if fel:
        for väg, vad in fel[:20]:
            print(f'nvz: {p}: {väg}: {vad}', file=sys.stderr)
        if len(fel) > 20:
            print(f'nvz: {p}: ... och {len(fel)-20} till', file=sys.stderr)
        return _fel(f'{p}: PROVET MISSLYCKADES ({len(fel)} poster)')
    if o['v']: print(f'{p}:\tOK')
    return 0


# --- standard in och ut ----------------------------------------------------

def _ström(o) -> int:
    """Utan filoperand: standard in -> standard ut, som gzip.

    Arkivet måste kunna sökas i (registret ligger sist och läses först), så
    strömmen landar i en tillfällig fil på vägen. Det är skillnaden mot gzip,
    som verkligen strömmar — och skälet är formatet, inte lathet: ett arkiv
    med register och block är inte en följd byte man kan läsa framifrån.
    """
    if not o['d'] and sys.stdout.isatty() and not o['f']:
        return _fel('packade data skrivs inte till en terminal. '
                    'Använd -f om du menar det, eller ange en fil.')
    with tempfile.TemporaryDirectory(prefix='nvz-ström-') as d:
        d = Path(d)
        inne = d/'in'
        with open(inne, 'wb') as u:
            shutil.copyfileobj(sys.stdin.buffer, u, 1 << 20)
        if o['d'] or o['t'] or o['l']:
            reg = _register(o, inne)
            if reg is None: return 1
            if o['l']:
                summa = dict(c=0, u=0, n=0)
                print(f'{"packat":>13} {"uppackat":>13}   kvot  namn')
                return _lista_operand(o, inne, summa)
            if o['t']:
                return _testa_operand(o, inne)
            if not reg.get('platt'):
                return _fel('arkivet innehåller ett träd och kan inte '
                            'packas upp till standard ut — ge arkivet som fil')
            ut = d/'ut'
            packa_upp_fil(inne, ut, jobb=o['jobb'])
            _till_stdout(ut)
            return 0
        ark = d/'ut.nv'
        packa_fil(inne, ark, namn='standard-in', **_packkw(o))
        _till_stdout(ark)
        return 0


# ---------------------------------------------------------------------------

def main_gzip(argv) -> int:
    # -l och -t vinner över -d, precis som i gzip: `nvz -dt ark.nv` provar.
    o, filer = tolka(argv)
    if not filer:
        return _ström(o)

    status = 0
    if o['l']:
        summa = dict(c=0, u=0, n=0)
        if not o['q']:
            print(f'{"packat":>13} {"uppackat":>13}   kvot  namn')
        for p in _utvidga(o, filer, True):
            status = max(status, _lista_operand(o, p, summa))
        if summa['n'] > 1:
            print(f'{summa["c"]:>13} {summa["u"]:>13} '
                  f'{(1-summa["c"]/max(summa["u"],1))*100:6.1f}%  (summa)')
        return status
    if o['t']:
        for p in _utvidga(o, filer, True):
            status = max(status, _testa_operand(o, p))
        return status
    if not o['d'] and o['c'] and sys.stdout.isatty() and not o['f']:
        return _fel('packade data skrivs inte till en terminal. '
                    'Använd -f om du menar det.')
    for p in _utvidga(o, filer, bool(o['d'])):
        if str(p) == '-':
            status = max(status, _ström(o)); continue
        status = max(status, _packa_upp_operand(o, p) if o['d']
                     else _packa_operand(o, p))
    return status


def main_underkommandon(argv) -> int:
    a = argparse.ArgumentParser(prog='nvz', description='Innehållsmedveten arkivpackare')
    u = a.add_subparsers(dest='kmd', required=True)
    p = u.add_parser('packa'); p.add_argument('katalog'); p.add_argument('arkiv')
    p.add_argument('-q','--tyst', action='store_true'); p.add_argument('-j','--jobb', type=int)
    p.add_argument('--utan-dedup', action='store_true')
    p.add_argument('--utan-omvandling', action='store_true')
    p.add_argument('--läge','--lage', dest='läge', choices=list(LÄGEN), default='liten')
    p.add_argument('--kodek', choices=['zstd3','zstd19','zstd22','xz9e','nvcm','nvlz'])
    q = u.add_parser('packa-upp'); q.add_argument('arkiv'); q.add_argument('mal')
    q.add_argument('-q','--tyst', action='store_true'); q.add_argument('-j','--jobb', type=int)
    l = u.add_parser('lista'); l.add_argument('arkiv')
    v = u.add_parser('prova'); v.add_argument('katalog')
    n = a.parse_args(argv)
    if n.kmd == 'packa':
        packa(Path(n.katalog), Path(n.arkiv), läge=n.läge, dedup=not n.utan_dedup,
              omvandla=False if n.utan_omvandling else None,
              jobb=n.jobb, tyst=n.tyst, kodek=n.kodek)
    elif n.kmd == 'packa-upp':
        packa_upp(Path(n.arkiv), Path(n.mal), jobb=n.jobb, tyst=n.tyst)
    elif n.kmd == 'lista':
        lista(Path(n.arkiv))
    elif n.kmd == 'prova':
        return 0 if prova(Path(n.katalog)) else 1
    return 0


def main(argv=None) -> int:
    # Första ordet avgör vilken yta det är. En fil som HETER som ett
    # underkommando når man med `nvz -- packa`, precis som man når en fil
    # som heter -f: det är därför `--` finns.
    argv = list(sys.argv[1:] if argv is None else argv)
    if argv and argv[0] in UNDERKOMMANDON:
        return main_underkommandon(argv)
    return main_gzip(argv)


if __name__ == '__main__':
    try:
        sys.exit(main())
    except BrokenPipeError:
        os._exit(0)                     # `nvz -dc stor.nv | head` är inte ett fel
    except KeyboardInterrupt:
        sys.exit(130)
