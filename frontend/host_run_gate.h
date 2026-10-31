#pragma once

#include <cstdint>

class HostRunGate {
public:
    uint64_t prepare() {
        if (active_ != 0) return 0;
        if (++sequence_ == 0) ++sequence_;
        active_ = sequence_;
        claimed_ = false;
        cancelled_ = false;
        return active_;
    }

    bool claim(uint64_t token) {
        if (!matches(token) || claimed_ || cancelled_) return false;
        claimed_ = true;
        return true;
    }

    bool cancel(uint64_t token) {
        if (!matches(token)) return false;
        cancelled_ = true;
        return true;
    }

    bool release(uint64_t token, bool finished) {
        if (!matches(token) || (claimed_ && !finished)) return false;
        active_ = 0;
        claimed_ = false;
        cancelled_ = false;
        return true;
    }

    bool matches(uint64_t token) const { return token != 0 && token == active_; }
    bool cancelled(uint64_t token) const { return matches(token) && cancelled_; }
    uint64_t active() const { return active_; }

private:
    uint64_t sequence_ = 0;
    uint64_t active_ = 0;
    bool claimed_ = false;
    bool cancelled_ = false;
};
