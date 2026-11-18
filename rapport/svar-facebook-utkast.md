=== HUVUDKOMMENTAR (ett inlägg i tråden) ===

Svar till alla, nu när proven är körda. Och först det viktigaste:

TED LYNGMOS TRE BRISTER VAR VERKLIGA. ALLA TRE ÄR ÅTGÄRDADE.

1) PNG. Receptet antog att fyra zlib-parametrar bestämmer utströmmen. Det
gör de inte — en annan deflate-motor ger andra bytes, och zlib-ng är
standard på fler och fler distributioner. Vid genomgången hittade jag något
värre än Ted rapporterade: koden kopierade skelettets CRC rakt in i
utdatan, så en ström med RÄTT LÄNGD men fel bytes hade blivit en PNG med fel
bilddata och en checksumma som ser riktig ut. Tyst korruption är det enda en
packare aldrig får göra. Nu CRC-verifieras varje IDAT-bit, receptet bär ett
avtryck av motorn som packade, och om receptet inte återskapar strömmen söker
nvz om bland parametrarna — vilket räddar arkivet i de flesta fall — och
vägrar med ett begripligt fel när inget fungerar. Aldrig fel data.

2) Skrivfel gav tyst dataförlust. `nvz -c > /dev/full` gav slutstatus 0 där
gzip ger 1, och vid full disk kunde originalet raderas efter ett stympat
arkiv. Nu kontrolleras varje skrivning, det fsyncas före stängning, ett
halvskrivet arkiv tas bort och källan rörs inte. Mätt: filtak → slutstatus 1,
md5 på originalet oförändrad, inget skräp kvar.

3) En oläsbar fil i `nvz a b c` kraschade med SIGABRT (134) och hade redan
raderat a:s original. Nu: varning, b hoppas över, a och c packas,
slutstatus 1 — exakt gzips kontrakt, mätt på samma fall.

Teds reproduktionsskript ligger nu i repot som regressionsprov:
prov/brister.sh. Sex gröna prov. Tack — det var den mest värdefulla
återkopplingen projektet har fått, och alla tre felen hade samma orsak:
packningens egen kontroll vaktade fel gräns. Den packar och packar upp på
SAMMA maskin, i ett lyckat flöde, med läsbara filer och ledig disk.

TESTDATA (Jim Groth, Christopher Lindberg, Björn Rohlén, Stefan Alfredsson,
Jonas Danielsson): linuxkärnan 6.12 hämtad från kernel.org — 1 476 500 559
byte i 86 618 filer, md5 före och efter: 86 618 av 86 618 OK, 0 fel.
zlib-ng/corpora: åtta korpusar valda av någon annan, sju vinster av åtta.
Silesia och enwik8 som litteraturen använder. Och 1 000 slumpgenererade träd
genom hela rundgången. Nvz är inte anpassad till min testdata: utfallet på
kärnan (−12,0 % mot bästa klassiska) ligger i samma storleksordning som på
min egen kodkorpus (−11,6 %).

SORTERINGEN (Stefan Alfredsson): du har rätt, och mätningen ger dig rätt på
ett sätt som inte gynnar mig. Med kompressorn konstant mäter sorteringen
NOLL på både kod och PNG — med litet fönster till och med svagt negativt.
Den är värd något först när likheterna ligger utspridda OCH fönstret är
litet: samma kodfiler utslängda i 200 slumpade kataloger ger −8,0 % med
2 MiB fönster och −2,2 % med 64 MiB ordbok. Precis den funktion av
fönsterstorleken du beskrev. Rapporten har nu ett avsnitt om förarbetet som
ger gwern.net/sort, precomp, preflate, packJPG, Lepton, PAQ och FreeArc var
sin rad — ingen metod här är ny, och på ett ställe är nvz sämre än känd
teknik (JPEG: −15,1 % mot publicerade −20 till −22 %).

TIDEN (Mikael Wedlund, Johan Rönnblom, Marcus Burken Brandberg): den hårda
frågan är om nvz slår SAMMA motståndare på båda axlarna samtidigt. Sex
korpusar av nio: kod −0,57 % och 15 % snabbare än xz, PNG −10,4 % och 18 %,
foto −14,8 % och 16 %, loggar −15,4 % och 79 %, Silesia −5,7 % och 37 %,
arkiv −0,11 % och 1 %. De tre återstående är de tre där det inte finns något
att vinna. Och de 100× i uppackning gäller bara läget `liten` på text —
`normal` packar upp kodkorpusen på en sekund, snabbare än tar.xz.

ENTROPIN (Peter Bergström, Fredrik Persson, Fredric Georgsson, Joachim
Vist): gränsen gäller och nvz påstår inte annat. Den är en egenskap hos
datan UNDER EN MODELL, inte hos datan. Ingen rad i rapporten slår någon
informationsteoretisk gräns, och på enwik8 — där litteraturen mätt i trettio
år — FÖRLORAR nvz mot paq8px, 20,4 mot 15,8 MB. Om programmet ska räknas in:
binären är 386 kB, konstant, utan inbyggda ordböcker, och vinsten på
loggkorpusen är 1,8 MiB, fem gånger hela programmet.

Allt går att köra själv. Rapport, mätdata och skript:
https://nattviken.com/projekt/nvz/

=== SVAR TILL TED LYNGMO (under hans kommentar) ===

Tack. Alla tre reproducerade på första försöket, och alla tre är fixade.

Brist 1 var värre än du skrev: png_aterskapa kopierade skelettets CRC rakt
in i utdatan, så en ström med rätt längd men fel bytes hade gett en PNG med
fel bilddata och korrekt CRC — tyst korruption, inte ett fel. Nu verifieras
varje IDAT-bit mot sin CRC, receptet bär ett beteendebaserat avtryck av
deflate-motorn (provsträng → crc32 på utdatan), och vid miss söker nvz om
bland nivå/minnesnivå/strategi; fönstret varieras inte, det står i filens
eget zlib-huvud. Provat: ett recept vars sanning ligger i sökrummet räddas
bitexakt, ett som inte gör det VÄGRAS med båda motorernas avtryck i felet.

Brist 2: varje fwrite/fflush/fclose kontrolleras, fsync före stängning, och
ett arkiv som inte blev färdigt tas bort. Jag ignorerar dessutom SIGXFSZ med
flit — gzip dör på 153 och hinner inte städa; nvz får EFBIG, städar och ger
1.

Brist 3: operandloopen fångar per fil. Varning, resten packas, slutstatus 1.

Ditt prov ligger i repot som prov/brister.sh och är grönt när felen är
borta, så de kan inte komma tillbaka obemärkt.

=== SVAR TILL STEFAN ALFREDSSON (under hans sorteringskommentar) ===

Du har rätt, och det kostade mig ett påstående. Mätt med kompressorn
konstant är sorteringen värd NOLL på mina korpusar — med zstd −3 till och med
+0,14 %. Den betalar sig först när likheterna är utspridda och fönstret litet:
samma filer i 200 slumpade kataloger ger −8,0 % (2 MiB fönster) och −2,2 %
(64 MiB ordbok). Precis den fönsterberoende effekt du beskrev, och
gwern.net/sort står nu i rapportens avsnitt om förarbete. Vinsterna i nvz
kommer från PNG/JPEG-omvandlingarna, dedupen där det finns dubbletter, och
kontextblandaren — inte från sorteringen.

Kärntestet du bad om: linux-6.12 från kernel.org, md5 före och efter,
86 618 av 86 618 filer OK.
