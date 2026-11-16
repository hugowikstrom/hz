/* nvcm — kontextblandande kompressor för text och källkod.
 *
 *     nvcm [flaggor] [fil ...]        (gzip-syntax, se man nvcm)
 *     nvcm packa|packaupp < in > ut  (gamla vägen, som nvpack anropar)
 *
 * VARFÖR. xz och zstd är ORDBOKSPACKARE: de letar upp en tidigare förekomst
 * av samma byte-följd och skriver "samma som 4 kB bakåt, 19 byte". Det är en
 * utmärkt idé för upprepningar, men den har ett golv. Varje hänvisning kostar
 * ett antal bitar även när nästa tecken var fullständigt förutsägbart, och
 * bokstaven efter "func" i en js-fil, eller mellanslagen efter ett radbrott i
 * indenterad kod, ÄR fullständigt förutsägbara.
 *
 * nvcm gissar i stället nästa BIT, och gissar med flera modeller samtidigt:
 *
 *   ORDNING 0–8   vad som brukar komma efter de senaste 0, 1, 2, 3, 4, 6 och
 *                 8 bytena. Korta ordningar lär sig snabbt, långa blir
 *                 skarpa; blandaren avgör vem man ska tro på.
 *   ORDBOK        vad som brukar komma efter det påbörjade ORDET, oavsett vad
 *                 som stod före. Fångar att "funct" blir "function" var det
 *                 än står.
 *   MATCHNING     om samma 6 byte setts förut: fortsätt som det gjorde då.
 *                 Det är ordbokspackarens trick, inlagt som en röst bland
 *                 andra — så att långa upprepningar fortfarande går snabbt
 *                 men inte tvingar fram en dyr hänvisning för varje träff.
 *
 * Rösterna vägs ihop av en liten blandare som lär sig under tiden vilka
 * modeller som brukar ha rätt i just den här sortens data, och resultatet
 * putsas av två efterjusterare innan det går till den aritmetiska kodaren.
 * Ingen modell behöver stämma: det räcker att blandaren lär sig vilken som
 * gör det.
 *
 * Priset är hastighet. nvcm packar i ett par MB/s medan zstd gör hundratals.
 * Därför väljer nvpack den bara för block där den faktiskt vinner, och bara
 * när arkivet ska vara litet snarare än snabbt.
 *
 * Avkodningen är spegelvänd kodning: samma modeller, samma uppdateringar, i
 * samma ordning. Därför kan koden för båda hållen vara samma funktion.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>

typedef uint8_t  U8;
typedef uint16_t U16;
typedef uint32_t U32;
typedef uint64_t U64;

static void dö(const char *m) { fprintf(stderr, "nvcm: %s\n", m); exit(2); }

/* ------------------------------------------------- logistisk omvandling -- */
/* Sannolikheter blandas inte linjärt utan i logistisk skala: där motsvarar
 * addition att väga ihop bevis, och en modell som är helt säker kan dra. */

static int squash(int d) {
    static const int t[33] = {1,2,3,6,10,16,27,45,73,120,194,310,488,747,1101,
        1546,2047,2549,2994,3348,3607,3785,3901,3975,4024,4050,4068,4079,4085,
        4089,4092,4093,4094};
    if (d > 2047) return 4095;
    if (d < -2047) return 0;
    int w = d & 127;
    d = (d >> 7) + 16;
    return (t[d] * (128 - w) + t[d + 1] * w + 64) >> 7;
}
static short STRETCH[4096];
static void bygg_stretch(void) {
    int pi = 0;
    for (int x = -2047; x <= 2047; x++) {
        int v = squash(x);
        for (int p = pi; p <= v; p++) STRETCH[p] = (short)x;
        pi = v + 1;
    }
    for (int p = pi; p < 4096; p++) STRETCH[p] = 2047;
}

/* ------------------------------------------------------ aritmetisk kod -- */
/* Carryless binär kodare: intervallet [x1,x2] delas i förhållande till
 * sannolikheten, och de översta byten skrivs ut så fort de är bestämda. */

