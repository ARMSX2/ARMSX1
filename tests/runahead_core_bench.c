#include "../psx/psx.h"
#include "../psx/state.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* Exercise instruction fetch, decode, RAM reads/writes and device updates.
   Compare the final serialized-state hash across compiler configurations. */
int main(int argc, char **argv) {
    FILE *f = fopen("bench-bios.bin", "wb");
    assert(f);
    unsigned char zero[4096] = {0};
    for (int i = 0; i < 128; ++i) assert(fwrite(zero, 1, sizeof(zero), f) == sizeof(zero));
    fclose(f);
    psx_t *p = psx_create();
    assert(p && psx_init(p, "bench-bios.bin", NULL) == 0);
    if (argc > 1 && argv[1][0] == 'p') p->gpu->display_mode |= 8;
    if (argc > 2 && argv[2][0] == 'i') psx_cpu_set_execution_mode(p->cpu, PSX_CPU_INTERPRETER);
    const uint32_t program[] = {
        0x24022000, /* addiu r2,zero,0x2000 */
        0x24210001, /* addiu r1,r1,1 */
        0xac410000, /* sw r1,0(r2) */
        0x8c430000, /* lw r3,0(r2) */
        0x08000401, /* j 0x1004 */
        0x00000000  /* nop delay slot */
    };
    for (unsigned i = 0; i < sizeof(program)/sizeof(program[0]); ++i)
        psx_bus_write32(p->bus, 0x1000 + i*4, program[i]);
    p->cpu->pc = 0x80001000;
    p->cpu->next_pc = 0x80001004;
    p->cpu->cop0_r[COP0_SR] = 0;
    clock_t start = clock();
    for (int i = 0; i < 5000000; ++i) psx_update(p);
    double ms = 1000.0 * (clock() - start) / CLOCKS_PER_SEC;
    assert(p->cpu->r[1] == 1000000);
    assert(psx_bus_read32(p->bus, 0x2000) == 1000000);
    void *data = NULL; size_t cap = 0, size = 0;
    assert(psx_save_state_to_memory_ex(p, &data, &cap, &size, PSX_STATE_SAVE_NO_THUMBNAIL) == 0);
    uint64_t hash = 14695981039346656037ull;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ ((unsigned char*)data)[i]) * 1099511628211ull;
    printf("5M instruction/device updates: %.3f ms; state=%016llx; bytes=%zu\n", ms, (unsigned long long)hash, size);
    free(data); psx_destroy(p); remove("bench-bios.bin");
    return 0;
}
