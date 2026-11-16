# nvz

En arkivpackare som vet vad den packar.

`tar | zstd` är två program som inte vet något om varandra. tar lägger filerna
i den ordning katalogträdet råkar ha dem och skickar vidare en enda byteström;
zstd ser den strömmen genom ett fönster och får aldrig veta att fil 4000 är en
exakt kopia av fil 12, att de 9 MB "slump" den just gav upp på i själva verket
är en deflate-ström som går att veckla ut, eller att de 400 MB foton den
maler igenom har ett entropilager från 1992 som går att byta ut.

nvz vet det, och mäter sig mot dagens verktyg på riktiga träd:

* **Programkod** — 37 % mindre än dagens arkiv, 13 % mindre än `tar.xz -9e`,
  som är det bästa de installerade verktygen klarar
* **PNG-bilder** — 42 % mindre än dagens arkiv, 15 % mindre än bästa klassiska
  (`tar.zst -22 --ultra`) — men med 1,6 gånger dess packtid
* **Foton (JPEG)** — 15 % mindre än dagens arkiv, där *varje* klassiskt verktyg
  — gzip, bzip2, zstd −22, xz −9e — stannar inom en procent från där det
  började

Allt är **förlustfritt och bit-exakt**: varje omvandling provas i båda
riktningarna vid packning, och det som inte går att återskapa lagras rått.
De exakta talen står i [Resultat](#resultat) och kommer ur `matning/`.

## Kommandoraden är gzips

Ytan är gzips, för att gzips yta är den man redan har i fingrarna. Samma
flaggor, samma standardbeteende: filen som packas ersätts av `fil.nv`.

```
nvz fil                 # -> fil.nv, originalet borta
nvz -d fil.nv           # -> fil, arkivet borta
nvz -c fil > ut.nv      # till standard ut, filen lämnas i fred
nvz -k -9 fil           # behåll filen, packa så litet det går
nvz -l ark.nv           # packat, uppackat, kvot
nvz -t ark.nv           # packa upp och jämför mot registret
cat fil | nvz | ...     # utan filnamn: standard in -> standard ut
```

`-c -d -f -h -k -l -L -n -N -q -r -S -t -v -V` och `-1`…`-9` betyder vad de
betyder i gzip, långa former (`--decompress`, `--keep`, `--suffix=.suf` …)
och förkortningar av dem också. `-1`–`-3` är läge **snabb**, `-4`–`-6`
**normal**, `-7`–`-9` **liten**. Slutstatus är gzips: 0 allt väl, 1 fel,
2 varning.

Egna flaggor utöver gzips: `-j/--jobb=N`, `--läge=`, `--kodek=`,
`--utan-dedup`, `--utan-omvandling`.

**En enda avvikelse, och den är med flit.** gzip säger `is a directory --
ignored` om en katalog. En katalog är precis vad nvz är byggd för, så en
katalog som operand packas till **ett** arkiv av hela trädet — med dedup och
sortering mellan filerna, alltså större delen av vinsten. Två följder:

* Källträdet lämnas **alltid** kvar. nvz raderar aldrig en katalog, hur
  mycket gzip-logiken än säger att originalet ska ersättas.
* `-r` gör i stället gzips bokstavliga sak: varje fil för sig, i katalogen.
  Då finns ingen gemensam ström, och vinsten mellan filerna försvinner —
  `-r` är till för när man vill kunna packa upp en fil i taget med fingrarna,
  inte för att spara plats.

```bash
make                                   # bygger C-delarna
./src/nvz -v ~/lostconnection       # -> ~/lostconnection.nv, trädet kvar
./src/nvz -d lostconnection.nv      # trädet tillbaka, arkivet borta
bash prov/prova-allt.sh                # hela provsviten
```

Underkommandona finns kvar och fungerar oförändrat. Mätningarna i `matning/`
och proven i `prov/` anropar dem, och ett byte av yta får inte göra gamla
siffror omätbara:

```
nvz [FLAGGA]... [FIL]...          # gzip-syntax: -c -d -f -k -l -r -t -v -1..-9
nvz packa     <katalog> <arkiv.nv> [--läge snabb|normal|liten]
nvz packa-upp <arkiv.nv> <målkatalog>
nvz lista     <arkiv.nv>
nvz prova     <katalog>           # packa, packa upp, jämför bit för bit
```

Utan flaggor packas varje FIL och originalet ersätts, precis som `gzip` —
men standardläget är `liten`, alltså det minsta nvz kan. Vill man ha det
snabbt säger man `-1`.

```bash
make                      # bygger C-delarna (nvcm, nvjpg, och gpu om den går)
make installera           # nvz, nvcm, nvjpg + man-sidor under /usr/local
make enfil                # dist/nvz: hela arkivpackaren som EN körbar fil
man nvz                   # eller: man -l man/nvz.1 direkt ur trädet
bash prov/prova-allt.sh   # hela provsviten
```

---

## De fem sakerna tar inte gör

### 1. Identiska filer lagras en gång

Innehållet hashas med blake2b; dubbletter blir pekare in i samma block. tar
lagrar varje kopia och hoppas att komprimeraren hittar dem, vilket den bara
gör om de råkar ligga inom fönstret. I PNG-korpusen är 28,7 % av byten
dubbletter — i kodkorpusen 0,7 %, och då gör dedupen ingenting. Båda talen
står i tabellerna: raden **nvz utan knep** är samma packare med dedup och
omvandlingar avstängda, så vinsten går att se separat.

### 2. Lika filer läggs bredvid varandra

Posterna sorteras på klass och ändelse före packning, så att all JavaScript
ligger i en följd och alla skärmdumpar i en annan. Komprimeringen får syfta på
likheter som i katalogordning låg hundratals megabyte isär.

### 3. PNG vecklas ut och packas om

En PNG är redan deflate-packad, och deflate från 1996 är ett dåligt skal att
ha runt sin bilddata: 32 kB fönster, en fil i taget. nvz vecklar ut
IDAT-strömmen till råa filterrader och packar dem tillsammans med alla andra
bilders rådata.

Vägen tillbaka är det svåra. Deflate är inte en kanonisk kodning — samma
indata ger olika utdata beroende på nivå, minnesnivå och strategi — så nvz
letar upp **receptet**: den uppsättning zlib-parametrar som råkar ge precis
den ström filen redan har. Sökningen är billig därför att zlib-huvudet röjer
fönsterstorlek och nivågrupp, därför att fel kandidat avviker inom de första
hundra kilobytena och då avbryts, och därför att receptet som senast fungerade
provas först (bilder ur samma verktyg delar alltid recept).

Hittas inget recept lagras filen rå. Det händer oftare än man tror:
Chrome-skärmdumpar och gnome-screenshot använder deflate-motorer som systemets
zlib inte kan härma, och i PNG-korpusen gick bara 281 av 4 422 provade filer
att veckla ut. Vinsten på dem som gick är desto större.

**Mätningen som ändrade designen.** Först satt här ett tak: en bild som blåstes
upp mer än sexton gånger sin IDAT-ström ansågs inte värd besväret.
`experiment/png-troskel.py` mätte saken och motbevisade det:

| uppblåsning | filer | original | zstd19 på original | via utveckling | vinst |
|:--|--:|--:|--:|--:|--:|
| 2–4× | 13 | 17,5 MB | 17,48 MB | 13,92 MB | 20,4 % |
| 4–8× | 14 | 32,1 MB | 26,51 MB | 18,45 MB | 30,4 % |
| 8–16× | 40 | 61,5 MB | 49,93 MB | 40,95 MB | 18,0 % |
| 16–32× | 26 | 17,7 MB | 12,24 MB | 9,88 MB | 19,3 % |
| **32×+** | 188 | 13,4 MB | 7,31 MB | **4,35 MB** | **40,4 %** |

Utvecklingen vann i varje klass, och allra mest för de bilder gissningen ville
hoppa över. Det är logiskt i efterhand: en bild som deflate klämt 50:1 är ett
skärmklipp med stora enfärgade ytor, och när femtio sådana ligger utvecklade i
samma block hittar en modern packare likheterna **mellan** bilderna. Taket
finns kvar i koden, men på 150× och bara som skyddsräcke mot att en enda
skärmdump ska bli flera gigabyte arbetsdata.

### 4. JPEG kodas om utan att en pixel rörs

En JPEG är DCT-koefficienter entropikodade med Huffman. Huffman kan inte koda
en symbol på mindre än en bit och vet ingenting om att blocket bredvid såg
nästan likadant ut. `src/nvjpg.c` avkodar koefficienterna och kodar om dem med
en adaptiv aritmetisk kodare vars modeller frågar tre grannskap:

* blocket till vänster och ovanför — *hur rörig är den här delen av bilden?*
* samma plats i grannblocken — *hur stor brukar just den frekvensen vara här?*
* platserna till vänster och ovanför i samma block — *var ligger energin?*

Resultatet på hela fotokorpusen: **1 143 av 1 143 filer bit-exakta, −15,0 %**.
Noll råfall, noll avvikelser (`resultat/jpg-kontroll.log`).

Modellen trimmades fram, inte gissad. Varje ändring mättes mot samma urval:

| ändring | resultat |
|:--|--:|
| första fungerande modell | −12,0 % |
| grannskapskontexter i stället för bara sicksackplats | −13,1 % |
| finare hinkar (sicksack 16, storlek 10) | −13,9 % |
| takt som sjunker med antalet observationer | −14,6 % |
| blockets täthet med i storleksmodellen | **−14,8 %** |

Två idéer som lät bra mättes också — och ströks: egen kontextdimension för
grannblockets riktning (−13,3 %, sämre: kontexterna tunnades ut) och två fasta
adaptionstakter som vägs ihop (−11,6 %, mycket sämre: den långsamma takten
drog medelvärdet mot 50 % hela bilden igenom).

### 5. Ingenting antas — allt provas

Varje block provpackas på åtta ställen spridda genom blocket, med varje
tillgänglig kodek, och den som vinner får blocket. Ger provet mindre än 2 %
lagras blocket rått: film och ljud kostar då ingen packtid alls, och tid är
hela skälet till att dagens arkiv körs med `zstd -3` i stället för `-19`.

Att det blev ett prov och inte en tabell är en rättelse. Första versionen
hade "redan packat" som en EGENSKAP hos filtypen: JPEG, film och PNG som inte
kunde vecklas ut lagrades rått per definition. På PNG-korpusen kostade det
175 MiB (431 → 256 MiB på samma korpus utan omvandlingar). De fyra tusen
skärmdumpar som inte gick att veckla ut är nämligen
nästan identiska med VARANDRA — det är bara deras deflate-strömmar som är
ogenomträngliga, var för sig. `tar.zst -19` hittade de likheterna; nvz
antog att det inte fanns några att hitta.

Provet kostar ett par sekunder per block och svarar "lagra rått" av sig självt
när antagandet faktiskt stämmer. Enda undantaget är nvjpgs egen utström: den
är aritmetiskt kodad, och den *är* brus.

Provet gör också det val man inte vill göra en gång för alla. På PNG-korpusen
valde det xz för den utvecklade bilddatan, och den sortens val går att
kontrollräkna: tvingad till `zstd -19` blir arkivet 217,10 MiB på 219 s, med
xz 208,88 MiB på 254 s. 3,8 % mindre för 16 % längre — här förtjänade xz sin
plats, och på kodkorpusens sorterade textblock förtjänade den den igen.

---

## nvcm: en egen kompressor för text och källkod

`xz` var det starkaste som fanns installerat, så nvz fick något starkare.
`src/nvcm.c` är en kontextblandande kompressor: i stället för att hänvisa
bakåt i en ordbok gissar den **nästa bit**, med åtta modeller samtidigt
(ordning 0–8, en ordmodell, och en matchmodell som gör ordbokspackarens trick
som en röst bland andra). Rösterna vägs ihop av en blandare som lär sig vilken
som brukar ha rätt, och putsas av två efterjusterare.

På kodkorpusen: **7,19 MiB mot xz:s 8,23 MiB — 13 % mindre.**

Den är inte bättre på allt, och det är hela poängen med att prova i stället
för att rangordna:

| indata | xz -9e | nvcm | |
|:--|--:|--:|:--|
| källkod, 8 MB | 782 876 | **663 744** | −15 % |
| `seq 1 50000` | **10 160** | 47 548 | +368 % |

En modell som gissar nästa bit ur de föregående byten ser inte att ett tal
ökar med ett; ordbokspackarens matchningslängd gör det. Därför provar läget
`liten` nvcm mot xz och zstd på ett stickprov ur varje block och tar den som
faktiskt vinner. Första versionen gjorde inte det, och gjorde ett träd med en
talkolumn i 8 % större.

---

## Lägena

| läge | vad som händer | när |
|:--|:--|:--|
| `snabb` | zstd −3, inga omvandlingar | motsvarar dagens arkivskript (`-1`) |
| `normal` | omvandlingar på, zstd −19 eller xz efter prov | `-4` .. `-6` |
| `liten` | dessutom nvcm på text där den vinner | **standard** |
| `gpu` | dessutom nvlz (lz77 på grafikkortet) i provet | aldrig — se mätningen nedan |

---

## Resultat

Mätt på en Ryzen AI 9 HX 470 (24 trådar, 2,0 GHz) med NVMe-disk. Varje rad är
en hel rundgång: packa, packa upp, och `diff -rq` mot originalträdet — kolumnen
**exakt** är inte en förhoppning utan utfallet av den jämförelsen. Korpusarna
byggs av `korpus/bygg-korpus.py` ur riktiga projekt på maskinen, som hårdlänkar
(noll extra diskutrymme, samma bytes).

<!-- TABELLER -->

### Programkod — 55.3 MiB i 5 962 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 60.83 | 0.91× | +430.8 % | 0 s | 0 s | ja |
| tar.gz -6 | 12.32 | 4.49× | +7.5 % | 2 s | 0 s | ja |
| tar.gz -9 (pigz) | 12.18 | 4.54× | +6.3 % | 0 s | 0 s | ja |
| tar.bz2 -9 | 9.51 | 5.81× | -17.0 % | 7 s | 2 s | ja |
| tar.zst -3 (dagens) | 11.46 | 4.82× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 8.53 | 6.48× | -25.6 % | 35 s | 0 s | ja |
| tar.zst -22 --ultra | 8.45 | 6.54× | -26.2 % | 56 s | 0 s | ja |
| tar.xz -9e | 8.23 | 6.72× | -28.2 % | 47 s | 1 s | ja |
| **nvz snabb** | 11.32 | 4.88× | -1.2 % | 3 s | 1 s | ja |
| **nvz utan knep** | 8.19 | 6.75× | -28.5 % | 43 s | 1 s | ja |
| **nvz normal** | 8.19 | 6.75× | -28.6 % | 43 s | 1 s | ja |
| **nvz liten** | 7.19 | 7.69× | -37.2 % | 71 s | 68 s | ja |

### Bilder (PNG) — 431.0 MiB i 4 617 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 438.41 | 0.98× | +20.8 % | 3 s | 0 s | ja |
| tar.gz -6 | 401.67 | 1.07× | +10.7 % | 20 s | 3 s | ja |
| tar.gz -9 (pigz) | 401.89 | 1.07× | +10.8 % | 2 s | 3 s | ja |
| tar.bz2 -9 | 385.38 | 1.12× | +6.2 % | 74 s | 44 s | ja |
| tar.zst -3 (dagens) | 362.85 | 1.19× | +0.0 % | 1 s | 0 s | ja |
| tar.zst -19 --long | 252.73 | 1.71× | -30.3 % | 40 s | 1 s | ja |
| tar.zst -22 --ultra | 245.22 | 1.76× | -32.4 % | 156 s | 1 s | ja |
| tar.xz -9e | 266.41 | 1.62× | -26.6 % | 128 s | 5 s | ja |
| **nvz snabb** | 279.28 | 1.54× | -23.0 % | 3 s | 1 s | ja |
| **nvz utan knep** | 256.42 | 1.68× | -29.3 % | 45 s | 2 s | ja |
| **nvz normal** | 208.88 | 2.06× | -42.4 % | 254 s | 9 s | ja |
| **nvz liten** | 208.88 | 2.06× | -42.4 % | 251 s | 9 s | ja |

### Foton (JPEG) — 408.5 MiB i 1 143 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 409.37 | 1.00× | +0.8 % | 2 s | 0 s | ja |
| tar.gz -6 | 405.81 | 1.01× | -0.1 % | 19 s | 2 s | ja |
| tar.gz -9 (pigz) | 406.04 | 1.01× | -0.1 % | 1 s | 2 s | ja |
| tar.bz2 -9 | 404.94 | 1.01× | -0.3 % | 57 s | 41 s | ja |
| tar.zst -3 (dagens) | 406.30 | 1.01× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 405.09 | 1.01× | -0.3 % | 53 s | 0 s | ja |
| tar.zst -22 --ultra | 405.09 | 1.01× | -0.3 % | 225 s | 0 s | ja |
| tar.xz -9e | 405.43 | 1.01× | -0.2 % | 157 s | 4 s | ja |
| **nvz snabb** | 408.50 | 1.00× | +0.5 % | 1 s | 0 s | ja |
| **nvz utan knep** | 406.35 | 1.01× | +0.0 % | 13 s | 1 s | ja |
| **nvz normal** | 345.07 | 1.18× | -15.1 % | 22 s | 8 s | ja |
| **nvz liten** | 345.07 | 1.18× | -15.1 % | 28 s | 8 s | ja |

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

### Film (h264/VP9) — 536.7 MiB i 67 filer

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

### Arkiv (zip/tar.gz) — 391.8 MiB i 133 filer

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

### Blandat projektträd (symbios-3d) — 1 814.4 MiB i 15 139 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 1 804.14 | 1.01× | +298.4 % | 7 s | 2 s | ja |
| tar.gz -6 | 494.04 | 3.67× | +9.1 % | 99 s | 10 s | ja |
| tar.gz -9 (pigz) | 489.62 | 3.71× | +8.1 % | 19 s | 9 s | ja |
| tar.bz2 -9 | 442.88 | 4.10× | -2.2 % | 208 s | 82 s | ja |
| tar.zst -3 (dagens) | 452.83 | 4.01× | +0.0 % | 2 s | 3 s | ja |
| tar.zst -19 --long | 319.30 | 5.68× | -29.5 % | 149 s | 4 s | ja |
| tar.zst -22 --ultra | 301.21 | 6.02× | -33.5 % | 623 s | 4 s | ja |
| tar.xz -9e | 297.54 | 6.10× | -34.3 % | 289 s | 5 s | ja |
| **nvz snabb** | 407.37 | 4.45× | -10.0 % | 14 s | 4 s | ja |
| **nvz normal** | 282.25 | 6.43× | -37.7 % | 315 s | 8 s | ja |
| **nvz liten** | 279.10 | 6.50× | -38.4 % | 325 s | 207 s | ja |

### Silesia (standardkorpus) — 202.1 MiB i 12 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 202.14 | 1.00× | +220.1 % | 0 s | 0 s | ja |
| tar.gz -6 | 65.06 | 3.11× | +3.0 % | 12 s | 1 s | ja |
| tar.gz -9 (pigz) | 64.52 | 3.13× | +2.2 % | 2 s | 1 s | ja |
| tar.bz2 -9 | 52.04 | 3.88× | -17.6 % | 24 s | 10 s | ja |
| tar.zst -3 (dagens) | 63.15 | 3.20× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 50.17 | 4.03× | -20.6 % | 63 s | 0 s | ja |
| tar.zst -22 --ultra | 49.82 | 4.06× | -21.1 % | 179 s | 0 s | ja |
| tar.xz -9e | 46.17 | 4.38× | -26.9 % | 158 s | 3 s | ja |
| **nvz snabb** | 63.16 | 3.20× | +0.0 % | 1 s | 0 s | ja |
| **nvz utan knep** | 46.16 | 4.38× | -26.9 % | 94 s | 2 s | ja |
| **nvz normal** | 46.16 | 4.38× | -26.9 % | 94 s | 2 s | ja |
| **nvz liten** | 43.54 | 4.64× | -31.0 % | 102 s | 100 s | ja |

### enwik8 (standardkorpus) — 95.4 MiB i 1 filer

| packare | storlek MiB | kvot | mot dagens | packtid | uppacktid | exakt |
|:--|--:|--:|--:|--:|--:|:-:|
| tar (okomprimerad) | 95.37 | 1.00× | +182.1 % | 0 s | 0 s | ja |
| tar.gz -6 | 34.83 | 2.74× | +3.0 % | 7 s | 1 s | ja |
| tar.gz -9 (pigz) | 34.80 | 2.74× | +2.9 % | 1 s | 1 s | ja |
| tar.bz2 -9 | 27.66 | 3.45× | -18.2 % | 11 s | 6 s | ja |
| tar.zst -3 (dagens) | 33.80 | 2.82× | +0.0 % | 0 s | 0 s | ja |
| tar.zst -19 --long | 25.18 | 3.79× | -25.5 % | 61 s | 0 s | ja |
| tar.zst -22 --ultra | 24.11 | 3.96× | -28.7 % | 118 s | 0 s | ja |
| tar.xz -9e | 23.68 | 4.03× | -29.9 % | 121 s | 1 s | ja |
| **nvz snabb** | 33.80 | 2.82× | +0.0 % | 1 s | 0 s | ja |
| **nvz utan knep** | 23.68 | 4.03× | -29.9 % | 124 s | 2 s | ja |
| **nvz normal** | 23.68 | 4.03× | -29.9 % | 126 s | 2 s | ja |
| **nvz liten** | 20.43 | 4.67× | -39.6 % | 133 s | 135 s | ja |

<!-- SLUT TABELLER -->

Priset står i samma tabell och ska inte läsas bort. På PNG-korpusen tar
nvz 254 s mot `tar.zst -19`:s 40 s — för att tre gigabyte utvecklad
bilddata ska packas om. Uppackningen är däremot nästan gratis (9 s), och ett
arkiv packas en gång och packas upp sällan. För det omvända fallet finns
läget `snabb`, som på samma korpus gör 279 MiB på 3 sekunder — mindre än
dagens arkiv OCH snabbare, bara på dedupen.

Kör om allt med `python3 matning/matning.py <korpus>` och bygg om tabellerna
med `python3 matning/rapport.py --in-i-readme`.

En **bred provning** från 2026-11-16 täcker åtta datasorter till — GIF, film,
arkiv av arkiv, loggfiler — plus standardkorpusarna Silesia och enwik8, och
ett återskapningsprov som kräver tillbaka varje byte, rättighet och
tidsstämpel: [`rapport/bred-provning.md`](rapport/bred-provning.md), även som
[PDF](rapport/bred-provning.pdf). Kortversionen: loggar packas 23,8×, nvcm
slår varje klassiskt verktyg på båda standardkorpusarna, film går inte att
packa (och det är rätt svar), och på arkiv av arkiv förlorar nvz på ett
kodekprov som ser för lite av blocket.

Den första mätningen finns också som **tryckfärdig rapport på nio sidor**:
`rapport/nvz-rapport.pdf`, byggd med `make pdf`. Den genereras ur samma
`resultat/*.json` och kontrollerar sig själv i två steg innan den godkänns —
att ingen sida svämmar över (sidorna är fasta A4-rutor med `overflow: hidden`,
så överfyllnad klipps annars tyst) och att inget typsnitt hamnat som Type 3.

### Och så verkligheten

Korpusarna ovan är rena: bara kod, bara PNG, bara foton. Ett riktigt
projektträd är blandat, och blandningen avgör vad man faktiskt tjänar.
`experiment/riktigt-arkiv.sh` kör samma jämförelse på en skarp katalog:

| träd | dagens `tar.zst -3` | nvz liten | |
|:--|--:|--:|--:|
| `~/lostconnection` (2,9 GB, 2 009 filer) | 2 869,25 MiB | 2 866,74 MiB | **−0,09 %** |
| samma utan `data/klipp` (25 MB, 432 filer) | 17,17 MiB | 16,52 MiB | **−3,8 %** |

Det är det ärliga svaret för just det arkivet: **ingenting hjälper, för det
ÄR 517 filmer.** 2,9 GB mp4 är redan packat av en videokodare, och ingen
arkivpackare i världen tar mer än en bråkdel av det. Vinsterna i tabellerna
ovan finns där det finns kod och bilder — och i lostconnection-arkivet är kod
och bilder 25 MB av 2 900.

Den mätningen var inte heller gratis att göra. Första gången stod nvz
0,06 % ÖVER dagens arkiv, alltså sämre, och felet satt i en tröskel: block
vars stickprov packade sig mindre än 2 % lagrades rått för att spara tid. En
halv procent av 2,9 GB film är fjorton megabyte, och `zstd -3` tar dem på
några sekunder. Tröskeln har nu tre utfall i stället för två — rått under
0,2 %, billig kodek däremellan, dyr kodek när det finns något att hämta.

---

## GPU-läget: byggt, mätt, och inte värt det

Frågan var om ett GPU-läge gör packningen snabbare. Svaret är **nej på den
här maskinen** — och det är mätt på fyra nivåer i stället för resonerat, för
ett nej som bara är ett resonemang går inte att pröva.

Det som byggdes är inte en skiss: `src/nvgpu.c` är en Vulkan-compute-värd med
tre kärnor (`src/*.comp`), en egen kodek, och ett läge i nvz där kodeken
får tävla om varje block. Bygg den med `make gpu` (kräver `libvulkan-dev` och
`glslc`); resten av nvz byggs och fungerar utan den.

```
nvgpu enhet     vad enheten klarar      nvgpu matcher FIL   lz77: gpu mot cpu
nvgpu band      bandbreddstaket         nvgpu prov          testvektorer + gpu==cpu
nvgpu hasha     blake2b-128 per fil     nvgpu packa|packaupp  kodeken nvlz
```

### 1. Taket: 1.11 gånger

Grafiken är integrerad. Den delar RAM med processorn, alltså delar den också
minnesbussen, och ett enda svep över data kan aldrig gå fortare än kvoten
mellan dem:

| vem läser 1 GiB | MiB/s |
|:--|--:|
| gpu ur delat minne | 34 881 |
| cpu ur vanligt minne (24 trådar) | 31 349 |
| **cpu ur det delade minnet** | **465** |

Den sista raden är den viktiga och den kostade mig en felaktig slutsats.
"Nollkopiering" på en APU betyder att gpu-bufferten ligger i samma fysiska
RAM — men den är *write-combined*, alltså ocachad från cpu-sidan, och en cpu
som läser ocachat minne går **67 gånger långsammare**. Första versionen läste
hem 64 MiB matchningar därifrån, och då
blev gpu-vägen långsammare än sitt eget cpu-fallback. Buffertar som cpu:n
läser tillbaka ligger nu i `HOST_CACHED`-minne, de gpu:n betar av i
`DEVICE_LOCAL`. Det ensamma bytet var värt 0,3 sekunder av 1,1.

### 2. Kärnorna: en vinner, en förlorar

Båda körs på gpu OCH på cpu med samma semantik, och svaren jämförs bit för
bit. En tid utan identiskt svar är ingen mätning.

**blake2b-128, hashen som dedupen bygger på** — gpu:n förlorar:

| korpus | filer | MiB | gpu | cpu (24 trådar) | identiska |
|:--|--:|--:|--:|--:|:-:|
| kod | 5 974 | 55.3 | 0.185 s | 0.095 s | ja |
| png | 4 618 | 431.1 | 0.505 s | 0.189 s | ja |

Kärnan själv tar 0.038 s av de 0.185 — resten är att samla in filerna i en
buffert. Hashning är minnesbunden, och taket ovan säger att
där finns ingenting att vinna.

**LZ77-matchningen** — gpu:n vinner, och det är den enda kärnan i en packare
som är brett parallell. Varje position ställer samma oberoende fråga ("hur
långt tillbaka finns den längsta upprepningen av det som börjar här?"), och
det är den frågan som kostar tiden i `xz -9e`, `zstd -19` och i zlibs nivå 9
(och därmed i PNG-receptsökningen). Mätt på ett riktigt 16 MiB-block ur kodkorpusen:

| kedjetak | cpu (24 trådar) | gpu | gpu-vinst | avvikelser |
|--:|--:|--:|--:|--:|
| 16 | 0.198 s | 0.073 s | 2.7x | 0 |
| 64 | 0.258 s | 0.111 s | 2.3x | 0 |
| 256 | 0.327 s | 0.153 s | 2.1x | 0 |
| 4096 | 0.533 s | 0.247 s | 2.2x | 0 |

Noll avvikelser över 16,8 miljoner positioner, i alla fyra körningarna.

### 3. Kodeken: nvlz

`xz`, `zstd` och `zlib` kan inte TA EMOT färdiga matchningar — deras utdata
måste bli bit för bit vad deras egna sökningar hade gett, annars är de inte
samma format. Därför fick gpu-matchningen en egen kodek: `nvlz`, där
sökningen ligger på gpu:n och tokenvalet och den aritmetiska kodningen på
cpu:n. **Uppackningen är ren cpu** — ett arkiv ska inte vara gisslan hos en
grafikdrivrutin.

Samma 16 MiB-block, alla rundturer kontrollerade:

| kodek | storlek | kvot | packtid | uppacktid |
|:--|--:|--:|--:|--:|
| **nvlz (gpu)** | 2 086 209 | x8.04 | 0.83 s | 0.33 s |
| **nvlz (cpu)** | 2 086 209 | x8.04 | 0.84 s | 0.33 s |
| zstd -3 | 2 203 155 | x7.62 | 0.05 s | 0.02 s |
| zstd -6 | 2 007 593 | x8.36 | 0.11 s | 0.03 s |
| zstd -12 | 1 911 072 | x8.78 | 0.49 s | 0.03 s |
| zstd -19 | 1 772 970 | x9.46 | 5.77 s | 0.04 s |
| xz -9e | 1 703 344 | x9.85 | 7.19 s | 0.18 s |

Där tar mätningen död på idén. `zstd -6` ger **bättre** kvot (x8.36 mot
x8.04) på **8 gånger kortare tid**, och packar upp tio gånger snabbare. Och lägg märke till de två första raderna: nvlz med gpu
och nvlz med cpu-matchare tar **lika lång tid**. Det är Amdahls lag i klartext
— gpu:n gör den parallella halvan 2,5 gånger snabbare, men den halvan är en
fjärdedel av kodekens tid, och entropikodningen som är resten är seriell av
konstruktion: nästa symbol beror på den förra.

### 4. Hela arkivet

`läge gpu` är `normal` plus att nvlz får vara med i varje blocks kodekprov.
Det provet är nvpacks egen mekanism för att inte anta något, och den
underkände kodeken av sig själv:

| läge | arkiv | packtid | valda kodekar |
|:--|--:|--:|:--|
| normal | 8 597 541 byte | 44.7 s | xz9e |
| gpu | 8 597 837 byte | 44.5 s | xz9e |

Inget block valde nvlz, och tiden är oförändrad. De 296 byte som skiljer är
inte gpu-lägets kostnad utan brus: två körningar av `--läge normal` i rad på
samma träd gav 8 597 884 och 8 597 603 byte. Arkivstorleken rör sig några
hundra byte mellan körningar — värt att veta när man jämför två tal som ligger
nära varandra, i den här tabellen eller någon annan.

### Vad som hade behövts

Gpu:n gör sin del bra. Att den inte hjälper beror på fyra saker som alla är
mätta ovan, och tre av dem är egenskaper hos maskinen, inte hos koden:

1. **Delat RAM ger ett tak på 1.11x.** Ett diskret kort med eget
   VRAM har högre tak — men då ska data dit över PCIe, och det är ett nytt
   tak på samma plats.
2. **Entropikodningen är seriell.** En gpu-packare behöver en kodning som är
   parallell i sig (tusentals oberoende småblock med egen ANS-tabell), och
   det är ett annat format med en annan kvot, inte en snabbare version av
   det här.
3. **zstd är inte min cpu-kod.** Gpu:n slår min egen cpu-matchare 2,5x med
   identisk semantik. zstd gör ett *smartare* jobb på cpu:n — hoppar över
   arbete min matchare utför — och vinner därför med marginal ändå.
4. **Drivrutinen har en vakthund.** En kärna som maler i flera sekunder
   dödas, och gpu:n sitter i samma krets som skärmen: första försöket med ett
   helt 32 MiB-stycke i ett anrop gav `context lost ... hard recovery`.
   Anropen skivas nu till 256 Ki positioner i taget.

Läget finns kvar för att mätningen ska gå att göra om — `python3
matning/gpu-matning.py` skriver om `resultat/gpu.json` — inte för att det ska
användas. Vill du ha arkivet mindre: `--läge liten`. Vill du ha det fort:
`--läge snabb`.

---

## Hur den är byggd

```
src/nvz.py    packaren: klassning, dedup, sortering, block, register
src/png.py       PNG-utvecklingen och receptsökningen
src/jpeg.py      skalet runt nvjpg, och delningen i huvud och ström
src/nvjpg.c      förlustfri JPEG-ompackning (aritmetisk kodare + modeller)
src/nvcm.c        kontextblandande kompressor för text och källkod
korpus/          bygger testkorpusarna som hårdlänkar
matning/         mätharness och rapportgenerator
experiment/      försöken som designbesluten kommer ur
prov/            provsviten
resultat/        mätdata och loggar
```

**Arkivformatet** är block plus register:

```
"NVPACK\x01" | block 0 | block 1 | ... | register (zstd) | <u64 offset, u64 längd>
```

Varje block är en oberoende packad följd av objektkroppar med sin egen kodek.
Oberoendet är med flit: blocken packas parallellt på alla kärnor, och en
uppackning som bara vill ha en fil behöver bara röra ett block.

Registret ligger i parallella listor, inte i en lista av ordböcker — nyckel-
namnen står en gång i stället för en gång per objekt, och objektens platser i
blocken lagras inte alls utan räknas fram ur längderna. På ett träd med 6 000
små filer var registret 2,8 % av hela arkivet innan det; nu 1,8 %.

Att platserna räknas fram är en risk: räknar packaren och uppackaren olika
pekar varje post på fel objekt. Därför kontrollerar packningen sin egen
härledning mot de verkliga platserna innan registret skrivs.

Registret bär också en flagga `platt`. Den är satt när arkivet är ett arkiv av
EN fil — det gzip-ytan gör när man packar en fil — och betyder att posten ska
läggas direkt i målet i stället för i en katalog med trädets namn. Ett
enfilsarkiv packas genom en tillfällig katalog med en *hårdlänk* till filen:
hårdlänken är samma inod, så innehåll, rättigheter och tid är filens egna och
ingenting kopieras. Arkiv från före flaggan läses som träd, vilket de är.

---

## Vad som inte fungerar

* **Progressiv JPEG** lämnas rå. nvjpg klarar baseline sekventiell (vilket är
  vad kameror och telefoner producerar); progressiv kräver en annan avkodare.
* **PNG ur okända deflate-motorer** lämnas rå — Chrome, gnome-screenshot och
  allt som använder zlib-ng eller libdeflate. Täckningen skulle kunna breddas
  genom att binda in flera deflate-implementationer och prova dem i tur och
  ordning.
* **nvcm är långsam**: runt 1 MB/s. Den väljs bara i läget `liten`, och bara
  för block där provet visar att hon vinner.
* **Kodekvalet gäller ett helt block.** Ett block med både källkod och en
  talkolumn får en enda kodek. Finare block skulle välja bättre men förlora
  likheterna mellan filer.
* **Inga specialfiler.** Rör, uttag och enheter hoppas över, med varning.
  Hårda länkar återskapas som fristående filer (innehållet är identiskt, men
  länkningen bevaras inte).
* **Standard in strömmar inte.** `cat fil | nvz` fungerar, men strömmen
  landar i en tillfällig fil på vägen. Registret ligger sist i arkivet och
  läses först, så ett nvz-arkiv går inte att läsa framifrån som en
  gzip-ström. Det är formatets pris, inte en lucka i koden.
* **Ingen kryptering och ingen redundans.** Ett arkiv med hemligheter ska ha
  rättigheter 600 som i dagens skript, och en bit som vänder sig i ett block
  förstör blocket.
* **Film rörs inte.** mp4 och mkv är redan packade av en videokodare, och där
  finns inget motsvarande entropilager att byta ut. Ett träd som domineras av
  film kommer alltid att packas som film.
* **Gpu-läget gör inte packningen snabbare.** Det är byggt och mätt (eget
  avsnitt ovan): grafikkortet vinner 2,5x på matchningssökningen men förlorar
  på allt annat, och kodeken `nvlz` är sämre än `zstd -6` på både kvot och
  tid. Läget finns kvar som mätinstrument.
* **`.git` är nästa sten att vända på.** Ett packfile är en följd av
  zlib-strömmar, precis som en PNG, och samma receptsökning borde kunna
  veckla ut dem. Det är inte provat här, så det står som en idé och inte som
  en siffra.

---

## Provsviten

```bash
bash prov/prova-allt.sh
```

Bygger ett träd av kantfall — tomma filer, dubbletter, symlänkar (relativa,
trasiga, och till kataloger), filnamn med nyrad och åäö i, udda rättigheter,
tidsstämplar från 1985 och 2031 — packar det i alla tre lägen och kräver att
varenda byte, rättighet och tidsstämpel kommer tillbaka.

Sviten provar också kommandoradens gzip-yta: att originalet ersätts, att
arkivet ärver filens tid och rättigheter, att rundan genom standard in och ut
är bit-exakt, att en katalog blir ett arkiv med trädet kvar, att `-r` packar
fil för fil och är upprepbart, och att slutstatus skiljer fel (1) från
varning (2) — skript hänger på den skillnaden.

Är gpu-delen byggd provas också den: att gpu-hashen är bit för bit
`hashlib.blake2b(digest_size=16)`, att matcharens svar är identiska på gpu och
cpu, att kodeken `nvlz` klarar text, slump, tomt och data över styckgränsen,
och — viktigast — att ett nvlz-arkiv går att packa upp **utan
vulkan-drivrutin** (`VK_ICD_FILENAMES=/dev/null`). Saknas gpu-delen hoppas
avsnittet över utan att räknas som fel.

Sviten hittade två riktiga fel första gången den kördes: en symlänk till en
katalog **försvann tyst** ur arkivet (den dyker upp bland `os.walk`s kataloger,
inte bland dess filer), och katalogernas egna tidsstämplar återställdes inte
alls. Båda är rättade; båda hade gått obemärkta förbi i varje mätning, för
mätningarna jämför innehåll och inte träd.