typedef struct { U32 x1, x2, x; FILE *f; } Kod;

static void kod_init_ut(Kod *k, FILE *f) { k->x1 = 0; k->x2 = 0xffffffff; k->x = 0; k->f = f; }
static void kod_init_in(Kod *k, FILE *f) {
    k->x1 = 0; k->x2 = 0xffffffff; k->x = 0; k->f = f;
    for (int i = 0; i < 4; i++) { int c = getc(f); k->x = (k->x << 8) | (U32)(c < 0 ? 0 : c); }
}
static void kod_ut(Kod *k, int p, int bit) {          /* p = P(bit=1), 12 bitar */
    U32 mitt = k->x1 + (U32)(((U64)(k->x2 - k->x1) * (U32)p) >> 12);
    if (bit) k->x2 = mitt; else k->x1 = mitt + 1;
    while (((k->x1 ^ k->x2) & 0xff000000u) == 0) {
        putc((int)(k->x2 >> 24), k->f);
        k->x1 <<= 8; k->x2 = (k->x2 << 8) | 255;
    }
}
static int kod_in(Kod *k, int p) {
    U32 mitt = k->x1 + (U32)(((U64)(k->x2 - k->x1) * (U32)p) >> 12);
    int bit = k->x <= mitt;
    if (bit) k->x2 = mitt; else k->x1 = mitt + 1;
    while (((k->x1 ^ k->x2) & 0xff000000u) == 0) {
        k->x1 <<= 8; k->x2 = (k->x2 << 8) | 255;
        int c = getc(k->f);
        k->x = (k->x << 8) | (U32)(c < 0 ? 0 : c);
    }
    return bit;
}
static void kod_töm(Kod *k) { for (int i = 0; i < 4; i++) { putc((int)(k->x1 >> 24), k->f); k->x1 <<= 8; } }

/* ------------------------------------------------------------- räknare -- */
/* En sannolikhet med egen takt: de första observationerna i en kontext
 * flyttar skattningen långt, senare bara putsar. Samma idé som i nvjpg. */

typedef struct { U16 p; U16 n; } Räknare;
#define RMAX 60
/* Takten slås upp i stället för att divideras fram. En division per modell
 * och bit är sju divisioner per bit, och det syns tydligt i klockan. */
static int RTAKT[RMAX + 1];
static void bygg_takt(void) { for (int i = 0; i <= RMAX; i++) RTAKT[i] = 65536 * 2 / (i + 3); }
static inline int r_p(const Räknare *r) { return r->p >> 4; }        /* 16 -> 12 bitar */
static inline void r_upp(Räknare *r, int bit) {
    int mål = bit ? 65535 : 0;
    r->p = (U16)((int)r->p + (((mål - (int)r->p) * RTAKT[r->n]) >> 16));
    if (r->n < RMAX) r->n++;
}

/* ------------------------------------------------------------ modeller -- */

#ifndef NORD
#define NORD   8                 /* antal kontextmodeller (ordningar) */
#endif
#ifndef VERIFIERA
#define VERIFIERA 1              /* jämför matchningens bytes, lita inte på hashen */
#endif
#ifndef MIXCTX
#define MIXCTX 4                 /* 2 = låt matchningens längd välja viktuppsättning */
#endif
#define NMOD   (NORD + 2)        /* antal röster in i blandaren */
#ifndef TBITAR
#define TBITAR 24                /* tak: 16 M platser per kontextmodell */
#endif
/* Tabellerna skalas efter indatans storlek. Ett block på en megabyte fyller
 * aldrig 16 M platser, och att nollställa en halv gigabyte räknare för det
 * tar längre tid än hela packningen. Formeln beror BARA på längden, som
 * står i huvudet — så avkodaren räknar fram samma storlek utan att fråga. */
static int tbitar_för(U64 n) {
    int b = 16;
    while (b < TBITAR && ((U64)1 << (b - 2)) < n) b++;
    return b;
}
#ifndef HBITAR
#define HBITAR 22                /* matchmodellens hashtabell */
#endif
#ifndef MORD
#define MORD 6                   /* hur många byte matchmodellen nycklar på */
#endif

