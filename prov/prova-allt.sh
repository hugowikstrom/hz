#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# [EN] The 44-edge-case suite: builds a tree of empty files, duplicates,
# symlinks (relative, broken, to directories), newline and non-ASCII file
# names, odd permissions and 1985/2031 timestamps; packs it in every mode
# and demands every byte, permission and timestamp back. Also tests the
# gzip surface contract (exit statuses, -c/-k/-r, stdin) and, when built,
# the GPU part.
# NOTE: this older script keeps its Swedish inline comments for now; it is
# verified by its own runs and is translated as it is next touched.
# Provsvit för nvz. Bygger ett träd av kantfall, packar det, packar upp det
# och kräver att varenda byte och varenda rättighet kommer tillbaka.
#
#   bash prov/prova-allt.sh
#
# Kantfallen är inte påhittade för syns skull. Varje rad här nere motsvarar
# något som faktiskt går sönder i arkivpackare: tomma filer som ser ut som
# saknade, länkar som pekar ut ur trädet, filnamn med nyrad i, filer som är
# exakta kopior av varandra (och som dedupen alltså måste ge tillbaka som två
# riktiga filer, inte som en länk), och rättigheter som tyst blir 644.
set -u
rot="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
arbete="$(mktemp -d)"
trap 'rm -rf -- "$arbete"' EXIT
fel=0
pass() { printf '  \033[32mok\033[0m    %s\n' "$1"; }
brist() { printf '  \033[31mFEL\033[0m   %s\n' "$1"; fel=$((fel+1)); }

printf '\nnvpack provsvit\n\n'

# --- 1. byggstenarna var för sig -------------------------------------------
printf 'Byggstenar\n'

prov=$arbete/bitar; mkdir -p "$prov"
head -c 3000000 /dev/urandom > "$prov/slump.bin"
find "$rot/src" -name '*.c' -o -name '*.py' | xargs cat > "$prov/text.txt" 2>/dev/null

if "$rot/src/nvcm" packa < "$prov/text.txt" | "$rot/src/nvcm" packaupp | cmp -s - "$prov/text.txt"
then pass "nvcm: text fram och tillbaka"; else brist "nvcm: text fram och tillbaka"; fi

if "$rot/src/nvcm" packa < "$prov/slump.bin" | "$rot/src/nvcm" packaupp | cmp -s - "$prov/slump.bin"
then pass "nvcm: inkomprimerbar slump"; else brist "nvcm: inkomprimerbar slump"; fi

: > "$prov/tom"
if "$rot/src/nvcm" packa < "$prov/tom" | "$rot/src/nvcm" packaupp | cmp -s - "$prov/tom"
then pass "nvcm: tom ström"; else brist "nvcm: tom ström"; fi

jpg="$(find "$rot/korpus/foto" -name '*.jpg' 2>/dev/null | head -1)"
if [ -n "$jpg" ]; then
    if "$rot/src/nvjpg" packa < "$jpg" | "$rot/src/nvjpg" packaupp | cmp -s - "$jpg"
    then pass "nvjpg: baseline-jpeg fram och tillbaka"; else brist "nvjpg: baseline-jpeg"; fi
fi

# En PNG utan känd deflate-motor ska avvisas, inte lagras fel.
python3 - "$rot" <<'PY' && pass "png: okänd motor avvisas i stället för att gissa" || brist "png: okänd motor"
import sys, zlib, struct, pathlib
sys.path.insert(0, sys.argv[1] + '/src')
import png
# En PNG vars IDAT packats med en ström ingen zlib-inställning återskapar
# (här: rå deflate med otypisk nivåkombination via två sammanfogade strömmar).
ra = bytes(range(256)) * 64
c = zlib.compressobj(9, zlib.DEFLATED, 15, 9, 0)
ström = c.compress(ra[:100]) + c.flush(zlib.Z_FULL_FLUSH)
c2 = zlib.compressobj(1, zlib.DEFLATED, 15, 9, 0)
ström += c2.compress(ra[100:]) + c2.flush()
def bit(typ, data):
    return struct.pack('>I', len(data)) + typ + data + struct.pack('>I', zlib.crc32(typ + data))
fil = (b'\x89PNG\r\n\x1a\n' + bit(b'IHDR', struct.pack('>IIBBBBB', 16, 16, 8, 6, 0, 0, 0))
       + bit(b'IDAT', ström) + bit(b'IEND', b''))
p = pathlib.Path(sys.argv[1]) / 'prov' / '.tillfällig.png'
p.write_bytes(fil)
try:
    r = png.veckla_ut(str(p))
