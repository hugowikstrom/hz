// SPDX-License-Identifier: GPL-2.0-only
//
// nvz archive container: pack, unpack, verify, list, and the register
// readers/writers (v1 JSON, binary v2/v3).
//
// Layout:
//
//   "NVPACK\x01" | block 0 | block 1 | ... | register | <u64 offset, u64 raw len>
//
// The container matches the Python implementation on purpose: an archive it
// wrote must unpack with this binary. Only the register differs — Python
// writes JSON, this writes binary v2/v3. The reader keys on the first byte:
// '{' is v1, "NVZREG2" is v2 and v3.
//
// Format-frozen strings (see the language note in nvz.hpp): block class
// names written into registers are the original Swedish words and must stay
// byte-identical forever — "text", "binar" (binary), "opak" (opaque),
// "png", "jpeg", "png-ra" (unfolded PNG filter rows), "png-rest" (PNG
// skeleton), "jpeg-kropp" (JPEG body), "jpeg-rest" (JPEG header), and
// "blandat" (mixed, from --no-classing). Codec names likewise. The probe
// sentinels ("probe", "probe-cm", "probe-gpu", "opak-eskalera") never reach
// disk and are free to change.
#include "archive.hpp"
#include <set>
#include "codec.hpp"
#include "tree.hpp"
#include "json.hpp"
#include "transform.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <ctime>
#include <functional>
#include <array>
#include <chrono>
#include <cerrno>

