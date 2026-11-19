// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvjpg - bit-exact lossless repacking of baseline JPEG.
 *
 *     nvjpg pack   < image.jpg > image.nvj     exit 2 = cannot, store raw
 *     nvjpg unpack < image.nvj > image.jpg
 *     (the old names packa|packaupp remain as aliases; hz runs nvjpg as a
 *     worker process)
 *
 * A JPEG is DCT coefficients Huffman-coded with tables carried in the
 * file. Huffman spends at least one bit per symbol and ignores the
 * neighbouring blocks, yet the coefficients are strongly patterned: a
 * block's nonzero count is almost fully predicted by the two neighbouring
 * blocks, and a coefficient's magnitude by the same coefficient in the
 * blocks to the left and above.
 *
 * nvjpg decodes the Huffman stream into coefficients and recodes them with
 * an adaptive arithmetic coder over exactly those contexts. The pixels are
 * never touched; only the entropy layer is recoded. Unpacking encodes the
 * same coefficients back through the file's own Huffman tables and
 * reproduces exactly the original bytes.
 *
 * Before anything is written out, the packer re-encodes the coefficients
 * to Huffman and compares with the original's bytes. Any mismatch exits
 * with status 2 and the archive stores the file raw: an archiver may have
 * a transform it cannot do, but it must never assume one worked that did
 * not.
 *
 * Support: baseline sequential (SOF0/SOF1), one or more components,
 * arbitrary subsampling, restart markers. Progressive JPEG (SOF2) and
 * arithmetic-coded JPEG are left to the raw path.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* --------------------------------------------------------------- helpers -- */

static void die(const char *m) { fprintf(stderr, "nvjpg: %s\n", m); exit(2); }

typedef struct { uint8_t *d; size_t n, cap; } Buf;
static void buf_append(Buf *b, const void *p, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 65536;
        b->d = realloc(b->d, b->cap);
        if (!b->d) die("out of memory");
    }
    memcpy(b->d + b->n, p, n); b->n += n;
}
static void buf_byte(Buf *b, uint8_t v) { buf_append(b, &v, 1); }

static uint8_t *read_all(FILE *f, size_t *n) {
    size_t cap = 1 << 20, len = 0; uint8_t *d = malloc(cap);
    for (;;) {
        if (len == cap) { cap *= 2; d = realloc(d, cap); if (!d) die("out of memory"); }
        size_t r = fread(d + len, 1, cap - len, f);
        if (!r) break;
        len += r;
    }
    *n = len; return d;
}

/* ----------------------------------------------------- arithmetic coder -- */
/*
 * Binary range coder in the LZMA style: 11-bit probabilities, adaptation
 * shift 5. Small, fast and exactly symmetric: the decoder makes the same
 * updates in the same order as the encoder.
 */

#define PBITS 11
#define PINIT  (1 << (PBITS - 1))
#ifndef BLEND
#define BLEND 2   /* 2 = a rate that falls with the number of observations */
#endif
#ifndef FAST_SHIFT
#define FAST_SHIFT 4
#endif
#ifndef SLOW_SHIFT
#define SLOW_SHIFT 7
#endif
#ifndef PSHIFT
#define PSHIFT 5
#endif

/*
 * A probability kept at two rates: the fast one tracks statistics that
 * shift from image area to image area, the slow one remembers the whole
 * image, and the average beats either alone. The same blend runs in both
 * directions, so encoder and decoder cannot drift apart.
 */
#if BLEND == 2
/*
 * The rate falls with the number of observations: the first bits in a
 * context move the estimate far, later bits only polish. That gives both a
 * fast start and a stable end state - what the two fixed rates aimed for,
 * but without the slow one dragging the average toward 50 % through the
 * whole image.
 */