finally:
    p.unlink()
sys.exit(0 if r is None else 1)
PY

# --- 2. hela trädet med kantfall -------------------------------------------
printf '\nKantfall i trädet\n'
t="$arbete/träd"
mkdir -p "$t/tom-katalog" "$t/djupt/ett/två/tre/fyra" "$t/namn"
: > "$t/noll-byte"
printf 'a' > "$t/en-byte"
printf 'samma innehåll\n' > "$t/dubblett-a"
printf 'samma innehåll\n' > "$t/dubblett-b"
printf 'samma innehåll\n' > "$t/djupt/ett/dubblett-c"
head -c 200000 /dev/urandom > "$t/slump.bin"
head -c 1000000 /dev/zero > "$t/nollor.bin"
seq 1 50000 > "$t/siffror.txt"
printf '#!/bin/sh\necho hej\n' > "$t/skript.sh"; chmod 755 "$t/skript.sh"
chmod 600 "$t/en-byte"; chmod 444 "$t/noll-byte"
printf 'hemlis' > "$t/låst"; chmod 400 "$t/låst"
ln -s ../dubblett-a "$t/djupt/länk-relativ"
ln -s /finns/inte "$t/länk-trasig"
ln -s . "$t/länk-katalog"
printf 'x' > "$t/namn/mellan slag och ÅÄÖ.txt"
printf 'y' > "$t/namn/$(printf 'ny\nrad')"
printf 'z' > "$t/namn/-börjar-med-streck"
printf 'q' > "$t/namn/.dold"
cp "$jpg" "$t/foto.jpg" 2>/dev/null || true
find "$rot/korpus/png" -name '*.png' 2>/dev/null | head -2 | while read -r f; do cp "$f" "$t/bild-$(basename "$f")"; done
touch -d '1985-03-14 12:34:56.123456789' "$t/siffror.txt"
touch -d '2031-01-01 00:00:00' "$t/en-byte"
chmod 700 "$t/tom-katalog"

# gpu-läget provas bara om gpu-delen är byggd OCH enheten svarar. En maskin
# utan vulkan ska köra resten av sviten utan att se ett enda fel.
lagen="snabb normal liten"
if [ -x "$rot/src/nvgpu" ] && "$rot/src/nvgpu" enhet --tyst >/dev/null 2>&1; then
    lagen="$lagen gpu"
fi

for lage in $lagen; do
    ark="$arbete/prov-$lage.nv"; upp="$arbete/upp-$lage"
    if ! "$rot/src/hz" packa "$t" "$ark" -q --läge "$lage" 2>"$arbete/fel.txt"; then
        brist "lage $lage: packningen misslyckades"; sed 's/^/        /' "$arbete/fel.txt"; continue
    fi
    mkdir -p "$upp"
    if ! "$rot/src/hz" packa-upp "$ark" "$upp" -q 2>"$arbete/fel.txt"; then
        brist "lage $lage: uppackningen misslyckades"; sed 's/^/        /' "$arbete/fel.txt"; continue
    fi
    if diff -rq --no-dereference "$t" "$upp/$(basename "$t")" > "$arbete/diff.txt" 2>&1; then
        pass "lage $lage: innehållet identiskt ($(stat -c%s "$ark") byte)"
    else
        brist "lage $lage: innehållet skiljer"; head -5 "$arbete/diff.txt" | sed 's/^/        /'
    fi
    # Rättigheter och tider jämförs separat: diff tittar inte på dem.
    ( cd "$t" && find . -printf '%P %m %T@\n' | sort ) > "$arbete/a.txt"
    ( cd "$upp/$(basename "$t")" && find . -printf '%P %m %T@\n' | sort ) > "$arbete/b.txt"
    if cmp -s "$arbete/a.txt" "$arbete/b.txt"; then
        pass "lage $lage: rättigheter och tider identiska"
    else
        brist "lage $lage: rättigheter eller tider skiljer"
        diff "$arbete/a.txt" "$arbete/b.txt" | head -5 | sed 's/^/        /'
    fi
    # Dubbletter ska komma tillbaka som riktiga filer, inte som hårda länkar.
    if [ "$(stat -c%h "$upp/$(basename "$t")/dubblett-a")" = 1 ]; then
        pass "lage $lage: dedup ger tillbaka fristående filer"
    else
        brist "lage $lage: dedup lämnade hårda länkar efter sig"
    fi
done

