#include "nvz.hpp"
#include <cstdarg>
#include <cstring>
#include <algorithm>

namespace nvz {

std::string gemener(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string andelse_av(const std::string &sokvag) {
    size_t snedstreck = sokvag.find_last_of('/');
    size_t punkt = sokvag.find_last_of('.');
    if (punkt == std::string::npos) return "";
    if (snedstreck != std::string::npos && punkt < snedstreck) return "";
    if (punkt == 0 || (snedstreck != std::string::npos && punkt == snedstreck + 1))
        return "";                       // ".dold" är inte en ändelse
    return gemener(sokvag.substr(punkt));
}

std::string hex(const u8 *d, size_t n) {
    static const char *siffror = "0123456789abcdef";
    std::string ut(n * 2, '0');
    for (size_t i = 0; i < n; i++) {
        ut[i*2]   = siffror[d[i] >> 4];
        ut[i*2+1] = siffror[d[i] & 15];
    }
    return ut;
}

// --- blake2b-128 -----------------------------------------------------------
// Samma hash som pythonversionen använder (hashlib.blake2b, digest_size=16),
// för att arkiv från de två ska bära samma objektnycklar. Implementationen
// är lyft ur src/nvgpu.c, där den redan fanns som facit åt GPU-kärnan.
namespace {
const u64 IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };
const u8 SIGMA[12][16] = {
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3},
    {11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4},
    {7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8},
    {9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13},
    {2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9},
    {12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11},
    {13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10},
    {6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5},
    {10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0},
    {0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15},
    {14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3}};

inline u64 rotr(u64 x, int n) { return (x >> n) | (x << (64 - n)); }

void G(u64 *v, int a, int b, int c, int d, u64 x, u64 y) {
    v[a] = v[a] + v[b] + x; v[d] = rotr(v[d] ^ v[a], 32);
    v[c] = v[c] + v[d];     v[b] = rotr(v[b] ^ v[c], 24);
    v[a] = v[a] + v[b] + y; v[d] = rotr(v[d] ^ v[a], 16);
    v[c] = v[c] + v[d];     v[b] = rotr(v[b] ^ v[c], 63);
}

void komprimera(u64 h[8], const u8 blk[128], u64 t, bool sista) {
    u64 v[16], m[16];
    for (int i = 0; i < 8; i++) v[i] = h[i];
    for (int i = 0; i < 8; i++) v[8+i] = IV[i];
    v[12] ^= t; v[14] ^= sista ? ~0ULL : 0ULL;
    for (int i = 0; i < 16; i++) {
        u64 x = 0;
        for (int b = 7; b >= 0; b--) x = (x << 8) | blk[i*8 + b];
        m[i] = x;
    }
    for (int r = 0; r < 12; r++) {
        const u8 *s = SIGMA[r];
        G(v, 0, 4,  8, 12, m[s[0]],  m[s[1]]);
        G(v, 1, 5,  9, 13, m[s[2]],  m[s[3]]);
        G(v, 2, 6, 10, 14, m[s[4]],  m[s[5]]);
        G(v, 3, 7, 11, 15, m[s[6]],  m[s[7]]);
        G(v, 0, 5, 10, 15, m[s[8]],  m[s[9]]);
        G(v, 1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(v, 2, 7,  8, 13, m[s[12]], m[s[13]]);
        G(v, 3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
    for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[8+i];
}
}  // namespace

void blake2b128(const u8 *data, size_t n, u8 ut[16]) {
    u64 h[8];
    for (int i = 0; i < 8; i++) h[i] = IV[i];
    h[0] ^= 0x01010000ULL ^ 16ULL;          // ingen nyckel, 16 byte utdata
    u8 blk[128];
    size_t kvar = n, t = 0;
    const u8 *p = data;
    while (kvar > 128) {
        memcpy(blk, p, 128); t += 128;
        komprimera(h, blk, t, false);
        p += 128; kvar -= 128;
    }
    memset(blk, 0, 128);
    memcpy(blk, p, kvar); t += kvar;
    komprimera(h, blk, t, true);
    for (int i = 0; i < 2; i++)
        for (int b = 0; b < 8; b++) ut[i*8 + b] = (u8)(h[i] >> (8*b));
}

// --- filer -----------------------------------------------------------------
Byte las_fil(const std::string &sokvag) {
    FILE *f = fopen(sokvag.c_str(), "rb");
    if (!f) throw Fel(sokvag + ": går inte att öppna");
    Byte d;
    u8 buf[1 << 16];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + r);
    fclose(f);
    return d;
}

void skriv_fil(const std::string &sokvag, const u8 *d, size_t n) {
    FILE *f = fopen(sokvag.c_str(), "wb");
    if (!f) throw Fel(sokvag + ": går inte att skriva");
    if (n && fwrite(d, 1, n, f) != n) { fclose(f); throw Fel(sokvag + ": kort skrivning"); }
    fclose(f);
}

std::string strf(const char *fmt, ...) {
    va_list a, b;
    va_start(a, fmt); va_copy(b, a);
    int n = vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    std::string ut((size_t)(n < 0 ? 0 : n), '\0');
    if (n > 0) vsnprintf(ut.data(), (size_t)n + 1, fmt, b);
    va_end(b);
    return ut;
}

}  // namespace nvz
