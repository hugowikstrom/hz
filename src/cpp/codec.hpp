// SPDX-License-Identifier: GPL-2.0-only
// Block codecs: compression, decompression and codec selection.
#pragma once
#include "hz.hpp"

namespace hz {

// Codec names are archive format constants, shared with the Python-era
// archives so a register means the same thing in both: ra, zstd3, zstd3L,
// zstd19, zstd22, xz9e, xz9e-j0, nvcm, nvlz. Unlike the Python
// implementation, the libraries — libzstd and liblzma — are called in
// process; Python started the zstd and xz programs, which cost a process
// per block and forced data through pipes.
//
// `concurrent` = how many blocks are expected to be compressing at once. It
// governs how large a dictionary xz may take: a dictionary that does not fit
// in memory divided by the number of encoders serializes the packing instead
// of shrinking the archive.
Byte compress(const std::string &codec, const u8 *d, size_t n, int threads,
              int concurrent = 1);
// `threads` bounds the nvcm-p shard pool; 0 means "ask the hardware".
// Callers that already run inside a parallel_for pass 1, so the block
// loop provides the parallelism instead of multiplying it.
Byte decompress(const std::string &codec, const u8 *d, size_t n, u64 raw_len,
                int threads = 0);

// Which xz variant suits the data? See the comment at `pb` in codec.cpp.
std::string xz_variant(const u8 *d, size_t n);

// Compresses a sample of the block with each candidate and picks the winner.
// Same sample (8 spots, 4 MiB total) and same thresholds as the Python
// implementation.
//
// The last parameter answers a second question: how sure was the choice? It
// is set to the winner's margin against the xz candidate on the sample. A
// thin lead means the ranking can flip when the whole block is compressed
// (see the re-check in archive.cpp).
std::string choose_codec(const u8 *d, size_t n, bool with_cm, bool with_gpu, int threads,
                         bool opaque = false, double *xz_margin = nullptr);

constexpr size_t PROBE_BYTES = 4u << 20;

}  // namespace hz
