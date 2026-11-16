/* nvjpg — bit-exakt förlustfri ompackning av baseline-JPEG.
 *
 *     nvjpg packa   < bild.jpg > bild.nvj      (slutkod 2 = kan inte, lagra rått)
 *     nvjpg packaupp < bild.nvj > bild.jpg
 *
 * VARFÖR. En JPEG är DCT-koefficienter entropikodade med Huffman enligt en
 * tabell som ligger i filen. Huffman kan inte koda en symbol på mindre än en
 * bit, och den vet ingenting om att blocket bredvid såg nästan likadant ut.
 * Koefficienterna i sig är starkt mönstrade: hur många som är nollskilda i ett
 * block kan gissas ur grannblocken, och hur stor en koefficient är kan gissas
 * ur samma koefficient i blocket till vänster och ovanför.
 *
 * nvjpg avkodar Huffman-strömmen till koefficienter och kodar om dem med en
 * ADAPTIV ARITMETISK KODARE som har just de kontexterna. Pixlarna rörs aldrig
 * — detta är inte omkodning av bilden, det är omkodning av entropilagret.
 * Samma koefficienter kodas tillbaka med filens egna Huffman-tabeller, och då
 * uppstår exakt samma bytes igen.
 *
 * BEVISET LIGGER I PACKNINGEN. Innan något skrivs ut kodar packaren om
 * koefficienterna till Huffman igen och jämför med originalets bytes. Skiljer
 * något avslutas med slutkod 2 och arkivet lagrar filen rå. En arkivpackare
 * får ha en omvandling den inte klarar; den får aldrig TRO att den klarade.
 *
 * Stöd: baseline sekventiell (SOF0/SOF1), en eller flera komponenter,
 * godtycklig delsampling, omstartsmarkörer. Progressiv JPEG (SOF2) och
 * aritmetiskt kodad JPEG lämnas till råfallet.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ---------------------------------------------------------------- hjälp -- */

static void dö(const char *m) { fprintf(stderr, "nvjpg: %s\n", m); exit(2); }

typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void buf_lägg(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 65536;
        b->d = realloc(b->d, b->cap);
        if (!b->d) dö("slut på minne");
    }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void buf_byte(Buf *b, uint8_t v) { buf_lägg(b, &v, 1); }

static uint8_t *läs_allt(FILE *f, size_t *n) {
    size_t cap = 1 << 20, len = 0; uint8_t *d = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; d = realloc(d, cap); if (!d) dö("slut på minne"); }
        size_t r = fread(d + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    *n = len; return d;
}

/* ------------------------------------------------- aritmetisk kodare ----- */
/* Binär intervallkodare av LZMA-snitt: 11-bitars sannolikheter, adaption med
 * skift 5. Den är liten, snabb och framför allt exakt symmetrisk — avkodaren
 * gör samma uppdateringar i samma ordning som kodaren. */

#define PBITAR 11
#define PINIT  (1 << (PBITAR - 1))
#ifndef BLANDA
#define BLANDA 2   /* 2 = takt som sjunker med antalet observationer */
#endif
#ifndef SNABB
#define SNABB 4
#endif
#ifndef LANGSAM
#define LANGSAM 7
#endif
#ifndef PSKIFT
#define PSKIFT 5
#endif

/* En sannolikhet hålls i två takter. Den snabba fångar att statistiken
 * ändrar sig från bildyta till bildyta, den långsamma minns hela bilden, och
 * medelvärdet är bättre än någon av dem ensam. Samma blandning görs i båda
 * riktningarna, så kodare och avkodare kan inte glida isär. */
#if BLANDA == 2
/* Takten sjunker med antalet observationer: de första bitarna i en kontext
 * flyttar skattningen långt, senare bitar bara putsar. Det ger både snabb
 * start och stabilt slutläge — det som två fasta takter försökte göra, men
 * utan att den långsamma drar medelvärdet mot 50 % hela bilden igenom. */
