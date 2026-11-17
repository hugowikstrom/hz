# nvz — bred provning

**Empirisk utvärdering av en innehållsmedveten arkivpackare: kompressionskvot,
tidsåtgång och korrekthet över åtta datatyper och två standardkorpusar.**

*2026-11-16 · Ryzen AI 9 HX 470 (24 trådar), 28 GiB RAM, NVMe ·
källkod, mätdata och denna rapport: <https://github.com/hugowikstrom/nvz> ·
alla tal genereras ur `resultat/*.json` och kan reproduceras enligt appendix A*

## Sammanfattning

nvz är en arkivpackare som klassificerar sitt innehåll och tillämpar
innehållsspecifika, förlustfria omvandlingar — deduplicering, likhetssortering,
PNG-utveckling med receptåtersökning, JPEG-omkodning till aritmetisk kod samt
kontextblandande kompression (nvcm) — innan den väljer kodek per block genom
stickprov. Denna rapport utvärderar nvz mot sju etablerade packare på elva
korpusar och prövar två hypoteser:

- **H1 (kompression):** innehållsmedvetenhet ger mätbart mindre arkiv än
  generisk strömkompression på data med utnyttjbar struktur, utan att
  försämra resultatet på data som saknar sådan.
- **H2 (korrekthet):** varje arkiv återger källträdet bitexakt — innehåll,
  rättigheter, tidsstämplar, symboliska länkar — och ett skadat arkiv
  avvisas i stället för att ge felaktiga data.

Resultaten stödjer båda hypoteserna med en kvantifierad avvikelse från H1.
I sammandrag:

| observation | mätvärde | referens |
|:--|--:|:--|
| loggfiler, kvot | **23,8×** (10,03 av 238,3 MiB) | xz −9e: 20,1× |
| loggfiler, relativt bästa klassiska | **−15,3 %** | mot xz −9e |
| Silesia, nvcm (en tråd) | **1,677 bit/byte** | xz −9e: 1,829 |
| enwik8, nvcm (en tråd) | **1,713 bit/byte** | xz −9e: 1,986 |
| film, samtliga packare | inom 1 % av råstorlek | väntat: redan kodad |
| arkiv-av-arkiv, nvz mot zstd −22 | **−0,1 %** efter åtgärd (+20,0 % före) | felsökt och rättat, avsnitt 4.2 |
| återskapning, 7 korpusar | bitexakt, samtliga poster | avsnitt 5 |
| skadeprov, 5 skadetyper | samtliga avvisade | avsnitt 5.2 |

Den ursprungligen uppmätta avvikelsen från H1 (+20,0 % på korpusen av
redan komprimerade arkiv) felsöktes till kodekprovets stickprovsurval,
åtgärdades under provningen och ommättes: med regelvalet i avsnitt 4.2 är
nvz 0,1 % *mindre* än bästa klassiska verktyg även där. H1 står därmed
utan känd motinstans i materialet.

## 1. Inledning

En konventionell arkivkedja (`tar | zstd`) behandlar trädet som en oordnad
byteström: komprimeraren ser varken fildubbletter utanför sitt fönster,
inbäddade komprimeringslager (deflate i PNG, Huffman i JPEG) eller
möjligheten att placera likartade filer intill varandra. nvz utnyttjar
samtliga tre, och varje omvandling verifieras vid packning genom fullständig
återsyntes och jämförelse mot originalet; en omvandling som inte kan bevisas
reversibel faller tillbaka till rålagring.

Tidigare mätningar omfattade tre renodlade korpusar (programkod, PNG,
JPEG-foton) och ett blandat projektträd. De besvarar inte hur packaren beter
sig på datatyper utan utnyttjbar struktur (video, GIF, redan packade arkiv),
på loggdata med hög redundans, eller i jämförelse med publicerade resultat.
Föreliggande provning utvidgar underlaget med fyra korpusar av verklig data,
två standardkorpusar (Silesia, enwik8), samt en systematisk
korrekthetsprövning.

## 2. Metod

### 2.1 Jämförelsepackare

Sju konfigurationer av fyra etablerade verktyg: gzip (−6, −9 via pigz),
bzip2 (−9), zstd (−3, −19 `--long=27`, −22 `--ultra --long=31`) och
xz (−9e). `tar | zstd −3` betecknas *dagens* och utgör referensnivå, då den
motsvarar maskinens befintliga arkivskript. nvz mäts i lägena `snabb`
(zstd −3, inga omvandlingar), `normal` (omvandlingar, kodekprov per block)
och `liten` (därtill nvcm som kandidat; standardläge), samt som `utan knep`
(läge normal med deduplicering och omvandlingar avslagna) för att isolera
omvandlingarnas bidrag.

### 2.2 Mätförfarande

Varje mätpunkt är en fullständig cykel: packning, uppackning till tom
katalog, rekursiv jämförelse mot källträdet (`diff -rq --no-dereference`).
Kolumnen *exakt* i tabellerna är utfallet av denna jämförelse, inte ett
antagande. Tider är väggklocketid; packning får använda samtliga 24 trådar.
Mätningarna kördes med `nice 19` och `ionice -c 3` på en i övrigt lastad
maskin med lediga kärnor; kontrollmätningar visade ingen mätbar påverkan av
prioriteten. Kodeknivåmätningen (avsnitt 3.3) följer i stället fackets
konvention: en fil i taget, en tråd, ingen tar och ingen deduplicering.

