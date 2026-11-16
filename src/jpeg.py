#!/usr/bin/env python3
"""JPEG-omvandlingen: skalet runt nvjpg-binären.

nvjpg gör arbetet (se nvjpg.c). Här delas dess utdata i två delar som vill
behandlas olika i arkivet:

    REST   — JPEG-huvudet ordagrant: markörer, kvantiseringstabeller,
             Huffman-tabeller och EXIF. Hundra bilder ur samma kamera har
             nästan identiska huvuden, så de packas tillsammans med zstd och
             krymper till nästan ingenting.

    KROPP  — den aritmetiskt kodade koefficientströmmen. Den är redan så nära
             slumpmässig en byteström kan bli; att skicka den genom en
             komprimerare till är bortkastad tid. Den lagras rå.

Varje omvandling provas i båda riktningarna innan den godtas. Kostnaden är att
packningen gör ungefär dubbelt jobb på foton; priset är värt det, för
alternativet är ett arkiv som ser färdigt ut och inte är det.
"""
from __future__ import annotations

import os, shutil, struct, subprocess
from pathlib import Path

def _hitta(namn: str) -> str:
    granne = Path(__file__).resolve().parent / namn
    if granne.is_file() and os.access(granne, os.X_OK):
        return str(granne)
    return shutil.which(namn) or str(granne)


NVJPG = _hitta('nvjpg')


def veckla_ut(sokvag: str):
    """Returnerar {'rest', 'kropp'} eller None om filen ska lagras rå."""
    with open(sokvag, 'rb') as f:
        orig = f.read()
    r = subprocess.run([NVJPG, 'packa'], input=orig, capture_output=True)
    if r.returncode != 0 or r.stdout[:4] != b'NVJ1':
        return None                       # progressiv, trasig, eller inte omkodbar
    d = r.stdout
    huvudlen, svanslen, _scanlen = struct.unpack('<III', d[4:16])
    gräns = 18 + huvudlen + svanslen
    if len(d) >= len(orig):
        return None                       # ingen vinst — lagra rått
    rest, kropp = d[:gräns], d[gräns:]
    if återskapa(rest, kropp) != orig:
        return None                       # ska inte hända; då lagras den rå
    return dict(rest=rest, kropp=kropp)


def återskapa(rest: bytes, kropp: bytes) -> bytes:
    r = subprocess.run([NVJPG, 'packaupp'], input=rest + kropp, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError('nvjpg packaupp: ' + r.stderr.decode()[:300])
    return r.stdout