# --- 2b. gpu-delen (nvgpu och kodeken nvlz) --------------------------------
# Tre saker provas, och de är de tre som gör gpu-vägen användbar eller inte:
# att hashen är EXAKT pythondelens (annars pekar dedupen fel), att matcharens
# svar är identiska på gpu och cpu (annars betyder tidsjämförelsen inget), och
# att ett nvlz-arkiv går att packa upp UTAN gpu — ett arkiv får inte vara
# gisslan hos en grafikdrivrutin.
printf '\nGpu-delen\n'
if [ ! -x "$rot/src/nvgpu" ]; then
    printf '  --    hoppas över: src/nvgpu är inte byggd (make gpu)\n'
elif ! "$rot/src/nvgpu" enhet --tyst >/dev/null 2>&1; then
    printf '  --    hoppas över: ingen vulkan-enhet svarar här\n'
else
    if "$rot/src/nvgpu" prov --tyst > "$arbete/gpuprov.txt" 2>&1
    then pass "nvgpu prov: testvektorer, gpu==cpu, matchare==cpu"
    else brist "nvgpu prov misslyckades"; sed 's/^/        /' "$arbete/gpuprov.txt" | head -8; fi

    # Hashen mot pythons egen hashlib, på riktiga filer ur src/.
    find "$rot/src" -maxdepth 1 -type f | sort > "$arbete/gpufiler.txt"
    "$rot/src/nvgpu" hasha --tyst < "$arbete/gpufiler.txt" > "$arbete/gpuhash.txt" 2>/dev/null
    if python3 - "$arbete/gpuhash.txt" <<'PYEOF'
import hashlib, sys
fel = 0
for rad in open(sys.argv[1]):
    h, _, sökväg = rad.rstrip('\n').partition('  ')
    with open(sökväg, 'rb') as f:
        if hashlib.blake2b(f.read(), digest_size=16).hexdigest() != h:
            print('skiljer:', sökväg); fel += 1
sys.exit(1 if fel else 0)
PYEOF
    then pass "nvgpu hasha: bitexakt mot hashlib.blake2b(digest_size=16)"
    else brist "nvgpu hasha: avviker från hashlib"; fi

    # Kodeken fram och tillbaka på text, slump och tomt.
    for namn in text.txt slump.bin tom; do
        if "$rot/src/nvgpu" packa --tyst < "$prov/$namn" \
           | "$rot/src/nvgpu" packaupp | cmp -s - "$prov/$namn"
        then pass "nvlz: $namn fram och tillbaka"
        else brist "nvlz: $namn fram och tillbaka"; fi
    done

    # Data som är större än ett stycke (32 MiB) — styckgränsen är där en
    # packare brukar tappa en byte.
    cat "$prov/text.txt" "$prov/text.txt" "$prov/text.txt" "$prov/text.txt" \
        "$prov/text.txt" "$prov/text.txt" > "$arbete/stort.bin" 2>/dev/null
    if [ "$(stat -c%s "$arbete/stort.bin")" -gt 1000 ]; then
        if "$rot/src/nvgpu" packa --tyst < "$arbete/stort.bin" \
           | "$rot/src/nvgpu" packaupp | cmp -s - "$arbete/stort.bin"
        then pass "nvlz: $(( $(stat -c%s "$arbete/stort.bin") / 1048576 )) MiB över styckgränsen"
        else brist "nvlz: flera stycken"; fi
    fi

    # Uppackningen ska klara sig helt utan vulkan. VK_ICD_FILENAMES mot en
    # tom fil gör att lastaren inte hittar någon drivrutin alls.
    "$rot/src/nvgpu" packa --tyst < "$prov/text.txt" > "$arbete/utan.nvlz"
    if VK_ICD_FILENAMES=/dev/null VK_DRIVER_FILES=/dev/null \
       "$rot/src/nvgpu" packaupp < "$arbete/utan.nvlz" | cmp -s - "$prov/text.txt"
    then pass "nvlz: uppackning utan vulkan-drivrutin"
    else brist "nvlz: uppackningen kräver gpu — arkivet blir gisslan"; fi

    # Och packningen ska kunna falla tillbaka på cpu med samma format.
    if VK_ICD_FILENAMES=/dev/null VK_DRIVER_FILES=/dev/null \
       "$rot/src/nvgpu" packa --cpu --tyst < "$prov/text.txt" \
       | "$rot/src/nvgpu" packaupp | cmp -s - "$prov/text.txt"
    then pass "nvlz: packning utan gpu (cpu-matchare, samma format)"
    else brist "nvlz: cpu-fallbacket packar inte"; fi

    # Ett helt arkiv med kodeken tvingad, och registret ska säga nvlz.
    if "$rot/src/hz" packa "$t" "$arbete/nvlz.nv" -q --kodek nvlz 2>"$arbete/fel.txt" \
       && "$rot/src/hz" packa-upp "$arbete/nvlz.nv" "$arbete/upp-nvlz" -q 2>>"$arbete/fel.txt" \
       && diff -rq --no-dereference "$t" "$arbete/upp-nvlz/$(basename "$t")" >/dev/null 2>&1
    then pass "nvz --kodek nvlz: arkivet går fram och tillbaka"
    else brist "nvz --kodek nvlz: arkivet går inte fram och tillbaka"
         head -3 "$arbete/fel.txt" | sed 's/^/        /'; fi
    if "$rot/src/hz" lista "$arbete/nvlz.nv" 2>/dev/null | grep -q nvlz
    then pass "nvz lista: blocken redovisas som nvlz"
    else brist "nvz lista: kodeken syns inte i registret"; fi