Arkivstorleken är inte deterministisk mellan körningar: två identiska
`--läge normal`-körningar gav 8 597 884 respektive 8 597 603 byte
(skillnad 3·10⁻⁵). Differenser inom några hundra byte är därför inte
signifikanta.

### 2.3 Korpusar

De sex nedan kompletterar de tidigare tre (kod 55,3 MiB/5 974 filer,
PNG 431,1/4 618, foto 408,5/1 143). Lokala korpusar byggs som hårda länkar
av `korpus/bygg-korpus.py`; loggkorpusen kopieras i stället en gång, eftersom
loggfiler skrivs under mätningen och en föränderlig korpus ogiltigförklarar
cykeljämförelsen. Filer ändrade de senaste 15 minuterna utesluts.

| korpus | omfång | innehåll | motivering |
|:--|--:|:--|:--|
| `gif` | 105 filer, 27,4 MiB | webbgrafik, animationer, dubbletter i flera `node_modules` | LZW-kodat entropilager som nvz inte öppnar; hög dubblettandel |
| `film` | 67 filer, 536,7 MiB | h264 (TikTok), VP9 (skärminspelning) | redan transformkodad data; förväntat nollresultat |
| `arkiv` | 133 filer, 391,8 MiB | zip och tar.gz, varav två bitidentiska par och åtta dagliga säkerhetskopior av samma databas | komprimerad data med redundans *mellan* filer |
| `logg` | 76 filer, 238,3 MiB | syslog, kern.log, apploggar, JSONL-transkript | maximalt repetitiv text; växande arkivtyp |
| `silesia` | 12 filer, 211 938 580 B | Silesia-korpusen | jämförbarhet: standard i lzbench m.fl. |
| `enwik8` | 1 fil, 10⁸ B | engelska Wikipedia, första 10⁸ byte | jämförbarhet: Large Text Compression Benchmark |

Dubblettandelen per korpus kvantifieras i tabellen nedan, eftersom den
avgör hur stor del av en kvot som förklaras av deduplicering snarare än
kompression.

<!-- DUBBLETTER -->

| korpus | filer | unika filer | rå MiB | exakta dubbletter MiB | andel |
|:--|--:|--:|--:|--:|--:|
| kod | 5 974 | 5 898 | 55,3 | 0,4 | 0,7 % |
| png | 4 618 | 1 963 | 431,1 | 123,8 | 28,7 % |
| foto | 1 143 | 1 142 | 408,5 | 0,0 | 0,0 % |
| gif | 105 | 59 | 27,4 | 13,0 | 47,3 % |
| film | 67 | 67 | 536,7 | 0,0 | 0,0 % |
| arkiv | 133 | 130 | 391,8 | 62,8 | 16,0 % |
| logg | 76 | 74 | 238,3 | 0,0 | 0,0 % |
| silesia | 12 | 12 | 202,1 | 0,0 | 0,0 % |
| enwik8 | 1 | 1 | 95,4 | 0,0 | 0,0 % |

<!-- SLUT DUBBLETTER -->

## 3. Resultat

### 3.1 Översikt

Tabellen sammanfattar bästa nvz-läge mot referensnivån och mot det bästa
klassiska verktyget per korpus. Negativa värden anger mindre arkiv.

<!-- ÖVERSIKT -->

| korpus | rå MiB | dagens MiB | bästa klassiska MiB | bästa nvz MiB | mot dagens | mot bästa klassiska | packtid nvz/klassisk |
|:--|--:|--:|--:|--:|--:|--:|--:|
| Programkod | 55 | 11,5 | 8,2 (xz -9e) | 7,3 (liten) | −36,5 % | −11,6 % | 70 / 47 s |
| Bilder (PNG) | 431 | 362,9 | 245,2 (zst -22) | 208,8 (liten) | −42,5 % | −14,8 % | 265 / 156 s |
| Foton (JPEG) | 409 | 406,3 | 404,9 (bz2 -9) | 345,1 (normal) | −15,1 % | −14,8 % | 49 / 57 s |
| Bilder (GIF) | 27 | 27,0 | 14,2 (zst -22) | 14,2 (normal) | −47,6 % | −0,0 % | 8 / 5 s |
| Film (h264 och VP9) | 537 | 533,9 | 532,0 (zst -19) | 532,0 (normal) | −0,3 % | +0,0 % | 61 / 57 s |
| Arkiv (zip och tar.gz) | 392 | 381,8 | 242,8 (zst -22) | 242,5 (liten) | −36,5 % | −0,1 % | 192 / 154 s |
| Loggfiler | 238 | 22,4 | 11,8 (xz -9e) | 10,0 (liten) | −55,2 % | −15,3 % | 197 / 897 s |
| Silesia (arkivnivå) | 202 | 63,1 | 46,2 (xz -9e) | 43,5 (liten) | −31,0 % | −5,7 % | 102 / 158 s |
| enwik8 (arkivnivå) | 95 | 33,8 | 23,7 (xz -9e) | 20,4 (liten) | −39,6 % | −13,7 % | 131 / 121 s |

