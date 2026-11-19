#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Gör rapport/bred-provning.md till en PDF.

    python3 rapport/bred-provning-pdf.py

Den stora rapporten (rapport/bygg-tryck.py) är handlagda A4-rutor med fast
innehåll per sida. Det går inte att hälla en text som växer i den utan att
något klipps bort tyst. Den här vägen är därför en annan: markdown till
enkel HTML, och sedan Chromes utskrift med FLYTANDE sidor — innehållet
bestämmer sidantalet i stället för tvärtom.

Två saker är inte valfria, båda inlärda:

  TYPSNITT. Chrome bäddar in varje @font-face-typsnitt som Type 3, och
  sådana underkänns av tryckerier. Därför refereras bara familjer som finns
  installerade i systemet (Archivo, Noto Serif, Liberation Mono), och
  pdffonts kontrolleras efteråt.

  TABELLER. Rapporten är mest tabeller, och en tabell som bryts mitt i
  tappar sin rubrikrad. break-inside: avoid på raderna och thead som
  upprepas löser det.
"""
import html, re, subprocess, sys
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
# Utan flagga byggs den svenska rapporten; `--en` bygger den engelska.
BAS = ('broad-evaluation' if '--en' in sys.argv
       else 'aterskapningsprotokoll' if '--protokoll' in sys.argv
       else 'bred-provning')
MD = ROT / 'rapport' / f'{BAS}.md'
HTML = ROT / 'rapport' / f'{BAS}.html'
PDF = ROT / 'rapport' / f'{BAS}.pdf'


def slug(t: str) -> str:
    t = re.sub(r'[`*]', '', t).strip().lower()
    return re.sub(r'[^a-z0-9åäö]+', '-', t).strip('-')


def infogad(t: str) -> str:
    """Inline-markdown. Koden först, så att * inuti kod inte blir kursiv."""
    bitar, rest = [], t
    ut = ''
    while True:
        m = re.search(r'`([^`]+)`', rest)
        if not m:
            ut += _stil(rest); break
        ut += _stil(rest[:m.start()]) + f'<code>{html.escape(m.group(1))}</code>'
        rest = rest[m.end():]
    return ut


def _stil(t: str) -> str:
    t = html.escape(t)
    t = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'<a href="\2">\1</a>', t)
    t = re.sub(r'\*\*([^*]+)\*\*', r'<strong>\1</strong>', t)
    t = re.sub(r'(?<![\w*])\*([^*\n]+)\*(?![\w*])', r'<em>\1</em>', t)
    return t


def tabell(rader: list[str]) -> str:
    def celler(r): return [c.strip() for c in r.strip().strip('|').split('|')]
    huvud = celler(rader[0])
    just = []
    for c in celler(rader[1]):
        just.append('center' if c.startswith(':') and c.endswith(':')
                    else 'right' if c.endswith(':') else 'left')
    ut = ['<table><thead><tr>']
    for i, c in enumerate(huvud):
        ut.append(f'<th style="text-align:{just[i] if i < len(just) else "left"}">{infogad(c)}</th>')
    ut.append('</tr></thead><tbody>')
    for r in rader[2:]:
        ut.append('<tr>')
        for i, c in enumerate(celler(r)):
            ut.append(f'<td style="text-align:{just[i] if i < len(just) else "left"}">{infogad(c)}</td>')
        ut.append('</tr>')
    ut.append('</tbody></table>')
    return ''.join(ut)


def till_html(md: str) -> str:
    rader = md.splitlines()
    ut, i = [], 0
    while i < len(rader):
        r = rader[i]
        if r.startswith('<!--'):                      # markörerna är osynliga
            i += 1; continue
        if r.startswith('```'):
            kropp = []
            i += 1
            while i < len(rader) and not rader[i].startswith('```'):
                kropp.append(html.escape(rader[i])); i += 1
            i += 1
            ut.append('<pre><code>' + '\n'.join(kropp) + '</code></pre>')
            continue
        if re.match(r'^#{1,4} ', r):
            n = len(r) - len(r.lstrip('#'))
            text = r[n:].strip()
            ut.append(f'<h{n} id="{slug(text)}">{infogad(text)}</h{n}>')
            i += 1; continue
        if r.strip() == '---':
            ut.append('<hr>'); i += 1; continue
        if r.startswith('|') and i + 1 < len(rader) and re.match(r'^\|[\s:|-]+\|$', rader[i+1]):
            block = []
            while i < len(rader) and rader[i].startswith('|'):
                block.append(rader[i]); i += 1
            ut.append(tabell(block)); continue
        if re.match(r'^(\d+\.|[-*]) ', r):
            ordnad = bool(re.match(r'^\d+\.', r))
            poster, nuvarande = [], ''
            while i < len(rader) and (re.match(r'^(\d+\.|[-*]) ', rader[i]) or
                                      (rader[i].startswith('   ') and nuvarande)):
                if re.match(r'^(\d+\.|[-*]) ', rader[i]):
                    if nuvarande: poster.append(nuvarande)
                    nuvarande = re.sub(r'^(\d+\.|[-*]) ', '', rader[i])
                else:
                    nuvarande += ' ' + rader[i].strip()
                i += 1
            if nuvarande: poster.append(nuvarande)
            t = 'ol' if ordnad else 'ul'
            ut.append(f'<{t}>' + ''.join(f'<li>{infogad(p)}</li>' for p in poster) + f'</{t}>')
            continue
        if not r.strip():
            i += 1; continue
        stycke = []
        while i < len(rader) and rader[i].strip() and not rader[i].startswith(('|', '#', '```', '<!--')) \
                and not re.match(r'^(\d+\.|[-*]) ', rader[i]) and rader[i].strip() != '---':
            stycke.append(rader[i].strip()); i += 1
        ut.append('<p>' + infogad(' '.join(stycke)) + '</p>')
    return '\n'.join(ut)


STIL = """
@page { size: A4; margin: 18mm 15mm 16mm 15mm; }
* { box-sizing: border-box; }
body { font-family: "Noto Serif", serif; font-size: 9.6pt; line-height: 1.5;
       color: #15130f; margin: 0; }
h1 { font-family: Archivo, sans-serif; font-size: 23pt; font-weight: 800;
     letter-spacing: -0.02em; margin: 0 0 2mm; line-height: 1.1; }
h2 { font-family: Archivo, sans-serif; font-size: 14pt; font-weight: 700;
     margin: 9mm 0 2.5mm; padding-top: 2mm; border-top: 1.5pt solid #15130f;
     break-after: avoid; letter-spacing: -0.01em; }
h3 { font-family: Archivo, sans-serif; font-size: 11pt; font-weight: 650;
     margin: 6mm 0 2mm; break-after: avoid; }
h4 { font-family: Archivo, sans-serif; font-size: 9.8pt; margin: 4mm 0 1.5mm;
     break-after: avoid; }
p { margin: 0 0 2.6mm; text-align: justify; hyphens: auto; }
strong { font-weight: 700; }
code { font-family: "Liberation Mono", monospace; font-size: 0.86em;
       background: #f1eee8; padding: 0.5pt 1.6pt; border-radius: 1.5pt; }
pre { font-family: "Liberation Mono", monospace; font-size: 7.9pt;
      background: #f6f4ef; border-left: 2pt solid #c9c2b4; padding: 2.5mm 3mm;
      margin: 0 0 3mm; overflow-x: hidden; white-space: pre-wrap;
      break-inside: avoid; line-height: 1.35; }
pre code { background: none; padding: 0; font-size: 1em; }
table { border-collapse: collapse; width: 100%; margin: 0 0 3.5mm;
        font-size: 8.3pt; font-family: Archivo, sans-serif; }
thead { display: table-header-group; }
th { font-weight: 650; text-align: left; border-bottom: 1pt solid #15130f;
     padding: 1.2mm 1.6mm; white-space: nowrap; }
td { border-bottom: 0.4pt solid #ddd8cd; padding: 1.1mm 1.6mm;
     vertical-align: top; }
tr { break-inside: avoid; }
tbody tr:nth-child(even) { background: #faf8f4; }
ul, ol { margin: 0 0 3mm; padding-left: 5mm; }
li { margin-bottom: 1.4mm; }
a { color: #15130f; text-decoration: none; border-bottom: 0.4pt dotted #8a8578; }
hr { border: none; border-top: 0.6pt solid #c9c2b4; margin: 6mm 0; }
h2, h3 { break-inside: avoid; }
"""


def main():
    md = MD.read_text()
    kropp = till_html(md)
    sprak = 'en' if '--en' in sys.argv else 'sv'
    titel = ('nvz — broad evaluation' if sprak == 'en'
             else 'nvz — återskapningsprotokoll' if '--protokoll' in sys.argv
             else 'nvz — bred provning')
    HTML.write_text(f"""<!doctype html>
<html lang="{sprak}"><head><meta charset="utf-8">
<title>{titel}</title>
<style>{STIL}</style></head>
<body>{kropp}</body></html>""")
    r = subprocess.run(['google-chrome', '--headless', '--disable-gpu', '--no-sandbox',
                        '--no-pdf-header-footer', '--virtual-time-budget=12000',
                        f'--print-to-pdf={PDF}', f'file://{HTML}'],
                       capture_output=True, text=True)
    if r.returncode or not PDF.exists():
        print(r.stderr[-1500:], file=sys.stderr); sys.exit(1)
    info = subprocess.run(['pdfinfo', str(PDF)], capture_output=True, text=True).stdout
    sidor = next((l.split()[1] for l in info.splitlines() if l.startswith('Pages:')), '?')
    typsnitt = subprocess.run(['pdffonts', str(PDF)], capture_output=True, text=True).stdout
    print(f'{PDF}\n  {PDF.stat().st_size/1024:.0f} kB, {sidor} sidor')
    if 'Type 3' in typsnitt:
        print('  TYPE 3 HITTAD:\n' + typsnitt, file=sys.stderr); sys.exit(1)
    print('  typsnitt ok, inga Type 3')
    for rad in typsnitt.splitlines()[2:]:
        if rad.strip(): print('    ' + rad.split()[0])


if __name__ == '__main__':
    main()