#ifndef FINAL_RATE
#define FINAL_RATE 5    /* the rate levels off here; higher = longer memory */
#endif
#define RATE_CAP (2 * (FINAL_RATE - 1))
typedef struct { uint16_t p, n; } Prob;
static inline uint32_t p_of(const Prob *p) { return p->p; }
static inline void p_update(Prob *p, int bit) {
    int sh = 1 + (p->n >> 1);
    if (sh > FINAL_RATE) sh = FINAL_RATE;
    if (!bit) p->p += ((1 << PBITS) - p->p) >> sh; else p->p -= p->p >> sh;
    if (p->n < RATE_CAP) p->n++;
}
#elif BLEND == 1
typedef struct { uint16_t fast, slow; } Prob;
static inline uint32_t p_of(const Prob *p) { return ((uint32_t)p->fast + p->slow) >> 1; }
static inline void p_update(Prob *p, int bit) {
    if (!bit) { p->fast += ((1 << PBITS) - p->fast) >> FAST_SHIFT;
                p->slow += ((1 << PBITS) - p->slow) >> SLOW_SHIFT; }
    else      { p->fast -= p->fast >> FAST_SHIFT;
                p->slow -= p->slow >> SLOW_SHIFT; }
}
#define PROB_INIT { PINIT, PINIT }
#else
typedef struct { uint16_t p; } Prob;
static inline uint32_t p_of(const Prob *p) { return p->p; }
static inline void p_update(Prob *p, int bit) {
    if (!bit) p->p += ((1 << PBITS) - p->p) >> PSHIFT; else p->p -= p->p >> PSHIFT;
}
#endif

typedef struct { uint64_t low; uint32_t range; uint8_t cache; int64_t cache_count; Buf out; } Encoder;

static void enc_init(Encoder *k) {
    k->low = 0; k->range = 0xFFFFFFFFu; k->cache = 0; k->cache_count = 1;
    k->out.d = NULL; k->out.n = k->out.cap = 0;
}
static void enc_shift(Encoder *k) {
    if ((uint32_t)(k->low >> 32) != 0 || (uint32_t)k->low < 0xFF000000u) {
        int64_t i = k->cache_count;
        uint8_t carry = (uint8_t)(k->low >> 32);
        do { buf_byte(&k->out, (uint8_t)(k->cache + carry)); k->cache = 0xFF; } while (--i);
        k->cache = (uint8_t)((uint32_t)k->low >> 24);
        k->cache_count = 0;
    }
    k->cache_count++;
    k->low = (uint32_t)k->low << 8;
}
static void enc_bit(Encoder *k, Prob *p, int bit) {
    uint32_t bound = (k->range >> PBITS) * p_of(p);
    if (!bit) k->range = bound;
    else { k->low += bound; k->range -= bound; }
    p_update(p, bit);
    while (k->range < (1u << 24)) { k->range <<= 8; enc_shift(k); }
}
static void enc_direct(Encoder *k, uint32_t v, int bits) {   /* unadapted */
    for (int i = bits - 1; i >= 0; i--) {
        k->range >>= 1;
        if ((v >> i) & 1) k->low += k->range;
        while (k->range < (1u << 24)) { k->range <<= 8; enc_shift(k); }
    }
}
static void enc_finish(Encoder *k) { for (int i = 0; i < 5; i++) enc_shift(k); }

typedef struct { const uint8_t *d; size_t n, pos; uint32_t range, code; } Decoder;
static uint8_t dec_byte(Decoder *a) { return a->pos < a->n ? a->d[a->pos++] : 0; }
static void dec_init(Decoder *a, const uint8_t *d, size_t n) {
    a->d = d; a->n = n; a->pos = 0; a->range = 0xFFFFFFFFu; a->code = 0;
    dec_byte(a);
    for (int i = 0; i < 4; i++) a->code = (a->code << 8) | dec_byte(a);
}
static int dec_bit(Decoder *a, Prob *p) {
    uint32_t bound = (a->range >> PBITS) * p_of(p);
    int bit;
    if (a->code < bound) { a->range = bound; bit = 0; }
    else { a->code -= bound; a->range -= bound; bit = 1; }
    p_update(p, bit);
    while (a->range < (1u << 24)) { a->range <<= 8; a->code = (a->code << 8) | dec_byte(a); }
    return bit;
}
static uint32_t dec_direct(Decoder *a, int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        a->range >>= 1;
        uint32_t t = (a->code - a->range) >> 31;       /* 1 if code < range */
        if (!t) a->code -= a->range;
        v = (v << 1) | (1 - t);
        while (a->range < (1u << 24)) { a->range <<= 8; a->code = (a->code << 8) | dec_byte(a); }
    }
    return v;
}

/*
 * Tree coding: a `bits`-bit number through a binary tree where every node
 * has its own probability; context-sensitive coding of small integers.
 */