<!-- SLUT ÖVERSIKT -->

### 3.2 Hela matrisen

Samtliga packare över samtliga korpusar, i tre paneler: storlek, packtid,
uppacktid. Kolumnerna silesia och enwik8 avser arkivnivån (hela korpusen
genom tar-ekvivalent väg), inte kodeknivån i 3.4.

<!-- MATRIS -->

**Arkivstorlek**

| packare | kod | png | foto | gif | film | arkiv | logg | silesia | enwik8 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| tar (okomprimerad) | 60,8 | 438,4 | 409,4 | 27,6 | 536,8 | 391,9 | 238,4 | 202,1 | 95,4 |
| tar.gz -6 | 12,3 | 401,7 | 405,8 | 27,2 | 532,4 | 380,0 | 30,5 | 65,1 | 34,8 |
| tar.gz -9 (pigz) | 12,2 | 401,9 | 406,0 | 27,2 | 532,5 | 380,1 | 29,0 | 64,5 | 34,8 |
| tar.bz2 -9 | 9,5 | 385,4 | 404,9 | 27,3 | 535,5 | 384,5 | 18,9 | 52,0 | 27,7 |
| tar.zst -3 (dagens) | 11,5 | 362,9 | 406,3 | 27,0 | 533,9 | 381,8 | 22,4 | 63,1 | 33,8 |
| tar.zst -19 --long | 8,5 | 252,7 | 405,1 | 14,2 | **532,0** | 261,4 | 13,1 | 50,2 | 25,2 |
| tar.zst -22 --ultra | 8,5 | 245,2 | 405,1 | 14,2 | 532,0 | 242,8 | 12,9 | 49,8 | 24,1 |
| tar.xz -9e | 8,2 | 266,4 | 405,4 | 14,2 | 532,9 | 251,7 | 11,8 | 46,2 | 23,7 |
| **nvz snabb** | 11,4 | 279,3 | 408,5 | 14,4 | 536,8 | 329,0 | 22,4 | 63,2 | 33,8 |
| **nvz utan knep** | 8,3 | 256,5 | 406,4 | 14,2 | 532,0 | 242,5 | 12,0 | 46,2 | 23,7 |
| **nvz normal** | 8,3 | 208,8 | **345,1** | **14,2** | 532,0 | 242,5 | 12,0 | 46,2 | 23,7 |
| **nvz liten** | **7,3** | **208,8** | **345,1** | **14,2** | 532,0 | **242,5** | **10,0** | **43,5** | **20,4** |

*MiB per korpus; minsta värde per kolumn i fet. Råstorlekar: kod 55, png 431, foto 409, gif 27, film 537, arkiv 392, logg 238, silesia 202, enwik8 95 MiB.*

**Packtid**

| packare | kod | png | foto | gif | film | arkiv | logg | silesia | enwik8 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| tar (okomprimerad) | **0** | 3 | 2 | 0 | 1 | 1 | 0 | **0** | **0** |
| tar.gz -6 | 2 | 20 | 19 | 1 | 24 | 18 | 4 | 12 | 7 |
| tar.gz -9 (pigz) | 0 | 2 | 1 | 0 | 2 | 1 | 1 | 2 | 1 |
| tar.bz2 -9 | 7 | 74 | 57 | 4 | 78 | 56 | 32 | 24 | 11 |
| tar.zst -3 (dagens) | 0 | **1** | **0** | **0** | **0** | **0** | **0** | 0 | 0 |
| tar.zst -19 --long | 35 | 40 | 53 | 4 | 57 | 47 | 113 | 63 | 61 |
| tar.zst -22 --ultra | 56 | 156 | 225 | 5 | 266 | 154 | 986 | 179 | 118 |
| tar.xz -9e | 47 | 128 | 157 | 8 | 154 | 144 | 897 | 158 | 121 |
| **nvz snabb** | 1 | 2 | 1 | 0 | 3 | 2 | 2 | 1 | 2 |
| **nvz utan knep** | 41 | 42 | 12 | 11 | 59 | 191 | 854 | 94 | 125 |
| **nvz normal** | 41 | 268 | 49 | 8 | 61 | 203 | 864 | 94 | 125 |
| **nvz liten** | 70 | 265 | 55 | 9 | 60 | 192 | 197 | 102 | 131 |

*s per korpus; minsta värde per kolumn i fet. Råstorlekar: kod 55, png 431, foto 409, gif 27, film 537, arkiv 392, logg 238, silesia 202, enwik8 95 MiB.*

**Uppacktid**

