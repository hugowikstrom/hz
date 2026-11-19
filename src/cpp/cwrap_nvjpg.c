// SPDX-License-Identifier: GPL-2.0-only
/*
 * nvjpg linked into the nvz binary, the same way as nvcm.
 *
 * nvjpg's die() does exit(2) — among other things for progressive JPEG,
 * which is an expected case, not an error. These functions therefore never
 * run in the packer's own process but in a worker nvz starts from itself
 * (/proc/self/exe nvjpg pack). An exit there is a refusal, not a crash.
 */
#define main nvjpg_main_unused
#include "../nvjpg.c"
#undef main

int nvz_nvjpg_pack(void)   { return pack_main(); }
int nvz_nvjpg_unpack(void) { return unpack_main(); }