#ifndef SLUTTAKT
#define SLUTTAKT 5      /* takten planar ut här; högre = längre minne */
#endif
#define TAKTTAK (2 * (SLUTTAKT - 1))
typedef struct { uint16_t p, n; } Sann;
static inline uint32_t p_av(const Sann *p) { return p->p; }
static inline void p_upp(Sann *p, int bit) {
    int sk = 1 + (p->n >> 1);
    if (sk > SLUTTAKT) sk = SLUTTAKT;
    if (!bit) p->p += ((1 << PBITAR) - p->p) >> sk; else p->p -= p->p >> sk;
    if (p->n < TAKTTAK) p->n++;
}
#elif BLANDA == 1
typedef struct { uint16_t snabb, langsam; } Sann;
static inline uint32_t p_av(const Sann *p) { return ((uint32_t)p->snabb + p->langsam) >> 1; }
static inline void p_upp(Sann *p, int bit) {
    if (!bit) { p->snabb += ((1 << PBITAR) - p->snabb) >> SNABB;
                p->langsam += ((1 << PBITAR) - p->langsam) >> LANGSAM; }
    else      { p->snabb -= p->snabb >> SNABB;
                p->langsam -= p->langsam >> LANGSAM; }
}
#define SANN_INIT { PINIT, PINIT }
#else
typedef struct { uint16_t p; } Sann;
static inline uint32_t p_av(const Sann *p) { return p->p; }
static inline void p_upp(Sann *p, int bit) {
    if (!bit) p->p += ((1 << PBITAR) - p->p) >> PSKIFT; else p->p -= p->p >> PSKIFT;
}
#endif

typedef struct { uint64_t låg; uint32_t vidd; uint8_t cache; int64_t cacheantal; Buf ut; } Kodare;

static void kod_init(Kodare *k) {
    k->låg = 0; k->vidd = 0xFFFFFFFFu; k->cache = 0; k->cacheantal = 1;
    k->ut.d = NULL; k->ut.n = k->ut.cap = 0;
}
static void kod_skifta(Kodare *k) {
    if ((uint32_t)(k->låg >> 32) != 0 || (uint32_t)k->låg < 0xFF000000u) {
        int64_t i = k->cacheantal;
        uint8_t bär = (uint8_t)(k->låg >> 32);
        do { buf_byte(&k->ut, (uint8_t)(k->cache + bär)); k->cache = 0xFF; } while (--i);
        k->cache = (uint8_t)((uint32_t)k->låg >> 24);
        k->cacheantal = 0;
    }
    k->cacheantal++;
    k->låg = (uint32_t)k->låg << 8;
}
static void kod_bit(Kodare *k, Sann *p, int bit) {
    uint32_t gräns = (k->vidd >> PBITAR) * p_av(p);
    if (!bit) k->vidd = gräns;
    else { k->låg += gräns; k->vidd -= gräns; }
    p_upp(p, bit);
    while (k->vidd < (1u << 24)) { k->vidd <<= 8; kod_skifta(k); }
}
static void kod_direkt(Kodare *k, uint32_t v, int bitar) {   /* oadapterat */
    for (int i = bitar - 1; i >= 0; i--) {
        k->vidd >>= 1;
        if ((v >> i) & 1) k->låg += k->vidd;
        while (k->vidd < (1u << 24)) { k->vidd <<= 8; kod_skifta(k); }
    }
}
static void kod_slut(Kodare *k) { for (int i = 0; i < 5; i++) kod_skifta(k); }

typedef struct { const uint8_t *d; size_t n, pos; uint32_t vidd, kod; } Avkodare;
static uint8_t av_byte(Avkodare *a) { return a->pos < a->n ? a->d[a->pos++] : 0; }
static void av_init(Avkodare *a, const uint8_t *d, size_t n) {
    a->d = d; a->n = n; a->pos = 0; a->vidd = 0xFFFFFFFFu; a->kod = 0;
    av_byte(a);
    for (int i = 0; i < 4; i++) a->kod = (a->kod << 8) | av_byte(a);
}
static int av_bit(Avkodare *a, Sann *p) {
    uint32_t gräns = (a->vidd >> PBITAR) * p_av(p);
    int bit;
    if (a->kod < gräns) { a->vidd = gräns; bit = 0; }
    else { a->kod -= gräns; a->vidd -= gräns; bit = 1; }
    p_upp(p, bit);
    while (a->vidd < (1u << 24)) { a->vidd <<= 8; a->kod = (a->kod << 8) | av_byte(a); }
    return bit;
}
static uint32_t av_direkt(Avkodare *a, int bitar) {
    uint32_t v = 0;
    for (int i = 0; i < bitar; i++) {
        a->vidd >>= 1;
        uint32_t t = (a->kod - a->vidd) >> 31;         /* 1 om kod < vidd */
        if (!t) a->kod -= a->vidd;
        v = (v << 1) | (1 - t);
        while (a->vidd < (1u << 24)) { a->vidd <<= 8; a->kod = (a->kod << 8) | av_byte(a); }
    }
    return v;
}

/* Träd: kodar ett tal med `bitar` bitar genom en binär trädstruktur där varje
 * nod har egen sannolikhet. Ger sammanhangskänslig kodning av små heltal. */
