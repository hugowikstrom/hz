// SPDX-License-Identifier: GPL-2.0-only
// PNG and JPEG transforms: split a file into rest + body so the body can
// be compressed in a block of its own kind.
#pragma once
#include "hz.hpp"

namespace hz {

// A successful transform: the rest (skeleton/header) and the body (raw
// data/stream), plus the recipe for PNG. Everything is proven reproducible
// before it is returned — the unfold reassembles its own result and
// compares it against the original.
struct Unfolded {
    std::string tag;      // "png" | "jpeg"  (format strings)
    Byte rest, body;
    std::string recipe;   // PNG: "level,wbits,mem,strategy[,engine]"; JPEG: empty
};

std::optional<Unfolded> png_unfold(const Byte &b, double inflation_cap);
std::optional<Unfolded> jpeg_unfold(const Byte &b);

Byte png_rebuild(const Byte &skeleton, const Byte &raw, const std::string &recipe);
Byte jpeg_rebuild(const Byte &rest, const Byte &body);

constexpr double PNG_INFLATION_CAP = 150.0;

}  // namespace hz
