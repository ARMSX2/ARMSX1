#include "frontend/mali_present_timing.h"
#include <cassert>
#include <cstdio>
#undef puts
static armsx_gpu_profile_t profile{};
static unsigned clock_reads=0, reports=0;
static Uint64 ticks=100;
extern "C" const armsx_gpu_profile_t* armsx_gpu_profile_get(void) { return &profile; }
extern "C" Uint64 SDL_GetPerformanceCounter(void) { ++clock_reads; ticks+=500; return ticks; }
extern "C" Uint64 SDL_GetPerformanceFrequency(void) { return 1000; }
extern "C" void psxe_diag_pacingf(const char*, ...) { ++reports; }
int main() {
    MaliPresentStats stats;
    profile.vendor=ARMSX_GPU_VENDOR_ADRENO;
    { MaliPresentTiming t; for(unsigned i=0;i<4;++i)t.mark(i);
      t.finish(stats,"test","a","b","c","d",320,240,320,240); }
    assert(clock_reads==0 && reports==0 && stats.frames==0);
    profile.vendor=ARMSX_GPU_VENDOR_MALI;
    { MaliPresentTiming t; for(unsigned i=0;i<4;++i)t.mark(i);
      t.finish(stats,"test","a","b","c","d",320,240,320,240); }
    assert(clock_reads==5 && reports==1 && stats.frames==0);
    puts("Mali timing gate and reporting tests passed");
}
