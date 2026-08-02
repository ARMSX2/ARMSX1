#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx/psx.h"

#define TEST_PC 0x80001000u
#define TEST_OFFSET 0x1000u

static int write_blank_bios(const char* path) {
    FILE* file = fopen(path, "wb");
    if (!file)
        return 0;

    uint8_t block[4096] = {0};
    for (size_t offset = 0; offset < 512u * 1024u; offset += sizeof(block)) {
        if (fwrite(block, 1, sizeof(block), file) != sizeof(block)) {
            fclose(file);
            return 0;
        }
    }

    return fclose(file) == 0;
}

static int init_pair(psx_t** reference_out, psx_t** cached_out, const char* bios_path) {
    psx_t* reference = psx_create();
    psx_t* cached = psx_create();

    if (!reference || !cached ||
        psx_init(reference, bios_path, NULL) != 0 ||
        psx_init(cached, bios_path, NULL) != 0) {
        return 0;
    }

    psx_cpu_set_execution_mode(reference->cpu, PSX_CPU_INTERPRETER);
    psx_cpu_set_execution_mode(cached->cpu, PSX_CPU_CACHED_INTERPRETER);
    reference->cpu->pc = cached->cpu->pc = TEST_PC;
    reference->cpu->next_pc = cached->cpu->next_pc = TEST_PC + 4;
    *reference_out = reference;
    *cached_out = cached;
    return 1;
}

static int compare_pair(const char* name, unsigned step, psx_t* reference, psx_t* cached) {
    const size_t cpu_state_size = offsetof(psx_cpu_t, bus);

    if (memcmp(reference->cpu, cached->cpu, cpu_state_size) != 0) {
        fprintf(
            stderr,
            "CPU_DIFFERENTIAL failed case=%s step=%u reason=cpu-state "
            "reference_pc=%08x cached_pc=%08x reference_opcode=%08x cached_opcode=%08x\n",
            name,
            step,
            reference->cpu->pc,
            cached->cpu->pc,
            reference->cpu->opcode,
            cached->cpu->opcode
        );
        return 0;
    }

    if (reference->ram->size != cached->ram->size ||
        memcmp(reference->ram->buf, cached->ram->buf, reference->ram->size) != 0) {
        fprintf(stderr, "CPU_DIFFERENTIAL failed case=%s step=%u reason=ram-state\n", name, step);
        return 0;
    }

    return 1;
}

static void write_program(psx_t* psx, const uint32_t* words, size_t count) {
    for (size_t index = 0; index < count; ++index)
        psx_bus_write32(psx->bus, TEST_OFFSET + (uint32_t)(index * 4u), words[index]);
}

static int run_steps(const char* name, psx_t* reference, psx_t* cached, unsigned steps) {
    printf("CPU_DIFFERENTIAL begin case=%s steps=%u\n", name, steps);
    for (unsigned step = 0; step < steps; ++step) {
        psx_cpu_cycle(reference->cpu);
        psx_cpu_cycle(cached->cpu);
        if (!compare_pair(name, step, reference, cached))
            return 0;
    }
    printf("CPU_DIFFERENTIAL passed case=%s\n", name);
    return 1;
}

static int case_integer_memory_branch(const char* bios_path) {
    static const uint32_t program[] = {
        0x3c018000u, /* lui r1, 0x8000 */
        0x24020005u, /* addiu r2, r0, 5 */
        0xac220100u, /* sw r2, 0x100(r1) */
        0x8c230100u, /* lw r3, 0x100(r1) */
        0x00000000u, /* load delay */
        0x24640007u, /* addiu r4, r3, 7 */
        0x10840002u, /* beq r4, r4, +2 */
        0x24050009u, /* delay slot */
        0x2406deadu, /* skipped */
        0x00850018u, /* mult r4, r5 */
        0x00003812u, /* mflo r7 */
        0x0800040cu, /* j TEST_PC+0x30 */
        0x00000000u, /* delay slot */
    };
    psx_t* reference = NULL;
    psx_t* cached = NULL;
    int ok = init_pair(&reference, &cached, bios_path);
    if (!ok)
        return 0;
    write_program(reference, program, sizeof(program) / sizeof(program[0]));
    write_program(cached, program, sizeof(program) / sizeof(program[0]));
    ok = run_steps("integer-memory-branch", reference, cached, 80);
    psx_destroy(reference);
    psx_destroy(cached);
    return ok;
}

