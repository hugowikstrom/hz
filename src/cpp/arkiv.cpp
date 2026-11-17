// Arkivet: block plus register.
//
//   "NVPACK\x01" | block 0 | block 1 | ... | register | <u64 offset, u64 rålen>
//
// Behållaren är densamma som pythonversionens, med flit: ett arkiv som den
// skrivit ska gå att packa upp med den här. Skillnaden ligger i registret.
// Python skriver JSON; här skrivs ett binärt v2. Läsaren känner igen båda på
// första byten — '{' är v1, "NVZREG2" är v2.
#include "arkiv.hpp"
#include "kodek.hpp"
#include "trad.hpp"
#include "json.hpp"
#include "omvandla.hpp"
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

namespace nvz {
namespace {

const u8 MAGI[7] = {'N','V','P','A','C','K',1};
const char REG2[8] = {'N','V','Z','R','E','G','2','\0'};

// Kodek per klass och läge. 'prova' = blocket får avgöra själv genom att
// packa ett stickprov med varje kandidat; 'prova-cm' låter även nvcm tävla.
// Tabellen är pythonversionens LÄGEN, ordagrant.
std::map<std::string, std::string> lagets_kodekar(const std::string &lage) {
    if (lage == "snabb")
        return {{"text","zstd3"},{"binar","zstd3"},{"png-ra","zstd3"},{"png-rest","zstd3"},
                {"jpeg-rest","zstd3"},{"jpeg-kropp","ra"},{"jpeg","ra"},{"opak","ra"},
                {"png","zstd3"}};
    if (lage == "normal")
        return {{"text","prova"},{"binar","prova"},{"png-ra","prova"},{"png-rest","zstd19"},
                {"jpeg-rest","zstd19"},{"jpeg-kropp","ra"},{"jpeg","prova"},{"opak","prova"},
                {"png","prova"}};
    if (lage == "gpu")
        // som normal, men nvlz får tävla om varje block. Mätinstrument:
        // på den här maskinen vinner den inget block (se README).
        return {{"text","prova-gpu"},{"binar","prova-gpu"},{"png-ra","prova-gpu"},
                {"png-rest","prova-gpu"},{"jpeg-rest","prova-gpu"},{"jpeg-kropp","ra"},
                {"jpeg","prova"},{"opak","prova"},{"png","prova-gpu"}};
    // liten — standardläget: nvcm på allt där den vinner provet. Utvecklad
    // bilddata (png-ra) är för voluminös för henne; den provar zstd/xz.
    return {{"text","prova-cm"},{"binar","prova-cm"},{"png-ra","prova"},{"png-rest","prova-cm"},
            {"jpeg-rest","prova-cm"},{"jpeg-kropp","ra"},{"jpeg","prova"},{"opak","prova"},
            {"png","prova"}};
}

// --- skrivhjälp för det binära registret -----------------------------------
struct Ut {
    Byte d;
    void u8v(u8 v) { d.push_back(v); }
    void u32v(u32 v) { for (int i = 0; i < 4; i++) d.push_back((u8)(v >> (8*i))); }
    void u64v(u64 v) { for (int i = 0; i < 8; i++) d.push_back((u8)(v >> (8*i))); }
    void i64v(i64 v) { u64v((u64)v); }
    void strv(const std::string &s) {
        u32v((u32)s.size());
        d.insert(d.end(), s.begin(), s.end());
    }
    void ravv(const u8 *p, size_t n) { d.insert(d.end(), p, p + n); }
};

struct In {
    const u8 *p, *slut;
    void kolla(size_t n) { if ((size_t)(slut - p) < n) throw Fel("registret är stympat"); }
    u8 u8v() { kolla(1); return *p++; }
    u32 u32v() { kolla(4); u32 v = 0; for (int i = 0; i < 4; i++) v |= (u32)p[i] << (8*i); p += 4; return v; }
    u64 u64v() { kolla(8); u64 v = 0; for (int i = 0; i < 8; i++) v |= (u64)p[i] << (8*i); p += 8; return v; }
    i64 i64v() { return (i64)u64v(); }
    std::string strv() { u32 n = u32v(); kolla(n); std::string s((const char*)p, n); p += n; return s; }
    void ravv(u8 *ut, size_t n) { kolla(n); memcpy(ut, p, n); p += n; }
};

Byte skriv_register(const Register &r) {
    Ut u;
    u.ravv((const u8*)REG2, 8);
    u.u32v(r.version);
    u.strv(r.rot);
    u.u8v(r.platt ? 1 : 0);
    u.strv(r.skapad);
    u.u32v((u32)r.block.size());
    for (const Block &b : r.block) {
        u.u32v(b.i); u.strv(b.klass); u.strv(b.kodek);
        u.u64v(b.off); u.u64v(b.clen); u.u64v(b.ralen);
    }
    u.u32v((u32)r.objekt.size());
    for (const Objekt &o : r.objekt) { u.ravv(o.h, 16); u.u64v(o.s); }
    for (const Objekt &o : r.objekt) { u.u32v(o.blk); u.u64v(o.off); u.u64v(o.len); }
    // Omvandlade objekt är få; de står för sig i stället för som tomma fält
    // på varje rad.
    u32 ntr = 0;
    for (const Objekt &o : r.objekt) if (!o.tr.empty()) ntr++;
    u.u32v(ntr);
    for (u32 i = 0; i < r.objekt.size(); i++) {
        const Objekt &o = r.objekt[i];
        if (o.tr.empty()) continue;
        u.u32v(i); u.strv(o.tr); u.strv(o.recept);
        u.u32v(o.rblk); u.u64v(o.roff); u.u64v(o.rlen);
    }
    u.u32v((u32)r.poster.size());
    for (const Post &p : r.poster) {
        u.strv(p.p); u.u8v((u8)p.t);
        u.u32v(p.m); u.i64v(p.mt);
        if (p.t == Typ::Lank) u.strv(p.mal);
        if (p.t == Typ::Fil)  u.i64v(p.o);
    }
    return u.d;
}

Register las_register_v2(const u8 *d, size_t n) {
    In in{d + 8, d + n};
    Register r;
    r.version = in.u32v();
    r.rot = in.strv();
    r.platt = in.u8v() != 0;
    r.skapad = in.strv();
    u32 nb = in.u32v();
    r.block.resize(nb);
    for (Block &b : r.block) {
        b.i = in.u32v(); b.klass = in.strv(); b.kodek = in.strv();
        b.off = in.u64v(); b.clen = in.u64v(); b.ralen = in.u64v();
    }
    u32 no = in.u32v();
    r.objekt.resize(no);
    for (Objekt &o : r.objekt) { in.ravv(o.h, 16); o.s = in.u64v(); }
    for (Objekt &o : r.objekt) { o.blk = in.u32v(); o.off = in.u64v(); o.len = in.u64v(); }
    u32 ntr = in.u32v();
    for (u32 i = 0; i < ntr; i++) {
        u32 id = in.u32v();
        if (id >= r.objekt.size()) throw Fel("registret pekar utanför objektlistan");
        Objekt &o = r.objekt[id];
        o.tr = in.strv(); o.recept = in.strv();
        o.rblk = in.u32v(); o.roff = in.u64v(); o.rlen = in.u64v();
    }
    u32 np = in.u32v();
    r.poster.resize(np);
    for (Post &p : r.poster) {
        p.p = in.strv();
        p.t = (Typ)in.u8v();
        p.m = in.u32v(); p.mt = in.i64v();
        if (p.t == Typ::Lank) p.mal = in.strv();
        if (p.t == Typ::Fil)  p.o = in.i64v();
    }
    return r;
}

// --- v1: pythonversionens JSON ---------------------------------------------
// Platserna i blocken står inte i v1 utan räknas fram ur längderna, i exakt
// den ordning objekten lades dit. Samma härledning som i src/nvz.py.
Register las_register_v1(const std::string &text) {
    Jptr j = las_json(text);
    Register r;
    r.hashlangd = 8;
    r.version = (u32)j->at("version")->som_tal();
    r.rot = j->at("rot")->som_strang();
    r.platt = j->at("platt")->b;
    r.skapad = j->at("skapad")->som_strang();
    for (const Jptr &b : j->at("block")->lista) {
        Block bl;
        bl.i = (u32)b->at("i")->som_tal();
        bl.klass = b->at("klass")->som_strang();
        bl.kodek = b->at("kodek")->som_strang();
        bl.off = (u64)b->at("off")->som_tal();
        bl.clen = (u64)b->at("clen")->som_tal();
        bl.ralen = (u64)b->at("rålen")->som_tal();
        r.block.push_back(bl);
    }
    const Jptr &o = j->at("objekt");
    const auto &blk = o->at("blk")->lista;
    const auto &len = o->at("len")->lista;
    const auto &sl  = o->at("s")->lista;
    const auto &hl  = o->at("h")->lista;
    const Jptr &tr  = o->at("tr");
    r.objekt.resize(blk.size());
    std::map<u32, u64> lopande;
    for (size_t i = 0; i < blk.size(); i++) {
        Objekt &ob = r.objekt[i];
        ob.id = (u32)i;
        ob.blk = (u32)blk[i]->som_tal();
        ob.len = (u64)len[i]->som_tal();
        ob.s = (u64)sl[i]->som_tal();
        const std::string &hx = hl[i]->som_strang();
        for (size_t k = 0; k + 1 < hx.size() && k / 2 < 16; k += 2)
            ob.h[k/2] = (u8)strtoul(hx.substr(k, 2).c_str(), nullptr, 16);
        auto it = tr->objekt.find(std::to_string(i));
        if (it != tr->objekt.end() && it->second->lista.size() >= 3) {
            const auto &t = it->second->lista;
            ob.tr = t[0]->som_strang();
            ob.rblk = (u32)t[1]->som_tal();
            ob.rlen = (u64)t[2]->som_tal();
            if (t.size() > 3 && t[3]->k == Jvarde::K::Lista && t[3]->lista.size() == 4)
                ob.recept = strf("%lld,%lld,%lld,%lld",
                                 (long long)t[3]->lista[0]->som_tal(),
                                 (long long)t[3]->lista[1]->som_tal(),
                                 (long long)t[3]->lista[2]->som_tal(),
                                 (long long)t[3]->lista[3]->som_tal());
            else if (t.size() > 3 && t[3]->k == Jvarde::K::Strang)
                ob.recept = t[3]->som_strang();
            ob.roff = lopande[ob.rblk];
            lopande[ob.rblk] = ob.roff + ob.rlen;
        }
        ob.off = lopande[ob.blk];
        lopande[ob.blk] = ob.off + ob.len;
    }
    for (const Jptr &p : j->at("poster")->lista) {
        Post po;
        po.p = p->at("p")->som_strang();
        po.t = (Typ)p->at("t")->som_strang()[0];
        po.m = p->har("m") ? (u32)p->at("m")->som_tal() : 0644;
        po.mt = p->at("mt")->som_tal();
        if (p->har("mal")) po.mal = p->at("mal")->som_strang();
        po.o = p->har("o") ? p->at("o")->som_tal() : -1;
        r.poster.push_back(po);
    }
    return r;
}

u64 filstorlek(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) throw Fel(p + ": finns inte");
    return (u64)st.st_size;
}

// Kör *n* uppgifter på *tradar* trådar.
void parallellt(size_t n, int tradar, const std::function<void(size_t)> &gor) {
    if (n == 0) return;
    int t = std::max(1, std::min<int>(tradar, (int)n));
    std::atomic<size_t> nasta{0};
    std::vector<std::thread> tr;
    std::mutex felmutex;
    std::string forsta_fel;
    for (int i = 0; i < t; i++) {
        tr.emplace_back([&] {
            for (;;) {
                size_t k = nasta.fetch_add(1);
                if (k >= n) return;
                try { gor(k); }
                catch (const std::exception &e) {
                    std::lock_guard<std::mutex> l(felmutex);
                    if (forsta_fel.empty()) forsta_fel = e.what();
                    nasta.store(n);
                    return;
                }
            }
        });
    }
    for (auto &x : tr) x.join();
    if (!forsta_fel.empty()) throw Fel(forsta_fel);
}

}  // namespace

