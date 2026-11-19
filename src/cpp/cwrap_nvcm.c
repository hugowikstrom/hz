// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvcm linked into the nvz binary.
 *
 * nvcm.c is a standalone program with a main(). Its main is renamed away
 * here and two functions with other names are exposed instead. Everything
 * else in the file is static, so nothing collides with the nvjpg wrapper
 * next door — they are separate translation units.
 */
#define main nvcm_main_unused
#include "../nvcm.c"
#undef main

int nvz_nvcm_pack(void)   { build_stretch(); build_rates(); return pack_stream(stdin, stdout); }
int nvz_nvcm_unpack(void) { build_stretch(); build_rates(); return unpack_stream(stdin, stdout); }