static void enc_tree(Encoder *k, Prob *p, int bits, uint32_t v) {
    uint32_t m = 1;
    for (int i = bits - 1; i >= 0; i--) {
        int bit = (v >> i) & 1;
        enc_bit(k, &p[m], bit);
        m = (m << 1) | bit;
    }
}
static uint32_t dec_tree(Decoder *a, Prob *p, int bits) {
    uint32_t m = 1;
    for (int i = 0; i < bits; i++) m = (m << 1) | dec_bit(a, &p[m]);
    return m - (1u << bits);
}

/* --------------------------------------------------------- JPEG structure -- */

typedef struct {
    uint8_t bits[17], vals[256];
    int mincode[17], maxcode[18], valptr[17];
    uint16_t code[256]; uint8_t size[256];       /* for re-encoding */
    int present;
} Huff;

typedef struct {
    int id, h, v, tq, td, ta;
    int bw, bh;                 /* size of the block plane */
    int16_t *coef;
} Comp;

typedef struct {
    int W, H, nk, hmax, vmax, mcux, mcuy, dri, prec;
    Comp k[4];
    Huff hdc[4], hac[4];
    size_t scan_start, scan_end;      /* the entropy data's byte range */
} Image;

static const uint8_t ZIGZAG[64] = {
     0, 1, 8,16, 9, 2, 3,10,17,24,32,25,18,11, 4, 5,
    12,19,26,33,40,48,41,34,27,20,13, 6, 7,14,21,28,
    35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,
    58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};

static void build_huff(Huff *h) {
    int code = 0, k = 0;
    int sizes[257]; int n = 0;
    for (int l = 1; l <= 16; l++) for (int i = 0; i < h->bits[l]; i++) sizes[n++] = l;
    sizes[n] = 0;
    /*
     * The re-encoding table is indexed by the symbol value, not by its
     * position in vals[]: encode_huffman() looks symbols up by value.
     */
    int si = sizes[0], i = 0; code = 0;
    memset(h->code, 0, sizeof h->code); memset(h->size, 0, sizeof h->size);
    while (sizes[i]) {
        while (sizes[i] == si) { h->code[h->vals[i]] = (uint16_t)code++; h->size[h->vals[i]] = (uint8_t)si; i++; }
        code <<= 1; si++;
    }
    k = 0; code = 0;
    for (int l = 1; l <= 16; l++) {
        if (h->bits[l]) {
            h->valptr[l] = k; h->mincode[l] = code;
            k += h->bits[l]; code += h->bits[l];
            h->maxcode[l] = code - 1;
        } else h->maxcode[l] = -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7FFFFFFF;
}

/* ------------------------------------------------ bit reading in the scan -- */

typedef struct { const uint8_t *d; size_t pos, end; uint32_t b; int n; int marker; } BitReader;

static int bl_bit(BitReader *r) {
    if (r->n == 0) {
        if (r->pos >= r->end) { r->marker = 1; return 0; }
        uint8_t c = r->d[r->pos++];
        if (c == 0xFF) {
            uint8_t c2 = r->pos < r->end ? r->d[r->pos] : 0xD9;
            if (c2 == 0x00) r->pos++;
            else { r->marker = 1; r->pos--; return 0; }
        }
        r->b = c; r->n = 8;
    }
    r->n--;
    return (r->b >> r->n) & 1;
}
static int bl_bits(BitReader *r, int n) {
    int v = 0; while (n--) v = (v << 1) | bl_bit(r); return v;
}
static int bl_huff(BitReader *r, Huff *h) {
    int code = bl_bit(r), l = 1;
    while (code > h->maxcode[l]) { code = (code << 1) | bl_bit(r); if (++l > 16) return -1; }
    return h->vals[h->valptr[l] + code - h->mincode[l]];
}
static int extend(int v, int t) { return v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

/* ------------------------------------------------------ bit writing out ---- */

typedef struct { Buf b; uint32_t acc; int n; } BitWriter;
static void bs_bits(BitWriter *w, int code, int n) {
    w->acc = (w->acc << n) | (uint32_t)(code & ((1 << n) - 1));
    w->n += n;
    while (w->n >= 8) {
        uint8_t c = (uint8_t)(w->acc >> (w->n - 8));
        buf_byte(&w->b, c);
        if (c == 0xFF) buf_byte(&w->b, 0x00);
        w->n -= 8;
    }
}
static void bs_fill(BitWriter *w) {           /* ones out to the byte boundary */
    if (w->n) bs_bits(w, (1 << (8 - w->n)) - 1, 8 - w->n);
    w->n = 0;
}

/* ---------------------------------------------------------- header parsing -- */

static size_t parse(Image *b, const uint8_t *d, size_t n) {
    if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) die("no SOI");
    size_t i = 2;
    while (i + 3 < n) {
        if (d[i] != 0xFF) die("expected a marker");
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
            if (b->nk > 4) die("too many components");
            for (int c = 0; c < b->nk; c++) {
                b->k[c].id = p[6 + c * 3];
                b->k[c].h = p[7 + c * 3] >> 4; b->k[c].v = p[7 + c * 3] & 15;
                b->k[c].tq = p[8 + c * 3];
                if (b->k[c].h < 1 || b->k[c].v < 1) die("invalid subsampling");
            }
            break; }
        case 0xC2: case 0xC3: case 0xC5: case 0xC6: case 0xC7:
        case 0xC9: case 0xCA: case 0xCB: case 0xCD: case 0xCE: case 0xCF:
            die("not baseline");                      /* progressive/arithmetic */
            /* never falls through: die() exits */
        case 0xC4: {
            size_t q = 0;
            while (q < pl) {
                int tc = p[q] >> 4, th = p[q] & 15; q++;
                if (th > 3) die("huffman table out of range");
                Huff *h = tc ? &b->hac[th] : &b->hdc[th];
                memset(h, 0, sizeof *h);
                int tot = 0;
                for (int l = 1; l <= 16; l++) { h->bits[l] = p[q + l - 1]; tot += h->bits[l]; }
                q += 16;
                for (int v = 0; v < tot; v++) h->vals[v] = p[q + v];
                q += tot;
                build_huff(h); h->present = 1;
            }
            break; }
        case 0xDD: b->dri = (p[0] << 8) | p[1]; break;
        case 0xDA: {
            int ns = p[0];
            if (ns != b->nk) die("scan does not cover all components");
            for (int s = 0; s < ns; s++) {
                int id = p[1 + s * 2];
                int c; for (c = 0; c < b->nk && b->k[c].id != id; c++) ;
                if (c == b->nk) die("unknown component in scan");
                b->k[c].td = p[2 + s * 2] >> 4; b->k[c].ta = p[2 + s * 2] & 15;
            }
            b->scan_start = i + 2 + len;
            return b->scan_start; }
        default: break;
        }
        i += 2 + len;
    }
    die("no scan");
    return 0;
}