namespace hz {
namespace {

const u8 MAGIC[7] = {'N','V','P','A','C','K',1};
const char REG2[8] = {'N','V','Z','R','E','G','2','\0'};

// Codec per class and mode. "probe": the block decides by compressing a
// sample with each candidate; "probe-cm" adds nvcm as a candidate. The
// table is the Python implementation's MODES table, verbatim.
std::map<std::string, std::string> mode_codecs(const std::string &mode) {
    if (mode == "fast")
        return {{"text","zstd3"},{"binar","zstd3"},{"png-ra","zstd3"},{"png-rest","zstd3"},
                {"jpeg-rest","zstd3"},{"jpeg-kropp","ra"},{"jpeg","ra"},{"opak","ra"},
                {"png","zstd3"}};
    if (mode == "normal")
        return {{"text","probe"},{"binar","probe"},{"png-ra","probe"},{"png-rest","zstd19"},
                {"jpeg-rest","zstd19"},{"jpeg-kropp","ra"},{"jpeg","probe"},{"opak","probe"},
                {"png","probe"}};
    if (mode == "gpu")
        // Like normal, but nvlz competes for every block. Measurement
        // instrument only: on this machine it never wins a block (see
        // README).
        return {{"text","probe-gpu"},{"binar","probe-gpu"},{"png-ra","probe-gpu"},
                {"png-rest","probe-gpu"},{"jpeg-rest","probe-gpu"},{"jpeg-kropp","ra"},
                {"jpeg","probe"},{"opak","probe"},{"png","probe-gpu"}};
    // small, the default mode: nvcm wherever it wins the probe. Unfolded
    // image data (png-ra) is too voluminous for nvcm; that class probes
    // zstd/xz.
    return {{"text","probe-cm"},{"binar","probe-cm"},{"png-ra","probe"},{"png-rest","probe-cm"},
            {"jpeg-rest","probe-cm"},{"jpeg-kropp","ra"},{"jpeg","probe"},{"opak","probe"},
            {"png","probe"}};
}

// Read/write helpers for the binary register.
struct Out {
    Byte d;
    void u8v(u8 v) { d.push_back(v); }
    void u32v(u32 v) { for (int i = 0; i < 4; i++) d.push_back((u8)(v >> (8*i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; i++) d.push_back((u8)(v >> (8*i))); }
    void i64v(i64 v) { u64v((u64)v); }
    void strv(const std::string &s) {
        u32v((u32)s.size());
        d.insert(d.end(), s.begin(), s.end());
    }
    void rawv(const u8 *p, size_t n) { d.insert(d.end(), p, p + n); }
};

struct In {
    const u8 *p, *end;
    void need(size_t n) { if ((size_t)(end - p) < n) throw Error("the register is truncated"); }
    u8 u8v() { need(1); return *p++; }
    u32 u32v() { need(4); u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)p[i] << (8*i); p += 4; return v; }
    u64 u64v() { need(8); u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)p[i] << (8*i); p += 8; return v; }
    i64 i64v() { return (i64)u64v(); }
    std::string strv() { u32 n = u32v(); need(n); std::string s((const char*)p, n); p += n; return s; }
    void rawv(u8 *out, size_t n) { need(n); memcpy(out, p, n); p += n; }
};

Byte write_register(const Register &r) {
    Out u;
    u.rawv((const u8*)REG2, 8);
    u.u32v(r.version);
    u.strv(r.root);
    u.u8v(r.flat ? 1 : 0);
    // v3 stores epoch seconds instead of an ISO string (20 bytes -> 8).
    if (r.version >= 3) u.i64v(r.created_epoch);
    else                u.strv(r.created);
    u.u32v((u32)r.blocks.size());
    for (const Block &b : r.blocks) {
        u.u32v(b.index); u.strv(b.cls); u.strv(b.codec);
        u.u64v(b.off); u.u64v(b.clen); u.u64v(b.raw_len);
    }
    u.u32v((u32)r.objects.size());
    // v3 shrank the hash from 16 bytes to 4. Its only job is `nvz -t`,
    // which recomputes it from the unpacked file to catch a mis-assembled
    // archive; that does not need 128 bits. Bit errors in a compressed
    // stream are already caught by the blocks' own checksums (CRC64 in xz,
    // xxhash in zstd); what remains is nvz's own assembly faults — swapped
    // objects, wrong offsets — where any deviation mismatches with
    // probability 1 - 2^-32.
    //
    // The twelve extra bytes were expensive because hashes are random and
    // therefore incompressible: on the code corpus (5 898 objects) they
    // were 94 368 bytes of a 239 049-byte register — 1.1 % of the whole
    // archive, more than the entire distance up to `xz -9e`.
    const size_t hl = r.version >= 3 ? 4 : 16;
    for (const Object &o : r.objects) { u.rawv(o.hash, hl); u.u64v(o.size); }
    for (const Object &o : r.objects) { u.u32v(o.blk); u.u64v(o.off); u.u64v(o.len); }
    // Transformed objects are few; store them separately rather than as
    // empty fields on every row.
    u32 ntr = 0;
    for (const Object &o : r.objects) if (!o.transform.empty()) ntr++;
    u.u32v(ntr);
    for (u32 i = 0; i < r.objects.size(); i++) {
        const Object &o = r.objects[i];
        if (o.transform.empty()) continue;
        u.u32v(i); u.strv(o.transform); u.strv(o.recipe);
        u.u32v(o.rblk); u.u64v(o.roff); u.u64v(o.rlen);
    }
    u.u32v((u32)r.entries.size());
    for (const Entry &p : r.entries) {
        u.strv(p.path); u.u8v((u8)p.kind);
        u.u32v(p.mode); u.i64v(p.mtime_ns);
        if (p.kind == Kind::Link) u.strv(p.target);
        if (p.kind == Kind::File) u.i64v(p.obj);
    }
    return u.d;
}

Register read_register_v2(const u8 *d, size_t n) {
    In in{d + 8, d + n};
    Register r;
    r.version = in.u32v();
    r.root = in.strv();
    r.flat = in.u8v() != 0;
    if (r.version >= 3) {
        r.created_epoch = in.i64v();
        char buf[32] = "";
        time_t t = (time_t)r.created_epoch;
        struct tm tm{};
        // localtime, not gmtime: v2 wrote local time as a string, and
        // `nvz list` must show the same thing for both formats.
        if (localtime_r(&t, &tm)) strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &tm);
        r.created = buf;
    } else {
        r.created = in.strv();
    }
    u32 nb = in.u32v();
    r.blocks.resize(nb);
    for (Block &b : r.blocks) {
        b.index = in.u32v(); b.cls = in.strv(); b.codec = in.strv();
        b.off = in.u64v(); b.clen = in.u64v(); b.raw_len = in.u64v();
    }
    u32 no = in.u32v();
    r.objects.resize(no);
    r.hash_len = r.version >= 3 ? 4 : 16;
    for (Object &o : r.objects) { in.rawv(o.hash, r.hash_len); o.size = in.u64v(); }
    for (Object &o : r.objects) { o.blk = in.u32v(); o.off = in.u64v(); o.len = in.u64v(); }
    u32 ntr = in.u32v();
    for (u32 i = 0; i < ntr; i++) {
        u32 id = in.u32v();
        if (id >= r.objects.size()) throw Error("the register points outside the object list");
        Object &o = r.objects[id];
        o.transform = in.strv(); o.recipe = in.strv();
        o.rblk = in.u32v(); o.roff = in.u64v(); o.rlen = in.u64v();
    }
    u32 np = in.u32v();
    r.entries.resize(np);
    for (Entry &p : r.entries) {
        p.path = in.strv();
        p.kind = (Kind)in.u8v();
        p.mode = in.u32v(); p.mtime_ns = in.i64v();
        if (p.kind == Kind::Link) p.target = in.strv();
        if (p.kind == Kind::File) p.obj = in.i64v();
    }
    return r;
}

// v1: the Python implementation's JSON register. Block positions are not
// stored in v1 but derived from the lengths, in exactly the order the
// objects were appended — same derivation as in src/hz.py. The JSON keys
// are format constants; do not translate.
Register read_register_v1(const std::string &text) {
    Jptr j = parse_json(text);
    Register r;
    r.hash_len = 8;
    r.version = (u32)j->at("version")->as_num();
    r.root = j->at("rot")->as_str();
    r.flat = j->at("platt")->b;
    r.created = j->at("skapad")->as_str();
    for (const Jptr &b : j->at("block")->list) {
        Block bl;
        bl.index = (u32)b->at("i")->as_num();
        bl.cls = b->at("klass")->as_str();
        bl.codec = b->at("kodek")->as_str();
        bl.off = (u64)b->at("off")->as_num();
        bl.clen = (u64)b->at("clen")->as_num();
        bl.raw_len = (u64)b->at("rålen")->as_num();
        r.blocks.push_back(bl);
    }
    const Jptr &o = j->at("objekt");
    const auto &blk = o->at("blk")->list;
    const auto &len = o->at("len")->list;
    const auto &sl  = o->at("s")->list;
    const auto &hl  = o->at("h")->list;
    const Jptr &tr  = o->at("tr");
    r.objects.resize(blk.size());
    std::map<u32, u64> running;
    for (size_t i = 0; i < blk.size(); i++) {
        Object &ob = r.objects[i];
        ob.id = (u32)i;
        ob.blk = (u32)blk[i]->as_num();
        ob.len = (u64)len[i]->as_num();
        ob.size = (u64)sl[i]->as_num();
        const std::string &hx = hl[i]->as_str();
        for (size_t k = 0; k + 1 < hx.size() && k / 2 < 16; k += 2)
            ob.hash[k/2] = (u8)strtoul(hx.substr(k, 2).c_str(), nullptr, 16);
        auto it = tr->obj.find(std::to_string(i));
        if (it != tr->obj.end() && it->second->list.size() >= 3) {
            const auto &t = it->second->list;
            ob.transform = t[0]->as_str();
            ob.rblk = (u32)t[1]->as_num();
            ob.rlen = (u64)t[2]->as_num();
            if (t.size() > 3 && t[3]->k == Jvalue::K::List && t[3]->list.size() == 4)
                ob.recipe = strf("%lld,%lld,%lld,%lld",
                                 (long long)t[3]->list[0]->as_num(),
                                 (long long)t[3]->list[1]->as_num(),
                                 (long long)t[3]->list[2]->as_num(),
                                 (long long)t[3]->list[3]->as_num());
            else if (t.size() > 3 && t[3]->k == Jvalue::K::Str)
                ob.recipe = t[3]->as_str();
            ob.roff = running[ob.rblk];
            running[ob.rblk] = ob.roff + ob.rlen;
        }
        ob.off = running[ob.blk];
        running[ob.blk] = ob.off + ob.len;
    }
    for (const Jptr &p : j->at("poster")->list) {
        Entry e;
        e.path = p->at("p")->as_str();
        e.kind = (Kind)p->at("t")->as_str()[0];
        e.mode = p->has("m") ? (u32)p->at("m")->as_num() : 0644;
        e.mtime_ns = p->at("mt")->as_num();
        if (p->has("mal")) e.target = p->at("mal")->as_str();
        e.obj = p->has("o") ? p->at("o")->as_num() : -1;
        r.entries.push_back(e);
    }
    return r;
}

// Run n tasks on up to the given number of threads. The first exception
// wins and stops the rest at the next task boundary.
void parallel_for(size_t n, int threads, const std::function<void(size_t)> &fn) {
    if (n == 0) return;
    int t = std::max(1, std::min<int>(threads, (int)n));
    std::atomic<size_t> next{0};
    std::vector<std::thread> tr;
    std::mutex err_mutex;
    std::string first_err;
    for (int i = 0; i < t; i++) {
        tr.emplace_back([&] {
            for (;;) {
                size_t k = next.fetch_add(1);
                if (k >= n) return;
                try { fn(k); }
                catch (const std::exception &e) {
                    std::lock_guard<std::mutex> l(err_mutex);
                    if (first_err.empty()) first_err = e.what();
                    next.store(n);
                    return;
                }
            }
        });
    }
    for (auto &x : tr) x.join();
    if (!first_err.empty()) throw Error(first_err);
}

}  // namespace

