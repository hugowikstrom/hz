#pragma once
#include "nvz.hpp"

namespace nvz {

// Kodekarnas namn är desamma som i pythonversionen, för att registret ska
// betyda samma sak i båda: ra, zstd3, zstd19, zstd22, xz9e, nvcm.
// Skillnaden är att det här anropas BIBLIOTEKEN — libzstd och liblzma — i
// samma process. Pythonversionen startar programmen zstd och xz, vilket
// kostar en process per block och tvingar data genom rör.
// `samtidiga` = hur många block som väntas packas samtidigt. Den styr hur
// stor ordbok xz får ta: en ordbok som inte ryms i minnet delat på antalet
// kodare serialiserar packningen i stället för att göra den mindre.
Byte komprimera(const std::string &kodek, const u8 *d, size_t n, int tradar,
                int samtidiga = 1);
Byte dekomprimera(const std::string &kodek, const u8 *d, size_t n, u64 ralen);

// Provar ett stickprov ur blocket med varje kandidat och väljer vinnaren.
// Samma stickprov (8 ställen, 4 MiB totalt) och samma trösklar som python.
// Vilken xz-variant passar datan? Se kommentaren vid pb i kodek.cpp.
std::string xz_variant(const u8 *d, size_t n);

// Sista parametern svarar på en andra fråga: HUR SÄKERT var valet? Den
// sätts till vinnarens marginal mot xz-kandidaten på stickprovet. Ett tunt
// försprång betyder att rangordningen kan vända när hela blocket packas
// (se kommentaren vid verifieringen i arkiv.cpp).
std::string valj_kodek(const u8 *d, size_t n, bool med_cm, bool med_gpu, int tradar,
                       bool opak = false, double *xz_marginal = nullptr);

constexpr size_t PROVBYTE = 4u << 20;

}  // namespace nvz
