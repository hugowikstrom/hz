// The gzip surface: nvz without a subcommand behaves like gzip.
//
// The surface is gzip's because every script, Makefile and habit that
// already exists speaks it. The promises the test suite holds it to:
//
//   * `nvz file` replaces the file with file.nv, and the archive INHERITS
//     its time and permissions. `nvz -d` returns exactly the same bytes and
//     removes the archive. -c and -k touch nothing.
//   * A directory operand becomes ONE archive of the tree (source tree
//     kept); with -r each file is packed on its own instead, as gzip
//     literally does.
//   * Exit status separates errors (1) from warnings (2). Scripts hang on
//     that distinction.
//   * A tree archive refuses to be written to standard out. A thousand
//     files are not a byte stream.
#include "archive.hpp"
#include "tree.hpp"
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>

namespace nvz {
namespace {

struct Flags {
    bool d=false,c=false,k=false,f=false,t=false,l=false,r=false,q=false,N=false;
    int v=0;
    std::string suffix = ".nv";
    PackOpts opts;                   // mode defaults to "small" — the smallest archive
};

int err(const std::string &m) { fprintf(stderr, "nvz: %s\n", m.c_str()); return 1; }
int warn(const Flags &o, const std::string &m) {
    if (!o.q) fprintf(stderr, "nvz: %s\n", m.c_str());
    return 2;
}

// The level digits map onto the modes. Unlike gzip the DEFAULT is the
// smallest: whoever packs an archive almost always wants it small, and
// nvz's transforms are expensive once and cheap to unpack.
const char *level_mode(int n) { return n <= 3 ? "fast" : n <= 6 ? "normal" : "small"; }

struct LongFlag { const char *name; char shortc; bool takes_value; };
const LongFlag LONG_FLAGS[] = {
    {"stdout",'c',false},{"to-stdout",'c',false},{"decompress",'d',false},
    {"uncompress",'d',false},{"force",'f',false},{"help",'h',false},{"keep",'k',false},
    {"list",'l',false},{"license",'L',false},{"no-name",'n',false},{"name",'N',false},
    {"quiet",'q',false},{"silent",'q',false},{"recursive",'r',false},{"suffix",'S',true},
    {"test",'t',false},{"verbose",'v',false},{"version",'V',false},
    {"fast",'1',false},{"best",'9',false},
    {"mode",'M',true},{"codec",'K',true},{"jobs",'j',true},
    {"no-dedup",'D',false},{"no-transform",'O',false},
    {"no-sort",'s',false},{"no-classing",'C',false},
    {"block-cap",'B',true},{"portable",'P',false},
    // Swedish aliases: published reports, measurement scripts and muscle
    // memory use these. They are permanent.
    {"tyst",'q',false},{"läge",'M',true},{"lage",'M',true},{"kodek",'K',true},
    {"jobb",'j',true},{"utan-dedup",'D',false},{"utan-omvandling",'O',false},
    {"utan-sortering",'s',false},{"utan-klassning",'C',false},{"blocktak",'B',true},
    {"synchronous",0,false},{"rsyncable",0,false},{"ascii",0,false},{"no-time",0,false},
};

void help_surface() {
    printf(
"usage: nvz [FLAG]... [FILE]...\n"
"Packs and unpacks files and directory trees. With no flags each FILE is\n"
"packed and the original replaced, just like gzip — but the default mode is\n"
"`small`, the smallest nvz can make. A directory becomes ONE archive of the\n"
"whole tree.\n"
"\n"
"  -c, --stdout       write to standard out, leave the files alone\n"
"  -d, --decompress   unpack\n"
"  -f, --force        overwrite targets, follow links, repack packed files\n"
"  -k, --keep         keep the file that was packed\n"
"  -l, --list         show what the archives contain\n"
"  -q, --quiet        say nothing\n"
"  -r, --recursive    descend into directories, packing each file on its own\n"
"  -S, --suffix=.SUF  archive suffix (default .nv)\n"
"  -t, --test         verify the archive: unpack and compare to the register\n"
"  -v, --verbose      say what happens\n"
"  -1 .. -9           fast .. small: -1..-3 fast, -4..-6 normal, -7..-9 small\n"
"                     (no digit: small, i.e. the smallest nvz can make)\n"
"      --mode=M       fast | normal | small (beats -1..-9; default small)\n"
"      --codec=C      force zstd3 | zstd19 | zstd22 | xz9e | nvcm | nvlz\n"
"      --portable     archive readable by the stdlib Python tool on any OS\n"
"      --no-dedup --no-transform -j N\n"
"\n"
"Swedish flag names (--läge, --kodek, --utan-dedup, ...) remain as aliases.\n"
"Subcommands: nvz pack|unpack|list|test — see `nvz pack --help`.\n");
}

// Returns 0 on ok, otherwise an exit status.
int apply(Flags &o, char shortc, const char *value) {
    switch (shortc) {
    case 'c': o.c = true; break;         case 'd': o.d = true; break;
    case 'f': o.f = true; break;         case 'k': o.k = true; break;
    case 'l': o.l = true; break;         case 't': o.t = true; break;
    case 'q': o.q = true; o.v = 0; break;
    case 'r': o.r = true; break;         case 'v': o.v++; break;
    case 'n': o.N = false; break;        case 'N': o.N = true; break;
    case 'h': help_surface(); exit(0);
    case 'V': printf("nvz 2.0 (c++)\n"); exit(0);
    case 'L': printf("nvz: original code; no terms beyond the source tree's.\n"); exit(0);
    case 'S': o.suffix = value && value[0] == '.' ? value : std::string(".") + (value ? value : ""); break;
    case 'M': o.opts.mode = value ? value : ""; break;
    case 'K': o.opts.codec = value ? value : ""; break;
    case 'j': o.opts.jobs = value ? atoi(value) : 0; break;
    case 'D': o.opts.dedup = false; break;
    case 'O': o.opts.transform = false; break;
    case 's': o.opts.sort = false; break;
    case 'C': o.opts.class_blocks = false; break;
    case 'B': o.opts.block_cap = value ? ((u64)atoll(value) << 20) : 0; break;
    case 'P': o.opts.portable = true; break;
    case 0: break;                       // accepted and does nothing
    default:
        if (shortc >= '1' && shortc <= '9') { o.opts.mode = level_mode(shortc - '0'); break; }
        return 1;
    }
    return 0;
}

bool is_dir(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}
bool is_link(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}
bool exists(const std::string &p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0;
}
bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool may_write(const Flags &o, const std::string &target, int *status) {
    if (o.f || !exists(target)) return true;
    if (isatty(0) && isatty(2)) {
        fprintf(stderr, "nvz: %s already exists; overwrite (y/n)? ", target.c_str());
        char line[16] = {0};
        if (fgets(line, sizeof line, stdin) && (line[0] == 'y' || line[0] == 'j')) return true;
        fprintf(stderr, "\tnot overwritten\n");
        *status = std::max(*status, 2);
        return false;
    }
    Flags quieter = o;
    *status = std::max(*status, warn(quieter, target + " already exists -- unchanged"));
    return false;
}

void to_stdout(const std::string &p) {
    Byte d = read_file(p);
    // `nvz -c file > /dev/full` returned 0 before this check; gzip returns
    // 1. A write error to standard out is an error, not a warning.
    write_checked(stdout, d.data(), d.size(), "standard out");
    if (fflush(stdout) != 0)
        throw Error(std::string("standard out: write failed (")
                    + std::strerror(errno) + ")");
}

void ratio_line(const Flags &o, const std::string &from, u64 raw, u64 packed,
                const std::string &to, bool replaced) {
    if (o.v < 1 || o.q) return;
    double pct = raw ? (1.0 - (double)packed / raw) * 100.0 : 0.0;
    fprintf(stderr, "%s:\t%5.1f%% -- %s %s\n", from.c_str(), pct,
            replaced ? "replaced with" : "written to", to.c_str());
}

// With -r a directory is swapped for the files inside it. In that sweep,
// files with the wrong suffix are skipped WITHOUT a word — an `nvz -r` that
// barks one line per already-packed file is unusable. Name a file yourself
// and you get the warning.
void expand(const Flags &o, const std::vector<std::string> &files,
            bool want_archives, std::vector<std::string> &out) {
    for (const std::string &f : files) {
        if (o.r && is_dir(f) && !is_link(f)) {
            std::vector<std::string> queue = {f};
            while (!queue.empty()) {
                std::string dir = queue.front(); queue.erase(queue.begin());
                DIR *d = opendir(dir.c_str());
                if (!d) continue;
                std::vector<std::string> names;
                for (struct dirent *e; (e = readdir(d)); ) {
                    if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
                        names.push_back(e->d_name);
                }
                closedir(d);
                std::sort(names.begin(), names.end());
                for (const std::string &nm : names) {
                    std::string s = dir + "/" + nm;
                    if (is_link(s)) continue;
                    if (is_dir(s)) { queue.push_back(s); continue; }
                    if (ends_with(nm, o.suffix) != want_archives) continue;
                    out.push_back(s);
                }
            }
        } else {
            out.push_back(f);
        }
    }
}

u64 tree_size(const std::string &root) {
    u64 tot = 0;
    std::vector<std::string> queue = {root};
    while (!queue.empty()) {
        std::string dir = queue.front(); queue.erase(queue.begin());
        DIR *d = opendir(dir.c_str());
        if (!d) continue;
        for (struct dirent *e; (e = readdir(d)); ) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            std::string s = dir + "/" + e->d_name;
            struct stat st;
            if (lstat(s.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) queue.push_back(s);
            else if (S_ISREG(st.st_mode)) tot += (u64)st.st_size;
        }
        closedir(d);
    }
    return tot;
}

std::string scratch_template() { return temp_dir() + "/nvz-out-XXXXXX"; }

// --- pack -------------------------------------------------------------------
int pack_operand(const Flags &o, const std::string &p) {
    if (!exists(p)) return err(p + ": does not exist");
    std::string source = p;
    if (is_link(p)) {
        if (!o.f) return warn(o, p + " is a symlink -- skipped");
        char real[4096];
        if (!realpath(p.c_str(), real)) return err(p + ": the link points nowhere");
        source = real;
    }
    if (is_dir(source)) {
        // ONE archive of the tree, and the source tree stays.
        u64 raw = o.v ? tree_size(source) : 0;
        if (o.c) {
            std::string tmpl_s = scratch_template();
            std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end()); tmpl.push_back(0);
            if (!mkdtemp(tmpl.data())) return err("could not create scratch space");
            std::string ark = std::string(tmpl.data()) + "/out.nv";
            int status = 0;
            try { pack(source, ark, o.opts); ratio_line(o, p, raw, file_size(ark), "standard out", false); to_stdout(ark); }
            catch (const std::exception &e) { status = err(e.what()); }
            unlink(ark.c_str()); rmdir(tmpl.data());
            return status;
        }
        std::string out = p + o.suffix;
        int status = 0;
        if (!may_write(o, out, &status)) return status;
        pack(source, out, o.opts);
        ratio_line(o, p, raw, file_size(out), out, false);
        return 0;
    }
    struct stat st;
    if (lstat(source.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
        return warn(o, p + " is not a regular file -- skipped");
    if (st.st_nlink > 1 && !(o.f || o.k || o.c))
        return warn(o, p + strf(" has %lu other link(s) -- unchanged", (unsigned long)(st.st_nlink - 1)));
    if (ends_with(p, o.suffix) && !o.f)
        return warn(o, p + " already has the suffix " + o.suffix + " -- unchanged");
    u64 raw = (u64)st.st_size;
    if (o.c) {
        std::string tmpl_s = scratch_template();
        std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end()); tmpl.push_back(0);
        if (!mkdtemp(tmpl.data())) return err("could not create scratch space");
        std::string ark = std::string(tmpl.data()) + "/out.nv";
        int status = 0;
        try { pack_file(source, ark, o.opts); ratio_line(o, p, raw, file_size(ark), "standard out", false); to_stdout(ark); }
        catch (const std::exception &e) { status = err(e.what()); }
        unlink(ark.c_str()); rmdir(tmpl.data());
        return status;
    }
    std::string out = p + o.suffix;
    int status = 0;
    if (!may_write(o, out, &status)) return status;
    pack_file(source, out, o.opts);
    // The archive inherits the file's time and permissions, like gzip.
    set_metadata(out, st.st_mode & 07777,
                 (i64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec, false);
    if (!o.k) unlink(p.c_str());
    ratio_line(o, p, raw, file_size(out), out, !o.k);
    return 0;
}

// --- unpack -----------------------------------------------------------------
int unpack_operand(const Flags &o, const std::string &p_in) {
    std::string p = p_in;
    struct stat st;
    if (stat(p.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        std::string with = p + o.suffix;               // `nvz -d file` finds file.nv
        if (exists(p) || stat(with.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
            return err(p + ": does not exist");
        p = with;
    }
    if (!ends_with(p, o.suffix) && !(o.c || o.f))
        return warn(o, p + ": unknown suffix -- skipped");
    Register reg;
    try { reg = read_register(p); }
    catch (const std::exception &e) { return err(p + ": " + e.what()); }

    if (reg.flat) {
        std::string name = archived_name(reg);
        // The target: strip the suffix; with -N use the stored name instead.
        // (The old code computed the same condition twice — simplified.)
        std::string target;
        if (o.N && !name.empty())
            target = p.substr(0, p.find_last_of('/') + 1) + name;
        else if (ends_with(p, o.suffix))
            target = p.substr(0, p.size() - o.suffix.size());
        if (target.empty()) return warn(o, p + ": unknown suffix -- skipped");
        if (o.c) {
            std::string tmpl_s = scratch_template();
            std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end()); tmpl.push_back(0);
            if (!mkdtemp(tmpl.data())) return err("could not create scratch space");
            std::string tmp = std::string(tmpl.data()) + "/f";
            int status = 0;
            try { unpack_file(p, tmp, o.opts); to_stdout(tmp); }
            catch (const std::exception &e) { status = err(e.what()); }
            unlink(tmp.c_str()); rmdir(tmpl.data());
            return status;
        }
        int status = 0;
        if (!may_write(o, target, &status)) return status;
        u64 c = file_size(p);
        unpack_file(p, target, o.opts);
        ratio_line(o, p, unpacked_size(reg), c, target, !o.k);
        if (!o.k) unlink(p.c_str());
        return 0;
    }
    // A tree. It cannot be poured into a byte stream, and it should not be
    // guessed into one either — an archive of a thousand files is not a
    // file.
    if (o.c)
        return err(p + ": the archive holds a tree (" + reg.root +
                   "); it cannot be written to standard out");
    std::string parent = p.find('/') == std::string::npos ? "." : p.substr(0, p.find_last_of('/'));
    std::string root = parent + "/" + reg.root;
    int status = 0;
    if (!may_write(o, root, &status)) return status;
    u64 c = file_size(p);
    PackOpts po = o.opts; po.quiet = (o.v < 2);
    unpack(p, parent, po);
    ratio_line(o, p, unpacked_size(reg), c, root, !o.k);
    if (!o.k) unlink(p.c_str());
    return 0;
}

// --- list and test ----------------------------------------------------------
int list_operand(const Flags &o, const std::string &p, u64 *sc, u64 *su, int *sn) {
    Register reg;
    try { reg = read_register(p); }
    catch (const std::exception &e) { return err(p + ": " + e.what()); }
    u64 c = file_size(p), u = unpacked_size(reg);
    std::string name = archived_name(reg);
    if (name.empty()) name = reg.root + "/";
    printf("%13llu %13llu %6.1f%%  %s\n", (unsigned long long)c, (unsigned long long)u,
           (1.0 - (double)c / std::max<u64>(u, 1)) * 100.0, name.c_str());
    *sc += c; *su += u; (*sn)++;
    if (o.v) list_archive(p);
    return 0;
}

int test_operand(const Flags &o, const std::string &p) {
    // gzip names the actual problem for a missing operand; so should we.
    // Without this, verify() swallowed the "cannot open" into a generic
    // THE TEST FAILED.
    if (!exists(p)) return err(p + ": does not exist");
    bool ok;
    try { ok = verify(p, o.opts); }
    catch (const std::exception &e) { return err(p + ": the test could not run (" + std::string(e.what()) + ")"); }
    if (!ok) return err(p + ": THE TEST FAILED");
    if (o.v || isatty(1)) printf("%s:\tOK\n", p.c_str());
    return 0;
}

// --- standard in -> standard out ---------------------------------------------
int stream(const Flags &o) {
    if (!o.d && !o.t && !o.l && isatty(1) && !o.f)
        return err("compressed data is not written to a terminal. "
                   "Use -f if you mean it, or give a file.");
    std::string tmpl_s = temp_dir() + "/nvz-stream-XXXXXX";
    std::vector<char> tmpl(tmpl_s.begin(), tmpl_s.end()); tmpl.push_back(0);
    if (!mkdtemp(tmpl.data())) return err("could not create scratch space");
    std::string d = tmpl.data();
    std::string inner = d + "/in";
    {
        FILE *u = fopen(inner.c_str(), "wb");
        if (!u) { rmdir(d.c_str()); return err("could not write to the scratch space"); }
        u8 buf[1 << 16];
        size_t r;
        while ((r = fread(buf, 1, sizeof buf, stdin)) > 0) write_checked(u, buf, r, inner);
        close_checked(u, inner, true);
    }
    int status = 0;
    try {
        if (o.d || o.t || o.l) {
            Register reg = read_register(inner);
            if (o.l) {
                u64 sc = 0, su = 0; int sn = 0;
                printf("%13s %13s  ratio  name\n", "packed", "unpacked");
                status = list_operand(o, inner, &sc, &su, &sn);
            } else if (o.t) {
                status = test_operand(o, inner);
            } else if (!reg.flat) {
                status = err("the archive holds a tree and cannot be unpacked "
                             "to standard out — give the archive as a file");
            } else {
                std::string out = d + "/out";
                unpack_file(inner, out, o.opts);
                to_stdout(out);
                unlink(out.c_str());
            }
        } else {
            std::string ark = d + "/out.nv";
            pack_file(inner, ark, o.opts, "standard-in");
            to_stdout(ark);
            unlink(ark.c_str());
        }
    } catch (const std::exception &e) {
        status = err(e.what());
    }
    unlink(inner.c_str());
    std::string clean = "rm -rf '" + d + "'";
    if (system(clean.c_str()) != 0) {}
    return status;
}

}  // namespace

int main_gzip(int argc, char **argv) {
    Flags o;
    std::vector<std::string> files;
    bool only_files = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (only_files || a == "-" || a[0] != '-') { files.push_back(a); continue; }
        if (a == "--") { only_files = true; continue; }
        if (a.size() > 1 && a[1] == '-') {
            std::string name = a.substr(2), value;
            bool has_value = false;
            size_t eq = name.find('=');
            if (eq != std::string::npos) { value = name.substr(eq + 1); name = name.substr(0, eq); has_value = true; }
            // Unambiguous abbreviations are accepted; ambiguous ones are
            // refused. A script that writes --l today could mean four
            // different things tomorrow.
            std::vector<const LongFlag*> hits;
            for (const LongFlag &lf : LONG_FLAGS) {
                if (name == lf.name) { hits = {&lf}; break; }
                if (strncmp(lf.name, name.c_str(), name.size()) == 0) hits.push_back(&lf);
            }
            if (hits.empty()) return err("unknown flag -- '--" + name + "'\nTry nvz --help.");
            if (hits.size() > 1) {
                std::string options;
                for (auto *lf : hits) options += std::string(options.empty() ? "" : ", ") + "--" + lf->name;
                return err("ambiguous flag -- '--" + name + "' (" + options + ")");
            }
            const LongFlag &lf = *hits[0];
            if (lf.takes_value && !has_value) {
                if (i + 1 >= argc) return err("--" + std::string(lf.name) + " needs a value");
                value = argv[++i];
            }
            if (apply(o, lf.shortc, lf.takes_value ? value.c_str() : nullptr))
                return err("unknown flag -- '--" + name + "'");
            continue;
        }
        for (size_t j = 1; j < a.size(); j++) {
            char ch = a[j];
            if (ch == 'S' || ch == 'j') {
                std::string value = a.substr(j + 1);
                if (value.empty()) {
                    if (i + 1 >= argc) return err(strf("-%c needs a value", ch));
                    value = argv[++i];
                }
                apply(o, ch, value.c_str());
                break;
            }
            if (apply(o, ch, nullptr))
                return err(strf("unknown flag -- '%c'\nTry nvz --help.", ch));
        }
    }
    std::string mode = canonical_mode(o.opts.mode);
    if (mode.empty()) return err("unknown mode " + o.opts.mode);
    o.opts.mode = mode;
    if (o.opts.mode == "fast") o.opts.transform = false;
    o.opts.quiet = (o.v < 2);

    if (files.empty()) return stream(o);

    int status = 0;
    if (o.l) {
        u64 sc = 0, su = 0; int sn = 0;
        if (!o.q) printf("%13s %13s  ratio  name\n", "packed", "unpacked");
        std::vector<std::string> all;
        expand(o, files, true, all);
        for (const std::string &p : all) status = std::max(status, list_operand(o, p, &sc, &su, &sn));
        if (sn > 1)
            printf("%13llu %13llu %6.1f%%  (total)\n", (unsigned long long)sc,
                   (unsigned long long)su, (1.0 - (double)sc / std::max<u64>(su, 1)) * 100.0);
        return status;
    }
    if (o.t) {
        std::vector<std::string> all;
        expand(o, files, true, all);
        for (const std::string &p : all) status = std::max(status, test_operand(o, p));
        return status;
    }
    std::vector<std::string> all;
    expand(o, files, o.d, all);
    for (const std::string &p : all) {
        // ONE FAILING OPERAND MUST NOT TAKE THE REST WITH IT. Without this
        // catch, an exception out of pack_operand escaped through main and
        // the process died in std::terminate: `nvz a.txt b.txt c.txt` with
        // b.txt unreadable gave SIGABRT and a core dump (exit status 134),
        // with a.txt already packed and deleted and c.txt never processed.
        // The fault came out of Ted Lyngmo's code review on 2026-11-18.
        // gzip's contract was measured on the same case: a warning, the
        // rest are packed, exit status 1.
        int s;
        try { s = o.d ? unpack_operand(o, p) : pack_operand(o, p); }
        catch (const std::exception &e) { s = err(p + ": " + e.what()); }
        status = std::max(status, s);
    }
    return status;
}

}  // namespace nvz