| packare | kod | png | foto | gif | film | arkiv | logg | silesia | enwik8 |
|:--|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| tar (okomprimerad) | **0** | **0** | **0** | **0** | **0** | **0** | **0** | **0** | **0** |
| tar.gz -6 | 0 | 3 | 2 | 0 | 2 | 2 | 1 | 1 | 1 |
| tar.gz -9 (pigz) | 0 | 3 | 2 | 0 | 2 | 2 | 1 | 1 | 1 |
| tar.bz2 -9 | 2 | 44 | 41 | 3 | 54 | 39 | 6 | 10 | 6 |
| tar.zst -3 (dagens) | 0 | 0 | **0** | **0** | 0 | 1 | 0 | 0 | 0 |
| tar.zst -19 --long | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| tar.zst -22 --ultra | 0 | 1 | 0 | 0 | 1 | 1 | 0 | 0 | 0 |
| tar.xz -9e | 1 | 5 | 4 | 0 | 5 | 7 | 1 | 3 | 1 |
| **nvz snabb** | 0 | 1 | 1 | 0 | 2 | 1 | 1 | 1 | 1 |
| **nvz utan knep** | 1 | 1 | 1 | 0 | 2 | 1 | 2 | 2 | 2 |
| **nvz normal** | 1 | 10 | 17 | 0 | 2 | 1 | 2 | 2 | 2 |
| **nvz liten** | 68 | 9 | 17 | 0 | 2 | 1 | 203 | 101 | 135 |

*s per korpus; minsta värde per kolumn i fet. Råstorlekar: kod 55, png 431, foto 409, gif 27, film 537, arkiv 392, logg 238, silesia 202, enwik8 95 MiB.*

<!-- SLUT MATRIS -->

### 3.3 Per korpus

Fullständiga serier per korpus. *Mot dagens* anger storleksförändring
relativt `tar.zst −3`.

<!-- TABELLER -->

### Bilder (GIF) — 27.4 MiB i 105 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 27,62 | 0,99× | +2,1 % | 0 s | 0 s | ja |
| tar.gz -6 | 27,18 | 1,01× | +0,5 % | 1 s | 0 s | ja |
| tar.gz -9 (pigz) | 27,19 | 1,01× | +0,6 % | 0 s | 0 s | ja |
| tar.bz2 -9 | 27,28 | 1,00× | +0,9 % | 4 s | 3 s | ja |
| tar.zst -3 (dagens) | 27,04 | 1,01× | +0,0 % | **0 s** | 0 s | ja |
| tar.zst -19 --long | 14,18 | 1,93× | −47,6 % | 4 s | 0 s | ja |
| tar.zst -22 --ultra | 14,18 | 1,93× | −47,6 % | 5 s | 0 s | ja |
| tar.xz -9e | 14,19 | 1,93× | −47,5 % | 8 s | 0 s | ja |
| **nvz snabb** | 14,44 | 1,90× | −46,6 % | 0 s | 0 s | ja |
| **nvz utan knep** | 14,18 | 1,93× | −47,6 % | 11 s | 0 s | ja |
| **nvz normal** | **14,18** | 1,93× | −47,6 % | 8 s | 0 s | ja |
| **nvz liten** | **14,18** | 1,93× | −47,6 % | 9 s | 0 s | ja |

### Film (h264 och VP9) — 536.7 MiB i 67 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 536,82 | 1,00× | +0,6 % | 1 s | 0 s | ja |
| tar.gz -6 | 532,39 | 1,01× | −0,3 % | 24 s | 2 s | ja |
| tar.gz -9 (pigz) | 532,54 | 1,01× | −0,2 % | 2 s | 2 s | ja |
| tar.bz2 -9 | 535,53 | 1,00× | +0,3 % | 78 s | 54 s | ja |
| tar.zst -3 (dagens) | 533,86 | 1,01× | +0,0 % | **0 s** | 0 s | ja |
| tar.zst -19 --long | **532,02** | 1,01× | −0,3 % | 57 s | 0 s | ja |
| tar.zst -22 --ultra | 532,03 | 1,01× | −0,3 % | 266 s | 1 s | ja |
| tar.xz -9e | 532,86 | 1,01× | −0,2 % | 154 s | 5 s | ja |
| **nvz snabb** | 536,75 | 1,00× | +0,5 % | 3 s | 2 s | ja |
| **nvz utan knep** | 532,04 | 1,01× | −0,3 % | 59 s | 2 s | ja |
| **nvz normal** | 532,04 | 1,01× | −0,3 % | 61 s | 2 s | ja |
| **nvz liten** | 532,04 | 1,01× | −0,3 % | 60 s | 2 s | ja |

### Arkiv (zip och tar.gz) — 391.8 MiB i 133 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 391,93 | 1,00× | +2,7 % | 1 s | 0 s | ja |
| tar.gz -6 | 380,05 | 1,03× | −0,5 % | 18 s | 2 s | ja |
| tar.gz -9 (pigz) | 380,14 | 1,03× | −0,4 % | 1 s | 2 s | ja |
| tar.bz2 -9 | 384,49 | 1,02× | +0,7 % | 56 s | 39 s | ja |
| tar.zst -3 (dagens) | 381,78 | 1,03× | +0,0 % | **0 s** | 1 s | ja |
| tar.zst -19 --long | 261,37 | 1,50× | −31,5 % | 47 s | 0 s | ja |
| tar.zst -22 --ultra | 242,77 | 1,61× | −36,4 % | 154 s | 1 s | ja |
| tar.xz -9e | 251,75 | 1,56× | −34,1 % | 144 s | 7 s | ja |
| **nvz snabb** | 329,03 | 1,19× | −13,8 % | 2 s | 1 s | ja |
| **nvz utan knep** | 242,53 | 1,62× | −36,5 % | 191 s | 1 s | ja |
| **nvz normal** | 242,53 | 1,62× | −36,5 % | 203 s | 1 s | ja |
| **nvz liten** | **242,53** | 1,62× | −36,5 % | 192 s | 1 s | ja |

