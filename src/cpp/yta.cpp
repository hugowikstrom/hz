// gzip-ytan: nvz utan underkommando beter sig som gzip.
//
// Ytan är gzips därför att varje skript, Makefile och vana som redan finns
// talar den. Löftena som provsviten håller den mot:
//
//   * `nvz fil` ersätter filen med fil.nv, och arkivet ÄRVER tid och
//     rättigheter. `nvz -d` ger tillbaka exakt samma bytes och städar bort
//     arkivet. -c och -k rör ingenting.
//   * En katalog som operand blir ETT arkiv av trädet (källträdet kvar);
//     med -r packas i stället varje fil för sig, som gzip bokstavligen gör.
//   * Slutstatus skiljer fel (1) från varning (2). Skript hänger på det.
//   * Ett trädarkiv vägrar skrivas till standard ut. Tusen filer är inte
//     en byteström.
#include "arkiv.hpp"
#include "trad.hpp"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>

namespace nvz {
namespace {

struct Flaggor {
    bool d=false,c=false,k=false,f=false,t=false,l=false,r=false,q=false,N=false;
    int v=0;
    std::string suffix = ".nv";
    Val val;                         // läge är "liten" som standard — minsta arkivet
};

int fel(const std::string &m) { fprintf(stderr, "nvz: %s\n", m.c_str()); return 1; }
int varna(const Flaggor &o, const std::string &m) {
    if (!o.q) fprintf(stderr, "nvz: %s\n", m.c_str());
    return 2;
}

// Nivåsiffrorna mappas på lägena. Till skillnad från gzip är STANDARDEN den
// minsta: den som packar ett arkiv vill nästan alltid ha det litet, och
// nvz:s omvandlingar är dyra en gång och billiga att packa upp.
const char *niva_lage(int n) { return n <= 3 ? "snabb" : n <= 6 ? "normal" : "liten"; }

struct Langflagga { const char *namn; char kort; bool tar_varde; };
const Langflagga LANGA[] = {
    {"stdout",'c',false},{"to-stdout",'c',false},{"decompress",'d',false},
    {"uncompress",'d',false},{"force",'f',false},{"help",'h',false},{"keep",'k',false},
    {"list",'l',false},{"license",'L',false},{"no-name",'n',false},{"name",'N',false},
    {"quiet",'q',false},{"silent",'q',false},{"recursive",'r',false},{"suffix",'S',true},
    {"test",'t',false},{"verbose",'v',false},{"version",'V',false},
    {"fast",'1',false},{"best",'9',false},{"tyst",'q',false},
    {"läge",'M',true},{"lage",'M',true},{"kodek",'K',true},{"jobb",'j',true},
    {"utan-dedup",'D',false},{"utan-omvandling",'O',false},
    {"synchronous",0,false},{"rsyncable",0,false},{"ascii",0,false},{"no-time",0,false},
};

void hjalp_yta() {
    printf(
"bruk: nvz [FLAGGA]... [FIL]...\n"
"Packar och packar upp filer och katalogträd. Utan flaggor packas varje FIL\n"
"och originalet ersätts, precis som gzip — men standardläget är `liten`,\n"
"det minsta nvz kan. En katalog blir ETT arkiv av hela trädet.\n"
"\n"
"  -c, --stdout       skriv till standard ut, lämna filerna i fred\n"
"  -d, --decompress   packa upp\n"
"  -f, --force        skriv över mål, följ länkar, packa om redan packat\n"
"  -k, --keep         behåll filen som packades\n"
"  -l, --list         visa vad arkiven innehåller\n"
"  -q, --quiet        säg ingenting\n"
"  -r, --recursive    gå ner i kataloger och packa varje fil för sig\n"
"  -S, --suffix=.SUF  ändelse på arkiven (standard .nv)\n"
"  -t, --test         prova arkivet: packa upp och jämför mot registret\n"
"  -v, --verbose      berätta vad som händer\n"
"  -1 .. -9           snabbt .. litet: -1..-3 snabb, -4..-6 normal, -7..-9 liten\n"
"                     (utan siffra: liten, alltså det minsta nvz kan)\n"
"      --läge=L       snabb | normal | liten (slår -1..-9; standard liten)\n"
"      --kodek=K      tvinga zstd3 | zstd19 | zstd22 | xz9e | nvcm | nvlz\n"
"      --utan-dedup --utan-omvandling -j N\n"
"\n"
"Underkommandon: nvz packa|packa-upp|lista|prova — se `nvz packa --help`.\n");
}

// Returnerar 0 vid ok, annars slutstatus. *behover_varde* ut: flaggan tog
// nästa argument som värde.
int tillampa(Flaggor &o, char kort, const char *varde) {
    switch (kort) {
    case 'c': o.c = true; break;         case 'd': o.d = true; break;
    case 'f': o.f = true; break;         case 'k': o.k = true; break;
    case 'l': o.l = true; break;         case 't': o.t = true; break;
    case 'q': o.q = true; o.v = 0; break;
    case 'r': o.r = true; break;         case 'v': o.v++; break;
    case 'n': o.N = false; break;        case 'N': o.N = true; break;
    case 'h': hjalp_yta(); exit(0);
    case 'V': printf("nvz 2.0 (c++)\n"); exit(0);
    case 'L': printf("nvz: egen kod, inga villkor utöver källträdets.\n"); exit(0);
    case 'S': o.suffix = varde && varde[0] == '.' ? varde : std::string(".") + (varde ? varde : ""); break;
    case 'M': o.val.lage = varde ? varde : ""; break;
    case 'K': o.val.kodek = varde ? varde : ""; break;
    case 'j': o.val.jobb = varde ? atoi(varde) : 0; break;
    case 'D': o.val.dedup = false; break;
    case 'O': o.val.omvandla = false; break;
    case 0: break;                       // tas emot och gör ingenting
    default:
        if (kort >= '1' && kort <= '9') { o.val.lage = niva_lage(kort - '0'); break; }
        return 1;
    }
    return 0;
}

bool ar_katalog(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool ar_lank(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}
bool finns(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}
bool slutar_med(const std::string &s, const std::string &andelse) {
    return s.size() >= andelse.size() &&
           s.compare(s.size() - andelse.size(), andelse.size(), andelse) == 0;
}

bool far_skriva(const Flaggor &o, const std::string &mal, int *status) {
    if (o.f || !finns(mal)) return true;
    if (isatty(0) && isatty(2)) {
        fprintf(stderr, "nvz: %s finns redan; skriva över (j/n)? ", mal.c_str());
        char rad[16] = {0};
        if (fgets(rad, sizeof rad, stdin) && (rad[0] == 'j' || rad[0] == 'y')) return true;
        fprintf(stderr, "\tej överskrivet\n");
        *status = std::max(*status, 2);
        return false;
    }
    Flaggor tystare = o;
    *status = std::max(*status, varna(tystare, mal + " finns redan -- oförändrat"));
    return false;
}

void till_stdout(const std::string &p) {
    Byte d = las_fil(p);
    fwrite(d.data(), 1, d.size(), stdout);
    fflush(stdout);
}

void kvot(const Flaggor &o, const std::string &fran, u64 ra, u64 packat,
          const std::string &till, bool ersatt) {
    if (o.v < 1 || o.q) return;
    double del = ra ? (1.0 - (double)packat / ra) * 100.0 : 0.0;
    fprintf(stderr, "%s:\t%5.1f%% -- %s %s\n", fran.c_str(), del,
            ersatt ? "ersatt med" : "skrivet till", till.c_str());
}

// Med -r byts en katalog mot filerna i den. I svepet hoppas filer med fel
// ändelse över UTAN ett ord — ett `nvz -r` som skäller en rad per redan
// packad fil är obrukbart. Namnger man en fil själv får man varningen.
void utvidga(const Flaggor &o, const std::vector<std::string> &filer,
             bool vill_ha_arkiv, std::vector<std::string> &ut) {
    for (const std::string &f : filer) {
        if (o.r && ar_katalog(f) && !ar_lank(f)) {
            std::vector<std::string> ko = {f};
            while (!ko.empty()) {
                std::string kat = ko.front(); ko.erase(ko.begin());
                DIR *d = opendir(kat.c_str());
                if (!d) continue;
                std::vector<std::string> namn;
                for (struct dirent *e; (e = readdir(d)); ) {
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                        namn.push_back(e->d_name);
                }
                closedir(d);
                std::sort(namn.begin(), namn.end());
                for (const std::string &nm : namn) {
                    std::string s = kat + "/" + nm;
                    if (ar_lank(s)) continue;
                    if (ar_katalog(s)) { ko.push_back(s); continue; }
                    if (slutar_med(nm, o.suffix) != vill_ha_arkiv) continue;
                    ut.push_back(s);
                }
            }
        } else {
            ut.push_back(f);
        }
    }
}

u64 tradstorlek(const std::string &rot) {
    u64 tot = 0;
    std::vector<std::string> ko = {rot};
    while (!ko.empty()) {
        std::string kat = ko.front(); ko.erase(ko.begin());
        DIR *d = opendir(kat.c_str());
        if (!d) continue;
        for (struct dirent *e; (e = readdir(d)); ) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string s = kat + "/" + e->d_name;
            struct stat st;
            if (lstat(s.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) ko.push_back(s);
            else if (S_ISREG(st.st_mode)) tot += (u64)st.st_size;
        }
        closedir(d);
    }
    return tot;
}

// --- packa -----------------------------------------------------------------
int packa_operand(const Flaggor &o, const std::string &p) {
    if (!finns(p)) return fel(p + ": finns inte");
    std::string kalla = p;
    if (ar_lank(p)) {
        if (!o.f) return varna(o, p + " är en symlänk -- hoppas över");
        char riktig[4096];
        if (!realpath(p.c_str(), riktig)) return fel(p + ": länken pekar ingenstans");
        kalla = riktig;
    }
    if (ar_katalog(kalla)) {
        // ETT arkiv av trädet, och källträdet lämnas kvar.
        u64 ra = o.v ? tradstorlek(kalla) : 0;
        if (o.c) {
            char mall[] = "/tmp/nvz-ut-XXXXXX";
            if (!mkdtemp(mall)) return fel("kunde inte skapa arbetsyta");
            std::string ark = std::string(mall) + "/ut.nv";
            int status = 0;
            try { packa(kalla, ark, o.val); kvot(o, p, ra, filstorlek_(ark), "standard ut", false); till_stdout(ark); }
            catch (const std::exception &e) { status = fel(e.what()); }
            unlink(ark.c_str()); rmdir(mall);
            return status;
        }
        std::string ut = p + o.suffix;
        int status = 0;
        if (!far_skriva(o, ut, &status)) return status;
        packa(kalla, ut, o.val);
        kvot(o, p, ra, filstorlek_(ut), ut, false);
        return 0;
    }
    struct stat st;
    if (lstat(kalla.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return varna(o, p + " är inte en vanlig fil -- hoppas över");
    if (st.st_nlink > 1 && !(o.f || o.k || o.c))
        return varna(o, p + strf(" har %lu annan länk -- oförändrad", (unsigned long)(st.st_nlink - 1)));
    if (slutar_med(p, o.suffix) && !o.f)
        return varna(o, p + " har redan ändelsen " + o.suffix + " -- oförändrad");
    u64 ra = (u64)st.st_size;
    if (o.c) {
        char mall[] = "/tmp/nvz-ut-XXXXXX";
        if (!mkdtemp(mall)) return fel("kunde inte skapa arbetsyta");
        std::string ark = std::string(mall) + "/ut.nv";
        int status = 0;
        try { packa_fil(kalla, ark, o.val); kvot(o, p, ra, filstorlek_(ark), "standard ut", false); till_stdout(ark); }
        catch (const std::exception &e) { status = fel(e.what()); }
        unlink(ark.c_str()); rmdir(mall);
        return status;
    }
    std::string ut = p + o.suffix;
    int status = 0;
    if (!far_skriva(o, ut, &status)) return status;
    packa_fil(kalla, ut, o.val);
    // Arkivet ärver filens tid och rättigheter, som gzip.
    satt_metadata(ut, st.st_mode & 07777,
                  (i64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec, false);
    if (!o.k) unlink(p.c_str());
    kvot(o, p, ra, filstorlek_(ut), ut, !o.k);
    return 0;
}

// --- packa upp -------------------------------------------------------------
int packa_upp_operand(const Flaggor &o, const std::string &p_in) {
    std::string p = p_in;
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        std::string med = p + o.suffix;                // `nvz -d fil` hittar fil.nv
        if (finns(p) || stat(med.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return fel(p + ": finns inte");
        p = med;
    }
    if (!slutar_med(p, o.suffix) && !(o.c || o.f))
        return varna(o, p + ": okänd ändelse -- hoppas över");
    Register reg;
    try { reg = las_register(p); }
    catch (const std::exception &e) { return fel(p + ": " + e.what()); }

    if (reg.platt) {
        std::string namn = namn_i_arkiv(reg);
        std::string mal = slutar_med(p, o.suffix)
            ? p.substr(0, p.size() - o.suffix.size())
            : (o.N && !namn.empty() ? p.substr(0, p.find_last_of('/') + 1) + namn : "");
        if (o.N && !namn.empty())
            mal = p.substr(0, p.find_last_of('/') + 1) + namn;
        if (mal.empty()) return varna(o, p + ": okänd ändelse -- hoppas över");
        if (o.c) {
            char mall[] = "/tmp/nvz-ut-XXXXXX";
            if (!mkdtemp(mall)) return fel("kunde inte skapa arbetsyta");
            std::string tmp = std::string(mall) + "/f";
            int status = 0;
            try { packa_upp_fil(p, tmp, o.val); till_stdout(tmp); }
            catch (const std::exception &e) { status = fel(e.what()); }
            unlink(tmp.c_str()); rmdir(mall);
            return status;
        }
        int status = 0;
        if (!far_skriva(o, mal, &status)) return status;
        u64 c = filstorlek_(p);
        packa_upp_fil(p, mal, o.val);
        kvot(o, p, uppackad_storlek(reg), c, mal, !o.k);
        if (!o.k) unlink(p.c_str());
        return 0;
    }
    // Ett träd. Det går inte att hälla ut i en byteström, och det ska inte
    // heller gissas fram — ett arkiv av tusen filer är inte en fil.
    if (o.c)
        return fel(p + ": arkivet innehåller ett träd (" + reg.rot +
                   "), det kan inte skrivas till standard ut");
    std::string foralder = p.find('/') == std::string::npos ? "." : p.substr(0, p.find_last_of('/'));
    std::string rot = foralder + "/" + reg.rot;
    int status = 0;
    if (!far_skriva(o, rot, &status)) return status;
    u64 c = filstorlek_(p);
    Val pv = o.val; pv.tyst = (o.v < 2);
    packa_upp(p, foralder, pv);
    kvot(o, p, uppackad_storlek(reg), c, rot, !o.k);
    if (!o.k) unlink(p.c_str());
    return 0;
}

// --- lista och prova -------------------------------------------------------
int lista_operand(const Flaggor &o, const std::string &p, u64 *sc, u64 *su, int *sn) {
    Register reg;
    try { reg = las_register(p); }
    catch (const std::exception &e) { return fel(p + ": " + e.what()); }
    u64 c = filstorlek_(p), u = uppackad_storlek(reg);
    std::string namn = namn_i_arkiv(reg);
    if (namn.empty()) namn = reg.rot + "/";
    printf("%13llu %13llu %6.1f%%  %s\n", (unsigned long long)c, (unsigned long long)u,
           (1.0 - (double)c / std::max<u64>(u, 1)) * 100.0, namn.c_str());
    *sc += c; *su += u; (*sn)++;
    if (o.v) lista(p);
    return 0;
}

int testa_operand(const Flaggor &o, const std::string &p) {
    bool ok;
    try { ok = testa(p, o.val); }
    catch (const std::exception &e) { return fel(p + ": provet gick inte att köra (" + std::string(e.what()) + ")"); }
    if (!ok) return fel(p + ": PROVET MISSLYCKADES");
    if (o.v || isatty(1)) printf("%s:\tOK\n", p.c_str());
    return 0;
}

// --- standard in -> standard ut --------------------------------------------
int strom(const Flaggor &o) {
    if (!o.d && !o.t && !o.l && isatty(1) && !o.f)
        return fel("packade data skrivs inte till en terminal. "
                   "Använd -f om du menar det, eller ange en fil.");
    char mall[] = "/tmp/nvz-strom-XXXXXX";
    if (!mkdtemp(mall)) return fel("kunde inte skapa arbetsyta");
    std::string d = mall;
    std::string inne = d + "/in";
    {
        FILE *u = fopen(inne.c_str(), "wb");
        if (!u) { rmdir(mall); return fel("kunde inte skriva arbetsytan"); }
        u8 buf[1 << 16];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, stdin)) > 0) fwrite(buf, 1, r, u);
        fclose(u);
    }
    int status = 0;
    try {
        if (o.d || o.t || o.l) {
            Register reg = las_register(inne);
            if (o.l) {
                u64 sc = 0, su = 0; int sn = 0;
                printf("%13s %13s   kvot  namn\n", "packat", "uppackat");
                status = lista_operand(o, inne, &sc, &su, &sn);
            } else if (o.t) {
                status = testa_operand(o, inne);
            } else if (!reg.platt) {
                status = fel("arkivet innehåller ett träd och kan inte packas upp "
                             "till standard ut — ge arkivet som fil");
            } else {
                std::string ut = d + "/ut";
                packa_upp_fil(inne, ut, o.val);
                till_stdout(ut);
                unlink(ut.c_str());
            }
        } else {
            std::string ark = d + "/ut.nv";
            packa_fil(inne, ark, o.val, "standard-in");
            till_stdout(ark);
            unlink(ark.c_str());
        }
    } catch (const std::exception &e) {
        status = fel(e.what());
    }
    unlink(inne.c_str());
    std::string stad = "rm -rf '" + d + "'";
    if (system(stad.c_str()) != 0) {}
    return status;
}

}  // namespace

u64 filstorlek_(const std::string &p) {
    struct stat st;
    if (stat(p.c_str(), &st) != 0) throw Fel(p + ": finns inte");
    return (u64)st.st_size;
}

int main_gzip(int argc, char **argv) {
    Flaggor o;
    std::vector<std::string> filer;
    bool bara_filer = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (bara_filer || a == "-" || a[0] != '-') { filer.push_back(a); continue; }
        if (a == "--") { bara_filer = true; continue; }
        if (a.size() > 1 && a[1] == '-') {
            std::string namn = a.substr(2), varde;
            bool har_varde = false;
            size_t lika = namn.find('=');
            if (lika != std::string::npos) { varde = namn.substr(lika + 1); namn = namn.substr(0, lika); har_varde = true; }
            // Otvetydiga förkortningar godtas; tvetydiga avvisas. Skript som
            // skriver --l idag kan mena fyra olika saker imorgon.
            std::vector<const Langflagga*> traff;
            for (const Langflagga &lf : LANGA) {
                if (namn == lf.namn) { traff = {&lf}; break; }
                if (strncmp(lf.namn, namn.c_str(), namn.size()) == 0) traff.push_back(&lf);
            }
            if (traff.empty()) return fel("okänd flagga -- '--" + namn + "'\nFörsök med nvz --help.");
            if (traff.size() > 1) {
                std::string alternativ;
                for (auto *lf : traff) alternativ += std::string(alternativ.empty() ? "" : ", ") + "--" + lf->namn;
                return fel("tvetydig flagga -- '--" + namn + "' (" + alternativ + ")");
            }
            const Langflagga &lf = *traff[0];
            if (lf.tar_varde && !har_varde) {
                if (i + 1 >= argc) return fel("--" + std::string(lf.namn) + " behöver ett värde");
                varde = argv[++i];
            }
            if (tillampa(o, lf.kort, lf.tar_varde ? varde.c_str() : nullptr))
                return fel("okänd flagga -- '--" + namn + "'");
            continue;
        }
        for (size_t j = 1; j < a.size(); j++) {
            char ch = a[j];
            if (ch == 'S' || ch == 'j') {
                std::string varde = a.substr(j + 1);
                if (varde.empty()) {
                    if (i + 1 >= argc) return fel(strf("-%c behöver ett värde", ch));
                    varde = argv[++i];
                }
                tillampa(o, ch, varde.c_str());
                break;
            }
            if (tillampa(o, ch, nullptr))
                return fel(strf("okänd flagga -- '%c'\nFörsök med nvz --help.", ch));
        }
    }
    if (o.val.lage != "snabb" && o.val.lage != "normal" && o.val.lage != "liten" && o.val.lage != "gpu")
        return fel("okänt läge " + o.val.lage);
    if (o.val.lage == "snabb") o.val.omvandla = false;
    o.val.tyst = (o.v < 2);

    if (filer.empty()) return strom(o);

    int status = 0;
    if (o.l) {
        u64 sc = 0, su = 0; int sn = 0;
        if (!o.q) printf("%13s %13s   kvot  namn\n", "packat", "uppackat");
        std::vector<std::string> alla;
        utvidga(o, filer, true, alla);
        for (const std::string &p : alla) status = std::max(status, lista_operand(o, p, &sc, &su, &sn));
        if (sn > 1)
            printf("%13llu %13llu %6.1f%%  (summa)\n", (unsigned long long)sc,
                   (unsigned long long)su, (1.0 - (double)sc / std::max<u64>(su, 1)) * 100.0);
        return status;
    }
    if (o.t) {
        std::vector<std::string> alla;
        utvidga(o, filer, true, alla);
        for (const std::string &p : alla) status = std::max(status, testa_operand(o, p));
        return status;
    }
    std::vector<std::string> alla;
    utvidga(o, filer, o.d, alla);
    for (const std::string &p : alla) {
        int s = o.d ? packa_upp_operand(o, p) : packa_operand(o, p);
        status = std::max(status, s);
    }
    return status;
}

}  // namespace nvz
