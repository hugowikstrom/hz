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
    o.recept = strf("%d,%d,%d,%d", (*recept)[0], (*recept)[1], (*recept)[2], (*recept)[3]);
    o.rest = std::move(skelett);
    o.kropp = std::move(ra);
    if (png_aterskapa(o.rest, o.kropp, o.recept) != b) return std::nullopt;  // ska inte hända
    return o;
}

Byte png_aterskapa(const Byte &skelett, const Byte &ra, const std::string &recept) {
    Byte strom = packa_om(ra, tolka_recept(recept));
    Byte ut(skelett.begin(), skelett.begin() + std::min<size_t>(8, skelett.size()));
    size_t i = 8, j = 0, n = skelett.size();
    while (i + 8 <= n) {
        size_t ln = be32(&skelett[i]);
        const u8 *typ = &skelett[i+4];
        ut.insert(ut.end(), skelett.begin() + i, skelett.begin() + i + 8);
        if (!memcmp(typ, "IDAT", 4)) {
            if (j + ln > strom.size()) throw Fel("png: strömmen är för kort för skelettet");
            ut.insert(ut.end(), strom.begin() + j, strom.begin() + j + ln);
            j += ln;
            ut.insert(ut.end(), skelett.begin() + i + 8, skelett.begin() + i + 12);
            i += 12;
        } else {
            ut.insert(ut.end(), skelett.begin() + i + 8, skelett.begin() + i + 12 + ln);
            i += 12 + ln;
        }
        if (!memcmp(typ, "IEND", 4)) break;
    }
    ut.insert(ut.end(), skelett.begin() + i, skelett.end());
    return ut;
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
