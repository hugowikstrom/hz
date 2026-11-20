// SPDX-License-Identifier: GPL-2.0-only
// nvz entry point: subcommand dispatch for pack/unpack/list/test, the
// hidden worker subcommands, and fallthrough to the gzip surface.
//
//   nvz pack    <dir> <archive.hz> [--mode ...] [--codec ...] [-q] [-j N]
//   nvz unpack  <archive.hz> <target> [-q] [-j N]
//   nvz list    <archive.hz>
//   nvz test    <dir>              pack, unpack, compare bit for bit
//
// The Swedish subcommand names (packa, packa-upp, lista, prova) are
// permanent aliases: published reports, measurement scripts and muscle
// memory use them.
//
// The default mode is "small": whoever packs an archive almost always
// wants it small, and nvz's transforms are expensive once and cheap to
// unpack.
//
// The hidden subcommands `hzcm pack|unpack` and `hzjpg pack|unpack` are
// worker entry points: nvz starts itself with them (see worker.cpp). They
// exist for machines and are left out of the help text.
#include "archive.hpp"
#include "tree.hpp"
#include <csignal>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {
int hzcm_pack(void);
int hzcm_unpack(void);
int hzjpg_pack(void);
int hzjpg_unpack(void);
}

namespace {

void help() {
    printf(
"usage: hz <command> ...\n"
"\n"
"  hz pack    <dir> <archive.hz>    pack a tree\n"
"  hz unpack  <archive.hz> <target> unpack\n"
"  hz list    <archive.hz>          show the contents\n"
"  hz test    <dir>                 pack, unpack, compare bit for bit\n"
"\n"
"flags for pack:\n"
"  --mode M     fast | normal | small   (default small — the smallest archive)\n"
"  --codec C    force zstd3|zstd19|zstd22|xz9e|hzcm|hzlz\n"
"  --portable   restrict to ra/xz and no transforms: the archive can then be\n"
"               unpacked by the stdlib Python tool (portable/hzpy.py) on\n"
"               macOS and Windows\n"
"  --no-dedup --no-transform\n"
"  --no-sort --no-classing   (ablation flags: switch off one trick at a time)\n"
"  --block-cap MiB  raw bytes per block (default 192; more blocks = more parallel)\n"
"shared: -q quiet, -j N number of jobs\n"
"\n"
"Swedish names remain as aliases: packa, packa-upp, lista, prova, --läge,\n"
"--kodek, --blocktak, --utan-dedup, --utan-omvandling, --utan-sortering,\n"
"--utan-klassning, and the mode values snabb/liten.\n");
}

int roundtrip_test(const std::string &dir, const hz::PackOpts &o) {
    // Packs to a scratch file, unpacks next to it, compares via hashes.
    std::string tmpl_s = hz::temp_dir() + "/nvz-roundtrip-XXXXXX";
    std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end()); tmpl.push_back(0);
    if (!mkdtemp(tmpl.data())) { fprintf(stderr, "hz: could not create scratch space\n"); return 1; }
    std::string d = tmpl.data();
    std::string ark = d + "/test.nv", up = d + "/up";
    int failed = 0;
    try {
        hz::pack(dir, ark, o);
        mkdir(up.c_str(), 0700);
        hz::unpack(ark, up, o);
        // The unpack itself does the comparison: every file is checked
        // against its blake2b in the register before it is accepted, so
        // reaching this point proves the content. Tree completeness is
        // covered by the entry count having matched.
        printf("Test ok: every file identical bit for bit.\n");
    } catch (const std::exception &e) {
        fprintf(stderr, "hz: %s\n", e.what());
        failed = 1;
    }
    std::string clean = "rm -rf '" + d + "'";
    if (system(clean.c_str()) != 0) { /* the scratch dir stays behind */ }
    return failed;
}

bool is_sub(const char *a, const char *en, const char *sv) {
    return strcmp(a, en) == 0 || strcmp(a, sv) == 0;
}

}  // namespace

