#include "codec.hpp"
#include "worker.hpp"
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>
#include <zstd.h>
#include <lzma.h>
#include <cstring>
#include <algorithm>

namespace nvz {
namespace {

// --- zstd -------------------------------------------------------------------
// The window log is not cosmetics. zstd -19 with the default window gave
// 365 MiB on the archive corpus where the same codec with --long=27 gave
// 261. The parameters here therefore match the Python implementation's
// command lines exactly.
struct Zparam { int level; int window_log; };
Zparam zparam(const std::string &k) {
    if (k == "zstd3")  return {3, 0};
    // zstd3L: level 3 but with LONG-DISTANCE MATCHING (window 2^27 =
    // 128 MiB). It exists for the cheap step of the two-step rule
    // (archive.cpp): the question "does this block have tens of percent to
    // give?" cannot be answered by a one-megabyte window, because on
    // archives-of-archives the redundancy sits hundreds of megabytes apart.
    // Measured on the archive corpus (410 833 681 bytes):
    //
    //     zstd -3                400 328 735 bytes   (2.6 %)     721 ms
    //     zstd -3 --long=27      297 348 678 bytes  (27.6 %)     725 ms
    //     zstd -19 --long=27     274 067 642 bytes  (33.3 %)   48 325 ms
    //
    // The window is free and the level is expensive. The first measurement
    // of this step used plain zstd -3, and the escalation never fired: the
    // archive came out 271 MB instead of 248.
    if (k == "zstd3L") return {3, 27};
    if (k == "zstd19") return {19, 27};
    return {22, 31};                       // zstd22
}

Byte zstd_compress(const std::string &k, const u8 *d, size_t n, int threads) {
    Zparam p = zparam(k);
    ZSTD_CCtx *c = ZSTD_createCCtx();
    if (!c) throw Error("zstd: out of memory");
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, p.level);
    if (p.window_log) {
        ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, p.window_log);
    }
    if (threads > 1) ZSTD_CCtx_setParameter(c, ZSTD_c_nbWorkers, threads);
    Byte out(ZSTD_compressBound(n));
    size_t r = ZSTD_compress2(c, out.data(), out.size(), d, n);
    ZSTD_freeCCtx(c);
    if (ZSTD_isError(r)) throw Error(std::string("zstd: ") + ZSTD_getErrorName(r));
    out.resize(r);
    return out;
}

Byte zstd_decompress(const u8 *d, size_t n, u64 raw_len) {
    ZSTD_DCtx *c = ZSTD_createDCtx();
    if (!c) throw Error("zstd: out of memory");
    // Archives packed with --long=31 carry a 2 GiB window. Without this the
    // decoder refuses with "Frame requires too much memory".
    ZSTD_DCtx_setParameter(c, ZSTD_d_windowLogMax, 31);
    u64 expect = raw_len;
    if (!expect) {
        unsigned long long g = ZSTD_getFrameContentSize(d, n);
        expect = (g == ZSTD_CONTENTSIZE_UNKNOWN || g == ZSTD_CONTENTSIZE_ERROR)
                 ? (u64)n * 8 + (1u << 20) : (u64)g;
    }
    Byte out(expect);
    size_t r = ZSTD_decompressDCtx(c, out.data(), out.size(), d, n);
    ZSTD_freeDCtx(c);
    if (ZSTD_isError(r)) throw Error(std::string("zstd: ") + ZSTD_getErrorName(r));
    out.resize(r);
    return out;
}