static void allocate(Image *b) {
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
        size_t count = (size_t)b->k[c].bw * b->k[c].bh * 64;
        b->k[c].coef = calloc(count, sizeof(int16_t));
        if (!b->k[c].coef) die("out of memory for coefficients");
    }
}

/*
 * Decode the whole scan into coefficient planes. Returns the offset where
 * the entropy data ends.
 */
static size_t decode_scan(Image *b, const uint8_t *d, size_t n) {
    BitReader r = { d, b->scan_start, n, 0, 0, 0 };
    int pred[4] = {0, 0, 0, 0};
    long to_restart = b->dri;
    for (int my = 0; my < b->mcuy; my++)
        for (int mx = 0; mx < b->mcux; mx++) {
            if (b->dri && to_restart == 0) {
                /*
                 * Restart marker: the bit buffer empties and the DC
                 * prediction resets.
                 */
                r.n = 0;
                while (r.pos + 1 < n && !(d[r.pos] == 0xFF && d[r.pos+1] >= 0xD0 && d[r.pos+1] <= 0xD7)) r.pos++;
                r.pos += 2;
                pred[0] = pred[1] = pred[2] = pred[3] = 0;
                to_restart = b->dri;
            }
            for (int c = 0; c < b->nk; c++)
                for (int by = 0; by < b->k[c].v; by++)
                    for (int bx = 0; bx < b->k[c].h; bx++) {
                        int brx = mx * b->k[c].h + bx, bry = my * b->k[c].v + by;
                        int16_t *blk = b->k[c].coef + ((size_t)bry * b->k[c].bw + brx) * 64;
                        int t = bl_huff(&r, &b->hdc[b->k[c].td]);
                        if (t < 0) die("broken huffman code (DC)");
                        int diff = t ? extend(bl_bits(&r, t), t) : 0;
                        pred[c] += diff;
                        blk[0] = (int16_t)pred[c];
                        for (int k = 1; k < 64; ) {
                            int rs = bl_huff(&r, &b->hac[b->k[c].ta]);
                            if (rs < 0) die("broken huffman code (AC)");
                            int s = rs & 15, run = rs >> 4;
                            if (s == 0) { if (run != 15) break; k += 16; continue; }
                            k += run;
                            if (k > 63) die("AC index outside the block");
                            blk[ZIGZAG[k]] = (int16_t)extend(bl_bits(&r, s), s);
                            k++;
                        }
                    }
            if (b->dri) to_restart--;
        }
    /* The entropy data ends where the next marker begins. */
    size_t p = r.pos;
    while (p + 1 < n && !(d[p] == 0xFF && d[p + 1] != 0x00 && !(d[p+1] >= 0xD0 && d[p+1] <= 0xD7))) p++;
    return p;
}

