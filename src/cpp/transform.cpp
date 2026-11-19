// SPDX-License-Identifier: GPL-2.0-only
// PNG and JPEG transforms, ported line by line from src/png.py and
// src/jpeg.py (the rationale lives there). One departure: nvjpg is linked
// in and run via a worker process (worker.cpp), not as the src/nvjpg binary.

#include "transform.hpp"
#include "worker.hpp"
#include <zlib.h>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <array>
#include <deque>

namespace hz {
namespace {

constexpr size_t STEP = 1 << 18;

// Recipes that have worked in this process, newest first. Images from the
// same tool in practice always share a recipe, so from image two onwards
// the first guess is right.
std::mutex recent_mutex;
std::deque<std::array<int,4>> recent;

u32 be32(const u8 *p) { return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }

struct Chunk { size_t i, len; char type[5]; };

// Returns false if the file is not a well-formed PNG (it is then stored raw).
bool chunks(const Byte &b, std::vector<Chunk> &out) {
    static const u8 SIGNATURE[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if (b.size() < 8 || memcmp(b.data(), SIGNATURE, 8)) return false;
    size_t i = 8, n = b.size();
    while (i + 8 <= n) {
        size_t len = be32(&b[i]);
        Chunk c; c.i = i; c.len = len;
        memcpy(c.type, &b[i+4], 4); c.type[4] = 0;
        if (i + 12 + len > n) return false;
        out.push_back(c);
        i += 12 + len;
        if (!memcmp(c.type, "IEND", 4)) break;
    }
    return i == n;            // trailing bytes — store raw instead
}

std::vector<std::array<int,4>> candidates(const Byte &stream) {
    if (stream.size() < 2) return {};
    u8 cmf = stream[0], flg = stream[1];
    if ((cmf & 0x0f) != 8 || (flg & 0x20)) return {};     // not deflate, or FDICT
    int wbits = (cmf >> 4) + 8;
    if (wbits < 9 || wbits > 15) return {};
    int flevel = flg >> 6;
    // zlib derives FLEVEL from the level, so the group is a statement, not
    // a guess. Within the group: 9 and 6 are what tools actually use.
    std::vector<int> group;
    switch (flevel) {
    case 0: group = {1}; break;
    case 1: group = {2,3,4,5}; break;
    case 2: group = {6}; break;
    default: group = {9,8,7}; break;
    }
    std::stable_sort(group.begin(), group.end(), [](int a, int b) {
        auto weight = [](int n) { return n == 9 ? 0 : n == 6 ? 1 : 10 - n; };
        return weight(a) < weight(b);
    });
    std::vector<std::array<int,4>> cand;
    for (int n : group)
        for (int m : {8, 9})
            for (int s : {0, 1, 2, 3, 4})
                cand.push_back(std::array<int,4>{n, wbits, m, s});
    std::lock_guard<std::mutex> l(recent_mutex);
    std::vector<std::array<int,4>> out;
    for (const auto &r : recent)
        if (std::find(cand.begin(), cand.end(), r) != cand.end()) out.push_back(r);
    for (const auto &r : cand)
        if (std::find(out.begin(), out.end(), r) == out.end()) out.push_back(r);
    return out;
}

// Recompresses the raw data and compares as it goes — aborts at the first
// deviation. Wrong parameters almost always deviate within the first
// hundred kilobytes.
bool try_recipe(const Byte &raw, const Byte &want, const std::array<int,4> &r) {
    z_stream z{};
    if (deflateInit2(&z, r[0], Z_DEFLATED, r[1], r[2], r[3]) != Z_OK) return false;
    Byte out;
    out.reserve(want.size() + 64);
    u8 buf[1 << 16];
    bool ok = true;
    size_t i = 0, n = raw.size();
    do {
        size_t piece = std::min(STEP, n - i);
        z.next_in = const_cast<u8*>(raw.data() + i);
        z.avail_in = (uInt)piece;
        i += piece;
        int mode = (i == n) ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = buf; z.avail_out = sizeof buf;
            int rc = deflate(&z, mode);
            if (rc == Z_STREAM_ERROR) { ok = false; break; }
            size_t got = sizeof buf - z.avail_out;
            if (out.size() + got > want.size() ||
                memcmp(want.data() + out.size(), buf, got)) { ok = false; break; }
            out.insert(out.end(), buf, buf + got);
            if (rc == Z_STREAM_END) break;
        } while (z.avail_out == 0 || z.avail_in > 0);
        if (!ok) break;
    } while (i < n);
    deflateEnd(&z);
    return ok && out.size() == want.size();
}

Byte redeflate(const Byte &raw, const std::array<int,4> &r) {
    z_stream z{};
    if (deflateInit2(&z, r[0], Z_DEFLATED, r[1], r[2], r[3]) != Z_OK)
        throw Error("zlib: deflateInit2 failed");
    Byte out(deflateBound(&z, raw.size()));
    z.next_in = const_cast<u8*>(raw.data()); z.avail_in = (uInt)raw.size();
    z.next_out = out.data(); z.avail_out = (uInt)out.size();
    int rc = deflate(&z, Z_FINISH);
    size_t n = out.size() - z.avail_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END) throw Error("zlib: incomplete deflate");
    out.resize(n);
    return out;
}

// Deflate engine fingerprint. Four zlib parameters do not determine the
// output stream — the implementation does: zlib and zlib-ng produce
// different bytes for the same parameters, and zlib-ng is the default libz
// on a growing number of distributions. Shown in Ted Lyngmo's code review
// of 2026-11-18: an archive packed against system zlib could not be
// unpacked against zlib-ng.
//
// The fingerprint is behavioral, not a version string: a fixed probe is
// deflated with fixed parameters and the crc32 of the output is the
// fingerprint. Two libraries that produce the same bytes are
// interchangeable regardless of name, which is the property that matters.
std::string engine_fingerprint() {
    static const std::string fp = [] {
        Byte probe;
        for (int i = 0; i < 4096; i++)                 // deterministic, not random
            probe.push_back((u8)((i * 37 + (i >> 3) * 11) & 0xff));
        for (const char *t = "nvz deflate probe "; *t; t++) probe.push_back((u8)*t);
        u32 c = 0;
        try {
            Byte out = redeflate(probe, {6, 15, 8, 0});
            c = (u32)crc32(0, out.data(), (uInt)out.size());
        } catch (...) { c = 0; }
        const char *v = zlibVersion();
        std::string clean;
        for (const char *q = v; *q && clean.size() < 12; q++)
            if (*q != ',' && *q != ';' && *q != ' ') clean.push_back(*q);
        return strf("z%s-%08x", clean.c_str(), c);
    }();
    return fp;
}

std::array<int,4> parse_recipe(const std::string &s) {
    std::array<int,4> r{};
    if (sscanf(s.c_str(), "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) != 4)
        throw Error("broken png recipe: " + s);
    return r;
}

}  // namespace

std::optional<Unfolded> png_unfold(const Byte &b, double inflation_cap) {
    std::vector<Chunk> all;
    if (!chunks(b, all)) return std::nullopt;
    std::vector<const Chunk*> idat;
    size_t idat_len = 0;
    for (const Chunk &c : all)
        if (!memcmp(c.type, "IDAT", 4)) { idat.push_back(&c); idat_len += c.len; }
    if (idat.empty() || idat_len < 512) return std::nullopt;   // too small to be worth it

    Byte stream;
    stream.reserve(idat_len);
    for (const Chunk *c : idat)
        stream.insert(stream.end(), b.begin() + c->i + 8, b.begin() + c->i + 8 + c->len);

    // zlib.decompress(stream): zlib header, unknown final size.
    Byte raw;
    {
        z_stream z{};
        if (inflateInit2(&z, 15) != Z_OK) return std::nullopt;
        raw.resize(std::max<size_t>(stream.size() * 4, 1 << 16));
        z.next_in = stream.data(); z.avail_in = (uInt)stream.size();
        size_t filled = 0;
        for (;;) {
            z.next_out = raw.data() + filled; z.avail_out = (uInt)(raw.size() - filled);
            int rc = inflate(&z, Z_NO_FLUSH);
            filled = raw.size() - z.avail_out;
            if (rc == Z_STREAM_END) break;
            if (rc != Z_OK && rc != Z_BUF_ERROR) { inflateEnd(&z); return std::nullopt; }
            if ((double)filled > (double)stream.size() * inflation_cap) {
                inflateEnd(&z); return std::nullopt;      // the guard rail, see nvz.py
            }
            if (z.avail_out == 0) raw.resize(raw.size() * 2);
            else if (rc == Z_BUF_ERROR) { inflateEnd(&z); return std::nullopt; }
        }
        inflateEnd(&z);
        raw.resize(filled);
    }

    std::optional<std::array<int,4>> recipe;
    for (const auto &k : candidates(stream))
        if (try_recipe(raw, stream, k)) { recipe = k; break; }
    if (!recipe) return std::nullopt;
    {
        std::lock_guard<std::mutex> l(recent_mutex);
        auto it = std::find(recent.begin(), recent.end(), *recipe);
        if (it != recent.end()) recent.erase(it);
        recent.push_front(*recipe);
        while (recent.size() > 4) recent.pop_back();
    }

    // The skeleton is the file without IDAT content; the checksums remain.
    Byte skeleton;
    size_t last = 0;
    for (const Chunk *c : idat) {
        skeleton.insert(skeleton.end(), b.begin() + last, b.begin() + c->i + 8);
        last = c->i + 8 + c->len;
    }
    skeleton.insert(skeleton.end(), b.begin() + last, b.end());

    Unfolded o;
    o.tag = "png";
    // The fifth field is the engine fingerprint. Older recipes have four
    // fields and parse as before, so the format is unchanged.
    o.recipe = strf("%d,%d,%d,%d,%s", (*recipe)[0], (*recipe)[1], (*recipe)[2],
                    (*recipe)[3], engine_fingerprint().c_str());
    o.rest = std::move(skeleton);
    o.body = std::move(raw);
    if (png_rebuild(o.rest, o.body, o.recipe) != b) return std::nullopt;  // should not happen
    return o;
}

namespace {

// Assembles the PNG from skeleton and stream, verifying every IDAT chunk
// against the skeleton's stored CRC32 before accepting it.
//
// The check is required: copying the skeleton's CRC bytes straight into the
// output, as the code once did, turns a stream of the right length but
// wrong bytes — exactly what a different deflate engine produces — into a
// PNG with wrong image data and a seemingly valid CRC. That case must be a
// refusal, not silent corruption.
std::optional<Byte> assemble(const Byte &skeleton, const Byte &stream) {
    Byte out(skeleton.begin(), skeleton.begin() + std::min<size_t>(8, skeleton.size()));
    size_t i = 8, j = 0, n = skeleton.size();
    while (i + 8 <= n) {
        size_t len = be32(&skeleton[i]);
        const u8 *type = &skeleton[i+4];
        if (i + 12 + (memcmp(type, "IDAT", 4) ? len : 0) > n) return std::nullopt;
        out.insert(out.end(), skeleton.begin() + i, skeleton.begin() + i + 8);
        if (!memcmp(type, "IDAT", 4)) {
            if (j + len > stream.size()) return std::nullopt;     // stream too short
            const u8 *data = stream.data() + j;
            u32 c = (u32)crc32(crc32(0, type, 4), data, (uInt)len);
            if (c != be32(&skeleton[i + 8])) return std::nullopt; // wrong bytes
            out.insert(out.end(), data, data + len);
            j += len;
            out.insert(out.end(), skeleton.begin() + i + 8, skeleton.begin() + i + 12);
            i += 12;
        } else {
            out.insert(out.end(), skeleton.begin() + i + 8, skeleton.begin() + i + 12 + len);
            i += 12 + len;
        }
        if (!memcmp(type, "IEND", 4)) break;
    }
    if (j != stream.size()) return std::nullopt;       // stream too long
    out.insert(out.end(), skeleton.begin() + i, skeleton.end());
    return out;
}

std::string recipe_engine(const std::string &s) {
    size_t k = 0;
    for (int f = 0; f < 4; f++) {
        k = s.find(',', k);
        if (k == std::string::npos) return "";
        k++;
    }
    return s.substr(k);
}

}  // namespace

Byte png_rebuild(const Byte &skeleton, const Byte &raw, const std::string &recipe) {
    const std::array<int,4> r = parse_recipe(recipe);
    if (auto out = assemble(skeleton, redeflate(raw, r))) return *out;

    // The recipe did not reproduce the stream, so this machine's deflate
    // engine is not the one that packed the archive. The recipe parameters
    // are a property of the file, not the engine, so another combination
    // may give the same bytes here; the search is cheap next to an archive
    // that cannot be unpacked and runs only on the failure path. The
    // window size is not varied: the PNG's own zlib header states it, and
    // it is the same on every engine.
    for (int level = 9; level >= 1; level--)
        for (int mem : {r[2], 9, 8, 7, 6, 5, 4, 3, 2, 1})
            for (int strat : {r[3], 0, 1, 2, 3, 4}) {
                if (level == r[0] && mem == r[2] && strat == r[3]) continue;
                std::array<int,4> k{level, r[1], mem, strat};
                try {
                    if (auto out = assemble(skeleton, redeflate(raw, k))) return *out;
                } catch (const std::exception &) { /* invalid combination */ }
            }

    const std::string packed_by = recipe_engine(recipe);
    throw Error("png: this machine's deflate engine (" + engine_fingerprint() +
                ") cannot reproduce the image stream" +
                (packed_by.empty() ? std::string(" and the archive does not say which one packed it")
                                   : " — the archive was packed with " + packed_by) +
                ". Unpack on a machine with the same engine, or repack the "
                "archive with --no-transform. No incorrect data has been "
                "written: every IDAT chunk is checked against its CRC.");
}

std::optional<Unfolded> jpeg_unfold(const Byte &b) {
    int exit_code = 0;
    Byte d = run_self({"nvjpg", "pack"}, b.data(), b.size(), &exit_code);
    if (exit_code != 0 || d.size() < 18 || memcmp(d.data(), "NVJ1", 4))
        return std::nullopt;             // progressive, broken, or not recodable
    u32 head_len, tail_len;
    memcpy(&head_len, &d[4], 4); memcpy(&tail_len, &d[8], 4);
    size_t split = 18 + (size_t)head_len + tail_len;
    if (split > d.size() || d.size() >= b.size()) return std::nullopt;  // no gain
    Unfolded o;
    o.tag = "jpeg";
    o.rest.assign(d.begin(), d.begin() + split);
    o.body.assign(d.begin() + split, d.end());
    if (jpeg_rebuild(o.rest, o.body) != b) return std::nullopt;         // should not happen
    return o;
}

Byte jpeg_rebuild(const Byte &rest, const Byte &body) {
    Byte joined(rest);
    joined.insert(joined.end(), body.begin(), body.end());
    int exit_code = 0;
    Byte out = run_self({"nvjpg", "unpack"}, joined.data(), joined.size(), &exit_code);
    if (exit_code != 0) throw Error("nvjpg unpack failed");
    return out;
}

}  // namespace hz
