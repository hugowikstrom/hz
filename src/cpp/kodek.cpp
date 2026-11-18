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
    // zstd3L: nivå 3 men med LÅNGDISTANSMATCHNING (fönster 2^27 = 128 MiB).
    // Den finns för tvåstegsregelns billiga steg (arkiv.cpp): frågan "har
    // blocket tiotals procent att ge?" kan inte besvaras av ett fönster på
    // en megabyte, för på arkiv-av-arkiv ligger redundansen mellan filer
    // hundratals megabyte isär. Mätt på arkivkorpusen (410 833 681 byte):
    //
    //     zstd −3                400 328 735 byte   (2,6 %)     721 ms
    //     zstd −3 --long=27      297 348 678 byte  (27,6 %)     725 ms
    //     zstd −19 --long=27     274 067 642 byte  (33,3 %)   48 325 ms
    //
    // Fönstret är alltså gratis och nivån är dyr. Första mätningen av det
    // här steget använde `zstd −3` utan fönster, och då uteblev
    // eskaleringen helt: arkivet blev 271 MB i stället för 248.
    if (k == "zstd3L") return {3, 27};
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
// ORDBOKEN SKA TÄCKA BLOCKET. preset 9e har en fast ordbok på 64 MiB, och
// nvz:s block är större: 192 MiB för vanliga klasser och hela korpusen för
// opaka. LZMA kan då per konstruktion inte se en upprepning längre bak än
// 64 MiB, hur stort blocket än är — och det är precis den upprepningen
// likhetssorteringen och dedupen lägger dit. Mätt på 120 MiB ur
// arkivkorpusen:
//
//     64 MiB ordbok (preset 9e)        98 700 988 byte    83 s
//     ordbok som täcker indatan        96 559 784 byte    73 s
//
// 2,17 % mindre OCH 12 % snabbare: en funnen matchning är billigare än en
// fortsatt sökning. Det var också hela avståndet till 7-Zips solida
// 512 MiB-form i rapportens 3.6.
//
// Två fällor. Den första: `lzma_stream_encoder_mt` med block_size = 0 delar
// indatan i oberoende bitar om ungefär tre ordböcker, och två oberoende
// bitar kan per konstruktion inte matcha varandra — flertrådningen tar
// alltså tillbaka det ordboken gav. Därför används den bara när blocket är
// större än ordbokstaket, där delningen sker ändå.
//
// Den andra: minnet. Kodaren behöver omkring tio gånger ordboken, så fyra
// parallella block med 512 MiB ordbok vore tjugo gigabyte. Behovet frågas
// därför av liblzma självt och tas ur en grind med en budget som utgår från
// maskinens lediga minne. En ensam begäran släpps alltid fram, hur stor den
// än är, så grinden kan aldrig låsa sig.
// Golvet är inte preset 9e:s 64 MiB utan vad blocket faktiskt behöver. En
// ordbok större än blocket kostar minne utan att kunna användas, och minnet
// är det som avgör hur många block som packas samtidigt: ett 32 MiB-block
// som fick 64 MiB ordbok band 700 MB i stället för 370, och halverade därmed
// parallelliteten på en maskin med lite ledigt minne.
constexpr u64 DICT_GOLV = 4ull << 20;
constexpr u64 DICT_TAK  = 512ull << 20;

u64 minnesbehov(u64 dict) {
    lzma_options_lzma o;
    if (lzma_lzma_preset(&o, 9 | LZMA_PRESET_EXTREME)) return dict * 11;
    o.dict_size = (u32)dict;
    lzma_filter f[2] = {{LZMA_FILTER_LZMA2, &o}, {LZMA_VLI_UNKNOWN, nullptr}};
    u64 b = lzma_raw_encoder_memusage(f);
    return b == UINT64_MAX ? dict * 11 : b;
}