typedef struct {
    Räknare *t[NORD];            /* ordning 1,2,3,4,6(,8) + ordmodell */
    U32 tmask;                   /* platser per tabell minus ett */
    U32 hash[NORD];              /* kontexthash vid bytegräns */
    U32 *mhash;                  /* matchmodell: hash -> position */
    U8  *buf; size_t bufn, bufcap;
    size_t mpek; int mlen;       /* pågående matchning */
    Räknare mp[64][2];           /* matchmodellens egen skattning */
    short vikt[256 * MIXCTX][NMOD];  /* blandarens vikter, per kontext */
    Räknare apm1[1024 * 33], apm2[4096 * 33];
    U32 bas[NORD];               /* radbörjan i varje tabell för pågående nibble */
    U32 c0;                      /* påbörjad byte, med ledande etta */
    U32 c0n;                     /* påbörjad NIBBLE, med ledande etta (1..15) */
    U32 c4;                      /* de fyra senaste byten */
    U32 ord;                     /* hash av påbörjat ord */
    int bitpos;
} Modell;

/* 0 = ordmodellen, -1 = ordning noll (ingen historik alls) */
#if NORD == 8
static const int ORDNING[8] = {-1, 1, 2, 3, 4, 6, 8, 0};
#elif NORD == 7
static const int ORDNING[7] = {1, 2, 3, 4, 6, 8, 0};
#else
static const int ORDNING[6] = {1, 2, 3, 4, 6, 0};
#endif

static Modell *modell_ny(size_t ingång) {
    Modell *m = calloc(1, sizeof *m);
    if (!m) dö("slut på minne");
    U32 tstor = (U32)1 << tbitar_för(ingång);
    m->tmask = tstor - 1;
    for (int i = 0; i < NORD; i++) {
        m->t[i] = malloc((size_t)tstor * sizeof(Räknare));
        if (!m->t[i]) dö("slut på minne");
        for (U32 j = 0; j < tstor; j++) { m->t[i][j].p = 32768; m->t[i][j].n = 0; }
    }
    m->mhash = calloc((size_t)1 << HBITAR, sizeof(U32));
    m->bufcap = ingång + 64;
    m->buf = malloc(m->bufcap);
    if (!m->mhash || !m->buf) dö("slut på minne");
    for (int i = 0; i < 64; i++) for (int j = 0; j < 2; j++) { m->mp[i][j].p = 32768; m->mp[i][j].n = 0; }
    for (int i = 0; i < 256 * MIXCTX; i++) for (int j = 0; j < NMOD; j++) m->vikt[i][j] = 0;
    for (size_t i = 0; i < sizeof m->apm1 / sizeof m->apm1[0]; i++) {
        m->apm1[i].p = (U16)(squash(((int)(i % 33) - 16) * 128) * 16);
        m->apm1[i].n = 0;
    }
    for (size_t i = 0; i < sizeof m->apm2 / sizeof m->apm2[0]; i++) {
        m->apm2[i].p = (U16)(squash(((int)(i % 33) - 16) * 128) * 16);
        m->apm2[i].n = 0;
    }
    m->c0 = 1;
    m->c0n = 1;
    return m;
}
static void modell_fri(Modell *m) {
    for (int i = 0; i < NORD; i++) free(m->t[i]);
    free(m->mhash); free(m->buf); free(m);
}

/* Efterjusterare: tar en färdig sannolikhet och en liten kontext och lär sig
 * hur skattningen brukar vara skev just där. Billig, och tar ofta några
 * procent — särskilt i början av en fil innan modellerna hunnit lära sig. */
static int apm(Räknare *t, int pr, int cx, int bit, int uppdatera) {
    int s = STRETCH[pr] + 2048;             /* 0..4095 */
    int w = s & 127;
    int i = cx * 33 + (s >> 7);
    int ut = (r_p(&t[i]) * (128 - w) + r_p(&t[i + 1]) * w) >> 7;
    if (uppdatera) { r_upp(&t[i], bit); r_upp(&t[i + 1], bit); }
    return ut;
}