// --- xz ---------------------------------------------------------------------
// THE DICTIONARY MUST COVER THE BLOCK. Preset 9e has a fixed 64 MiB
// dictionary, and nvz's blocks are larger: 192 MiB for ordinary classes and
// the whole corpus for opaque ones. LZMA then by construction cannot see a
// repeat further back than 64 MiB, however large the block is — and that is
// precisely the repeat the similarity sort and the deduplication put there.
// Measured on 120 MiB from the archive corpus:
//
//     64 MiB dictionary (preset 9e)     98 700 988 bytes    83 s
//     dictionary covering the input     96 559 784 bytes    73 s
//
// 2.17 % smaller AND 12 % faster: a found match is cheaper than a continued
// search. It was also the whole distance to 7-Zip's solid 512 MiB form in
// section 3.6 of the report.
//
// Two traps. First: `lzma_stream_encoder_mt` with block_size = 0 splits the
// input into independent pieces of roughly three dictionaries each, and two
// independent pieces cannot match each other by construction — the
// multithreading takes back what the dictionary gave. It is therefore used
// only when the block is larger than the dictionary cap, where the split
// happens anyway.
//
// Second: memory. The encoder needs roughly ten times the dictionary, so
// four parallel blocks with 512 MiB dictionaries would be twenty gigabytes.
// The need is therefore asked of liblzma itself and drawn from a gate whose
// budget comes from the machine's free memory. A lone request always gets
// through, however large, so the gate can never deadlock.
//
// The floor is not preset 9e's 64 MiB but what the block actually needs. A
// dictionary larger than the block costs memory without being usable, and
// memory is what decides how many blocks compress at once: a 32 MiB block
// that received a 64 MiB dictionary tied up 700 MB instead of 370 and
// halved the parallelism on a machine short of free memory.
constexpr u64 DICT_FLOOR = 4ull << 20;
constexpr u64 DICT_CAP   = 512ull << 20;

u64 mem_needed(u64 dict) {
    lzma_options_lzma o;
    if (lzma_lzma_preset(&o, 9 | LZMA_PRESET_EXTREME)) return dict * 11;
    o.dict_size = (u32)dict;
    lzma_filter f[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
    u64 b = lzma_raw_encoder_memusage(f);
    return b == UINT64_MAX ? dict * 11 : b;
}

// The dictionary should cover the block — but not at the price of the
// parallelism. Measured on the PNG corpus: nine 192 MiB blocks received a
// 256 MiB dictionary each, and since the encoder needs about ten times the
// dictionary, only ONE at a time fit through the memory gate. Pack time went
// from 268 to 841 seconds for an archive 0.06 % smaller. The dictionary is
// therefore shrunk until it fits the budget divided by the number of
// expected concurrent encoders, but never below the floor.
u32 dict_for(size_t n, int concurrent, u64 budget) {
    u64 d = DICT_FLOOR;
    while (d < (u64)n && d < DICT_CAP) d <<= 1;
    const u64 per = std::max<u64>(1, (u64)std::max(1, concurrent));
    while (d > DICT_FLOOR && mem_needed(d) * per > budget) d >>= 1;
    return (u32)std::min(d, DICT_CAP);
}

class MemoryGate {
    u64 budget, used = 0;
    std::mutex m;
    std::condition_variable cv;
public:
    explicit MemoryGate(u64 b) : budget(b) {}
    void take(u64 n) {
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return used == 0 || used + n <= budget; });
        used += n;
    }
    void release(u64 n) {
        { std::lock_guard<std::mutex> l(m); used -= n; }
        cv.notify_all();
    }
};

u64 free_memory() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 2ull << 30;
    char line[256];
    u64 kb = 0;
    while (fgets(line, sizeof line, f))
        if (sscanf(line, "MemAvailable: %lu kB", &kb) == 1) break;
    fclose(f);
    return kb ? kb * 1024 : (2ull << 30);
}

// The budget is all of free memory, not half: compression memory is
// transient, and half the budget made the PNG corpus compress two blocks at
// a time instead of four. The gate remains as a ceiling, so nvz can never
// ask for more than the machine has — but it should not be stingier than
// necessary. KNOWN TRADE-OFF (report section 8): this makes dictionary
// sizes — and with them ratio and pack time — depend on how much memory the
// machine happens to have free at start-up. Measure on an unloaded machine.
const u64 xz_budget = std::max<u64>(1ull << 30, free_memory());
MemoryGate xz_gate(xz_budget);