fi

# --- 3. trasiga arkiv ska vägra, inte gissa --------------------------------
printf '\nTrasiga arkiv\n'
ark="$arbete/prov-normal.nv"
cp "$ark" "$arbete/trasig.nv"
printf 'FEL' | dd of="$arbete/trasig.nv" bs=1 seek=2 conv=notrunc status=none
if "$rot/src/hz" packa-upp "$arbete/trasig.nv" "$arbete/upp-trasig" -q 2>/dev/null; then
    brist "skadad magi godtogs"
else
    pass "skadad magi avvisas"
fi

# --- 4. gzip-ytan ----------------------------------------------------------
# Ytan är gzips, och det som provas här är just de löftena: originalet
# ersätts, rundan är bit-exakt, flaggorna betyder vad de betyder i gzip, och
# slutstatus skiljer fel (1) från varning (2). Proven körs i en egen katalog
# eftersom hela poängen med ytan är att den skriver i katalogen den står i.
printf '\nKommandoradens gzip-yta\n'

nv="$rot/src/hz"
y="$arbete/yta"; mkdir -p "$y/trad/under"
python3 - "$y" <<'PYEOF'
import os, random, sys
y = sys.argv[1]; random.seed(11)
rader = ''.join(f'def f{i}(x):\n    return x * {i}\n' for i in range(300))
open(f'{y}/trad/kod.py','w').write(rader)
open(f'{y}/trad/under/kopia.py','w').write(rader)        # dubblett: dedupen ska se den
open(f'{y}/trad/slump.bin','wb').write(bytes(random.randrange(256) for _ in range(20000)))
open(f'{y}/ensam.txt','w').write('hej hopp\n' * 1500)
os.symlink('kod.py', f'{y}/trad/lank.py')
PYEOF
chmod 640 "$y/ensam.txt"; touch -d '2021-03-04 05:06:07' "$y/ensam.txt"
sha="$(sha256sum "$y/ensam.txt" | cut -d' ' -f1)"
rattigheter="$(stat -c '%a %Y' "$y/ensam.txt")"
cp -a "$y/trad" "$y/trad-orig"

# En fil: originalet ska vara borta och arkivet ska ärva tid och rättigheter.
( cd "$y" && "$nv" -q ensam.txt )
if [ ! -e "$y/ensam.txt" ] && [ -f "$y/ensam.txt.hz" ]; then
    pass "nvz fil: originalet ersatt av fil.hz"
else brist "nvz fil: originalet ersattes inte"; fi
if [ "$(stat -c '%a %Y' "$y/ensam.txt.hz")" = "$rattigheter" ]; then
    pass "nvz fil: arkivet ärvde tid och rättigheter"
else brist "nvz fil: arkivet fick andra tider än filen"; fi

( cd "$y" && "$nv" -dq ensam.txt.hz )
if [ "$(sha256sum "$y/ensam.txt" | cut -d' ' -f1)" = "$sha" ] \
   && [ "$(stat -c '%a %Y' "$y/ensam.txt")" = "$rattigheter" ] && [ ! -e "$y/ensam.txt.hz" ]; then
    pass "nvz -d: bit-exakt tillbaka, arkivet borta"
else brist "nvz -d: filen kom inte tillbaka oförändrad"; fi

