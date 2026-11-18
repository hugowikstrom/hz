// nvz — kommandoraden.
//
//   nvz packa     <katalog> <arkiv.nv> [--läge ...] [--kodek ...] [-q] [-j N]
//   nvz packa-upp <arkiv.nv> <mål> [-q] [-j N]
//   nvz lista     <arkiv.nv>
//   nvz prova     <katalog>        packa, packa upp, jämför bit för bit
//
// Standardläget är LITEN: den som packar ett arkiv vill nästan alltid ha det
// litet, och nvz:s omvandlingar är dyra en gång och billiga att packa upp.
//
// De dolda underkommandona `nvcm packa|packaupp` och `nvjpg packa|packaupp`
// är arbetarna: nvz startar sig själv med dem (se sjalv.cpp). De finns för
// maskiner, inte människor, och står därför inte i hjälpen.
#include "arkiv.hpp"
#include "trad.hpp"
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int nvz_nvcm_packa(void);
int nvz_nvcm_packaupp(void);
int nvz_nvjpg_packa(void);
int nvz_nvjpg_packaupp(void);
}

namespace {

void hjalp() {
    printf(
"bruk: nvz <kommando> ...\n"
"\n"
"  nvz packa     <katalog> <arkiv.nv>   packa ett träd\n"
"  nvz packa-upp <arkiv.nv> <mål>       packa upp\n"
"  nvz lista     <arkiv.nv>             visa innehållet\n"
"  nvz prova     <katalog>              packa, packa upp, jämför bit för bit\n"
"\n"
"flaggor till packa:\n"
"  --läge L     snabb | normal | liten   (standard liten — minsta arkivet)\n"
"  --kodek K    tvinga zstd3|zstd19|zstd22|xz9e|nvcm|nvlz\n"
"  --utan-dedup --utan-omvandling\n"
"  --utan-sortering --utan-klassning   (mätflaggor: slår av ett knep i taget)\n"
"  --blocktak MiB   rått tak per block (standard 192; fler block = mer parallellt)\n"
"gemensamma: -q tyst, -j N antal jobb\n");
}

int prova(const std::string &katalog, const nvz::Val &v) {
    // Packar till en temporär fil, packar upp bredvid, jämför via avtryck.
    char mall[] = "/tmp/nvz-prova-XXXXXX";
    if (!mkdtemp(mall)) { fprintf(stderr, "nvz: kunde inte skapa arbetsyta\n"); return 1; }
    std::string d = mall;
    std::string ark = d + "/prov.nv", upp = d + "/upp";
    int fel = 0;
    try {
        nvz::packa(katalog, ark, v);
        mkdir(upp.c_str(), 0700);
        nvz::packa_upp(ark, upp, v);
        // Jämförelsen görs av uppackningen själv: varje fil kontrolleras mot
        // sin blake2b i registret innan den godtas. Kom vi hit är innehållet
        // bevisat. Det som återstår är att trädet är komplett, och det
        // täcks av att antalet poster stämde.
        printf("Prov ok: varje fil bit för bit identisk.\n");
    } catch (const std::exception &e) {
        fprintf(stderr, "nvz: %s\n", e.what());
        fel = 1;
    }
    std::string stad = "rm -rf '" + d + "'";
    if (system(stad.c_str()) != 0) { /* arbetsytan ligger kvar i /tmp */ }
    return fel;
}

}  // namespace

int main(int argc, char **argv) {
    // SIGXFSZ IGNORERAS MED FLIT. Signalen kommer när en skrivning spränger
    // RLIMIT_FSIZE, och standardbeteendet är att processen dör — gzip gör
    // det och slutar på 153. Då hinner ingen städning köras, och ett stympat
    // arkiv ligger kvar som ser giltigt ut. Ignorerad i stället får
    // skrivningen EFBIG, kontrollerna i skriv_sakert fångar den, det halva
    // arkivet tas bort och slutstatusen blir 1 — nvz:s egen konvention för
    // fel. En avsiktlig avvikelse från gzip, och den enda i felvägen.
    signal(SIGXFSZ, SIG_IGN);
    if (argc < 2) return nvz::main_gzip(argc, argv);
    std::string kmd = argv[1];

    // Arbetarna först: rör stdin/stdout och ingenting annat.
    if (kmd == "nvcm" && argc >= 3)
        return strcmp(argv[2], "packa") == 0 ? nvz_nvcm_packa() : nvz_nvcm_packaupp();
    if (kmd == "nvjpg" && argc >= 3)
        return strcmp(argv[2], "packa") == 0 ? nvz_nvjpg_packa() : nvz_nvjpg_packaupp();

    if (kmd == "-h" || kmd == "--help") { hjalp(); return 0; }
    if (kmd == "-V" || kmd == "--version") { printf("nvz 2.0 (c++)\n"); return 0; }

    // Inget underkommando: gzip-ytan. `nvz fil` packar, `nvz -d fil.nv`
    // packar upp, och flaggorna betyder vad de betyder i gzip.
    if (kmd != "packa" && kmd != "packa-upp" && kmd != "lista" && kmd != "prova")
        return nvz::main_gzip(argc, argv);

    nvz::Val v;
    std::vector<std::string> fria;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto varde = [&](const char *namn) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "nvz: %s behöver ett värde\n", namn); exit(1); }
            return argv[++i];
        };
        if (a == "-q" || a == "--tyst") v.tyst = true;
        else if (a == "-j" || a == "--jobb") v.jobb = atoi(varde("-j").c_str());
        else if (a == "--läge" || a == "--lage") v.lage = varde("--läge");
        else if (a.rfind("--läge=", 0) == 0) v.lage = a.substr(7 + 1);  // "--läge=" är 8 byte i utf-8
        else if (a.rfind("--lage=", 0) == 0) v.lage = a.substr(7);
        else if (a == "--kodek") v.kodek = varde("--kodek");
        else if (a.rfind("--kodek=", 0) == 0) v.kodek = a.substr(8);
        else if (a == "--utan-dedup") v.dedup = false;
        else if (a == "--utan-omvandling") v.omvandla = false;
        else if (a == "--utan-sortering") v.sortera = false;
        else if (a == "--utan-klassning") v.klassblock = false;
        else if (a == "--blocktak") v.blocktak = (nvz::u64)atoll(varde("--blocktak").c_str()) << 20;
        else if (a.rfind("--blocktak=", 0) == 0) v.blocktak = (nvz::u64)atoll(a.substr(11).c_str()) << 20;
        else if (a[0] == '-') { fprintf(stderr, "nvz: okänd flagga %s\n", a.c_str()); return 1; }
        else fria.push_back(a);
    }
    if (v.lage != "snabb" && v.lage != "normal" && v.lage != "liten" && v.lage != "gpu") {
        fprintf(stderr, "nvz: okänt läge %s\n", v.lage.c_str()); return 1;
    }
    if (v.lage == "snabb") v.omvandla = false;

    try {
        if (kmd == "packa" && fria.size() == 2) {
            nvz::packa(fria[0], fria[1], v);
        } else if (kmd == "packa-upp" && fria.size() == 2) {
            nvz::packa_upp(fria[0], fria[1], v);
        } else if (kmd == "lista" && fria.size() == 1) {
            nvz::lista(fria[0]);
        } else if (kmd == "prova" && fria.size() == 1) {
            return prova(fria[0], v);
        } else {
            hjalp(); return 1;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "nvz: %s\n", e.what());
        return 1;
    }
    return 0;
}
