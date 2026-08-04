#pragma once

#include <SDL.h>

namespace armsx {

// SDL subsystem initialization is reference-counted. Embedded frontends therefore must not
// initialize a subsystem merely because it is already available: doing so adds a reference the
// embedder cannot distinguish from its own. A lease acquires only the requested bits that were
// missing at the time of the call and releases exactly those bits.
class SdlSubsystemLease final {
public:
    SdlSubsystemLease() = default;
    ~SdlSubsystemLease() { release(); }

    SdlSubsystemLease(const SdlSubsystemLease&) = delete;
    SdlSubsystemLease& operator=(const SdlSubsystemLease&) = delete;

    bool acquire(Uint32 requested) {
        const Uint32 before = SDL_WasInit(requested);
        const Uint32 missing = requested & ~before;
        if (missing == 0) {
            return true;
        }

        SDL_SetMainReady();
        if (SDL_InitSubSystem(missing) != 0) {
            // SDL normally rolls a failed initialization back itself. Balance any requested
            // bits it did leave live so a driver fallback starts from a genuinely clean state.
            const Uint32 partial = SDL_WasInit(requested) & ~before;
            if (partial != 0) {
                SDL_QuitSubSystem(partial);
            }
            return false;
        }

        acquired_ |= missing;
        return true;
    }

    void release() {
        if (acquired_ == 0) {
            return;
        }
        SDL_QuitSubSystem(acquired_);
        acquired_ = 0;
    }

    Uint32 acquired() const { return acquired_; }

private:
    Uint32 acquired_ = 0;
};

} // namespace armsx