# -c och -k rör inte filen, och strömmen ska gå att packa upp igen.
( cd "$y" && "$nv" -ckq ensam.txt > "$arbete/via-ut.hz" )
if [ -f "$y/ensam.txt" ] && "$nv" -dcq "$arbete/via-ut.hz" | cmp -s - "$y/ensam.txt"; then
    pass "-c -k: filen i fred, arkivet till standard ut"
else brist "-c -k: strömmen eller filen stämmer inte"; fi

# Utan filnamn: standard in -> standard ut, båda vägarna.
if cat "$y/ensam.txt" | "$nv" -9q | "$nv" -dq | cmp -s - "$y/ensam.txt"; then
    pass "standard in -> standard ut, fram och tillbaka"
else brist "röret gav inte tillbaka samma byte"; fi

# En katalog som operand: ETT arkiv av trädet, och trädet lämnas kvar.
( cd "$y" && "$nv" -q trad )
if [ -f "$y/trad.hz" ] && [ -d "$y/trad" ]; then
    pass "katalog: ett arkiv av trädet, källträdet kvar"
else brist "katalog: fel utfall (arkiv saknas eller trädet raderades)"; fi
if "$nv" -tq "$y/trad.hz"; then pass "-t: arkivet provas mot registret"
else brist "-t: provet underkände ett friskt arkiv"; fi
rm -rf "$y/trad"
( cd "$y" && "$nv" -dq trad.hz )
if diff -rq --no-dereference "$y/trad-orig" "$y/trad" >/dev/null 2>&1; then
    pass "-d på trädarkiv: hela trädet tillbaka"
else brist "-d på trädarkiv: trädet skiljer sig"; fi

# -r är gzips bokstavliga beteende: en fil i taget, och upprepbart.
( cd "$y" && "$nv" -rq trad && "$nv" -rq trad )
if [ -f "$y/trad/kod.py.hz" ] && [ ! -e "$y/trad/kod.py" ] && [ -L "$y/trad/lank.py" ]; then
    pass "-r: fil för fil, symlänkar lämnade i fred"
else brist "-r: svepet packade inte som gzip gör"; fi
( cd "$y" && "$nv" -rdq trad )
if diff -rq --no-dereference "$y/trad-orig" "$y/trad" >/dev/null 2>&1; then
    pass "-r -d: svepet tillbaka bit för bit"
else brist "-r -d: svepet gav inte tillbaka trädet"; fi

# Slutstatus: 1 är fel, 2 är varning. Skript hänger på skillnaden.
"$nv" -q "$arbete/finns-inte-alls" 2>/dev/null; [ "$?" = 1 ] \
    && pass "slutstatus 1 när filen inte finns" || brist "fel slutstatus för saknad fil"
# Arvssuffixet: arkiv från nvz-tiden heter .nv och ska packas upp utan byte.
cp "$arbete/via-ut.hz" "$y/arv.nv"
( cd "$y" && "$nv" -dq arv.nv )
if [ -f "$y/arv" ] && cmp -s "$y/arv" "$y/ensam.txt"; then
    pass ".nv-arkiv från nvz-tiden packas upp med suffixet klippt"
else fel ".nv-arvssuffixet"; fi

( cd "$y" && "$nv" -q ensam.txt && "$nv" -kq ensam.txt.hz ) 2>/dev/null; [ "$?" = 2 ] \
    && pass "slutstatus 2 när filen redan är packad" || brist "fel slutstatus för redan packad fil"
( cd "$y" && "$nv" -dq trad-orig/kod.py ) 2>/dev/null; [ "$?" = 2 ] \
    && pass "slutstatus 2 vid okänd ändelse" || brist "fel slutstatus för okänd ändelse"
"$nv" --ingen-sadan-flagga 2>/dev/null; [ "$?" = 1 ] \
    && pass "slutstatus 1 vid okänd flagga" || brist "fel slutstatus för okänd flagga"
"$nv" --l 2>/dev/null; [ "$?" = 1 ] \
    && pass "tvetydig förkortning avvisas" || brist "tvetydig förkortning godtogs"

# Ett trädarkiv är inte en byteström och ska inte låtsas vara det.
( cd "$y" && "$nv" -q trad && "$nv" -dcq trad.hz >/dev/null ) 2>/dev/null; [ "$?" = 1 ] \
    && pass "trädarkiv vägrar skrivas till standard ut" || brist "trädarkiv hälldes ut i en ström"

printf '\n'
if [ "$fel" -eq 0 ]; then printf 'Allt gick igenom.\n\n'; else printf '%d prov misslyckades.\n\n' "$fel"; fi
exit $((fel > 0))
