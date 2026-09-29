#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../psx/dev/gpu.h"

int main(void) {
    static psx_gpu_t gpu;
    /* Independent expectations from the current scanout clock/line model.
       These are not claims about every interlaced hardware video mode. */
    const double ntsc = 53693175.0 / (3413.0 * 263.0);
    const double pal = 53203425.0 / (3406.0 * 314.0);
    assert(fabs(psx_gpu_frame_rate(NULL) - ntsc) < 0.00001);
    for (unsigned mode = 0; mode < 128; ++mode) {
        gpu.display_mode = mode;
        const double expected = (mode & 8) ? pal : ntsc;
        assert(fabs(psx_gpu_frame_rate(&gpu) - expected) < 0.00001);
        /* One second of pacing must cover one second of emulated scanout. */
        const double clocks = (mode & 8) ? 53203425.0 : 53693175.0;
        const double frame_clocks = (mode & 8) ? 3406.0 * 314.0 : 3413.0 * 263.0;
        assert(fabs(psx_gpu_frame_rate(&gpu) * frame_clocks / clocks - 1.0) < 0.000001);
    }
    printf("GPU frame-rate tests passed: NTSC %.6f Hz, PAL %.6f Hz\n", ntsc, pal);
    return 0;
}