Register las_register(const std::string &arkiv) {
    u64 n = filstorlek(arkiv);
    if (n < sizeof MAGI + 16) throw Fel("Inte ett nvz-arkiv.");
    FILE *f = fopen(arkiv.c_str(), "rb");
    if (!f) throw Fel(arkiv + ": går inte att öppna");
    u8 magi[sizeof MAGI];
    if (fread(magi, 1, sizeof magi, f) != sizeof magi || memcmp(magi, MAGI, sizeof MAGI)) {
        fclose(f); throw Fel("Inte ett nvz-arkiv.");
    }
    u8 svans[16];
    fseek(f, -16, SEEK_END);
    if (fread(svans, 1, 16, f) != 16) { fclose(f); throw Fel("arkivet är stympat"); }
    u64 regoff = 0, ralen = 0;
    for (int i = 0; i < 8; i++) regoff |= (u64)svans[i] << (8*i);
    for (int i = 0; i < 8; i++) ralen  |= (u64)svans[8+i] << (8*i);
    if (regoff + 16 > n) { fclose(f); throw Fel("registret pekar utanför arkivet"); }
    Byte packat(n - 16 - regoff);
    fseek(f, (long)regoff, SEEK_SET);
    if (fread(packat.data(), 1, packat.size(), f) != packat.size()) {
        fclose(f); throw Fel("kunde inte läsa registret");
    }
    fclose(f);
    Byte rad = dekomprimera("zstd19", packat.data(), packat.size(), ralen);
    if (rad.size() >= 8 && !memcmp(rad.data(), REG2, 8)) return las_register_v2(rad.data(), rad.size());
    if (!rad.empty() && rad[0] == '{')
        return las_register_v1(std::string((const char*)rad.data(), rad.size()));
    throw Fel("okänt registerformat");
}