// --codec accepts the hz-era tool names as input; the register keeps the
// frozen format strings (hz.hpp), so hzcm maps to "nvcm" and so on. The
// frozen name is also still accepted.

// The register stores the frozen nv-era codec strings; the listing shows
// today's tool names. Data unchanged, display honest about what runs.
std::string display_codec(const std::string &c) {
    if (c == "nvcm")   return "hzcm";
    if (c == "nvcm-p") return "hzcm-p";
    if (c == "nvlz")   return "hzlz";
    if (c == "nvz")    return "hzcm";      // pre-rename register spelling
    return c;
}

std::string canonical_codec(const std::string &c) {
    if (c == "hzcm")   return "nvcm";
    if (c == "hzcm-p") return "nvcm-p";
    if (c == "hzlz")   return "nvlz";
    return c;
}

std::string canonical_mode(const std::string &mode) {
    if (mode == "fast"   || mode == "snabb") return "fast";
    if (mode == "normal")                    return "normal";
    if (mode == "small"  || mode == "liten") return "small";
    if (mode == "gpu")                       return "gpu";
    return "";
}

u64 file_size(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) throw Error(p + ": does not exist");
    return (u64)st.st_size;
}

Register read_register(const std::string &archive) {
    u64 n = file_size(archive);
    if (n < sizeof MAGIC + 16) throw Error("Not an hz archive.");
    FILE *f = fopen(archive.c_str(), "rb");
    if (!f) throw Error(archive + ": cannot open");
    u8 magic[sizeof MAGIC];
    if (fread(magic, 1, sizeof magic, f) != sizeof magic || memcmp(magic, MAGIC, sizeof MAGIC)) {
        fclose(f); throw Error("Not an hz archive.");
    }
    u8 tail[16];
    if (fseeko(f, -16, SEEK_END) != 0 || fread(tail, 1, 16, f) != 16) {
        fclose(f); throw Error("the archive is truncated");
    }
    u64 reg_off = 0, raw_len = 0;
    for (int i = 0; i < 8; i++) reg_off |= (u64)tail[i] << (8*i);
    for (int i = 0; i < 8; i++) raw_len |= (u64)tail[8+i] << (8*i);
    if (reg_off + 16 > n) { fclose(f); throw Error("the register points outside the archive"); }
    Byte packed(n - 16 - reg_off);
    if (fseeko(f, (off_t)reg_off, SEEK_SET) != 0 ||
        fread(packed.data(), 1, packed.size(), f) != packed.size()) {
        fclose(f); throw Error("could not read the register");
    }
    fclose(f);
    // The register's compressor is identified by the stream's own magic,
    // not by the version field — that sits inside the register and is
    // unreadable until the register is unpacked. xz starts with
    // FD 37 7A 58 5A 00.
    const bool reg_xz = packed.size() >= 6 && packed[0] == 0xFD && packed[1] == 0x37 &&
                        packed[2] == 0x7A && packed[3] == 0x58 && packed[4] == 0x5A &&
                        packed[5] == 0x00;
    Byte raw = decompress(reg_xz ? "xz9e" : "zstd19", packed.data(), packed.size(), raw_len);
    if (raw.size() >= 8 && !memcmp(raw.data(), REG2, 8)) return read_register_v2(raw.data(), raw.size());
    if (!raw.empty() && raw[0] == '{')
        return read_register_v1(std::string((const char*)raw.data(), raw.size()));
    throw Error("unknown register format");
}