/* Encode the coefficients back to Huffman: the same bytes as the original. */
static Buf encode_huffman(Image *b) {
    BitWriter w = { {NULL, 0, 0}, 0, 0 };
    int pred[4] = {0, 0, 0, 0};
    long to_restart = b->dri; int next_rst = 0;
    for (int my = 0; my < b->mcuy; my++)
        for (int mx = 0; mx < b->mcux; mx++) {
            if (b->dri && to_restart == 0) {
                bs_fill(&w);
                buf_byte(&w.b, 0xFF); buf_byte(&w.b, (uint8_t)(0xD0 + (next_rst & 7)));
                next_rst++;
                pred[0] = pred[1] = pred[2] = pred[3] = 0;
                to_restart = b->dri;
            }
            for (int c = 0; c < b->nk; c++)
                for (int by = 0; by < b->k[c].v; by++)
                    for (int bx = 0; bx < b->k[c].h; bx++) {
                        int brx = mx * b->k[c].h + bx, bry = my * b->k[c].v + by;
                        int16_t *blk = b->k[c].coef + ((size_t)bry * b->k[c].bw + brx) * 64;
                        Huff *hd = &b->hdc[b->k[c].td], *ha = &b->hac[b->k[c].ta];
                        int diff = blk[0] - pred[c]; pred[c] = blk[0];
                        int t = 0, a = diff < 0 ? -diff : diff;
                        while (a) { t++; a >>= 1; }
                        bs_bits(&w, hd->code[t], hd->size[t]);
                        if (t) bs_bits(&w, diff < 0 ? diff - 1 : diff, t);
                        int last = 0;
                        for (int k = 1; k < 64; k++) if (blk[ZIGZAG[k]]) last = k;
                        int run = 0;
                        for (int k = 1; k <= last; k++) {
                            int v = blk[ZIGZAG[k]];
                            if (!v) { run++; continue; }
                            while (run > 15) { bs_bits(&w, ha->code[0xF0], ha->size[0xF0]); run -= 16; }
                            int s = 0, m = v < 0 ? -v : v;
                            while (m) { s++; m >>= 1; }
                            int rs = (run << 4) | s;
                            bs_bits(&w, ha->code[rs], ha->size[rs]);
                            bs_bits(&w, v < 0 ? v - 1 : v, s);
                            run = 0;
                        }
                        if (last < 63) bs_bits(&w, ha->code[0], ha->size[0]);
                    }
            if (b->dri) to_restart--;
        }
    bs_fill(&w);
    return w.b;
}

/* -------------------------------------------------------------- the model -- */
/*
 * The contexts are chosen by what actually predicts the next number. Three
 * neighbourhoods are used, and they are different things:
 *
 *   blocks left and above: how busy this part of the image is. The number
 *   of nonzero coefficients in a block is predicted almost entirely by the
 *   counts in the two neighbouring blocks - smooth sky next to smooth sky.
 *
 *   the same slot in the neighbours: how large this particular frequency
 *   tends to be around here. A vertical edge pattern persists into the
 *   block next door.
 *
 *   the slots left and above in the same block: where in the block the
 *   energy sits. Both are already coded on arrival: the zigzag path always
 *   visits (r-1,c) and (r,c-1) before (r,c).
 *
 * DC is not coded the way JPEG does it (difference against the previous
 * block in scan order) but against the mean of the blocks left and above:
 * an image is read row by row but resembles itself in two directions, and
 * at the end of a row the previous block in scan order neighbours the
 * wrong edge of the image.
 *
 * Luma and chroma are kept apart because their statistics differ entirely.
 * The mantissa's lowest bits are coded directly: they are in practice
 * random, and a model on them costs more than it gives.
 */
