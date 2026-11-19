// SPDX-License-Identifier: GPL-2.0-only
// Shared helpers: string utilities, blake2b-128, checked file I/O.
#include "nvz.hpp"
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdarg>
#include <algorithm>

namespace nvz {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string extension_of(const std::string &path) {
    size_t slash = path.find_last_of('/');
    size_t dot = path.find_last_of('.');
    if (dot == std::string::npos) return "";
    if (slash != std::string::npos && dot < slash) return "";
    if (dot == 0 || (slash != std::string::npos && dot == slash + 1))
        return "";                       // ".hidden" is not an extension
    return lower(path.substr(dot));
}

std::string hex(const u8 *d, size_t n) {
    static const char *digits = "0123456789abcdef";
    std::string out(n * 2, '0');
    for (size_t i = 0; i < n; i++) {
        out[i*2]   = digits[d[i] >> 4];
        out[i*2+1] = digits[d[i] & 15];
    }
    return out;
}

std::string temp_dir() {
    const char *t = getenv("TMPDIR");
    return (t && t[0]) ? t : "/tmp";
}

// --- blake2b-128 ------------------------------------------------------------
// The same hash the Python implementation used (hashlib.blake2b,
// digest_size=16), so archives from both carry the same object keys.
// Lifted from src/nvgpu.c, where it already serves as the oracle for the
// GPU kernel.
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

void b2b_round(u64 h[8], const u8 blk[128], u64 t, bool last) {
    u64 v[16], m[16];
    for (int i = 0; i < 8; i++) v[i] = h[i];
    for (int i = 0; i < 8; i++) v[8+i] = IV[i];
    v[12] ^= t; v[14] ^= last ? ~0ULL : 0ULL;
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

void blake2b128(const u8 *data, size_t n, u8 out[16]) {
    u64 h[8];
    for (int i = 0; i < 8; i++) h[i] = IV[i];
    h[0] ^= 0x01010000ULL ^ 16ULL;          // no key, 16 bytes of output
    u8 blk[128];
    size_t left = n, t = 0;
    const u8 *p = data;
    while (left > 128) {
        memcpy(blk, p, 128); t += 128;
        b2b_round(h, blk, t, false);
        p += 128; left -= 128;
    }
    memset(blk, 0, 128);
    memcpy(blk, p, left); t += left;
    b2b_round(h, blk, t, true);
    for (int i = 0; i < 2; i++)
        for (int b = 0; b < 8; b++) out[i*8 + b] = (u8)(h[i] >> (8*b));
}

// --- files ------------------------------------------------------------------
Byte read_file(const std::string &path) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) throw Error(path + ": cannot open");
    // One allocation, one read. Reading in 64 kB chunks into a growing
    // vector re-copies roughly the whole file once — measurable in a
    // packer whose fast mode is dominated by these sweeps. The size from
    // fstat is a hint, not a guarantee (the file may grow between stat and
    // read), so any tail beyond st_size is read chunkwise below.
    struct stat st;
    Byte d;
    if (fstat(fileno(f), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        d.resize((size_t)st.st_size);
        size_t got = fread(d.data(), 1, d.size(), f);
        d.resize(got);
    }
    u8 buf[1 << 16];
    size_t r;
    while ((r = fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + r);
    fclose(f);
    return d;
}

// Every write must be checked, for a reason specific to a packer with a
// gzip surface: the original is deleted once the pack has succeeded. If a
// write fails silently — full disk, exceeded quota, a file-size limit —
// and nvz still reports success, the source is deleted, the archive is
// truncated and the data is gone.
//
// Found in Ted Lyngmo's code review of 2026-11-18, reproduced under
// RLIMIT_FSIZE: truncated archive, exit status 0, original deleted, and
// `nvz -d` afterwards said the register pointed outside the archive. The
// reference behaviour: `gzip -c file > /dev/full` returns 1; nvz
// returned 0.
//
// Three traps these helpers cover. (i) fwrite buffers, so ENOSPC often
// surfaces only when the buffer is flushed — hence fflush before fclose,
// with the error read before the stream is closed. (ii) fclose frees the
// stream even when it fails, so an error only fclose sees would otherwise
// be lost. (iii) fsync: a packer that deletes its source needs the bytes
// on disk, not merely accepted by the kernel. EINVAL is tolerated for
// targets that cannot be synced (pipes, /dev/null).
void write_checked(FILE *f, const void *d, size_t n, const std::string &what) {
    if (n && fwrite(d, 1, n, f) != n)
        throw Error(what + ": write failed (" + std::strerror(errno) + ")");
}

void close_checked(FILE *f, const std::string &what, bool sync) {
    if (fflush(f) != 0) {
        int e = errno; fclose(f);
        throw Error(what + ": could not flush (" + std::strerror(e) + ")");
    }
    if (sync && fsync(fileno(f)) != 0 && errno != EINVAL && errno != ENOTTY) {
        int e = errno; fclose(f);
        throw Error(what + ": could not sync to disk (" + std::strerror(e) + ")");
    }
    if (fclose(f) != 0)
        throw Error(what + ": could not close (" + std::strerror(errno) + ")");
}

void write_file(const std::string &path, const u8 *d, size_t n) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) throw Error(path + ": cannot write");
    try { write_checked(f, d, n, path); }
    catch (...) { fclose(f); throw; }
    close_checked(f, path, true);
}

std::string strf(const char *fmt, ...) {
    va_list a, b;
    va_start(a, fmt); va_copy(b, a);
    int n = vsnprintf(nullptr, 0, fmt, a);
    va_end(a);
    std::string out((size_t)(n < 0 ? 0 : n), '\0');
    if (n > 0) vsnprintf(out.data(), (size_t)n + 1, fmt, b);
    va_end(b);
    return out;
}

}  // namespace nvz