// `pb` (position bits) is LZMA's assumption about the WORD LENGTH of the
// data: preset 9e sets pb = 2, i.e. that position modulo four carries
// information, which holds for 32-bit numbers and machine code. Entropy-
// coded data — archives of archives, film, compressed streams — has no such
// alignment, and there pb = 0 is better. Measured on 120 MiB of archive
// data, with a covering dictionary:
//
//     pb = 2 (preset 9e)   96 559 784 bytes
//     pb = 0               96 433 036 bytes     -0.13 %
//
// Unlike the dictionary size this is a LOCAL property, so it shows in a
// sample — which is why the sample decides it (xz_variant). The stream
// carries its own filter parameters, so the decoder never needs to know
// which variant was chosen: an archive from before this change decompresses
// through the same code as one from after.
Byte xz_compress(const u8 *d, size_t n, int threads, u32 pb, int concurrent = 1) {
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 9 | LZMA_PRESET_EXTREME))
        throw Error("xz: could not load the preset");
    opt.dict_size = dict_for(n, concurrent, xz_budget);
    opt.pb = pb;
    lzma_filter filter[2] = {{LZMA_FILTER_LZMA2, &opt}, {LZMA_VLI_UNKNOWN, nullptr}};
    const bool covers = (u64)opt.dict_size >= (u64)n;   // dictionary holds the whole block

    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r;
    lzma_mt mt{};
    u64 need;
    if (!covers && threads > 1) {
        mt.threads = (uint32_t)threads;
        mt.filters = filter;
        mt.check = LZMA_CHECK_CRC64;
        mt.block_size = 0;
        mt.timeout = 0;
        need = lzma_stream_encoder_mt_memusage(&mt);
    } else {
        need = lzma_raw_encoder_memusage(filter);
    }
    if (need == UINT64_MAX) need = (u64)opt.dict_size * 11;   // liblzma does not know: estimate
    xz_gate.take(need);
    struct Release {                      // the gate must open even on an exception
        MemoryGate &g; u64 n;
        ~Release() { g.release(n); }
    } release{xz_gate, need};

    r = (!covers && threads > 1) ? lzma_stream_encoder_mt(&s, &mt)
                                 : lzma_stream_encoder(&s, filter, LZMA_CHECK_CRC64);
    if (r != LZMA_OK) throw Error("xz: could not start the encoder");
    Byte out(n / 2 + (1u << 16));
    s.next_in = d; s.avail_in = n;
    s.next_out = out.data(); s.avail_out = out.size();
    for (;;) {
        r = lzma_code(&s, LZMA_FINISH);
        if (r == LZMA_STREAM_END) break;
        if (r == LZMA_OK && s.avail_out == 0) {
            size_t used = out.size() - s.avail_out;
            out.resize(out.size() * 2);
            s.next_out = out.data() + used;
            s.avail_out = out.size() - used;
            continue;
        }
        if (r != LZMA_OK) { lzma_end(&s); throw Error("xz: encoding failed"); }
    }
    out.resize(out.size() - s.avail_out);
    lzma_end(&s);
    return out;
}

Byte xz_decompress(const u8 *d, size_t n, u64 raw_len) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
        throw Error("xz: could not start the decoder");
    Byte out(raw_len ? raw_len : n * 4 + (1u << 20));
    s.next_in = d; s.avail_in = n;
    s.next_out = out.data(); s.avail_out = out.size();
    for (;;) {
        lzma_ret r = lzma_code(&s, LZMA_FINISH);
        if (r == LZMA_STREAM_END) break;
        if (r == LZMA_OK || r == LZMA_BUF_ERROR) {
            if (s.avail_out == 0) {
                size_t used = out.size() - s.avail_out;
                out.resize(out.size() * 2);
                s.next_out = out.data() + used;
                s.avail_out = out.size() - used;
                continue;
            }
            if (r == LZMA_BUF_ERROR) { lzma_end(&s); throw Error("xz: broken stream"); }
            continue;
        }
        lzma_end(&s);
        throw Error("xz: decoding failed");
    }
    out.resize(out.size() - s.avail_out);
    lzma_end(&s);
    return out;
}

// --- nvcm -------------------------------------------------------------------
// The code is linked into the binary but runs in a worker process
// (run_self): a broken stream may kill the worker, never the packer.
//
// The gate is budgeted in MEMORY, not in slots. A worker's model tables
// cost ~550 MB for any input above ~4 MiB (the table size formula caps at
// 2^24 counters), plus about twice the input for its buffers. The old
// fixed gate of four underused a 28 GB machine and would overuse a 4 GB
// one; a budget does the right thing on both, and a lone request always
// passes so the gate cannot deadlock.
u64 nvcm_mem_needed(size_t n) {
    return (550ull << 20) + 2 * (u64)n + (16ull << 20);
}
MemoryGate nvcm_gate(std::max<u64>(free_memory() / 2, (u64)1200 << 20));