// ---------------------------------------------------------------------------
// Packning
// ---------------------------------------------------------------------------
u64 packa(const std::string &rot_in, const std::string &arkiv, const Val &v) {
    auto t0 = std::chrono::steady_clock::now();
    int jobb = v.jobb > 0 ? v.jobb : std::min(24u, std::max(1u, std::thread::hardware_concurrency()));
    auto kodekar = lagets_kodekar(v.lage);
    char riktig[4096];
    if (!realpath(rot_in.c_str(), riktig)) throw Fel(rot_in + ": finns inte");
    std::string rot = riktig;
    auto sag = [&](const std::string &s) { if (!v.tyst) { printf("%s\n", s.c_str()); fflush(stdout); } };

    size_t overhoppat = 0;
    std::vector<Post> poster = las_trad(rot, &overhoppat);
    if (overhoppat)
        fprintf(stderr, "  varning: %zu specialfiler (rör/uttag/enheter) hoppades över\n", overhoppat);

    std::vector<size_t> filindex;
    u64 rabyte = 0;
    for (size_t i = 0; i < poster.size(); i++)
        if (poster[i].t == Typ::Fil) { filindex.push_back(i); rabyte += poster[i].s; }
    sag(strf("  %zu poster, %.1f MiB, läge %s", poster.size(),
             rabyte / 1048576.0, v.lage.c_str()));

    // --- 1. hasha och klassa ------------------------------------------------
    std::vector<std::string> klasser(filindex.size());
    std::vector<std::array<u8,16>> hashar(filindex.size());
    parallellt(filindex.size(), jobb, [&](size_t k) {
        const Post &p = poster[filindex[k]];
        Byte d = las_fil(rot + "/" + p.p);
        blake2b128(d.data(), d.size(), hashar[k].data());
        klasser[k] = klassificera(p.p, d.data(), d.size());
    });

    // --- 2. dedup -----------------------------------------------------------
    std::vector<Objekt> objekt;
    std::map<std::string, u32> sedda;
    for (size_t k = 0; k < filindex.size(); k++) {
        Post &p = poster[filindex[k]];
        std::string nyckel = hex(hashar[k].data(), 16);
        if (!v.dedup) nyckel += ":" + p.p;
        auto it = sedda.find(nyckel);
        if (it == sedda.end()) {
            Objekt o;
            memcpy(o.h, hashar[k].data(), 16);
            o.s = p.s; o.klass = klasser[k];
            o.andelse = andelse_av(p.p);
            o.kalla = p.p;
            o.id = (u32)objekt.size();
            sedda[nyckel] = o.id;
            objekt.push_back(o);
            p.o = (i64)o.id;
        } else {
            p.o = (i64)it->second;
        }
    }
    u64 unikt = 0;
    for (const Objekt &o : objekt) unikt += o.s;
    if (v.dedup && rabyte > unikt)
        sag(strf("  dedup: %zu dubbletter, %.1f MiB",
                 filindex.size() - objekt.size(), (rabyte - unikt) / 1048576.0));

    // --- 3. sortera ---------------------------------------------------------
    // Klass först, sedan ändelse, sedan sökväg: filer som liknar varandra
    // hamnar efter varandra i strömmen.
    std::vector<u32> ordning(objekt.size());
    for (u32 i = 0; i < ordning.size(); i++) ordning[i] = i;
    std::sort(ordning.begin(), ordning.end(), [&](u32 a, u32 b) {
        const Objekt &x = objekt[a], &y = objekt[b];
        if (x.klass != y.klass) return x.klass < y.klass;
        if (x.andelse != y.andelse) return x.andelse < y.andelse;
        return x.kalla < y.kalla;
    });
    std::vector<u32> nytt(objekt.size());
    for (u32 i = 0; i < ordning.size(); i++) nytt[ordning[i]] = i;
    std::vector<Objekt> sorterade(objekt.size());
    for (u32 i = 0; i < ordning.size(); i++) {
        sorterade[i] = objekt[ordning[i]];
        sorterade[i].id = i;
    }
    objekt.swap(sorterade);
    for (Post &p : poster) if (p.t == Typ::Fil && p.o >= 0) p.o = nytt[p.o];

    // --- 4. omvandla och lägg i block ---------------------------------------
    // Omvandlingarna körs i omgångar om *jobb* så att ordningen från
    // sorteringen överlever och minnet hålls nere: en utvecklad PNG kan vara
    // hundra gånger sin filstorlek, och 24 sådana i luften räcker för att
    // fylla maskinen.
    struct Bygge { Block b; Byte ra; };
    std::vector<Bygge> block;
    std::map<std::string, size_t> oppet;
    // Opaka block delas inte vid 192 MiB. Taket finns för parallellitetens
    // skull, men på opak data är blockdelning en kompressionsgräns: de
    // likheter som alls finns ligger mellan filer långt isär, och två
    // oberoende block kan per konstruktion inte matcha varandra
    // (rapport §4.2: hela vinsten bor i fönstret). zstd är dessutom
    // flertrådad inuti blocket, så parallelliteten överlever ändå.
    auto tak_for = [](const std::string &klass) {
        return klass == "opak" ? (u64)16 << 30 : BLOCKTAK;
    };
    auto lagg = [&](const std::string &klass, const u8 *d, size_t n) {
        auto it = oppet.find(klass);
        if (it == oppet.end() || block[it->second].ra.size() >= tak_for(klass)) {
            Bygge nb;
            nb.b.i = (u32)block.size();
            nb.b.klass = klass;
            nb.b.kodek = kodekar.count(klass) ? kodekar[klass] : "prova";
            block.push_back(std::move(nb));
            oppet[klass] = block.size() - 1;
            it = oppet.find(klass);
        }
        Bygge &b = block[it->second];
        u64 off = b.ra.size();
        b.ra.insert(b.ra.end(), d, d + n);
        return std::make_tuple(b.b.i, off, (u64)n);
    };

    int png_om = 0, png_alla = 0, jpeg_om = 0, jpeg_alla = 0;
    for (size_t start = 0; start < objekt.size(); start += (size_t)jobb) {
        size_t antal = std::min<size_t>((size_t)jobb, objekt.size() - start);
        std::vector<std::optional<Omvandlat>> svar(antal);
        std::vector<Byte> rad(antal);
        parallellt(antal, jobb, [&](size_t k) {
            Objekt &o = objekt[start + k];
            Byte d = las_fil(rot + "/" + o.kalla);
            if (d.size() != o.s) throw Fel(o.kalla + ": filen ändrades under packningen");
            if (v.omvandla && o.klass == "png")
                svar[k] = png_veckla_ut(d, PNG_UPPBLASNING_TAK);
            else if (v.omvandla && o.klass == "jpeg")
                svar[k] = jpeg_veckla_ut(d);
            if (!svar[k]) rad[k] = std::move(d);
        });
        for (size_t k = 0; k < antal; k++) {
            Objekt &o = objekt[start + k];
            if (o.klass == "png") png_alla++;
            if (o.klass == "jpeg") jpeg_alla++;
            if (svar[k]) {
                const Omvandlat &om = *svar[k];
                (om.tr == "png" ? png_om : jpeg_om)++;
                o.tr = om.tr;
                o.recept = om.recept;
                std::tie(o.rblk, o.roff, o.rlen) = lagg(om.tr + "-rest", om.rest.data(), om.rest.size());
                std::tie(o.blk, o.off, o.len) =
                    lagg(om.tr == "png" ? "png-ra" : "jpeg-kropp", om.kropp.data(), om.kropp.size());
            } else {
                std::tie(o.blk, o.off, o.len) = lagg(o.klass, rad[k].data(), rad[k].size());
            }
        }
    }
    if (v.omvandla && png_alla)  sag(strf("  png: %d/%d omvandlade", png_om, png_alla));
    if (v.omvandla && jpeg_alla) sag(strf("  jpeg: %d/%d omvandlade", jpeg_om, jpeg_alla));
    for (Bygge &b : block) b.b.ralen = b.ra.size();
    sag(strf("  %zu block", block.size()));

    // --- 5. packa blocken ---------------------------------------------------
    std::vector<Byte> packade(block.size());
    int per_block = std::max(1, jobb / std::max<int>(1, (int)block.size()));
    parallellt(block.size(), jobb, [&](size_t k) {
        Bygge &b = block[k];
        std::string kod = v.kodek.empty() ? b.b.kodek : v.kodek;
        if (kod.rfind("prova", 0) == 0)
            kod = valj_kodek(b.ra.data(), b.ra.size(), kod == "prova-cm",
                             kod == "prova-gpu", per_block,
                             b.b.klass == "opak");
        if (kod == "opak-eskalera") {
            // Tvåsteg för opaka block: zstd19 på hela blocket avgör det
            // stickprovet inte kan. Vann den mindre än 10 % är innehållet
            // film eller likvärdigt — då är dyrare kodek bortkastad tid.
            // Vann den mer finns likheter mellan filer, och då tar zstd22
            // med sitt större fönster hem resten.
            kod = "zstd19";
            packade[k] = komprimera(kod, b.ra.data(), b.ra.size(), per_block);
            if ((double)packade[k].size() < (double)b.ra.size() * 0.90) {
                Byte dyr = komprimera("zstd22", b.ra.data(), b.ra.size(), per_block);
                if (dyr.size() < packade[k].size()) {
                    packade[k] = std::move(dyr);
                    kod = "zstd22";
                }
            }
        } else {
            packade[k] = komprimera(kod, b.ra.data(), b.ra.size(), per_block);
        }
        b.b.kodek = kod;
        Byte().swap(b.ra);
    });

    // --- 6. skriv arkivet ---------------------------------------------------
    FILE *ut = fopen(arkiv.c_str(), "wb");
    if (!ut) throw Fel(arkiv + ": går inte att skriva");
    fwrite(MAGI, 1, sizeof MAGI, ut);
    u64 pos = sizeof MAGI;
    for (size_t k = 0; k < block.size(); k++) {
        block[k].b.off = pos;
        block[k].b.clen = packade[k].size();
        fwrite(packade[k].data(), 1, packade[k].size(), ut);
        pos += packade[k].size();
        Byte().swap(packade[k]);
    }
    Register reg;
    reg.rot = rot.substr(rot.find_last_of('/') + 1);
    reg.platt = v.platt;
    time_t nu = time(nullptr);
    char tid[64];
    strftime(tid, sizeof tid, "%Y-%m-%dT%H:%M:%S", localtime(&nu));
    reg.skapad = tid;
    for (Bygge &b : block) reg.block.push_back(b.b);
    reg.objekt = objekt;
    reg.poster = poster;
    Byte rad = skriv_register(reg);
    Byte regp = komprimera("zstd19", rad.data(), rad.size(), jobb);
    fwrite(regp.data(), 1, regp.size(), ut);
    u8 svans[16];
    for (int i = 0; i < 8; i++) svans[i] = (u8)(pos >> (8*i));
    for (int i = 0; i < 8; i++) svans[8+i] = (u8)(rad.size() >> (8*i));
    fwrite(svans, 1, 16, ut);
    fclose(ut);

    u64 byte = filstorlek(arkiv);
    double sek = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    sag(strf("  klart: %.2f MiB (x%.2f) på %.1fs", byte / 1048576.0,
             byte ? (double)rabyte / byte : 0.0, sek));
    return byte;
}

