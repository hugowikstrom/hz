// SPDX-License-Identifier: GPL-2.0-only
/*
 * hzcm - context-mixing compressor for text and source code.
 *
 *     hzcm [flags] [file ...]        gzip-style CLI (see man hzcm)
 *     hzcm pack|unpack < in > out    worker path; hz runs hzcm as a worker
 *                                    process (the old names packa|packaupp
 *                                    remain as aliases)
 *
 * Dictionary compressors (xz, zstd) pay bits for every back-reference even
 * when the next byte is fully determined by context (the letter after
 * "func" in a js file, the indent after a newline). hzcm instead predicts
 * the next bit with several models voting at once:
 *
 *   orders 0-8   what follows the last 0, 1, 2, 3, 4, 6 and 8 bytes; short
 *                orders learn fast, long ones become sharp
 *   word         what follows the word begun so far, regardless of what
 *                preceded it ("funct" -> "function" wherever it appears)
 *   match        continue the way an earlier occurrence of the same
 *                6 bytes did; the dictionary trick cast as one vote among
 *                others, with no forced per-hit reference cost
 *
 * A mixer learns per context which models to believe, and two adaptive
 * probability maps refine the mix before the arithmetic coder. No single
 * model needs to be right.
 *
 * The price is speed: hzcm packs at a few MB/s where zstd does hundreds.
 * hz picks it only for blocks where it actually wins, and only when the
 * archive should be small rather than fast.
 *
 * Decoding is mirrored encoding: the same models, the same updates, in the
 * same order, which is why one function serves both directions.
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

static void die(const char *m) { fprintf(stderr, "hzcm: %s\n", m); exit(2); }

/* -------------------------------------------------- logistic transform -- */
/*
 * Probabilities are mixed on the logistic scale, not linearly: addition
 * there weighs evidence together, and a confident model pulls hard.
 */

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
/*
 * STRETCH inverts squash(). It is precomputed once at startup: the inverse
 * would otherwise need a search per lookup, and stretch runs for every
 * model on every bit.
 */
static short STRETCH[4096];
static void build_stretch(void) {
    int pi = 0;
    for (int x = -2047; x <= 2047; x++) {
        int v = squash(x);
        for (int p = pi; p <= v; p++) STRETCH[p] = (short)x;
        pi = v + 1;
    }
    for (int p = pi; p < 4096; p++) STRETCH[p] = 2047;
}

/* ------------------------------------------------------ arithmetic coder -- */
/*
 * Carryless binary coder: the interval [x1,x2] is split in proportion to
 * the probability, and the top bytes are emitted as soon as they settle.
 */

typedef struct { U32 x1, x2, x; FILE *f; } Coder;

static void coder_init_out(Coder *k, FILE *f) { k->x1 = 0; k->x2 = 0xffffffff; k->x = 0; k->f = f; }
static void coder_init_in(Coder *k, FILE *f) {
    k->x1 = 0; k->x2 = 0xffffffff; k->x = 0; k->f = f;
    for (int i = 0; i < 4; i++) { int c = getc(f); k->x = (k->x << 8) | (U32)(c < 0 ? 0 : c); }
}
static void coder_encode(Coder *k, int p, int bit) {   /* p = P(bit=1), 12 bits */
    U32 mid = k->x1 + (U32)(((U64)(k->x2 - k->x1) * (U32)p) >> 12);
    if (bit) k->x2 = mid; else k->x1 = mid + 1;
    while (((k->x1 ^ k->x2) & 0xff000000u) == 0) {
        putc((int)(k->x2 >> 24), k->f);
        k->x1 <<= 8; k->x2 = (k->x2 << 8) | 255;
    }
}
static int coder_decode(Coder *k, int p) {
    U32 mid = k->x1 + (U32)(((U64)(k->x2 - k->x1) * (U32)p) >> 12);
    int bit = k->x <= mid;
    if (bit) k->x2 = mid; else k->x1 = mid + 1;
    while (((k->x1 ^ k->x2) & 0xff000000u) == 0) {
        k->x1 <<= 8; k->x2 = (k->x2 << 8) | 255;
        int c = getc(k->f);
        k->x = (k->x << 8) | (U32)(c < 0 ? 0 : c);
    }
    return bit;
}
static void coder_flush(Coder *k) { for (int i = 0; i < 4; i++) { putc((int)(k->x1 >> 24), k->f); k->x1 <<= 8; } }

