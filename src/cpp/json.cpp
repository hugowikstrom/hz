#include "json.hpp"
#include <cstdlib>

namespace nvz {
namespace {

struct Reader {
    const char *p, *end;
    void skip() { while (p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; }
    char peek() { skip(); if (p >= end) throw Error("json: unexpected end"); return *p; }
    void expect(char c) { if (peek() != c) throw Error(strf("json: expected %c", c)); p++; }

    std::string string() {
        expect('"');
        std::string out;
        while (p < end && *p != '"') {
            if (*p == '\\') {
                p++;
                if (p >= end) throw Error("json: broken string");
                switch (*p) {
                case 'n': out += '\n'; break;  case 't': out += '\t'; break;
                case 'r': out += '\r'; break;  case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;  case '/': out += '/';  break;
                case '"': out += '"';  break;  case '\\': out += '\\'; break;
                case 'u': {
                    // \uXXXX, with surrogate pairs. Python only writes them
                    // for control characters and (with ensure_ascii) for
                    // everything above ASCII.
                    auto hex4 = [&]() {
                        if (p + 4 >= end) throw Error("json: broken \\u");
                        char buf[5] = {p[1], p[2], p[3], p[4], 0};
                        p += 4;
                        return (unsigned)strtoul(buf, nullptr, 16);
                    };
                    unsigned code = hex4();
                    if (code >= 0xd800 && code < 0xdc00 && p + 6 < end &&
                        p[1] == '\\' && p[2] == 'u') {
                        p += 2;
                        unsigned low = hex4();
                        code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    }
                    if (code < 0x80) out += (char)code;
                    else if (code < 0x800) {
                        out += (char)(0xc0 | (code >> 6));
                        out += (char)(0x80 | (code & 0x3f));
                    } else if (code < 0x10000) {
                        out += (char)(0xe0 | (code >> 12));
                        out += (char)(0x80 | ((code >> 6) & 0x3f));
                        out += (char)(0x80 | (code & 0x3f));
                    } else {
                        out += (char)(0xf0 | (code >> 18));
                        out += (char)(0x80 | ((code >> 12) & 0x3f));
                        out += (char)(0x80 | ((code >> 6) & 0x3f));
                        out += (char)(0x80 | (code & 0x3f));
                    }
                    break;
                }
                default: throw Error("json: unknown escape");
                }
                p++;
            } else {
                out += *p++;
            }
        }
        expect('"');
        return out;
    }

    Jptr value() {
        char c = peek();
        auto v = std::make_shared<Jvalue>();
        if (c == '{') {
            v->k = Jvalue::K::Obj;
            p++;
            if (peek() == '}') { p++; return v; }
            for (;;) {
                std::string key = string();
                expect(':');
                v->obj[key] = value();
                char n = peek(); p++;
                if (n == '}') break;
                if (n != ',') throw Error("json: expected , or }");
            }
        } else if (c == '[') {
            v->k = Jvalue::K::List;
            p++;
            if (peek() == ']') { p++; return v; }
            for (;;) {
                v->list.push_back(value());
                char n = peek(); p++;
                if (n == ']') break;
                if (n != ',') throw Error("json: expected , or ]");
            }
        } else if (c == '"') {
            v->k = Jvalue::K::Str;
            v->s = string();
        } else if (c == 't' || c == 'f') {
            v->k = Jvalue::K::Bool;
            v->b = (c == 't');
            p += (c == 't') ? 4 : 5;
        } else if (c == 'n') {
            v->k = Jvalue::K::Null;
            p += 4;
        } else {
            v->k = Jvalue::K::Num;
            char *after = nullptr;
            v->num = (i64)strtoll(p, &after, 10);
            if (after == p) throw Error("json: expected a number");
            // Floats do not occur in the register, but skip past a decimal
            // part rather than stopping in the middle of one.
            while (after < end && (*after=='.'||*after=='e'||*after=='E'||
                                   *after=='+'||*after=='-'||(*after>='0'&&*after<='9')))
                after++;
            p = after;
        }
        return v;
    }
};

const Jptr NIL = std::make_shared<Jvalue>();

}  // namespace

const Jptr &Jvalue::at(const std::string &key) const {
    auto i = obj.find(key);
    return i == obj.end() ? NIL : i->second;
}

Jptr parse_json(const std::string &text) {
    Reader r{text.data(), text.data() + text.size()};
    return r.value();
}

}  // namespace nvz
