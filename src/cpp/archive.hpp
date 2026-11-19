// SPDX-License-Identifier: GPL-2.0-only
// nvz archive front end: pack/unpack entry points, options, and the
// single-file (gzip-surface) variants.
#pragma once
#include "hz.hpp"

namespace hz {

struct PackOpts {
    std::string mode = "small";     // fast | normal | small | gpu
    bool dedup = true;
    bool transform = true;
    // Ablation flags, so each trick can be measured in isolation (the
    // report's section on prior work). The sort and the class split are
    // two distinct effects that would otherwise be measured together.
    bool sort = true;               // place similar files next to each other
    bool class_blocks = true;       // one block per content class
    u64 block_cap = 0;              // 0 = BLOCK_CAP; otherwise raw bytes per block
    // Portable archives: restrict the codecs to {ra, xz9e, xz9e-j0} and
    // switch the transforms off, so the archive can be unpacked by the
    // pure-stdlib Python tool (portable/nvzpy.py) on macOS and Windows —
    // no libzstd, no nvcm, no nvjpg, no deflate-engine dependency. Slower
    // and somewhat larger; that is the price of running everywhere.
    bool portable = false;
    int jobs = 0;                   // 0 = number of cores
    bool quiet = false;
    std::string codec;              // forced codec, empty = probe
    bool flat = false;              // single-file archive: the root is invisible on unpack
};

u64 pack(const std::string &root, const std::string &archive, const PackOpts &o);
void unpack(const std::string &archive, const std::string &target, const PackOpts &o);
void list_archive(const std::string &archive);
Register read_register(const std::string &archive);

// Single-file archives behind the gzip surface: one file becomes a "flat"
// archive — same format, but the register is marked so that unpacking
// creates no root directory. A large archive pays zero bytes for the
// capability.
u64 pack_file(const std::string &file, const std::string &archive, const PackOpts &o,
              const std::string &name = "");
void unpack_file(const std::string &archive, const std::string &target, const PackOpts &o);
std::string archived_name(const Register &reg);   // empty string = tree archive
u64 unpacked_size(const Register &reg);
bool verify(const std::string &archive, const PackOpts &o);  // unpack to scratch, check everything

u64 file_size(const std::string &p);
int main_gzip(int argc, char **argv);

// Canonicalize a mode name. The Swedish names remain as aliases because
// published reports, measurement scripts and muscle memory use them.
// Returns the canonical name, or "" if unknown.
std::string canonical_mode(const std::string &mode);

constexpr u64 BLOCK_CAP = 192ull << 20;

}  // namespace hz
