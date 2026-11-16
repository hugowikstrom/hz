#pragma once
#include "nvz.hpp"

namespace nvz {

// Kör den egna binären som arbetare: /proc/self/exe <arg...> med *in* på
// stdin. Returnerar arbetarens utdata; *felkod* får dess slutstatus.
// Det är så nvcm och nvjpg används: koden är inlänkad i samma binär, men
// körs i en egen process så att en vägran (exit 2 på progressiv JPEG) inte
// fäller packaren, och så att en trasig ström inte kan skada arkivet.
Byte kor_sjalv(const std::vector<std::string> &arg, const u8 *in, size_t n, int *felkod);

// Samma sak men med en namngiven binär. Används för nvgpu, som är ett eget
// program (vulkan-beroendet ska inte in i nvz). Binären söks bredvid den
// egna först, sedan i PATH.
Byte kor_binar(const std::string &binar, const std::vector<std::string> &arg,
               const u8 *in, size_t n, int *felkod);
std::string binar_bredvid(const std::string &namn);

}  // namespace nvz