// Ordboken ska täcka blocket — men inte till priset av parallelliteten.
// Mätt på PNG-korpusen: nio block om 192 MiB fick 256 MiB ordbok styck, och
// eftersom kodaren behöver ungefär tio gånger ordboken rymdes bara EN åt
// gången i minnesgrinden. Packtiden gick från 268 till 841 sekunder för
// 0,06 % mindre arkiv. Ordboken krymps därför tills den ryms i budgeten
// delad på antalet kodare som väntas samtidigt, men aldrig under golvet —
// under 64 MiB vore det inte längre samma kodek som `xz -9e`.
u32 dict_for(size_t n, int samtidiga, u64 budget) {
    u64 d = DICT_GOLV;
    while (d < (u64)n && d < DICT_TAK) d <<= 1;
    const u64 per = std::max<u64>(1, (u64)std::max(1, samtidiga));
    while (d > DICT_GOLV && minnesbehov(d) * per > budget) d >>= 1;
    return (u32)std::min(d, DICT_TAK);
}

class Minnesgrind {
    u64 budget, anvant = 0;
    std::mutex m;
    std::condition_variable kv;
public:
    explicit Minnesgrind(u64 b) : budget(b) {}
    void ta(u64 n) {
        std::unique_lock<std::mutex> l(m);
        kv.wait(l, [&] { return anvant == 0 || anvant + n <= budget; });
        anvant += n;
    }
    void slapp(u64 n) {
        { std::lock_guard<std::mutex> l(m); anvant -= n; }
        kv.notify_all();
    }
};

u64 ledigt_minne() {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 2ull << 30;
    char rad[256];
    u64 kb = 0;
    while (fgets(rad, sizeof rad, f))
        if (sscanf(rad, "MemAvailable: %lu kB", &kb) == 1) break;
    fclose(f);
    return kb ? kb * 1024 : (2ull << 30);
}

// Budgeten är hela det lediga minnet, inte hälften: kompressionsminne är
// övergående, och halva budgeten gjorde att PNG-korpusen packade två block
// åt gången i stället för fyra. Grinden står kvar som tak, så nvz kan aldrig
// begära mer än maskinen har — men den ska inte vara snålare än nödvändigt.
const u64 xz_budget = std::max<u64>(1ull << 30, ledigt_minne());
Minnesgrind xz_grind(xz_budget);