/* ------------------------------------------------------------- counters -- */
/*
 * A probability with its own learning rate: the first observations in a
 * context move the estimate far, later ones only polish. Same scheme as
 * in nvjpg.
 */

typedef struct { U16 p; U16 n; } Counter;
#define RMAX 60
/*
 * The rate is a table lookup, not a division: one division per model and
 * bit means seven divisions per bit, clearly visible in profiles.
 */
static int RATE[RMAX + 1];
static void build_rates(void) { for (int i = 0; i <= RMAX; i++) RATE[i] = 65536 * 2 / (i + 3); }
static inline int ctr_p(const Counter *r) { return r->p >> 4; }       /* 16 -> 12 bits */
static inline void ctr_update(Counter *r, int bit) {
    int target = bit ? 65535 : 0;
    r->p = (U16)((int)r->p + (((target - (int)r->p) * RATE[r->n]) >> 16));
    if (r->n < RMAX) r->n++;
}

/* --------------------------------------------------------------- models -- */

#ifndef NORD
#define NORD   8                 /* number of context models (orders) */
#endif
#ifndef VERIFY
#define VERIFY 1                 /* compare the match's bytes, do not trust the hash */
#endif
#ifndef MIXCTX
#define MIXCTX 4                 /* 2 = let the match length pick the weight set */
#endif
#define NMOD   (NORD + 2)        /* number of votes into the mixer */
#ifndef TBITS
#define TBITS 24                 /* cap: 16 M slots per context model */
#endif
/*
 * The tables scale with the input size: a one-megabyte block never fills
 * 16 M slots, and zeroing half a gigabyte of counters for it takes longer
 * than the whole compression. The formula depends only on the input
 * length, which is in the header, so the decoder derives the same size.
 */
static int table_bits_for(U64 n) {
    int b = 16;
    while (b < TBITS && ((U64)1 << (b - 2)) < n) b++;
    return b;
}
#ifndef HBITS
#define HBITS 22                 /* the match model's hash table */
#endif
#ifndef MORD
#define MORD 6                   /* how many bytes the match model keys on */
#endif

typedef struct {
    Counter *t[NORD];            /* order 1,2,3,4,6(,8) + the word model */
    U32 tmask;                   /* slots per table minus one */
    U32 hash[NORD];              /* context hash at the byte boundary */
    U32 *mhash;                  /* match model: hash -> position */
    U8  *buf; size_t bufn, bufcap;
    size_t mpos; int mlen;       /* the match in progress */
    Counter mp[64][2];           /* the match model's own estimate */
    short weight[256 * MIXCTX][NMOD];  /* mixer weights, per context */
    Counter apm1[1024 * 33], apm2[4096 * 33];
    U32 base[NORD];              /* row start in each table for the current nibble */
    U32 c0;                      /* the byte in progress, with a leading one */
    U32 c0n;                     /* the NIBBLE in progress, with a leading one (1..15) */
    U32 c4;                      /* the four most recent bytes */
    U32 word;                    /* hash of the word in progress */
    int bitpos;
} Model;

/* 0 = the word model, -1 = order zero (no history at all) */
#if NORD == 8
static const int ORDERS[8] = {-1, 1, 2, 3, 4, 6, 8, 0};
#elif NORD == 7
static const int ORDERS[7] = {1, 2, 3, 4, 6, 8, 0};
#else
static const int ORDERS[6] = {1, 2, 3, 4, 6, 0};
#endif

