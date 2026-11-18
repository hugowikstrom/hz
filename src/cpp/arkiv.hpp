#pragma once
#include "nvz.hpp"

namespace nvz {

struct Val {
    std::string lage = "liten";     // snabb | normal | liten
    bool dedup = true;
    bool omvandla = true;
    // Mätflaggor: de finns för att kunna isolera VARJE knep för sig i en
    // ablation (rapportens avsnitt om förarbete). Sorteringen och
    // klassindelningen är två skilda effekter som annars mäts ihop.
    bool sortera = true;            // lägg lika filer intill varandra
    bool klassblock = true;         // ett block per innehållsklass
    u64 blocktak = 0;               // 0 = BLOCKTAK; annars rått tak per block
    int jobb = 0;                   // 0 = antal kärnor
    bool tyst = false;
    std::string kodek;              // tvingad kodek, tom = prova fram
    bool platt = false;             // enfilsarkiv: roten är osynlig vid uppackning
};

u64 packa(const std::string &rot, const std::string &arkiv, const Val &v);
void packa_upp(const std::string &arkiv, const std::string &mal, const Val &v);
void lista(const std::string &arkiv);
Register las_register(const std::string &arkiv);

// Enfilsarkiven bakom gzip-ytan: en fil blir ett "platt" arkiv — samma
// format, men registret är märkt så att uppackningen inte skapar någon
// rotkatalog. Ett stort arkiv betalar noll byte för det.
u64 packa_fil(const std::string &fil, const std::string &arkiv, const Val &v,
              const std::string &namn = "");
void packa_upp_fil(const std::string &arkiv, const std::string &mal, const Val &v);
std::string namn_i_arkiv(const Register &reg);   // tom sträng = trädarkiv
u64 uppackad_storlek(const Register &reg);
bool testa(const std::string &arkiv, const Val &v);  // packa upp till tomrum, kontrollera allt

u64 filstorlek_(const std::string &p);
int main_gzip(int argc, char **argv);

constexpr u64 BLOCKTAK = 192ull << 20;

}  // namespace nvz