/* En bit: ta fram alla röster, blanda, koda eller avkoda, och lär upp allt.
 * Kodning och avkodning är samma funktion med samma ordning på uppdateringar
 * — annars kan de inte följas åt. */
static int bit_steg(Modell *m, Kod *k, int koda, int bit_in) {
    Räknare *slot[NMOD];
    short st[NMOD];
    int n = 0;

    /* Alla femton bitvägar genom en nibble ligger på samma cache-rad. Det är
     * skillnaden mellan åtta cachemissar per byte och modell, och två. */
    for (int i = 0; i < NORD; i++) {
        U32 idx = m->bas[i] + (m->c0n & 15);
        slot[n] = &m->t[i][idx];
        st[n] = STRETCH[r_p(slot[n])];
        n++;
    }

    /* Matchmodellen: om vi följer en tidigare förekomst, vad sa den? */
    int mbit = -1;
    if (m->mlen > 0 && m->mpek < m->bufn) {
        U32 förutsagd = m->buf[m->mpek];
        U32 höga = m->c0;                      /* ledande etta + hittills kodade bitar */
        int bitar = m->bitpos;
        if ((U32)((förutsagd | 256) >> (8 - bitar)) == höga) {
            mbit = (int)(förutsagd >> (7 - bitar)) & 1;
        } else {
            m->mlen = 0;                       /* matchningen bröts mitt i byten */
        }
    }
    int mhink = m->mlen > 31 ? 31 : m->mlen;
    if (mbit >= 0) {
        slot[n] = &m->mp[mhink * 2 + mbit][0];
        st[n] = (short)(mbit ? STRETCH[r_p(slot[n])] : -STRETCH[r_p(slot[n])]);
    } else {
        slot[n] = &m->mp[0][1];
        st[n] = 0;
    }
    int matchplats = n;
    n++;

    /* Blandaren. Viktuppsättningen väljs av den påbörjade byten, så att
     * modellen får olika avvägning i början och slutet av ett tecken. */
#if MIXCTX > 1
    /* En lång pågående matchning gör rösterna helt annorlunda värda än när
     * modellen famlar. Då ska de inte dela viktuppsättning heller. */
    int vkt = (int)(m->c0 & 255) + 256 * (m->mlen == 0 ? 0 : m->mlen < 8 ? 1 : m->mlen < 24 ? 2 : 3);
#else
    int vkt = (int)(m->c0 & 255);
#endif
    short *w = m->vikt[vkt];
    int summa = 0;
    for (int i = 0; i < n; i++) summa += st[i] * w[i];
    summa >>= 16;
    int pr = squash(summa);

    /* Två efterjusterare, vägda mot den obearbetade skattningen. */
    int pr1 = apm(m->apm1, pr, (int)(m->c0 & 1023), 0, 0);
    int pr2 = apm(m->apm2, pr, (int)((m->c4 & 0xff) << 4 | (m->c0 & 15)), 0, 0);
    int slutlig = (pr + pr1 + 2 * pr2 + 2) >> 2;
    if (slutlig < 1) slutlig = 1;
    if (slutlig > 4094) slutlig = 4094;

    int bit;
    if (koda) { bit = bit_in; kod_ut(k, slutlig, bit); }
    else bit = kod_in(k, slutlig);

    /* Lär upp: blandarvikter mot felet, sedan varje röst mot utfallet. */
    int fel = ((bit << 12) - pr) * 6;
    for (int i = 0; i < n; i++) {
        int d = (st[i] * fel * 2 >> 16);
        int ny = w[i] + ((d + 1) >> 1);
        w[i] = (short)(ny > 32767 ? 32767 : ny < -32768 ? -32768 : ny);
    }
    for (int i = 0; i < n; i++) {
        if (i == matchplats && mbit < 0) continue;
        if (i == matchplats) r_upp(slot[i], bit == mbit);
        else r_upp(slot[i], bit);
    }
    apm(m->apm1, pr, (int)(m->c0 & 1023), bit, 1);
    apm(m->apm2, pr, (int)((m->c4 & 0xff) << 4 | (m->c0 & 15)), bit, 1);

    if (mbit >= 0 && bit != mbit) m->mlen = 0;
    return bit;
}