// pb (position bits) är LZMA:s antagande om datans ORDLÄNGD: preset 9e
// sätter pb = 2, alltså att positionen modulo fyra bär information, vilket
// gäller för 32-bitars tal och maskinkod. Entropikodad data — arkiv av
// arkiv, film, komprimerade strömmar — har ingen sådan justering, och där är
// pb = 0 bättre. Mätt på 120 MiB ur arkivkorpusen, med ordbok som täcker:
//
//     pb = 2 (preset 9e)   96 559 784 byte
//     pb = 0               96 433 036 byte     -0,13 %
//
// Till skillnad från ordbokens storlek är det här en LOKAL egenskap, så den
// syns i ett stickprov — och därför får stickprovet avgöra den (xz_variant).
// Strömmen bär sina egna filterparametrar, så avkodaren behöver inte veta
// vilken variant som valdes: ett arkiv från före den här ändringen packas
// upp av samma kod som ett efter.
Byte xz_packa(const u8 *d, size_t n, int tradar, u32 pb, int samtidiga = 1) {
    lzma_options_lzma opt;
    if (lzma_lzma_preset(&opt, 9 | LZMA_PRESET_EXTREME))
        throw Fel("xz: kunde inte läsa förinställningen");
    opt.dict_size = dict_for(n, samtidiga, xz_budget);
    opt.pb = pb;
    lzma_filter filter[2] = {{LZMA_FILTER_LZMA2, &opt}, {LZMA_VLI_UNKNOWN, nullptr}};
    const bool tacker = (u64)opt.dict_size >= (u64)n;   // ordboken rymmer hela blocket

    lzma_stream s = LZMA_STREAM_INIT;
    lzma_ret r;
    lzma_mt mt{};
    u64 behov;
    if (!tacker && tradar > 1) {
        mt.threads = (uint32_t)tradar;
        mt.filters = filter;
        mt.check = LZMA_CHECK_CRC64;
        mt.block_size = 0;
        mt.timeout = 0;
        behov = lzma_stream_encoder_mt_memusage(&mt);
    } else {
        behov = lzma_raw_encoder_memusage(filter);
    }
    if (behov == UINT64_MAX) behov = (u64)opt.dict_size * 11;   // liblzma vet inte: skatta
    xz_grind.ta(behov);
    struct Slapp {                      // grinden ska öppnas även vid undantag
        Minnesgrind &g; u64 n;
        ~Slapp() { g.slapp(n); }
    } slapp{xz_grind, behov};

    r = (!tacker && tradar > 1) ? lzma_stream_encoder_mt(&s, &mt)
                                : lzma_stream_encoder(&s, filter, LZMA_CHECK_CRC64);
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

Byte komprimera(const std::string &kodek, const u8 *d, size_t n, int tradar,
                int samtidiga) {
    if (kodek == "ra")   return Byte(d, d + n);
    if (kodek == "xz9e")    return xz_packa(d, n, tradar, 2, samtidiga);
    if (kodek == "xz9e-j0") return xz_packa(d, n, tradar, 0, samtidiga);
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
    if (kodek.rfind("xz9e", 0) == 0) return xz_packa_upp(d, n, ralen);
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
                       bool opak, double *xz_marginal) {
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

    // xz på stickprovet är provets dyraste post, och den ska inte betalas i
    // onödan. Två avgränsningar, båda mätta:
    //
    //   OPAKA BLOCK frågar inte efter xz här. De går till tvåstegsregeln
    //   (opak-eskalera), som avgör saken på hela blocket, så ett xz-pass på
    //   stickprovet är bortkastat. På gif-korpusen var det 4 av 10 sekunder.
    //
    //   pb-VARIANTEN avgörs på en TIONDEL av stickprovet. pb är en lokal
    //   egenskap — vilken ordlängd datan har — och den syns i en halv
    //   megabyte lika väl som i fyra. Att välja variant på hela provet
    //   fördubblade provets kostnad för ett svar som inte blev bättre.
    size_t xb = 0;
    bool j0 = false;
    if (!opak) {
        const size_t pbn = std::min<size_t>(pn, 512u << 10);
        const size_t a = xz_packa(prov.data(), pbn, 1, 2).size();
        const size_t b0 = xz_packa(prov.data(), pbn, 1, 0).size();
        j0 = b0 < a;
        xb = xz_packa(prov.data(), pn, 1, j0 ? 0 : 2).size();
    }
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
    size_t bast = xb ? std::min(zb, xb) : zb;
    if (cb) bast = std::min(bast, cb);
    if (gb) bast = std::min(bast, gb);
    // Marginalen mot xz-kandidaten: 0 betyder "xz var lika bra eller bättre",
    // 0,15 betyder "vinnaren var 15 % mindre än xz på provet".
    if (xz_marginal) *xz_marginal = xb ? (double)(xb - bast) / (double)xb : 0.0;

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
    if (!xb || (double)xb >= zb * 0.995) return "zstd19";
    return j0 ? "xz9e-j0" : "xz9e";
}

std::string xz_variant(const u8 *d, size_t n) {
    // Ett litet sammanhängande prov räcker: pb är en lokal egenskap. Provet
    // tas på mitten för att undvika filhuvuden i början av blocket.
    const size_t pn = std::min<size_t>(n, 1u << 20);
    const size_t start = (n - pn) / 2;
    size_t a = xz_packa(d + start, pn, 1, 2).size();
    size_t b = xz_packa(d + start, pn, 1, 0).size();
    return b < a ? "xz9e-j0" : "xz9e";
}

}  // namespace nvz