// ---------------------------------------------------------------------------
// Uppackning
// ---------------------------------------------------------------------------
void packa_upp(const std::string &arkiv, const std::string &mal, const Val &v) {
    auto t0 = std::chrono::steady_clock::now();
    int jobb = v.jobb > 0 ? v.jobb : std::min(24u, std::max(1u, std::thread::hardware_concurrency()));
    auto sag = [&](const std::string &s) { if (!v.tyst) { printf("%s\n", s.c_str()); fflush(stdout); } };
    Register reg = las_register(arkiv);

    // Målkatalogen skapas med föräldrar: `nvz packa-upp arkiv.nv ny/plats`
    // ska fungera utan att någon först bygger ny/plats för hand.
    std::string bygg;
    for (size_t i = 0; i <= mal.size(); i++) {
        if (i == mal.size() || mal[i] == '/') {
            if (!bygg.empty() && mkdir(bygg.c_str(), 0755) != 0 && errno != EEXIST)
                throw Fel(bygg + ": går inte att skapa");
        }
        if (i < mal.size()) bygg += mal[i];
    }
    std::string rotut = reg.platt ? mal : mal + "/" + reg.rot;
    sag(strf("  %zu poster, %zu block", reg.poster.size(), reg.block.size()));

    // Blocken packas upp en gång var och hålls i minnet medan filerna skrivs.
    std::vector<Byte> blockdata(reg.block.size());
    FILE *f = fopen(arkiv.c_str(), "rb");
    if (!f) throw Fel(arkiv + ": går inte att öppna");
    std::vector<Byte> packade(reg.block.size());
    for (size_t i = 0; i < reg.block.size(); i++) {
        packade[i].resize(reg.block[i].clen);
        fseek(f, (long)reg.block[i].off, SEEK_SET);
        if (fread(packade[i].data(), 1, packade[i].size(), f) != packade[i].size())
            { fclose(f); throw Fel("arkivet är stympat"); }
    }
    fclose(f);
    parallellt(reg.block.size(), jobb, [&](size_t i) {
        blockdata[i] = dekomprimera(reg.block[i].kodek, packade[i].data(),
                                    packade[i].size(), reg.block[i].ralen);
        if (blockdata[i].size() != reg.block[i].ralen)
            throw Fel("block " + std::to_string(i) + ": fel längd efter uppackning");
        Byte().swap(packade[i]);
    });

    // Kataloger först, i ordning, så att allt har någonstans att ta vägen.
    for (const Post &p : reg.poster) {
        if (p.t != Typ::Katalog) continue;
        if (reg.platt && p.p.empty()) continue;   // rotkatalogen är arkivets, inte målets
        std::string s = p.p.empty() ? rotut : rotut + "/" + p.p;
        if (mkdir(s.c_str(), 0700) != 0 && errno != EEXIST)
            throw Fel(s + ": går inte att skapa");
    }
    // Filerna skrivs PARALLELLT. Skälet är inte diskarna utan omvandlingarna:
    // varje JPEG återskapas av en arbetarprocess, och 1 143 foton i följd är
    // två minuter där 24 i bredd är åtta sekunder. Kataloger finns redan,
    // och två poster skriver aldrig samma fil, så det enda delade är blocken
    // — och de bara läses.
    std::vector<const Post*> filposter;
    for (const Post &q : reg.poster)
        if (q.t != Typ::Katalog) filposter.push_back(&q);
    parallellt(filposter.size(), jobb, [&](size_t fi) {
        const Post &p = *filposter[fi];
        std::string s = rotut + "/" + p.p;
        if (p.t == Typ::Lank) {
            unlink(s.c_str());
            if (symlink(p.mal.c_str(), s.c_str()) != 0) throw Fel(s + ": länken gick inte att skapa");
            satt_metadata(s, 0, p.mt, true);
        } else if (p.t == Typ::Fil) {
            if (p.o < 0 || (size_t)p.o >= reg.objekt.size()) throw Fel(s + ": posten pekar fel");
            const Objekt &o = reg.objekt[(size_t)p.o];
            const Byte &b = blockdata[o.blk];
            if (o.off + o.len > b.size()) throw Fel(s + ": objektet ligger utanför blocket");
            Byte hel;
            const u8 *data = b.data() + o.off;
            u64 len = o.len;
            if (!o.tr.empty()) {
                // Omvandlad: sy ihop originalet ur rest + kropp. Kropp ligger
                // i (blk,off,len), resten i (rblk,roff,rlen).
                const Byte &rb = blockdata[o.rblk];
                if (o.roff + o.rlen > rb.size()) throw Fel(s + ": resten ligger utanför blocket");
                Byte rest(rb.begin() + o.roff, rb.begin() + o.roff + o.rlen);
                Byte kropp(b.begin() + o.off, b.begin() + o.off + o.len);
                hel = (o.tr == "png") ? png_aterskapa(rest, kropp, o.recept)
                                      : jpeg_aterskapa(rest, kropp);
                data = hel.data(); len = hel.size();
            }
            skriv_fil(s, data, len);
            // Kontrollen är inte gratis, men ett arkiv som ger fel bytes utan
            // att säga något är värre än ett som är långsamt.
            u8 h[16];
            blake2b128(data, len, h);
            if (memcmp(h, o.h, reg.hashlangd)) throw Fel(s + ": innehållet stämmer inte med registret");
            satt_metadata(s, p.m, p.mt, false);
        }
    });
    // Katalogernas tider sist: att skriva i en katalog ändrar dess mtid.
    for (auto it = reg.poster.rbegin(); it != reg.poster.rend(); ++it) {
        if (it->t != Typ::Katalog) continue;
        if (reg.platt && it->p.empty()) continue;
        std::string s = it->p.empty() ? rotut : rotut + "/" + it->p;
        satt_metadata(s, it->m, it->mt, false);
    }
    double sek = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    sag(strf("  klart på %.1fs", sek));
}