### Loggfiler — 238.3 MiB i 76 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 238,39 | 1,00× | +965,0 % | 0 s | 0 s | ja |
| tar.gz -6 | 30,46 | 7,82× | +36,1 % | 4 s | 1 s | ja |
| tar.gz -9 (pigz) | 29,03 | 8,21× | +29,7 % | 1 s | 1 s | ja |
| tar.bz2 -9 | 18,92 | 12,60× | −15,5 % | 32 s | 6 s | ja |
| tar.zst -3 (dagens) | 22,38 | 10,65× | +0,0 % | **0 s** | 0 s | ja |
| tar.zst -19 --long | 13,09 | 18,21× | −41,5 % | 113 s | 0 s | ja |
| tar.zst -22 --ultra | 12,92 | 18,45× | −42,3 % | 986 s | 0 s | ja |
| tar.xz -9e | 11,84 | 20,12× | −47,1 % | 897 s | 1 s | ja |
| **nvz snabb** | 22,41 | 10,63× | +0,1 % | 2 s | 1 s | ja |
| **nvz utan knep** | 11,97 | 19,91× | −46,5 % | 854 s | 2 s | ja |
| **nvz normal** | 11,97 | 19,91× | −46,5 % | 864 s | 2 s | ja |
| **nvz liten** | **10,03** | 23,77× | −55,2 % | 197 s | 203 s | ja |

<!-- SLUT TABELLER -->

### 3.4 Standardkorpusar, kodeknivå

Jämförelsen avser kompressorn nvcm isolerad — en fil i taget, en tråd —
eftersom publicerade tal för dessa korpusar är definierade så. nvz som
arkivpackare ingår i 3.1–3.3.

<!-- STANDARD -->

**Silesia** — 211 938 580 byte

| kodek | storlek byte | kvot | bits/byte | pack MiB/s | upp MiB/s | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| gzip -9 | 67 631 990 | 3,134× | 2,553 | 6,8 | 157,9 | ja |
| bzip2 -9 | 54 506 769 | 3,888× | 2,058 | 8,1 | 19,2 | ja |
| zstd -19 | 52 895 350 | 4,007× | 1,997 | 1,6 | 374,3 | ja |
| zstd -22 --ultra | 52 369 178 | 4,047× | 1,977 | 1,2 | 360,9 | ja |
| brotli -q 11 | 49 564 563 | 4,276× | 1,871 | 0,3 | 175,8 | ja |
| xz -9e | 48 456 004 | 4,374× | 1,829 | 1,3 | 66,5 | ja |
| **nvcm** | 44 439 240 | 4,769× | 1,677 | 0,9 | 0,8 | ja |

**enwik8** — 100 000 000 byte

| kodek | storlek byte | kvot | bits/byte | pack MiB/s | upp MiB/s | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| gzip -9 | 36 445 248 | 2,744× | 2,916 | 10,2 | 138,2 | ja |
| bzip2 -9 | 29 008 758 | 3,447× | 2,321 | 8,7 | 16,5 | ja |
| zstd -19 | 26 944 227 | 3,711× | 2,155 | 1,1 | 340,6 | ja |
| brotli -q 11 | 25 742 001 | 3,885× | 2,059 | 0,3 | 207,3 | ja |
| zstd -22 --ultra | 25 272 471 | 3,957× | 2,022 | 0,8 | 381,5 | ja |
| xz -9e | 24 831 648 | 4,027× | 1,986 | 0,8 | 69,6 | ja |
| **nvcm** | 21 417 184 | 4,669× | 1,713 | 0,8 | 0,7 | ja |

<!-- SLUT STANDARD -->

Per fil på Silesia, där korpusens heterogenitet är själva poängen:

<!-- SILESIA-PER-FIL -->

| fil | innehåll | rå byte | zstd -19 | brotli -q 11 | xz -9e | nvcm | nvcm mot bäst |
|:--|:--|--:|--:|--:|--:|--:|--:|
| `dickens` | engelsk litteratur, text | 10 193 352 | 2 849 945 | 2 827 777 | 2 831 212 | 2 234 894 | −20,2 % |
| `mozilla` | körbara filer (tar) | 51 215 653 | 15 071 672 | 13 872 265 | 13 376 240 | 14 366 242 | +7,4 % |
| `mr` | medicinsk 3D-bild | 9 970 424 | 3 106 737 | 2 823 136 | 2 751 892 | 2 205 358 | −9,7 % |
| `nci` | kemiska strukturer, text | 33 553 317 | 1 664 988 | 1 519 768 | 1 449 272 | 1 505 038 | +3,8 % |
| `ooffice` | körbar DLL | 6 151 755 | 2 595 377 | 2 478 855 | 2 427 224 | 2 379 789 | −2,0 % |
| `osdb` | databasdump, binär | 10 084 646 | 3 100 177 | 2 816 278 | 2 844 556 | 2 395 972 | −14,5 % |
| `reymont` | polsk litteratur, PDF-källa | 6 627 493 | 1 348 462 | 1 332 158 | 1 315 592 | 1 053 488 | −15,5 % |
| `samba` | källkod (tar) | 21 606 321 | 3 894 675 | 3 766 340 | 3 739 524 | 3 377 043 | −9,7 % |
| `sao` | stjärnkatalog, binär | 7 250 892 | 5 000 576 | 4 586 092 | 4 425 664 | 4 509 261 | +1,9 % |
| `webster` | ordbok, text | 41 458 740 | 8 679 705 | 8 428 575 | 8 368 672 | 6 317 041 | −24,5 % |
| `x-ray` | röntgenbild, 16-bitars | 8 475 800 | 5 129 827 | 4 682 753 | 4 491 264 | 3 742 075 | −7,6 % |
| `xml` | XML | 5 345 363 | 453 209 | 430 566 | 434 892 | 353 039 | −18,0 % |

