/* nvcm inlänkad i nvz-binären.
 *
 * nvcm.c är ett eget program med main(). Här byts main bort och två
 * funktioner med andra namn exponeras i stället. Allt annat i filen är
 * static, så inget kolliderar med nvjpg-wrappen bredvid — de är varsin
 * översättningsenhet. */
#define main nvcm_main_oanvand
#include "../nvcm.c"
#undef main

int nvz_nvcm_packa(void)    { bygg_stretch(); bygg_takt(); return packa_strom(stdin, stdout); }
int nvz_nvcm_packaupp(void) { bygg_stretch(); bygg_takt(); return packa_upp_strom(stdin, stdout); }