static int case_self_modifying_alias(const char* bios_path) {
    static const uint32_t first = 0x24020001u;  /* addiu r2, r0, 1 */
    static const uint32_t second = 0x24020002u; /* addiu r2, r0, 2 */
    psx_t* reference = NULL;
    psx_t* cached = NULL;
    int ok = init_pair(&reference, &cached, bios_path);
    if (!ok)
        return 0;

    psx_bus_write32(reference->bus, TEST_OFFSET, first);
    psx_bus_write32(cached->bus, TEST_OFFSET, first);
    ok = run_steps("self-modifying-warmup", reference, cached, 1);

    reference->cpu->pc = cached->cpu->pc = TEST_PC;
    reference->cpu->next_pc = cached->cpu->next_pc = TEST_PC + 4;
    psx_bus_write32(reference->bus, TEST_OFFSET, second);
    psx_bus_write32(cached->bus, TEST_OFFSET, second);
    if (ok)
        ok = run_steps("self-modifying-alias", reference, cached, 1);
    if (ok && cached->cpu->r[2] != 2u) {
        fprintf(stderr, "CPU_DIFFERENTIAL failed case=self-modifying-alias reason=stale-opcode\n");
        ok = 0;
    }

    psx_destroy(reference);
    psx_destroy(cached);
    return ok;
}

static int case_irq_and_exception(const char* bios_path) {
    static const uint32_t program[] = {
        0x24010001u, /* addiu r1, r0, 1 */
        0x00210820u, /* add r1, r1, r1 */
        0x0000000cu, /* syscall */
        0x00000000u,
    };
    psx_t* reference = NULL;
    psx_t* cached = NULL;
    int ok = init_pair(&reference, &cached, bios_path);
    if (!ok)
        return 0;
    write_program(reference, program, sizeof(program) / sizeof(program[0]));
    write_program(cached, program, sizeof(program) / sizeof(program[0]));
    reference->cpu->cop0_r[COP0_SR] |= SR_IEC | SR_IM2;
    cached->cpu->cop0_r[COP0_SR] |= SR_IEC | SR_IM2;
    psx_cpu_set_irq_pending(reference->cpu);
    psx_cpu_set_irq_pending(cached->cpu);
    ok = run_steps("irq-exception", reference, cached, 8);
    psx_destroy(reference);
    psx_destroy(cached);
    return ok;
}