<!-- SLUT SILESIA-PER-FIL -->

Som referenspunkt mot publicerade resultat (LTCB, andra maskiner och
programversioner; ej egna mätningar):

| program | enwik8 (byte) | bit/byte | metod |
|:--|--:|--:|:--|
| gzip 1.3.5 | 36 445 248 | 2,92 | ordbok |
| bzip2 1.0.2 | 29 008 736 | 2,32 | blocksortering |
| zstd 0.6.0 | 25 405 601 | 2,03 | ordbok |
| xz 5.2.1 | 24 703 772 | 1,98 | ordbok (LZMA) |
| nanozip 0.09a | 18 594 163 | 1,49 | kontextblandning |
| zpaq 6.42 | 17 855 729 | 1,43 | kontextblandning |
| paq8px_v206fix1 | 15 849 084 | 1,27 | kontextblandning |
| cmix v21 | 14 623 723 | 1,17 | kontextblandning |

<!-- ENWIK8-PLATS -->

Vår egen mätning: **nvcm gör enwik8 på 21 417 184 byte, 1,713 bits/byte** — 13,8 % mindre än `xz -9e` på samma maskin (24 831 648 byte, 1,986 bpb).

Med 1,71 bits/byte hamnar nvcm mellan ordbokspackarna och de tunga kontextblandarna i tabellen ovan: den tar hem det en ordbokspackare inte kan se, men den har åtta modeller där cmix har hundratals, och den kör i ett par MiB/s där cmix kör i kilobyte per sekund.

<!-- SLUT ENWIK8-PLATS -->

### 3.5 Genomströmning

Sekunder är inte jämförbara mellan korpusar av olika storlek; tabellen anger
därför MiB/s rådata genom väggklockan.

<!-- TIDER -->

| korpus | nvz normal pack | nvz normal upp | zstd -3 pack | zstd -19 pack | xz -9e pack |
|:--|--:|--:|--:|--:|--:|
| Programkod | 1 | 86 | 325 | 2 | 1 |
| Bilder (PNG) | 2 | 45 | 743 | 11 | 3 |
| Foton (JPEG) | 8 | 24 | 869 | 8 | 3 |
| Bilder (GIF) | 3 | 274 | 304 | 6 | 3 |
| Film (h264 och VP9) | 9 | 333 | 1 220 | 9 | 3 |
| Arkiv (zip och tar.gz) | 2 | 363 | 1 059 | 8 | 3 |
| Loggfiler | 0 | 128 | 1 254 | 2 | 0 |
| Silesia (arkivnivå) | 2 | 83 | 612 | 3 | 1 |
| enwik8 (arkivnivå) | 1 | 50 | 502 | 2 | 1 |

*MiB per sekund, rådata genom väggklockan.*

<!-- SLUT TIDER -->

## 4. Analys

### 4.1 Loggdata: kompressorvalet dominerar

Loggkorpusen uppvisar provningens högsta kvot, 23,8×, och källan kan
isoleras genom lägesjämförelsen: `normal` och `liten` skiljer sig enbart i
att nvcm deltar i kodekprovet.

| läge | storlek | packtid | aktiv skillnad |
|:--|--:|--:|:--|
| `normal` | 11,97 MiB | 864 s | kodekprov zstd/xz |
| `liten` | **10,03 MiB** | **197 s** | därtill nvcm |

nvcm vann provet för samtliga textblock, vilket arkivregistret bekräftar
(2 block, båda nvcm; 192,03 → 9,00 MiB respektive 46,29 → 1,03 MiB).
Att det mindre arkivet också packas snabbare beror på parallellitet:
nvz:s block komprimeras oberoende av varandra, medan `xz -9e -T0` på denna
datamängd i praktiken exekverar enkeltrådat till följd av sin
minnesbudget per tråd. Blockens olika kvoter (21,4× respektive 45,1×)
illustrerar adaptionens förlopp: modellen som sett 192 MiB logg
predikterar nästa block väsentligt bättre.

### 4.2 Redan komprimerade arkiv: stickprovets gräns

På arkivkorpusen är nvz 20,0 % större än `zstd −22 --ultra --long=31`
(291,3 mot 242,8 MiB). Två förklaringshypoteser prövades experimentellt
(`experiment/arkiv-fonster.sh`): (a) blockpartitioneringen hindrar
matchning mellan block; (b) kodekprovet väljer fel kodek.

