#include "../psx/psx.h"
#include <assert.h>
#include <stdio.h>

void gpu_hblank_event(psx_gpu_t*);

int main(void) {
    psx_gpu_t* g = psx_gpu_create();
    psx_ic_t* ic = psx_ic_create();
    psx_cpu_t* cpu = psx_cpu_create();
    psx_ic_init(ic, cpu);
    psx_gpu_init(g, ic);
    unsigned checks = 0;
    for (unsigned pal = 0; pal < 2; ++pal) {
        for (unsigned interlace = 0; interlace < 2; ++interlace) {
            for (unsigned high = 0; high < 2; ++high) {
                psx_gpu_write32(g, 4, 0x08000000u | (pal << 3) |
                                (interlace << 5) | (high << 2));
                g->line = 0;
                const unsigned length = pal ? GPU_SCANS_PER_FRAME_PAL : GPU_SCANS_PER_FRAME_NTSC;
                const unsigned active = pal ? GPU_SCANS_PER_VDRAW_PAL : GPU_SCANS_PER_VDRAW_NTSC;
                for (unsigned frame = 0; frame < 4; ++frame) {
                    const unsigned field = (g->gpustat >> 13) & 1u;
                    if (!interlace) assert(field == 1);
                    for (unsigned line = 0; line < length; ++line) {
                        gpu_hblank_event(g);
                        const unsigned expected = line + 1 < active
                            ? ((interlace && high) ? field : (line & 1u)) : 0;
                        assert((g->gpustat >> 31) == expected);
                        ++checks;
                    }
                    assert(g->line == 0);
                    assert(((g->gpustat >> 13) & 1u) == (interlace ? !field : 1));
                }
            }
        }
    }
    psx_gpu_destroy(g);
    psx_ic_destroy(ic);
    psx_cpu_destroy(cpu);
    printf("GPU field/scanline timing: %u checks passed\n", checks);
    return 0;
}
