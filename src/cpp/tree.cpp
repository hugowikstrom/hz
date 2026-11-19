// SPDX-License-Identifier: GPL-2.0-only
// Tree walking and content classification: scan a directory into Entry
// records, classify file heads, restore metadata on unpack.
#include "tree.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <algorithm>
#include <set>

namespace nvz {
namespace {

const std::set<std::string> TEXT_EXTS = {
 ".txt",".md",".json",".jsonl",".js",".mjs",".cjs",".ts",".tsx",".jsx",".py",".rs",
 ".go",".c",".h",".cpp",".hpp",".java",".sh",".bash",".zsh",".sql",".css",".scss",".html",".htm",
 ".xml",".svg",".yml",".yaml",".toml",".ini",".cfg",".conf",".service",".log",".csv",".tsv",
 ".po",".pot",".map",".lock",".patch",".diff",".rst",".tex",".webmanifest"};

const std::set<std::string> OPAQUE_EXTS = {
 ".mp4",".m4a",".m4v",".mov",".mkv",".webm",".mp3",".ogg",".opus",".flac",
 ".webp",".avif",".heic",".gif",".zip",".gz",".tgz",".bz2",".xz",".zst",".7z",".rar",".br",
 ".woff",".woff2",".pdf",".jxl",".wasm",".deb",".rpm",".whl",".jar",".apk",".odt",".ods",
 ".docx",".xlsx",".pptx"};

}  // namespace

std::string classify(const std::string &path, const u8 *head, size_t n) {
    if (n >= 8 && !memcmp(head, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (n >= 3 && head[0] == 0xff && head[1] == 0xd8 && head[2] == 0xff) return "jpeg";
    std::string e = extension_of(path);
    if (OPAQUE_EXTS.count(e)) return "opak";
    if (TEXT_EXTS.count(e)) return "text";
    // Unknown extension: fewer than one byte in twenty outside the
    // printable range classifies as text. Text compresses better next to
    // text.
    size_t probe = std::min<size_t>(n, 4096), odd = 0;
    for (size_t i = 0; i < probe; i++) {
        u8 b = head[i];
        if (b < 9 || (b > 13 && b < 32)) odd++;
    }
    if (probe && odd * 20 < probe) return "text";
    return "binar";
}

std::vector<Entry> scan_tree(const std::string &root, size_t *skipped) {
    std::vector<Entry> entries;
    *skipped = 0;

    // Breadth first with directories sorted: a deterministic walk order
    // gives a reproducible archive.
    std::vector<std::string> queue = {""};
    while (!queue.empty()) {
        std::string rel = queue.front();
        queue.erase(queue.begin());
        std::string full = rel.empty() ? root : root + "/" + rel;

        struct stat st;
        if (lstat(full.c_str(), &st) != 0) throw Error(full + ": cannot read");
        Entry dir; dir.path = rel; dir.kind = Kind::Dir;
        dir.mode = st.st_mode & 07777;
        dir.mtime_ns = (i64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
        entries.push_back(dir);

        DIR *d = opendir(full.c_str());
        if (!d) throw Error(full + ": cannot open");
        std::vector<std::string> names;
        for (struct dirent *e; (e = readdir(d)); ) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            names.push_back(e->d_name);
        }
        closedir(d);
        std::sort(names.begin(), names.end());

        std::vector<std::string> subdirs;
        for (const std::string &nm : names) {
            std::string r = rel.empty() ? nm : rel + "/" + nm;
            std::string f = root + "/" + r;
            struct stat s2;
            if (lstat(f.c_str(), &s2) != 0) continue;
            i64 mt = (i64)s2.st_mtim.tv_sec * 1000000000 + s2.st_mtim.tv_nsec;
            if (S_ISLNK(s2.st_mode)) {
                // A symlink to a directory must be caught here: testing
                // for directories first drops it silently from the archive
                // — the fault the test suite found in the Python version.
                char buf[4096];
                ssize_t l = readlink(f.c_str(), buf, sizeof buf - 1);
                if (l < 0) continue;
                buf[l] = 0;
                Entry p; p.path = r; p.kind = Kind::Link; p.target = buf; p.mtime_ns = mt;
                entries.push_back(p);
            } else if (S_ISDIR(s2.st_mode)) {
                subdirs.push_back(r);
            } else if (S_ISREG(s2.st_mode)) {
                Entry p; p.path = r; p.kind = Kind::File; p.mode = s2.st_mode & 07777;
                p.mtime_ns = mt; p.size = (u64)s2.st_size;
                entries.push_back(p);
            } else {
                (*skipped)++;      // pipes, sockets, devices
            }
        }
        queue.insert(queue.begin(), subdirs.begin(), subdirs.end());
    }
    return entries;
}

void set_metadata(const std::string &path, u32 mode, i64 mtime_ns, bool is_link) {
    struct timespec times[2];
    times[0].tv_sec = mtime_ns / 1000000000; times[0].tv_nsec = mtime_ns % 1000000000;
    times[1] = times[0];
    if (is_link) {
        utimensat(AT_FDCWD, path.c_str(), times, AT_SYMLINK_NOFOLLOW);
        return;
    }
    if (chmod(path.c_str(), mode) != 0) { /* chmod is best effort */ }
    utimensat(AT_FDCWD, path.c_str(), times, 0);
}

}  // namespace nvz
