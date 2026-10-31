#include <assert.h>
#include <stdio.h>
#include "../psx/dev/gpu.h"

int main(void) {
    static psx_gpu_t gpu;
    /* Captured SCPH-1001 boot: VRAM row 2, field lines 16 through 254. */
    gpu.display_mode = 0x27;
    gpu.disp_y = 2;
    gpu.disp_y1 = 16;
    gpu.disp_y2 = 255;
    assert(psx_gpu_display_height(&gpu) == 478);
    assert(gpu.disp_y + psx_gpu_display_height(&gpu) == 480);
    gpu.disp_y2 = 256;
    assert(psx_gpu_display_height(&gpu) == 480);
    gpu.disp_y2 = 240;
    assert(psx_gpu_display_height(&gpu) == 448);
    gpu.disp_y2 = 272;
    assert(psx_gpu_display_height(&gpu) == 512);
    gpu.disp_y2 = 1023;
    assert(psx_gpu_display_height(&gpu) == 512);
    gpu.disp_y2 = 16;
    assert(psx_gpu_display_height(&gpu) == 0);
    gpu.disp_y2 = 0;
    assert(psx_gpu_display_height(&gpu) == 0);
    gpu.disp_y2 = 240;
    gpu.display_mode = 0x03;
    assert(psx_gpu_display_height(&gpu) == 224);
    gpu.display_mode = 0x07; /* High-resolution bit alone is not interlace. */
    assert(psx_gpu_display_height(&gpu) == 224);
    gpu.disp_y2 = 256;
    assert(psx_gpu_display_height(&gpu) == 240);
    puts("GPU display-height tests passed");
    return 0;
}