#ifndef KB
#define KB 16   /* zigzag buckets */
#endif
#ifndef NB
#define NB 16                    /* buckets for "nonzeros remaining" */
#endif
#ifndef MB
#define MB 10   /* buckets for the neighbours' magnitude in the same block */
#endif
#ifndef EB
#define EB 3    /* 3 = weigh in how dense the block is in the magnitude model */
#endif
#ifndef NEIGHBOURS
#define NEIGHBOURS 1    /* how large an in-block neighbourhood is weighed in */
#endif
#ifndef CN
#define CN 4                    /* buckets for the neighbour block in its direction */
#endif
#define DB 7                    /* buckets for the DC slope */
#define ZB 7                    /* buckets for the neighbour blocks' nonzero counts */

typedef struct {
    Prob nnz[2][ZB][ZB][64];
    Prob dc_exp[2][DB][16];
    Prob dc_man[2][16][16];
    Prob dc_sign[2][DB];
    Prob ac_zero[2][KB][NB][MB][CN];
    Prob ac_exp[2][KB][MB][CN][EB][16];
    Prob ac_man[2][16][16];
    Prob ac_sign[2][KB];
} Model;

static void model_init(Model *m) {
    Prob *p = (Prob *)m;
    for (size_t i = 0; i < sizeof(Model) / sizeof(Prob); i++) {
#if BLEND
#if BLEND == 2
        p[i].p = PINIT; p[i].n = 0;
#else
        p[i].fast = PINIT; p[i].slow = PINIT;
#endif
#else
        p[i].p = PINIT;
#endif
    }
}
static int bucket_k(int k) {
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
static int bucket_mag(int m) {
#if MB >= 10
    return m == 0 ? 0 : m == 1 ? 1 : m == 2 ? 2 : m == 3 ? 3 : m == 4 ? 4
         : m <= 6 ? 5 : m <= 9 ? 6 : m <= 15 ? 7 : m <= 31 ? 8 : 9;
#else
    return m == 0 ? 0 : m == 1 ? 1 : m == 2 ? 2 : m <= 4 ? 3 : m <= 8 ? 4 : 5;
#endif
}
static int bucket_zb(int n) { return n == 0 ? 0 : n == 1 ? 1 : n == 2 ? 2 : n <= 4 ? 3 : n <= 8 ? 4 : n <= 16 ? 5 : 6; }
static int bucket_dc(int g) { return g == 0 ? 0 : g <= 1 ? 1 : g <= 3 ? 2 : g <= 7 ? 3 : g <= 15 ? 4 : g <= 63 ? 5 : 6; }

static int count_nnz(const int16_t *blk) {
    int n = 0;
    for (int k = 1; k < 64; k++) if (blk[ZIGZAG[k]]) n++;
    return n;
}

/*
 * Mantissa: the top bits below the leading one are modeled, the rest pass
 * straight through. The same split in both directions.
 */
#ifndef MODELED
#define MODELED 3
#endif

static void code_mantissa(Encoder *k, Decoder *a, Prob *p, int encode, int e, int *mag) {
    if (encode) {
        int m = *mag;
        for (int i = e - 2; i >= 0; i--) {
            int bit = (m >> i) & 1;
            if (i >= e - 1 - MODELED) enc_bit(k, &p[i > 15 ? 15 : i], bit);
            else enc_direct(k, (uint32_t)bit, 1);
        }
    } else {
        int m = 1;
        for (int i = e - 2; i >= 0; i--) {
            int bit = (i >= e - 1 - MODELED) ? dec_bit(a, &p[i > 15 ? 15 : i])
                                             : (int)dec_direct(a, 1);
            m = (m << 1) | bit;
        }
        *mag = m;
    }
}

/*
 * One function handles both encoding and decoding, steered by `encode`.
 * Deliberate: with only one description of the model, the two directions
 * cannot fall out of step.
 */
static void walk(Image *b, Model *m, Encoder *k, Decoder *a, int encode) {
    for (int c = 0; c < b->nk; c++) {
        int ck = c == 0 ? 0 : 1;
        Comp *K = &b->k[c];
        for (int by = 0; by < K->bh; by++)
            for (int bx = 0; bx < K->bw; bx++) {
                int16_t *blk = K->coef + ((size_t)by * K->bw + bx) * 64;
                const int16_t *left  = bx ? blk - 64 : NULL;
                const int16_t *above = by ? blk - (size_t)K->bw * 64 : NULL;

                /* --- number of nonzeros -------------------------------- */
                int nl = left ? count_nnz(left) : 0, na = above ? count_nnz(above) : 0;
                int nnz;
                Prob *pn = m->nnz[ck][bucket_zb(nl)][bucket_zb(na)];
                if (encode) { nnz = count_nnz(blk); enc_tree(k, pn, 6, (uint32_t)nnz); }
                else nnz = (int)dec_tree(a, pn, 6);

                /* --- DC against the mean of the two neighbours ---------- */
                int pred = (left && above) ? (left[0] + above[0]) / 2 : left ? left[0] : above ? above[0] : 0;
                int slope = (left && above) ? absi(left[0] - above[0]) : 0;
                int cd = bucket_dc(slope);
                if (encode) {
                    int diff = blk[0] - pred, e = exponent(diff);
                    enc_tree(k, m->dc_exp[ck][cd], 4, (uint32_t)e);
                    if (e) {
                        int mag = absi(diff);
                        code_mantissa(k, NULL, m->dc_man[ck][e > 15 ? 15 : e], 1, e, &mag);
                        enc_bit(k, &m->dc_sign[ck][cd], diff < 0);
                    }
                } else {
                    int e = (int)dec_tree(a, m->dc_exp[ck][cd], 4), diff = 0;
                    if (e) {
                        int mag = 1;
                        code_mantissa(NULL, a, m->dc_man[ck][e > 15 ? 15 : e], 0, e, &mag);
                        diff = dec_bit(a, &m->dc_sign[ck][cd]) ? -mag : mag;
                    }
                    blk[0] = (int16_t)(pred + diff);
                }

                /* --- AC ------------------------------------------------ */
                int remaining = nnz;
#if EB > 1
                int ce = nnz <= 2 ? 0 : nnz <= 8 ? 1 : EB - 1;
#else
                int ce = 0;
#endif
                for (int kk = 1; kk < 64 && remaining > 0; kk++) {
                    int z = ZIGZAG[kk], r = z >> 3, s = z & 7;
                    /*
                     * Neighbour magnitude: two slots in the same block,
                     * plus the same slot in the blocks left and above.
                     */
                    int gm = (s ? absi(blk[z - 1]) : 0) + (r ? absi(blk[z - 8]) : 0);
#if NEIGHBOURS >= 2
                    if (r && s) gm += (absi(blk[z - 9]) + 1) >> 1;      /* diagonal */
#endif
#if NEIGHBOURS >= 3
                    if (s > 1) gm += (absi(blk[z - 2]) + 1) >> 1;
                    if (r > 1) gm += (absi(blk[z - 16]) + 1) >> 1;
#endif
                    /*
                     * The neighbour block is weighed by direction: the top
                     * row of a block holds horizontal frequencies and
                     * continues down from the block above, the left column
                     * likewise sideways.
                     */
                    int gr = (r == 0) ? (above ? absi(above[z]) : 0)
                           : (s == 0) ? (left ? absi(left[z]) : 0)
                           : (((left ? absi(left[z]) : 0) + (above ? absi(above[z]) : 0) + 1) >> 1);
#if CN > 1
                    int cn = gr == 0 ? 0 : gr == 1 ? 1 : gr <= 3 ? 2 : CN - 1;
#else
                    int cn = 0; gm += gr;
#endif
                    int cm = bucket_mag(gm), ckk = bucket_k(kk);
                    int cnq = remaining > NB - 1 ? NB - 1 : remaining;
                    int nonzero;
                    /*
                     * When no more slots remain than nonzeros, the answer
                     * is forced and must not cost a single bit.
                     */
                    int forced = (64 - kk <= remaining);
                    if (encode) {
                        nonzero = blk[z] != 0;
                        if (!forced) enc_bit(k, &m->ac_zero[ck][ckk][cnq][cm][cn], nonzero);
                    } else {
                        nonzero = forced ? 1 : dec_bit(a, &m->ac_zero[ck][ckk][cnq][cm][cn]);
                    }
                    if (!nonzero) continue;
                    if (encode) {
                        int val = blk[z], e = exponent(val), mag = absi(val);
                        enc_tree(k, m->ac_exp[ck][ckk][cm][cn][ce], 4, (uint32_t)e);
                        code_mantissa(k, NULL, m->ac_man[ck][e > 15 ? 15 : e], 1, e, &mag);
                        enc_bit(k, &m->ac_sign[ck][ckk], val < 0);
                    } else {
                        int e = (int)dec_tree(a, m->ac_exp[ck][ckk][cm][cn][ce], 4), mag = 1;
                        code_mantissa(NULL, a, m->ac_man[ck][e > 15 ? 15 : e], 0, e, &mag);
                        blk[z] = (int16_t)(dec_bit(a, &m->ac_sign[ck][ckk]) ? -mag : mag);
                    }
                    remaining--;
                }
            }
    }
}

/* ------------------------------------------------------------------ main --- */

static void write_u32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static uint32_t read_u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static int pack_main(void) {
    size_t n; uint8_t *d = read_all(stdin, &n);
    Image b; memset(&b, 0, sizeof b);
    parse(&b, d, n);
    allocate(&b);
    size_t end = decode_scan(&b, d, n);

    /* Verify before trusting the transform: encode back to Huffman and compare. */
    Buf again = encode_huffman(&b);
    size_t orig_len = end - b.scan_start;
    int last_literal = 0;
    if (again.n != orig_len) return 2;
    if (memcmp(again.d, d + b.scan_start, orig_len ? orig_len - 1 : 0) != 0) return 2;
    if (orig_len && again.d[orig_len - 1] != d[b.scan_start + orig_len - 1]) last_literal = 1;

    Model m; model_init(&m);
    Encoder k; enc_init(&k);
    walk(&b, &m, &k, NULL, 1);
    enc_finish(&k);

    fwrite("NVJ1", 1, 4, stdout);
    write_u32(stdout, (uint32_t)b.scan_start);
    write_u32(stdout, (uint32_t)(n - end));
    write_u32(stdout, (uint32_t)orig_len);
    fputc(last_literal, stdout);
    fputc(orig_len ? d[b.scan_start + orig_len - 1] : 0, stdout);
    fwrite(d, 1, b.scan_start, stdout);          /* the header verbatim */
    fwrite(d + end, 1, n - end, stdout);         /* the tail verbatim */
    fwrite(k.out.d, 1, k.out.n, stdout);
    return 0;
}

static int unpack_main(void) {
    size_t n; uint8_t *d = read_all(stdin, &n);
    if (n < 18 || memcmp(d, "NVJ1", 4)) die("not an nvj stream");
    uint32_t head_len = read_u32(d + 4), tail_len = read_u32(d + 8), scan_len = read_u32(d + 12);
    int last_literal = d[16]; uint8_t last = d[17];
    const uint8_t *head = d + 18, *tail = head + head_len, *stream = tail + tail_len;
    size_t stream_len = n - 18 - head_len - tail_len;

    Image b; memset(&b, 0, sizeof b);
    parse(&b, head, head_len);
    allocate(&b);
    Model m; model_init(&m);
    Decoder a; dec_init(&a, stream, stream_len);
    walk(&b, &m, NULL, &a, 0);

    Buf again = encode_huffman(&b);
    if (again.n != scan_len) die("scan length does not match");
    if (last_literal && again.n) again.d[again.n - 1] = last;
    fwrite(head, 1, head_len, stdout);
    fwrite(again.d, 1, again.n, stdout);
    fwrite(tail, 1, tail_len, stdout);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: nvjpg pack|unpack < in > out\n"); return 1; }
    if (!strcmp(argv[1], "pack") || !strcmp(argv[1], "packa")) return pack_main();
    if (!strcmp(argv[1], "unpack") || !strcmp(argv[1], "packaupp")) return unpack_main();
    fprintf(stderr, "unknown command\n");
    return 1;
}
