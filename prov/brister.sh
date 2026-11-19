#!/usr/bin/env bash
# De tre bristerna ur Ted Lyngmos kodgranskning 2026-11-18 — som
# regressionsprov. Varje prov är GRÖNT när felet är åtgärdat.
#
#   bash prov/brister.sh
#
# Granskningen är den mest värdefulla återkopplingen projektet fått, och
# poängen med att ha den här är att de tre felen hade en gemensam nämnare:
# packningens egen kontroll vaktar fel gräns. `nvz prova` och
# återskapningsproven packar och packar upp PÅ SAMMA MASKIN, i ett lyckat
# flöde, med läsbara filer och ledig disk. Ingen av dem kunde se något av
# det här.
set -uo pipefail
export LC_ALL=C
ROT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NVZ="$ROT/src/nvz"
ARB="$(mktemp -d /tmp/nvz-brister-XXXXXX)"
fel=0
rensa() { chmod -R u+rwX "$ARB" 2>/dev/null; rm -rf "$ARB"; }
trap rensa EXIT

ok()   { printf '  \033[32mok\033[0m    %s\n' "$1"; }
inte() { printf '  \033[31mFEL\033[0m   %s\n' "$1"; fel=1; }

echo "brist 1: PNG-strömmen verifieras mot sin CRC, och receptet bär motorns avtryck"
# Receptet är fyra zlib-parametrar, och de bestämmer INTE utströmmen — den
# beror på implementationen. Provet lappar receptet i registret (samma
# stränglängd) och prövar båda utfallen: ett recept vars sanning ligger i
# sökrummet ska räddas, ett som inte gör det ska VÄGRAS, aldrig korrumperas.
mkdir -p "$ARB/bilder"
n=0
while IFS= read -r f; do cp "$f" "$ARB/bilder/" 2>/dev/null && n=$((n+1)); [ $n -ge 120 ] && break; done \
    < <(find "$ROT/korpus/png" -name '*.png' 2>/dev/null)
if [ "$n" -eq 0 ]; then
    echo "        (hoppas över: korpus/png saknas)"
else
    "$NVZ" packa "$ARB/bilder" "$ARB/p.nvz" --läge normal --kodek zstd3 -q
    python3 - "$ARB" <<'PY'
import re, subprocess, sys
arb = sys.argv[1]
d = open(f'{arb}/p.nvz','rb').read()
pos = int.from_bytes(d[-16:-8],'little')
packat = d[pos:len(d)-16]
xz = packat[:6] == b'\xfd7zXZ\x00'
rad = subprocess.run(['xz','-dc'] if xz else ['zstd','-dqc'], input=packat, capture_output=True).stdout
m = re.search(rb'(\d+),(\d+),(\d+),(\d+),z[0-9.]+-[0-9a-f]{8}', rad)
if not m:
    open(f'{arb}/inget-recept','w').write('1'); raise SystemExit(0)
niva, fonster = int(m.group(1)), int(m.group(2))
def skriv(nyrad, mal):
    ny = subprocess.run(['xz','-9e','-T1','-c'] if xz else ['zstd','-19','-q','-c'],
                        input=nyrad, capture_output=True).stdout
    open(mal,'wb').write(d[:pos] + ny + pos.to_bytes(8,'little') + len(nyrad).to_bytes(8,'little'))
gammal = m.group(0)
annan_niva = b'%d' % (1 if niva != 1 else 2) + gammal[len(m.group(1)):]
skriv(rad.replace(gammal, annan_niva, 1), f'{arb}/fel-niva.nvz')
annat_f = b'%d,%d' % (niva, 11 if fonster != 11 else 12) + gammal[len(m.group(1))+1+len(m.group(2)):]
skriv(rad.replace(gammal, annat_f, 1), f'{arb}/fel-fonster.nvz')
PY
    if [ -f "$ARB/inget-recept" ]; then
        echo "        (hoppas över: ingen PNG i urvalet gick att veckla ut)"
    else
        rm -rf "$ARB/uA"
        if "$NVZ" packa-upp "$ARB/fel-niva.nvz" "$ARB/uA" -q 2>/dev/null &&
           diff -r "$ARB/bilder" "$ARB/uA/bilder" >/dev/null 2>&1; then
            ok "ett recept som inte återskapar strömmen söks om och räddas bitexakt"
        else
            inte "fallbacken hittar inte rätt parametrar"
        fi
        rm -rf "$ARB/uB"
        utdata=$("$NVZ" packa-upp "$ARB/fel-fonster.nvz" "$ARB/uB" 2>&1); status=$?
        if [ $status -ne 0 ] && grep -q "deflate engine" <<<"$utdata"; then
            ok "ett orimligt recept VÄGRAS med motorns avtryck i felet, aldrig tyst korruption"
        else
            inte "ett orimligt recept gav status $status i stället för ett nej"
        fi
    fi
fi

echo "brist 2: skrivfel ger fel, inte tyst dataförlust"
printf 'data data data\n' > "$ARB/d.txt"
"$NVZ" -c "$ARB/d.txt" > /dev/full 2>/dev/null
[ $? -ne 0 ] && ok "-c mot /dev/full ger slutstatus skild från noll (gzip ger 1)" \
             || inte "-c mot /dev/full gav 0"

mkdir -p "$ARB/full"; head -c 3000000 /dev/urandom | base64 > "$ARB/full/stor.txt"
( cd "$ARB/full" && md5sum stor.txt > fore.md5 )
( cd "$ARB/full" && ulimit -f 200 && "$NVZ" stor.txt >/dev/null 2>&1 )
if [ -f "$ARB/full/stor.txt" ] && ( cd "$ARB/full" && md5sum -c fore.md5 >/dev/null 2>&1 ); then
    ok "originalet överlever ett avbrutet arkiv (filtak), md5 oförändrad"
else
    inte "originalet förlorades när arkivet inte kunde skrivas färdigt"
fi
[ -f "$ARB/full/stor.txt.nv" ] && inte "ett stympat arkiv lämnades kvar" \
                               || ok "det halvskrivna arkivet städades bort"

echo "brist 3: en oläsbar fil stoppar inte resten"
mkdir -p "$ARB/flera"; cd "$ARB/flera"
printf 'aaa\n' > a.txt; printf 'bbb\n' > b.txt; printf 'ccc\n' > c.txt; chmod 000 b.txt
utdata=$("$NVZ" a.txt b.txt c.txt 2>&1); status=$?
chmod 644 b.txt
if [ $status -eq 1 ] && [ -f a.txt.nv ] && [ -f c.txt.nv ] && [ -f b.txt ]; then
    ok "varning, resten packas, slutstatus 1 — gzips kontrakt, ingen core dump"
else
    inte "slutstatus $status (134 = SIGABRT), a.txt.nv=$([ -f a.txt.nv ] && echo ja || echo nej) c.txt.nv=$([ -f c.txt.nv ] && echo ja || echo nej)"
fi

echo
[ $fel -eq 0 ] && echo "Alla tre bristerna är åtgärdade." || echo "NÅGOT HAR GÅTT TILLBAKA."
exit $fel
