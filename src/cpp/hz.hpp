// SPDX-License-Identifier: GPL-2.0-only
// nvz — content-aware archiver, C++ implementation.
// Core types and small helpers shared by every other translation unit.
//
// Codecs, classification, the tree walk, the PNG recipe and the archive
// itself each live in their own file and depend only on what is here.
//
// The retired Python implementation (src/nvz.py) stays in the tree as the
// v1 format reference and as the test oracle: the suite packs the same
// tree with both and demands identical bytes. The field names here mirror
// the Python ones for that reason.
//
// Frozen format strings (Swedish) — written into archives; translating
// them breaks compatibility (millions of bytes of existing archives
// contain them):
//   block class names: "text", "binar", "opak", "png-ra", "png-rest",
//                      "jpeg-kropp", "jpeg-rest", "blandat"
//   codec names:       "ra", "zstd3", ...
//   plus the v1 JSON register keys and the magic numbers.
// The source was translated from Swedish to English on 2026-11-19; these
// strings are data, not prose — do not translate them.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <stdexcept>

namespace hz {

using u8 = uint8_t;  using u16 = uint16_t;
using u32 = uint32_t; using u64 = uint64_t;
using i64 = int64_t;
using Byte = std::vector<u8>;

// Error meant to reach the user as a message, not a stack dump.
struct Error : std::runtime_error {
    explicit Error(const std::string &m) : std::runtime_error(m) {}
};

// Entry kinds in the tree.
enum class Kind : u8 { Dir = 'd', File = 'f', Link = 'l' };

struct Entry {
    std::string path;     // relative to the root; empty string = the root itself
    Kind kind = Kind::File;
    u32 mode = 0644;      // permissions (dirs and files)
    i64 mtime_ns = 0;     // mtime in nanoseconds
    u64 size = 0;         // size (files)
    std::string target;   // link target (links)
    i64 obj = -1;         // object number (files), -1 = none
};

// Object: one unique file body. Several entries may point at the same
// object; that is the deduplication.
struct Object {
    u8 hash[16]{};        // blake2b-128 of the original content
    u64 size = 0;         // raw size
    std::string cls;      // text | binar | png | jpeg | opak   (format strings)
    std::string ext;      // extension in lower case, for the sort
    std::string source;   // the path it was read from (only during packing)
    u32 id = 0;

    // Transform, if one was used: "png" or "jpeg". The body goes into one
    // block and the "rest" (PNG skeleton or JPEG header) into another.
    std::string transform;
    std::string recipe;   // PNG: the zlib recipe that reproduces the stream
    u32 blk = 0;  u64 off = 0, len = 0;          // where the body lives
    u32 rblk = 0; u64 roff = 0, rlen = 0;        // where the rest lives
};

struct Block {
    u32 index = 0;
    std::string cls;      // format string — see the frozen-strings list above
    std::string codec;    // format string
    u64 off = 0, clen = 0, raw_len = 0;
};

struct Register {
    u32 version = 2;
    std::string root;
    bool flat = false;
    std::string created;
    std::vector<Block> blocks;
    std::vector<Object> objects;
    std::vector<Entry> entries;
    // v1 (Python/JSON) carries only 8 hash bytes per object — that register
    // stored the first 16 hex digits. v2 carries all 16 bytes, v3 carries 4
    // (enough to catch mis-assembly; bit errors are caught by the block
    // checksums). The unpack-time check compares as many bytes as the
    // register actually has.
    size_t hash_len = 16;
    i64 created_epoch = 0;   // v3: creation time as epoch seconds
};

// Small helpers.
std::string lower(std::string s);
std::string extension_of(const std::string &path);   // ".PNG" -> ".png"
std::string hex(const u8 *d, size_t n);
void blake2b128(const u8 *data, size_t n, u8 out[16]);

Byte read_file(const std::string &path);
void write_file(const std::string &path, const u8 *d, size_t n);
// Checked writes: see the comment in util.cpp. `sync` fsyncs before closing
// and must be true for anything a source file is deleted after.
void write_checked(FILE *f, const void *d, size_t n, const std::string &what);
void close_checked(FILE *f, const std::string &what, bool sync);

// Scratch-file directory: $TMPDIR if set, else /tmp. Archives larger than
// /tmp do occur, and gzip honors TMPDIR too.
std::string temp_dir();

// printf-style formatting into a std::string.
std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace hz
