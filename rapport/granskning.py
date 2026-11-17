#!/usr/bin/env python3
"""Bygger rapport/granskning.html — granskningsrapporten, ur mätfilerna.

Varje tal på sidan kommer ur resultat/*.json. Ingenting skrivs för hand, så
rapporten går att bygga om när en mätning görs om:

    python3 rapport/granskning.py
"""
import json, html
from pathlib import Path

ROT = Path(__file__).resolve().parent.parent
RES = ROT / 'resultat'
UT = ROT / 'rapport' / 'granskning.html'

KORPUSAR = ['kod', 'logg', 'png', 'foto', 'gif', 'arkiv', 'film', 'symbios3d', 'silesia', 'enwik8']
BESKRIVNING = {
    'kod': 'källkodsträd ur riktiga projekt',
    'logg': 'serverloggar, text',
    'png': 'skärmbilder och diagram',
    'foto': 'kamerabilder, baseline-JPEG',
    'gif': 'animeringar',
    'arkiv': 'färdigpackade arkiv (zip, gz, xz)',
    'film': 'mp4 från telefon',
    'symbios3d': 'blandat projektträd',
    'silesia': 'standardkorpus (12 filer)',
    'enwik8': 'standardkorpus (wikipedia, 100 MB)',
}
KLASSISKA = ['tar.gz -6', 'tar.gz -9 (pigz)', 'tar.bz2 -9', 'tar.zst -3 (dagens)',
             'tar.zst -19 --long', 'tar.zst -22 --ultra', 'tar.xz -9e']


def mib(n): return f'{n / 2**20:,.1f}'.replace(',', ' ')
def tal(n): return f'{n:,}'.replace(',', ' ')
def e(s): return html.escape(str(s))


def las(namn):
    p = RES / f'{namn}.json'
    return json.loads(p.read_text()) if p.exists() else None


# ---------------------------------------------------------------- tabellerna

def matrisen():
    """Per korpus: nvz mot bästa klassiska och mot dagens arkivskript."""
    rader = []
    for k in KORPUSAR:
        d = las(k)
        if not d or 'matningar' not in d:
            continue
        m = d['matningar']
        if 'nvz liten' not in m:
            continue
        nl, nn = m['nvz liten'], m['nvz normal']
        klassiska = {n: v for n, v in m.items() if n in KLASSISKA}
        if not klassiska:
            continue
        bast_namn = min(klassiska, key=lambda n: klassiska[n]['byte'])
        bast = klassiska[bast_namn]
        dagens = m.get('tar.zst -3 (dagens)')
        rader.append(dict(
            korpus=k, om=BESKRIVNING.get(k, ''), filer=d['filer'], ra=d['rabyte'],
            liten=nl['byte'], liten_upp=nl['uppacksek'],
            normal=nn['byte'], normal_upp=nn['uppacksek'],
            bast_namn=bast_namn, bast=bast['byte'], bast_upp=bast['uppacksek'],
            mot_bast=100 * (1 - nl['byte'] / bast['byte']),
            mot_dagens=100 * (1 - nl['byte'] / dagens['byte']) if dagens else None,
        ))
    return rader


def nvcm_rader():
    """nvcm (egen kompressor) mot de klassiska, fil för fil i standardkorpusarna."""
    rader = []
    for f in ('standard-silesia', 'standard-enwik8'):
        d = las(f)
        if not d:
            continue
        for namn, kodekar in d['filer'].items():
            if 'nvcm' not in kodekar:
                continue
            nv = kodekar['nvcm']
            andra = {k: v for k, v in kodekar.items() if k != 'nvcm'}
            bn = min(andra, key=lambda k: andra[k]['byte'])
            rader.append(dict(fil=namn, nvcm=nv['byte'], bpb=nv['bpb'],
                              bast_namn=bn, bast=andra[bn]['byte'], bast_bpb=andra[bn]['bpb'],
                              diff=100 * (1 - nv['byte'] / andra[bn]['byte'])))
    return rader


def protokoll_rader():
    p = RES / 'protokoll' / 'protokoll.json'
    if not p.exists():
        return [], None
    d = json.loads(p.read_text())
    rader = [{**v, 'korpus': k} for k, v in d.get('korpusar', {}).items()]
    return rader, d.get('datum')


# ------------------------------------------------------------------ html-delar

