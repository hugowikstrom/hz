// SPDX-License-Identifier: GPL-2.0-only
// rANS entropy codec: order-0, 4-way interleaved, chunked static tables.
//
// The algorithm is Jarek Duda's asymmetric numeral systems in the "range"
// variant; state layout, renormalization and the reverse-encode trick
// follow Fabian Giesen's rans_byte (ryg_rans, public domain) — search
// "Fabian Giesen rANS". This implementation is written fresh for hyperZip.
//
// Why it exists, and what to expect of it: rANS is a pure entropy coder —
// no match finding, no context. Its selling point is decode speed (one
// multiply, one table slot and an occasional byte read per symbol, four
// streams in flight to hide latency), not ratio. zstd already carries ANS
// internally (FSE) behind an LZ front end, so on whole blocks rANS cannot
// beat the probe's existing candidates by construction. It is wired in as
// a codec all the same, chosen the only honest way: by measurement. See
// the README for where it landed.
//
// Container: "HZR1", u64 raw length, then chunks. Per chunk: u32 raw_len,
// u32 stream_len, 256 x u16 frequencies summing to 4096 (0 = absent
// symbol), 4 x u32 initial decoder states, stream bytes. The encoder
// walks the chunk BACKWARDS (ANS is LIFO), so the decoder reads strictly
// forward — the property that makes interleaving and wide decode work.

#include "codec.hpp"
#include <cstring>
#include <algorithm>

namespace hz {
namespace {

constexpr u32 PROB_BITS = 12;                  // frequencies sum to 4096
constexpr u32 PROB_SCALE = 1u << PROB_BITS;
constexpr u32 RANS_L = 1u << 23;               // renormalization floor
constexpr size_t CHUNK = 1u << 20;             // table cost 528 B per MiB
constexpr char RANS_MAGIC[4] = {'H','Z','R','1'};

// Scale a chunk histogram so it sums to exactly PROB_SCALE with every
// present symbol >= 1. Rounding drift is settled against the most common
// symbol, where one slot more or less costs the least.
void normalize(const u64 *hist, u64 n, u16 *freq) {
    u32 sum = 0; int top = 0;
    for (int s = 0; s < 256; s++) {
        if (!hist[s]) { freq[s] = 0; continue; }
        u64 f = hist[s] * PROB_SCALE / n;
        freq[s] = (u16)std::max<u64>(1, f);
        sum += freq[s];
        if (hist[s] > hist[top]) top = s;
    }
    freq[top] = (u16)(freq[top] + (int)PROB_SCALE - (int)sum);
}

void put_u32(Byte &b, u32 v) { for (int i = 0; i < 4; i++) b.push_back((u8)(v >> (8*i))); }
u32  get_u32(const u8 *p)    { u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)p[i] << (8*i); return v; }

}  // namespace

Byte rans_compress(const u8 *d, size_t n) {
    Byte out;
    out.insert(out.end(), RANS_MAGIC, RANS_MAGIC + 4);
    for (int i = 0; i < 8; i++) out.push_back((u8)((u64)n >> (8*i)));

    u16 freq[256]; u32 cum[257]; u64 hist[256];
    Byte tmp;
    for (size_t off = 0; off < n; off += CHUNK) {
        const size_t len = std::min(CHUNK, n - off);
        const u8 *p = d + off;

        memset(hist, 0, sizeof hist);
        for (size_t i = 0; i < len; i++) hist[p[i]]++;
        normalize(hist, len, freq);
        cum[0] = 0;
        for (int s = 0; s < 256; s++) cum[s+1] = cum[s] + freq[s];

        // Encode in reverse so the decoder runs forward. Renormalize
        // BEFORE each symbol (the decoder renormalizes after), emitting
        // low byte first; the whole-buffer reversal below turns that into
        // the high-byte-first order the decoder's (x<<8)|byte rebuild
        // expects. This pairing is the rans_byte invariant.
        u32 x[4] = {RANS_L, RANS_L, RANS_L, RANS_L};
        tmp.clear();
        for (size_t i = len; i-- > 0; ) {
            const u8 s = p[i];
            u32 &st = x[i & 3];
            const u32 f = freq[s];
            const u32 x_max = ((RANS_L >> PROB_BITS) << 8) * f;
            while (st >= x_max) { tmp.push_back((u8)st); st >>= 8; }
            st = ((st / f) << PROB_BITS) + (st % f) + cum[s];
        }
        std::reverse(tmp.begin(), tmp.end());

        put_u32(out, (u32)len);
        put_u32(out, (u32)tmp.size());
        for (int s = 0; s < 256; s++) { out.push_back((u8)freq[s]); out.push_back((u8)(freq[s] >> 8)); }
        for (int j = 0; j < 4; j++) put_u32(out, x[j]);
        out.insert(out.end(), tmp.begin(), tmp.end());
    }
    return out;
}

Byte rans_decompress(const u8 *d, size_t n) {
    if (n < 12 || memcmp(d, RANS_MAGIC, 4)) throw Error("rans: broken container");
    u64 raw = 0; for (int i = 0; i < 8; i++) raw |= (u64)d[4+i] << (8*i);
    Byte out(raw);
    size_t pos = 12, done = 0;

    u16 freq[256]; u32 cum[257];
    // Slot -> symbol lookup: 4 KiB, rebuilt per chunk. One byte per slot
    // keeps it in L1; the per-symbol work is one slot read, one multiply.
    static thread_local u8 slot2sym[PROB_SCALE];

    while (done < raw) {
        if (pos + 8 + 512 + 16 > n) throw Error("rans: truncated header");
        const u32 len  = get_u32(d + pos);
        const u32 slen = get_u32(d + pos + 4);
        pos += 8;
        cum[0] = 0;
        for (int s = 0; s < 256; s++) {
            freq[s] = (u16)(d[pos] | (d[pos+1] << 8)); pos += 2;
            cum[s+1] = cum[s] + freq[s];
        }
        if (cum[256] != PROB_SCALE) throw Error("rans: bad frequency table");
        for (int s = 0; s < 256; s++)
            memset(slot2sym + cum[s], s, freq[s]);

        u32 x[4];
        for (int j = 0; j < 4; j++) { x[j] = get_u32(d + pos); pos += 4; }
        if (pos + slen > n || done + len > raw) throw Error("rans: truncated stream");
        const u8 *in = d + pos, *end = in + slen;

        u8 *o = out.data() + done;
        for (u32 i = 0; i < len; i++) {
            u32 &st = x[i & 3];
            const u32 slot = st & (PROB_SCALE - 1);
            const u8 s = slot2sym[slot];
            o[i] = s;
            st = freq[s] * (st >> PROB_BITS) + slot - cum[s];
            while (st < RANS_L) {
                if (in == end) throw Error("rans: stream underrun");
                st = (st << 8) | *in++;
            }
        }
        if (in != end) throw Error("rans: trailing stream bytes");
        pos += slen; done += len;
    }
    return out;
}

}  // namespace hz
