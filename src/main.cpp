#include "app.h"
#include <SDL3/SDL_main.h>
#include "platform/platform.h"
#include "git/repository.h"
#include "ssh/session.h"
#include <cstdio>
#include <cstdlib>

int main(int argc, char** argv) {
    oh::platform::init();

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
    oh::platform::shutdown();

    return exit_code;
}