// ----------------------------------------------------------------------------
// Packing
// ----------------------------------------------------------------------------
u64 pack(const std::string &root_in, const std::string &archive, const PackOpts &o) {
    auto t0 = std::chrono::steady_clock::now();
    int jobs = o.jobs > 0 ? o.jobs : std::min(24u, std::max(1u, std::thread::hardware_concurrency()));
    auto codecs = mode_codecs(o.mode);
    char real[4096];
    if (!realpath(root_in.c_str(), real)) throw Error(root_in + ": does not exist");
    std::string root = real;
    auto say = [&](const std::string &s) { if (!o.quiet) { printf("%s\n", s.c_str()); fflush(stdout); } };

    size_t skipped = 0;
    std::vector<Entry> entries = scan_tree(root, &skipped);
    if (skipped)
        fprintf(stderr, "  warning: %zu special files (pipes/sockets/devices) were skipped\n", skipped);

    std::vector<size_t> file_index;
    u64 raw_bytes = 0;
    for (size_t i = 0; i < entries.size(); i++)
        if (entries[i].kind == Kind::File) { file_index.push_back(i); raw_bytes += entries[i].size; }
    say(strf("  %zu entries, %.1f MiB, mode %s%s", entries.size(),
             raw_bytes / 1048576.0, o.mode.c_str(), o.portable ? " (portable)" : ""));

    // --- 1. hash and classify -------------------------------------------------
    std::vector<std::string> classes(file_index.size());
    std::vector<std::array<u8,16>> hashes(file_index.size());
    parallel_for(file_index.size(), jobs, [&](size_t k) {
        const Entry &p = entries[file_index[k]];
        Byte d = read_file(root + "/" + p.path);
        blake2b128(d.data(), d.size(), hashes[k].data());
        classes[k] = classify(p.path, d.data(), d.size());
    });

    // --- 2. dedup -------------------------------------------------------------
    std::vector<Object> objects;
    std::map<std::string, u32> seen;
    for (size_t k = 0; k < file_index.size(); k++) {
        Entry &p = entries[file_index[k]];
        std::string key = hex(hashes[k].data(), 16);
        if (!o.dedup) key += ":" + p.path;
        auto it = seen.find(key);
        if (it == seen.end()) {
            Object ob;
            memcpy(ob.hash, hashes[k].data(), 16);
            ob.size = p.size; ob.cls = classes[k];
            ob.ext = extension_of(p.path);
            ob.source = p.path;
            ob.id = (u32)objects.size();
            seen[key] = ob.id;
            objects.push_back(ob);
            p.obj = (i64)ob.id;
        } else {
            p.obj = (i64)it->second;
        }
    }
    u64 unique = 0;
    for (const Object &ob : objects) unique += ob.size;
    if (o.dedup && raw_bytes > unique)
        say(strf("  dedup: %zu duplicates, %.1f MiB",
                 file_index.size() - objects.size(), (raw_bytes - unique) / 1048576.0));

    // --- 3. sort --------------------------------------------------------------
    // Class first, then extension, then path: similar files land next to
    // each other in the stream.
    std::vector<u32> order(objects.size());
    for (u32 i = 0; i < order.size(); i++) order[i] = i;
    if (o.sort)
        std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
            const Object &x = objects[a], &y = objects[b];
            if (x.cls != y.cls) return x.cls < y.cls;
            if (x.ext != y.ext) return x.ext < y.ext;
            // Large files first within the group, the rest in path order.
            //
            // A large file builds the dictionary the smaller, similar
            // files then match into — a known heuristic in solid archives.
            // Measured on zlib-ng/corpora: neuro is two volumes of the same
            // scan, 38 and 19 MiB, and path order (19 before 38) cost 2.0 %.
            //
            // The rule must not apply to all files: on the whole group it
            // cost 2.99 % on the code corpus, because a real project tree
            // already has its similarity gathered per directory — `.py`
            // sits with `.py` in the same package — and size order tears
            // that locality apart. The threshold limits the rule to files
            // with a mechanism: only a file large enough to fill a
            // noticeable share of the block's window can serve as the base
            // for the ones that follow. Hence an eighth of the block cap
            // rather than a fixed number — 24 MiB with the default 192 MiB
            // cap.
            //
            // The boundary is measured. At 4 MiB the gif corpus's largest
            // file (7.4 MiB) qualified, moved first, changed the codec
            // probe's sample and cost 0.95 % on that corpus. At an eighth
            // of the cap it falls outside, while neuro's 36 MiB volume —
            // the case the rule exists for — stays inside.
            const u64 DICT_BUILDER = (o.block_cap ? o.block_cap : BLOCK_CAP) / 8;
            const bool xl = x.size >= DICT_BUILDER, yl = y.size >= DICT_BUILDER;
            if (xl != yl) return xl;                       // large before small
            if (xl && x.size != y.size) return x.size > y.size;  // largest first among the large
            return x.source < y.source;
        });
    std::vector<u32> renum(objects.size());
    for (u32 i = 0; i < order.size(); i++) renum[order[i]] = i;
    std::vector<Object> sorted(objects.size());
    for (u32 i = 0; i < order.size(); i++) {
        sorted[i] = objects[order[i]];
        sorted[i].id = i;
    }
    objects.swap(sorted);
    for (Entry &p : entries) if (p.kind == Kind::File && p.obj >= 0) p.obj = renum[p.obj];

    // --- 4. transform and place into blocks -----------------------------------
    // Transforms run in rounds of `jobs` so the sort order survives and
    // memory stays bounded: an unfolded PNG can be a hundred times its file
    // size, and 24 of those in flight fills the machine.
    struct Building { Block b; Byte raw; };
    std::vector<Building> blocks;
    std::map<std::string, size_t> open;
    // Opaque blocks are not split at 192 MiB. The cap exists for
    // parallelism, but on opaque data a split is a compression limit:
    // whatever similarity exists sits between files far apart, and two
    // independent blocks cannot match each other by construction (report
    // section 4.2: the whole gain lives in the window). zstd is
    // multithreaded inside the block, so the parallelism survives anyway.
    const u64 cap = o.block_cap ? o.block_cap : BLOCK_CAP;
    // Unfolded image data is split finer in the fast modes. png-ra is PNG
    // filter rows, special in three ways: it is the only class that
    // inflates (a 431 MiB tree becomes 2 GiB of blocks), its similarity
    // sits between neighbouring images rather than hundreds of megabytes
    // away, and it is therefore the class where splitting costs the least
    // ratio per second saved. Measured on the PNG corpus:
    //
    //     192 MiB blocks   261 s   218 956 236 bytes
    //      64 MiB blocks   201 s   221 495 996   (1.30x faster, +1.16 %)
    //      32 MiB blocks   137 s   231 569 236   (1.90x faster, +5.76 %)
    //
    // The best established tool on that corpus is zstd -22 at 257 132 369
    // bytes in 156 s: at 32 MiB nvz is still 9.9 % smaller and faster,
    // which is what `normal` exists for; `small` keeps the full cap and
    // takes the last 5.8 %. The split also keeps memory down: a 32 MiB
    // block gets a 64 MiB dictionary instead of 256, and several encoders
    // then fit at once instead of the memory gate serializing them.
    const bool fast_modes = (o.mode != "small");
    auto cap_for = [&](const std::string &cls) {
        if (cls == "opak") return (u64)16 << 30;
        if (cls == "png-ra" && fast_modes && !o.block_cap) return (u64)32 << 20;
        return cap;
    };
    auto place = [&](const std::string &cls_in, const u8 *d, size_t n) {
        // With --no-classing everything lands in one block stream. Objects
        // address their content with (block, offset, length), and unpacking
        // reads only the block's codec and the object's transform tag —
        // never the class — so the merge is invisible there; only the
        // compressor's window notices. Verified bit-exact with PNG- and
        // JPEG-transformed objects in the same block.
        //
        // Side effect when the flag is used for measurement: the merged
        // block is named "blandat" (format string), so the two-step rule
        // for opaque blocks (which keys on the class name below) never
        // fires. Intentional — the ablation measures nvz without tricks —
        // but it makes the row unsuitable as a reference for opaque data.
        const std::string cls = o.class_blocks ? cls_in : std::string("blandat");
        auto it = open.find(cls);
        if (it == open.end() || blocks[it->second].raw.size() >= cap_for(cls)) {
            Building nb;
            nb.b.index = (u32)blocks.size();
            nb.b.cls = cls;
            nb.b.codec = codecs.count(cls) ? codecs[cls] : "probe";
            // Reserve the block's cap up front. Otherwise the vector grows
            // in steps and each step re-copies everything already there —
            // for a block headed for 192 MiB roughly one extra full copy,
            // plus a doubled memory peak during the reallocation. The
            // reservation is virtual memory until the pages are touched, so
            // a block that stays small costs nothing. Opaque blocks
            // (16 GiB cap) get a more careful advance.
            nb.raw.reserve((size_t)std::min<u64>(cap_for(cls), 256u << 20));
            blocks.push_back(std::move(nb));
            open[cls] = blocks.size() - 1;
            it = open.find(cls);
        }
        Building &b = blocks[it->second];
        u64 off = b.raw.size();
        b.raw.insert(b.raw.end(), d, d + n);
        return std::make_tuple(b.b.index, off, (u64)n);
    };

    int png_done = 0, png_all = 0, jpeg_done = 0, jpeg_all = 0;
    const bool do_transform = o.transform && !o.portable;
    for (size_t start = 0; start < objects.size(); start += (size_t)jobs) {
        size_t count = std::min<size_t>((size_t)jobs, objects.size() - start);
        std::vector<std::optional<Unfolded>> result(count);
        std::vector<Byte> raw(count);
        parallel_for(count, jobs, [&](size_t k) {
            Object &ob = objects[start + k];
            Byte d = read_file(root + "/" + ob.source);
            if (d.size() != ob.size) throw Error(ob.source + ": file changed during packing");
            if (do_transform && ob.cls == "png")
                result[k] = png_unfold(d, PNG_INFLATION_CAP);
            else if (do_transform && ob.cls == "jpeg")
                result[k] = jpeg_unfold(d);
            if (!result[k]) raw[k] = std::move(d);
        });
        for (size_t k = 0; k < count; k++) {
            Object &ob = objects[start + k];
            if (ob.cls == "png") png_all++;
            if (ob.cls == "jpeg") jpeg_all++;
            if (result[k]) {
                const Unfolded &un = *result[k];
                (un.tag == "png" ? png_done : jpeg_done)++;
                ob.transform = un.tag;
                ob.recipe = un.recipe;
                std::tie(ob.rblk, ob.roff, ob.rlen) = place(un.tag + "-rest", un.rest.data(), un.rest.size());
                std::tie(ob.blk, ob.off, ob.len) =
                    place(un.tag == "png" ? "png-ra" : "jpeg-kropp", un.body.data(), un.body.size());
            } else {
                std::tie(ob.blk, ob.off, ob.len) = place(ob.cls, raw[k].data(), raw[k].size());
            }
        }
    }
    if (do_transform && png_all)  say(strf("  png: %d/%d transformed", png_done, png_all));
    if (do_transform && jpeg_all) say(strf("  jpeg: %d/%d transformed", jpeg_done, jpeg_all));
    for (Building &b : blocks) b.b.raw_len = b.raw.size();
    say(strf("  %zu blocks", blocks.size()));

    // --- 5. compress the blocks -----------------------------------------------
    std::vector<Byte> packed(blocks.size());
    int per_block = std::max(1, jobs / std::max<int>(1, (int)blocks.size()));
    const int concurrent = std::min<int>((int)blocks.size(), jobs);

    // The codec probe runs per class, not per block.
    //
    // A probe costs a couple of seconds (zstd19 and xz on 4 MiB) —
    // negligible while a tree produced a handful of blocks. Once unfolded
    // image data started splitting at 32 MiB there were sixty blocks of
    // the same kind of data, and sixty probes with the same answer took
    // over half the pack time: the PNG corpus ran in 191 seconds of which
    // roughly 130 were probes.
    //
    // The class is nvz's answer to "what is this data" — set by the
    // content, and every block in a class carries the same kind of content
    // by construction. So the first block of each class is probed and the
    // rest inherit the answer. This is a simplification: two blocks of
    // class `binar` could in principle want different codecs, but finding
    // out would mean paying the probe sixty times.
    std::map<std::string, std::pair<std::string, double>> probe_answer;
    {
        std::vector<size_t> firsts;      // one block per class that needs probing
        std::set<std::string> seen_cls;
        for (size_t k = 0; k < blocks.size(); k++) {
            const std::string cod = o.codec.empty() ? blocks[k].b.codec : o.codec;
            if (cod.rfind("probe", 0) != 0) continue;
            if (!seen_cls.insert(blocks[k].b.cls).second) continue;
            firsts.push_back(k);
        }
        std::vector<std::pair<std::string, double>> answers(firsts.size());
        parallel_for(firsts.size(), jobs, [&](size_t i) {
            Building &b = blocks[firsts[i]];
            const std::string cod = o.codec.empty() ? b.b.codec : o.codec;
            double m = 1.0;
            std::string chosen = choose_codec(b.raw.data(), b.raw.size(), cod == "probe-cm",
                                              cod == "probe-gpu", per_block,
                                              b.b.cls == "opak", &m);
            answers[i] = {chosen, m};
        });
        for (size_t i = 0; i < firsts.size(); i++)
            probe_answer[blocks[firsts[i]].b.cls] = answers[i];
    }

    parallel_for(blocks.size(), jobs, [&](size_t k) {
        Building &b = blocks[k];
        std::string cod = o.codec.empty() ? b.b.codec : o.codec;
        double margin = 1.0;      // 1.0: no probe was run
        if (cod.rfind("probe", 0) == 0) {
            auto it = probe_answer.find(b.b.cls);
            if (it != probe_answer.end()) { cod = it->second.first; margin = it->second.second; }
            else cod = choose_codec(b.raw.data(), b.raw.size(), cod == "probe-cm",
                                    cod == "probe-gpu", per_block,
                                    b.b.cls == "opak", &margin);
        }
        // Portable archives may only write {ra, xz9e, xz9e-j0}: what a
        // pure-stdlib Python reader can decode (the lzma module). The probe
        // still ran with its full candidate set — it only guides the choice
        // — but anything that is not raw becomes xz here, with the pb
        // variant settled on a sample. Deliberately slower and sometimes
        // larger: portability is the point, see PackOpts.
        if (o.portable && cod != "ra" && cod.rfind("xz9e", 0) != 0)
            cod = xz_variant(b.raw.data(), b.raw.size());
        // nvcm is single-threaded per stream, which made it the whole price
        // of mode `small` on text in both directions. Blocks holding at
        // least two 32 MiB shards are upgraded to nvcm-p: the same model in
        // parallel independent shards (codec.cpp has the measured
        // trade-off: +1.88 % on enwik8 for 3-4x pack and unpack). A forced
        // --codec nvcm is left alone — the forced name is the single-stream
        // minimum and stays what it says.
        if (cod == "nvcm" && o.codec.empty() && b.raw.size() >= (64u << 20))
            cod = "nvcm-p";
        if (cod == "opak-eskalera") {
            // Two questions, and the first one must be cheap.
            //
            // The sample cannot tell film from archives-of-archives: both
            // are entropy-coded and locally dense in 4 MiB (report section
            // 4.2). But "does this block have tens of percent to give?"
            // needs no strong compressor — just an honest pass over the
            // whole block. Measured on the gif corpus:
            //
            //     zstd -3     14 983 720 bytes      68 ms
            //     zstd -19    14 868 267 bytes   4 388 ms
            //     zstd -22    14 867 123 bytes   3 856 ms
            //
            // zstd -3 sees 47.8 of the 48 percentage points available, in
            // a sixtieth of the time — but only when the redundancy is
            // local. On archives-of-archives it sits hundreds of megabytes
            // apart, and plain zstd -3 with its megabyte window saw just
            // 2.6 % of the 33 that existed: the escalation never fired and
            // the archive grew from 248 to 271 MB. The step is therefore
            // zstd3L — the same level with long-distance matching, 27.6 %
            // in the same 725 ms.
            cod = "zstd3L";
            packed[k] = compress(cod, b.raw.data(), b.raw.size(), per_block, concurrent);
            if ((double)packed[k].size() < (double)b.raw.size() * 0.90) {
                // Similarity between files exists, so the window decides:
                // zstd -22 with its 2 GiB window is the right candidate;
                // zstd -19 is dominated and skipped.
                Byte costly = compress("zstd22", b.raw.data(), b.raw.size(), per_block, concurrent);
                if (costly.size() < packed[k].size()) {
                    packed[k] = std::move(costly);
                    cod = "zstd22";
                }
                // Mode `small` only: LZMA with a dictionary covering the
                // whole block — the only candidate whose window reaches
                // across an entire opaque block, and exactly the form 7-Zip
                // beat nvz with in section 3.6 of the report. The step is a
                // full extra compression, worth 2.3 % on the archive corpus
                // — affordable only in the mode that exists to be smallest.
                if (o.mode == "small" && !o.portable) {
                    std::string variant = xz_variant(b.raw.data(), b.raw.size());
                    Byte lz = compress(variant, b.raw.data(), b.raw.size(), per_block, concurrent);
                    if (lz.size() < packed[k].size()) {
                        packed[k] = std::move(lz);
                        cod = variant;
                    }
                }
            } else {
                // Film-like: half a percent to collect, not tens. zstd -19
                // takes the per-mille and is both cheaper and better than
                // zstd -22 on such data (film: 557 863 118 bytes in 56 s
                // vs 557 873 791 in 264 s).
                Byte z19 = compress("zstd19", b.raw.data(), b.raw.size(), per_block, concurrent);
                if (z19.size() < packed[k].size()) {
                    packed[k] = std::move(z19);
                    cod = "zstd19";
                }
            }
            // A portable archive must not keep a zstd block even here.
            if (o.portable && cod.rfind("zstd", 0) == 0) {
                std::string variant = xz_variant(b.raw.data(), b.raw.size());
                Byte lz = compress(variant, b.raw.data(), b.raw.size(), per_block, concurrent);
                u64 raw_n = b.raw.size();
                if (lz.size() < raw_n) { packed[k] = std::move(lz); cod = variant; }
                else { packed[k] = std::move(b.raw); cod = "ra"; }
            }
        } else if (cod == "ra") {
            // Move the block instead of copying it: the film corpus is
            // 537 MB that was otherwise memcpy'd for nothing, with a
            // matching memory peak.
            packed[k] = std::move(b.raw);
        } else {
            packed[k] = compress(cod, b.raw.data(), b.raw.size(), per_block, concurrent);
            // The sample has a blind spot that costs percent. The probe
            // takes 4 MiB as eight disjoint pieces of the block — the right
            // construction for seeing what the data is, but it tears apart
            // exactly the long-range structure that separates the codecs.
            // Eight pieces from different parts of the block hold no
            // repeats between them, so the probe cannot see what a
            // dictionary covering the whole block can do. Measured on a
            // 120 MiB block from the archive corpus: the probe chose
            // zstd19 and got 100.13 MiB where xz with a covering dictionary
            // gave 91.97 — the probe missed 8 percent.
            //
            // A cheap predictive signal does not work: zstd -3's ratio on
            // the sample vs the whole block does differ (1.016 vs 1.025 on
            // two blocks) but ranks them wrongly, so it cannot replace a
            // measurement.
            //
            // Hence, in mode `small`, where size outranks time, the whole
            // block is re-tried with covering-dictionary xz when the probe
            // chose something else — but only when the probe's lead was
            // thin. On text nvcm wins the probe by tens of percent and
            // wins the whole block too, so unconditional re-checking cost a
            // full xz pass for nothing: enwik8 went from 131 to 259
            // seconds without saving a byte. Where the ranking actually
            // flipped (the archive-corpus block, 8.2 %) the probe's lead
            // was under one percent. The threshold stands at eight percent,
            // with margin on the safe side.
            if (o.mode == "small" && !o.portable && cod.rfind("xz9e", 0) != 0 && cod != "ra"
                && margin < 0.08 && b.raw.size() > (16u << 20)) {
                std::string variant = xz_variant(b.raw.data(), b.raw.size());
                Byte lz = compress(variant, b.raw.data(), b.raw.size(), per_block, concurrent);
                if (lz.size() < packed[k].size()) {
                    packed[k] = std::move(lz);
                    cod = variant;
                }
            }
        }
        b.b.codec = cod;
        Byte().swap(b.raw);
    });

    // --- 6. write the archive -------------------------------------------------
    FILE *out = fopen(archive.c_str(), "wb");
    if (!out) throw Error(archive + ": cannot write");
    // Every write is checked, and an archive that did not finish is removed
    // rather than left behind looking valid. See write_checked in util.cpp:
    // the gzip surface deletes the source after a successful pack, so
    // success must mean the bytes are on disk.
    struct Cleanup {
        const std::string &name; FILE *f; bool done = false;
        ~Cleanup() { if (!done) { if (f) fclose(f); unlink(name.c_str()); } }
    } cleanup{archive, out};
    write_checked(out, MAGIC, sizeof MAGIC, archive);
    u64 pos = sizeof MAGIC;
    for (size_t k = 0; k < blocks.size(); k++) {
        blocks[k].b.off = pos;
        blocks[k].b.clen = packed[k].size();
        write_checked(out, packed[k].data(), packed[k].size(), archive);
        pos += packed[k].size();
        Byte().swap(packed[k]);
    }
    Register reg;
    reg.version = 3;                 // v3: short hash, epoch time, selectable register codec
    reg.root = root.substr(root.find_last_of('/') + 1);
    reg.flat = o.flat;
    time_t now = time(nullptr);
    reg.created_epoch = (i64)now;
    char buf[64];
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", localtime(&now));
    reg.created = buf;
    for (Building &b : blocks) reg.blocks.push_back(b.b);
    reg.objects = objects;
    reg.entries = entries;
    Byte raw = write_register(reg);
    // The register is text and numbers and compresses better with xz than
    // with zstd — 227 048 vs 239 053 bytes on the code corpus. But xz's
    // container is ~60 bytes against zstd's ~13, so on a tiny archive xz
    // loses on its own frame. Both are tried and the smaller wins; the
    // reader keys on the stream's magic. A portable archive skips the zstd
    // candidate: its reader may have no zstd at all.
    Byte reg_x = compress("xz9e", raw.data(), raw.size(), jobs);
    Byte reg_packed;
    if (o.portable) {
        reg_packed = std::move(reg_x);
    } else {
        Byte reg_z = compress("zstd19", raw.data(), raw.size(), jobs);
        reg_packed = reg_x.size() < reg_z.size() ? std::move(reg_x) : std::move(reg_z);
    }
    write_checked(out, reg_packed.data(), reg_packed.size(), archive);
    u8 tail[16];
    for (int i = 0; i < 8; i++) tail[i] = (u8)(pos >> (8*i));
    for (int i = 0; i < 8; i++) tail[8+i] = (u8)(raw.size() >> (8*i));
    write_checked(out, tail, 16, archive);
    close_checked(out, archive, true);
    cleanup.f = nullptr; cleanup.done = true;

    u64 bytes = file_size(archive);
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    say(strf("  done: %.2f MiB (x%.2f) in %.1fs", bytes / 1048576.0,
             bytes ? (double)raw_bytes / bytes : 0.0, sec));
    return bytes;
}

