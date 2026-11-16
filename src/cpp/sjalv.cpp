#include "sjalv.hpp"
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <thread>
#include <cstring>

namespace nvz {

std::string binar_bredvid(const std::string &namn) {
    char egen[4096];
    ssize_t l = readlink("/proc/self/exe", egen, sizeof egen - 1);
    if (l > 0) {
        egen[l] = 0;
        std::string granne = std::string(egen);
        granne = granne.substr(0, granne.find_last_of('/') + 1) + namn;
        if (access(granne.c_str(), X_OK) == 0) return granne;
    }
    return namn;      // får sökas i PATH av execvp
}

Byte kor_binar(const std::string &binar, const std::vector<std::string> &arg,
               const u8 *in, size_t n, int *felkod) {
    std::vector<std::string> alla = {binar};
    alla.insert(alla.end(), arg.begin(), arg.end());
    return kor_sjalv(alla, in, n, felkod);   // kor_sjalv ser binärnamnet i arg[0]
}

Byte kor_sjalv(const std::vector<std::string> &arg, const u8 *in, size_t n, int *felkod) {
    // ALLT barnet behöver byggs FÖRE fork. Packaren är trådad, och ett barn
    // av en trådad process ärver låsen som andra trådar höll i
    // fork-ögonblicket — ett enda malloc i barnet kan då vänta för evigt på
    // ett lås vars ägare inte följde med. Mellan fork och exec får bara
    // async-signal-säkra anrop förekomma: dup2, close, execv, _exit.
    static char sjalv[] = "/proc/self/exe";
    bool egen_binar = !arg.empty() &&
        (arg[0].find('/') != std::string::npos || arg[0] == "nvgpu");
    std::vector<char*> argv;
    if (!egen_binar) argv.push_back(sjalv);
    for (const auto &a : arg) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    const char *program = egen_binar ? argv[0] : "/proc/self/exe";

    // O_CLOEXEC på allt: arbetarna startas från många trådar samtidigt, och
    // ett barn som forkas medan en ANNAN tråd har sina rör öppna ärver även
    // dem. Utan close-on-exec håller barnet då grannens skrivände öppen,
    // grannens arbetare får aldrig EOF på stdin, och båda står för evigt.
    // dup2 tar bort flaggan på just de två fd:er barnet ska ha — resten
    // försvinner i exec.
    int in_ror[2], ut_ror[2];
    if (pipe2(in_ror, O_CLOEXEC) || pipe2(ut_ror, O_CLOEXEC)) throw Fel("pipe misslyckades");
    pid_t barn = fork();
    if (barn < 0) throw Fel("fork misslyckades");
    if (barn == 0) {
        dup2(in_ror[0], 0); dup2(ut_ror[1], 1);
        close(in_ror[0]); close(in_ror[1]); close(ut_ror[0]); close(ut_ror[1]);
        if (egen_binar) execvp(program, argv.data());
        else execv(program, argv.data());
        _exit(127);
    }
    close(in_ror[0]); close(ut_ror[1]);

    // Skrivaren får en egen tråd. Utan den låser sig stora strömmar: barnet
    // fyller sitt utrör, vi står och skriver i dess inrör, ingen läser.
    std::thread skrivare([&] {
        size_t kvar = n;
        const u8 *p = in;
        while (kvar) {
            ssize_t s = write(in_ror[1], p, kvar);
            if (s <= 0) break;
            p += s; kvar -= (size_t)s;
        }
        close(in_ror[1]);
    });

    Byte ut;
    u8 buf[1 << 16];
    for (;;) {
        ssize_t r = read(ut_ror[0], buf, sizeof buf);
        if (r <= 0) break;
        ut.insert(ut.end(), buf, buf + r);
    }
    close(ut_ror[0]);
    skrivare.join();
    int status = 0;
    waitpid(barn, &status, 0);
    *felkod = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return ut;
}

}  // namespace nvz
