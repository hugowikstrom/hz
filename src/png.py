#!/usr/bin/env python3
"""PNG-omvandlingen: veckla ut deflate-skalet, och kunna sy ihop det igen.

En PNG är en följd av bitar (chunks) där IDAT-bitarnas innehåll tillsammans
bildar EN zlib-ström. Strömmen i sin tur innehåller filterrader — rå bilddata
med en filterbyte först på varje rad. Det är de raderna som är intressanta:
de är starkt mönstrade, och en modern packare som får se flera bilders rader i
samma block hittar likheter som deflate aldrig kunde se.

Problemet är vägen tillbaka. Ett arkiv får inte ge tillbaka "en PNG med samma
bild" — det ska ge tillbaka exakt samma bytes, annars går kontrollsummor,
signaturer och git sönder. Deflate är inte en kanonisk kodning: samma indata
ger olika utdata beroende på nivå, minnesnivå och strategi. Därför letar vi
upp RECEPTET — den uppsättning zlib-parametrar som råkar ge precis den ström
filen redan har — och sparar det. Hittas inget recept lagras filen rå.

Sökningen är billigare än den ser ut:

  * zlib-huvudets två första bytes röjer fönsterstorleken (CMF) och ungefär
    vilken nivågrupp kodaren använde (FLEVEL i FLG). Det skär bort det mesta.
  * Varje kandidat jämförs STEGVIS. Fel parametrar avviker nästan alltid inom
    de första hundra kilobytena, och då avbryts försöket direkt i stället för
    att packa hela bilden i onödan.
  * Receptet som senast fungerade provas först. Bilder ur samma verktyg delar
    i praktiken alltid recept, så från och med bild två är första gissningen
    rätt.
"""
from __future__ import annotations

import struct, zlib

SIGNATUR = b'\x89PNG\r\n\x1a\n'
STEG = 1 << 18
_senaste = []          # recept som fungerat i den här processen, nyast först


def _bitar(b: bytes):
    """Går igenom PNG-bitarna. Returnerar None om filen inte är välformad."""
    if b[:8] != SIGNATUR: return None
    i, ut = 8, []
    n = len(b)
    while i + 8 <= n:
        (ln,) = struct.unpack('>I', b[i:i+4])
        typ = b[i+4:i+8]
        if i + 12 + ln > n: return None
        ut.append((i, ln, typ))
        i += 12 + ln
        if typ == b'IEND': break
    if i != n: return None            # efterhängsna bytes — lagra rått i stället
    return ut


def _kandidater(strom: bytes):
    """Receptkandidater i den ordning de är värda att prova."""
    cmf, flg = strom[0], strom[1]
    if (cmf & 0x0f) != 8 or (flg & 0x20): return []      # inte deflate, eller FDICT
    wbits = (cmf >> 4) + 8
    if not 9 <= wbits <= 15: return []
    flevel = flg >> 6
    grupp = {0: (1,), 1: (2,3,4,5), 2: (6,), 3: (7,8,9)}[flevel]
    # zlib sätter FLEVEL ur nivån, så gruppen är ett besked och inte en gissning.
    # Inom gruppen: 9 och 6 är det som verktyg faktiskt använder.
    nivaer = sorted(grupp, key=lambda n: (n not in (9, 6), -n))
    kand = [(n, wbits, m, s) for n in nivaer for m in (8, 9) for s in (0, 1, 2, 3, 4)]
    forst = [r for r in _senaste if r in kand]
    return forst + [r for r in kand if r not in forst]


def _provar(ra: bytes, mal: bytes, recept) -> bool:
    """Packar om rådatan och jämför stegvis — avbryter vid första avvikelsen."""
    niva, wbits, mem, strategi = recept
    c = zlib.compressobj(niva, zlib.DEFLATED, wbits, mem, strategi)
    sikt = memoryview(mal)
    ut = bytearray()
    for i in range(0, len(ra), STEG):
        ut += c.compress(ra[i:i+STEG])
        if len(ut) > len(mal) or sikt[:len(ut)] != memoryview(ut):
            return False
    ut += c.flush()
    return len(ut) == len(mal) and sikt == memoryview(ut)


def _packa_om(ra: bytes, recept) -> bytes:
    niva, wbits, mem, strategi = recept
    c = zlib.compressobj(niva, zlib.DEFLATED, wbits, mem, strategi)
    return c.compress(ra) + c.flush()


def veckla_ut(sokvag: str, uppblåsningstak: float = 1e9):
    """Returnerar {'recept', 'rest', 'kropp'} eller None om filen ska lagras rå.

    `uppblåsningstak` är hur många gånger större den utvecklade rådatan får
    vara än IDAT-strömmen för att utvecklingen ska vara värd besväret. En PNG
    som deflate redan klämt ihop 50:1 är ett skärmklipp med stora enfärgade
    ytor; där finns inget kvar att hämta, bara packtid att förlora.

    Resultatet provas alltid genom att sys ihop igen och jämföras med
    originalet. Det som returneras är därmed bevisat återskapbart.
    """
    with open(sokvag, 'rb') as f:
        b = f.read()
    bitar = _bitar(b)
    if not bitar: return None
    idat = [(i, ln) for i, ln, typ in bitar if typ == b'IDAT']
    if not idat or sum(ln for _, ln in idat) < 512: return None   # för litet för att vara värt något

    strom = b''.join(b[i+8:i+8+ln] for i, ln in idat)
    try:
        ra = zlib.decompress(strom)
    except zlib.error:
        return None
    if len(ra) > len(strom) * uppblåsningstak:
        return None

    recept = None
    for k in _kandidater(strom):
        if _provar(ra, strom, k):
            recept = k; break
    if recept is None: return None
    if recept in _senaste: _senaste.remove(recept)
    _senaste.insert(0, recept); del _senaste[4:]

    # Skelettet är filen utan IDAT-innehåll. Allt annat — signatur, alla
    # övriga bitar, längder och kontrollsummor — följer med ordagrant, så
    # ihopsyningen inte behöver veta något om PNG-semantik.
    skelett = bytearray()
    sist = 0
    for i, ln in idat:
        skelett += b[sist:i+8]
        sist = i + 8 + ln
    skelett += b[sist:]

    if återskapa(bytes(skelett), ra, recept) != b:
        return None                                   # ska inte hända; då lagras den rå
    return dict(recept=list(recept), rest=bytes(skelett), kropp=ra)


def återskapa(skelett: bytes, ra: bytes, recept) -> bytes:
    """Syr ihop PNG:en igen: deflatera rådatan med receptet och fyll i IDAT."""
    strom = _packa_om(ra, tuple(recept))
    ut = bytearray(skelett[:8])
    i, j = 8, 0                         # i = läsläge i skelettet, j = i strömmen
    n = len(skelett)
    while i + 8 <= n:
        huvud = skelett[i:i+8]
        (ln,) = struct.unpack('>I', huvud[:4])
        typ = huvud[4:8]
        ut += huvud
        if typ == b'IDAT':
            ut += strom[j:j+ln]; j += ln
            ut += skelett[i+8:i+12]     # kontrollsumman låg kvar i skelettet
            i += 12
        else:
            ut += skelett[i+8:i+12+ln]
            i += 12 + ln
        if typ == b'IEND': break
    ut += skelett[i:]
    return bytes(ut)