// ----------------------------------------------------------------------------
// Unpacking
// ----------------------------------------------------------------------------
void unpack(const std::string &archive, const std::string &target, const PackOpts &o) {
    auto t0 = std::chrono::steady_clock::now();
    int jobs = o.jobs > 0 ? o.jobs : std::min(24u, std::max(1u, std::thread::hardware_concurrency()));
    auto say = [&](const std::string &s) { if (!o.quiet) { printf("%s\n", s.c_str()); fflush(stdout); } };
    Register reg = read_register(archive);

    // Create the target directory with parents: `nvz unpack arkiv.nv
    // new/place` must work without new/place existing beforehand.
    std::string built;
    for (size_t i = 0; i <= target.size(); i++) {
        if (i == target.size() || target[i] == '/') {
            if (!built.empty() && mkdir(built.c_str(), 0755) != 0 && errno != EEXIST)
                throw Error(built + ": cannot create");
        }
        if (i < target.size()) built += target[i];
    }
    std::string root_out = reg.flat ? target : target + "/" + reg.root;
    say(strf("  %zu entries, %zu blocks", reg.entries.size(), reg.blocks.size()));

    // Each block is decompressed once and kept in memory while the files
    // are written out.
    std::vector<Byte> block_data(reg.blocks.size());
    FILE *f = fopen(archive.c_str(), "rb");
    if (!f) throw Error(archive + ": cannot open");
    std::vector<Byte> packed(reg.blocks.size());
    for (size_t i = 0; i < reg.blocks.size(); i++) {
        packed[i].resize(reg.blocks[i].clen);
        if (fseeko(f, (off_t)reg.blocks[i].off, SEEK_SET) != 0 ||
            fread(packed[i].data(), 1, packed[i].size(), f) != packed[i].size())
            { fclose(f); throw Error("the archive is truncated"); }
    }
    fclose(f);
    parallel_for(reg.blocks.size(), jobs, [&](size_t i) {
        block_data[i] = decompress(reg.blocks[i].codec, packed[i].data(),
                                   packed[i].size(), reg.blocks[i].raw_len);
        if (block_data[i].size() != reg.blocks[i].raw_len)
            throw Error("block " + std::to_string(i) + ": wrong length after decompression");
        Byte().swap(packed[i]);
    });

    // Directories first, in order, so everything has somewhere to go.
    for (const Entry &p : reg.entries) {
        if (p.kind != Kind::Dir) continue;
        if (reg.flat && p.path.empty()) continue;   // the root dir belongs to the archive, not the target
        std::string s = p.path.empty() ? root_out : root_out + "/" + p.path;
        if (mkdir(s.c_str(), 0700) != 0 && errno != EEXIST)
            throw Error(s + ": cannot create");
    }
    // Files are written in parallel — not for the disks but for the
    // transforms: every JPEG is rebuilt by a worker process, and 1 143
    // photos in sequence is two minutes where 24 abreast is eight seconds.
    // Directories already exist and no two entries write the same file,
    // so the only shared state is the blocks, which are only read.
    std::vector<const Entry*> file_entries;
    for (const Entry &q : reg.entries)
        if (q.kind != Kind::Dir) file_entries.push_back(&q);
    parallel_for(file_entries.size(), jobs, [&](size_t fi) {
        const Entry &p = *file_entries[fi];
        std::string s = root_out + "/" + p.path;
        if (p.kind == Kind::Link) {
            unlink(s.c_str());
            if (symlink(p.target.c_str(), s.c_str()) != 0) throw Error(s + ": cannot create the link");
            set_metadata(s, 0, p.mtime_ns, true);
        } else if (p.kind == Kind::File) {
            if (p.obj < 0 || (size_t)p.obj >= reg.objects.size()) throw Error(s + ": entry points wrong");
            const Object &ob = reg.objects[(size_t)p.obj];
            const Byte &b = block_data[ob.blk];
            if (ob.off + ob.len > b.size()) throw Error(s + ": object lies outside its block");
            Byte whole;
            const u8 *data = b.data() + ob.off;
            u64 len = ob.len;
            if (!ob.transform.empty()) {
                // Transformed: stitch the original from rest + body. The
                // body sits at (blk, off, len), the rest at
                // (rblk, roff, rlen).
                const Byte &rb = block_data[ob.rblk];
                if (ob.roff + ob.rlen > rb.size()) throw Error(s + ": the rest lies outside its block");
                Byte rest(rb.begin() + ob.roff, rb.begin() + ob.roff + ob.rlen);
                Byte body(b.begin() + ob.off, b.begin() + ob.off + ob.len);
                whole = (ob.transform == "png") ? png_rebuild(rest, body, ob.recipe)
                                                : jpeg_rebuild(rest, body);
                data = whole.data(); len = whole.size();
            }
            write_file(s, data, len);
            // The check is not free, but an archive that silently hands out
            // wrong bytes is worse than one that is slow.
            u8 h[16];
            blake2b128(data, len, h);
            if (memcmp(h, ob.hash, reg.hash_len)) throw Error(s + ": content does not match the register");
            set_metadata(s, p.mode, p.mtime_ns, false);
        }
    });
    // Directory times last: writing into a directory changes its mtime.
    for (auto it = reg.entries.rbegin(); it != reg.entries.rend(); ++it) {
        if (it->kind != Kind::Dir) continue;
        if (reg.flat && it->path.empty()) continue;
        std::string s = it->path.empty() ? root_out : root_out + "/" + it->path;
        set_metadata(s, it->mode, it->mtime_ns, false);
    }
    double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    say(strf("  done in %.1fs", sec));
}