u64 packa_fil(const std::string &fil, const std::string &arkiv, const Val &v,
              const std::string &namn) {
    // Filen hårdlänkas in i en tillfällig katalog bredvid arkivet och trädet
    // packas platt. En länk kostar ingenting; går den inte (annan
    // filsystemsgräns) kopieras filen, och då får det kosta.
    std::string katalog = arkiv.substr(0, arkiv.find_last_of('/') + 1);
    if (katalog.empty()) katalog = ".";
    char mall[4096];
    snprintf(mall, sizeof mall, "%s/nvz-en-XXXXXX", katalog.c_str());
    if (!mkdtemp(mall)) throw Fel("kunde inte skapa arbetsyta");
    std::string d = mall;
    std::string basnamn = namn.empty() ? fil.substr(fil.find_last_of('/') + 1) : namn;
    std::string mellan = d + "/" + basnamn;
    if (link(fil.c_str(), mellan.c_str()) != 0) {
        Byte inne = las_fil(fil);
        skriv_fil(mellan, inne.data(), inne.size());
        struct stat st;
        if (stat(fil.c_str(), &st) == 0)
            satt_metadata(mellan, st.st_mode & 07777,
                          (i64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec, false);
    }
    Val pv = v; pv.platt = true;
    u64 byte;
    try { byte = packa(d, arkiv, pv); }
    catch (...) { unlink(mellan.c_str()); rmdir(d.c_str()); throw; }
    unlink(mellan.c_str()); rmdir(d.c_str());
    return byte;
}

void packa_upp_fil(const std::string &arkiv, const std::string &mal, const Val &v) {
    // Packas upp bredvid målet och FLYTTAS på plats: målet finns antingen
    // inte alls eller färdigt, aldrig halvskrivet.
    std::string katalog = mal.substr(0, mal.find_last_of('/') + 1);
    if (katalog.empty()) katalog = ".";
    char mall[4096];
    snprintf(mall, sizeof mall, "%s/nvz-en-XXXXXX", katalog.c_str());
    if (!mkdtemp(mall)) throw Fel("kunde inte skapa arbetsyta");
    std::string d = mall;
    try {
        Val pv = v; pv.tyst = true;
        packa_upp(arkiv, d, pv);
        Register reg = las_register(arkiv);
        std::string namn = namn_i_arkiv(reg);
        if (namn.empty()) throw Fel(arkiv + ": inte ett enfilsarkiv");
        if (rename((d + "/" + namn).c_str(), mal.c_str()) != 0)
            throw Fel(mal + ": gick inte att flytta på plats");
    } catch (...) {
        std::string stad = "rm -rf '" + d + "'";
        if (system(stad.c_str()) != 0) {}
        throw;
    }
    std::string stad = "rm -rf '" + d + "'";
    if (system(stad.c_str()) != 0) {}
}

std::string namn_i_arkiv(const Register &reg) {
    if (!reg.platt) return "";
    for (const Post &p : reg.poster)
        if (p.t == Typ::Fil) return p.p;
    return "";
}

u64 uppackad_storlek(const Register &reg) {
    u64 tot = 0;
    for (const Post &p : reg.poster)
        if (p.t == Typ::Fil && p.o >= 0 && (size_t)p.o < reg.objekt.size())
            tot += reg.objekt[(size_t)p.o].s;
    return tot;
}

bool testa(const std::string &arkiv, const Val &v) {
    char mall[] = "/tmp/nvz-test-XXXXXX";
    if (!mkdtemp(mall)) throw Fel("kunde inte skapa arbetsyta");
    std::string d = mall;
    bool ok = true;
    try {
        Val pv = v; pv.tyst = true;
        packa_upp(arkiv, d, pv);      // varje fil kontrolleras mot sin hash
    } catch (const std::exception &) {
        ok = false;
    }
    std::string stad = "rm -rf '" + d + "'";
    if (system(stad.c_str()) != 0) {}
    return ok;
}

void lista(const std::string &arkiv) {
    Register reg = las_register(arkiv);
    printf("nvz v%u  rot=%s  skapad %s\n", reg.version, reg.rot.c_str(), reg.skapad.c_str());
    std::map<std::string, int> omv;
    for (const Objekt &o : reg.objekt) if (!o.tr.empty()) omv[o.tr]++;
    std::string omvtext;
    for (auto &kv : omv) omvtext += strf("%s'%s': %d", omvtext.empty() ? "" : ", ",
                                         kv.first.c_str(), kv.second);
    printf("%zu poster, %zu objekt, %zu block%s%s\n", reg.poster.size(), reg.objekt.size(),
           reg.block.size(), omvtext.empty() ? "" : ", omvandlade: {",
           omvtext.empty() ? "" : (omvtext + "}").c_str());
    for (const Block &b : reg.block)
        printf("  block %3u  %-11s %-7s %9.2f -> %8.2f MiB  x%.2f\n", b.i, b.klass.c_str(),
               b.kodek.c_str(), b.ralen / 1048576.0, b.clen / 1048576.0,
               b.clen ? (double)b.ralen / b.clen : 0.0);
}

}  // namespace nvz
