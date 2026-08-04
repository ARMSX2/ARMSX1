#include "../frontend/sdl_subsystem_lease.h"

#include <cstdio>
#include <cstdlib>

namespace {

bool require(bool condition, const char* message) {
    if (!condition) {
        std::fprintf(stderr, "SDL_LEASE failed: %s (live=0x%x error=%s)\n",
                     message, SDL_WasInit(0), SDL_GetError());
        return false;
    }
    return true;
}

bool runSession() {
    armsx::SdlSubsystemLease host;
    armsx::SdlSubsystemLease core;

    if (!require(SDL_WasInit(0) == 0, "session did not start clean") ||
        !require(host.acquire(SDL_INIT_VIDEO | SDL_INIT_EVENTS), "host acquire failed") ||
        !require((host.acquired() & SDL_INIT_VIDEO) != 0, "host did not acquire video") ||
        !require(core.acquire(SDL_INIT_VIDEO | SDL_INIT_EVENTS), "core reuse failed") ||
        !require(core.acquired() == 0, "core incremented host-owned video/events") ||
        !require(core.acquire(SDL_INIT_AUDIO), "core audio acquire failed") ||
        !require((core.acquired() & SDL_INIT_AUDIO) != 0, "core did not acquire audio")) {
        return false;
    }

    core.release();
    if (!require(SDL_WasInit(SDL_INIT_AUDIO) == 0, "core left audio initialized") ||
        !require(SDL_WasInit(SDL_INIT_VIDEO | SDL_INIT_EVENTS) ==
                     (SDL_INIT_VIDEO | SDL_INIT_EVENTS),
                 "core released host-owned video/events")) {
        return false;
    }

    host.release();
    return require(SDL_WasInit(0) == 0, "host left a subsystem reference");
}

} // namespace

int main() {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("SDL_AUDIODRIVER", "dummy", 1);
    SDL_SetMainReady();

    for (int session = 0; session < 4; ++session) {
        if (!runSession()) {
            return 1;
        }
    }

    std::puts("SDL_LEASE passed sessions=4 residual=0");
    return 0;
}