u64 pack_file(const std::string &file, const std::string &archive, const PackOpts &o,
              const std::string &name) {
    // Hard-link the file into a scratch directory next to the archive and
    // pack the tree flat. A link costs nothing; if it fails (filesystem
    // boundary) the file is copied, which may cost.
    std::string dir = archive.substr(0, archive.find_last_of('/') + 1);
    if (dir.empty()) dir = ".";
    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%s/hz-one-XXXXXX", dir.c_str());
    if (!mkdtemp(tmpl)) throw Error("could not create scratch space");
    std::string d = tmpl;
    std::string base = name.empty() ? file.substr(file.find_last_of('/') + 1) : name;
    std::string inner = d + "/" + base;
    if (link(file.c_str(), inner.c_str()) != 0) {
        Byte content = read_file(file);
        write_file(inner, content.data(), content.size());
        struct stat st;
        if (stat(file.c_str(), &st) == 0)
            set_metadata(inner, st.st_mode & 07777,
                         (i64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec, false);
    }
    PackOpts po = o; po.flat = true;
    u64 bytes;
    try { bytes = pack(d, archive, po); }
    catch (...) { unlink(inner.c_str()); rmdir(d.c_str()); throw; }
    unlink(inner.c_str()); rmdir(d.c_str());
    return bytes;
}

void unpack_file(const std::string &archive, const std::string &target, const PackOpts &o) {
    // Unpack next to the target and move into place: the target either
    // does not exist at all or is complete, never half-written.
    std::string dir = target.substr(0, target.find_last_of('/') + 1);
    if (dir.empty()) dir = ".";
    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%s/hz-one-XXXXXX", dir.c_str());
    if (!mkdtemp(tmpl)) throw Error("could not create scratch space");
    std::string d = tmpl;
    try {
        PackOpts po = o; po.quiet = true;
        unpack(archive, d, po);
        Register reg = read_register(archive);
        std::string name = archived_name(reg);
        if (name.empty()) throw Error(archive + ": not a single-file archive");
        if (rename((d + "/" + name).c_str(), target.c_str()) != 0)
            throw Error(target + ": could not move into place");
    } catch (...) {
        std::string clean = "rm -rf '" + d + "'";
        if (system(clean.c_str()) != 0) {}
        throw;
    }
    std::string clean = "rm -rf '" + d + "'";
    if (system(clean.c_str()) != 0) {}
}

std::string archived_name(const Register &reg) {
    if (!reg.flat) return "";
    for (const Entry &p : reg.entries)
        if (p.kind == Kind::File) return p.path;
    return "";
}

u64 unpacked_size(const Register &reg) {
    u64 tot = 0;
    for (const Entry &p : reg.entries)
        if (p.kind == Kind::File && p.obj >= 0 && (size_t)p.obj < reg.objects.size())
            tot += reg.objects[(size_t)p.obj].size;
    return tot;
}

bool verify(const std::string &archive, const PackOpts &o) {
    char tmpl[4096];
    snprintf(tmpl, sizeof tmpl, "%s/nvz-test-XXXXXX", temp_dir().c_str());
    if (!mkdtemp(tmpl)) throw Error("could not create scratch space");
    std::string d = tmpl;
    bool ok = true;
    try {
        PackOpts po = o; po.quiet = true;
        unpack(archive, d, po);      // every file is checked against its hash
    } catch (const std::exception &) {
        ok = false;
    }
    std::string clean = "rm -rf '" + d + "'";
    if (system(clean.c_str()) != 0) {}
    return ok;
}

void list_archive(const std::string &archive) {
    Register reg = read_register(archive);
    printf("hz v%u  root=%s  created %s\n", reg.version, reg.root.c_str(), reg.created.c_str());
    std::map<std::string, int> tf;
    for (const Object &ob : reg.objects) if (!ob.transform.empty()) tf[ob.transform]++;
    std::string tf_text;
    for (auto &kv : tf) tf_text += strf("%s'%s': %d", tf_text.empty() ? "" : ", ",
                                        kv.first.c_str(), kv.second);
    printf("%zu entries, %zu objects, %zu blocks%s%s\n", reg.entries.size(), reg.objects.size(),
           reg.blocks.size(), tf_text.empty() ? "" : ", transformed: {",
           tf_text.empty() ? "" : (tf_text + "}").c_str());
    for (const Block &b : reg.blocks)
        printf("  block %3u  %-11s %-7s %9.2f -> %8.2f MiB  x%.2f\n", b.index, b.cls.c_str(),
               display_codec(b.codec).c_str(), b.raw_len / 1048576.0, b.clen / 1048576.0,
               b.clen ? (double)b.raw_len / b.clen : 0.0);
}

}  // namespace hz