| konfiguration | storlek |
|:--|--:|
| zstd −19, standardfönster (8 MiB) | 365,3 MiB |
| zstd −19 `--long=27`, hel ström | 261,4 MiB |
| zstd −22 `--ultra --long=31`, hel ström | 242,8 MiB |
| nvz, kodekprovet väljer | 291,3 MiB |
| nvz, `--kodek zstd19` | 260,2 MiB |
| nvz, `--kodek zstd22` | **244,6 MiB** |

Hypotes (a) falsifieras: med manuellt vald kodek kostar partitioneringen
1,2 MiB (0,7 %). Hypotes (b) bekräftas: provet komprimerar 4 MiB spridda
över blocket, och korpusens redundans ligger i nästan identiska filer
hundratals megabyte isär — en egenskap ett glest stickprov inte kan
observera. Provet klassificerar därför blocket som svårkomprimerat och
väljer en billig kodek. Deduplikationen fångar de exakta kopiorna
(62,8 MiB, 16,0 %); differensen utgörs av *nästan*-kopior (olika
gzip-strömmar av likartad rådata), som endast långdistansmatchning över
hela strömmen ser.

**Åtgärd och ommätning.** Diagnosen implementerades som två regler:
(i) opaka block delas inte vid 192 MiB — på opak data är blockdelning en
kompressionsgräns, inte en minnesoptimering, och zstd parallelliserar
inom blocket; (ii) för opaka block som stickprovet klassar som
komprimerbara ersätts stickprovsvalet av en tvåstegsregel, eftersom
stickprovet inte kan skilja video från arkiv-av-arkiv — båda är lokalt
täta, och skillnaden syns först över hela blocket. Blocket packas först
med zstd −19 (fönster 2²⁷); vinner den mer än 10 % finns redundans
mellan filer, och blocket packas om med zstd −22 (fönster 2³¹) som tar
hem resten. Video stannar vid någon procent och betalar aldrig för den
dyra vägen. Ommätt resultat:

| konfiguration | storlek |
|:--|--:|
| zstd −22 `--ultra --long=31`, hel ström | 242,8 MiB |
| nvz efter åtgärd (1 block, eskalerad till zstd22) | **242,5 MiB** |

Filmkorpusen prövar regelns andra gren: eskaleringen uteblir (zstd −19
vinner 0,9 %), och packtiden stannar vid 60 s mot de 255 s en ovillkorlig
zstd −22 hade kostat.

nvz går därmed från +20,0 % till −0,1 % mot bästa klassiska verktyg på
korpusen; differensen utgörs av deduplikationens 62,8 MiB, som
långdistansmatchningen i det klassiska fallet måste återupptäcka som
matchningar i stället för att aldrig lagra.

### 4.3 Video: nollresultatet och dess kostnad

Samtliga tolv konfigurationer landar inom 1 % av råstorleken — väntat,
eftersom transformkodad video saknar entropilager åtkomligt för generisk
kompression. Den relevanta jämförelsen är därför kostnaden för att
konstatera detta: nvz `normal` når samma storlek som `zstd −22` på 16 %
av tiden (61 mot 266 s), eftersom kodekprovet klassificerar blocken som
rålagringsfall efter stickprov i stället för att komprimera dem fullt ut.

### 4.4 GIF: kvot utan kompressionsbidrag

GIF-korpusens kvot 1,93× förklaras i sin helhet av dubbletter (47,3 % av
korpusen) och av att korpusen (27,4 MiB) ryms inom samtliga moderna
packares fönster: nvz, zstd −19/−22 och xz når samma storlek, och
`nvz utan knep` är identisk med `nvz normal`. nvz:s omvandlingar bidrar
med noll på LZW-kodat innehåll — formatet öppnas inte. En
GIF-motsvarighet till nvjpg (LZW → aritmetisk kodning av bildrutorna) är
därmed en identifierad men omätt möjlighet; korpusen för att mäta den
finns nu.

## 5. Korrekthet

### 5.1 Återskapning

Rekursiv diff besvarar en svagare fråga än H2 kräver: den jämför innehåll i
filer som existerar på båda sidor, men är blind för rättigheter,
tidsstämplar, tomma kataloger och poster som tyst fallit bort.
`prov/aterskapning.py` beräknar därför ett fullständigt avtryck — SHA-256,
läge, mtime i nanosekunder, länkmål, posttyp — före packning och efter
uppackning, kräver identitet, samt verifierar att arkivregistrets
postantal överensstämmer med trädets.

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

Därtill prövar `prov/prova-allt.sh` kantfall — tomma filer, dubbletter,
relativa, brutna och katalogpekande symlänkar, filnamn med radbrytning och
åäö, rättigheter 400–755, tidsstämplar 1985 och 2031 — i samtliga lägen,
med krav på full metadataidentitet. Sviten omfattar 44 prov och passerar
utan anmärkning.

### 5.2 Skadetålighet

En packare som vid skada returnerar felaktiga data utan felindikation är
värre än en som vägrar: den förra upptäcks först vid användning. Fem
skadeklasser injicerades i ett friskt arkiv; kravet är avvisning eller
bevisat oskadd utdata, aldrig tyst korruption.

