#include <assert.h>
#include <stdio.h>
#include "psx/perf.h"
#define main cpu_reference_main
#include "cpu_differential.c"
#undef main

static uint64_t ticks;
static uint64_t clock_tick(void) { return ticks++; }

int main(void) {
    psx_work_diag_begin(NULL);
    assert(psx_work_diag_start() == 0);
    psx_work_diag_end(PSX_WORK_SPU, 0);
    assert(ticks == 0 && g_psx_work_diag.calls[PSX_WORK_SPU] == 0);

    psx_work_diag_begin(clock_tick);
    uint64_t outer = psx_work_diag_start();
    uint64_t inner = psx_work_diag_start();
    psx_work_diag_end(PSX_WORK_DISC, inner);
    psx_work_diag_end(PSX_WORK_AUDIO_QUEUE, outer);
    assert(g_psx_work_diag.ticks[PSX_WORK_DISC] == 1);
    assert(g_psx_work_diag.ticks[PSX_WORK_AUDIO_QUEUE] == 3);
    assert(g_psx_work_diag.calls[PSX_WORK_DISC] == 1);
    assert(g_psx_work_diag.max_ticks[PSX_WORK_AUDIO_QUEUE] == 3);

    g_psx_work_diag.cpu_sample_phase = 4092;
    psx_work_diag_begin(clock_tick);
    assert(g_psx_work_diag.calls[PSX_WORK_DISC] == 0);
    assert(g_psx_work_diag.cpu_sample_phase == 4092);
    uint64_t before = ticks;
    psx_work_diag_end(PSX_WORK_COUNT, 0);
    assert(ticks == before);
    psx_work_diag_begin(NULL);
    psx_work_diag_end(PSX_WORK_DISC, 0);
    assert(ticks == before);
    assert(write_blank_bios("build/tests/blank-bios.bin"));
    psx_t *reference = NULL, *profiled = NULL;
    assert(init_pair(&reference, &profiled, "build/tests/blank-bios.bin"));
    const uint32_t program[] = {0x24420001u, 0x08000400u, 0};
    write_program(reference, program, 3);
    write_program(profiled, program, 3);
    psx_spu_begin_frame(reference->spu, 64);
    psx_spu_begin_frame(profiled->spu, 64);
    psx_work_diag_begin(NULL);
    for (unsigned i = 0; i < 10000; ++i) psx_update(reference);
    g_psx_work_diag.cpu_sample_phase = 0;
    psx_work_diag_begin(clock_tick);
    for (unsigned i = 0; i < 10000; ++i) psx_update(profiled);
    assert(compare_pair("work-probe", 10000, reference, profiled));
    assert(g_psx_work_diag.calls[PSX_WORK_CPU_SAMPLE] == 2);
    assert(g_psx_work_diag.calls[PSX_WORK_SPU] > 0);
    assert(reference->spu->gen_count == profiled->spu->gen_count);
    assert(memcmp(reference->spu->gen_ring, profiled->spu->gen_ring,
                  sizeof(reference->spu->gen_ring)) == 0);
    psx_work_diag_begin(NULL);
    psx_destroy(reference); psx_destroy(profiled);
    puts("work diagnostics tests passed");
    return 0;
}