static int case_opcode_matrix(const char* bios_path) {
    static const uint8_t special_functions[] = {
        0x00, 0x02, 0x03, 0x04, 0x06, 0x07, 0x08, 0x09,
        0x0c, 0x0d, 0x10, 0x11, 0x12, 0x13, 0x18, 0x19,
        0x1a, 0x1b, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
        0x26, 0x27, 0x2a, 0x2b,
    };
    static const uint8_t regimm_types[] = {0x00, 0x01, 0x10, 0x11, 0x02, 0x03};
    static const uint8_t primary_operations[] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
        0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x28, 0x29, 0x2a, 0x2b, 0x2e,
        0x30, 0x31, 0x32, 0x33, 0x38, 0x39, 0x3a, 0x3b,
    };
    static const uint8_t gte_functions[] = {
        0x01, 0x06, 0x0c, 0x10, 0x11, 0x12, 0x13,
        0x14, 0x16, 0x1b, 0x1c, 0x1e, 0x20, 0x28,
        0x29, 0x2a, 0x2d, 0x2e, 0x30, 0x3d, 0x3e, 0x3f,
    };
    uint32_t opcodes[128];
    size_t opcode_count = 0;

    for (size_t i = 0; i < sizeof(special_functions); ++i) {
        opcodes[opcode_count++] =
            (1u << 21) | (2u << 16) | (3u << 11) | (4u << 6) | special_functions[i];
    }
    for (size_t i = 0; i < sizeof(regimm_types); ++i) {
        opcodes[opcode_count++] =
            (0x01u << 26) | (1u << 21) | ((uint32_t)regimm_types[i] << 16) | 1u;
    }
    for (size_t i = 0; i < sizeof(primary_operations); ++i) {
        opcodes[opcode_count++] =
            ((uint32_t)primary_operations[i] << 26) | (1u << 21) | (2u << 16) | 1u;
    }

    opcodes[opcode_count++] = (0x10u << 26) | (0x00u << 21) | (2u << 16) | (12u << 11);
    opcodes[opcode_count++] = (0x10u << 26) | (0x04u << 21) | (2u << 16) | (12u << 11);
    opcodes[opcode_count++] = (0x10u << 26) | (0x10u << 21) | 0x10u;
    opcodes[opcode_count++] = (0x12u << 26) | (0x00u << 21) | (2u << 16) | (3u << 11);
    opcodes[opcode_count++] = (0x12u << 26) | (0x02u << 21) | (2u << 16) | (3u << 11);
    opcodes[opcode_count++] = (0x12u << 26) | (0x04u << 21) | (2u << 16) | (3u << 11);
    opcodes[opcode_count++] = (0x12u << 26) | (0x06u << 21) | (2u << 16) | (3u << 11);
    for (size_t i = 0; i < sizeof(gte_functions); ++i) {
        opcodes[opcode_count++] =
            (0x12u << 26) | (0x10u << 21) | gte_functions[i];
    }
    opcodes[opcode_count++] = (0x12u << 26) | (0x10u << 21) | 0x00u;

    psx_t* reference = NULL;
    psx_t* cached = NULL;
    int ok = init_pair(&reference, &cached, bios_path);
    if (!ok)
        return 0;

    printf("CPU_DIFFERENTIAL begin case=opcode-matrix opcodes=%zu\n", opcode_count);
    for (size_t index = 0; index < opcode_count; ++index) {
        psx_cpu_init(reference->cpu, reference->bus);
        psx_cpu_init(cached->cpu, cached->bus);
        psx_cpu_set_execution_mode(reference->cpu, PSX_CPU_INTERPRETER);
        psx_cpu_set_execution_mode(cached->cpu, PSX_CPU_CACHED_INTERPRETER);
        reference->cpu->pc = cached->cpu->pc = TEST_PC;
        reference->cpu->next_pc = cached->cpu->next_pc = TEST_PC + 4;
        reference->cpu->r[1] = cached->cpu->r[1] = 0x80002000u;
        reference->cpu->r[2] = cached->cpu->r[2] = 0x12345678u;
        reference->cpu->r[3] = cached->cpu->r[3] = 3u;
        reference->cpu->hi = cached->cpu->hi = 0x11111111u;
        reference->cpu->lo = cached->cpu->lo = 0x22222222u;
        reference->cpu->cop2_cr.h = cached->cpu->cop2_cr.h = 1u;
        psx_bus_write32(reference->bus, 0x2000u, 0x89abcdefu);
        psx_bus_write32(cached->bus, 0x2000u, 0x89abcdefu);
        psx_bus_write32(reference->bus, TEST_OFFSET, opcodes[index]);
        psx_bus_write32(cached->bus, TEST_OFFSET, opcodes[index]);

        psx_cpu_cycle(reference->cpu);
        psx_cpu_cycle(cached->cpu);
        if (!compare_pair("opcode-matrix", (unsigned)index, reference, cached)) {
            fprintf(
                stderr,
                "CPU_DIFFERENTIAL opcode-matrix detail index=%zu opcode=%08x\n",
                index,
                opcodes[index]
            );
            ok = 0;
            break;
        }
    }
    if (ok)
        printf("CPU_DIFFERENTIAL passed case=opcode-matrix opcodes=%zu\n", opcode_count);

    psx_destroy(reference);
    psx_destroy(cached);
    return ok;
}

int main(void) {
    const char* bios_path = "build/tests/blank-bios.bin";
    if (!write_blank_bios(bios_path)) {
        fprintf(stderr, "CPU_DIFFERENTIAL failed reason=create-bios path=%s\n", bios_path);
        return 1;
    }

    if (!case_integer_memory_branch(bios_path) ||
        !case_self_modifying_alias(bios_path) ||
        !case_irq_and_exception(bios_path) ||
        !case_opcode_matrix(bios_path)) {
        return 1;
    }

    printf("CPU_DIFFERENTIAL all cases passed\n");
    return 0;
}