/* Vid varje nibblegräns flyttas raden: andra halvan av byten får en egen rad
 * som beror på vad första halvan blev. */
static void sätt_rader(Modell *m, U32 nibbel) {
    for (int i = 0; i < NORD; i++) {
        m->bas[i] = (m->hash[i] ^ (nibbel * 0x85EBCA6Bu)) & (m->tmask & ~15u);
        __builtin_prefetch(&m->t[i][m->bas[i]], 1, 3);
    }
}

/* Efter varje hel byte: flytta fram kontexterna och matchmodellen. */
static void byte_klar(Modell *m, U8 c) {
    if (m->bufn + 1 >= m->bufcap) {
        m->bufcap = m->bufcap * 2 + 1024;
        m->buf = realloc(m->buf, m->bufcap);
        if (!m->buf) dö("slut på minne");
    }
    m->buf[m->bufn++] = c;
    m->c4 = (m->c4 << 8) | c;

    /* Ordmodellen: bokstäver och siffror bygger ett ord, allt annat bryter. */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
        m->ord = m->ord * 0x6F4F2F1Du + c + 1;
    else
        m->ord = 0;

    U64 h = 0;
    for (int i = 0; i < NORD; i++) {
        int ordning = ORDNING[i];
        if (ordning == 0) { m->hash[i] = m->ord * 0x2545F491u + 0x9E3779B1u; continue; }
        if (ordning < 0) { m->hash[i] = 0; continue; }
        h = 0;
        for (int j = 0; j < ordning && j < (int)m->bufn; j++)
            h = h * 0x100000001B3ull + m->buf[m->bufn - 1 - j] + 1;
        m->hash[i] = (U32)((h ^ (h >> 29)) * (U32)(ordning * 0x9E3779B1u + 1));
    }

    /* Matchmodellen: nyckeln är de sex senaste byten. Hittas de igen pekas
     * mpek på byten EFTER den tidigare förekomsten. */
    if (m->bufn >= MORD) {
        U64 mh = 0;
        for (int j = 0; j < MORD; j++) mh = mh * 0x100000001B3ull + m->buf[m->bufn - 1 - j] + 1;
        U32 idx = (U32)((mh ^ (mh >> 31)) & (((U32)1 << HBITAR) - 1));
        if (m->mlen > 0 && m->mpek < m->bufn && m->buf[m->mpek] == c) {
            m->mpek++;
            if (m->mlen < 63) m->mlen++;
        } else {
            U32 kandidat = m->mhash[idx];
            /* Hashen kan ljuga. En falsk matchning är värre än ingen alls:
             * den ger blandaren en självsäker röst som har fel. */
            if (kandidat >= MORD && kandidat < m->bufn
#if VERIFIERA
                && memcmp(m->buf + kandidat - MORD, m->buf + m->bufn - MORD, MORD) == 0
#endif
               ) { m->mpek = kandidat; m->mlen = 1; }
            else m->mlen = 0;
        }
        m->mhash[idx] = (U32)m->bufn;
    }
    m->c0 = 1;
    m->c0n = 1;
    m->bitpos = 0;
    sätt_rader(m, 0);
}

/* ---------------------------------------------------------------- main -- */

/* Magin är NVCM1. Under några timmar hette programmet nvcm och skrev NVZ01;
 * den magin läses fortfarande, så att inget som redan packats blir
 * oläsbart av att ett namn vände tillbaka. */
static const char *MAGI = "NVCM1";
static const char *MAGI_GAMMAL = "NVZ01";


