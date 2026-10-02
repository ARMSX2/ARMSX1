/* Host timing policy must stay local to each CPU and survive reset/state load. */
#define main cpu_differential_main
#include "cpu_differential.c"
#undef main
#include <assert.h>

static unsigned step(psx_t* p, unsigned mode, unsigned base, uint32_t instruction) {
    psx_cpu_init(p->cpu, p->bus);
    psx_cpu_set_execution_mode(p->cpu, mode);
    psx_cpu_set_base_instruction_cycles(p->cpu, base);
    psx_bus_write32(p->bus, TEST_OFFSET, instruction);
    p->cpu->pc = TEST_PC;
    p->cpu->next_pc = TEST_PC + 4;
    psx_cpu_cycle(p->cpu);
    return p->cpu->last_cycles;
}

int main(void) {
    const char* bios = "build/tests/blank-bios.bin";
    assert(write_blank_bios(bios));
    psx_t* p = psx_create();
    assert(p && psx_init(p, bios, NULL) == 0);
    assert(p->cpu->base_instruction_cycles == 1);
    for (unsigned mode = PSX_CPU_INTERPRETER; mode <= PSX_CPU_CACHED_INTERPRETER; ++mode) {
        unsigned standard = step(p, mode, 1, 0); /* NOP */
        unsigned compat = step(p, mode, 2, 0);
        assert(compat == standard + 1);
        unsigned gte_standard = step(p, mode, 1, 0x4a000006); /* NCLIP, explicit latency */
        unsigned gte_compat = step(p, mode, 2, 0x4a000006);
        assert(gte_compat == gte_standard); /* explicit GTE timing is unchanged */
        assert(gte_standard > standard);
        psx_cpu_init(p->cpu, p->bus);
        assert(p->cpu->base_instruction_cycles == 2);
    }
    void* data = NULL;
    size_t capacity = 0, size = 0;
    assert(psx_save_state_to_memory_ex(p, &data, &capacity, &size,
        PSX_STATE_SAVE_NO_THUMBNAIL) == 0);
    psx_cpu_set_base_instruction_cycles(p->cpu, 1);
    assert(psx_load_state_from_memory(p, data, size) == 0);
    assert(p->cpu->base_instruction_cycles == 1); /* state's policy must not leak */
    psx_cpu_set_base_instruction_cycles(p->cpu, 2);
    assert(psx_load_state_from_memory(p, data, size) == 0);
    assert(p->cpu->base_instruction_cycles == 2);
    psx_cpu_set_base_instruction_cycles(p->cpu, 0);
    assert(p->cpu->base_instruction_cycles == 1);
    free(data);
    psx_destroy(p);
    puts("CPU_BASE_CYCLE_POLICY all checks passed");
    return 0;
}
