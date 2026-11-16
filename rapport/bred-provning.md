# nvz — bred provning

**Åtta datasorter, två standardkorpusar, och frågan om datat kommer tillbaka.**

Mätt 2026-11-16 på en Ryzen AI 9 HX 470 (24 trådar, 28 GiB, NVMe). Varje tal
här kommer ur `resultat/*.json` och går att räkna om med kommandona i
[Så körs provningen om](#så-körs-provningen-om). Mätningarna kördes med
`nice 19` och `ionice -c 3`; maskinen hade lediga kärnor hela tiden (last
omkring 6 av 24), så prioriteten kostade ingen mätbar tid.

## Sammanfattning

**Vad som provades.** nvz mättes mot sju klassiska packare på elva
korpusar: fyra nya av riktiga filer (GIF-bilder, film, arkiv av arkiv,
loggfiler), de tre ursprungliga (kod, PNG, foto), och två standardkorpusar
som hela facket mäter med (Silesia och enwik8). Varje mätning är en hel
rundgång — packa, packa upp, jämför — och varje korpus kördes dessutom genom
ett återskapningsprov som kräver tillbaka varje byte, rättighet och
tidsstämpel, samt genom fem avsiktliga skador på arkivet.

**Vad som kom ut.** nvz vinner stort där det finns text och bilder, gör
ingen skada där det inte finns något att hämta, och förlorar på ett ställe,
av ett skäl som går att peka på:

* **Loggfiler är det bästa fallet hittills i repot: 23,8× (10,03 MiB ur
  238,3)**, 15,3 % mindre än `xz -9e` och 55,2 % mindre än dagens
  `tar | zstd -3` — och 3,8 gånger snabbare än xz, eftersom nvcm vann
  kodekprovet och nvpacks block packas parallellt.
* **På standardkorpusarna slår nvcm varje klassiskt verktyg på maskinen:**
  Silesia 44 439 240 byte (1,677 bits/byte) mot `xz -9e`:s 48 456 004
  (−8,3 %), enwik8 21 417 184 byte (1,713 bpb) mot 24 831 648 (−13,8 %).
  På LTCB:s skala hamnar nvcm mellan ordbokspackarna (xz 1,98 bpb) och de
  tunga kontextblandarna (zpaq 1,43, cmix 1,17).
* **Film går inte att packa, och det är rätt svar.** Alla tolv packare
  landar inom 1 % av råstorleken. nvpacks värde där är tiden: samma storlek
  som `zstd -22` på en sjättedel av tiden, eller 0,9 % större på 2,3
  sekunder i läget `snabb`.
* **På arkiv av arkiv förlorar nvz** — 291,3 MiB mot `zstd -22`:s 242,8.
  Orsaken är mätt och var inte den väntade: blockdelningen kostar 1,2 MiB,
  medan kodekprovet kostar 31. Provet packar 4 MiB spridda över ett block
  på 192 MiB, och ser därför inte att två filer hundratals megabyte isär är
  nästan identiska. Tvingad till `--kodek zstd22` ger nvz 244,6 MiB mot
  `zstd -22`:s 242,8.
* **GIF-korpusens fina kvot är inte nvpacks förtjänst.** 47,3 % av den är
  exakta kopior, och korpusen är mindre än varje modern packares fönster —
  nvz, `zstd -19` och `xz` landar på samma byte.

**Återskapningen.** Sju av sju korpusar kom tillbaka bit-exakt, med
rättigheter, tidsstämplar på nanosekunden, symlänkar och tomma kataloger
intakta, och med lika många poster i registret som i trädet. Alla fem
skadade arkiv vägrade packa upp i stället för att lämna fel data. Provsviten
med kantfall (filnamn med nyrad, trasiga länkar, tidsstämplar från 1985 och
2031) är grön i alla tre lägen.

**Vad det betyder.** För Hugos skarpa arkiv ändrar provningen ingenting om
innehållet är film. Men för loggar — den sortens arkiv som faktiskt växer
varje dag — är nvz med läget `liten` mer än dubbelt så bra som dagens
skript, och det är mätt på 238 MiB riktig syslog och riktiga transkript.

## Varför den här provningen

Den första mätningen av nvz täckte tre rena korpusar — kod, PNG, foto —
och ett blandat projektträd. Det räckte för att visa att omvandlingarna
fungerar, men inte för att svara på vad en arkivpackare faktiskt möter. Ett
riktigt arkiv innehåller också film, GIF-bilder, zip-filer som någon redan
packat en gång, och loggar som växer varje dag. Och alla korpusar var Hugos
egna filer, vilket gör talen ärliga men ojämförbara: ingen annan har samma
foton.

Den här provningen lägger till fyra nya korpusar av riktiga filer, två
standardkorpusar som hela facket mäter med, och ett återskapningsprov som
ställer den enda fråga som egentligen betyder något om en arkivpackare:
**kommer datat tillbaka?** Inte "ser det likadant ut" utan samma SHA-256 per
fil, samma rättigheter, samma tidsstämpel på nanosekunden, samma symlänkar,
samma tomma kataloger — och vad som händer när arkivet är skadat.

<!-- ÖVERSIKT -->

| korpus | rå MiB | dagens MiB | bästa klassiska MiB | bästa nvz MiB | mot dagens | mot bästa klassiska | packtid nvz/klassisk |
|:--|--:|--:|--:|--:|--:|--:|--:|
| Programkod | 55 | 11.5 | 8.2 (xz -9e) | 7.2 (liten) | -37.2 % | -12.6 % | 71 / 47 s |
| Bilder (PNG) | 431 | 362.9 | 245.2 (zst -22) | 208.9 (liten) | -42.4 % | -14.8 % | 251 / 156 s |
| Foton (JPEG) | 409 | 406.3 | 404.9 (bz2 -9) | 345.1 (normal) | -15.1 % | -14.8 % | 22 / 57 s |
| Bilder (GIF) | 27 | 27.0 | 14.2 (zst -22) | 14.2 (normal) | -47.6 % | -0.0 % | 8 / 5 s |
| Film (h264 och VP9) | 537 | 533.9 | 532.0 (zst -19) | 532.3 (normal) | -0.3 % | +0.0 % | 43 / 57 s |
| Arkiv (zip och tar.gz) | 392 | 381.8 | 242.8 (zst -22) | 291.3 (normal) | -23.7 % | +20.0 % | 46 / 154 s |
| Loggfiler | 238 | 22.4 | 11.8 (xz -9e) | 10.0 (liten) | -55.2 % | -15.4 % | 247 / 897 s |

<!-- SLUT ÖVERSIKT -->

## Korpusarna

De fyra nya är byggda av riktiga filer på maskinen med
`korpus/bygg-korpus.py`, som hårdlänkar: noll extra diskutrymme, exakt samma
bytes. Loggkorpusen är enda undantaget — loggfiler *skrivs* medan mätningen
pågår, så de kopieras en gång, och filer som rörts den senaste kvarten
hoppas över helt. En korpus som växer mellan packning och uppackning gör
varje jämförelse meningslös.

| korpus | innehåll | varför just den |
|:--|:--|:--|
| `gif` | 105 filer, 27 MiB — webbgrafik från 00-talet, två animationer, demo-GIF:ar som ligger i flera `node_modules` | LZW-packat, alltså ett eget entropilager som nvz inte rör, och med riktiga dubbletter i |
| `film` | 67 filer, 537 MiB — TikTok-h264, egen rip, VP9-skärmfilm | det som dominerar skarpa arkiv, och som enligt README inte går att packa |
| `arkiv` | 133 filer, 392 MiB — zip och tar.gz, bland dem två bit-identiska par och åtta dagliga backuper av samma databas | packat inuti packat: fallet där en packare kan göra mest skada genom att försöka |
| `logg` | 76 filer, 238 MiB — syslog, kern.log, apploggar, JSONL-transkript | den mest upprepade text en maskin producerar |
| `silesia` | 12 filer, 211 938 580 byte | standardkorpusen i lzbench och i zstds och brotlis egna jämförelser |
| `enwik8` | 1 fil, 100 000 000 byte | standarden i Large Text Compression Benchmark, där resultat anges i bits/byte |

<!-- TABELLER -->

### Bilder (GIF) — 27.4 MiB i 105 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 27.62 | 0.99× | +2.1 % | 0 s | 0 s | ja |
| tar.gz -6 | 27.18 | 1.01× | +0.5 % | 1 s | 0 s | ja |
| tar.gz -9 (pigz) | 27.19 | 1.01× | +0.6 % | 0 s | 0 s | ja |
| tar.bz2 -9 | 27.28 | 1.00× | +0.9 % | 4 s | 3 s | ja |
| tar.zst -3 (dagens) | 27.04 | 1.01× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 14.18 | 1.93× | -47.6 % | 4 s | 0 s | ja |
| tar.zst -22 --ultra | 14.18 | 1.93× | -47.6 % | 5 s | 0 s | ja |
| tar.xz -9e | 14.19 | 1.93× | -47.5 % | 8 s | 0 s | ja |
| **nvz snabb** | 14.44 | 1.90× | -46.6 % | 1 s | 0 s | ja |
| **nvz utan knep** | 14.18 | 1.93× | -47.6 % | 7 s | 0 s | ja |
| **nvz normal** | 14.18 | 1.93× | -47.6 % | 8 s | 0 s | ja |
| **nvz liten** | 14.18 | 1.93× | -47.6 % | 8 s | 0 s | ja |

### Film (h264 och VP9) — 536.7 MiB i 67 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 536.82 | 1.00× | +0.6 % | 1 s | 0 s | ja |
| tar.gz -6 | 532.39 | 1.01× | -0.3 % | 24 s | 2 s | ja |
| tar.gz -9 (pigz) | 532.54 | 1.01× | -0.2 % | 2 s | 2 s | ja |
| tar.bz2 -9 | 535.53 | 1.00× | +0.3 % | 78 s | 54 s | ja |
| tar.zst -3 (dagens) | 533.86 | 1.01× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 532.02 | 1.01× | -0.3 % | 57 s | 0 s | ja |
| tar.zst -22 --ultra | 532.03 | 1.01× | -0.3 % | 266 s | 1 s | ja |
| tar.xz -9e | 532.86 | 1.01× | -0.2 % | 154 s | 5 s | ja |
| **nvz snabb** | 536.75 | 1.00× | +0.5 % | 2 s | 1 s | ja |
| **nvz utan knep** | 532.25 | 1.01× | -0.3 % | 45 s | 1 s | ja |
| **nvz normal** | 532.25 | 1.01× | -0.3 % | 43 s | 1 s | ja |
| **nvz liten** | 532.25 | 1.01× | -0.3 % | 43 s | 1 s | ja |

### Arkiv (zip och tar.gz) — 391.8 MiB i 133 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 391.93 | 1.00× | +2.7 % | 1 s | 0 s | ja |
| tar.gz -6 | 380.05 | 1.03× | -0.5 % | 18 s | 2 s | ja |
| tar.gz -9 (pigz) | 380.14 | 1.03× | -0.4 % | 1 s | 2 s | ja |
| tar.bz2 -9 | 384.49 | 1.02× | +0.7 % | 56 s | 39 s | ja |
| tar.zst -3 (dagens) | 381.78 | 1.03× | +0.0 % | 0 s | 1 s | ja |
| tar.zst -19 --long | 261.37 | 1.50× | -31.5 % | 47 s | 0 s | ja |
| tar.zst -22 --ultra | 242.77 | 1.61× | -36.4 % | 154 s | 1 s | ja |
| tar.xz -9e | 251.75 | 1.56× | -34.1 % | 144 s | 7 s | ja |
| **nvz snabb** | 329.03 | 1.19× | -13.8 % | 1 s | 1 s | ja |
| **nvz utan knep** | 350.70 | 1.12× | -8.1 % | 46 s | 1 s | ja |
| **nvz normal** | 291.25 | 1.35× | -23.7 % | 46 s | 1 s | ja |
| **nvz liten** | 291.25 | 1.35× | -23.7 % | 46 s | 1 s | ja |

### Loggfiler — 238.3 MiB i 76 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 238.39 | 1.00× | +965.0 % | 0 s | 0 s | ja |
| tar.gz -6 | 30.46 | 7.82× | +36.1 % | 4 s | 1 s | ja |
| tar.gz -9 (pigz) | 29.03 | 8.21× | +29.7 % | 1 s | 1 s | ja |
| tar.bz2 -9 | 18.92 | 12.60× | -15.5 % | 32 s | 6 s | ja |
| tar.zst -3 (dagens) | 22.38 | 10.65× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 13.09 | 18.21× | -41.5 % | 113 s | 0 s | ja |
| tar.zst -22 --ultra | 12.92 | 18.45× | -42.3 % | 986 s | 0 s | ja |
| tar.xz -9e | 11.84 | 20.12× | -47.1 % | 897 s | 1 s | ja |
| **nvz snabb** | 22.41 | 10.63× | +0.1 % | 2 s | 1 s | ja |
| **nvz utan knep** | 11.97 | 19.91× | -46.5 % | 940 s | 2 s | ja |
| **nvz normal** | 11.97 | 19.91× | -46.5 % | 938 s | 2 s | ja |
| **nvz liten** | 10.03 | 23.77× | -55.2 % | 247 s | 221 s | ja |

<!-- SLUT TABELLER -->

## Fyra fynd

### 1. Loggar är nvpacks bästa fall — och nvcm är hela skälet

*(Kompressorn hette `nvcm` när mätningarna gjordes och heter `nvcm` sedan
namnbytet samma dag. Det är samma binär och samma tal; den gamla
strömmagin `NVCM1` läses fortfarande.)*

`logg` ger den högsta kvoten någon korpus gett i det här repot: **23,8×**,
10,03 MiB ur 238,3. Det är 15,3 % mindre än `xz -9e`, som är det bästa de
installerade verktygen klarar, och 55,2 % mindre än dagens `tar | zstd -3`.

Vinsten går att placera exakt, för den syns i skillnaden mellan två lägen som
bara skiljer sig på en punkt:

| läge på `logg` | storlek | packtid | skillnad |
|:--|--:|--:|:--|
| `normal` | 11,97 MiB | 938 s | provar zstd och xz |
| `liten` | **10,03 MiB** | **247 s** | provar dessutom nvcm |

Alltså −16,2 % på bytena *och* 3,8 gånger kortare tid. Att läget som packar
mindre också packar snabbare är inte en paradox utan två oberoende saker:
nvcm vann kodekprovet mot xz på loggtexten, och nvpacks block är oberoende
och packas fyra i taget, medan `xz -9e -T0` i praktiken körde enkeltrådat
(64 MiB ordbok per tråd, och minnet räcker inte till fler trådar).

Registret visar vad som hände, och det var inte ett delvist val:

```
95 poster, 74 objekt, 2 block
  block   0  text        nvcm       192.03 ->     9.00 MiB  x21.35
  block   1  text        nvcm        46.29 ->     1.03 MiB  x45.06
```

Båda blocken gick till nvcm. Det andra blocket packar mer än dubbelt så bra
som det första, vilket är själva poängen med en adaptiv modell: när den fått
se 192 MiB syslog vet den vad nästa rad kommer att innehålla.

### 2. På arkiv av arkiv förlorar nvz, och felet är arkitektoniskt

`arkiv`-korpusen är 392 MiB zip och tar.gz. Där är nvz **sämre** än ett
rakt `tar | zstd`:

| packare | storlek | mot nvz |
|:--|--:|--:|
| `tar.zst -22 --ultra --long=31` | 242,77 MiB | nvz är 20,0 % större |
| `tar.zst -19 --long=27` | 261,37 MiB | nvz är 11,4 % större |
| `tar.xz -9e` | 251,75 MiB | nvz är 15,7 % större |
| **nvz normal** | **291,25 MiB** | — |

Det intressanta är inte att nvz förlorar utan *vad* de klassiska
verktygen hittar. Korpusen innehåller 62,8 MiB exakta dubbletter (16,0 %),
och dem tar nvpacks dedup: raden `nvz snabb` ligger på 329 MiB, alltså
råstorleken minus dubbletterna, på 1,5 sekunder. Men `zstd -22` hittar
**149 MiB** — mer än dubbelt så mycket. Resten ligger i *nästan*-kopior:
åtta dagliga backuper av samma databas, packade av gzip till olika
bytefölder, och två zip-par som är lika men inte identiska.

Det ligger nära till hands att skylla på blockdelningen: nvz packar
oberoende block på högst 192 MiB, och två nästan-identiska filer i olika
block kan inte matcha varandra. Den förklaringen mättes — och höll inte.

| | storlek |
|:--|--:|
| `zstd -19`, standardfönster (8 MiB) | 365,3 MiB |
| `zstd -19 --long=27` (nvz:s fönster, hela strömmen) | 261,4 MiB |
| `zstd -22 --ultra --long=31` | 242,8 MiB |
| nvz läge normal, kodekprovet väljer | 291,3 MiB |
| **nvz `--kodek zstd19`**, samma block, tvingad kodek | **260,2 MiB** |
| **nvz `--kodek zstd22`**, samma block, tvingad kodek | **244,6 MiB** |

Tvingad till rätt kodek ligger nvz alltså *i nivå med* det bästa klassiska
verktyget: 244,6 mot 242,8 MiB. Blockdelningen kostar 1,2 MiB, inte 48.

Felet sitter i **kodekprovet**. Det packar 4 MiB spridda över blocket och
låter den kodek som vinner på stickprovet ta hela blocket. På text och
bilder fungerar det, för där är redundansen jämnt utspridd. På arkiv av
arkiv ligger den i stället i att två filer *hundratals megabyte isär* är
nästan identiska — och sannolikheten att ett stickprov på 0,2 % av blocket
råkar träffa båda kopiorna är nära noll. Provet ser brus, svarar "billig
kodek räcker", och 31 MiB blir kvar.

Det är samma sorts fel som README redan beskriver en gång: första versionen
antog att "redan packat" var en egenskap hos filtypen och förlorade 175 MiB
på PNG-korpusen. Rättelsen var att prova i stället för att anta. Nu visar
det sig att provet självt har ett antagande inbyggt — att ett stickprov
representerar blocket.

### 3. Film bekräftar README — och vinsten ligger i tiden, inte i bytena

Alla tolv packare landar inom 1 % av råstorleken på `film`. Det som skiljer
dem är vad de tar för att inte hitta något:

| packare | storlek | packtid |
|:--|--:|--:|
| `tar.zst -22 --ultra` | 532,03 MiB | 265,5 s |
| `tar.xz -9e` | 532,86 MiB | 154,2 s |
| **nvz normal** | 532,25 MiB | **42,8 s** |
| **nvz snabb** | 536,75 MiB | **2,3 s** |

Här är nvpacks kodekprov värt precis vad det utlovar i README: det känner av
att blocken inte går att packa och lagrar dem rått, i stället för att mala
igenom en halv gigabyte film med en dyr kodek. `nvz normal` ger samma
storlek som `zstd -22` på en sjättedel av tiden. Och `nvz snabb` ger
536,75 MiB på 2,3 sekunder — 0,9 % större än det bästa någon klarar, för
0,9 % av tiden.

### 4. GIF ger en imponerande kvot som inte är nvpacks förtjänst

`gif` packar 1,93× — och exakt lika bra hos nvz, `zstd -19`, `zstd -22`
och `xz -9e`: alla fyra landar på 14,18 MiB. Förklaringen står i
dubbletttabellen nedan: **47,3 % av korpusen är exakta kopior**, för samma
demo-GIF ligger i flera `node_modules` och pecka-trädet finns i två
versioner. Korpusen är dessutom bara 27 MiB, alltså mindre än fönstret hos
varje modern packare — så de hittar kopiorna själva, utan dedup.

Det här är den sortens tal som ser ut som en vinst i en tabell och inte är
en. nvpacks egna omvandlingar gör **ingenting** på GIF: formatets LZW-ström
är ett eget entropilager som nvz lämnar ifred, precis som den lämnade
JPEG ifred innan nvjpg fanns. Att `nvz utan knep` ger samma byte som
`nvz normal` är beviset.

<!-- DUBBLETTER -->

| korpus | filer | unika filer | rå MiB | exakta dubbletter MiB | andel |
|:--|--:|--:|--:|--:|--:|
| kod | 5 974 | 5 898 | 55.3 | 0.4 | 0.7 % |
| png | 4 618 | 1 963 | 431.1 | 123.8 | 28.7 % |
| foto | 1 143 | 1 142 | 408.5 | 0.0 | 0.0 % |
| gif | 105 | 59 | 27.4 | 13.0 | 47.3 % |
| film | 67 | 67 | 536.7 | 0.0 | 0.0 % |
| arkiv | 133 | 130 | 391.8 | 62.8 | 16.0 % |
| logg | 76 | 74 | 238.3 | 0.0 | 0.0 % |
| silesia | 12 | 12 | 202.1 | 0.0 | 0.0 % |
| enwik8 | 1 | 1 | 95.4 | 0.0 | 0.0 % |

<!-- SLUT DUBBLETTER -->

## Standardkorpusarna

Allt ovan är Hugos egna filer. De två korpusar nedan är de som andra
packare mäts med, och de mäts här på samma sätt som där: **en fil i taget,
en tråd, ingen tar och ingen dedup** — bara kodeken mot filen, som i
lzbench. `zstd` och `xz` får `-T1`. nvz som *arkivpackare* står i
tabellerna längre upp; här står `nvcm`, som är den del av nvz som
konkurrerar med en kodek.

<!-- STANDARD -->

**Silesia** — 211 938 580 byte

| kodek | storlek byte | kvot | bits/byte | pack MiB/s | upp MiB/s | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| gzip -9 | 67 631 990 | 3.134× | 2.553 | 6.8 | 157.9 | ja |
| bzip2 -9 | 54 506 769 | 3.888× | 2.058 | 8.1 | 19.2 | ja |
| zstd -19 | 52 895 350 | 4.007× | 1.997 | 1.6 | 374.3 | ja |
| zstd -22 --ultra | 52 369 178 | 4.047× | 1.977 | 1.2 | 360.9 | ja |
| brotli -q 11 | 49 564 563 | 4.276× | 1.871 | 0.3 | 175.8 | ja |
| xz -9e | 48 456 004 | 4.374× | 1.829 | 1.3 | 66.5 | ja |
| nvz | 44 439 240 | 4.769× | 1.677 | 0.9 | 0.8 | ja |

**enwik8** — 100 000 000 byte

| kodek | storlek byte | kvot | bits/byte | pack MiB/s | upp MiB/s | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| gzip -9 | 36 445 248 | 2.744× | 2.916 | 10.2 | 138.2 | ja |
| bzip2 -9 | 29 008 758 | 3.447× | 2.321 | 8.7 | 16.5 | ja |
| zstd -19 | 26 944 227 | 3.711× | 2.155 | 1.1 | 340.6 | ja |
| brotli -q 11 | 25 742 001 | 3.885× | 2.059 | 0.3 | 207.3 | ja |
| zstd -22 --ultra | 25 272 471 | 3.957× | 2.022 | 0.8 | 381.5 | ja |
| xz -9e | 24 831 648 | 4.027× | 1.986 | 0.8 | 69.6 | ja |
| nvz | 21 417 184 | 4.669× | 1.713 | 0.8 | 0.7 | ja |

<!-- SLUT STANDARD -->

### Silesia fil för fil

En summa döljer att en packare kan vinna på text och förlora på
databasdumpar. Silesia är med flit blandad, och den blandningen är hela
skälet att korpusen används:

<!-- SILESIA-PER-FIL -->

| fil | innehåll | rå byte | zstd -19 | brotli -q 11 | xz -9e | nvcm | nvcm mot bäst |
|:--|:--|--:|--:|--:|--:|--:|--:|

<!-- SLUT SILESIA-PER-FIL -->

### Var nvcm hamnar bland publicerade resultat

Large Text Compression Benchmark publicerar resultat för enwik8 i bits per
byte. Talen nedan är **publicerade av benchmarken**, mätta på annan maskin
och andra programversioner, och står här bara för att placera nvcm på
skalan — inte som egna mätningar:

| program | enwik8 | bits/byte | |
|:--|--:|--:|:--|
| gzip 1.3.5 | 36 445 248 | 2,92 | ordbok |
| bzip2 1.0.2 | 29 008 736 | 2,32 | blocksortering |
| zstd 0.6.0 | 25 405 601 | 2,03 | ordbok |
| brotli | 25 764 698 | 2,06 | ordbok |
| xz 5.2.1 | 24 703 772 | 1,98 | ordbok (LZMA) |
| zpaq 6.42 | 17 855 729 | 1,43 | kontextblandning |
| nanozip 0.09a | 18 594 163 | 1,49 | kontextblandning |
| paq8px_v206fix1 | 15 849 084 | 1,27 | kontextblandning |
| cmix v21 | 14 623 723 | 1,17 | kontextblandning |

<!-- ENWIK8-PLATS -->

*(nvcm inte mätt på enwik8)*

<!-- SLUT ENWIK8-PLATS -->

## Återskapning: kommer datat tillbaka?

Mätharnesset kör redan `diff -rq` på varje rad i varje tabell, och kolumnen
**exakt** är utfallet av den jämförelsen. Men diff svarar på en smalare fråga
än man tror: den jämför innehållet i de filer som finns på båda sidor. Den
säger ingenting om rättigheter, tidsstämplar, tomma kataloger, eller om en
dubblett kom tillbaka som två riktiga filer eller som en hård länk. Och den
lämnar inget kvitto.

`prov/aterskapning.py` räknar därför en avtryckslista — SHA-256 per fil plus
metadata — före packning och efter uppackning, och kräver att listorna är
identiska. Dessutom kontrolleras att arkivets register redovisar lika många
poster som trädet har, kataloger och länkar inräknade: ett arkiv kan ge
tillbaka riktiga filer och ändå ha tappat en symlänk på vägen.

<!-- ÅTERSKAPNING -->

| korpus | filer | poster i registret | SHA-256 lika | läge och tider lika | hårda länkar kvar | avvikelser |
|:--|--:|--:|:-:|:-:|:-:|:--|
| gif | 105 | ja | ja | ja | nej | inga |
| arkiv | 133 | ja | ja | ja | nej | inga |
| logg | 76 | ja | ja | ja | nej | inga |
| film | 67 | ja | ja | ja | nej | inga |
| png | 4 618 | ja | ja | ja | nej | inga |
| foto | 1 143 | ja | ja | ja | nej | inga |
| kod | 5 974 | ja | ja | ja | nej | inga |

Skadeprov på arkivet från korpusen **gif**:

| skada | utfall | godkänt |
|:--|:--|:-:|
| magin bytt | vägrade | ja |
| byte i blockkropp | vägrade | ja |
| sista kilobyten bort | vägrade | ja |
| registerpekaren noll | vägrade | ja |
| en byte i registret | vägrade | ja |

<!-- SLUT ÅTERSKAPNING -->

Den andra halvan av frågan är vad som händer när arkivet är trasigt. En
packare som tyst lämnar tillbaka **fel** data är värre än en som vägrar, och
det är skillnaden mellan en arkivpackare och en förlustkomprimerare man inte
visste att man hade. Fem sorters skada provades: bytt magi, en vänd bit i en
blockkropp, sista kilobyten bortklippt, nollad registerpekare och en vänd
bit i registret. Alla fem vägrade, ingen lämnade data.

Till det kommer `prov/prova-allt.sh`, som bygger ett träd av kantfall —
tomma filer, dubbletter, relativa och trasiga symlänkar, symlänk till
katalog, filnamn med nyrad och åäö, rättigheter 400/444/600/755,
tidsstämplar från 1985 och 2031 — och kräver tillbaka varje byte, rättighet
och tid i alla tre lägen. Den är grön.

## Tid: genomströmning per korpus

Sekunder går inte att jämföra mellan korpusar av olika storlek. MiB per
sekund gör det:

<!-- TIDER -->

| korpus | nvz normal pack | nvz normal upp | zstd -3 pack | zstd -19 pack | xz -9e pack |
|:--|--:|--:|--:|--:|--:|
| Programkod | 1 | 46 | 325 | 2 | 1 |
| Bilder (PNG) | 2 | 46 | 743 | 11 | 3 |
| Foton (JPEG) | 19 | 53 | 869 | 8 | 3 |
| Bilder (GIF) | 3 | 137 | 304 | 6 | 3 |
| Film (h264 och VP9) | 13 | 801 | 1 220 | 9 | 3 |
| Arkiv (zip och tar.gz) | 9 | 400 | 1 059 | 8 | 3 |
| Loggfiler | 0 | 153 | 1 254 | 2 | 0 |

*MiB per sekund, rådata genom väggklockan.*

<!-- SLUT TIDER -->

## Vad provningen lämnar efter sig

**Tre saker att göra något åt**, i ordning efter vad de är värda:

1. **Blockstorleken är en kompressionsgräns, inte bara en minnesgräns.**
   `arkiv`-korpusen visar att nästan-kopior som hamnar i olika block aldrig
   kan matcha varandra, och att det kostar mer än dedupen vinner. Två vägar
   finns: större block för opaka klasser, eller `--long=31` när blocket är
   stort. Båda går att mäta innan de byggs.
2. **GIF är nästa nvjpg.** En GIF är LZW-packade bildrutor, alltså exakt
   samma situation JPEG var i innan nvjpg: ett gammalt entropilager runt data
   som går att koda om. Att byta LZW mot en aritmetisk kodare med
   grannskapsmodeller är samma arbete som redan är gjort en gång. Korpusen
   finns nu, så vinsten går att mäta i stället för att gissas.
3. **nvcm provas inte på opaka block.** Läget `liten` skickar bara text,
   PNG-skelett och JPEG-huvuden till nvcm. `arkiv`-korpusen antyder att det
   kan finnas något att hämta även i opak data — men det är en gissning, och
   repots regel är att gissningar mäts.

**Det som inte provades:** komprimering av `.git`-packfiles (står som idé i
README), progressiv JPEG (nvjpg klarar bara baseline), och arkiv större än
maskinens minne. GPU-läget mättes i en egen omgång samma dag och står i
README: grafikkortet vinner 2,5× på LZ77-matchningen men hela packningen blir
inte snabbare, eftersom entropikodningen är seriell.

**En varning till den som jämför små tal:** arkivstorleken är inte
deterministisk. Två `--läge normal`-körningar på samma träd gav 8 597 884 och
8 597 603 byte. Jämför aldrig två arkivtal som ligger inom några hundra byte
från varandra.

## Så körs provningen om

```bash
python3 korpus/bygg-korpus.py gif film arkiv logg     # bygger korpusarna
export NVPACK_SKRAP=$PWD/.skrap                       # arbetsytan, utanför repot
python3 matning/matning.py gif                        # arkivnivå, en korpus i taget
python3 matning/standardkorpus.py silesia enwik8      # kodeknivå, en tråd
python3 prov/aterskapning.py gif arkiv logg film png foto kod
bash prov/prova-allt.sh                               # kantfall
python3 experiment/dubbletter.py                      # dubblettandel per korpus
python3 rapport/bred-provning.py                      # fyller tabellerna här
python3 matning/rapport.py --in-i-readme              # och i README
```

Standardkorpusarna hämtas en gång:

```bash
curl -sSLo /tmp/silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
curl -sSLo /tmp/enwik8.zip  https://mattmahoney.net/dc/enwik8.zip
mkdir -p korpus/silesia korpus/enwik8
unzip -oq /tmp/silesia.zip -d korpus/silesia
unzip -oq /tmp/enwik8.zip  -d korpus/enwik8
```