static Model *model_new(size_t input_len) {
    Model *m = calloc(1, sizeof *m);
    if (!m) die("out of memory");
    U32 tsize = (U32)1 << table_bits_for(input_len);
    m->tmask = tsize - 1;
    for (int i = 0; i < NORD; i++) {
        m->t[i] = malloc((size_t)tsize * sizeof(Counter));
        if (!m->t[i]) die("out of memory");
        for (U32 j = 0; j < tsize; j++) { m->t[i][j].p = 32768; m->t[i][j].n = 0; }
    }
    m->mhash = calloc((size_t)1 << HBITS, sizeof(U32));
    m->bufcap = input_len + 64;
    m->buf = malloc(m->bufcap);
    if (!m->mhash || !m->buf) die("out of memory");
    for (int i = 0; i < 64; i++) for (int j = 0; j < 2; j++) { m->mp[i][j].p = 32768; m->mp[i][j].n = 0; }
    for (int i = 0; i < 256 * MIXCTX; i++) for (int j = 0; j < NMOD; j++) m->weight[i][j] = 0;
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
static void model_free(Model *m) {
    for (int i = 0; i < NORD; i++) free(m->t[i]);
    free(m->mhash); free(m->buf); free(m);
}

/*
 * Adaptive probability map: given a finished probability and a small
 * context, learns how the estimate tends to be skewed there. Cheap, and
 * often worth a few percent, especially early in a file before the models
 * have learned.
 */
static int apm(Counter *t, int pr, int cx, int bit, int update) {
    int s = STRETCH[pr] + 2048;             /* 0..4095 */
    int w = s & 127;
    int i = cx * 33 + (s >> 7);
    int out = (ctr_p(&t[i]) * (128 - w) + ctr_p(&t[i + 1]) * w) >> 7;
    if (update) { ctr_update(&t[i], bit); ctr_update(&t[i + 1], bit); }
    return out;
}

/*
 * One bit: gather the votes, mix, code, train. Encoding and decoding are
 * the same function with the same update order; anything else and they
 * fall out of step.
 */
static int bit_step(Model *m, Coder *k, int encode, int bit_in) {
    Counter *slot[NMOD];
    short st[NMOD];
    int n = 0;

    /*
     * All fifteen bit paths through a nibble sit on the same cache line:
     * two cache misses per byte and model instead of eight.
     */
    for (int i = 0; i < NORD; i++) {
        U32 idx = m->base[i] + (m->c0n & 15);
        slot[n] = &m->t[i][idx];
        st[n] = STRETCH[ctr_p(slot[n])];
        n++;
    }

    /* Match model: the bit the earlier occurrence predicts, if any. */
    int mbit = -1;
    if (m->mlen > 0 && m->mpos < m->bufn) {
        U32 predicted = m->buf[m->mpos];
        U32 high = m->c0;                      /* leading one + the bits coded so far */
        int bits = m->bitpos;
        if ((U32)((predicted | 256) >> (8 - bits)) == high) {
            mbit = (int)(predicted >> (7 - bits)) & 1;
        } else {
            m->mlen = 0;                       /* the match broke mid-byte */
        }
    }
    int mbucket = m->mlen > 31 ? 31 : m->mlen;
    if (mbit >= 0) {
        slot[n] = &m->mp[mbucket * 2 + mbit][0];
        st[n] = (short)(mbit ? STRETCH[ctr_p(slot[n])] : -STRETCH[ctr_p(slot[n])]);
    } else {
        slot[n] = &m->mp[0][1];
        st[n] = 0;
    }
    int match_slot = n;
    n++;

    /*
     * Mixer: the weight set is picked by the byte in progress, so the
     * balance differs between the start and end of a character.
     */
#if MIXCTX > 1
    /*
     * A long running match changes what every vote is worth, so match
     * states get weight sets of their own.
     */
    int wsel = (int)(m->c0 & 255) + 256 * (m->mlen == 0 ? 0 : m->mlen < 8 ? 1 : m->mlen < 24 ? 2 : 3);
#else
    int wsel = (int)(m->c0 & 255);
#endif
    short *w = m->weight[wsel];
    int sum = 0;
    for (int i = 0; i < n; i++) sum += st[i] * w[i];
    sum >>= 16;
    int pr = squash(sum);

    /* Two adaptive maps, weighed against the raw estimate. */
    int pr1 = apm(m->apm1, pr, (int)(m->c0 & 1023), 0, 0);
    int pr2 = apm(m->apm2, pr, (int)((m->c4 & 0xff) << 4 | (m->c0 & 15)), 0, 0);
    int final = (pr + pr1 + 2 * pr2 + 2) >> 2;
    if (final < 1) final = 1;
    if (final > 4094) final = 4094;

    int bit;
    if (encode) { bit = bit_in; coder_encode(k, final, bit); }
    else bit = coder_decode(k, final);

    /*
     * Train: mixer weights against the error, then every vote against the
     * outcome.
     */
    int err = ((bit << 12) - pr) * 6;
    for (int i = 0; i < n; i++) {
        int d = (st[i] * err * 2 >> 16);
        int nw = w[i] + ((d + 1) >> 1);
        w[i] = (short)(nw > 32767 ? 32767 : nw < -32768 ? -32768 : nw);
    }
    for (int i = 0; i < n; i++) {
        if (i == match_slot && mbit < 0) continue;
        if (i == match_slot) ctr_update(slot[i], bit == mbit);
        else ctr_update(slot[i], bit);
    }
    apm(m->apm1, pr, (int)(m->c0 & 1023), bit, 1);
    apm(m->apm2, pr, (int)((m->c4 & 0xff) << 4 | (m->c0 & 15)), bit, 1);

    if (mbit >= 0 && bit != mbit) m->mlen = 0;
    return bit;
}

/*
 * At every nibble boundary the row moves: the second half of the byte gets
 * its own row, keyed by what the first half turned out to be.
 */
static void set_rows(Model *m, U32 nibble) {
    for (int i = 0; i < NORD; i++) {
        m->base[i] = (m->hash[i] ^ (nibble * 0x85EBCA6Bu)) & (m->tmask & ~15u);
        __builtin_prefetch(&m->t[i][m->base[i]], 1, 3);
    }
}

/* After each whole byte: advance the contexts and the match model. */
static void byte_done(Model *m, U8 c) {
    if (m->bufn + 1 >= m->bufcap) {
        m->bufcap = m->bufcap * 2 + 1024;
        m->buf = realloc(m->buf, m->bufcap);
        if (!m->buf) die("out of memory");
    }
    m->buf[m->bufn++] = c;
    m->c4 = (m->c4 << 8) | c;

    /* Word model: identifier characters extend the hash, anything else resets it. */
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')
        m->word = m->word * 0x6F4F2F1Du + c + 1;
    else
        m->word = 0;

    U64 h = 0;
    for (int i = 0; i < NORD; i++) {
        int order = ORDERS[i];
        if (order == 0) { m->hash[i] = m->word * 0x2545F491u + 0x9E3779B1u; continue; }
        if (order < 0) { m->hash[i] = 0; continue; }
        h = 0;
        for (int j = 0; j < order && j < (int)m->bufn; j++)
            h = h * 0x100000001B3ull + m->buf[m->bufn - 1 - j] + 1;
        m->hash[i] = (U32)((h ^ (h >> 29)) * (U32)(order * 0x9E3779B1u + 1));
    }

    /*
     * Match model: keyed by the six most recent bytes. On a hit, mpos
     * points at the byte after the earlier occurrence.
     */
    if (m->bufn >= MORD) {
        U64 mh = 0;
        for (int j = 0; j < MORD; j++) mh = mh * 0x100000001B3ull + m->buf[m->bufn - 1 - j] + 1;
        U32 idx = (U32)((mh ^ (mh >> 31)) & (((U32)1 << HBITS) - 1));
        if (m->mlen > 0 && m->mpos < m->bufn && m->buf[m->mpos] == c) {
            m->mpos++;
            if (m->mlen < 63) m->mlen++;
        } else {
            U32 cand = m->mhash[idx];
            /*
             * The hash can lie, and a false match is worse than none: it
             * hands the mixer a confident vote that is wrong.
             */
            if (cand >= MORD && cand < m->bufn
#if VERIFY
                && memcmp(m->buf + cand - MORD, m->buf + m->bufn - MORD, MORD) == 0
#endif
               ) { m->mpos = cand; m->mlen = 1; }
            else m->mlen = 0;
        }
        m->mhash[idx] = (U32)m->bufn;
    }
    m->c0 = 1;
    m->c0n = 1;
    m->bitpos = 0;
    set_rows(m, 0);
}

/* ------------------------------------------------------------------ main -- */

/*
 * The magic is NVCM1. An early build wrote NVZ01; that magic is still
 * accepted so data already packed with it stays readable. Both are format
 * constants.
 */
static const char *MAGIC = "NVCM1";
static const char *MAGIC_OLD = "NVZ01";


static U8 *read_all(FILE *f, size_t *n) {
    size_t cap = 1 << 22, len = 0;
    U8 *d = malloc(cap);
    if (!d) die("out of memory");
    for (;;) {
        if (len == cap) { cap *= 2; d = realloc(d, cap); if (!d) die("out of memory"); }
        size_t r = fread(d + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    *n = len;
    return d;
}

static int pack_stream(FILE *in, FILE *out) {
    size_t n;
    U8 *d = read_all(in, &n);
    fwrite(MAGIC, 1, 5, out);
    U64 len = n;
    fwrite(&len, 8, 1, out);
    Model *m = model_new(n);
    Coder k; coder_init_out(&k, out);
    for (size_t i = 0; i < n; i++) {
        U8 c = d[i];
        for (int b = 7; b >= 0; b--) {
            int bit = (c >> b) & 1;
            bit_step(m, &k, 1, bit);
            m->c0 = (m->c0 << 1) | (U32)bit;
            m->c0n = (m->c0n << 1) | (U32)bit;
            if (++m->bitpos == 4) { set_rows(m, m->c0n); m->c0n = 1; }
        }
        byte_done(m, c);
    }
    coder_flush(&k);
    model_free(m); free(d);
    return 0;
}

static int unpack_stream(FILE *in, FILE *out) {
    char magic[5];
    if (fread(magic, 1, 5, in) != 5 || (memcmp(magic, MAGIC, 5) && memcmp(magic, MAGIC_OLD, 5)))
        die("not an hzcm stream");
    U64 len;
    if (fread(&len, 8, 1, in) != 1) die("broken header");
    Model *m = model_new((size_t)len);
    Coder k; coder_init_in(&k, in);
    U8 *outd = malloc((size_t)len + 1);
    if (!outd) die("out of memory");
    for (U64 i = 0; i < len; i++) {
        U32 c = 0;
        for (int b = 7; b >= 0; b--) {
            int bit = bit_step(m, &k, 0, 0);
            m->c0 = (m->c0 << 1) | (U32)bit;
            m->c0n = (m->c0n << 1) | (U32)bit;
            if (++m->bitpos == 4) { set_rows(m, m->c0n); m->c0n = 1; }
            c = (c << 1) | (U32)bit;
        }
        byte_done(m, (U8)c);
        outd[i] = (U8)c;
    }
    fwrite(outd, 1, (size_t)len, out);
    free(outd); model_free(m);
    return 0;
}


/* ------------------------------------------------------- gzip-style CLI -- */
/*
 * hzcm is invoked like gzip:
 *
 *     hzcm file           -> file.hzcm, the original is removed
 *     hzcm -d file.hzcm   -> file, the packed one is removed
 *     hzcm -c file > out  -> to stdout, the file is untouched
 *     hzcm -t file.hzcm   -> tests that it can be unpacked
 *     hzcm -l file.hzcm   -> the sizes without unpacking
 *     ... | hzcm | ...    -> no file arguments: stdin to stdout
 *
 * gzip's syntax rather than a private one, because existing scripts,
 * Makefiles and benchmarks already speak it; a codec that demands
 * `hzcm pack < in > out` cannot be dropped into an existing invocation
 * and then never gets measured.
 *
 * -1..-9, --fast and --best are accepted and ignored: hzcm has no levels,
 * but scripts send them and refusing a meaningless flag helps nothing.
 *
 * The worker subcommands `pack` and `unpack` (and their old names `packa`
 * and `packaupp`) remain for hz but are not listed in the help.
 */

static const char *SUF = ".hzcm";

typedef struct { int d, c, k, f, t, l, v; } Flags;

static int grumble(const char *name, const char *what) {
    fprintf(stderr, "hzcm: %s: %s\n", name, what);
    return 1;
}

static void keep_metadata(const char *target, const struct stat *st) {
    struct timespec times[2] = { st->st_atim, st->st_mtim };
    if (chmod(target, st->st_mode & 07777) != 0) { /* best effort */ }
    if (utimensat(AT_FDCWD, target, times, 0) != 0) { /* best effort */ }
}

/* The header carries the unpacked length, so -l need not unpack anything. */
static int list_file(const char *name) {
    FILE *f = fopen(name, "rb");
    if (!f) return grumble(name, strerror(errno));
    char magic[5]; U64 len = 0;
    int ok = fread(magic, 1, 5, f) == 5 && !memcmp(magic, "NVCM1", 5)
             && fread(&len, 8, 1, f) == 1;
    struct stat st;
    int have_st = stat(name, &st) == 0;
    fclose(f);
    if (!ok) return grumble(name, "not an hzcm stream");
    double packed = have_st ? (double)st.st_size : 0;
    double ratio = len ? 100.0 * (1.0 - packed / (double)len) : 0;
    printf("%12.0f %12llu %6.1f%% %s\n", packed, (unsigned long long)len, ratio, name);
    return 0;
}

static int do_file(const char *name, Flags fl) {
    if (fl.l) return list_file(name);

    size_t nlen = strlen(name), slen = strlen(SUF);
    int has_suffix = nlen > slen && !strcmp(name + nlen - slen, SUF);
    if (!fl.d && has_suffix && !fl.c && !fl.t)
        return grumble(name, "already has the suffix .hzcm -- left alone");
    if ((fl.d || fl.t) && !has_suffix && !fl.c)
        return grumble(name, "unknown suffix -- left alone");

    struct stat st;
    if (stat(name, &st) != 0) return grumble(name, strerror(errno));
    if (!S_ISREG(st.st_mode)) return grumble(name, "not a regular file");

    char target[4096];
    int to_stdout = fl.c || fl.t;
    if (fl.t) {
        snprintf(target, sizeof target, "/dev/null");
    } else if (fl.d) {
        if (nlen - slen >= sizeof target) return grumble(name, "file name too long");
        snprintf(target, sizeof target, "%.*s", (int)(nlen - slen), name);
    } else {
        if (snprintf(target, sizeof target, "%s%s", name, SUF) >= (int)sizeof target)
            return grumble(name, "file name too long");
    }
    if (!to_stdout && !fl.f && access(target, F_OK) == 0)
        return grumble(target, "already exists -- not overwritten (use -f)");

    FILE *in = fopen(name, "rb");
    if (!in) return grumble(name, strerror(errno));
    FILE *out = fl.c ? stdout : fopen(fl.t ? "/dev/null" : target, "wb");
    if (!out) { fclose(in); return grumble(target, strerror(errno)); }

    (fl.d || fl.t) ? unpack_stream(in, out) : pack_stream(in, out);
    fflush(out);
    long packed = fl.c ? -1 : ftell(out);
    fclose(in);
    if (!fl.c) fclose(out);

    if (!to_stdout) {
        keep_metadata(target, &st);
        if (!fl.k && unlink(name) != 0) grumble(name, strerror(errno));
    }
    if (fl.t) { printf("%s: OK\n", name); return 0; }
    if (fl.v && packed > 0) {
        double before = fl.d ? (double)packed : (double)st.st_size;
        double after = fl.d ? (double)st.st_size : (double)packed;
        fprintf(stderr, "%s:\t%5.1f%% -- %s %s\n", name,
                before ? 100.0 * (1.0 - after / before) : 0.0,
                fl.k ? "wrote" : "replaced with", target);
    }
    return 0;
}

static void help(void) {
    printf(
"usage: hzcm [flags] [file ...]\n"
"\n"
"  -d, --decompress   unpack\n"
"  -c, --stdout       write to stdout, leave the file alone\n"
"  -k, --keep         keep the file instead of replacing it\n"
"  -f, --force        overwrite a file that already exists\n"
"  -t, --test         test that the packed data can be unpacked\n"
"  -l, --list         show packed and unpacked size\n"
"  -v, --verbose      say what happened\n"
"  -1 .. -9           accepted and ignored (hzcm has no levels)\n"
"  -h, --help         this text\n"
"  -V, --version      the version\n"
"\n"
"With no file arguments the stream goes stdin -> stdout. The suffix is .hzcm.\n");
}

int main(int argc, char **argv) {
    build_stretch();
    build_rates();

    /*
     * Worker path first: hz calls `hzcm pack|unpack`. The old Swedish
     * names remain as aliases.
     */
    if (argc >= 2 && (!strcmp(argv[1], "pack") || !strcmp(argv[1], "packa")))
        return pack_stream(stdin, stdout);
    if (argc >= 2 && (!strcmp(argv[1], "unpack") || !strcmp(argv[1], "packaupp")))
        return unpack_stream(stdin, stdout);

    Flags fl = {0};
    int i = 1, end_of_flags = 0;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (end_of_flags || a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--")) { end_of_flags = 1; i++; break; }
        if (a[1] == '-') {
            if (!strcmp(a, "--decompress") || !strcmp(a, "--uncompress")) fl.d = 1;
            else if (!strcmp(a, "--stdout") || !strcmp(a, "--to-stdout")) fl.c = 1;
            else if (!strcmp(a, "--keep")) fl.k = 1;
            else if (!strcmp(a, "--force")) fl.f = 1;
            else if (!strcmp(a, "--test")) fl.t = 1;
            else if (!strcmp(a, "--list")) fl.l = 1;
            else if (!strcmp(a, "--verbose")) fl.v = 1;
            else if (!strcmp(a, "--fast") || !strcmp(a, "--best")) { /* no levels */ }
            else if (!strcmp(a, "--help")) { help(); return 0; }
            else if (!strcmp(a, "--version")) { printf("hzcm 1.0\n"); return 0; }
            else { fprintf(stderr, "hzcm: unknown flag %s\n", a); return 1; }
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
            case 'h': help(); return 0;
            case 'V': printf("hzcm 1.0\n"); return 0;
            default:
                fprintf(stderr, "hzcm: unknown flag -%c\n", *c);
                return 1;
            }
        }
    }

    if (i >= argc) {
        /* No file: stdin -> stdout, like gzip in a pipe. */
        if (fl.l) { fprintf(stderr, "hzcm: -l needs a file\n"); return 1; }
        if (!fl.d && !fl.t && isatty(1) && !fl.f) {
            fprintf(stderr, "hzcm: compressed data is not written to a terminal. "
                            "Use -f if you mean it.\n");
            return 1;
        }
        if (fl.t) { FILE *n = fopen("/dev/null", "wb");
                    int r = unpack_stream(stdin, n ? n : stdout);
                    if (n) fclose(n);
                    if (!r) printf("stdin: OK\n");
                    return r; }
        return (fl.d ? unpack_stream : pack_stream)(stdin, stdout);
    }

    if (fl.l) printf("%12s %12s %6s %s\n", "packed", "unpacked", "ratio", "name");
    int err = 0;
    for (; i < argc; i++) err |= do_file(argv[i], fl);
    return err;
}
