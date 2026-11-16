#include "trad.hpp"
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

const std::set<std::string> TEXTANDELSER = {
 ".txt",".md",".json",".jsonl",".js",".mjs",".cjs",".ts",".tsx",".jsx",".py",".rs",
 ".go",".c",".h",".cpp",".hpp",".java",".sh",".bash",".zsh",".sql",".css",".scss",".html",".htm",
 ".xml",".svg",".yml",".yaml",".toml",".ini",".cfg",".conf",".service",".log",".csv",".tsv",
 ".po",".pot",".map",".lock",".patch",".diff",".rst",".tex",".webmanifest"};

const std::set<std::string> OPAKA = {
 ".mp4",".m4a",".m4v",".mov",".mkv",".webm",".mp3",".ogg",".opus",".flac",
 ".webp",".avif",".heic",".gif",".zip",".gz",".tgz",".bz2",".xz",".zst",".7z",".rar",".br",
 ".woff",".woff2",".pdf",".jxl",".wasm",".deb",".rpm",".whl",".jar",".apk",".odt",".ods",
 ".docx",".xlsx",".pptx"};

}  // namespace

std::string klassificera(const std::string &sokvag, const u8 *huvud, size_t n) {
    if (n >= 8 && !memcmp(huvud, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (n >= 3 && huvud[0] == 0xff && huvud[1] == 0xd8 && huvud[2] == 0xff) return "jpeg";
    std::string a = andelse_av(sokvag);
    if (OPAKA.count(a)) return "opak";
    if (TEXTANDELSER.count(a)) return "text";
    // Okänd ändelse: färre än var tjugonde byte utanför det läsbara området
    // räcker för att kalla något text. Text vinner på att hamna bland text.
    size_t prov = std::min<size_t>(n, 4096), udda = 0;
    for (size_t i = 0; i < prov; i++) {
        u8 b = huvud[i];
        if (b < 9 || (b > 13 && b < 32)) udda++;
    }
    if (prov && udda * 20 < prov) return "text";
    return "binar";
}

std::vector<Post> las_trad(const std::string &rot, size_t *overhoppat) {
    std::vector<Post> poster;
    *overhoppat = 0;

    // Bredden först, och kataloger i sorterad ordning: samma ordning varje
    // gång ger samma arkiv varje gång.
    std::vector<std::string> ko = {""};
    while (!ko.empty()) {
        std::string rel = ko.front();
        ko.erase(ko.begin());
        std::string full = rel.empty() ? rot : rot + "/" + rel;

        struct stat st;
        if (lstat(full.c_str(), &st) != 0) throw Fel(full + ": går inte att läsa");
        Post k; k.p = rel; k.t = Typ::Katalog;
        k.m = st.st_mode & 07777;
        k.mt = (i64)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
        poster.push_back(k);

        DIR *d = opendir(full.c_str());
        if (!d) throw Fel(full + ": går inte att öppna");
        std::vector<std::string> namn;
        for (struct dirent *e; (e = readdir(d)); ) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            namn.push_back(e->d_name);
        }
        closedir(d);
        std::sort(namn.begin(), namn.end());

        std::vector<std::string> underkataloger;
        for (const std::string &nm : namn) {
            std::string r = rel.empty() ? nm : rel + "/" + nm;
            std::string f = rot + "/" + r;
            struct stat s2;
            if (lstat(f.c_str(), &s2) != 0) continue;
            i64 mt = (i64)s2.st_mtim.tv_sec * 1000000000 + s2.st_mtim.tv_nsec;
            if (S_ISLNK(s2.st_mode)) {
                // En symlänk TILL EN KATALOG måste fångas här. Går man på
                // kataloger först försvinner den tyst ur arkivet — felet som
                // provsviten hittade i pythonversionen.
                char buf[4096];
                ssize_t l = readlink(f.c_str(), buf, sizeof buf - 1);
                if (l < 0) continue;
                buf[l] = 0;
                Post p; p.p = r; p.t = Typ::Lank; p.mal = buf; p.mt = mt;
                poster.push_back(p);
            } else if (S_ISDIR(s2.st_mode)) {
                underkataloger.push_back(r);
            } else if (S_ISREG(s2.st_mode)) {
                Post p; p.p = r; p.t = Typ::Fil; p.m = s2.st_mode & 07777;
                p.mt = mt; p.s = (u64)s2.st_size;
                poster.push_back(p);
            } else {
                (*overhoppat)++;      // rör, uttag, enheter
            }
        }
        ko.insert(ko.begin(), underkataloger.begin(), underkataloger.end());
    }
    return poster;
}

void satt_metadata(const std::string &sokvag, u32 lage, i64 mtid_ns, bool ar_lank) {
    struct timespec tider[2];
    tider[0].tv_sec = mtid_ns / 1000000000; tider[0].tv_nsec = mtid_ns % 1000000000;
    tider[1] = tider[0];
    if (ar_lank) {
        utimensat(AT_FDCWD, sokvag.c_str(), tider, AT_SYMLINK_NOFOLLOW);
        return;
    }
    if (chmod(sokvag.c_str(), lage) != 0) { /* rättigheter är en önskan */ }
    utimensat(AT_FDCWD, sokvag.c_str(), tider, 0);
}

}  // namespace nvz
