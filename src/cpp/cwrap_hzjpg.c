// SPDX-License-Identifier: GPL-2.0-only
/*
 * hzjpg linked into the hz binary, the same way as nvcm.
 *
 * hzjpg's die() does exit(2) — among other things for progressive JPEG,
 * which is an expected case, not an error. These functions therefore never
 * run in the packer's own process but in a worker hz starts from itself
 * (/proc/self/exe hzjpg pack). An exit there is a refusal, not a crash.
 */
#define main hzjpg_main_unused
#include "../hzjpg.c"
#undef main

int hzjpg_pack(void)   { return pack_main(); }
int hzjpg_unpack(void) { return unpack_main(); }
