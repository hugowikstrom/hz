// SPDX-License-Identifier: GPL-2.0-only
// Tree scan, content classification, and metadata restore.
#pragma once
#include "hz.hpp"

namespace hz {

// The class decides three things: which transform is attempted, which block
// the entry ends up in, and which codec that block gets. An extension is a
// guess; magic bytes are a statement.
//
// The returned strings ("text", "binar", "png", "jpeg", "opak") are archive
// format constants — see the frozen-strings list in hz.hpp.
std::string classify(const std::string &path, const u8 *head, size_t n);

// Walks the tree once and produces entries in a stable order. The number of
// skipped special files (pipes, sockets, devices) is returned in *skipped*.
std::vector<Entry> scan_tree(const std::string &root, size_t *skipped);

// Restores an entry's metadata on disk: permissions and mtime. Files are
// written by whoever holds the content; this only sets metadata.
void set_metadata(const std::string &path, u32 mode, i64 mtime_ns, bool is_link);

}  // namespace hz
