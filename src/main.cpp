#include "app.h"
#include "git/repository.h"
#include "ssh/session.h"
#include <cstdio>
#include <csignal>
#include <cstdlib>

int main(int argc, char** argv) {
    // Child processes (LSP servers, shells) may exit mid-write; report EPIPE instead of dying.
    std::signal(SIGPIPE, SIG_IGN);

    oh::GitRepository::global_init();
    oh::SSHSession::global_init();

    int exit_code = 0;
    {
        oh::App app;
        if (!app.init(argc, argv)) {
            std::fprintf(stderr, "overheat: failed to initialize\n");
            exit_code = 1;
        } else {
            app.run();
        }
    }

    oh::SSHSession::global_shutdown();
    oh::GitRepository::global_shutdown();

    return exit_code;
}
