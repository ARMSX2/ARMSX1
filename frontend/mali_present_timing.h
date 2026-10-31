#ifndef ARMSX_MALI_PRESENT_TIMING_H
#define ARMSX_MALI_PRESENT_TIMING_H
#include <SDL.h>
#include <algorithm>
#include <cstdint>
#include "gpu_profile.h"
#include "diagnostics.h"

struct MaliPresentStats {
    uint64_t start = 0, sum[4] = {}, peak[4] = {};
    unsigned frames = 0, failures = 0, scaled = 0;
};

// CPU wall time only: a blocked API includes queue/driver waiting, not just
// GPU execution. No timing calls or logging on other GPU vendors.
class MaliPresentTiming {
public:
    MaliPresentTiming() : active(armsx_gpu_profile_get()->vendor == ARMSX_GPU_VENDOR_MALI) {
        if (active) begin = last = SDL_GetPerformanceCounter();
    }
    void mark(unsigned phase) {
        if (!active) return;
        const uint64_t now = SDL_GetPerformanceCounter();
        elapsed[phase] += now - last;
        last = now;
    }
    void finish(MaliPresentStats& stats, const char* path,
                const char* p0, const char* p1, const char* p2, const char* p3,
                int sw, int sh, int dw, int dh, bool failed = false, bool scaled = false) {
        if (!active) return;
        if (!stats.start) stats.start = begin;
        ++stats.frames;
        stats.failures += failed;
        stats.scaled += scaled;
        for (unsigned i=0; i<4; ++i) {
            stats.sum[i] += elapsed[i];
            stats.peak[i] = std::max(stats.peak[i], elapsed[i]);
        }
        const uint64_t frequency = SDL_GetPerformanceFrequency();
        if (last - stats.start < frequency * 2) return;
        const double ms = 1000.0 / static_cast<double>(frequency);
        psxe_diag_pacingf("present_detail path=%s model=%d samples=%u src=%dx%d dst=%dx%d "
                         "%s_ms=%.3f/%.3f %s_ms=%.3f/%.3f %s_ms=%.3f/%.3f %s_ms=%.3f/%.3f "
                         "failures=%u scaled_frames=%u (avg/max CPU wall time)",
                         path, armsx_gpu_profile_get()->model, stats.frames, sw, sh, dw, dh,
                         p0, stats.sum[0]*ms/stats.frames, stats.peak[0]*ms,
                         p1, stats.sum[1]*ms/stats.frames, stats.peak[1]*ms,
                         p2, stats.sum[2]*ms/stats.frames, stats.peak[2]*ms,
                         p3, stats.sum[3]*ms/stats.frames, stats.peak[3]*ms,
                         stats.failures, stats.scaled);
        stats = {};
    }
private:
    bool active;
    uint64_t begin = 0, last = 0, elapsed[4] = {};
};
#endif
