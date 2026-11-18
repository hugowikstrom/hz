// PNG- och JPEG-omvandlingarna. Porterade rad för rad från src/png.py och
// src/jpeg.py — kommentarerna där förklarar varför; här står bara hur.
//
// Det enda som INTE är en portering är anropssättet för nvjpg: python körde
// binären src/nvjpg, här körs samma kod inlänkad i den egna binären via en
// arbetarprocess (se sjalv.cpp).
#include "omvandla.hpp"
#include "sjalv.hpp"
#include <zlib.h>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <array>
#include <deque>

namespace nvz {
namespace {

constexpr size_t STEG = 1 << 18;

// Recept som fungerat i den här processen, nyast först. Bilder ur samma
// verktyg delar i praktiken alltid recept, så från och med bild två är
// första gissningen rätt.
std::mutex senaste_mutex;
std::deque<std::array<int,4>> senaste;

u32 be32(const u8 *p) { return ((u32)p[0]<<24)|((u32)p[1]<<16)|((u32)p[2]<<8)|p[3]; }

struct Bit { size_t i, ln; char typ[5]; };

// Returnerar false om filen inte är en välformad PNG (då lagras den rå).
bool bitar(const Byte &b, std::vector<Bit> &ut) {
    static const u8 SIGNATUR[8] = {0x89,'P','N','G','\r','\n',0x1a,'\n'};
    if (b.size() < 8 || memcmp(b.data(), SIGNATUR, 8)) return false;
    size_t i = 8, n = b.size();
    while (i + 8 <= n) {
        size_t ln = be32(&b[i]);
        Bit bit; bit.i = i; bit.ln = ln;
        memcpy(bit.typ, &b[i+4], 4); bit.typ[4] = 0;
        if (i + 12 + ln > n) return false;
        ut.push_back(bit);
        i += 12 + ln;
        if (!memcmp(bit.typ, "IEND", 4)) break;
    }
    return i == n;            // efterhängsna bytes — lagra rått i stället
}

std::vector<std::array<int,4>> kandidater(const Byte &strom) {
    if (strom.size() < 2) return {};
    u8 cmf = strom[0], flg = strom[1];
    if ((cmf & 0x0f) != 8 || (flg & 0x20)) return {};     // inte deflate, eller FDICT
    int wbits = (cmf >> 4) + 8;
    if (wbits < 9 || wbits > 15) return {};
    int flevel = flg >> 6;
    // zlib sätter FLEVEL ur nivån, så gruppen är ett besked och inte en
    // gissning. Inom gruppen: 9 och 6 är det som verktyg faktiskt använder.
    std::vector<int> grupp;
    switch (flevel) {
    case 0: grupp = {1}; break;
    case 1: grupp = {2,3,4,5}; break;
    case 2: grupp = {6}; break;
    default: grupp = {9,8,7}; break;
    }
    std::stable_sort(grupp.begin(), grupp.end(), [](int a, int b) {
        auto vikt = [](int n) { return n == 9 ? 0 : n == 6 ? 1 : 10 - n; };
        return vikt(a) < vikt(b);
    });
    std::vector<std::array<int,4>> kand;
    for (int n : grupp)
        for (int m : {8, 9})
            for (int s : {0, 1, 2, 3, 4})
                kand.push_back(std::array<int,4>{n, wbits, m, s});
    std::lock_guard<std::mutex> l(senaste_mutex);
    std::vector<std::array<int,4>> ut;
    for (const auto &r : senaste)
        if (std::find(kand.begin(), kand.end(), r) != kand.end()) ut.push_back(r);
    for (const auto &r : kand)
        if (std::find(ut.begin(), ut.end(), r) == ut.end()) ut.push_back(r);
    return ut;
}

// Packar om rådatan och jämför stegvis — avbryter vid första avvikelsen.
// Fel parametrar avviker nästan alltid inom de första hundra kilobytena.
bool provar(const Byte &ra, const Byte &mal, const std::array<int,4> &r) {
    z_stream z{};
    if (deflateInit2(&z, r[0], Z_DEFLATED, r[1], r[2], r[3]) != Z_OK) return false;
    Byte ut;
    ut.reserve(mal.size() + 64);
    u8 buf[1 << 16];
    bool ok = true;
    size_t i = 0, n = ra.size();
    do {
        size_t bit = std::min(STEG, n - i);
        z.next_in = const_cast<u8*>(ra.data() + i);
        z.avail_in = (uInt)bit;
        i += bit;
        int lage = (i == n) ? Z_FINISH : Z_NO_FLUSH;
        do {
            z.next_out = buf; z.avail_out = sizeof buf;
            int rc = deflate(&z, lage);
            if (rc == Z_STREAM_ERROR) { ok = false; break; }
            size_t fick = sizeof buf - z.avail_out;
            if (ut.size() + fick > mal.size() ||
                memcmp(mal.data() + ut.size(), buf, fick)) { ok = false; break; }
            ut.insert(ut.end(), buf, buf + fick);
            if (rc == Z_STREAM_END) break;
        } while (z.avail_out == 0 || z.avail_in > 0);
        if (!ok) break;
    } while (i < n);
    deflateEnd(&z);
    return ok && ut.size() == mal.size();
}

Byte packa_om(const Byte &ra, const std::array<int,4> &r) {
    z_stream z{};
    if (deflateInit2(&z, r[0], Z_DEFLATED, r[1], r[2], r[3]) != Z_OK)
        throw Fel("zlib: deflateInit2 misslyckades");
    Byte ut(deflateBound(&z, ra.size()));
    z.next_in = const_cast<u8*>(ra.data()); z.avail_in = (uInt)ra.size();
    z.next_out = ut.data(); z.avail_out = (uInt)ut.size();
    int rc = deflate(&z, Z_FINISH);
    size_t n = ut.size() - z.avail_out;
    deflateEnd(&z);
    if (rc != Z_STREAM_END) throw Fel("zlib: ofullständig deflate");
    ut.resize(n);
    return ut;
}

// DEFLATE-MOTORNS AVTRYCK, och varför det behövs.
//
// Receptet är fyra zlib-parametrar, och det vilar på ett antagande som inte
// håller: att de fyra bestämmer utströmmen. Det gör de inte — utdatan beror
// på IMPLEMENTATIONEN. zlib och zlib-ng ger olika bytes för samma
// parametrar, och zlib-ng är standard-libz på fler och fler distributioner.
// Ted Lyngmo visade det i en kodgranskning 2026-11-18: ett arkiv packat mot
// systemets zlib gick inte att packa upp mot zlib-ng.
//
// Avtrycket är beteendebaserat, inte en versionssträng: en fast provsträng
// deflateras med fasta parametrar och utdatans crc32 blir avtrycket. Två
// bibliotek som ger samma bytes är utbytbara oavsett vad de kallar sig, och
// det är precis den egenskapen som spelar roll här.
std::string motoravtryck() {
    static const std::string avtryck = [] {
        Byte prov;
        for (int i = 0; i < 4096; i++)                 // deterministisk, inte slumpad
            prov.push_back((u8)((i * 37 + (i >> 3) * 11) & 0xff));
        for (const char *t = "nvz deflate probe "; *t; t++) prov.push_back((u8)*t);
        u32 c = 0;
        try {
            Byte ut = packa_om(prov, {6, 15, 8, 0});
            c = (u32)crc32(0, ut.data(), (uInt)ut.size());
        } catch (...) { c = 0; }
        const char *v = zlibVersion();
        std::string rent;
        for (const char *q = v; *q && rent.size() < 12; q++)
            if (*q != ',' && *q != ';' && *q != ' ') rent.push_back(*q);
        return strf("z%s-%08x", rent.c_str(), c);
    }();
    return avtryck;
}

std::array<int,4> tolka_recept(const std::string &s) {
    std::array<int,4> r{};
    if (sscanf(s.c_str(), "%d,%d,%d,%d", &r[0], &r[1], &r[2], &r[3]) != 4)
        throw Fel("trasigt png-recept: " + s);
    return r;
}

}  // namespace

std::optional<Omvandlat> png_veckla_ut(const Byte &b, double uppblasningstak) {
    std::vector<Bit> alla;
    if (!bitar(b, alla)) return std::nullopt;
    std::vector<const Bit*> idat;
    size_t idatlen = 0;
    for (const Bit &bit : alla)
        if (!memcmp(bit.typ, "IDAT", 4)) { idat.push_back(&bit); idatlen += bit.ln; }
    if (idat.empty() || idatlen < 512) return std::nullopt;   // för litet för att vara värt något

    Byte strom;
    strom.reserve(idatlen);
    for (const Bit *bit : idat)
        strom.insert(strom.end(), b.begin() + bit->i + 8, b.begin() + bit->i + 8 + bit->ln);

    // zlib.decompress(strom): zlib-huvud, okänd slutstorlek.
    Byte ra;
    {
        z_stream z{};
        if (inflateInit2(&z, 15) != Z_OK) return std::nullopt;
        ra.resize(std::max<size_t>(strom.size() * 4, 1 << 16));
        z.next_in = strom.data(); z.avail_in = (uInt)strom.size();
        size_t fyllt = 0;
        for (;;) {
            z.next_out = ra.data() + fyllt; z.avail_out = (uInt)(ra.size() - fyllt);
            int rc = inflate(&z, Z_NO_FLUSH);
            fyllt = ra.size() - z.avail_out;
            if (rc == Z_STREAM_END) break;
            if (rc != Z_OK && rc != Z_BUF_ERROR) { inflateEnd(&z); return std::nullopt; }
            if ((double)fyllt > (double)strom.size() * uppblasningstak) {
                inflateEnd(&z); return std::nullopt;      // skyddsräcket, se nvz.py
            }
            if (z.avail_out == 0) ra.resize(ra.size() * 2);
            else if (rc == Z_BUF_ERROR) { inflateEnd(&z); return std::nullopt; }
        }
        inflateEnd(&z);
        ra.resize(fyllt);
    }

    std::optional<std::array<int,4>> recept;
    for (const auto &k : kandidater(strom))
        if (provar(ra, strom, k)) { recept = k; break; }
    if (!recept) return std::nullopt;
    {
        std::lock_guard<std::mutex> l(senaste_mutex);
        auto it = std::find(senaste.begin(), senaste.end(), *recept);
        if (it != senaste.end()) senaste.erase(it);
        senaste.push_front(*recept);
        while (senaste.size() > 4) senaste.pop_back();
    }

    // Skelettet är filen utan IDAT-innehåll; kontrollsummorna ligger kvar.
    Byte skelett;
    size_t sist = 0;
    for (const Bit *bit : idat) {
        skelett.insert(skelett.end(), b.begin() + sist, b.begin() + bit->i + 8);
        sist = bit->i + 8 + bit->ln;
    }
    skelett.insert(skelett.end(), b.begin() + sist, b.end());

    Omvandlat o;
    o.tr = "png";
    // Femte fältet är motorns avtryck. Äldre recept har fyra fält och
    // tolkas som förut, så formatet är oförändrat.
    o.recept = strf("%d,%d,%d,%d,%s", (*recept)[0], (*recept)[1], (*recept)[2],
                    (*recept)[3], motoravtryck().c_str());
    o.rest = std::move(skelett);
    o.kropp = std::move(ra);
    if (png_aterskapa(o.rest, o.kropp, o.recept) != b) return std::nullopt;  // ska inte hända
    return o;
}

namespace {

// Sätter ihop PNG:en av skelett och ström, och VERIFIERAR varje IDAT-bit mot
// skelettets egen CRC32 innan den godtas.
//
// Verifieringen är inte en försiktighetsåtgärd utan en nödvändighet. Den
// gamla koden kopierade skelettets CRC-bytes rakt in i utdatan, så en ström
// med rätt LÄNGD men fel bytes — vilket är precis vad en annan deflate-motor
// kan ge — hade blivit en PNG med fel bilddata och en CRC som ser riktig ut.
// Tyst korruption är det enda utfall en packare aldrig får ha, och det var
// vad som fanns här. Nu blir samma fall ett nej.
std::optional<Byte> satt_ihop(const Byte &skelett, const Byte &strom) {
    Byte ut(skelett.begin(), skelett.begin() + std::min<size_t>(8, skelett.size()));
    size_t i = 8, j = 0, n = skelett.size();
    while (i + 8 <= n) {
        size_t ln = be32(&skelett[i]);
        const u8 *typ = &skelett[i+4];
        if (i + 12 + (memcmp(typ, "IDAT", 4) ? ln : 0) > n) return std::nullopt;
        ut.insert(ut.end(), skelett.begin() + i, skelett.begin() + i + 8);
        if (!memcmp(typ, "IDAT", 4)) {
            if (j + ln > strom.size()) return std::nullopt;       // strömmen för kort
            const u8 *data = strom.data() + j;
            u32 c = (u32)crc32(crc32(0, typ, 4), data, (uInt)ln);
            if (c != be32(&skelett[i + 8])) return std::nullopt;  // fel bytes
            ut.insert(ut.end(), data, data + ln);
            j += ln;
            ut.insert(ut.end(), skelett.begin() + i + 8, skelett.begin() + i + 12);
            i += 12;
        } else {
            ut.insert(ut.end(), skelett.begin() + i + 8, skelett.begin() + i + 12 + ln);
            i += 12 + ln;
        }
        if (!memcmp(typ, "IEND", 4)) break;
    }
    if (j != strom.size()) return std::nullopt;        // strömmen för lång
    ut.insert(ut.end(), skelett.begin() + i, skelett.end());
    return ut;
}

std::string recept_motor(const std::string &s) {
    size_t k = 0;
    for (int f = 0; f < 4; f++) {
        k = s.find(',', k);
        if (k == std::string::npos) return "";
        k++;
    }
    return s.substr(k);
}

}  // namespace

Byte png_aterskapa(const Byte &skelett, const Byte &ra, const std::string &recept) {
    const std::array<int,4> r = tolka_recept(recept);
    if (auto ut = satt_ihop(skelett, packa_om(ra, r))) return *ut;

    // Receptet återskapade inte strömmen. Den här maskinens deflate-motor är
    // alltså inte den som packade arkivet — men parametrarna i receptet är
    // en egenskap hos FILEN, inte hos motorn, så en annan kombination kan
    // råka ge samma bytes här. Sökningen är billig i förhållande till
    // alternativet (ett arkiv som inte går att packa upp) och körs bara på
    // felvägen. Fönsterstorleken varieras inte: den står i PNG:ens eget
    // zlib-huvud och är densamma oavsett motor.
    for (int niva = 9; niva >= 1; niva--)
        for (int minne : {r[2], 9, 8, 7, 6, 5, 4, 3, 2, 1})
            for (int strat : {r[3], 0, 1, 2, 3, 4}) {
                if (niva == r[0] && minne == r[2] && strat == r[3]) continue;
                std::array<int,4> k{niva, r[1], minne, strat};
                try {
                    if (auto ut = satt_ihop(skelett, packa_om(ra, k))) return *ut;
                } catch (const std::exception &) { /* ogiltig kombination */ }
            }

    const std::string packad = recept_motor(recept);
    throw Fel("png: den här maskinens deflate-motor (" + motoravtryck() +
              ") återskapar inte bildströmmen" +
              (packad.empty() ? std::string(" och arkivet säger inte vilken som packade det")
                              : " — arkivet packades med " + packad) +
              ". Packa upp på en maskin med samma motor, eller packa om "
              "arkivet med --utan-omvandling. Ingen felaktig data har "
              "skrivits: varje IDAT-bit kontrolleras mot sin CRC.");
}

std::optional<Omvandlat> jpeg_veckla_ut(const Byte &b) {
    int felkod = 0;
    Byte d = kor_sjalv({"nvjpg", "packa"}, b.data(), b.size(), &felkod);
    if (felkod != 0 || d.size() < 18 || memcmp(d.data(), "NVJ1", 4))
        return std::nullopt;             // progressiv, trasig, eller inte omkodbar
    u32 huvudlen, svanslen;
    memcpy(&huvudlen, &d[4], 4); memcpy(&svanslen, &d[8], 4);
    size_t grans = 18 + (size_t)huvudlen + svanslen;
    if (grans > d.size() || d.size() >= b.size()) return std::nullopt;  // ingen vinst
    Omvandlat o;
    o.tr = "jpeg";
    o.rest.assign(d.begin(), d.begin() + grans);
    o.kropp.assign(d.begin() + grans, d.end());
    if (jpeg_aterskapa(o.rest, o.kropp) != b) return std::nullopt;      // ska inte hända
    return o;
}

Byte jpeg_aterskapa(const Byte &rest, const Byte &kropp) {
    Byte ihop(rest);
    ihop.insert(ihop.end(), kropp.begin(), kropp.end());
    int felkod = 0;
    Byte ut = kor_sjalv({"nvjpg", "packaupp"}, ihop.data(), ihop.size(), &felkod);
    if (felkod != 0) throw Fel("nvjpg packaupp misslyckades");
    return ut;
}

}  // namespace nvz
