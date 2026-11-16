#pragma once
#include "nvz.hpp"

namespace nvz {

// Kodekarnas namn är desamma som i pythonversionen, för att registret ska
// betyda samma sak i båda: ra, zstd3, zstd19, zstd22, xz9e, nvcm.
// Skillnaden är att det här anropas BIBLIOTEKEN — libzstd och liblzma — i
// samma process. Pythonversionen startar programmen zstd och xz, vilket
// kostar en process per block och tvingar data genom rör.
Byte komprimera(const std::string &kodek, const u8 *d, size_t n, int tradar);
Byte dekomprimera(const std::string &kodek, const u8 *d, size_t n, u64 ralen);

// Provar ett stickprov ur blocket med varje kandidat och väljer vinnaren.
// Samma stickprov (8 ställen, 4 MiB totalt) och samma trösklar som python.
std::string valj_kodek(const u8 *d, size_t n, bool med_cm, bool med_gpu, int tradar);

constexpr size_t PROVBYTE = 4u << 20;

}  // namespace nvz