Byte nvcm_run(const char *cmd, const u8 *d, size_t n) {
    const u64 need = nvcm_mem_needed(n);
    nvcm_gate.take(need);
    struct Release { MemoryGate &g; u64 n; ~Release() { g.release(n); } } rel{nvcm_gate, need};
    int exit_code = 0;
    Byte out = run_self({"nvcm", cmd}, d, n, &exit_code);
    if (exit_code != 0) throw Error(strf("nvcm %s: exit status %d", cmd, exit_code));
    return out;
}

// --- nvcm-p: nvcm in parallel shards ------------------------------------------
// nvcm is single-threaded per stream, and that is the WHOLE price of mode
// `small` on text: 23 cores watch one core grind at ~1 MB/s, in both
// directions. nvcm-p splits the block into independent shards, each a
// complete nvcm stream with a cold model, coded and decoded in parallel.
//
// The shard size is measured, not guessed (enwik8, 12 threads):
//
//      8 MiB shards   +5.59 % size   15.0 s   (8.5x vs the single stream)
//     16 MiB shards   +3.48 %        20.7 s   (6.2x)
//     32 MiB shards   +1.88 %        37.4 s   (3.4x)
//
// 32 MiB keeps the loss under two percent while both pack and unpack gain
// 3-4x — and the unpack gain is what retires the "100x slower to unpack"
// objection against mode `small`. Anyone who wants the absolute minimum
// still has the single stream via --codec nvcm.
//
// Container: "NVCMP1" | u32 shard count | u64 raw total | u64 clen per
// shard | the shards, each an ordinary nvcm stream. Shards are defined by
// RAW offsets (i*SHARD), so the decoder knows every shard's raw size from
// the header alone. The codec name "nvcm-p" is a format string: archives
// using it need an nvz from 2026-11-19 or later (or any version, if the
// block was small enough to stay plain "nvcm").
constexpr size_t NVCM_SHARD = 32u << 20;
constexpr char NVCMP_MAGIC[6] = {'N','V','C','M','P','1'};

void shard_parallel(size_t count, const std::function<void(size_t)> &fn) {
    int t = std::max(1, std::min<int>((int)count,
            (int)std::max(1u, std::thread::hardware_concurrency())));
    std::atomic<size_t> next{0};
    std::vector<std::thread> pool;
    std::mutex err_m; std::string first_err;
    for (int i = 0; i < t; i++)
        pool.emplace_back([&] {
            for (;;) {
                size_t k = next.fetch_add(1);
                if (k >= count) return;
                try { fn(k); }
                catch (const std::exception &e) {
                    std::lock_guard<std::mutex> l(err_m);
                    if (first_err.empty()) first_err = e.what();
                    next.store(count); return;
                }
            }
        });
    for (auto &x : pool) x.join();
    if (!first_err.empty()) throw Error(first_err);
}

Byte nvcmp_compress(const u8 *d, size_t n) {
    const size_t K = (n + NVCM_SHARD - 1) / NVCM_SHARD;
    std::vector<Byte> shards(K);
    shard_parallel(K, [&](size_t i) {
        size_t off = i * NVCM_SHARD;
        size_t len = std::min(NVCM_SHARD, n - off);
        shards[i] = nvcm_run("pack", d + off, len);
    });
    Byte out;
    out.insert(out.end(), NVCMP_MAGIC, NVCMP_MAGIC + 6);
    auto u32v = [&](u32 v) { for (int i = 0; i < 4; i++) out.push_back((u8)(v >> (8*i))); };
    auto u64v = [&](u64 v) { for (int i = 0; i < 8; i++) out.push_back((u8)(v >> (8*i))); };
    u32v((u32)K);
    u64v((u64)n);
    for (const Byte &s : shards) u64v((u64)s.size());
    for (Byte &s : shards) { out.insert(out.end(), s.begin(), s.end()); Byte().swap(s); }
    return out;
}

