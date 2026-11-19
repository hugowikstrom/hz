// SPDX-License-Identifier: GPL-2.0-only
// Worker plumbing: locate a sibling binary and run a child process with
// piped stdin/stdout from a threaded parent.
#include "worker.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <thread>
#include <cstring>

namespace nvz {

std::string sibling_binary(const std::string &name) {
    char self[4096];
    ssize_t l = readlink("/proc/self/exe", self, sizeof self - 1);
    if (l > 0) {
        self[l] = 0;
        std::string next_door = std::string(self);
        next_door = next_door.substr(0, next_door.find_last_of('/') + 1) + name;
        if (access(next_door.c_str(), X_OK) == 0) return next_door;
    }
    return name;      // let execvp search PATH
}

Byte run_program(const std::string &binary, const std::vector<std::string> &arg,
                 const u8 *in, size_t n, int *exit_code) {
    std::vector<std::string> all = {binary};
    all.insert(all.end(), arg.begin(), arg.end());
    return run_self(all, in, n, exit_code);   // run_self sees the binary name in arg[0]
}

Byte run_self(const std::vector<std::string> &arg, const u8 *in, size_t n, int *exit_code) {
    // Everything the child needs is built before fork. The packer is
    // threaded, and a child of a threaded process inherits the locks other
    // threads held at the moment of the fork — a single malloc in the
    // child can then wait forever on a lock whose owner did not come
    // along. Observed symptom: workers at 0 % CPU for 84 minutes. Between
    // fork and exec only async-signal-safe calls may occur: dup2, close,
    // execv, _exit.
    static char self[] = "/proc/self/exe";
    bool external = !arg.empty() &&
        (arg[0].find('/') != std::string::npos || arg[0] == "nvgpu");
    std::vector<char*> argv;
    if (!external) argv.push_back(self);
    for (const auto &a : arg) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char *program = external ? argv[0] : "/proc/self/exe";

    // O_CLOEXEC on everything: workers start from many threads at once,
    // and a child forked while another thread has its pipes open inherits
    // those too. Without close-on-exec the child holds the neighbour's
    // write end open, the neighbour's worker never sees EOF on stdin, and
    // both hang forever. dup2 clears the flag on exactly the two fds the
    // child should keep; the rest close on exec.
    int in_pipe[2], out_pipe[2];
    if (pipe2(in_pipe, O_CLOEXEC) || pipe2(out_pipe, O_CLOEXEC)) throw Error("pipe failed");
    pid_t child = fork();
    if (child < 0) throw Error("fork failed");
    if (child == 0) {
        dup2(in_pipe[0], 0); dup2(out_pipe[1], 1);
        close(in_pipe[0]); close(in_pipe[1]); close(out_pipe[0]); close(out_pipe[1]);
        if (external) execvp(program, argv.data());
        else execv(program, argv.data());
        _exit(127);
    }
    close(in_pipe[0]); close(out_pipe[1]);

    // The writer gets its own thread. Without it, large streams deadlock:
    // the child fills its output pipe, we are stuck writing into its input
    // pipe, nobody reads.
    std::thread writer([&] {
        size_t left = n;
        const u8 *p = in;
        while (left) {
            ssize_t s = write(in_pipe[1], p, left);
            if (s <= 0) break;
            p += s; left -= (size_t)s;
        }
        close(in_pipe[1]);
    });

    Byte out;
    u8 buf[1 << 16];
    for (;;) {
        ssize_t r = read(out_pipe[0], buf, sizeof buf);
        if (r <= 0) break;
        out.insert(out.end(), buf, buf + r);
    }
    close(out_pipe[0]);
    writer.join();
    int status = 0;
    waitpid(child, &status, 0);
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return out;
}

}  // namespace nvz
