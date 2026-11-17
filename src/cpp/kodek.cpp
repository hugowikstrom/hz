#include "kodek.hpp"
#include "sjalv.hpp"
#include <condition_variable>
#include <zstd.h>
#include <lzma.h>
#include <cstring>
#include <algorithm>

namespace nvz {
namespace {

// --- zstd ------------------------------------------------------------------
// Fönsterloggen är inte kosmetik. zstd -19 med standardfönster gav 365 MiB
// på arkivkorpusen där samma kodek med --long=27 gav 261. Parametrarna här
// motsvarar därför exakt pythonversionens kommandorader.
struct Zparam { int niva; int fonsterlogg; };
Zparam zparam(const std::string &k) {
    if (k == "zstd3")  return {3, 0};
    if (k == "zstd19") return {19, 27};
    return {22, 31};                       // zstd22
}

Byte zstd_packa(const std::string &k, const u8 *d, size_t n, int tradar) {
    Zparam p = zparam(k);
    ZSTD_CCtx *c = ZSTD_createCCtx();
    if (!c) throw Fel("zstd: slut på minne");
    ZSTD_CCtx_setParameter(c, ZSTD_c_compressionLevel, p.niva);
    if (p.fonsterlogg) {
        ZSTD_CCtx_setParameter(c, ZSTD_c_enableLongDistanceMatching, 1);
        ZSTD_CCtx_setParameter(c, ZSTD_c_windowLog, p.fonsterlogg);
    }
    if (tradar > 1) ZSTD_CCtx_setParameter(c, ZSTD_c_nbWorkers, tradar);
    Byte ut(ZSTD_compressBound(n));
    size_t r = ZSTD_compress2(c, ut.data(), ut.size(), d, n);
    ZSTD_freeCCtx(c);
    if (ZSTD_isError(r)) throw Fel(std::string("zstd: ") + ZSTD_getErrorName(r));
    ut.resize(r);
    return ut;
}

Byte zstd_packa_upp(const u8 *d, size_t n, u64 ralen) {
    ZSTD_DCtx *c = ZSTD_createDCtx();
    if (!c) throw Fel("zstd: slut på minne");
    // Arkiv packade med --long=31 bär ett fönster på 2 GiB. Utan det här
    // vägrar avkodaren med "Frame requires too much memory".
    ZSTD_DCtx_setParameter(c, ZSTD_d_windowLogMax, 31);
    u64 vantat = ralen;
    if (!vantat) {
        unsigned long long g = ZSTD_getFrameContentSize(d, n);
        vantat = (g == ZSTD_CONTENTSIZE_UNKNOWN || g == ZSTD_CONTENTSIZE_ERROR)
                 ? (u64)n * 8 + (1u << 20) : (u64)g;
    }
    Byte ut(vantat);
    size_t r = ZSTD_decompressDCtx(c, ut.data(), ut.size(), d, n);
    ZSTD_freeDCtx(c);
    if (ZSTD_isError(r)) throw Fel(std::string("zstd: ") + ZSTD_getErrorName(r));
    ut.resize(r);
    return ut;
}

// --- xz --------------------------------------------------------------------
Byte xz_packa(const u8 *d, size_t n, int tradar) {
    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r;
    if (tradar > 1) {
        lzma_mt mt{};
        mt.threads = (uint32_t)tradar;
        mt.preset = 9 | LZMA_PRESET_EXTREME;
        mt.check = LZMA_CHECK_CRC64;
        mt.block_size = 0;
        mt.timeout = 0;
        r = lzma_stream_encoder_mt(&s, &mt);
    } else {
        r = lzma_easy_encoder(&s, 9 | LZMA_PRESET_EXTREME, LZMA_CHECK_CRC64);
    }
    if (r != LZMA_OK) throw Fel("xz: kunde inte starta kodaren");
    Byte ut(n / 2 + (1u << 16));
    s.next_in = d; s.avail_in = n;
    s.next_out = ut.data(); s.avail_out = ut.size();
    for (;;) {
        r = lzma_code(&s, LZMA_FINISH);
        if (r == LZMA_STREAM_END) break;
        if (r == LZMA_OK && s.avail_out == 0) {
            size_t anv = ut.size() - s.avail_out;
            ut.resize(ut.size() * 2);
            s.next_out = ut.data() + anv;
            s.avail_out = ut.size() - anv;
            continue;
        }
        if (r != LZMA_OK) { lzma_end(&s); throw Fel("xz: kodningen misslyckades"); }
    }
    ut.resize(ut.size() - s.avail_out);
    lzma_end(&s);
    return ut;
}

Byte xz_packa_upp(const u8 *d, size_t n, u64 ralen) {
    lzma_stream s = LZMA_STREAM_INIT;
    if (lzma_stream_decoder(&s, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
        throw Fel("xz: kunde inte starta avkodaren");
    Byte ut(ralen ? ralen : n * 4 + (1u << 20));
    s.next_in = d; s.avail_in = n;
    s.next_out = ut.data(); s.avail_out = ut.size();
    for (;;) {
        lzma_ret r = lzma_code(&s, LZMA_FINISH);
        if (r == LZMA_STREAM_END) break;
        if (r == LZMA_OK || r == LZMA_BUF_ERROR) {
            if (s.avail_out == 0) {
                size_t anv = ut.size() - s.avail_out;
                ut.resize(ut.size() * 2);
                s.next_out = ut.data() + anv;
                s.avail_out = ut.size() - anv;
                continue;
            }
            if (r == LZMA_BUF_ERROR) { lzma_end(&s); throw Fel("xz: trasig ström"); }
            continue;
        }
        lzma_end(&s);
        throw Fel("xz: avkodningen misslyckades");
    }
    ut.resize(ut.size() - s.avail_out);
    lzma_end(&s);
    return ut;
}

// --- nvcm ------------------------------------------------------------------
// Koden är inlänkad i binären men körs i en arbetarprocess (kor_sjalv):
// en trasig ström får döda arbetaren, aldrig packaren. Grinden släpper
// fram fyra åt gången — nvcm tar en halv gigabyte modelltabeller styck.
class Grind {
    int platser;
    std::mutex m;
    std::condition_variable kv;
public:
    explicit Grind(int n) : platser(n) {}
    void ta()    { std::unique_lock<std::mutex> l(m); kv.wait(l, [&]{ return platser > 0; }); platser--; }
    void slapp() { { std::lock_guard<std::mutex> l(m); platser++; } kv.notify_one(); }
};
Grind nvcm_grind(4);

Byte nvcm_kor(const char *kmd, const u8 *d, size_t n) {
    nvcm_grind.ta();
    int felkod = 0;
    Byte ut;
    try { ut = kor_sjalv({"nvcm", kmd}, d, n, &felkod); }
    catch (...) { nvcm_grind.slapp(); throw; }
    nvcm_grind.slapp();
    if (felkod != 0) throw Fel(strf("nvcm %s: slutstatus %d", kmd, felkod));
    return ut;
}

}  // namespace

Byte komprimera(const std::string &kodek, const u8 *d, size_t n, int tradar) {
    if (kodek == "ra")   return Byte(d, d + n);
    if (kodek == "xz9e") return xz_packa(d, n, tradar);
    if (kodek == "nvcm") return nvcm_kor("packa", d, n);
    if (kodek == "nvlz") {
        // GPU-kodeken bor i nvgpu — eget program, för vulkan-beroendet ska
        // inte in i nvz. Mätinstrument, inte bruksväg: se README.
        int felkod = 0;
        Byte ut = kor_binar(binar_bredvid("nvgpu"), {"packa", "--tyst"}, d, n, &felkod);
        if (felkod != 0) throw Fel("nvgpu packa: slutstatus " + std::to_string(felkod));
        return ut;
    }
    if (kodek.rfind("zstd", 0) == 0) return zstd_packa(kodek, d, n, tradar);
    throw Fel("okänd kodek: " + kodek);
}

Byte dekomprimera(const std::string &kodek, const u8 *d, size_t n, u64 ralen) {
    if (kodek == "ra")   return Byte(d, d + n);
    if (kodek == "xz9e") return xz_packa_upp(d, n, ralen);
    if (kodek == "nvcm" || kodek == "nvz")   // "nvz" skrevs under namnbytets timmar
        return nvcm_kor("packaupp", d, n);
    if (kodek == "nvlz") {
        int felkod = 0;
        Byte ut = kor_binar(binar_bredvid("nvgpu"), {"packaupp", "--tyst"}, d, n, &felkod);
        if (felkod != 0) throw Fel("nvgpu packaupp: slutstatus " + std::to_string(felkod));
        return ut;
    }
    if (kodek.rfind("zstd", 0) == 0) return zstd_packa_upp(d, n, ralen);
    throw Fel("okänd kodek: " + kodek);
}

// --- provet ----------------------------------------------------------------
std::string valj_kodek(const u8 *d, size_t n, bool med_cm, bool med_gpu, int tradar,
                       bool opak) {
    (void)tradar;
    if (n == 0) return "ra";

    // Stickprovet tas på ÅTTA ställen spridda genom blocket. Ett block är
    // sorterat efter ändelse, så de första megabyten är en enda filtyp och
    // säger ingenting om resten.
    Byte prov;
    if (n <= PROVBYTE) {
        prov.assign(d, d + n);
    } else {
        const size_t delar = 8, del = PROVBYTE / delar;
        prov.reserve(PROVBYTE);
        for (size_t i = 0; i < delar; i++) {
            size_t start = i * (n - del) / (delar - 1);
            prov.insert(prov.end(), d + start, d + start + del);
        }
    }
    size_t pn = prov.size();
    size_t zb = zstd_packa("zstd19", prov.data(), pn, 1).size();
    size_t xb = xz_packa(prov.data(), pn, 1).size();
    size_t cb = 0;
    if (med_cm) {
        try { cb = nvcm_kor("packa", prov.data(), pn).size(); }
        catch (...) { cb = 0; }          // vägrar nvcm är det bara ett prov som uteblev
    }
    size_t gb = 0;
    if (med_gpu) {
        int felkod = 0;
        try {
            Byte g = kor_binar(binar_bredvid("nvgpu"), {"packa", "--tyst"},
                               prov.data(), pn, &felkod);
            if (felkod == 0) gb = g.size();
        } catch (...) { gb = 0; }
    }
    size_t bast = std::min(zb, xb);
    if (cb) bast = std::min(bast, cb);
    if (gb) bast = std::min(bast, gb);

    // Tre utfall, inte två — se kommentaren i src/nvz.py: en halv procent av
    // 2,9 GB film är fjorton megabyte, och en billig kodek tar dem gratis.
    if ((double)bast > pn * 0.998) return "ra";
    if ((double)bast > pn * 0.98)  return "zstd3";
    // nvcm behöver marginal: den är mycket dyrare än zstd och ska bara
    // väljas när den tar hem något verkligt.
    if (cb && (double)cb < std::min(zb, xb) * 0.98) return "nvcm";
    // nvlz behöver marginal av samma skäl som nvcm: dyrare än zstd, och ska
    // bara väljas när den tar hem något verkligt.
    if (gb && (double)gb < std::min(zb, xb) * 0.98) return "nvlz";
    // OPAKT OCH ÄNDÅ KOMPRIMERBART är tvetydigt på ett sätt stickprovet
    // inte kan lösa upp: film och arkiv-av-arkiv ser likadana ut i 4 MiB —
    // båda är entropikodade och lokalt täta. Skillnaden syns först när
    // HELA blocket packas: filmens kvot stannar vid någon procent, medan
    // arkivens likheter mellan filer ger tiotals. Därför svarar provet här
    // "eskalera": packa blocket billigt på riktigt, och betala för zstd22
    // bara om den billiga vägen bevisat att det finns något att hämta
    // (rapporten §4.2 — regeln gav −0,1 % mot bästa klassiska på
    // arkivkorpusen utan att röra filmkorpusens tid).
    if (opak) return "opak-eskalera";
    return (double)xb < zb * 0.995 ? "xz9e" : "zstd19";
}

}  // namespace nvz
