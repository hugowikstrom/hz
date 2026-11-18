// Arkivet: block plus register.
//
//   "NVPACK\x01" | block 0 | block 1 | ... | register | <u64 offset, u64 rålen>
//
// Behållaren är densamma som pythonversionens, med flit: ett arkiv som den
// skrivit ska gå att packa upp med den här. Skillnaden ligger i registret.
// Python skriver JSON; här skrivs ett binärt v2. Läsaren känner igen båda på
// första byten — '{' är v1, "NVZREG2" är v2.
#include "arkiv.hpp"
#include <set>
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
    // v3: tiden som epoksekunder i stället för ISO-sträng (20 byte -> 5).
    if (r.version >= 3) u.i64v(r.skapad_epok);
    else                u.strv(r.skapad);
    u.u32v((u32)r.block.size());
    for (const Block &b : r.block) {
        u.u32v(b.i); u.strv(b.klass); u.strv(b.kodek);
        u.u64v(b.off); u.u64v(b.clen); u.u64v(b.ralen);
    }
    u.u32v((u32)r.objekt.size());
    // HASHLÄNGDEN, och varför den krympte från 16 byte till 4.
    //
    // Hashen i registret har EN uppgift: `nvz -t` räknar om den ur den
    // uppackade filen och jämför, så att ett arkiv som monterats fel
    // upptäcks. Den uppgiften kräver inte 128 bitar. Bitfel i en
    // komprimerad ström fångas redan av blockens egna kontrollsummor
    // (CRC64 i xz, xxhash i zstd), och det hashen ska fånga är nvz:s egna
    // monteringsfel — förväxlade objekt, fel offset — där en avvikelse ger
    // olikhet med sannolikhet 1 - 2^-32.
    //
    // Priset för de tolv extra byten var högt, för hashar är slumpmässiga
    // och därmed inkomprimerbara: på kodkorpusen (5 898 objekt) var de
    // 94 368 byte av ett register på 239 049 — 1,1 % av hela arkivet, och
    // mer än hela avståndet upp till `xz -9e`.
    const size_t hl = r.version >= 3 ? 4 : 16;
    for (const Objekt &o : r.objekt) { u.ravv(o.h, hl); u.u64v(o.s); }
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
    if (r.version >= 3) {
        r.skapad_epok = in.i64v();
        char tid[32] = "";
        time_t t = (time_t)r.skapad_epok;
        struct tm tm{};
        // localtime, inte gmtime: v2 skrev lokal tid som sträng, och
        // `nvz lista` ska visa samma sak för båda formaten.
        if (localtime_r(&t, &tm)) strftime(tid, sizeof tid, "%Y-%m-%dT%H:%M:%S", &tm);
        r.skapad = tid;
    } else {
        r.skapad = in.strv();
    }
    u32 nb = in.u32v();
    r.block.resize(nb);
    for (Block &b : r.block) {
        b.i = in.u32v(); b.klass = in.strv(); b.kodek = in.strv();
        b.off = in.u64v(); b.clen = in.u64v(); b.ralen = in.u64v();
    }
    u32 no = in.u32v();
    r.objekt.resize(no);
    r.hashlangd = r.version >= 3 ? 4 : 16;
    for (Objekt &o : r.objekt) { in.ravv(o.h, r.hashlangd); o.s = in.u64v(); }
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
    // Vilken kompressor registret packades med avgörs av strömmens EGEN
    // magi, inte av versionsfältet — det ligger inuti registret och är
    // oläsbart innan registret är uppackat. xz börjar med FD 37 7A 58 5A 00.
    const bool reg_xz = packat.size() >= 6 && packat[0] == 0xFD && packat[1] == 0x37 &&
                        packat[2] == 0x7A && packat[3] == 0x58 && packat[4] == 0x5A &&
                        packat[5] == 0x00;
    Byte rad = dekomprimera(reg_xz ? "xz9e" : "zstd19", packat.data(), packat.size(), ralen);
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
    if (v.sortera)
        std::sort(ordning.begin(), ordning.end(), [&](u32 a, u32 b) {
            const Objekt &x = objekt[a], &y = objekt[b];
            if (x.klass != y.klass) return x.klass < y.klass;
            if (x.andelse != y.andelse) return x.andelse < y.andelse;
            // STORA filer först inom gruppen, resten i sökvägsordning.
            //
            // Den stora filen bygger ordboken som de mindre, liknande
            // filerna sedan matchar in i — känd heuristik i solida arkiv.
            // Mätt på zlib-ng/corpora: neuro är två volymer av samma
            // skanning, 38 och 19 MiB, och sökvägsordningen (19 före 38)
            // kostade 2,0 %.
            //
            // Men regeln får INTE gälla alla filer. Prövad på hela gruppen
            // kostade den 2,99 % på kodkorpusen, för ett projektträd har
            // redan sin likhet samlad per katalog — `.py` ligger med `.py` i
            // samma paket — och storleksordning river sönder den lokaliteten.
            // Tröskeln avgränsar regeln till de filer som har en mekanism:
            // bara en fil som är stor nog att själv fylla en märkbar del av
            // blockets fönster kan tjäna som underlag för de följande.
            // Därför en ÅTTONDEL AV BLOCKTAKET och inte ett fast tal — med
            // standardtaket 192 MiB blir det 24 MiB.
            //
            // Gränsen är mätt, inte gissad. Med 4 MiB kvalificerade
            // gif-korpusens största fil (7,4 MiB), hamnade först, ändrade
            // kodekprovets stickprov och kostade 0,95 % på den korpusen.
            // Med en åttondel av taket faller den utanför, medan neuros
            // 36 MiB-volym — fallet regeln finns för — ligger innanför.
            const u64 ORDBOKSBYGGARE = (v.blocktak ? v.blocktak : BLOCKTAK) / 8;
            const bool xs = x.s >= ORDBOKSBYGGARE, ys = y.s >= ORDBOKSBYGGARE;
            if (xs != ys) return xs;                 // stora före små
            if (xs && x.s != y.s) return x.s > y.s;  // störst först bland de stora
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
    // Taket styr både parallelliteten (ett block är en tråd) och kvoten (ett
    // block är kompressorns fönster). Opaka block undantas enligt 4.2 och
    // berörs inte av flaggan.
    const u64 tak = v.blocktak ? v.blocktak : BLOCKTAK;
    // UTVECKLAD BILDDATA delas finare i de snabba lägena. png-ra är PNG:ers
    // filterrader, och den klassen är särskild på tre sätt: den är den enda
    // som blåses upp (en PNG kan bli 150 gånger sin filstorlek, så ett träd
    // på 431 MiB blir 2 GiB block), dess likheter ligger mellan NÄRLIGGANDE
    // bilder och inte hundratals megabyte bort, och den är därför den klass
    // där blockdelning kostar minst kvot per sparad sekund. Mätt på
    // PNG-korpusen:
    //
    //     192 MiB block   261 s   218 956 236 byte
    //      64 MiB block   201 s   221 495 996   (1,30x snabbare, +1,16 %)
    //      32 MiB block   137 s   231 569 236   (1,90x snabbare, +5,76 %)
    //
    // Bästa etablerade på samma korpus är zstd −22 med 257 132 369 byte på
    // 156 s. Vid 32 MiB är nvz alltså fortfarande 9,9 % mindre OCH snabbare,
    // vilket `normal` finns för; `liten` behåller hela taket och tar de sista
    // 5,8 procenten. Delningen håller dessutom nere minnet: ett 32 MiB-block
    // får en 64 MiB-ordbok i stället för 256, och då ryms flera kodare
    // samtidigt i stället för att minnesgrinden serialiserar dem.
    const bool snabba_lagen = (v.lage != "liten");
    auto tak_for = [&](const std::string &klass) {
        if (klass == "opak") return (u64)16 << 30;
        if (klass == "png-ra" && snabba_lagen && !v.blocktak) return (u64)32 << 20;
        return tak;
    };
    auto lagg = [&](const std::string &klass_in, const u8 *d, size_t n) {
        // Med --utan-klassning hamnar allt i samma blockström. Objekten pekar
        // ut sitt innehåll med (block, offset, längd), och uppackningen läser
        // bara blockets KODEK och objektets omvandlingstagg — aldrig klassen —
        // så sammanslagningen är osynlig där. Bara kompressorns fönster
        // märker den. Provat bitexakt med png- och jpeg-omvandlade objekt i
        // samma block.
        //
        // En bieffekt att känna till när flaggan används till mätning: det
        // sammanslagna blocket heter "blandat", så tvåstegsregeln för opaka
        // block (rapportens 4.2, som nedan frågar på klassnamnet) slår aldrig
        // till. Det är avsiktligt — ablationen ska mäta nvz UTAN knep — men
        // det gör raden olämplig som referens för opak data.
        const std::string klass = v.klassblock ? klass_in : std::string("blandat");
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
    const int samtidiga = std::min<int>((int)block.size(), jobb);

    // KODEKPROVET GÖRS PER KLASS, INTE PER BLOCK.
    //
    // Provet kostar ett par sekunder (zstd19 och xz på 4 MiB), och det var
    // försumbart så länge ett träd gav en handfull block. När utvecklad
    // bilddata började delas vid 32 MiB blev det sextio block av samma slags
    // data, och sextio prov med samma svar tog över hälften av packtiden:
    // PNG-korpusen gick på 191 sekunder, varav ungefär 130 var prov.
    //
    // Klassen ÄR nvz:s svar på frågan "vad är det här för data" — den sätts
    // av innehållet, och alla block i en klass bär samma slags innehåll per
    // konstruktion. Därför provas det första blocket i varje klass, och
    // resten ärver svaret. Att det är en förenkling ska stå här: två block
    // av klassen `binar` kan i princip vilja ha olika kodek. Priset för att
    // pröva det vore att betala provet sextio gånger.
    std::map<std::string, std::pair<std::string, double>> provsvar;
    {
        std::vector<size_t> forsta;      // ett block per klass som behöver prov
        std::set<std::string> sedda;
        for (size_t k = 0; k < block.size(); k++) {
            const std::string kod = v.kodek.empty() ? block[k].b.kodek : v.kodek;
            if (kod.rfind("prova", 0) != 0) continue;
            if (!sedda.insert(block[k].b.klass).second) continue;
            forsta.push_back(k);
        }
        std::vector<std::pair<std::string, double>> svar(forsta.size());
        parallellt(forsta.size(), jobb, [&](size_t i) {
            Bygge &b = block[forsta[i]];
            const std::string kod = v.kodek.empty() ? b.b.kodek : v.kodek;
            double m = 1.0;
            std::string vald = valj_kodek(b.ra.data(), b.ra.size(), kod == "prova-cm",
                                          kod == "prova-gpu", per_block,
                                          b.b.klass == "opak", &m);
            svar[i] = {vald, m};
        });
        for (size_t i = 0; i < forsta.size(); i++)
            provsvar[block[forsta[i]].b.klass] = svar[i];
    }

    parallellt(block.size(), jobb, [&](size_t k) {
        Bygge &b = block[k];
        std::string kod = v.kodek.empty() ? b.b.kodek : v.kodek;
        double marginal = 1.0;      // 1.0 = ingen aning (ingen prövning gjord)
        if (kod.rfind("prova", 0) == 0) {
            auto it = provsvar.find(b.b.klass);
            if (it != provsvar.end()) { kod = it->second.first; marginal = it->second.second; }
            else kod = valj_kodek(b.ra.data(), b.ra.size(), kod == "prova-cm",
                                  kod == "prova-gpu", per_block,
                                  b.b.klass == "opak", &marginal);
        }
        if (kod == "opak-eskalera") {
            // TVÅ FRÅGOR, OCH DEN FÖRSTA SKA VARA BILLIG.
            //
            // Stickprovet kan inte skilja film från arkiv-av-arkiv: båda är
            // entropikodade och lokalt täta i 4 MiB (rapport §4.2). Men
            // frågan "har det här blocket tiotals procent att ge?" behöver
            // ingen stark kompressor för att besvaras — den behöver bara en
            // ärlig körning över HELA blocket. Mätt på gif-korpusen:
            //
            //     zstd −3     14 983 720 byte      68 ms
            //     zstd −19    14 868 267 byte   4 388 ms
            //     zstd −22    14 867 123 byte   3 856 ms
            //
            // zstd −3 ser 47,8 % av de 48 som finns att hämta, på en
            // sextiondel av tiden — men BARA när redundansen är lokal. På
            // arkiv-av-arkiv ligger den hundratals megabyte isär, och där
            // såg `zstd −3` med sitt megabytefönster bara 2,6 % av de 33
            // som fanns: eskaleringen uteblev och arkivet blev 271 MB i
            // stället för 248. Steget är därför `zstd3L` — samma nivå med
            // långdistansmatchning, 27,6 % på samma 725 ms. Det
            // kostar nästan ingenting och svarar på frågan ändå. (Innan den
            // här ändringen var första steget zstd −19, vilket gjorde att
            // nvz betalade 4,4 sekunder för ett svar som kostar 68
            // millisekunder — och det var skillnaden mellan att vinna och
            // förlora mot zstd −22 på tid.)
            kod = "zstd3L";
            packade[k] = komprimera(kod, b.ra.data(), b.ra.size(), per_block, samtidiga);
            if ((double)packade[k].size() < (double)b.ra.size() * 0.90) {
                // Det finns likheter mellan filer. Då är det fönstret som
                // avgör, och zstd −22 med 2 GiB fönster är rätt kandidat;
                // zstd −19 är dominerad och hoppas över.
                Byte dyr = komprimera("zstd22", b.ra.data(), b.ra.size(), per_block, samtidiga);
                if (dyr.size() < packade[k].size()) {
                    packade[k] = std::move(dyr);
                    kod = "zstd22";
                }
                // Bara i läget `liten`: LZMA med en ordbok som täcker hela
                // blocket. Den kandidaten är den enda vars fönster når över
                // ett helt opakt block, och det var exakt den form 7-Zip
                // slog nvz med i rapportens 3.6. Steget är en hel extra
                // packning, och på arkivkorpusen är det värt 2,3 % — men
                // bara i det läge som finns för att bli minst.
                if (v.lage == "liten") {
                    std::string variant = xz_variant(b.ra.data(), b.ra.size());
                    Byte lz = komprimera(variant, b.ra.data(), b.ra.size(), per_block, samtidiga);
                    if (lz.size() < packade[k].size()) {
                        packade[k] = std::move(lz);
                        kod = variant;
                    }
                }
            } else {
                // Filmlikt: en halv procent att hämta, inte tiotals. Där är
                // zstd −19 ändå den som tar promillena, och den är både
                // billigare och bättre än zstd −22 på sådan data (film:
                // 557 863 118 byte på 56 s mot 557 873 791 på 264 s).
                Byte z19 = komprimera("zstd19", b.ra.data(), b.ra.size(), per_block, samtidiga);
                if (z19.size() < packade[k].size()) {
                    packade[k] = std::move(z19);
                    kod = "zstd19";
                }
            }
        } else {
            packade[k] = komprimera(kod, b.ra.data(), b.ra.size(), per_block, samtidiga);
            // STICKPROVETS BLINDA FLÄCK, och varför den kostar procent.
            //
            // Provet tar 4 MiB som åtta lösryckta bitar ur blocket. Det är
            // rätt konstruktion för att se vad datan ÄR (rapportens 2.2:
            // ett block är sorterat, så de första megabyten är en enda
            // filtyp och säger inget om resten) — men det river sönder
            // precis den långdistansstruktur som skiljer kodekerna åt. Åtta
            // bitar ur skilda delar av blocket har inga upprepningar mellan
            // sig, och då ser provet inte vad en ordbok som täcker hela
            // blocket kan göra. Mätt på ett 120 MiB-block ur arkivkorpusen:
            // provet valde zstd19 och fick 100,13 MiB, medan xz med
            // täckande ordbok gav 91,97 — provet missade 8 procent.
            //
            // En billig förhandssignal provades och underkändes: zstd −3:s
            // kvot på provet mot samma kvot på hela blocket skiljer sig åt
            // (1,016 mot 1,025 på två block) men rangordnar dem fel, så den
            // kan inte ersätta en mätning.
            //
            // Därför: i läget `liten`, där storleken går före tiden, provas
            // xz med täckande ordbok på HELA blocket när provet valde något
            // annat än xz. Villkoren håller kostnaden nere — valde provet
            // redan xz jämförs ingenting, och små block (där provet nästan
            // ÄR blocket) hoppas över.
            // ... men bara när provets försprång var TUNT. Mätt: på text
            // vinner nvcm provet med tiotals procent och vinner också hela
            // blocket, så verifieringen kostar en full xz-packning och ger
            // ingenting — enwik8 gick från 131 till 259 sekunder utan en
            // enda sparad byte. Där rangordningen faktiskt vände (blocket ur
            // arkivkorpusen, 8,2 %) var provets försprång under en procent.
            // Tröskeln står vid åtta procent, med marginal åt det säkra.
            if (v.lage == "liten" && kod.rfind("xz9e", 0) != 0 && kod != "ra"
                && marginal < 0.08 && b.ra.size() > (16u << 20)) {
                std::string variant = xz_variant(b.ra.data(), b.ra.size());
                Byte lz = komprimera(variant, b.ra.data(), b.ra.size(), per_block, samtidiga);
                if (lz.size() < packade[k].size()) {
                    packade[k] = std::move(lz);
                    kod = variant;
                }
            }
        }
        b.b.kodek = kod;
        Byte().swap(b.ra);
    });

    // --- 6. skriv arkivet ---------------------------------------------------
    FILE *ut = fopen(arkiv.c_str(), "wb");
    if (!ut) throw Fel(arkiv + ": går inte att skriva");
    // Varje skrivning kontrolleras, och ett arkiv som inte blev färdigt
    // tas bort i stället för att ligga kvar som ett stympat arkiv som ser
    // giltigt ut. Se kommentaren vid skriv_sakert i verktyg.cpp: gzip-ytan
    // raderar källan efter en lyckad packning, så "lyckad" måste betyda att
    // byten ligger på disk.
    struct Stad {
        const std::string &namn; FILE *f; bool klar = false;
        ~Stad() { if (!klar) { if (f) fclose(f); unlink(namn.c_str()); } }
    } stad{arkiv, ut};
    skriv_sakert(ut, MAGI, sizeof MAGI, arkiv);
    u64 pos = sizeof MAGI;
    for (size_t k = 0; k < block.size(); k++) {
        block[k].b.off = pos;
        block[k].b.clen = packade[k].size();
        skriv_sakert(ut, packade[k].data(), packade[k].size(), arkiv);
        pos += packade[k].size();
        Byte().swap(packade[k]);
    }
    Register reg;
    reg.version = 3;                 // v3: kort hash, epoktid, valbar registerkodek
    reg.rot = rot.substr(rot.find_last_of('/') + 1);
    reg.platt = v.platt;
    time_t nu = time(nullptr);
    reg.skapad_epok = (i64)nu;
    char tid[64];
    strftime(tid, sizeof tid, "%Y-%m-%dT%H:%M:%S", localtime(&nu));
    reg.skapad = tid;
    for (Bygge &b : block) reg.block.push_back(b.b);
    reg.objekt = objekt;
    reg.poster = poster;
    Byte rad = skriv_register(reg);
    // Registret är text och tal och packar sig bättre med xz än med zstd —
    // på kodkorpusen 227 048 mot 239 053 byte. Men xz:s container är ~60
    // byte mot zstds ~13, så på ett litet arkiv förlorar xz på sin egen
    // ram. Därför provas båda och den minsta vinner; uppackningen känner
    // igen vilken det blev på strömmens magi.
    Byte regz = komprimera("zstd19", rad.data(), rad.size(), jobb);
    Byte regx = komprimera("xz9e", rad.data(), rad.size(), jobb);
    const Byte &regp = regx.size() < regz.size() ? regx : regz;
    skriv_sakert(ut, regp.data(), regp.size(), arkiv);
    u8 svans[16];
    for (int i = 0; i < 8; i++) svans[i] = (u8)(pos >> (8*i));
    for (int i = 0; i < 8; i++) svans[8+i] = (u8)(rad.size() >> (8*i));
    skriv_sakert(ut, svans, 16, arkiv);
    stang_sakert(ut, arkiv, true);
    stad.f = nullptr; stad.klar = true;

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
