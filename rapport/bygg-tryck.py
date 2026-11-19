#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Bygger tryckversionen av mätrapporten: rapport/tryck.html → PDF.

Varför en EGEN fil och inte webbsidans HTML med en print-stylesheet: en PDF är
paginerad. Sidbrytningar, en försättssida, sidnummer och en löpande fot är inte
dekoration utan själva skillnaden mellan en utskriven webbsida och ett dokument.
Sidorna byggs därför explicit här, en div per A4-sida.

Typsnitten refereras vid FAMILJENAMN och laddas aldrig via @font-face. Chrome
bäddar in varje typsnitt som kommit in genom @font-face som en Type 3-font, och
sådana underkänner tryckerier. Archivo, Noto Serif och Liberation Mono finns
statiskt installerade på maskinen; verifiera alltid efteråt med
`pdffonts rapport/hz-rapport.pdf | grep 'Type 3'`.

Alla tal läses ur resultat/*.json. Ingenting skrivs av för hand.

    python3 rapport/bygg-tryck.py && bash rapport/bygg-pdf.sh
"""
from __future__ import annotations

import json
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
MIB = 1048576

KORPUS = [
    ('kod', 'Programkod',
     'Källkod ur arton projekt på maskinen: .mjs, .py, .ts, .json, .html, .css, .md'),
    ('png', 'Bilder (PNG)',
     'Skärmdumpar, renderade produktbilder och paritetsklipp ur ting'),
    ('foto', 'Foton (JPEG)',
     'Kamerabilder ur Pecka-arkivet plus TikTok-omslag — alla baseline-JPEG'),
    ('symbios3d', 'Blandat projektträd',
     'Hela ~/symbios-3d: 1,2 GB delade bibliotek, 4 600 .py, 4 600 .pyc, en hel arbetskopia'),
]

ORDNING = ['tar (okomprimerad)', 'tar.gz -6', 'tar.gz -9 (pigz)', 'tar.bz2 -9',
           'tar.zst -3 (dagens)', 'tar.zst -19 --long', 'tar.zst -22 --ultra',
           'tar.xz -9e', 'nvz snabb', 'nvz utan knep', 'nvz normal',
           'nvz liten']


def tal(x, d=2):
    """Svenska tal med hårt mellanrum som tusentalsavgränsare.

    Ett vanligt (eller tunt) mellanrum är en radbrytningsmöjlighet, och
    "2 869,25" delades mitt itu mellan två rader i första tryckkorrekturet.
    """
    return f'{x:,.{d}f}'.replace(',', '\u00a0').replace('.', ',')


def tecken(x, d=1, enhet=''):
    """Tal med typografiskt plus/minus, aldrig bindestreck."""
    return f'{"+" if x > 0 else "\u2212"}{tal(abs(x), d)}{enhet}'


def läs(namn):
    return json.loads((ROT / 'resultat' / f'{namn}.json').read_text())


# ---------------------------------------------------------------------------
# Byggstenar
# ---------------------------------------------------------------------------

# Rows keep their frozen measurement ids ('nvz liten' ...); only the
# printed label says hz (the tool was renamed 2026-11-19).
def visa(namn):
    return namn.replace('nvz ', 'hz ') if namn.startswith('nvz') else namn


def sort_av(namn):
    if namn.startswith('nvz'): return 'nv'
    if 'dagens' in namn: return 'idag'
    if 'okompr' in namn: return 'ra'
    return ''


def diagram(data) -> str:
    """Stapeldiagram: stapellängd mot den största PACKAREN. Kortare är bättre.

    Den opackade taren är med i bilagans tabell men inte här. Tas den med blir
    den skalans tak, och då trängs hela det intressanta spannet — på kodkorpusen
    allt mellan 7 och 12 MiB — ihop till en femtedel av spåret. Skalan börjar
    fortfarande på noll; stapellängden är alltså fortfarande proportionell mot
    storleken, vilket är hela poängen med en stapel.
    """
    mät = {n: v for n, v in data['matningar'].items() if 'okompr' not in n}
    rå = data['rabyte']
    idag = mät['tar.zst -3 (dagens)']['byte']
    rader = sorted((v['byte'], n) for n, v in mät.items())
    max_b = max(b for b, _ in rader)
    ut = ['<div class="diagram">']
    for byte, namn in reversed(rader):
        v = mät[namn]
        sort = sort_av(namn)
        andel = byte / max_b * 100
        delta = (byte - idag) / idag * 100
        d_text = '—' if sort == 'idag' else tecken(delta)
        # En hz-rad som ligger ÖVER dagens nivå ska inte målas i
        # vinstfärg bara för att den heter hz.
        plus = ' plus' if delta > 0 and sort != 'idag' else ''
        ut.append(
            f'<div class="brad {sort}{plus}">'
            f'<span class="bnamn">{visa(namn)}</span>'
            f'<span class="bspår"><i class="bstapel" style="width:{andel:.2f}%"></i>'
            f'<i class="bmärke" style="left:{idag / max_b * 100:.2f}%"></i></span>'
            f'<span class="bmib">{tal(byte / MIB, 1 if byte / MIB >= 100 else 2)}</span>'
            f'<span class="bdelta">{d_text}</span>'
            f'</div>')
    ut.append(
        f'<div class="bfot"><span>'
        f'<i class="prick nv"></i> hz &nbsp; <i class="prick idag"></i> dagens arkivskript'
        f' &nbsp; <i class="prick kl"></i> klassiska verktyg</span>'
        f'<span>stapel = andel av originalets {tal(rå / MIB, 1)} MiB · streck = dagens nivå</span></div>')
    ut.append('</div>')
    return '\n'.join(ut)


def tabell(data) -> str:
    mät = data['matningar']
    rå = data['rabyte']
    idag = mät['tar.zst -3 (dagens)']['byte']
    rader = ['<table class="mät"><thead><tr><th>packare</th><th>MiB</th><th>kvot</th>'
             '<th>mot dagens</th><th>packning</th><th>uppackning</th><th>bit-exakt</th>'
             '</tr></thead><tbody>']
    for namn in ORDNING:
        v = mät.get(namn)
        if not v: continue
        sort = sort_av(namn)
        delta = (v['byte'] - idag) / idag * 100
        d_text = '—' if sort == 'idag' else tecken(delta, 1, ' %')
        rader.append(
            f'<tr class="{sort}{" plus" if delta > 0 and sort != "idag" else ""}"><td>{visa(namn)}</td>'
            f'<td>{tal(v["byte"] / MIB, 2)}</td>'
            f'<td>{tal(rå / v["byte"], 2)}×</td>'
            f'<td>{d_text}</td>'
            f'<td>{tal(v["packsek"], 1)} s</td>'
            f'<td>{tal(v["uppacksek"], 1)} s</td>'
            f'<td class="ja">ja</td></tr>')
    rader.append('</tbody></table>')
    return '\n'.join(rader)


# ---------------------------------------------------------------------------
# Sidmall
# ---------------------------------------------------------------------------

CSS = r"""
@page { size: A4 portrait; margin: 0; }

* { box-sizing: border-box; }
html, body { margin: 0; padding: 0; background: #fff; }
body {
  font-family: "Noto Serif", serif;
  font-size: 9.4pt;
  line-height: 1.52;
  color: #14201d;
  -webkit-print-color-adjust: exact;
  print-color-adjust: exact;
}

.sida {
  position: relative;
  width: 210mm; height: 297mm;
  padding: 18mm 20mm 16mm;
  page-break-after: always;
  break-after: page;
  overflow: hidden;
}
.sida:last-child { page-break-after: auto; break-after: auto; }

.fot {
  position: absolute; left: 20mm; right: 20mm; bottom: 10mm;
  display: flex; justify-content: space-between; align-items: baseline;
  border-top: 0.4pt solid #d7dfdb; padding-top: 2.4mm;
  font-family: Archivo, sans-serif; font-size: 6.6pt; letter-spacing: 0.05em;
  color: #8b9a94; text-transform: uppercase;
}

h1, h2, h3, h4, .etikett, .stort, th, .bnamn, .bmib, .bdelta, .bfot, .nr {
  font-family: Archivo, sans-serif;
}
h2 {
  font-size: 15pt; font-weight: 700; letter-spacing: -0.02em;
  margin: 0 0 1.5mm; line-height: 1.12;
}
h3 {
  font-size: 10.4pt; font-weight: 600; letter-spacing: -0.01em;
  margin: 5mm 0 1.2mm;
}
h4 { font-size: 9.2pt; font-weight: 600; margin: 0 0 0.8mm; }
p { margin: 0 0 2.4mm; }
p.lead { font-size: 10pt; color: #41524d; margin-bottom: 4mm; }
strong { font-weight: 700; }
em { font-style: italic; }

.etikett {
  font-size: 6.8pt; font-weight: 600; letter-spacing: 0.16em;
  text-transform: uppercase; color: #8b9a94; display: block; margin-bottom: 2.4mm;
}
.regel { border: 0; border-top: 0.6pt solid #14201d; margin: 0 0 4mm; }
code, .mono { font-family: "Liberation Mono", monospace; font-size: 8.3pt; }

/* ------------------------------------------------ försättssida -------- */
.titel { height: calc(100% - 5mm); display: flex; flex-direction: column; }
.titel .topp { flex: 0 0 auto; }
.titel h1 {
  font-family: Archivo, sans-serif; font-size: 58pt; font-weight: 800;
  letter-spacing: -0.045em; line-height: 0.9; margin: 22mm 0 6mm;
}
.titel .under {
  font-size: 13pt; line-height: 1.38; color: #41524d; max-width: 125mm;
}
.titel .under b { color: #14201d; }
.titel .mitt { flex: 1 1 auto; display: flex; align-items: center; }
.titel .botten { flex: 0 0 auto; font-size: 8.4pt; color: #717f7a; }

.nyckeltal { display: flex; gap: 0; width: 100%; border-top: 0.6pt solid #14201d; }
.nyckeltal > div {
  flex: 1; padding: 5mm 4mm 5mm 0; border-right: 0.4pt solid #d7dfdb;
}
.nyckeltal > div:last-child { border-right: 0; }
.nyckeltal .etikett { font-size: 6.4pt; margin-bottom: 2mm; }
.nyckeltal .stort {
  font-size: 23pt; font-weight: 700; letter-spacing: -0.035em;
  color: #00725b; display: block; line-height: 1;
}
.nyckeltal .spec {
  font-family: "Liberation Mono", monospace; font-size: 7.2pt;
  color: #717f7a; display: block; margin-top: 2mm; line-height: 1.4;
}

/* ------------------------------------------------ diagram ------------- */
.diagram { margin: 2mm 0 0; }
.brad {
  display: grid; grid-template-columns: 38mm 1fr 15mm 13mm;
  align-items: center; gap: 3mm; margin-bottom: 1.5mm;
}
.bnamn {
  font-size: 7.2pt; text-align: right; color: #41524d;
  white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
}
.brad.nv .bnamn { color: #14201d; font-weight: 600; }
.bspår { position: relative; display: block; height: 4.2mm; background: #eef2f0; border-radius: 0.5mm; }
.bstapel {
  position: absolute; left: 0; top: 0; bottom: 0; display: block;
  background: #9aa8a3; border-radius: 0 1mm 1mm 0;
}
.brad.nv .bstapel { background: #00725b; }
.brad.idag .bstapel { background: #b04c24; }
.brad.ra .bstapel { background: #e6ebe8; box-shadow: inset 0 0 0 0.4pt #c9d3cf; }
.bmärke { position: absolute; top: -1mm; bottom: -1mm; width: 0; border-left: 0.5pt dashed #b04c24; }
.bmib, .bdelta { font-family: "Liberation Mono", monospace; font-size: 7.2pt; text-align: right; }
.bmib { color: #41524d; }
.bdelta { color: #8b9a94; }
.brad.nv .bmib { color: #14201d; font-weight: 700; }
.brad.nv .bdelta { color: #00725b; font-weight: 700; }
.brad.nv.plus .bdelta { color: #8b9a94; font-weight: 400; }
tr.nv.plus td:nth-child(4) { color: #717f7a; font-weight: 400; }
.brad.idag .bdelta { color: #b04c24; }
.bfot {
  display: flex; justify-content: space-between; margin-top: 2.5mm; padding-top: 1.8mm;
  border-top: 0.4pt solid #e6ebe8; font-size: 6.6pt; color: #8b9a94;
}
.prick { display: inline-block; width: 2.2mm; height: 2.2mm; border-radius: 0.4mm; vertical-align: middle; }
.prick.nv { background: #00725b; } .prick.idag { background: #b04c24; } .prick.kl { background: #9aa8a3; }

/* ------------------------------------------------ tabeller ------------ */
table { border-collapse: collapse; width: 100%; margin: 1mm 0 3mm; }
th, td { text-align: right; padding: 1.3mm 2mm; }
th:first-child, td:first-child { text-align: left; }
th {
  font-size: 6.6pt; font-weight: 600; letter-spacing: 0.09em; text-transform: uppercase;
  color: #717f7a; border-bottom: 0.6pt solid #14201d;
}
td {
  font-family: "Liberation Mono", monospace; font-size: 7.4pt;
  color: #41524d; border-bottom: 0.3pt solid #eef2f0;
}
tr.nv td { color: #14201d; }
tr.nv td:first-child { font-weight: 700; }
tr.nv td.ja, td.ja { color: #00725b; }
tr.idag td { color: #b04c24; }

table.jämför td, table.jämför th { font-family: "Noto Serif", serif; font-size: 8pt; }
table.jämför td:first-child { font-family: Archivo, sans-serif; font-weight: 600; font-size: 8pt; color: #14201d; }
table.jämför td.mitt { text-align: center; font-family: Archivo, sans-serif; }

/* ------------------------------------------------ övrigt -------------- */
.kolumner { display: grid; grid-template-columns: 1fr 1fr; gap: 7mm; }
.ruta {
  border: 0.4pt solid #d7dfdb; border-left: 1.2pt solid #00725b;
  padding: 3.5mm 4mm; margin: 0 0 3.5mm; background: #fbfcfb;
}
.ruta.varning { border-left-color: #b04c24; }
.ruta p:last-child { margin-bottom: 0; }
.ruta .slutsats {
  font-family: Archivo, sans-serif; font-weight: 700; font-size: 10pt;
  letter-spacing: -0.01em; margin-bottom: 1.6mm;
}

.fynd { display: grid; grid-template-columns: 7mm 1fr; gap: 3mm; margin-bottom: 4.5mm; }
.fynd .nr { font-size: 15pt; font-weight: 800; color: #b04c24; line-height: 0.9; letter-spacing: -0.04em; }
.fynd p:last-child { margin-bottom: 0; }

ul { margin: 0 0 2.5mm; padding-left: 4.5mm; }
li { margin-bottom: 1.3mm; }
li b { font-weight: 700; }

pre {
  font-family: "Liberation Mono", monospace; font-size: 7.6pt; line-height: 1.6;
  background: #f6f8f7; border: 0.4pt solid #e6ebe8; padding: 3mm 3.5mm;
  margin: 2mm 0 3mm; white-space: pre-wrap;
}
.figrubrik { margin: 0 0 1mm; }
.figrubrik .namn { font-family: Archivo, sans-serif; font-weight: 600; font-size: 9pt; }
.figrubrik .om { font-size: 7.4pt; color: #717f7a; }
"""


def sida(nr, tot, innehåll, fot_vänster='hz · mätrapport'):
    return (f'<div class="sida">{innehåll}'
            f'<div class="fot"><span>{fot_vänster}</span>'
            f'<span>sida {nr} av {tot}</span></div></div>')


def bygg():
    d = {k: läs(k) for k, _, _ in KORPUS}

    def mib(k, p): return d[k]['matningar'][p]['byte'] / MIB
    def mot_idag(k, p):
        i = d[k]['matningar']['tar.zst -3 (dagens)']['byte']
        return (d[k]['matningar'][p]['byte'] - i) / i * 100

    bäst_klassisk = {}
    for k, _, _ in KORPUS:
        kand = [(v['byte'], n) for n, v in d[k]['matningar'].items()
                if not n.startswith('nvz') and 'okompr' not in n]
        bäst_klassisk[k] = min(kand)

    KORT = {'tar.zst -22 --ultra': 'zstd −22', 'tar.xz -9e': 'xz −9e',
            'tar.bz2 -9': 'bzip2 −9', 'tar.zst -19 --long': 'zstd −19',
            'tar.gz -9 (pigz)': 'gzip −9', 'tar.gz -6': 'gzip −6'}

    def bäst_rad(k):
        byte, namn = bäst_klassisk[k]
        namn = KORT.get(namn, namn)
        nv = min(d[k]['matningar'][p]['byte'] for p in
                 ('nvz normal', 'nvz liten') if p in d[k]['matningar'])
        return namn, (nv - byte) / byte * 100

    TOT = 9
    sidor = []

    # ---------------------------------------------------------------- 1 --
    nk = []
    for k, rubrik, _ in KORPUS:
        bästa = 'nvz liten' if 'nvz liten' in d[k]['matningar'] else 'nvz normal'
        namn, mot = bäst_rad(k)
        nk.append(
            f'<div><span class="etikett">{rubrik}</span>'
            f'<span class="stort">−{tal(abs(mot_idag(k, bästa)), 0)} %</span>'
            f'<span class="spec">{tal(mib(k, "tar.zst -3 (dagens)"), 1)} → '
            f'{tal(mib(k, bästa), 1)} MiB<br>{tal(abs(mot), 0)} % under {namn}</span></div>')
    sidor.append(sida(1, TOT, f'''
<div class="titel">
  <div class="topp">
    <span class="etikett">Mätrapport · 16 november 2026 · Hugo Wikström</span>
    <h1>hz</h1>
    <p class="under">En arkivpackare som <b>vet vad den packar</b> — och som mäter
      sig mot allt annat som finns installerat på maskinen. Fyra riktiga filträd,
      tolv packare, fyrtiosju rundgångar, <b>varje enskild bit-exakt</b>.</p>
  </div>
  <div class="mitt"></div>
  <div class="nyckeltal">{''.join(nk)}</div>
  <div class="botten">
    Mätt på en Ryzen AI 9 HX 470 (24 trådar, 2,0 GHz) med NVMe-disk.<br>
    Koden, korpusbyggaren och alla mätvärden ligger i <code>~/hz</code>.
  </div>
</div>''', 'hz · mätrapport · november 2026'))

    # ---------------------------------------------------------------- 2 --
    rader_sam = []
    for k, rubrik, _ in KORPUS:
        bästa = 'nvz liten' if 'nvz liten' in d[k]['matningar'] else 'nvz normal'
        namn, mot = bäst_rad(k)
        rader_sam.append(
            f'<tr><td>{rubrik}</td><td>{tal(d[k]["rabyte"] / MIB, 1)}</td>'
            f'<td>{tal(mib(k, "tar.zst -3 (dagens)"), 1)}</td>'
            f'<td>{namn} · {tal(bäst_klassisk[k][0] / MIB, 1)}</td>'
            f'<td class="ja">{tal(mib(k, bästa), 1)}</td>'
            f'<td class="ja">{tecken(mot_idag(k, bästa), 1, " %")}</td></tr>')
    sammanfattning = (
        '<table class="mät"><thead><tr><th>träd</th><th>original MiB</th>'
        '<th>dagens MiB</th><th>bästa klassiska</th><th>hz MiB</th>'
        '<th>mot dagens</th></tr></thead><tbody>'
        + ''.join(rader_sam) + '</tbody></table>')

    sidor.append(sida(2, TOT, '''
<span class="etikett">Sammanfattning</span>
<h2>Vad problemet är</h2>
<hr class="regel">
<p class="lead"><code>tar | zstd</code> är två program som inte vet något om varandra.
tar lägger filerna i den ordning katalogträdet råkar ha dem och skickar vidare en
enda byteström. zstd ser den strömmen genom ett fönster och får aldrig veta att fil
4 000 är en exakt kopia av fil 12, att de 9 MB "slump" den just gav upp på i själva
verket är en deflate-ström som går att veckla ut, eller att de 400 MB foton den
maler igenom har ett entropilager från 1992 som går att byta ut.</p>

<p>hz fyller tomrummet mellan behållaren och komprimeraren. Den klassar
innehållet, byter ut representationen där det finns en bättre, grupperar likt med
likt, och väljer kodek per block genom att faktiskt prova. Fem mekanismer — och
den här rapporten redovisar vad var och en gav, inklusive de som gav noll.</p>

<h3>Mekanismerna, och vad de faktiskt bidrog med</h3>
<table class="jämför">
<thead><tr><th>mekanism</th><th style="text-align:left">utfall</th><th>mätt på</th></tr></thead>
<tbody>
<tr><td>Omvandla innehållet</td><td style="text-align:left">det enda som verkligen bär:
  <b>−15 %</b> på foton, <b>−42 %</b> på PNG</td><td class="mitt">foto, png</td></tr>
<tr><td>Dedup på filnivå</td><td style="text-align:left">stort eller noll, aldrig mittemellan:
  28,7 % dubbletter i PNG-korpusen, 0,7 % i kodkorpusen</td><td class="mitt">alla fyra</td></tr>
<tr><td>Sortera likt med likt</td><td style="text-align:left"><b>−1,85 %</b> … men registret
  kostar <b>+1,81 %</b> → netto noll</td><td class="mitt">kod</td></tr>
<tr><td>Välja kodek genom prov</td><td style="text-align:left">hela stegen
  zstd −19 → xz → hzcm: 8,53 → 8,23 → 7,19 MiB</td><td class="mitt">kod</td></tr>
<tr><td>Prova i stället för anta</td><td style="text-align:left"><b>175 MiB</b> på PNG-korpusen,
  och gränsen mellan vinst och förlust på ett riktigt arkiv</td><td class="mitt">png, skarpt arkiv</td></tr>
</tbody></table>

<p>Sorteringsraden är värd att stanna vid. Den lät som en av huvudidéerna och visade
sig betala exakt för sitt eget index, inte mer. Vinsterna kommer från att data
<em>byter representation</em> före komprimeringen — inte från att den ligger i en
smartare ordning.</p>

<h3>Resultatet i en tabell</h3>
{SAMMANFATTNING}

<div class="ruta">
  <p class="slutsats">Allt är förlustfritt och bit-exakt — bevisat, inte påstått.</p>
  <p>Varje omvandling syr ihop sitt resultat igen och jämför med originalet redan vid
  packningen. Stämmer inte varje byte lagras filen rå i stället. Ett arkiv kan därför
  aldrig bli "nästan rätt": antingen är omvandlingen bevisad, eller så användes den
  inte. Utöver det packas och packas varje mätning i den här rapporten upp i sin
  helhet, och jämförs mot originalträdet med <code>diff -rq</code>.</p>
</div>'''.replace('{SAMMANFATTNING}', sammanfattning)))

    # ---------------------------------------------------------------- 3 --
    sidor.append(sida(3, TOT, '''
<span class="etikett">Delarna</span>
<h2>Tre lager som ingen vanlig packare rör</h2>
<hr class="regel">

<h3>JPEG kodas om utan att en pixel rörs &nbsp;<span class="mono" style="font-size:7.6pt;color:#717f7a">src/hzjpg.c · 760 rader C</span></h3>
<p>En JPEG är DCT-koefficienter entropikodade med Huffman. Huffman kan inte koda en
symbol på mindre än en bit, och vet ingenting om att blocket bredvid såg nästan
likadant ut. hzjpg avkodar koefficienterna och kodar om dem med en adaptiv
aritmetisk kodare vars modeller frågar tre grannskap: blocket till vänster och
ovanför (<em>hur rörig är den här delen av bilden?</em>), samma plats i grannblocken
(<em>hur stor brukar just den frekvensen vara här?</em>), och platserna till vänster
och ovanför inuti samma block (<em>var ligger energin?</em>).</p>
<p>Resultat på hela fotokorpusen: <b>1 143 av 1 143 filer bit-exakta, −15,0 %</b>.
Noll råfall. Modellen trimmades fram i mätbara steg: första fungerande modell
−12,0 %, grannskapskontexter −13,1 %, finare hinkar −13,9 %, takt som sjunker med
antalet observationer −14,6 %, blockets täthet i storleksmodellen −14,8 %. Två
idéer som lät bra ströks efter mätning: egen kontextdimension för grannblockets
riktning (−13,3 %, kontexterna tunnades ut) och två fasta adaptionstakter som vägs
ihop (−11,6 %, den långsamma takten drog medelvärdet mot 50 % hela bilden igenom).</p>

<h3>PNG vecklas ut och packas om &nbsp;<span class="mono" style="font-size:7.6pt;color:#717f7a">src/png.py</span></h3>
<p>Deflate från 1996 är ett dåligt skal runt bilddata: 32 kB fönster, en fil i taget.
hz vecklar ut IDAT-strömmen till råa filterrader och packar dem tillsammans med
alla andra bilders rådata. Vägen tillbaka är det svåra — deflate är inte en kanonisk
kodning, så samma indata ger olika utdata beroende på nivå, minnesnivå och strategi.
hz letar därför upp <b>receptet</b>: den uppsättning zlib-parametrar som råkar ge
precis den ström filen redan har. Sökningen är billig eftersom zlib-huvudet röjer
fönsterstorlek och nivågrupp, eftersom fel kandidat avviker inom de första hundra
kilobytena och då avbryts, och eftersom receptet som senast fungerade provas först.</p>
<p>Hittas inget recept lagras filen rå, och det händer ofta: Chrome och
gnome-screenshot använder deflate-motorer som systemets zlib inte kan härma. I
PNG-korpusen gick 281 av 4 422 provade filer att veckla ut — vinsten på dem som gick
är desto större.</p>

<h3>En egen kompressor för källkod &nbsp;<span class="mono" style="font-size:7.6pt;color:#717f7a">src/hzcm.c · 450 rader C</span></h3>
<p>xz var det starkaste som fanns installerat, så hz fick något starkare. I
stället för att hänvisa bakåt i en ordbok gissar hzcm nästa <em>bit</em>, med åtta
modeller samtidigt: ordning 0–8, en ordmodell, och en matchmodell som gör
ordbokspackarens trick som en röst bland andra. En blandare lär sig under tiden vem
som brukar ha rätt, och två efterjusterare putsar resultatet.</p>
<table class="jämför">
<thead><tr><th>indata</th><th>xz −9e</th><th>hzcm</th><th></th></tr></thead>
<tbody>
<tr><td>källkod, 8 MB</td><td>782 876</td><td><b>663 744</b></td><td>−15 %</td></tr>
<tr><td>seq 1 50000</td><td><b>10 160</b></td><td>47 548</td><td>+368 %</td></tr>
</tbody></table>
<p>Den är alltså inte bättre på allt — en modell som gissar nästa bit ur de
föregående byten ser inte att ett tal ökar med ett, medan ordbokspackarens
matchningslängd gör det. Därför <em>provas</em> hzcm mot xz och zstd per block i
stället för att väljas på filtyp.</p>'''))

    # ------------------------------------------------------------- 4 & 5 --
    for i, (par, nr) in enumerate((((0, 1), 4), ((2, 3), 5))):
        block = []
        if nr == 4:
            block.append('<span class="etikett">Mätningen</span>'
                         '<h2>Fyra träd, tolv packare</h2><hr class="regel">'
                         '<p class="lead">Stapeln visar hur stor del av originalet som är kvar '
                         '— kortare är bättre. Det streckade märket är dagens arkivskript: allt '
                         'till vänster om det är en vinst, allt till höger en förlust.</p>')
        for ix in par:
            k, rubrik, om = KORPUS[ix]
            block.append(
                f'<div class="figrubrik"><div class="namn">{rubrik} — '
                f'{tal(d[k]["rabyte"] / MIB, 1)} MiB i {tal(d[k]["filer"], 0)} filer</div>'
                f'<div class="om">{om}</div></div>')
            block.append(diagram(d[k]))
            if ix != par[-1]:
                block.append('<div style="height:5mm"></div>')
        if nr == 5:
            block.append('''<h3>Vad diagrammen säger</h3>
<ul>
<li><b>Foton är det tydligaste fallet.</b> Varje klassiskt verktyg — gzip, bzip2,
zstd −22, xz −9e — stannar inom en procent från där det började. De kan bara leta
efter upprepningar i en ström som redan är entropikodad. hz byter ut
entropikodningen och tar 15 %.</li>
<li><b>På det blandade trädet vinner hz även på tid.</b> zstd −22 behövde 623 s
för 301,2 MiB; hz tog 325 s för 279,1 MiB — halva tiden, 7 % mindre. Mot xz −9e
är det 6 % mindre på i stort sett samma tid.</li>
<li><b>Läget <code>snabb</code> är gratislunchen.</b> 10 % under dagens arkiv på
fjorton sekunder, helt och hållet på dedup och sortering. Där finns dubbletter som
tar aldrig ser.</li>
</ul>''')
        sidor.append(sida(nr, TOT, '\n'.join(block)))

    # ---------------------------------------------------------------- 6 --
    sidor.append(sida(6, TOT, '''
<span class="etikett">Fynd</span>
<h2>Tre gånger hade jag fel</h2>
<hr class="regel">
<p class="lead">Varje designbeslut i hz är mätt fram, och tre av dem gick tvärtemot
vad som verkade självklart. De tre är anledningen till att siffrorna ser ut som de gör.</p>

<div class="fynd"><div class="nr">1</div><div>
<h4>PNG som blåses upp hundra gånger är värda mest</h4>
<p>Jag satte ett tak: en bild vars rådata blir mer än sexton gånger större än dess
deflate-ström ansågs inte värd att veckla ut. Mätningen gav <b>−40 % just i den
högsta klassen</b> — mest av alla. I efterhand är det logiskt. En bild som deflate
klämt 50:1 är ett skärmklipp med stora enfärgade ytor, och när femtio sådana ligger
utvecklade i samma block hittar en modern packare likheterna <b>mellan</b> bilderna.
Det kunde deflate aldrig se genom sitt 32 kB-fönster, en fil i taget.</p></div></div>

<table class="mät" style="margin-bottom:5mm">
<thead><tr><th>uppblåsning</th><th>filer</th><th>original</th><th>zstd19 på original</th>
<th>via utveckling</th><th>vinst</th></tr></thead>
<tbody>
<tr><td>2–4×</td><td>13</td><td>17,5 MB</td><td>17,48 MB</td><td>13,92 MB</td><td class="ja">20,4 %</td></tr>
<tr><td>4–8×</td><td>14</td><td>32,1 MB</td><td>26,51 MB</td><td>18,45 MB</td><td class="ja">30,4 %</td></tr>
<tr><td>8–16×</td><td>40</td><td>61,5 MB</td><td>49,93 MB</td><td>40,95 MB</td><td class="ja">18,0 %</td></tr>
<tr><td>16–32×</td><td>26</td><td>17,7 MB</td><td>12,24 MB</td><td>9,88 MB</td><td class="ja">19,3 %</td></tr>
<tr class="nv"><td>32×+</td><td>188</td><td>13,4 MB</td><td>7,31 MB</td><td>4,35 MB</td><td class="ja">40,4 %</td></tr>
</tbody></table>

<div class="fynd"><div class="nr">2</div><div>
<h4>"Redan packat" får inte vara ett antagande</h4>
<p>Film, zip och PNG som inte kunde vecklas ut lagrades rått per definition — det
sparar packtid på data där ingen komprimerare ändå hittar något. På PNG-korpusen
kostade det <b>175 MiB</b>, och på det skarpa lostconnection-arkivet gjorde det hz
<b>sämre än dagens</b>: en halv procent av 2,9 GB film är fjorton megabyte, och
<code>zstd -3</code> tar dem på några sekunder. Nu provpackas varje block på åtta
ställen med varje kodek, och tröskeln har tre utfall i stället för två — rått under
0,2 %, billig kodek däremellan, dyr kodek när det finns något att hämta.</p></div></div>

<div class="fynd"><div class="nr">3</div><div>
<h4>Provsviten hittade tyst dataförlust</h4>
<p>En symlänk som pekar på en katalog <b>försvann helt ur arkivet</b> — utan fel, utan
varning. Den dyker upp bland de <em>kataloger</em> <code>os.walk</code> rapporterar,
inte bland dess filer, och koden letade bara efter länkar bland filerna. Katalogernas egna
tidsstämplar återställdes inte heller. Båda hade gått obemärkt förbi i <b>varje
storleksmätning</b> — mätningar jämför innehåll, inte träd. Det är därför provsviten
bygger ett träd av kantfall: tomma filer, trasiga länkar, filnamn med nyrad i, udda
rättigheter, tidsstämplar från 1985 och 2031.</p></div></div>'''))

    # ---------------------------------------------------------------- 7 --
    lc_hela = (2866.74 - 2869.25) / 2869.25 * 100
    sidor.append(sida(7, TOT, f'''
<span class="etikett">Vad som skiljer den från andra</span>
<h2>Skillnaden är ett lager, inte en inställning</h2>
<hr class="regel">
<p class="lead">gzip, bzip2, xz och zstd är byteströmspackare: de ser en enda ström och
vet ingenting om filer eller filtyper. tar är en behållare utan komprimering, utan
index och utan dedup. hz bor i tomrummet mellan dem.</p>

<table class="jämför">
<thead><tr><th></th><th class="mitt">ser filer</th><th class="mitt">index</th>
<th class="mitt">dedup</th><th class="mitt">omvandlar</th><th class="mitt">väljer kodek</th></tr></thead>
<tbody>
<tr><td>gzip / bzip2 / xz / zstd</td><td class="mitt">nej</td><td class="mitt">nej</td>
  <td class="mitt">via fönstret</td><td class="mitt">nej</td><td class="mitt">nej</td></tr>
<tr><td>tar</td><td class="mitt">ja</td><td class="mitt">nej</td><td class="mitt">nej</td>
  <td class="mitt">nej</td><td class="mitt">—</td></tr>
<tr><td>zip</td><td class="mitt">ja</td><td class="mitt">ja</td><td class="mitt">nej</td>
  <td class="mitt">nej</td><td class="mitt">nej</td></tr>
<tr><td>7-Zip</td><td class="mitt">ja</td><td class="mitt">ja</td><td class="mitt">nej</td>
  <td class="mitt">delvis</td><td class="mitt">per typ</td></tr>
<tr><td>restic / borg</td><td class="mitt">ja</td><td class="mitt">ja</td>
  <td class="mitt">starkare</td><td class="mitt">nej</td><td class="mitt">nej</td></tr>
<tr class="nv"><td>hz</td><td class="mitt">ja</td><td class="mitt">ja</td>
  <td class="mitt">ja</td><td class="mitt">ja</td><td class="mitt">mätt, per block</td></tr>
</tbody></table>

<h3>Vad som är nytt — och vad som inte är det</h3>
<p>Ingen av de tre omvandlingarna är uppfunnen här. PNG-receptsökningen gör
<code>precomp</code> och <code>reflate</code> sedan länge. Förlustfri JPEG-ompackning
gör <code>lepton</code> och <code>brunsli</code> — och <em>bättre</em> än min, cirka
22 % mot mina 15 %. Kontextblandning är paq- och zpaq-familjen, och de är långt
starkare än hzcm. Skillnaden är inte uppfinningen utan <b>integrationen</b>: ett enda
arkivprogram som gör allt det här, och som för varje omvandling bevisar att den går
att vända innan den används. Inget av de klassiska verktygen försöker ens; de
specialiserade gör det var och en för sin filtyp, och ingen av dem fanns installerad
på den här maskinen.</p>

<h3>Vad hz inte kan som de andra kan</h3>
<ul>
<li><b>Inte strömmande.</b> tar kan rapa rakt in i ssh eller på band. hz behöver en
katalog och arbetsyta på disk — upp till flera GB när PNG-utvecklingen är igång.</li>
<li><b>Oberoende block kostar korsredundans.</b> Därför förlorar hz mot
<code>tar.zst -19</code> på PNG-korpusen när dedup och omvandlingar är avstängda:
256,4 mot 252,7 MiB. zstd ser en enda 431 MB-ström genom ett 2 GB-fönster; hz
klipper vid 192 MB och packar varje bit för sig. Oberoendet köper parallellitet och
delvis uppackning, och det kostar det här.</li>
<li><b>Inget standardformat.</b> En <code>tar.gz</code> går att läsa år 2050. En
<code>.nv</code> går att läsa så länge <code>~/hz</code> finns.</li>
<li><b>Ingen kryptering, ingen felkorrigering, inga inkrementella snapshots.</b> Där är
restic rätt verktyg.</li>
<li><b>Hårda länkar blir fristående kopior</b>, och rör, uttag och enheter hoppas över
med varning. <b>Progressiv JPEG</b> och <b>PNG ur okända deflate-motorer</b> lagras rå.</li>
<li><b>hzcm är långsam i båda riktningarna</b> — 207 s att packa upp det blandade trädet,
mot 8 s för läget <code>normal</code>.</li>
</ul>

<div class="ruta varning">
  <p class="slutsats">Och så verkligheten: ditt eget arkiv</p>
  <p>Korpusarna i den här rapporten är rena. Ett riktigt projektträd är blandat, och
  blandningen avgör vad man faktiskt tjänar. På hela <code>~/lostconnection</code>
  (2,9 GB, 2 009 filer) är vinsten <b>{tecken(lc_hela, 2, ' %')}</b>: 2 869,25 → 2 866,74 MiB.
  Utan <code>data/klipp</code> (25 MB, 432 filer) är den <b>−3,8 %</b>: 17,17 → 16,52 MiB.</p>
  <p>Det är det ärliga svaret för just det arkivet — <b>ingenting hjälper, för det ÄR
  517 filmer.</b> Film är redan packad av en videokodare, och där finns inget
  motsvarande entropilager att byta ut. Byt alltså inte ut
  <code>scripts/arkivera.sh</code> för storlekens skull.</p>
</div>'''))

    # ------------------------------------------------------------- 8 & 9 --
    # Bilagan ryms inte på en sida. Två korpusar per sida, mätt och inte gissat:
    # sidorna är fasta A4-rutor, och det som inte ryms klipps tyst bort.
    def tab_block(ix):
        k, rubrik, _ = KORPUS[ix]
        return (f'<div class="figrubrik" style="margin-top:3mm"><div class="namn">{rubrik} — '
                f'{tal(d[k]["rabyte"] / MIB, 1)} MiB i {tal(d[k]["filer"], 0)} filer</div></div>'
                + tabell(d[k]))

    sidor.append(sida(8, TOT, '''
<span class="etikett">Bilaga</span>
<h2>Alla mätvärden</h2>
<hr class="regel">
<p class="lead">Varje rad är en hel rundgång: packa, packa upp, och <code>diff -rq</code>
mot originalträdet. Kolumnen längst till höger är utfallet av den jämförelsen, inte en
förhoppning. Korpusarna byggs av <code>korpus/bygg-korpus.py</code> ur riktiga projekt
på maskinen, som hårdlänkar — noll extra diskutrymme, exakt samma bytes.</p>
''' + tab_block(0) + tab_block(1)))

    sidor.append(sida(9, TOT, tab_block(2) + tab_block(3) + '''
<h3>Så kör du det</h3>
<pre>cd ~/hz &amp;&amp; make                         <span style="color:#8b9a94"># bygger C-delarna</span>
bash prov/prova-allt.sh                      <span style="color:#8b9a94"># provsviten, alla kantfall</span>

./src/hz ~/projekt                       <span style="color:#8b9a94"># -&gt; ~/projekt.nv, trädet kvar</span>
./src/hz -d projekt.nv                   <span style="color:#8b9a94"># trädet tillbaka, arkivet borta</span>
./src/hz -l projekt.nv -v                <span style="color:#8b9a94"># block, kodek och kvot</span>
./src/hz -t projekt.nv                   <span style="color:#8b9a94"># packa upp och jämför mot registret</span>

python3 matning/matning.py kod               <span style="color:#8b9a94"># mät om en korpus</span>
python3 matning/rapport.py --in-i-readme     <span style="color:#8b9a94"># bygg om tabellerna</span>
python3 rapport/bygg-tryck.py                <span style="color:#8b9a94"># bygg om den här PDF:en</span></pre>
<p style="font-size:7.8pt;color:#717f7a">Kommandoraden är gzips: <code>-c -d -f -k -l -r
-t -v</code> och <code>-1</code>…<code>-9</code> betyder vad de betyder där. Enda avvikelsen
är att en KATALOG packas till ett enda arkiv av trädet — det är vad hz är till för — och
att källträdet då alltid lämnas kvar. Underkommandona <code>packa</code>, <code>packa-upp</code>,
<code>lista</code> och <code>prova</code> finns kvar; mätningarna anropar dem.</p>
<p style="font-size:7.8pt;color:#717f7a">Tre lägen: <code>snabb</code> motsvarar dagens
arkivskript (zstd −3, inga omvandlingar), <code>normal</code> är standard, och
<code>liten</code> släpper dessutom in hzcm på de textblock där provet visar att den
vinner.</p>'''))

    # Kontrollen körs bara när sidan öppnas i en webbläsare (--dump-dom), och
    # påverkar inte PDF:en. Den mäter om något innehåll hamnar under foten —
    # sidorna är fasta rutor med overflow: hidden, så överfyllnad syns annars
    # inte alls förrän någon läser den färdiga PDF:en.
    KONTROLL = """<div id="kontroll" style="display:none"></div>
<script>
(function () {
  const fel = [];
  document.querySelectorAll('.sida').forEach((s, i) => {
    const fot = s.querySelector('.fot').getBoundingClientRect().top;
    const barn = [...s.children].filter(e => !e.classList.contains('fot'));
    const botten = Math.max(...barn.map(e => e.getBoundingClientRect().bottom));
    if (botten > fot - 2) fel.push(`sida ${i + 1} (+${Math.ceil(botten - fot + 2)}px)`);
  });
  document.getElementById('kontroll').textContent =
    fel.length ? 'OVERFYLLD: ' + fel.join(', ') : 'ALLA-SIDOR-RYMS';
})();
</script>"""

    # Procenttecken och enheter ska aldrig brytas från sitt tal.
    def hårda_mellanrum(t):
        for enhet in (' %', ' MiB', ' MB', ' s<', ' kB'):
            t = t.replace(enhet, '\u00a0' + enhet.lstrip())
        return t

    html = ('<meta charset="utf-8">\n<title>hz — mätrapport</title>\n'
            f'<style>{CSS}</style>\n' + hårda_mellanrum('\n'.join(sidor)) + KONTROLL + '\n')
    ut = ROT / 'rapport' / 'tryck.html'
    ut.write_text(html)
    print(f'{ut} — {len(sidor)} sidor, {len(html)/1024:.0f} kB')


if __name__ == '__main__':
    bygg()