static void kod_träd(Kodare *k, Sann *p, int bitar, uint32_t v) {
    uint32_t m = 1;
    for (int i = bitar - 1; i >= 0; i--) {
        int bit = (v >> i) & 1;
        kod_bit(k, &p[m], bit);
        m = (m << 1) | bit;
    }
}
static uint32_t av_träd(Avkodare *a, Sann *p, int bitar) {
    uint32_t m = 1;
    for (int i = 0; i < bitar; i++) m = (m << 1) | av_bit(a, &p[m]);
    return m - (1u << bitar);
}

/* ------------------------------------------------------- JPEG-struktur -- */

typedef struct {
    uint8_t bits[17], vals[256];
    int mincode[17], maxcode[18], valptr[17];
    uint16_t kod[256]; uint8_t storlek[256];     /* för omkodning */
    int finns;
} Huff;

typedef struct {
    int id, h, v, tq, td, ta;
    int bw, bh;                 /* blockplanets storlek */
    int16_t *koef;
} Komp;

typedef struct {
    int W, H, nk, hmax, vmax, mcux, mcuy, dri, prec;
    Komp k[4];
    Huff hdc[4], hac[4];
    size_t scan_start, scan_slut;     /* entropidatans byteintervall */
} Bild;

static const uint8_t SICKSACK[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void bygg_huff(Huff *h) {
    int kod = 0, k = 0;
    int storlekar[257]; int n = 0;
    for (int l = 1; l <= 16; l++) for (int i = 0; i < h->bits[l]; i++) storlekar[n++] = l;
    storlekar[n] = 0;
    /* Kodtabellen för OMKODNING indexeras på symbolvärdet, inte på platsen i
     * vals[] — det är symbolen koda_huffman() har i handen. */
    int si = storlekar[0], i = 0; kod = 0;
    memset(h->kod, 0, sizeof h->kod); memset(h->storlek, 0, sizeof h->storlek);
    while (storlekar[i]) {
        while (storlekar[i] == si) { h->kod[h->vals[i]] = (uint16_t)kod++; h->storlek[h->vals[i]] = (uint8_t)si; i++; }
        kod <<= 1; si++;
    }
    k = 0; kod = 0;
    for (int l = 1; l <= 16; l++) {
        if (h->bits[l]) {
            h->valptr[l] = k; h->mincode[l] = kod;
            k += h->bits[l]; kod += h->bits[l];
            h->maxcode[l] = kod - 1;
        } else h->maxcode[l] = -1;
        kod <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

/* --------------------------------------------------- bitläsning i scan -- */

typedef struct { const uint8_t *d; size_t pos, slut; uint32_t b; int n; int markör; } Bitläsare;

static int bl_bit(Bitläsare *r) {
    if (r->n == 0) {
        if (r->pos >= r->slut) { r->markör = 1; return 0; }
        uint8_t c = r->d[r->pos++];
        if (c == 0xFF) {
            uint8_t c2 = r->pos < r->slut ? r->d[r->pos] : 0xD9;
            if (c2 == 0x00) r->pos++;
            else { r->markör = 1; r->pos--; return 0; }
        }
        r->b = c; r->n = 8;
    }
    r->n--;
    return (r->b >> r->n) & 1;
}
static int bl_bitar(Bitläsare *r, int n) {
    int v = 0; while (n--) v = (v << 1) | bl_bit(r); return v;
}
static int bl_huff(Bitläsare *r, Huff *h) {
    int kod = bl_bit(r), l = 1;
    while (kod > h->maxcode[l]) { kod = (kod << 1) | bl_bit(r); if (++l > 16) return -1; }
    return h->vals[h->valptr[l] + kod - h->mincode[l]];
}
static int förläng(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

/* --------------------------------------------------- bitskrivning ut ---- */

typedef struct { Buf b; uint32_t ack; int n; } Bitskrivare;
static void bs_bitar(Bitskrivare *w, int kod, int n) {
    w->ack = (w->ack << n) | (uint32_t)(kod & ((1 << n) - 1));
    w->n += n;
    while (w->n >= 8) {
        uint8_t c = (uint8_t)(w->ack >> (w->n - 8));
        buf_byte(&w->b, c);
        if (c == 0xFF) buf_byte(&w->b, 0x00);
        w->n -= 8;
    }
}
static void bs_fyll(Bitskrivare *w) {          /* ettor ut till bytegräns */
    if (w->n) bs_bitar(w, (1 << (8 - w->n)) - 1, 8 - w->n);
    w->n = 0;
}

/* ------------------------------------------------------- huvudparsning -- */

static size_t parsa(Bild *b, const uint8_t *d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) dö("ingen SOI");
    size_t i = 2;
    while (i + 3 < n) {
        if (d[i] != 0xFF) dö("markör väntades");
        uint8_t m = d[i + 1];
        if (m == 0xFF) { i++; continue; }
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
        size_t len = ((size_t)d[i + 2] << 8) | d[i + 3];
        const uint8_t *p = d + i + 4; size_t pl = len - 2;
        switch (m) {
        case 0xC0: case 0xC1: {
            b->prec = p[0];
            b->H = (p[1] << 8) | p[2]; b->W = (p[3] << 8) | p[4];
            b->nk = p[5];
            if (b->nk > 4) dö("för många komponenter");
            for (int c = 0; c < b->nk; c++) {
                b->k[c].id = p[6 + c * 3];
                b->k[c].h = p[7 + c * 3] >> 4; b->k[c].v = p[7 + c * 3] & 15;
                b->k[c].tq = p[8 + c * 3];
                if (b->k[c].h < 1 || b->k[c].v < 1) dö("ogiltig delsampling");
            }
            break; }
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            dö("inte baseline");                      /* progressiv/aritmetisk */
            /* faller aldrig igenom: dö() avslutar */
        case 0xC4: {
            size_t q = 0;
            while (q < pl) {
                int tc = p[q] >> 4, th = p[q] & 15; q++;
                if (th > 3) dö("huffmantabell utanför intervall");
                Huff *h = tc ? &b->hac[th] : &b->hdc[th];
                memset(h, 0, sizeof *h);
                int tot = 0;
                for (int l = 1; l <= 16; l++) { h->bits[l] = p[q + l - 1]; tot += h->bits[l]; }
                q += 16;
                for (int v = 0; v < tot; v++) h->vals[v] = p[q + v];
                q += tot;
                bygg_huff(h); h->finns = 1;
            }
            break; }
        case 0xDD: b->dri = (p[0] << 8) | p[1]; break;
        case 0xDA: {
            int ns = p[0];
            if (ns != b->nk) dö("scan täcker inte alla komponenter");
            for (int s = 0; s < ns; s++) {
                int id = p[1 + s * 2];
                int c; for (c = 0; c < b->nk && b->k[c].id != id; c++) ;
                if (c == b->nk) dö("okänd komponent i scan");
                b->k[c].td = p[2 + s * 2] >> 4; b->k[c].ta = p[2 + s * 2] & 15;
            }
            b->scan_start = i + 2 + len;
            return b->scan_start; }
        default: break;
        }
        i += 2 + len;
    }
    dö("ingen scan");
    return 0;
}

static void avsätt(Bild *b) {
    b->hmax = b->vmax = 1;
    for (int c = 0; c < b->nk; c++) {
        if (b->k[c].h > b->hmax) b->hmax = b->k[c].h;
        if (b->k[c].v > b->vmax) b->vmax = b->k[c].v;
    }
    b->mcux = (b->W + 8 * b->hmax - 1) / (8 * b->hmax);
    b->mcuy = (b->H + 8 * b->vmax - 1) / (8 * b->vmax);
    for (int c = 0; c < b->nk; c++) {
        b->k[c].bw = b->mcux * b->k[c].h;
        b->k[c].bh = b->mcuy * b->k[c].v;
        size_t antal = (size_t)b->k[c].bw * b->k[c].bh * 64;
        b->k[c].koef = calloc(antal, sizeof(int16_t));
        if (!b->k[c].koef) dö("slut på minne för koefficienter");
    }
}

/* Avkodar hela scanen till koefficientplan. Returnerar sista entropibyten. */
static size_t avkoda_scan(Bild *b, const uint8_t *d, size_t n) {
    Bitläsare r = { d, b->scan_start, n, 0, 0, 0 };
    int pred[4] = {0, 0, 0, 0};
    long till_omstart = b->dri;
    for (int my = 0; my < b->mcuy; my++)
        for (int mx = 0; mx < b->mcux; mx++) {
            if (b->dri && till_omstart == 0) {
                /* omstartsmarkör: bitbufferten töms och DC-prediktionen nollas */
                r.n = 0;
                while (r.pos + 1 < n && !(d[r.pos] == 0xFF && d[r.pos+1] >= 0xD0 && d[r.pos+1] <= 0xD7)) r.pos++;
                r.pos += 2;
                pred[0] = pred[1] = pred[2] = pred[3] = 0;
                till_omstart = b->dri;
            }
            for (int c = 0; c < b->nk; c++)
                for (int by = 0; by < b->k[c].v; by++)
                    for (int bx = 0; bx < b->k[c].h; bx++) {
                        int brx = mx * b->k[c].h + bx, bry = my * b->k[c].v + by;
                        int16_t *blk = b->k[c].koef + ((size_t)bry * b->k[c].bw + brx) * 64;
                        int t = bl_huff(&r, &b->hdc[b->k[c].td]);
                        if (t < 0) dö("trasig huffmankod (DC)");
                        int diff = t ? förläng(bl_bitar(&r, t), t) : 0;
                        pred[c] += diff;
                        blk[0] = (int16_t)pred[c];
                        for (int k = 1; k < 64; ) {
                            int rs = bl_huff(&r, &b->hac[b->k[c].ta]);
                            if (rs < 0) dö("trasig huffmankod (AC)");
                            int s = rs & 15, löp = rs >> 4;
                            if (s == 0) { if (löp != 15) break; k += 16; continue; }
                            k += löp;
                            if (k > 63) dö("AC-index utanför blocket");
                            blk[SICKSACK[k]] = (int16_t)förläng(bl_bitar(&r, s), s);
                            k++;
                        }
                    }
            if (b->dri) till_omstart--;
        }
    /* Entropidatan slutar där nästa markör börjar. */
    size_t p = r.pos;
    while (p + 1 < n && !(d[p] == 0xFF && d[p + 1] != 0x00 && !(d[p+1] >= 0xD0 && d[p+1] <= 0xD7))) p++;
    return p;
}

/* Kodar koefficienterna tillbaka till Huffman — samma bytes som originalet. */
static Buf koda_huffman(Bild *b) {
    Bitskrivare w = { {NULL, 0, 0}, 0, 0 };
    int pred[4] = {0, 0, 0, 0};
    long till_omstart = b->dri; int nästa_rst = 0;
    for (int my = 0; my < b->mcuy; my++)
        for (int mx = 0; mx < b->mcux; mx++) {
            if (b->dri && till_omstart == 0) {
                bs_fyll(&w);
                buf_byte(&w.b, 0xFF); buf_byte(&w.b, (uint8_t)(0xD0 + (nästa_rst & 7)));
                nästa_rst++;
                pred[0] = pred[1] = pred[2] = pred[3] = 0;
                till_omstart = b->dri;
            }
            for (int c = 0; c < b->nk; c++)
                for (int by = 0; by < b->k[c].v; by++)
                    for (int bx = 0; bx < b->k[c].h; bx++) {
                        int brx = mx * b->k[c].h + bx, bry = my * b->k[c].v + by;
                        int16_t *blk = b->k[c].koef + ((size_t)bry * b->k[c].bw + brx) * 64;
                        Huff *hd = &b->hdc[b->k[c].td], *ha = &b->hac[b->k[c].ta];
                        int diff = blk[0] - pred[c]; pred[c] = blk[0];
                        int t = 0, a = diff < 0 ? -diff : diff;
                        while (a) { t++; a >>= 1; }
                        bs_bitar(&w, hd->kod[t], hd->storlek[t]);
                        if (t) bs_bitar(&w, diff < 0 ? diff - 1 : diff, t);
                        int sista = 0;
                        for (int k = 1; k < 64; k++) if (blk[SICKSACK[k]]) sista = k;
                        int löp = 0;
                        for (int k = 1; k <= sista; k++) {
                            int v = blk[SICKSACK[k]];
                            if (!v) { löp++; continue; }
                            while (löp > 15) { bs_bitar(&w, ha->kod[0xF0], ha->storlek[0xF0]); löp -= 16; }
                            int s = 0, m = v < 0 ? -v : v;
                            while (m) { s++; m >>= 1; }
                            int rs = (löp << 4) | s;
                            bs_bitar(&w, ha->kod[rs], ha->storlek[rs]);
                            bs_bitar(&w, v < 0 ? v - 1 : v, s);
                            löp = 0;
                        }
                        if (sista < 63) bs_bitar(&w, ha->kod[0], ha->storlek[0]);
                    }
            if (b->dri) till_omstart--;
        }
    bs_fyll(&w);
    return w.b;
}

/* ------------------------------------------------------------- modellen - */
/*
 * Kontexterna är valda efter vad som faktiskt förutsäger nästa tal. Tre
 * grannskap används, och de är olika saker:
 *
 *   BLOCKET TILL VÄNSTER OCH OVANFÖR  säger hur rörig den här delen av bilden
 *      är. Antalet nollskilda koefficienter i ett block förutsägs nästan helt
 *      av antalet i de två grannblocken — slät himmel bredvid slät himmel.
 *
 *   SAMMA PLATS I GRANNBLOCKEN        säger hur stor just den här frekvensen
 *      brukar vara häromkring. Ett lodrätt kantmönster finns kvar i blocket
 *      bredvid.
 *
 *   PLATSERNA TILL VÄNSTER OCH OVANFÖR I SAMMA BLOCK säger var i blocket
 *      energin ligger. De är redan kodade när vi kommer fram: sicksackvägen
 *      besöker alltid (r-1,c) och (r,c-1) före (r,c).
 *
 * DC kodas inte som JPEG gör det (differens mot föregående block i ordningen)
 * utan mot medelvärdet av blocket till vänster och ovanför. En bild läses
 * radvis, men den LIKNAR sig själv i två riktningar, och vid radslut är
 * föregående block i ordningen granne med fel kant av bilden.
 *
 * Luma och kroma skiljs åt eftersom de har helt olika statistik. Mantissans
 * nedersta bitar kodas direkt: de är i praktiken slumpmässiga, och en modell
 * på dem kostar mer än den ger.
 */
#ifndef KB
#define KB 16   /* sicksack-hinkar */
#endif
#ifndef NB
#define NB 16                    /* hinkar för "kvarvarande nollskilda" */
#endif
#ifndef MB
#define MB 10   /* hinkar för grannarnas storlek i samma block */
#endif
#ifndef EB
#define EB 3    /* 3 = väg in hur tätt blocket är i storleksmodellen */
#endif
#ifndef GRANNAR
#define GRANNAR 1       /* hur stort grannskap i blocket som vägs in */
#endif
#ifndef CN
#define CN 4                    /* hinkar för grannblocket i rätt riktning */
#endif
#define DB 7                    /* hinkar för DC-lutning */
#define ZB 7                    /* hinkar för grannblockens antal nollskilda */

typedef struct {
    Sann nnz[2][ZB][ZB][64];
    Sann dc_exp[2][DB][16];
    Sann dc_man[2][16][16];
    Sann dc_tkn[2][DB];
    Sann ac_noll[2][KB][NB][MB][CN];
    Sann ac_exp[2][KB][MB][CN][EB][16];
    Sann ac_man[2][16][16];
    Sann ac_tkn[2][KB];
} Modell;

static void modell_init(Modell *m) {
    Sann *p = (Sann *)m;
    for (size_t i = 0; i < sizeof(Modell) / sizeof(Sann); i++) {
#if BLANDA
#if BLANDA == 2
        p[i].p = PINIT; p[i].n = 0;
#else
        p[i].snabb = PINIT; p[i].langsam = PINIT;
#endif
#else
        p[i].p = PINIT;
#endif
    }
}
static int hink_k(int k) {
#if KB >= 16
    static const int t[64] = {0,0,1,2,3,4,5,5,6,6,7,7,8,8,8,8,9,9,9,9,10,10,10,10,
        11,11,11,11,11,11,12,12,12,12,12,12,13,13,13,13,13,13,13,13,14,14,14,14,
        14,14,14,14,15,15,15,15,15,15,15,15,15,15,15,15};
#else
    static const int t[64] = {0,0,1,1,2,2,2,3,3,3,3,4,4,4,4,4,5,5,5,5,5,5,5,5,
        6,6,6,6,6,6,6,6,6,6,6,6,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7};
#endif
    return t[k];
}
static int absi(int v) { return v < 0 ? -v : v; }
static int exponent(int v) { int e = 0, a = absi(v); while (a) { e++; a >>= 1; } return e; }
static int hink_mag(int m) {
#if MB >= 10
    return m == 0 ? 0 : m == 1 ? 1 : m == 2 ? 2 : m == 3 ? 3 : m == 4 ? 4
         : m <= 6 ? 5 : m <= 9 ? 6 : m <= 15 ? 7 : m <= 31 ? 8 : 9;
#else
    return m == 0 ? 0 : m == 1 ? 1 : m == 2 ? 2 : m <= 4 ? 3 : m <= 8 ? 4 : 5;
#endif
}
static int hink_zb(int n) { return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n <= 4 ? 3 : n <= 8 ? 4 : n <= 16 ? 5 : 6; }
static int hink_dc(int g) { return g == 0 ? 0 : g <= 1 ? 1 : g <= 3 ? 2 : g <= 7 ? 3 : g <= 15 ? 4 : g <= 63 ? 5 : 6; }

static int räkna_nnz(const int16_t *blk) {
    int n = 0;
    for (int k = 1; k < 64; k++) if (blk[SICKSACK[k]]) n++;
    return n;
}

/* Mantissan: de översta bitarna under den ledande ettan modelleras, resten
 * skickas rakt igenom. Samma uppdelning i båda riktningarna. */
#ifndef MODELLERADE
#define MODELLERADE 3
#endif

static void koda_mantissa(Kodare *k, Avkodare *a, Sann *p, int koda, int e, int *mag) {
    if (koda) {
        int m = *mag;
        for (int i = e - 2; i >= 0; i--) {
            int bit = (m >> i) & 1;
            if (i >= e - 1 - MODELLERADE) kod_bit(k, &p[i > 15 ? 15 : i], bit);
            else kod_direkt(k, (uint32_t)bit, 1);
        }
    } else {
        int m = 1;
        for (int i = e - 2; i >= 0; i--) {
            int bit = (i >= e - 1 - MODELLERADE) ? av_bit(a, &p[i > 15 ? 15 : i])
                                                 : (int)av_direkt(a, 1);
            m = (m << 1) | bit;
        }
        *mag = m;
    }
}

/* En enda funktion sköter både kodning och avkodning, styrd av `koda`. Det är
 * med flit: modellen KAN inte komma i otakt mellan de två hållen om det bara
 * finns en beskrivning av den. */
static void gå_igenom(Bild *b, Modell *m, Kodare *k, Avkodare *a, int koda) {
    for (int c = 0; c < b->nk; c++) {
        int ck = c == 0 ? 0 : 1;
        Komp *K = &b->k[c];
        for (int by = 0; by < K->bh; by++)
            for (int bx = 0; bx < K->bw; bx++) {
                int16_t *blk = K->koef + ((size_t)by * K->bw + bx) * 64;
                const int16_t *v = bx ? blk - 64 : NULL;                  /* vänster */
                const int16_t *o = by ? blk - (size_t)K->bw * 64 : NULL;  /* ovanför */

                /* --- antal nollskilda --------------------------------- */
                int nv = v ? räkna_nnz(v) : 0, no = o ? räkna_nnz(o) : 0;
                int nnz;
                Sann *pn = m->nnz[ck][hink_zb(nv)][hink_zb(no)];
                if (koda) { nnz = räkna_nnz(blk); kod_träd(k, pn, 6, (uint32_t)nnz); }
                else nnz = (int)av_träd(a, pn, 6);

                /* --- DC mot medelvärdet av de två grannarna ------------ */
                int pred = (v && o) ? (v[0] + o[0]) / 2 : v ? v[0] : o ? o[0] : 0;
                int lutning = (v && o) ? absi(v[0] - o[0]) : 0;
                int cd = hink_dc(lutning);
                if (koda) {
                    int diff = blk[0] - pred, e = exponent(diff);
                    kod_träd(k, m->dc_exp[ck][cd], 4, (uint32_t)e);
                    if (e) {
                        int mag = absi(diff);
                        koda_mantissa(k, NULL, m->dc_man[ck][e > 15 ? 15 : e], 1, e, &mag);
                        kod_bit(k, &m->dc_tkn[ck][cd], diff < 0);
                    }
                } else {
                    int e = (int)av_träd(a, m->dc_exp[ck][cd], 4), diff = 0;
                    if (e) {
                        int mag = 1;
                        koda_mantissa(NULL, a, m->dc_man[ck][e > 15 ? 15 : e], 0, e, &mag);
                        diff = av_bit(a, &m->dc_tkn[ck][cd]) ? -mag : mag;
                    }
                    blk[0] = (int16_t)(pred + diff);
                }

                /* --- AC ------------------------------------------------ */
                int kvar = nnz;
#if EB > 1
                int ce = nnz <= 2 ? 0 : nnz <= 8 ? 1 : EB - 1;
#else
                int ce = 0;
#endif
                for (int kk = 1; kk < 64 && kvar > 0; kk++) {
                    int z = SICKSACK[kk], r = z >> 3, s = z & 7;
                    /* grannarnas storlek: två platser i samma block, plus
                     * samma plats i blocken bredvid och ovanför */
                    int gm = (s ? absi(blk[z - 1]) : 0) + (r ? absi(blk[z - 8]) : 0);
#if GRANNAR >= 2
                    if (r && s) gm += (absi(blk[z - 9]) + 1) >> 1;      /* snett ovan */
#endif
#if GRANNAR >= 3
                    if (s > 1) gm += (absi(blk[z - 2]) + 1) >> 1;
                    if (r > 1) gm += (absi(blk[z - 16]) + 1) >> 1;
#endif
                    /* Grannblocket vägs efter riktning: översta raden i ett
                     * block är vågräta frekvenser och fortsätter nedåt från
                     * blocket OVANFÖR, vänsterkolumnen på samma sätt i sidled. */
                    int gr = (r == 0) ? (o ? absi(o[z]) : 0)
                           : (s == 0) ? (v ? absi(v[z]) : 0)
                           : (((v ? absi(v[z]) : 0) + (o ? absi(o[z]) : 0) + 1) >> 1);
#if CN > 1
                    int cn = gr == 0 ? 0 : gr == 1 ? 1 : gr <= 3 ? 2 : CN - 1;
#else
                    int cn = 0; gm += gr;
#endif
                    int cm = hink_mag(gm), ckk = hink_k(kk);
                    int cnq = kvar > NB - 1 ? NB - 1 : kvar;
                    int nollskild;
                    /* Går det inte fler platser än kvarvarande nollskilda är
                     * svaret givet och ska inte kosta en enda bit. */
                    int tvingad = (64 - kk <= kvar);
                    if (koda) {
                        nollskild = blk[z] != 0;
                        if (!tvingad) kod_bit(k, &m->ac_noll[ck][ckk][cnq][cm][cn], nollskild);
                    } else {
                        nollskild = tvingad ? 1 : av_bit(a, &m->ac_noll[ck][ckk][cnq][cm][cn]);
                    }
                    if (!nollskild) continue;
                    if (koda) {
                        int val = blk[z], e = exponent(val), mag = absi(val);
                        kod_träd(k, m->ac_exp[ck][ckk][cm][cn][ce], 4, (uint32_t)e);
                        koda_mantissa(k, NULL, m->ac_man[ck][e > 15 ? 15 : e], 1, e, &mag);
                        kod_bit(k, &m->ac_tkn[ck][ckk], val < 0);
                    } else {
                        int e = (int)av_träd(a, m->ac_exp[ck][ckk][cm][cn][ce], 4), mag = 1;
                        koda_mantissa(NULL, a, m->ac_man[ck][e > 15 ? 15 : e], 0, e, &mag);
                        blk[z] = (int16_t)(av_bit(a, &m->ac_tkn[ck][ckk]) ? -mag : mag);
                    }
                    kvar--;
                }
            }
    }
}

/* --------------------------------------------------------------- main --- */

static void skriv_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static uint32_t läs_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int packa(void) {
    size_t n; uint8_t *d = läs_allt(stdin, &n);
    Bild b; memset(&b, 0, sizeof b);
    parsa(&b, d, n);
    avsätt(&b);
    size_t slut = avkoda_scan(&b, d, n);

    /* Beviset: koda tillbaka till Huffman och jämför. */
    Buf åter = koda_huffman(&b);
    size_t orig_len = slut - b.scan_start;
    int sista_literal = 0;
    if (åter.n != orig_len) return 2;
    if (memcmp(åter.d, d + b.scan_start, orig_len ? orig_len - 1 : 0) != 0) return 2;
    if (orig_len && åter.d[orig_len - 1] != d[b.scan_start + orig_len - 1]) sista_literal = 1;

    Modell m; modell_init(&m);
    Kodare k; kod_init(&k);
    gå_igenom(&b, &m, &k, NULL, 1);
    kod_slut(&k);

    fwrite("NVJ1", 1, 4, stdout);
    skriv_u32(stdout, (uint32_t)b.scan_start);
    skriv_u32(stdout, (uint32_t)(n - slut));
    skriv_u32(stdout, (uint32_t)orig_len);
    fputc(sista_literal, stdout);
    fputc(orig_len ? d[b.scan_start + orig_len - 1] : 0, stdout);
    fwrite(d, 1, b.scan_start, stdout);          /* huvudet ordagrant */
    fwrite(d + slut, 1, n - slut, stdout);       /* svansen ordagrant */
    fwrite(k.ut.d, 1, k.ut.n, stdout);
    return 0;
}

static int packa_upp(void) {
    size_t n; uint8_t *d = läs_allt(stdin, &n);
    if (n < 18 || memcmp(d, "NVJ1", 4)) dö("inte en nvj-ström");
    uint32_t huvudlen = läs_u32(d + 4), svanslen = läs_u32(d + 8), scanlen = läs_u32(d + 12);
    int sista_literal = d[16]; uint8_t sista = d[17];
    const uint8_t *huvud = d + 18, *svans = huvud + huvudlen, *ström = svans + svanslen;
    size_t strömlen = n - 18 - huvudlen - svanslen;

    Bild b; memset(&b, 0, sizeof b);
    parsa(&b, huvud, huvudlen);
    avsätt(&b);
    Modell m; modell_init(&m);
    Avkodare a; av_init(&a, ström, strömlen);
    gå_igenom(&b, &m, NULL, &a, 0);

    Buf åter = koda_huffman(&b);
    if (åter.n != scanlen) dö("scanlängd stämmer inte");
    if (sista_literal && åter.n) åter.d[åter.n - 1] = sista;
    fwrite(huvud, 1, huvudlen, stdout);
    fwrite(åter.d, 1, åter.n, stdout);
    fwrite(svans, 1, svanslen, stdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "bruk: nvjpg packa|packaupp < in > ut\n"); return 1; }
    if (!strcmp(argv[1], "packa")) return packa();
    if (!strcmp(argv[1], "packaupp")) return packa_upp();
    fprintf(stderr, "okänt kommando\n");
    return 1;
}
