#!/usr/bin/env python3
"""Genererar blake2b.comp — den utrullade blake2b-kärnan.

Varför generera: en array som indexeras med något annat än en konstant läggs
i scratch-minne på en AMD-gpu, och blake2b:s tillstånd (16 ord) plus
meddelandet (16 ord) indexeras av sigma-tabellen. Utrullat blir varje index
en konstant och allt ligger i register. Skillnaden är ungefär fem gånger.

Kör `python3 src/gen-blake2b.py` (eller `make`) efter en ändring här.
"""
from pathlib import Path

SIGMA = [
 [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],
 [14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3],
 [11,8,12,0,5,2,15,13,10,14,3,6,7,1,9,4],
 [7,9,3,1,13,12,11,14,2,6,5,10,4,0,15,8],
 [9,0,5,7,2,4,10,15,14,1,11,12,6,8,3,13],
 [2,12,6,10,0,11,8,3,4,13,7,5,15,14,1,9],
 [12,5,1,15,14,13,4,10,0,7,6,3,9,2,8,11],
 [13,11,7,14,12,1,3,9,5,0,15,4,8,6,2,10],
 [6,15,14,9,11,3,0,8,12,2,13,7,1,4,10,5],
 [10,2,8,4,7,6,1,5,15,11,9,14,3,12,13,0],
 [0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15],
 [14,10,4,8,9,15,13,6,1,12,0,2,11,7,5,3]]

# De åtta G-anropen per varv: fyra kolumner, sedan fyra diagonaler.
PAR = [(0,4,8,12),(1,5,9,13),(2,6,10,14),(3,7,11,15),
       (0,5,10,15),(1,6,11,12),(2,7,8,13),(3,4,9,14)]


def rundor() -> str:
    rader = []
    for r in range(12):
        rader.append(f'    /* varv {r} */')
        for k, (a, b, c, d) in enumerate(PAR):
            x, y = SIGMA[r][2*k], SIGMA[r][2*k+1]
            rader.append(f'    G(v{a}, v{b}, v{c}, v{d}, m{x}, m{y});')
    return '\n'.join(rader)


KALLA = '''#version 450
#extension GL_ARB_gpu_shader_int64 : require
/* blake2b-128, en tråd per fil. Måste ge BIT FÖR BIT samma svar som
 * hashlib.blake2b(digest_size=16) i pythondelen — dedupen och
 * uppackningens objektkontroll hänger på det.
 *
 * Varven är utrullade och tillståndet ligger i namngivna variabler, inte i
 * arrayer: en array som indexeras med något annat än en konstant hamnar i
 * scratch-minne på en AMD-gpu. Den här filen är därför GENERERAD av
 * src/gen-blake2b.py — rör den inte för hand.
 *
 * En tråd per fil betyder att lastbalansen följer filstorlekarna: tusentals
 * små filer fyller maskinen fint, men en enda stor fil körs av EN tråd och
 * då är gpu:n hopplös. Anroparen delar därför upp stora filer själv. */
layout(local_size_x_id = 0) in;
layout(constant_id = 0) const uint GRUPP = 64;

layout(std430, binding = 0) readonly buffer Data {{ uint d[]; }};
layout(std430, binding = 1) readonly buffer Jobb {{ uvec2 j[]; }};   /* (ordoffset, bytelängd) */
layout(std430, binding = 2) writeonly buffer Ut  {{ uvec4 u[]; }};   /* 16 byte per jobb */

layout(push_constant) uniform P {{ uint antal; }} p;

const uint64_t IV0 = 0x6a09e667f3bcc908ul, IV1 = 0xbb67ae8584caa73bul,
               IV2 = 0x3c6ef372fe94f82bul, IV3 = 0xa54ff53a5f1d36f1ul,
               IV4 = 0x510e527fade682d1ul, IV5 = 0x9b05688c2b3e6c1ful,
               IV6 = 0x1f83d9abfb41bd6bul, IV7 = 0x5be0cd19137e2179ul;

uint64_t rotr(uint64_t x, uint n) {{ return (x >> n) | (x << (64u - n)); }}

#define G(a, b, c, dd, x, y) \\
    a = a + b + (x); dd = rotr(dd ^ a, 32u); \\
    c = c + dd;      b  = rotr(b ^ c, 24u); \\
    a = a + b + (y); dd = rotr(dd ^ a, 16u); \\
    c = c + dd;      b  = rotr(b ^ c, 63u)

uint64_t h0, h1, h2, h3, h4, h5, h6, h7;
{mdecl}

void komprimera(uint64_t t, bool sista) {{
    uint64_t v0 = h0, v1 = h1, v2 = h2, v3 = h3, v4 = h4, v5 = h5, v6 = h6, v7 = h7;
    uint64_t v8 = IV0, v9 = IV1, v10 = IV2, v11 = IV3;
    uint64_t v12 = IV4 ^ t, v13 = IV5, v14 = sista ? ~IV6 : IV6, v15 = IV7;
{rundor}
    h0 ^= v0 ^ v8;  h1 ^= v1 ^ v9;  h2 ^= v2 ^ v10; h3 ^= v3 ^ v11;
    h4 ^= v4 ^ v12; h5 ^= v5 ^ v13; h6 ^= v6 ^ v14; h7 ^= v7 ^ v15;
}}

/* Ett 32-bitsord ur jobbet, nollat utanför längden. Jobben ligger
 * ordjusterade, så orden läses rakt. */
uint ord(uint bas, uint w, uint len) {{
    uint b = w * 4u;
    if (b + 4u <= len) return d[bas + w];
    if (b >= len) return 0u;
    uint kvar = len - b;
    return d[bas + w] & ((1u << (kvar * 8u)) - 1u);
}}

void main() {{
    uint jag = gl_GlobalInvocationID.x;
    if (jag >= p.antal) return;
    uint bas = j[jag].x, len = j[jag].y;

    h0 = IV0 ^ 0x01010010ul;   /* fanout 1, djup 1, nyckellängd 0, utlängd 16 */
    h1 = IV1; h2 = IV2; h3 = IV3; h4 = IV4; h5 = IV5; h6 = IV6; h7 = IV7;

    uint64_t t = 0ul;
    uint i = 0u;
    while (len - i > 128u) {{            /* alla hela block UTOM det sista */
        uint w = i >> 2u;
{hel}
        t += 128ul;
        komprimera(t, false);
        i += 128u;
    }}
    uint w = i >> 2u;
{sist}
    t += uint64_t(len - i);
    komprimera(t, true);

    u[jag] = uvec4(uint(h0 & 0xfffffffful), uint(h0 >> 32u),
                   uint(h1 & 0xfffffffful), uint(h1 >> 32u));
}}
'''


def main() -> None:
    src = KALLA.format(
        mdecl='\n'.join(f'uint64_t m{i};' for i in range(16)),
        rundor=rundor(),
        hel='\n'.join(f'        m{i} = uint64_t(d[bas + w + {2*i}u]) '
                      f'| (uint64_t(d[bas + w + {2*i+1}u]) << 32u);' for i in range(16)),
        sist='\n'.join(f'    m{i} = uint64_t(ord(bas, w + {2*i}u, len)) '
                       f'| (uint64_t(ord(bas, w + {2*i+1}u, len)) << 32u);' for i in range(16)))
    mal = Path(__file__).resolve().parent / 'blake2b.comp'
    mal.write_text(src)
    print(f'{mal}: {len(src.splitlines())} rader')


if __name__ == '__main__':
    main()
