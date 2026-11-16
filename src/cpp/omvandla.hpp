#pragma once
#include "nvz.hpp"

namespace nvz {

// En lyckad omvandling: resten (skelett/huvud) och kroppen (rådata/ström),
// plus receptet för PNG. Allt är BEVISAT återskapbart innan det returneras —
// veckla_ut syr själv ihop resultatet och jämför med originalet.
struct Omvandlat {
    std::string tr;       // "png" | "jpeg"
    Byte rest, kropp;
    std::string recept;   // PNG: "nivå,wbits,mem,strategi"; JPEG: tom
};

std::optional<Omvandlat> png_veckla_ut(const Byte &b, double uppblasningstak);
std::optional<Omvandlat> jpeg_veckla_ut(const Byte &b);

Byte png_aterskapa(const Byte &skelett, const Byte &ra, const std::string &recept);
Byte jpeg_aterskapa(const Byte &rest, const Byte &kropp);

constexpr double PNG_UPPBLASNING_TAK = 150.0;

}  // namespace nvz