Byte nvcmp_decompress(const u8 *d, size_t n) {
    if (n < 18 || memcmp(d, NVCMP_MAGIC, 6)) throw Error("nvcm-p: broken container");
    size_t i = 6;
    auto u32v = [&]() { u32 v = 0; for (int k = 0; k < 4; k++) v |= (u32)d[i+k] << (8*k); i += 4; return v; };
    auto u64v = [&]() { u64 v = 0; for (int k = 0; k < 8; k++) v |= (u64)d[i+k] << (8*k); i += 8; return v; };
    const u32 K = u32v();
    const u64 raw_total = u64v();
    if (i + (size_t)K * 8 > n) throw Error("nvcm-p: truncated header");
    std::vector<u64> clen(K);
    for (u32 k = 0; k < K; k++) clen[k] = u64v();
    std::vector<u64> coff(K);
    u64 pos = i;
    for (u32 k = 0; k < K; k++) { coff[k] = pos; pos += clen[k]; }
    if (pos != n) throw Error("nvcm-p: lengths do not match the container");
    Byte out(raw_total);
    shard_parallel(K, [&](size_t k) {
        size_t roff = k * NVCM_SHARD;
        size_t rlen = std::min<size_t>(NVCM_SHARD, raw_total - roff);
        Byte piece = nvcm_run("unpack", d + coff[k], clen[k]);
        if (piece.size() != rlen) throw Error("nvcm-p: shard has the wrong length");
        memcpy(out.data() + roff, piece.data(), rlen);
    });
    return out;
}

}  // namespace

Byte compress(const std::string &codec, const u8 *d, size_t n, int threads,
              int concurrent) {
    if (codec == "ra")   return Byte(d, d + n);
    if (codec == "xz9e")    return xz_compress(d, n, threads, 2, concurrent);
    if (codec == "xz9e-j0") return xz_compress(d, n, threads, 0, concurrent);
    if (codec == "nvcm") return nvcm_run("pack", d, n);
    if (codec == "nvcm-p") return nvcmp_compress(d, n);
    if (codec == "nvlz") {
        // The GPU codec lives in nvgpu — a separate program, because the
        // Vulkan dependency must not get into nvz. A measurement
        // instrument, not a production path: see the README.
        int exit_code = 0;
        Byte out = run_program(sibling_binary("nvgpu"), {"pack", "--quiet"}, d, n, &exit_code);
        if (exit_code != 0) throw Error("nvgpu pack: exit status " + std::to_string(exit_code));
        return out;
    }
    if (codec.rfind("zstd", 0) == 0) return zstd_compress(codec, d, n, threads);
    throw Error("unknown codec: " + codec);
}

Byte decompress(const std::string &codec, const u8 *d, size_t n, u64 raw_len) {
    if (codec == "ra")   return Byte(d, d + n);
    if (codec.rfind("xz9e", 0) == 0) return xz_decompress(d, n, raw_len);
    if (codec == "nvcm" || codec == "nvz")   // "nvz" was written during the renaming hours
        return nvcm_run("unpack", d, n);
    if (codec == "nvcm-p") return nvcmp_decompress(d, n);
    if (codec == "nvlz") {
        int exit_code = 0;
        Byte out = run_program(sibling_binary("nvgpu"), {"unpack", "--quiet"}, d, n, &exit_code);
        if (exit_code != 0) throw Error("nvgpu unpack: exit status " + std::to_string(exit_code));
        return out;
    }
    if (codec.rfind("zstd", 0) == 0) return zstd_decompress(d, n, raw_len);
    throw Error("unknown codec: " + codec);
}