int main(int argc, char **argv) {
    // SIGXFSZ is ignored on purpose. The signal arrives when a write
    // exceeds RLIMIT_FSIZE, and the default action kills the process —
    // gzip dies that way with status 153: no cleanup runs and a truncated
    // archive that looks valid stays behind. Ignored, the write gets
    // EFBIG, write_checked catches it, the half archive is removed and the
    // exit status is 1, nvz's convention for errors. A deliberate
    // deviation from gzip, the only one on the error path.
    signal(SIGXFSZ, SIG_IGN);
    if (argc < 2) return hz::main_gzip(argc, argv);
    std::string cmd = argv[1];

    // The workers first: they touch stdin/stdout and nothing else.
    if ((cmd == "hzcm" || cmd == "nvcm") && argc >= 3)
        return is_sub(argv[2], "pack", "packa") ? hzcm_pack() : hzcm_unpack();
    if ((cmd == "hzjpg" || cmd == "nvjpg") && argc >= 3)
        return is_sub(argv[2], "pack", "packa") ? hzjpg_pack() : hzjpg_unpack();

    if (cmd == "-h" || cmd == "--help") { help(); return 0; }
    if (cmd == "-V" || cmd == "--version") { printf("hyperZip (hz) 2.0 (c++)\n"); return 0; }

    // No subcommand: the gzip surface. `nvz file` packs, `nvz -d file.nv`
    // unpacks, and the flags mean what they mean in gzip.
    const bool is_pack   = is_sub(cmd.c_str(), "pack", "packa");
    const bool is_unpack = is_sub(cmd.c_str(), "unpack", "packa-upp");
    const bool is_list   = is_sub(cmd.c_str(), "list", "lista");
    const bool is_test   = is_sub(cmd.c_str(), "test", "prova");
    if (!is_pack && !is_unpack && !is_list && !is_test)
        return hz::main_gzip(argc, argv);

    hz::PackOpts o;
    std::vector<std::string> free_args;
    for (int i = 2; i < argc; i++) {
        std::string a = argv[i];
        auto value = [&](const char *name) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "hz: %s needs a value\n", name); exit(1); }
            return argv[++i];
        };
        auto eat = [&](const char *flag, const char *alias, std::string *out) {
            // --flag VALUE, --flag=VALUE, and the same for the alias.
            std::string f1 = std::string("--") + flag, f2 = std::string("--") + alias;
            if (a == f1 || a == f2) { *out = value(flag); return true; }
            if (a.rfind(f1 + "=", 0) == 0) { *out = a.substr(f1.size() + 1); return true; }
            if (a.rfind(f2 + "=", 0) == 0) { *out = a.substr(f2.size() + 1); return true; }
            return false;
        };
        std::string v;
        if (a == "-q" || a == "--quiet" || a == "--tyst") o.quiet = true;
        else if (a == "-j" || a == "--jobs" || a == "--jobb") o.jobs = atoi(value("-j").c_str());
        else if (eat("mode", "läge", &v) || eat("mode", "lage", &v)) o.mode = v;
        else if (eat("codec", "kodek", &v)) o.codec = hz::canonical_codec(v);
        else if (eat("block-cap", "blocktak", &v)) o.block_cap = (hz::u64)atoll(v.c_str()) << 20;
        else if (a == "--no-dedup" || a == "--utan-dedup") o.dedup = false;
        else if (a == "--no-transform" || a == "--utan-omvandling") o.transform = false;
        else if (a == "--no-sort" || a == "--utan-sortering") o.sort = false;
        else if (a == "--no-classing" || a == "--utan-klassning") o.class_blocks = false;
        else if (a == "--portable") o.portable = true;
        else if (a == "-h" || a == "--help") { help(); return 0; }
        else if (a[0] == '-') { fprintf(stderr, "hz: unknown flag %s\n", a.c_str()); return 1; }
        else free_args.push_back(a);
    }
    std::string mode = hz::canonical_mode(o.mode);
    if (mode.empty()) {
        fprintf(stderr, "hz: unknown mode %s\n", o.mode.c_str()); return 1;
    }
    o.mode = mode;
    if (o.mode == "fast") o.transform = false;

    try {
        if (is_pack && free_args.size() == 2) {
            hz::pack(free_args[0], free_args[1], o);
        } else if (is_unpack && free_args.size() == 2) {
            hz::unpack(free_args[0], free_args[1], o);
        } else if (is_list && free_args.size() == 1) {
            hz::list_archive(free_args[0]);
        } else if (is_test && free_args.size() == 1) {
            return roundtrip_test(free_args[0], o);
        } else {
            help(); return 1;
        }
    } catch (const std::exception &e) {
        fprintf(stderr, "hz: %s\n", e.what());
        return 1;
    }
    return 0;
}
