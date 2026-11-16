/* nvjpg inlänkad i nvz-binären, på samma sätt som nvcm.
 *
 * nvjpg:s dö() gör exit(2) — bland annat för progressiv JPEG, som är ett
 * VÄNTAT fall, inte ett fel. Därför körs de här funktionerna aldrig i
 * packarens egen process utan i en arbetare som nvz startar av sig själv
 * (/proc/self/exe nvjpg packa). En exit där är en vägran, inte en krasch. */
#define main nvjpg_main_oanvand
#include "../nvjpg.c"
#undef main

int nvz_nvjpg_packa(void)    { return packa(); }
int nvz_nvjpg_packaupp(void) { return packa_upp(); }