| skadeklass | utfall |
|:--|:--|
| utbytt magi | avvisat |
| bitfel i blockkropp | avvisat |
| trunkering (sista 1 024 byte) | avvisat |
| nollställd registerpekare | avvisat |
| bitfel i registret | avvisat |

### 5.3 Versionskompatibilitet

C++-implementationen (avsnitt 6) läser Python-implementationens arkiv
(JSON-register, 8 bytes hashprefix) bitexakt, inklusive PNG- och
JPEG-omvandlade objekt; detta ingår i provsviten. Det omvända stöds inte:
v2-registret är binärt, och Python-implementationen är utgången.

## 6. Implementation

nvz skrevs under provningens gång om från Python till en fristående
C++17-binär (287 kB): nvcm och nvjpg länkas in och exekveras som
arbetarprocesser via `/proc/self/exe`, zstd/xz/zlib anropas som bibliotek i
processen, och `liten` är standardläge. Kompressionsresultaten är
ekvivalenta inom stickprovsbruset (kodekarna är samma bibliotek);
skillnaden är tidsprofilen där Python deltog per fil:

| mätpunkt | Python | C++ |
|:--|--:|--:|
| kod, läge `snabb` (5 974 filer) | 2,7 s | 0,8 s |
| gif, läge `snabb` | 0,6 s | 0,1 s |
| 516 JPEG-omvandlingar | ≈8 s | 2,8 s |
| foto, uppackning (1 143 omvandlade) | 7,8 s | 17,4 s |

Den kvarstående regressionen på foto-uppackning beror på
arbetarprocessernas uppstartskostnad per bild och är den enda punkt där
Python-implementationens processpool var snabbare.

Porteringen exponerade tre fel av det slag som endast uppträder under
samtidighet respektive formatmigration, dokumenterade här då de är
generella:

1. **Allokering mellan `fork` och `exec` i flertrådad process.** Barnet
   ärver låstillstånd från trådar som inte följer med; en allokering i
   barnet kan då blockera permanent. Symptom: arbetare med 0 % CPU i
   84 minuter. Åtgärd: argumentvektorn byggs före `fork`; mellan `fork`
   och `exec` förekommer endast async-signal-säkra anrop.
2. **Filbeskrivararv mellan parallella arbetare.** Ett barn som skapas
   medan en annan tråds rör är öppna ärver även dem; motpartens läsände
   får då aldrig EOF. Åtgärd: `pipe2(O_CLOEXEC)`.
3. **Hashlängd i v1-registret.** Python-registret lagrar de första 16
   hextecknen av objekthashen — 8 byte. En läsare som jämför 16 byte
   underkänner friska arkiv.

## 7. Begränsningar och vidare arbete

Ordnade efter uppskattad avkastning:

1. **Stickprovsurvalet för icke-opaka klasser:** åtgärden i 4.2 omfattar
   endast opaka block, där regel kan ersätta prov. För text- och
   binärklasser kvarstår frågan om ett stickprov som växer med
   blockstorleken förbättrar kodekvalet; omätt.
2. **GIF-omkodning** (avsnitt 4.4): samma konstruktion som nvjpg,
   tillämpad på LZW-lagret.
3. **nvcm på opaka block:** läget `liten` sänder endast textklasser till
   nvcm; huruvida opak data lönar sig är omätt.

Utanför provningen: progressiv JPEG (nvjpg omfattar endast baseline
sekventiell), gits packfiler (samma receptansats som PNG, oprövad), arkiv
större än arbetsminnet, samt GPU-läget — mätt separat med slutsatsen att
LZ77-matchningen accelereras 2,5× men att seriell entropikodning begränsar
helheten (Amdahl; se README).

## Appendix A: Reproduktion

Källkod och samtliga mätdata är publicerade på
<https://github.com/hugowikstrom/nvz>. Beroenden: g++ (C++17), libzstd-dev,
liblzma-dev, zlib1g-dev; för jämförelsemätningarna därtill gzip, pigz,
bzip2, zstd och xz.

```bash
git clone https://github.com/hugowikstrom/nvz && cd nvz
make                                                  # bygger nvz, nvcm, nvjpg
python3 korpus/bygg-korpus.py gif film arkiv logg     # lokala korpusar
export NVPACK_SKRAP=$PWD/.skrap
python3 matning/matning.py <korpus>                   # arkivnivå, per korpus
python3 matning/standardkorpus.py silesia enwik8      # kodeknivå, en tråd
python3 prov/aterskapning.py gif arkiv logg film png foto kod
bash prov/prova-allt.sh                               # kantfall, 44 prov
bash experiment/arkiv-fonster.sh                      # hypotesprövningen i 4.2
python3 experiment/dubbletter.py
python3 rapport/bred-provning.py                      # fyller tabellerna här
python3 matning/rapport.py --in-i-readme
```

Standardkorpusarna hämtas en gång:

```bash
curl -sSLo /tmp/silesia.zip https://sun.aei.polsl.pl/~sdeor/corpus/silesia.zip
curl -sSLo /tmp/enwik8.zip  https://mattmahoney.net/dc/enwik8.zip
mkdir -p korpus/silesia korpus/enwik8
unzip -oq /tmp/silesia.zip -d korpus/silesia
unzip -oq /tmp/enwik8.zip  -d korpus/enwik8
```
