#include "json.hpp"
#include <cstdlib>

namespace nvz {
namespace {

struct Las {
    const char *p, *slut;
    void hoppa() { while (p < slut && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; }
    char kika() { hoppa(); if (p >= slut) throw Fel("json: oväntat slut"); return *p; }
    void vanta(char c) { if (kika() != c) throw Fel(strf("json: väntade %c", c)); p++; }

    std::string strang() {
        vanta('"');
        std::string ut;
        while (p < slut && *p != '"') {
            if (*p == '\\') {
                p++;
                if (p >= slut) throw Fel("json: trasig sträng");
                switch (*p) {
                case 'n': ut += '\n'; break;  case 't': ut += '\t'; break;
                case 'r': ut += '\r'; break;  case 'b': ut += '\b'; break;
                case 'f': ut += '\f'; break;  case '/': ut += '/';  break;
                case '"': ut += '"';  break;  case '\\': ut += '\\'; break;
                case 'u': {
                    // \uXXXX, med surrogatpar. Python skriver dem bara för
                    // styrtecken och (om ensure_ascii) för allt över ascii.
                    auto hexfyra = [&]() {
                        if (p + 4 >= slut) throw Fel("json: trasig \\u");
                        char buf[5] = {p[1], p[2], p[3], p[4], 0};
                        p += 4;
                        return (unsigned)strtoul(buf, nullptr, 16);
                    };
                    unsigned kod = hexfyra();
                    if (kod >= 0xd800 && kod < 0xdc00 && p + 6 < slut &&
                        p[1] == '\\' && p[2] == 'u') {
                        p += 2;
                        unsigned lag = hexfyra();
                        kod = 0x10000 + ((kod - 0xd800) << 10) + (lag - 0xdc00);
                    }
                    if (kod < 0x80) ut += (char)kod;
                    else if (kod < 0x800) {
                        ut += (char)(0xc0 | (kod >> 6));
                        ut += (char)(0x80 | (kod & 0x3f));
                    } else if (kod < 0x10000) {
                        ut += (char)(0xe0 | (kod >> 12));
                        ut += (char)(0x80 | ((kod >> 6) & 0x3f));
                        ut += (char)(0x80 | (kod & 0x3f));
                    } else {
                        ut += (char)(0xf0 | (kod >> 18));
                        ut += (char)(0x80 | ((kod >> 12) & 0x3f));
                        ut += (char)(0x80 | ((kod >> 6) & 0x3f));
                        ut += (char)(0x80 | (kod & 0x3f));
                    }
                    break;
                }
                default: throw Fel("json: okänd escape");
                }
                p++;
            } else {
                ut += *p++;
            }
        }
        vanta('"');
        return ut;
    }

    Jptr varde() {
        char c = kika();
        auto v = std::make_shared<Jvarde>();
        if (c == '{') {
            v->k = Jvarde::K::Objekt;
            p++;
            if (kika() == '}') { p++; return v; }
            for (;;) {
                std::string nyckel = strang();
                vanta(':');
                v->objekt[nyckel] = varde();
                char n = kika(); p++;
                if (n == '}') break;
                if (n != ',') throw Fel("json: väntade , eller }");
            }
        } else if (c == '[') {
            v->k = Jvarde::K::Lista;
            p++;
            if (kika() == ']') { p++; return v; }
            for (;;) {
                v->lista.push_back(varde());
                char n = kika(); p++;
                if (n == ']') break;
                if (n != ',') throw Fel("json: väntade , eller ]");
            }
        } else if (c == '"') {
            v->k = Jvarde::K::Strang;
            v->s = strang();
        } else if (c == 't' || c == 'f') {
            v->k = Jvarde::K::Bool;
            v->b = (c == 't');
            p += (c == 't') ? 4 : 5;
        } else if (c == 'n') {
            v->k = Jvarde::K::Null;
            p += 4;
        } else {
            v->k = Jvarde::K::Tal;
            char *efter = nullptr;
            v->tal = (i64)strtoll(p, &efter, 10);
            if (efter == p) throw Fel("json: väntade ett tal");
            // Flyttal förekommer inte i registret, men hoppa förbi en
            // decimaldel hellre än att stanna mitt i den.
            while (efter < slut && (*efter=='.'||*efter=='e'||*efter=='E'||
                                    *efter=='+'||*efter=='-'||(*efter>='0'&&*efter<='9')))
                efter++;
            p = efter;
        }
        return v;
    }
};

const Jptr NOLL = std::make_shared<Jvarde>();

}  // namespace

const Jptr &Jvarde::at(const std::string &nyckel) const {
    auto i = objekt.find(nyckel);
    return i == objekt.end() ? NOLL : i->second;
}

Jptr las_json(const std::string &text) {
    Las l{text.data(), text.data() + text.size()};
    return l.varde();
}

}  // namespace nvz