static U8 *läs_allt(FILE *f, size_t *n) {
    size_t cap = 1 << 22, len = 0;
    U8 *d = malloc(cap);
    if (!d) dö("slut på minne");
    for (;;) {
        if (len == cap) { cap *= 2; d = realloc(d, cap); if (!d) dö("slut på minne"); }
        size_t r = fread(d + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    *n = len;
    return d;
}

static int packa_strom(FILE *in, FILE *ut) {
    size_t n;
    U8 *d = läs_allt(in, &n);
    fwrite(MAGI, 1, 5, ut);
    U64 len = n;
    fwrite(&len, 8, 1, ut);
    Modell *m = modell_ny(n);
    Kod k; kod_init_ut(&k, ut);
    for (size_t i = 0; i < n; i++) {
        U8 c = d[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (c >> b) & 1;
            bit_steg(m, &k, 1, bit);
            m->c0 = (m->c0 << 1) | (U32)bit;
            m->c0n = (m->c0n << 1) | (U32)bit;
            if (++m->bitpos == 4) { sätt_rader(m, m->c0n); m->c0n = 1; }
        }
        byte_klar(m, c);
    }
    kod_töm(&k);
    modell_fri(m); free(d);
    return 0;
}

static int packa_upp_strom(FILE *in, FILE *ut) {
    char magi[5];
    if (fread(magi, 1, 5, in) != 5 || (memcmp(magi, MAGI, 5) && memcmp(magi, MAGI_GAMMAL, 5)))
        dö("inte en nvcm-ström");
    U64 len;
    if (fread(&len, 8, 1, in) != 1) dö("trasigt huvud");
    Modell *m = modell_ny((size_t)len);
    Kod k; kod_init_in(&k, in);
    U8 *utd = malloc((size_t)len + 1);
    if (!utd) dö("slut på minne");
    for (U64 i = 0; i < len; i++) {
        U32 c = 0;
        for (int b = 7; b >= 0; b--) {
            int bit = bit_steg(m, &k, 0, 0);
            m->c0 = (m->c0 << 1) | (U32)bit;
            m->c0n = (m->c0n << 1) | (U32)bit;
            if (++m->bitpos == 4) { sätt_rader(m, m->c0n); m->c0n = 1; }
            c = (c << 1) | (U32)bit;
        }
        byte_klar(m, (U8)c);
        utd[i] = (U8)c;
    }
    fwrite(utd, 1, (size_t)len, ut);
    free(utd); modell_fri(m);
    return 0;
}


/* ------------------------------------------------------- gzip-syntaxen -- */
/* nvcm anropas som gzip:
 *
 *     nvcm fil            -> fil.nvcm, originalet tas bort
 *     nvcm -d fil.nvcm    -> fil, det packade tas bort
 *     nvcm -c fil > ut    -> till stdout, filen rörs inte
 *     nvcm -t fil.nvcm    -> provar att den går att packa upp
 *     nvcm -l fil.nvcm    -> storlekarna utan att packa upp
 *     ... | nvcm | ...    -> utan filargument: stdin till stdout
 *
 * Varför gzips syntax och inte en egen? För att varje skript, Makefile och
 * benchmark som redan finns talar den. En kodek som kräver
 * `nvcm packa < in > ut` går inte att sätta in i ett befintligt anrop utan
 * att skriva om anropet, och då mäts den inte.
 *
 * -1..-9, --fast och --best tas emot och ignoreras: nvcm har inga nivåer,
 * men skript skickar dem, och att vägra starta för en meningslös flagga
 * vore att bråka om ingenting.
 *
 * De gamla underkommandona `packa` och `packaupp` finns kvar oförändrade —
 * nvpack.py anropar dem — men de står inte i hjälptexten.
 */

static const char *SUF = ".nvcm";

typedef struct { int d, c, k, f, t, l, v; } Flaggor;

static int gnäll(const char *namn, const char *vad) {
    fprintf(stderr, "nvcm: %s: %s\n", namn, vad);
    return 1;
}

static void behåll_metadata(const char *mal, const struct stat *st) {
    struct timespec tider[2] = { st->st_atim, st->st_mtim };
    if (chmod(mal, st->st_mode & 07777) != 0) { /* rättigheter är en önskan */ }
    if (utimensat(AT_FDCWD, mal, tider, 0) != 0) { /* liksom tiderna */ }
}

/* Huvudet bär den opackade längden, så -l behöver inte packa upp något. */
static int lista_fil(const char *namn) {
    FILE *f = fopen(namn, "rb");
    if (!f) return gnäll(namn, strerror(errno));
    char magi[5]; U64 len = 0;
    int ok = fread(magi, 1, 5, f) == 5 && !memcmp(magi, "NVCM1", 5)
             && fread(&len, 8, 1, f) == 1;
    struct stat st;
    int har_st = stat(namn, &st) == 0;
    fclose(f);
    if (!ok) return gnäll(namn, "inte en nvcm-ström");
    double packad = har_st ? (double)st.st_size : 0;
    double kvot = len ? 100.0 * (1.0 - packad / (double)len) : 0;
    printf("%12.0f %12llu %6.1f%% %s\n", packad, (unsigned long long)len, kvot, namn);
    return 0;
}

static int gör_fil(const char *namn, Flaggor fl) {
    if (fl.l) return lista_fil(namn);

    size_t nlen = strlen(namn), slen = strlen(SUF);
    int har_suffix = nlen > slen && !strcmp(namn + nlen - slen, SUF);
    if (!fl.d && har_suffix && !fl.c && !fl.t)
        return gnäll(namn, "har redan ändelsen .nvcm -- lämnas i fred");
    if ((fl.d || fl.t) && !har_suffix && !fl.c)
        return gnäll(namn, "okänd ändelse -- lämnas i fred");

    struct stat st;
    if (stat(namn, &st) != 0) return gnäll(namn, strerror(errno));
    if (!S_ISREG(st.st_mode)) return gnäll(namn, "inte en vanlig fil");

    char mal[4096];
    int till_stdout = fl.c || fl.t;
    if (fl.t) {
        snprintf(mal, sizeof mal, "/dev/null");
    } else if (fl.d) {
        if (nlen - slen >= sizeof mal) return gnäll(namn, "för långt filnamn");
        snprintf(mal, sizeof mal, "%.*s", (int)(nlen - slen), namn);
    } else {
        if (snprintf(mal, sizeof mal, "%s%s", namn, SUF) >= (int)sizeof mal)
            return gnäll(namn, "för långt filnamn");
    }
    if (!till_stdout && !fl.f && access(mal, F_OK) == 0)
        return gnäll(mal, "finns redan -- skrivs inte över (använd -f)");

    FILE *in = fopen(namn, "rb");
    if (!in) return gnäll(namn, strerror(errno));
    FILE *ut = fl.c ? stdout : fopen(fl.t ? "/dev/null" : mal, "wb");
    if (!ut) { fclose(in); return gnäll(mal, strerror(errno)); }

    (fl.d || fl.t) ? packa_upp_strom(in, ut) : packa_strom(in, ut);
    fflush(ut);
    long packad = fl.c ? -1 : ftell(ut);
    fclose(in);
    if (!fl.c) fclose(ut);

    if (!till_stdout) {
        behåll_metadata(mal, &st);
        if (!fl.k && unlink(namn) != 0) gnäll(namn, strerror(errno));
    }
    if (fl.t) { printf("%s: OK\n", namn); return 0; }
    if (fl.v && packad > 0) {
        double före = fl.d ? (double)packad : (double)st.st_size;
        double efter = fl.d ? (double)st.st_size : (double)packad;
        fprintf(stderr, "%s:\t%5.1f%% -- %s %s\n", namn,
                före ? 100.0 * (1.0 - efter / före) : 0.0,
                fl.k ? "skrev" : "ersatt med", mal);
    }
    return 0;
}

static void hjälp(void) {
    printf(
"bruk: nvcm [flaggor] [fil ...]\n"
"\n"
"  -d, --decompress   packa upp\n"
"  -c, --stdout       skriv till stdout, lämna filen i fred\n"
"  -k, --keep         behåll filen i stället för att ersätta den\n"
"  -f, --force        skriv över en fil som redan finns\n"
"  -t, --test         prova att det packade går att packa upp\n"
"  -l, --list         visa packad och opackad storlek\n"
"  -v, --verbose      berätta vad som hände\n"
"  -1 .. -9           tas emot och ignoreras (nvcm har inga nivåer)\n"
"  -h, --help         den här texten\n"
"  -V, --version      versionen\n"
"\n"
"Utan filargument går strömmen stdin -> stdout. Ändelsen är .nvcm.\n");
}

int main(int argc, char **argv) {
    bygg_stretch();
    bygg_takt();

    /* Gamla syntaxen först: nvpack.py anropar `nvcm packa|packaupp`. */
    if (argc >= 2 && !strcmp(argv[1], "packa"))    return packa_strom(stdin, stdout);
    if (argc >= 2 && !strcmp(argv[1], "packaupp")) return packa_upp_strom(stdin, stdout);

    Flaggor fl = {0};
    int i = 1, slut_på_flaggor = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (slut_på_flaggor || a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { slut_på_flaggor = 1; i++; break; }
        if (a[1] == '-') {
            if (!strcmp(a, "--decompress") || !strcmp(a, "--uncompress")) fl.d = 1;
            else if (!strcmp(a, "--stdout") || !strcmp(a, "--to-stdout")) fl.c = 1;
            else if (!strcmp(a, "--keep")) fl.k = 1;
            else if (!strcmp(a, "--force")) fl.f = 1;
            else if (!strcmp(a, "--test")) fl.t = 1;
            else if (!strcmp(a, "--list")) fl.l = 1;
            else if (!strcmp(a, "--verbose")) fl.v = 1;
            else if (!strcmp(a, "--fast") || !strcmp(a, "--best")) { /* inga nivåer */ }
            else if (!strcmp(a, "--help")) { hjälp(); return 0; }
            else if (!strcmp(a, "--version")) { printf("nvcm 1.0\n"); return 0; }
            else { fprintf(stderr, "nvcm: okänd flagga %s\n", a); return 1; }
            continue;
        }
        for (const char *c = a + 1; *c; c++) {
            switch (*c) {
            case 'd': fl.d = 1; break;
            case 'c': fl.c = 1; break;
            case 'k': fl.k = 1; break;
            case 'f': fl.f = 1; break;
            case 't': fl.t = 1; break;
            case 'l': fl.l = 1; break;
            case 'v': fl.v = 1; break;
            case 'q': break;
            case 'n': case 'N': break;
            case '1': case '2': case '3': case '4': case '5':
            case '6': case '7': case '8': case '9': break;
            case 'h': hjälp(); return 0;
            case 'V': printf("nvcm 1.0\n"); return 0;
            default:
                fprintf(stderr, "nvcm: okänd flagga -%c\n", *c);
                return 1;
            }
        }
    }

    if (i >= argc) {
        /* Ingen fil: stdin -> stdout, som gzip i ett rör. */
        if (fl.l) { fprintf(stderr, "nvcm: -l behöver en fil\n"); return 1; }
        if (!fl.d && !fl.t && isatty(1) && !fl.f) {
            fprintf(stderr, "nvcm: packad data skrivs inte till en terminal. "
                            "Använd -f om du menar allvar.\n");
            return 1;
        }
        if (fl.t) { FILE *n = fopen("/dev/null", "wb");
                    int r = packa_upp_strom(stdin, n ? n : stdout);
                    if (n) fclose(n);
                    if (!r) printf("stdin: OK\n");
                    return r; }
        return (fl.d ? packa_upp_strom : packa_strom)(stdin, stdout);
    }

    if (fl.l) printf("%12s %12s %6s %s\n", "packad", "opackad", "kvot", "namn");
    int fel = 0;
    for (; i < argc; i++) fel |= gör_fil(argv[i], fl);
    return fel;
}