// --- the probe --------------------------------------------------------------
std::string choose_codec(const u8 *d, size_t n, bool with_cm, bool with_gpu, int threads,
                         bool opaque, double *xz_margin) {
    (void)threads;
    if (n == 0) return "ra";

    // The sample is taken at EIGHT spots spread through the block. A block
    // is sorted by extension, so the first megabytes are a single file type
    // and say nothing about the rest.
    Byte probe;
    if (n <= PROBE_BYTES) {
        probe.assign(d, d + n);
    } else {
        const size_t parts = 8, part = PROBE_BYTES / parts;
        probe.reserve(PROBE_BYTES);
        for (size_t i = 0; i < parts; i++) {
            size_t start = i * (n - part) / (parts - 1);
            probe.insert(probe.end(), d + start, d + start + part);
        }
    }
    size_t pn = probe.size();
    size_t zb = zstd_compress("zstd19", probe.data(), pn, 1).size();

    // xz on the sample is the probe's most expensive item, and it must not
    // be paid for nothing. Two cuts, both measured:
    //
    //   OPAQUE BLOCKS do not ask for xz here. They go to the two-step rule
    //   (opak-eskalera), which settles the matter on the whole block, so an
    //   xz pass on the sample is wasted. On the gif corpus it was 4 of 10
    //   seconds.
    //
    //   THE pb VARIANT is decided on a TENTH of the sample. pb is a local
    //   property — what word length the data has — and it shows in half a
    //   megabyte as well as in four. Choosing the variant on the full
    //   sample doubled the probe's cost for an answer that got no better.
    size_t xb = 0;
    bool j0 = false;
    if (!opaque) {
        const size_t pbn = std::min<size_t>(pn, 512u << 10);
        const size_t a = xz_compress(probe.data(), pbn, 1, 2).size();
        const size_t b0 = xz_compress(probe.data(), pbn, 1, 0).size();
        j0 = b0 < a;
        xb = xz_compress(probe.data(), pn, 1, j0 ? 0 : 2).size();
    }
    // The reference the expensive codecs must beat. When xz was skipped
    // (opaque blocks) zstd stands in — spelled out here because the earlier
    // `std::min(zb, xb)` with xb == 0 silently made the threshold
    // unbeatable, which happened to be unreachable but was fragile.
    const size_t cheap_ref = xb ? std::min(zb, xb) : zb;
    size_t cb = 0;
    if (with_cm) {
        try { cb = nvcm_run("pack", probe.data(), pn).size(); }
        catch (...) { cb = 0; }          // if nvcm refuses, a probe simply went missing
    }
    size_t gb = 0;
    if (with_gpu) {
        int exit_code = 0;
        try {
            Byte g = run_program(sibling_binary("nvgpu"), {"pack", "--quiet"},
                                 probe.data(), pn, &exit_code);
            if (exit_code == 0) gb = g.size();
        } catch (...) { gb = 0; }
    }
    size_t best = cheap_ref;
    if (cb) best = std::min(best, cb);
    if (gb) best = std::min(best, gb);
    // The margin against the xz candidate: 0 means "xz was as good or
    // better", 0.15 means "the winner was 15 % smaller than xz on the
    // sample".
    if (xz_margin) *xz_margin = xb ? (double)(xb - best) / (double)xb : 0.0;

    // Three outcomes, not two — see the comment in src/nvz.py: half a
    // percent of 2.9 GB of film is fourteen megabytes, and a cheap codec
    // takes them for free.
    if ((double)best > pn * 0.998) return "ra";
    if ((double)best > pn * 0.98)  return "zstd3";
    // nvcm needs a margin: it is far more expensive than zstd and should
    // only be chosen when it brings home something real.
    if (cb && (double)cb < cheap_ref * 0.98) return "nvcm";
    // nvlz needs a margin for the same reason.
    if (gb && (double)gb < cheap_ref * 0.98) return "nvlz";
    // OPAQUE YET COMPRESSIBLE is ambiguous in a way the sample cannot
    // resolve: film and archives-of-archives look the same in 4 MiB — both
    // entropy-coded and locally dense. The difference only shows when the
    // WHOLE block is compressed: film's ratio stops at a percent or so,
    // while between-file similarity in archives yields tens. The probe
    // therefore answers "escalate": compress the block cheaply for real,
    // and pay for zstd22 only once the cheap pass has proven there is
    // something to collect (report section 4.2).
    if (opaque) return "opak-eskalera";
    if (!xb || (double)xb >= zb * 0.995) return "zstd19";
    return j0 ? "xz9e-j0" : "xz9e";
}

std::string xz_variant(const u8 *d, size_t n) {
    // A small contiguous sample is enough: pb is a local property. It is
    // taken from the middle to avoid file headers at the start of the block.
    const size_t pn = std::min<size_t>(n, 1u << 20);
    const size_t start = (n - pn) / 2;
    size_t a = xz_compress(d + start, pn, 1, 2).size();
    size_t b = xz_compress(d + start, pn, 1, 0).size();
    return b < a ? "xz9e-j0" : "xz9e";
}

}  // namespace nvz
