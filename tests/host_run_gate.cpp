#include "frontend/host_run_gate.h"

#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <thread>

int main() {
    HostRunGate gate;
    assert(!gate.claim(0) && !gate.cancel(0) && !gate.release(0, true));
    const uint64_t cancelled = gate.prepare();
    assert(cancelled && gate.prepare() == 0);
    assert(gate.cancel(cancelled));
    assert(!gate.claim(cancelled));
    assert(gate.release(cancelled, false) && !gate.active());

    const uint64_t startup = gate.prepare();
    assert(startup != cancelled && gate.claim(startup));
    assert(!gate.claim(startup) && !gate.release(startup, false));
    assert(gate.cancel(startup) && gate.cancelled(startup));
    assert(gate.release(startup, true));

    const uint64_t next = gate.prepare();
    assert(next != startup && !gate.cancelled(next));
    assert(!gate.cancel(startup) && !gate.release(startup, true));
    assert(gate.claim(next) && !gate.cancelled(next));
    assert(gate.release(next, true));

    const uint64_t failed = gate.prepare();
    assert(gate.release(failed, false));
    const uint64_t retry = gate.prepare();
    assert(gate.claim(retry) && gate.release(retry, true));

    std::mutex mutex;
    std::condition_variable changed;
    bool stop_queued = false;
    bool new_run_started = false;
    bool stale_stop_accepted = true;
    const uint64_t old = gate.prepare();
    assert(gate.claim(old));
    std::thread delayed_stop([&] {
        std::unique_lock<std::mutex> lock(mutex);
        stop_queued = true;
        changed.notify_all();
        changed.wait(lock, [&] { return new_run_started; });
        stale_stop_accepted = gate.cancel(old);
    });
    uint64_t fresh = 0;
    {
        std::unique_lock<std::mutex> lock(mutex);
        changed.wait(lock, [&] { return stop_queued; });
        assert(gate.release(old, true));
        fresh = gate.prepare();
        assert(gate.claim(fresh));
        new_run_started = true;
        changed.notify_all();
    }
    delayed_stop.join();
    assert(!stale_stop_accepted && !gate.cancelled(fresh) && gate.active() == fresh);
    assert(gate.release(fresh, true));
    puts("host run reservation, startup cancellation, stale stop and release checks passed");
}
