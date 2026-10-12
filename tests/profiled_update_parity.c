/* Instrumentation must not change guest timing, memory or peripheral state. */
#define main cpu_differential_main
#include "cpu_differential.c"
#undef main
#include "psx/perf.h"
#include <assert.h>

static uint64_t fake_clock(void) {
    static uint64_t ticks;
    return ticks += 17;
}

int main(void) {
    const char* bios = "build/tests/blank-bios.bin";
    assert(write_blank_bios(bios));
    const uint32_t program[] = {
        0x3c018000u, /* lui r1,0x8000 */
        0x24020005u, /* addiu r2,0,5 */
        0xac220100u, /* sw r2,0x100(r1) */
        0x8c230100u, /* lw r3,0x100(r1) */
        0x00000000u, /* load delay */
        0x24640007u, /* addiu r4,r3,7 */
        0x00820018u, /* mult r4,r2 */
        0x00003812u, /* mflo r7 */
        0x4a000006u, /* GTE NCLIP, explicit latency */
        0x08000400u, /* loop */
        0x00000000u,
    };
    unsigned cases = 0;
    const unsigned clocks[] = {100, 175, 200};
    for (unsigned mode = 0; mode < 2; ++mode)
    for (unsigned base = 1; base <= 2; ++base)
    for (unsigned c = 0; c < 3; ++c)
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        psx_t *plain = NULL, *sampled = NULL;
        assert(init_pair(&plain, &sampled, bios));
        psx_t* machines[] = {plain, sampled};
        for (unsigned i = 0; i < 2; ++i) {
            psx_t* p = machines[i];
            psx_cpu_set_execution_mode(p->cpu, mode);
            psx_cpu_set_base_instruction_cycles(p->cpu, base);
            /* Power-on scratchpad and VRAM allocations are unspecified. */
            memset(p->scratchpad->buf, 0, PSX_SCRATCHPAD_SIZE);
            memset(p->gpu->vram, 0, PSX_GPU_VRAM_SIZE);
            write_program(p, program, sizeof(program) / sizeof(program[0]));
            psx_spu_begin_frame(p->spu, 1024);
            psx_bus_write32(p->bus, 0x1f801104u, 0x0058u); /* timer 0 IRQ/reset mode */
            psx_bus_write32(p->bus, 0x1f801108u, 127u);
        }
        memset(&g_psx_cpu_probe, 0, sizeof(g_psx_cpu_probe));
        for (unsigned i = 0; i < 2; ++i) {
            psx_set_cpu_overclock(clocks[c]); /* fractional carry starts identically */
            psx_work_diag_begin(enabled ? fake_clock : NULL);
            for (unsigned step = 0; step < 40000; ++step) {
                if (i == 1 && step % 31 == 0) psx_update_profiled(machines[i]);
                else psx_update(machines[i]);
            }
        }
        assert(compare_pair("profiled-update", cases, plain, sampled));
        void *a = NULL, *b = NULL;
        size_t a_capacity = 0, b_capacity = 0, a_size = 0, b_size = 0;
        assert(psx_save_state_to_memory_ex(plain, &a, &a_capacity, &a_size,
            PSX_STATE_SAVE_NO_THUMBNAIL) == PSX_STATE_OK);
        assert(psx_save_state_to_memory_ex(sampled, &b, &b_capacity, &b_size,
            PSX_STATE_SAVE_NO_THUMBNAIL) == PSX_STATE_OK);
        if (a_size != b_size || memcmp(a, b, a_size) != 0) {
            FILE* fa = fopen("plain.state", "wb"), *fb = fopen("sampled.state", "wb");
            assert(fa && fb);
            fwrite(a, 1, a_size, fa); fwrite(b, 1, b_size, fb);
            fclose(fa); fclose(fb);
            fprintf(stderr, "state mismatch mode=%u base=%u clock=%u enabled=%u sizes=%zu/%zu\n",
                mode, base, clocks[c], enabled, a_size, b_size);
            assert(0);
        }
        if (enabled) {
            assert(g_psx_cpu_probe.samples == 1291);
            assert(g_psx_work_diag.calls[PSX_WORK_CPU_SAMPLE] == 1291);
            for (unsigned device = PSX_WORK_CDROM_UPDATE; device <= PSX_WORK_SPU_TICK; ++device)
                assert(g_psx_work_diag.calls[device] == 1291);
            assert(g_psx_cpu_probe.regions[0] == 1291);
        } else assert(g_psx_cpu_probe.samples == 0);
        free(a); free(b);
        psx_destroy(plain); psx_destroy(sampled);
        ++cases;
    }
    psx_work_diag_begin(NULL);
    psx_set_cpu_overclock(100);
    printf("PROFILED_UPDATE_PARITY %u cases passed: both CPU engines, base cycles, fractional clocks, full guest states\n", cases);
    return 0;
}
