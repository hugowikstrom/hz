// SPDX-License-Identifier: GPL-2.0-only
// Worker processes: run code from this binary, or a sibling binary, in a
// separate process so its failures stay its own.
#pragma once
#include "hz.hpp"

namespace hz {

// Run our own binary as a worker: /proc/self/exe <arg...> with *in* on
// stdin. Returns the worker's output; *exit_code* receives its status.
// This is how nvcm and nvjpg are used: their code is linked into the same
// binary but runs in its own process, so a refusal (exit 2 on progressive
// JPEG) cannot bring down the packer, and a broken stream cannot damage the
// archive.
Byte run_self(const std::vector<std::string> &arg, const u8 *in, size_t n, int *exit_code);

// The same, but with a named binary. Used for nvgpu, which is a separate
// program (the Vulkan dependency must not get into hz). The binary is
// looked for next to our own first, then in PATH.
Byte run_program(const std::string &binary, const std::vector<std::string> &arg,
                 const u8 *in, size_t n, int *exit_code);
std::string sibling_binary(const std::string &name);

}  // namespace hz
