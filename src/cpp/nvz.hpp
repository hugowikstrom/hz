// nvz — innehållsmedveten arkivpackare, C++-versionen.
//
// Den här filen är de typer och småverktyg resten vilar på. Allt annat —
// kodekar, klassning, trädet, PNG-receptet, arkivet — ligger i egna filer
// och känner bara till det som står här.
//
// Pythonversionen (src/nvz.py) finns kvar i trädet som referens. Den är
// facit: provsviten packar samma träd med båda och kräver samma bytes
// tillbaka. Därför är fältnamnen här medvetet desamma som där.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <optional>
#include <stdexcept>

namespace nvz {

using u8 = uint8_t;  using u16 = uint16_t;
using u32 = uint32_t; using u64 = uint64_t;
using i64 = int64_t;
using Byte = std::vector<u8>;

// Fel som ska nå användaren som ett meddelande, inte som en stackdump.
struct Fel : std::runtime_error {
    explicit Fel(const std::string &m) : std::runtime_error(m) {}
};

// --- posttyper i trädet ----------------------------------------------------
enum class Typ : u8 { Katalog = 'd', Fil = 'f', Lank = 'l' };

struct Post {
    std::string p;        // sökväg relativt roten; tom sträng = roten själv
    Typ t = Typ::Fil;
    u32 m = 0644;         // rättigheter (katalog och fil)
    i64 mt = 0;           // mtime i nanosekunder
    u64 s = 0;            // storlek (fil)
    std::string mal;      // länkmål (länk)
    i64 o = -1;           // objektnummer (fil), -1 = inget
};

// --- objekt: en unik filkropp ---------------------------------------------
// Flera poster kan peka på samma objekt; det är dedupen.
struct Objekt {
    u8 h[16]{};           // blake2b-128 av originalinnehållet
    u64 s = 0;            // rå storlek
    std::string klass;    // text | binar | png | jpeg | opak
    std::string andelse;  // ändelse i gemener, för sorteringen
    std::string kalla;    // sökvägen den lästes från (bara under packning)
    u32 id = 0;

    // Omvandling, om någon användes: "png" eller "jpeg". Kroppen hamnar i ett
    // block och "resten" (PNG-skelett eller JPEG-huvud) i ett annat.
    std::string tr;
    std::string recept;   // PNG: zlib-receptet som återskapar strömmen
    u32 blk = 0;  u64 off = 0, len = 0;          // kroppens plats
    u32 rblk = 0; u64 roff = 0, rlen = 0;        // restens plats
};

struct Block {
    u32 i = 0;
    std::string klass;
    std::string kodek;
    u64 off = 0, clen = 0, ralen = 0;
};

struct Register {
    u32 version = 2;
    std::string rot;
    bool platt = false;
    std::string skapad;
    std::vector<Block> block;
    std::vector<Objekt> objekt;
    std::vector<Post> poster;
    // v1 (python/JSON) bär bara 8 hashbyte per objekt — registret sparade de
    // 16 första HEXTECKNEN. v2 bär alla 16 byte. Kontrollen vid uppackning
    // jämför så många byte registret faktiskt har.
    size_t hashlangd = 16;
};

// --- småverktyg ------------------------------------------------------------
std::string gemener(std::string s);
std::string andelse_av(const std::string &sokvag);   // ".PNG" -> ".png"
std::string hex(const u8 *d, size_t n);
void blake2b128(const u8 *data, size_t n, u8 ut[16]);

Byte las_fil(const std::string &sokvag);
void skriv_fil(const std::string &sokvag, const u8 *d, size_t n);

// Formaterad std::string utan iostream-dans.
std::string strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace nvz