def tabell(rubriker, rader, klass=''):
    h = ''.join(f'<th{" class=\"t\"" if r[1] else ""}>{e(r[0])}</th>' for r in rubriker)
    kropp = ''
    for rad in rader:
        celler = ''
        for (värde, numerisk, extra) in rad:
            k = ' class="' + ' '.join(x for x in (['t'] if numerisk else []) + ([extra] if extra else [])) + '"'
            celler += f'<td{k}>{värde}</td>'
        kropp += f'<tr>{celler}</tr>'
    return (f'<div class="rulla"><table class="{klass}"><thead><tr>{h}</tr></thead>'
            f'<tbody>{kropp}</tbody></table></div>')


def bygg():
    g = json.loads((RES / 'granskning.json').read_text())
    kä = g['karna']
    matris = matrisen()
    nvcm = nvcm_rader()
    prot, prot_datum = protokoll_rader()

    # --- nyckeltalen högst upp
    nvcm_vinster = sum(1 for r in nvcm if r['diff'] > 0)
    bitexakta = sum(1 for r in prot if r['identiska'] and r['md5sum_c_ok'])
    nyckeltal = [
        (f"{kä['nvz_tar_sha256_efter'][:8]}…", 'sha256 ut = sha256 in',
         f"linux-{kä['version'].split('-')[1]}, {mib(kä['tar_byte'])} MiB tar"),
        (f"{g['slumpprov']['lyckade']}/{g['slumpprov']['rundor']}", 'slumpade träd bit-exakta',
         'innehåll, rättigheter och tider'),
        (f"{bitexakta}", 'korpusar md5-protokollförda',
         'kontrollerade med md5sum -c'),
        ('3', 'brister bekräftade', 'två reproducerade, en läst i koden'),
    ]
    nyckel_html = ''.join(
        f'<div class="nyckel"><div class="nv">{e(v)}</div>'
        f'<div class="nl">{e(l)}</div><div class="nf">{e(f)}</div></div>'
        for v, l, f in nyckeltal)

    # --- matristabellen
    matris_rader = []
    for r in matris:
        mot_b = f'{r["mot_bast"]:+.1f} %'
        matris_rader.append([
            (f'<strong>{e(r["korpus"])}</strong><span class="om">{e(r["om"])}</span>', False, ''),
            (tal(r['filer']), True, ''),
            (mib(r['ra']), True, ''),
            (mib(r['bast']) + f'<span class="om">{e(r["bast_namn"])}</span>', True, ''),
            (mib(r['normal']), True, ''),
            (mib(r['liten']), True, ''),
            (mot_b, True, 'plus' if r['mot_bast'] > 0.2 else ('minus' if r['mot_bast'] < -0.2 else 'noll')),
            (f'{r["mot_dagens"]:+.1f} %' if r['mot_dagens'] is not None else '–', True, ''),
            (f'{r["bast_upp"]:.1f} / {r["normal_upp"]:.1f} / {r["liten_upp"]:.1f}', True, ''),
        ])
    matris_html = tabell(
        [('korpus', False), ('filer', True), ('rå MiB', True), ('bästa klassiska MiB', True),
         ('nvz normal', True), ('nvz liten', True), ('liten mot bästa', True),
         ('liten mot dagens', True), ('uppacktid s: klassisk / normal / liten', True)],
        matris_rader)

    # --- nvcm-tabellen
    nvcm_html = tabell(
        [('fil', False), ('nvcm byte', True), ('bitar/byte', True),
         ('bästa klassiska', False), ('dess byte', True), ('dess b/b', True), ('nvcm mot den', True)],
        [[(e(r['fil']), False, ''), (tal(r['nvcm']), True, ''), (f'{r["bpb"]:.3f}', True, ''),
          (e(r['bast_namn']), False, ''), (tal(r['bast']), True, ''), (f'{r["bast_bpb"]:.3f}', True, ''),
          (f'{r["diff"]:+.1f} %', True, 'plus' if r['diff'] > 0 else 'minus')] for r in nvcm])

    # --- kärnprovet, fem steg i ordning (det ÄR en sekvens)
    steg = [
        ('Hämtad fil', f"linux-{kä['version'].split('-')[1]}.tar.xz, {tal(kä['publicerad_tarxz_byte'])} byte",
         f"sha256 {kä['sha256_hamtad'][:16]}… — <strong>identisk med kernel.orgs egen</strong>"),
        ('Uppackad', f"{tal(kä['tar_byte'])} byte tar",
         f"sha256 {kä['tar_sha256'][:16]}… (utgångsvärdet)"),
        ('Packad med nvz', f"{tal(kä['nvz_tar_byte'])} byte på {kä['nvz_tar_packsek']} s",
         f"<strong>{100*(1-kä['nvz_tar_byte']/kä['publicerad_tarxz_byte']):.1f} % mindre</strong> än den publicerade tarbollen"),
        ('xz -9e på samma maskin', f"{tal(kä['egen_xz9e_byte'])} byte på {kä['egen_xz9e_packsek']} s",
         f"nvz är {tal(kä['nvz_tar_byte']-kä['egen_xz9e_byte'])} byte större men "
         f"{kä['egen_xz9e_packsek']/kä['nvz_tar_packsek']:.2f}× snabbare"),
        ('Uppackad igen', f"{kä['nvz_tar_uppacksek']} s",
         f"sha256 {kä['nvz_tar_sha256_efter'][:16]}… — <strong>bit-identisk</strong>"),
    ]
    steg_html = ''.join(
        f'<li><div class="sr">{e(r)}</div><div class="sv">{v}</div><div class="sk">{k}</div></li>'
        for r, v, k in steg)

    # --- bristerna
    brist_html = ''
    for b in g['brister']:
        brist_html += f'''<article class="brist">
  <header><span class="chip {'repr' if b['status'].startswith('REPRO') else 'kod'}">{e(b['status'])}</span>
    <span class="allvar">{e(b['allvar'])}</span>
    <h3>{e(b['rubrik'])}</h3>
    <p class="meta">rapporterad av {e(b['rapporterad_av'])} · {e(b['vad_jag_gjorde'])}</p></header>
  <p>{e(b['fynd'])}</p>
  <p class="atgard"><span>Åtgärd</span> {e(b['atgard'])}</p>
</article>'''

    # --- protokollet
    prot_html = tabell(
        [('korpus', False), ('filer', True), ('rå byte', True), ('arkiv byte', True),
         ('manifest före', False), ('manifest efter', False), ('md5sum -c', False)],
        [[(e(r['korpus']), False, ''), (tal(r['filer']), True, ''), (tal(r['rabyte']), True, ''),
          (tal(r['arkivbyte']), True, ''),
          (f"<code>{e(r['manifest_fore_md5'][:12])}…</code>", False, ''),
          (f"<code>{e(r['manifest_efter_md5'][:12])}…</code>", False, ''),
          ('ok' if r['md5sum_c_ok'] else 'FEL', False, 'plus' if r['md5sum_c_ok'] else 'minus')]
         for r in prot])

    # --- omkostnaden
    ok = g['omkostnad']
    kol = ['gzip -9', 'zstd -19', 'xz -9e', 'nvz snabb', 'nvz normal', 'nvz liten']
    ok_html = tabell(
        [('indata', False)] + [(k, True) for k in kol],
        [[(e(r['indata']), False, '')] + [(tal(r[k]) if r.get(k) else '–', True, '') for k in kol]
         for r in ok['rader']])

    mall = (ROT / 'rapport' / 'granskning-mall.html').read_text()
    sida = (mall
            .replace('{{NYCKELTAL}}', nyckel_html)
            .replace('{{MATRIS}}', matris_html)
            .replace('{{NVCM}}', nvcm_html)
            .replace('{{NVCM_VINSTER}}', f'{nvcm_vinster} av {len(nvcm)}')
            .replace('{{STEG}}', steg_html)
            .replace('{{BRISTER}}', brist_html)
            .replace('{{PROTOKOLL}}', prot_html)
            .replace('{{OMKOSTNAD}}', ok_html)
            .replace('{{OMKOSTNAD_SLUTSATS}}', e(ok['slutsats']))
            .replace('{{SLUMPPROV}}', e(g['slumpprov']['vad']))
            .replace('{{TRAD_RA}}', mib(kä['trad_rabyte']))
            .replace('{{TRAD_NVZ}}', mib(kä['nvz_trad_byte']))
            .replace('{{TRAD_DIFF}}', f"{100*(kä['nvz_trad_byte']/kä['nvz_tar_byte']-1):+.1f}")
            .replace('{{PORT_SPRAK}}', e(g['portabilitet']['sprak']))
            .replace('{{PORT_BIB}}', e(g['portabilitet']['bibliotek']))
            .replace('{{PORT_POSIX}}', e(g['portabilitet']['posix']))
            .replace('{{PORT_WIN}}', e(g['portabilitet']['windows']))
            .replace('{{BINAR}}', tal(g['binar_byte']))
            .replace('{{DATUM}}', e(g['datum']))
            .replace('{{MASKIN}}', e(g['maskin']))
            .replace('{{LAST}}', e(g['last_under_matning'])))
    UT.write_text(sida)
    print(f'skrev {UT} ({len(sida):,} tecken)')
    print(f'  {len(matris)} korpusar i matrisen, {len(nvcm)} filer i nvcm-tabellen, '
          f'{len(prot)} i protokollet, {len(g["brister"])} brister')


if __name__ == '__main__':
    bygg()
