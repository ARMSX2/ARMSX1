/* ------------------------------------------------------------------------------------
   R3000A against the documented rules, independently recomputed.

   WHY THIS EXISTS

   tests/cpu_differential.c is DIFFERENTIAL: the interpreter against the cached
   interpreter, both this project's own code, both written from one understanding. A rule
   that is wrong in BOTH halves reads green forever. That blindness has already bitten
   this project five times (GPU parity, the blend rule, the shift matrix, GPUINFO, the
   GTE). tests/gte_matrix.c closed the hole for COP2; this file closes it for the integer
   core, in the same style: the psx-spx / R3000A rules are re-implemented INSIDE the test
   and the real emulator is compared against them.

   Every vector runs through BOTH execution modes (interpreter and cached interpreter) and
   both are checked against the reference, so a rule shared by the two dispatch paths is
   still caught, and a divergence between them is caught as well.

   Covered:
     * load-extend        LB/LBU/LH/LHU/LW: exhaustive over all 256 byte values x 4 byte
                          lanes and all 65536 halfword values x 2 lanes; sign vs zero
                          extension recomputed from the spec
     * store-width        SB/SH/SW: byte lanes and halfword lanes, surrounding bytes must
                          be untouched
     * addr-error         ADEL/ADES: every misaligned LH/LHU/LW/SH/SW alignment; EPC,
                          CAUSE.ExcCode, the destination register and memory must all be
                          left as the spec says
     * unaligned          LWL/LWR/SWL/SWR: exhaustive over 4 alignments x a pattern set x
                          prior-rt set, against the switch-form spec, plus the canonical
                          LWL+LWR / SWL+SWR unaligned-access idioms
     * load-delay         the R3000 load delay slot: for every consumer class (ALU, store
                          rt, store base, branch, shift, mult, MTC0/MTC2, second load) the
                          instruction after a load must observe the OLD rt
     * shift-matrix       SLL/SRL/SRA/SLLV/SRLV/SRAV over all 32 amounts (variable forms
                          swept to 63 to prove the & 31) x a boundary operand set
     * alu-matrix         ADD/ADDU/SUB/SUBU/AND/OR/XOR/NOR/SLT/SLTU and the immediate
                          forms over a boundary lattice; ADDI/ADD/SUB overflow must raise
                          and must NOT write the destination
     * muldiv             MULT/MULTU/DIV/DIVU over boundary pairs + a pseudo-random sweep;
                          div-by-zero (both signs) and INT_MIN / -1 from the spec
     * branch-matrix      BEQ/BNE/BLEZ/BGTZ/BLTZ/BGEZ/BLTZAL/BGEZAL/J/JAL/JR/JALR: taken
                          and not-taken targets, the delay slot, the link value, the
                          "link happens even when not taken" rule and rs==31/rd==rs
     * exception-model    SYSCALL/BREAK/overflow/address-error inside and outside a branch
                          delay slot: EPC, CAUSE.BD, the SR mode-stack push, both vectors
                          (BEV) and the RFE pop
     * cop0-masks         MTC0/MFC0 per-register write masks from the documented table
     * cache-isolation    with SR.IsC set, NO store may reach memory (SB/SH/SW/SWL/SWR)

   Reference convention: "spec" means the psx-spx CPU chapter / the R3000A manual as
   cross-confirmed by hardware-verified emulators. Where a rule is stated in the code
   below it is written out from that source, never copied from psx/cpu.c.

   MUTATION PROTOCOL (a gate that cannot fail is not a gate): temporarily perturb, one at
   a time, in psx/cpu.c: (1) one LWL/LWR mask, (2) the SE8/SE16 sign extension of one
   load, (3) the div-by-zero LO value, (4) the BD bit in psx_cpu_exception, (5) one COP0
   write mask, (6) the & 31 on a variable shift. `make test-cpu-spec` must go red for each.
   Revert byte-identically afterwards.
   ------------------------------------------------------------------------------------ */

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psx/psx.h"

#define TEST_PC       0x80001000u
#define TEST_OFFSET   0x00001000u
#define DATA_VA       0x80002000u
#define DATA_PA       0x00002000u

#define EXC_VEC_NORMAL 0x80000080u
#define EXC_VEC_BEV    0xbfc00180u

static unsigned long g_checked;
static int g_failures;

static void fail(const char* fmt, ...);

#include <stdarg.h>

static void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "CPU_SPEC failed ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    ++g_failures;
}

/* ==================================================================================
   Instruction encoders (field layout only -- no semantics).
   ================================================================================== */

static uint32_t enc_i(unsigned op, unsigned rs, unsigned rt, uint16_t imm) {
    return ((op & 0x3fu) << 26) | ((rs & 0x1fu) << 21) | ((rt & 0x1fu) << 16) | imm;
}

static uint32_t enc_r(unsigned rs, unsigned rt, unsigned rd, unsigned sa, unsigned funct) {
    return ((rs & 0x1fu) << 21) | ((rt & 0x1fu) << 16) | ((rd & 0x1fu) << 11) |
           ((sa & 0x1fu) << 6) | (funct & 0x3fu);
}

static uint32_t enc_j(unsigned op, uint32_t target26) {
    return ((op & 0x3fu) << 26) | (target26 & 0x3ffffffu);
}

#define OP_SPECIAL 0x00u
#define OP_BCOND   0x01u
#define OP_J       0x02u
#define OP_JAL     0x03u
#define OP_BEQ     0x04u
#define OP_BNE     0x05u
#define OP_BLEZ    0x06u
#define OP_BGTZ    0x07u
#define OP_ADDI    0x08u
#define OP_ADDIU   0x09u
#define OP_SLTI    0x0au
#define OP_SLTIU   0x0bu
#define OP_ANDI    0x0cu
#define OP_ORI     0x0du
#define OP_XORI    0x0eu
#define OP_LUI     0x0fu
#define OP_COP0    0x10u
#define OP_LB      0x20u
#define OP_LH      0x21u
#define OP_LWL     0x22u
#define OP_LW      0x23u
#define OP_LBU     0x24u
#define OP_LHU     0x25u
#define OP_LWR     0x26u
#define OP_SB      0x28u
#define OP_SH      0x29u
#define OP_SWL     0x2au
#define OP_SW      0x2bu
#define OP_SWR     0x2eu

#define F_SLL   0x00u
#define F_SRL   0x02u
#define F_SRA   0x03u
#define F_SLLV  0x04u
#define F_SRLV  0x06u
#define F_SRAV  0x07u
#define F_JR    0x08u
#define F_JALR  0x09u
#define F_SYSCALL 0x0cu
#define F_BREAK 0x0du
#define F_MFHI  0x10u
#define F_MTHI  0x11u
#define F_MFLO  0x12u
#define F_MTLO  0x13u
#define F_MULT  0x18u
#define F_MULTU 0x19u
#define F_DIV   0x1au
#define F_DIVU  0x1bu
#define F_ADD   0x20u
#define F_ADDU  0x21u
#define F_SUB   0x22u
#define F_SUBU  0x23u
#define F_AND   0x24u
#define F_OR    0x25u
#define F_XOR   0x26u
#define F_NOR   0x27u
#define F_SLT   0x2au
#define F_SLTU  0x2bu

#define NOP 0x00000000u

/* ==================================================================================
   Machine plumbing. Two machines, one per execution mode; every vector runs on both.
   ================================================================================== */

typedef struct {
    psx_t* m[2];  /* [0] = interpreter, [1] = cached interpreter */
} pair_t;

static const char* const MODE_NAME[2] = { "interp", "cached" };

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

static int pair_init(pair_t* p, const char* bios) {
    for (int i = 0; i < 2; i++) {
        p->m[i] = psx_create();
        if (!p->m[i] || psx_init(p->m[i], bios, NULL) != 0)
            return 0;
        psx_cpu_set_execution_mode(p->m[i]->cpu,
            i == 0 ? PSX_CPU_INTERPRETER : PSX_CPU_CACHED_INTERPRETER);
    }
    return 1;
}

static void pair_destroy(pair_t* p) {
    for (int i = 0; i < 2; i++)
        if (p->m[i])
            psx_destroy(p->m[i]);
}

/* Light reset: architectural state only. Deliberately does NOT touch the decode cache --
   the program writer below goes through psx_bus_write32, which is the same path the
   emulator's own write observer watches, so a stale-cache bug stays visible. */
static void cpu_reset(psx_cpu_t* c) {
    memset(c->r, 0, sizeof(c->r));
    c->hi = 0;
    c->lo = 0;
    c->pc = TEST_PC;
    c->next_pc = TEST_PC + 4u;
    c->saved_pc = 0;
    c->opcode = 0;
    c->load_d = 0;
    c->load_v = 0xffffffffu;
    c->branch = 0;
    c->delay_slot = 0;
    c->branch_taken = 0;
    memset(c->cop0_r, 0, sizeof(c->cop0_r));
    c->cop0_r[COP0_SR] = 0x10000000u;  /* CU0 only: IEc clear (no IRQs), BEV clear */
    c->cop0_r[COP0_PRID] = 0x00000002u;
    c->last_cycles = 0;
    c->total_cycles = 0;
}

static void write_prog(psx_t* psx, const uint32_t* words, size_t n) {
    for (size_t i = 0; i < n; i++)
        psx_bus_write32(psx->bus, TEST_OFFSET + (uint32_t)(i * 4u), words[i]);
}

static void write_data(psx_t* psx, uint32_t off, uint32_t value) {
    psx_bus_write32(psx->bus, DATA_PA + off, value);
}

static uint32_t read_data(psx_t* psx, uint32_t off) {
    return psx_bus_read32(psx->bus, DATA_PA + off);
}

/* ==================================================================================
   Reference model -- written from the spec, never from psx/cpu.c.
   ================================================================================== */

static uint32_t ref_lwl(uint32_t rt, uint32_t mem, unsigned n) {
    switch (n & 3u) {
        case 0: return (rt & 0x00ffffffu) | (mem << 24);
        case 1: return (rt & 0x0000ffffu) | (mem << 16);
        case 2: return (rt & 0x000000ffu) | (mem << 8);
        default: return mem;
    }
}

static uint32_t ref_lwr(uint32_t rt, uint32_t mem, unsigned n) {
    switch (n & 3u) {
        case 0: return mem;
        case 1: return (rt & 0xff000000u) | (mem >> 8);
        case 2: return (rt & 0xffff0000u) | (mem >> 16);
        default: return (rt & 0xffffff00u) | (mem >> 24);
    }
}

static uint32_t ref_swl(uint32_t mem, uint32_t rt, unsigned n) {
    switch (n & 3u) {
        case 0: return (mem & 0xffffff00u) | (rt >> 24);
        case 1: return (mem & 0xffff0000u) | (rt >> 16);
        case 2: return (mem & 0xff000000u) | (rt >> 8);
        default: return rt;
    }
}

static uint32_t ref_swr(uint32_t mem, uint32_t rt, unsigned n) {
    switch (n & 3u) {
        case 0: return rt;
        case 1: return (mem & 0x000000ffu) | (rt << 8);
        case 2: return (mem & 0x0000ffffu) | (rt << 16);
        default: return (mem & 0x00ffffffu) | (rt << 24);
    }
}

static uint32_t ref_shift(unsigned funct, uint32_t value, unsigned amount) {
    unsigned a = amount & 31u;
    switch (funct) {
        case F_SLL: case F_SLLV: return (uint32_t)(value << a);
        case F_SRL: case F_SRLV: return (uint32_t)(value >> a);
        case F_SRA: case F_SRAV: {
            /* well-defined arithmetic shift right */
            if (value & 0x80000000u)
                return (uint32_t)(~((~value) >> a));
            return value >> a;
        }
        default: return 0;
    }
}

/* ==================================================================================
   Case: load sign/zero extension, exhaustive.
   ================================================================================== */

static int case_load_extend(pair_t* p) {
    printf("CPU_SPEC begin case=load-extend\n");
    int ok = 1;

    /* LB / LBU over every byte value in every lane. */
    for (unsigned lane = 0; lane < 4u && ok; lane++) {
        for (unsigned bv = 0; bv < 256u && ok; bv++) {
            uint32_t word = 0xa5a5a5a5u;
            ((uint8_t*)&word)[lane] = (uint8_t)bv;  /* host is LE; PSX RAM is LE */

            for (int signed_load = 0; signed_load < 2 && ok; signed_load++) {
                uint32_t op = enc_i(signed_load ? OP_LB : OP_LBU, 1, 2, (uint16_t)lane);
                uint32_t prog[3] = { op, NOP, NOP };
                uint32_t want = signed_load ? (uint32_t)(int32_t)(int8_t)bv : bv;

                for (int mi = 0; mi < 2 && ok; mi++) {
                    psx_t* m = p->m[mi];
                    cpu_reset(m->cpu);
                    write_prog(m, prog, 3);
                    write_data(m, 0, word);
                    m->cpu->r[1] = DATA_VA;
                    m->cpu->r[2] = 0xdeadbeefu;
                    psx_cpu_cycle(m->cpu);   /* the load */
                    psx_cpu_cycle(m->cpu);   /* nop -- commits the pending load */
                    ++g_checked;
                    if (m->cpu->r[2] != want) {
                        fail("case=load-extend mode=%s %s lane=%u byte=%02x want=%08x got=%08x",
                             MODE_NAME[mi], signed_load ? "lb" : "lbu", lane, bv,
                             want, m->cpu->r[2]);
                        ok = 0;
                    }
                }
            }
        }
    }

    /* LH / LHU over every halfword value in both lanes. */
    for (unsigned lane = 0; lane < 2u && ok; lane++) {
        for (unsigned hv = 0; hv < 65536u && ok; hv++) {
            uint32_t word = 0xa5a5a5a5u;
            ((uint16_t*)&word)[lane] = (uint16_t)hv;

            for (int signed_load = 0; signed_load < 2 && ok; signed_load++) {
                uint32_t op = enc_i(signed_load ? OP_LH : OP_LHU, 1, 2, (uint16_t)(lane * 2u));
                uint32_t prog[3] = { op, NOP, NOP };
                uint32_t want = signed_load ? (uint32_t)(int32_t)(int16_t)hv : hv;

                for (int mi = 0; mi < 2 && ok; mi++) {
                    psx_t* m = p->m[mi];
                    cpu_reset(m->cpu);
                    write_prog(m, prog, 3);
                    write_data(m, 0, word);
                    m->cpu->r[1] = DATA_VA;
                    m->cpu->r[2] = 0xdeadbeefu;
                    psx_cpu_cycle(m->cpu);
                    psx_cpu_cycle(m->cpu);
                    ++g_checked;
                    if (m->cpu->r[2] != want) {
                        fail("case=load-extend mode=%s %s lane=%u half=%04x want=%08x got=%08x",
                             MODE_NAME[mi], signed_load ? "lh" : "lhu", lane, hv,
                             want, m->cpu->r[2]);
                        ok = 0;
                    }
                }
            }
        }
    }

    /* Negative offsets must sign-extend the 16-bit immediate. */
    {
        uint32_t op = enc_i(OP_LW, 1, 2, 0xfffcu); /* -4 */
        uint32_t prog[3] = { op, NOP, NOP };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 3);
            write_data(m, 0, 0x11223344u);
            m->cpu->r[1] = DATA_VA + 4u;
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[2] != 0x11223344u) {
                fail("case=load-extend mode=%s lw-negative-offset want=11223344 got=%08x",
                     MODE_NAME[mi], m->cpu->r[2]);
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=load-extend\n");
    return ok;
}

/* ==================================================================================
   Case: store widths -- the bytes around the target must not be disturbed.
   ================================================================================== */

static int case_store_width(pair_t* p) {
    printf("CPU_SPEC begin case=store-width\n");
    int ok = 1;
    const uint32_t seed = 0xa5a5a5a5u;

    for (unsigned lane = 0; lane < 4u && ok; lane++) {
        for (unsigned bv = 0; bv < 256u && ok; bv++) {
            uint32_t want = seed;
            ((uint8_t*)&want)[lane] = (uint8_t)bv;
            uint32_t prog[2] = { enc_i(OP_SB, 1, 2, (uint16_t)lane), NOP };

            for (int mi = 0; mi < 2 && ok; mi++) {
                psx_t* m = p->m[mi];
                cpu_reset(m->cpu);
                write_prog(m, prog, 2);
                write_data(m, 0, seed);
                m->cpu->r[1] = DATA_VA;
                m->cpu->r[2] = 0x11223300u | bv;  /* high bytes must be discarded */
                psx_cpu_cycle(m->cpu);
                ++g_checked;
                if (read_data(m, 0) != want) {
                    fail("case=store-width mode=%s sb lane=%u byte=%02x want=%08x got=%08x",
                         MODE_NAME[mi], lane, bv, want, read_data(m, 0));
                    ok = 0;
                }
            }
        }
    }

    for (unsigned lane = 0; lane < 2u && ok; lane++) {
        for (unsigned hv = 0; hv < 65536u; hv += 7u) {
            uint32_t want = seed;
            ((uint16_t*)&want)[lane] = (uint16_t)hv;
            uint32_t prog[2] = { enc_i(OP_SH, 1, 2, (uint16_t)(lane * 2u)), NOP };

            for (int mi = 0; mi < 2 && ok; mi++) {
                psx_t* m = p->m[mi];
                cpu_reset(m->cpu);
                write_prog(m, prog, 2);
                write_data(m, 0, seed);
                m->cpu->r[1] = DATA_VA;
                m->cpu->r[2] = 0xbeef0000u | hv;
                psx_cpu_cycle(m->cpu);
                ++g_checked;
                if (read_data(m, 0) != want) {
                    fail("case=store-width mode=%s sh lane=%u half=%04x want=%08x got=%08x",
                         MODE_NAME[mi], lane, hv, want, read_data(m, 0));
                    ok = 0;
                    break;
                }
            }
            if (!ok) break;
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=store-width\n");
    return ok;
}

/* ==================================================================================
   Case: address errors. Misaligned LH/LHU/LW -> ADEL, SH/SW -> ADES. The destination
   register must be untouched, memory must be untouched, EPC must point at the faulting
   instruction and CAUSE.ExcCode must carry the right code.
   ================================================================================== */

static int case_addr_error(pair_t* p) {
    printf("CPU_SPEC begin case=addr-error\n");
    int ok = 1;

    struct { unsigned op; unsigned align_mask; uint32_t excode; const char* name; int is_store; } t[] = {
        { OP_LH,  1u, CAUSE_ADEL, "lh",  0 },
        { OP_LHU, 1u, CAUSE_ADEL, "lhu", 0 },
        { OP_LW,  3u, CAUSE_ADEL, "lw",  0 },
        { OP_SH,  1u, CAUSE_ADES, "sh",  1 },
        { OP_SW,  3u, CAUSE_ADES, "sw",  1 },
    };

    for (size_t i = 0; i < sizeof t / sizeof t[0] && ok; i++) {
        for (unsigned off = 0; off < 4u && ok; off++) {
            int misaligned = (off & t[i].align_mask) != 0u;
            uint32_t prog[2] = { enc_i(t[i].op, 1, 2, (uint16_t)off), NOP };

            for (int mi = 0; mi < 2 && ok; mi++) {
                psx_t* m = p->m[mi];
                cpu_reset(m->cpu);
                write_prog(m, prog, 2);
                write_data(m, 0, 0x11223344u);
                write_data(m, 4, 0x55667788u);
                m->cpu->r[1] = DATA_VA;
                m->cpu->r[2] = 0xcafef00du;
                psx_cpu_cycle(m->cpu);
                ++g_checked;

                if (!misaligned)
                    continue;

                if (m->cpu->cop0_r[COP0_EPC] != TEST_PC) {
                    fail("case=addr-error mode=%s %s off=%u epc want=%08x got=%08x",
                         MODE_NAME[mi], t[i].name, off, TEST_PC, m->cpu->cop0_r[COP0_EPC]);
                    ok = 0;
                }
                if ((m->cpu->cop0_r[COP0_CAUSE] & 0x7cu) != t[i].excode) {
                    fail("case=addr-error mode=%s %s off=%u excode want=%02x got=%02x",
                         MODE_NAME[mi], t[i].name, off, t[i].excode,
                         m->cpu->cop0_r[COP0_CAUSE] & 0x7cu);
                    ok = 0;
                }
                if (m->cpu->pc != EXC_VEC_NORMAL) {
                    fail("case=addr-error mode=%s %s off=%u vector want=%08x got=%08x",
                         MODE_NAME[mi], t[i].name, off, EXC_VEC_NORMAL, m->cpu->pc);
                    ok = 0;
                }
                if (!t[i].is_store) {
                    /* the load must not have written rt, not even the pending slot */
                    if (m->cpu->load_d == 2u) {
                        fail("case=addr-error mode=%s %s off=%u pending-load-armed",
                             MODE_NAME[mi], t[i].name, off);
                        ok = 0;
                    }
                } else {
                    if (read_data(m, 0) != 0x11223344u || read_data(m, 4) != 0x55667788u) {
                        fail("case=addr-error mode=%s %s off=%u memory-modified %08x %08x",
                             MODE_NAME[mi], t[i].name, off, read_data(m, 0), read_data(m, 4));
                        ok = 0;
                    }
                }
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=addr-error\n");
    return ok;
}

/* ==================================================================================
   Case: LWL/LWR/SWL/SWR, exhaustive over alignment x pattern x prior value.
   ================================================================================== */

static int case_unaligned(pair_t* p) {
    printf("CPU_SPEC begin case=unaligned\n");
    int ok = 1;

    static const uint32_t mem_patterns[] = {
        0x00000000u, 0xffffffffu, 0x01020304u, 0x80000000u, 0x0000ffffu,
        0xdeadbeefu, 0x12345678u, 0xa55aa55au, 0x7f7f7f7fu, 0x000000ffu
    };
    static const uint32_t rt_patterns[] = {
        0x00000000u, 0xffffffffu, 0xcafef00du, 0x80000001u, 0x5a5a5a5au
    };

    for (size_t mp = 0; mp < sizeof mem_patterns / sizeof mem_patterns[0] && ok; mp++) {
        for (size_t rp = 0; rp < sizeof rt_patterns / sizeof rt_patterns[0] && ok; rp++) {
            for (unsigned n = 0; n < 4u && ok; n++) {
                uint32_t mem = mem_patterns[mp];
                uint32_t rtv = rt_patterns[rp];

                /* --- LWL --- */
                {
                    uint32_t prog[3] = { enc_i(OP_LWL, 1, 2, (uint16_t)n), NOP, NOP };
                    uint32_t want = ref_lwl(rtv, mem, n);
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 3);
                        write_data(m, 0, mem);
                        m->cpu->r[1] = DATA_VA;
                        m->cpu->r[2] = rtv;
                        psx_cpu_cycle(m->cpu);
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (m->cpu->r[2] != want) {
                            fail("case=unaligned mode=%s lwl n=%u mem=%08x rt=%08x want=%08x got=%08x",
                                 MODE_NAME[mi], n, mem, rtv, want, m->cpu->r[2]);
                            ok = 0;
                        }
                    }
                }

                /* --- LWR --- */
                {
                    uint32_t prog[3] = { enc_i(OP_LWR, 1, 2, (uint16_t)n), NOP, NOP };
                    uint32_t want = ref_lwr(rtv, mem, n);
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 3);
                        write_data(m, 0, mem);
                        m->cpu->r[1] = DATA_VA;
                        m->cpu->r[2] = rtv;
                        psx_cpu_cycle(m->cpu);
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (m->cpu->r[2] != want) {
                            fail("case=unaligned mode=%s lwr n=%u mem=%08x rt=%08x want=%08x got=%08x",
                                 MODE_NAME[mi], n, mem, rtv, want, m->cpu->r[2]);
                            ok = 0;
                        }
                    }
                }

                /* --- SWL --- */
                {
                    uint32_t prog[2] = { enc_i(OP_SWL, 1, 2, (uint16_t)n), NOP };
                    uint32_t want = ref_swl(mem, rtv, n);
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 2);
                        write_data(m, 0, mem);
                        m->cpu->r[1] = DATA_VA;
                        m->cpu->r[2] = rtv;
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (read_data(m, 0) != want) {
                            fail("case=unaligned mode=%s swl n=%u mem=%08x rt=%08x want=%08x got=%08x",
                                 MODE_NAME[mi], n, mem, rtv, want, read_data(m, 0));
                            ok = 0;
                        }
                    }
                }

                /* --- SWR --- */
                {
                    uint32_t prog[2] = { enc_i(OP_SWR, 1, 2, (uint16_t)n), NOP };
                    uint32_t want = ref_swr(mem, rtv, n);
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 2);
                        write_data(m, 0, mem);
                        m->cpu->r[1] = DATA_VA;
                        m->cpu->r[2] = rtv;
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (read_data(m, 0) != want) {
                            fail("case=unaligned mode=%s swr n=%u mem=%08x rt=%08x want=%08x got=%08x",
                                 MODE_NAME[mi], n, mem, rtv, want, read_data(m, 0));
                            ok = 0;
                        }
                    }
                }
            }
        }
    }

    /* The canonical unaligned-word idiom: LWL hi ; LWR lo, back to back into the SAME
       register with no interlock nop. On hardware the second one sees the first one's
       (still pending) result, which is what makes the idiom work at all. This is the
       single most load-bearing sequence for any engine that walks packed vertex data. */
    for (unsigned base = 0; base < 4u && ok; base++) {
        uint32_t w0 = 0x44332211u, w1 = 0x88776655u;
        uint8_t bytes[8];
        memcpy(bytes + 0, &w0, 4);
        memcpy(bytes + 4, &w1, 4);
        uint32_t want;
        memcpy(&want, bytes + base, 4);   /* the unaligned word the idiom must produce */

        uint32_t prog[4] = {
            enc_i(OP_LWL, 1, 2, (uint16_t)(base + 3u)),
            enc_i(OP_LWR, 1, 2, (uint16_t)base),
            NOP, NOP
        };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 4);
            write_data(m, 0, w0);
            write_data(m, 4, w1);
            m->cpu->r[1] = DATA_VA;
            m->cpu->r[2] = 0xffffffffu;
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[2] != want) {
                fail("case=unaligned mode=%s lwl+lwr idiom base=%u want=%08x got=%08x",
                     MODE_NAME[mi], base, want, m->cpu->r[2]);
                ok = 0;
            }
        }
    }

    /* The store counterpart: SWL hi ; SWR lo must lay an unaligned word into memory. */
    for (unsigned base = 0; base < 4u && ok; base++) {
        uint32_t value = 0xddccbbaau;
        uint8_t expect[8];
        uint32_t seed0 = 0x11111111u, seed1 = 0x22222222u;
        memcpy(expect + 0, &seed0, 4);
        memcpy(expect + 4, &seed1, 4);
        memcpy(expect + base, &value, 4);
        uint32_t want0, want1;
        memcpy(&want0, expect + 0, 4);
        memcpy(&want1, expect + 4, 4);

        uint32_t prog[3] = {
            enc_i(OP_SWL, 1, 2, (uint16_t)(base + 3u)),
            enc_i(OP_SWR, 1, 2, (uint16_t)base),
            NOP
        };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 3);
            write_data(m, 0, seed0);
            write_data(m, 4, seed1);
            m->cpu->r[1] = DATA_VA;
            m->cpu->r[2] = value;
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (read_data(m, 0) != want0 || read_data(m, 4) != want1) {
                fail("case=unaligned mode=%s swl+swr idiom base=%u want=%08x/%08x got=%08x/%08x",
                     MODE_NAME[mi], base, want0, want1, read_data(m, 0), read_data(m, 4));
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=unaligned\n");
    return ok;
}

/* ==================================================================================
   Case: the load delay slot. The instruction directly after a load must observe the OLD
   value of the loaded register. Every consumer class is swept.
   ================================================================================== */

typedef enum {
    CONS_ALU_RT,      /* or rd, r0, rt         -> rd = OLD rt          */
    CONS_ALU_RS,      /* or rd, rt, r0         -> rd = OLD rt          */
    CONS_STORE_RT,    /* sw rt, 8(r1)          -> mem = OLD rt         */
    CONS_STORE_BASE,  /* sw r3, 0(rt)          -> address from OLD rt  */
    CONS_SWL_RT,      /* swl rt, 8(r1)         -> mem from OLD rt      */
    CONS_SWR_RT,      /* swr rt, 11(r1)        -> mem from OLD rt      */
    CONS_SHIFT_RS,    /* sllv rd, r3, rt       -> amount from OLD rt   */
    CONS_MULT_RS,     /* mult rt, r3           -> OLD rt               */
    CONS_BRANCH,      /* bne rt, r0, +2        -> OLD rt decides       */
    CONS_SB_RT,       /* sb rt, 8(r1)                                   */
    CONS_LOAD_BASE,   /* lw r4, 0(rt)          -> address from OLD rt  */
    CONS_MTC2_RT,     /* mtc2 rt, $0           -> GTE gets OLD rt      */
    CONS_CTC2_RT,     /* ctc2 rt, $5           -> GTE gets OLD rt      */
    CONS_MTC0_RT,     /* mtc0 rt, $3 (BPC)     -> COP0 gets OLD rt     */
    CONS_MTHI_RS,     /* mthi rt               -> HI gets OLD rt       */
    CONS_MTLO_RS,     /* mtlo rt               -> LO gets OLD rt       */
    CONS_COUNT
} consumer_t;

/* COP0/COP2 move encodings (field layout only). */
#define ENC_MTC2(rt, rd) ((0x12u << 26) | (0x04u << 21) | ((rt) << 16) | ((rd) << 11))
#define ENC_MFC2(rt, rd) ((0x12u << 26) | (0x00u << 21) | ((rt) << 16) | ((rd) << 11))
#define ENC_CTC2(rt, rd) ((0x12u << 26) | (0x06u << 21) | ((rt) << 16) | ((rd) << 11))
#define ENC_CFC2(rt, rd) ((0x12u << 26) | (0x02u << 21) | ((rt) << 16) | ((rd) << 11))
#define ENC_MTC0(rt, rd) ((0x10u << 26) | (0x04u << 21) | ((rt) << 16) | ((rd) << 11))

static int case_load_delay(pair_t* p) {
    printf("CPU_SPEC begin case=load-delay\n");
    int ok = 1;

    const uint32_t OLD = 0x00002010u;   /* also a valid DATA_PA-ish offset when used as base */
    const uint32_t NEWV = 0x7fffabcdu;

    for (int c = 0; c < CONS_COUNT && ok; c++) {
        uint32_t prog[6];
        uint32_t old_rt = OLD;

        /* r1 = DATA_VA, r2 = rt (loaded), r3 = 0x00000003, r5 = result */
        prog[0] = enc_i(OP_LW, 1, 2, 0);     /* lw r2, 0(r1)  -> NEWV */
        switch (c) {
            case CONS_ALU_RT:     prog[1] = enc_r(0, 2, 5, 0, F_OR); break;
            case CONS_ALU_RS:     prog[1] = enc_r(2, 0, 5, 0, F_OR); break;
            case CONS_STORE_RT:   prog[1] = enc_i(OP_SW, 1, 2, 8); break;
            case CONS_STORE_BASE: prog[1] = enc_i(OP_SW, 2, 3, 0); break;
            case CONS_SWL_RT:     prog[1] = enc_i(OP_SWL, 1, 2, 8); break;
            case CONS_SWR_RT:     prog[1] = enc_i(OP_SWR, 1, 2, 11); break;
            case CONS_SHIFT_RS:   prog[1] = enc_r(2, 3, 5, 0, F_SLLV); break;
            case CONS_MULT_RS:    prog[1] = enc_r(2, 3, 0, 0, F_MULT); break;
            case CONS_BRANCH:     prog[1] = enc_i(OP_BNE, 2, 0, 2); break;
            case CONS_SB_RT:      prog[1] = enc_i(OP_SB, 1, 2, 8); break;
            case CONS_LOAD_BASE:  prog[1] = enc_i(OP_LW, 2, 4, 0); break;
            case CONS_MTC2_RT:    prog[1] = ENC_MTC2(2u, 0u); break;
            case CONS_CTC2_RT:    prog[1] = ENC_CTC2(2u, 5u); break;
            case CONS_MTC0_RT:    prog[1] = ENC_MTC0(2u, COP0_BPC); break;
            case CONS_MTHI_RS:    prog[1] = enc_r(2, 0, 0, 0, F_MTHI); break;
            case CONS_MTLO_RS:    prog[1] = enc_r(2, 0, 0, 0, F_MTLO); break;
            default:              prog[1] = NOP; break;
        }
        /* The COP2 readbacks need their own slot; everything else leaves it a nop. */
        prog[2] = (c == CONS_MTC2_RT) ? ENC_MFC2(5u, 0u)
                : (c == CONS_CTC2_RT) ? ENC_CFC2(5u, 5u)
                : NOP;
        /* Only reached when the CONS_BRANCH consumer was NOT taken; r9 is the witness. */
        prog[3] = enc_i(OP_ORI, 0, 9, 0x4444);
        prog[4] = NOP;
        prog[5] = NOP;

        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 6);
            write_data(m, 0, NEWV);
            write_data(m, 8, 0x99999999u);
            write_data(m, 12, 0x99999999u);
            write_data(m, 16, 0x5a5a5a5au);   /* DATA_VA + 16 == 0x80002010 */
            m->cpu->r[1] = DATA_VA;
            m->cpu->r[2] = old_rt;
            m->cpu->r[3] = 3u;
            m->cpu->r[5] = 0xeeeeeeeeu;
            m->cpu->r[4] = 0xeeeeeeeeu;

            psx_cpu_cycle(m->cpu);   /* load  */
            psx_cpu_cycle(m->cpu);   /* consumer -- must see OLD */
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            ++g_checked;

            uint32_t got = 0, want = 0;
            const char* what = "";
            switch (c) {
                case CONS_ALU_RT: case CONS_ALU_RS:
                    got = m->cpu->r[5]; want = old_rt; what = "rd"; break;
                case CONS_STORE_RT:
                    got = read_data(m, 8); want = old_rt; what = "mem8"; break;
                case CONS_STORE_BASE:
                    /* base = OLD rt = 0x00002010 (KUSEG alias of DATA_PA+0x10) */
                    got = read_data(m, 16); want = 3u; what = "mem@oldbase"; break;
                case CONS_SWL_RT:
                    got = read_data(m, 8); want = ref_swl(0x99999999u, old_rt, 0); what = "mem8"; break;
                case CONS_SWR_RT:
                    got = read_data(m, 8); want = ref_swr(0x99999999u, old_rt, 3); what = "mem8"; break;
                case CONS_SHIFT_RS:
                    got = m->cpu->r[5]; want = ref_shift(F_SLLV, 3u, old_rt); what = "rd"; break;
                case CONS_MULT_RS:
                    got = m->cpu->lo;
                    want = (uint32_t)(((int64_t)(int32_t)old_rt * 3) & 0xffffffffu);
                    what = "lo"; break;
                case CONS_BRANCH:
                    /* OLD rt is non-zero -> branch taken -> prog[3] is skipped.
                       If the branch saw the NEW value it is also non-zero, so the
                       discriminator is the OFFSET: taken skips r9. */
                    got = m->cpu->r[9]; want = 0u; what = "r9(branch-taken)"; break;
                case CONS_SB_RT:
                    got = read_data(m, 8) & 0xffu; want = old_rt & 0xffu; what = "mem8.b"; break;
                case CONS_LOAD_BASE:
                    got = m->cpu->r[4]; want = 0x5a5a5a5au; what = "r4"; break;
                default: break;
            }

            if (got != want) {
                fail("case=load-delay mode=%s consumer=%d %s want=%08x got=%08x "
                     "(old rt=%08x new rt=%08x)",
                     MODE_NAME[mi], c, what, want, got, old_rt, NEWV);
                ok = 0;
            }

            /* After the delay slot the register must hold the loaded value (unless the
               consumer itself wrote it -- none of these do). */
            if (ok && c != CONS_LOAD_BASE && m->cpu->r[2] != NEWV) {
                fail("case=load-delay mode=%s consumer=%d rt-after want=%08x got=%08x",
                     MODE_NAME[mi], c, NEWV, m->cpu->r[2]);
                ok = 0;
            }
        }
    }

    /* Two loads into the same register: the first result is DISCARDED, the register ends
       up holding the second. */
    {
        uint32_t prog[4] = {
            enc_i(OP_LW, 1, 2, 0),
            enc_i(OP_LW, 1, 2, 4),
            NOP, NOP
        };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 4);
            write_data(m, 0, 0xaaaaaaaau);
            write_data(m, 4, 0xbbbbbbbbu);
            m->cpu->r[1] = DATA_VA;
            m->cpu->r[2] = 0xccccccccu;
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[2] != 0xbbbbbbbbu) {
                fail("case=load-delay mode=%s double-load want=bbbbbbbb got=%08x",
                     MODE_NAME[mi], m->cpu->r[2]);
                ok = 0;
            }
        }
    }

    /* A load into a register followed by an ALU write of the SAME register: the ALU
       result wins (the load's write-back is overwritten). */
    {
        uint32_t prog[4] = {
            enc_i(OP_LW, 1, 2, 0),
            enc_i(OP_ORI, 0, 2, 0x1234),   /* ori r2, r0, 0x1234 */
            NOP, NOP
        };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 4);
            write_data(m, 0, 0xaaaaaaaau);
            m->cpu->r[1] = DATA_VA;
            m->cpu->r[2] = 0xccccccccu;
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[2] != 0x1234u) {
                fail("case=load-delay mode=%s load-then-alu-same-reg want=00001234 got=%08x",
                     MODE_NAME[mi], m->cpu->r[2]);
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=load-delay\n");
    return ok;
}

/* ==================================================================================
   Case: shift matrix (all amounts x boundary operands, variable forms swept past 31).
   ================================================================================== */

static int case_shift_matrix(pair_t* p) {
    printf("CPU_SPEC begin case=shift-matrix\n");
    int ok = 1;

    static const uint32_t operands[] = {
        0x00000000u, 0xffffffffu, 0x80000000u, 0x7fffffffu, 0x00000001u,
        0x0000ffffu, 0xdeadbeefu, 0xa5a5a5a5u, 0x00000080u, 0xfffffffeu
    };
    static const unsigned imm_f[3] = { F_SLL, F_SRL, F_SRA };
    static const unsigned var_f[3] = { F_SLLV, F_SRLV, F_SRAV };

    for (size_t v = 0; v < sizeof operands / sizeof operands[0] && ok; v++) {
        for (unsigned amount = 0; amount < 64u && ok; amount++) {
            for (int f = 0; f < 3 && ok; f++) {
                /* immediate forms only exist for 0..31 */
                if (amount < 32u) {
                    uint32_t op = enc_r(0, 2, 5, amount, imm_f[f]);
                    uint32_t prog[2] = { op, NOP };
                    uint32_t want = ref_shift(imm_f[f], operands[v], amount);
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 2);
                        m->cpu->r[2] = operands[v];
                        m->cpu->r[5] = 0xcafef00du;
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (m->cpu->r[5] != want) {
                            fail("case=shift-matrix mode=%s funct=%02x imm amount=%u rt=%08x "
                                 "want=%08x got=%08x",
                                 MODE_NAME[mi], imm_f[f], amount, operands[v], want, m->cpu->r[5]);
                            ok = 0;
                        }
                    }
                }

                {
                    uint32_t op = enc_r(1, 2, 5, 0, var_f[f]);
                    uint32_t prog[2] = { op, NOP };
                    uint32_t want = ref_shift(var_f[f], operands[v], amount);
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 2);
                        m->cpu->r[2] = operands[v];
                        /* high bits in rs prove the mandatory & 31 is applied */
                        m->cpu->r[1] = amount | 0x5a5a0000u;
                        m->cpu->r[5] = 0xcafef00du;
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (m->cpu->r[5] != want) {
                            fail("case=shift-matrix mode=%s funct=%02x var amount=%u rt=%08x "
                                 "want=%08x got=%08x",
                                 MODE_NAME[mi], var_f[f], amount, operands[v], want, m->cpu->r[5]);
                            ok = 0;
                        }
                    }
                }
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=shift-matrix\n");
    return ok;
}

/* ==================================================================================
   Case: ALU matrix + overflow semantics.
   ================================================================================== */

static int case_alu_matrix(pair_t* p) {
    printf("CPU_SPEC begin case=alu-matrix\n");
    int ok = 1;

    static const uint32_t vals[] = {
        0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu, 0x80000000u,
        0x00008000u, 0xffff8000u, 0x00007fffu, 0x12345678u, 0xdeadbeefu
    };

    for (size_t a = 0; a < sizeof vals / sizeof vals[0] && ok; a++) {
        for (size_t b = 0; b < sizeof vals / sizeof vals[0] && ok; b++) {
            uint32_t s = vals[a], t = vals[b];
            int64_t ssum = (int64_t)(int32_t)s + (int64_t)(int32_t)t;
            int64_t sdif = (int64_t)(int32_t)s - (int64_t)(int32_t)t;
            int add_ovf = (ssum < -2147483648LL) || (ssum > 2147483647LL);
            int sub_ovf = (sdif < -2147483648LL) || (sdif > 2147483647LL);

            struct { unsigned funct; uint32_t want; int overflow; const char* n; } r[] = {
                { F_ADD,  (uint32_t)(s + t), add_ovf, "add"  },
                { F_ADDU, (uint32_t)(s + t), 0,       "addu" },
                { F_SUB,  (uint32_t)(s - t), sub_ovf, "sub"  },
                { F_SUBU, (uint32_t)(s - t), 0,       "subu" },
                { F_AND,  s & t,             0,       "and"  },
                { F_OR,   s | t,             0,       "or"   },
                { F_XOR,  s ^ t,             0,       "xor"  },
                { F_NOR,  ~(s | t),          0,       "nor"  },
                { F_SLT,  ((int32_t)s < (int32_t)t) ? 1u : 0u, 0, "slt"  },
                { F_SLTU, (s < t) ? 1u : 0u, 0,       "sltu" },
            };

            for (size_t i = 0; i < sizeof r / sizeof r[0] && ok; i++) {
                uint32_t prog[2] = { enc_r(1, 2, 5, 0, r[i].funct), NOP };
                for (int mi = 0; mi < 2 && ok; mi++) {
                    psx_t* m = p->m[mi];
                    cpu_reset(m->cpu);
                    write_prog(m, prog, 2);
                    m->cpu->r[1] = s;
                    m->cpu->r[2] = t;
                    m->cpu->r[5] = 0xcafef00du;
                    psx_cpu_cycle(m->cpu);
                    ++g_checked;
                    if (r[i].overflow) {
                        if (m->cpu->r[5] != 0xcafef00du) {
                            fail("case=alu-matrix mode=%s %s s=%08x t=%08x overflow wrote rd=%08x",
                                 MODE_NAME[mi], r[i].n, s, t, m->cpu->r[5]);
                            ok = 0;
                        }
                        if ((m->cpu->cop0_r[COP0_CAUSE] & 0x7cu) != CAUSE_OV) {
                            fail("case=alu-matrix mode=%s %s s=%08x t=%08x excode want=%02x got=%02x",
                                 MODE_NAME[mi], r[i].n, s, t, CAUSE_OV,
                                 m->cpu->cop0_r[COP0_CAUSE] & 0x7cu);
                            ok = 0;
                        }
                    } else if (m->cpu->r[5] != r[i].want) {
                        fail("case=alu-matrix mode=%s %s s=%08x t=%08x want=%08x got=%08x",
                             MODE_NAME[mi], r[i].n, s, t, r[i].want, m->cpu->r[5]);
                        ok = 0;
                    }
                }
            }

            /* Immediate forms: the 16-bit field is SIGN-extended for ADDI/ADDIU/SLTI/SLTIU
               and ZERO-extended for ANDI/ORI/XORI. */
            {
                uint16_t imm = (uint16_t)(t & 0xffffu);
                int32_t simm = (int32_t)(int16_t)imm;
                int64_t isum = (int64_t)(int32_t)s + (int64_t)simm;
                int iovf = (isum < -2147483648LL) || (isum > 2147483647LL);

                struct { unsigned op; uint32_t want; int overflow; const char* n; } q[] = {
                    { OP_ADDI,  (uint32_t)(s + (uint32_t)simm), iovf, "addi"  },
                    { OP_ADDIU, (uint32_t)(s + (uint32_t)simm), 0,    "addiu" },
                    { OP_SLTI,  ((int32_t)s < simm) ? 1u : 0u,  0,    "slti"  },
                    { OP_SLTIU, (s < (uint32_t)simm) ? 1u : 0u, 0,    "sltiu" },
                    { OP_ANDI,  s & imm,                        0,    "andi"  },
                    { OP_ORI,   s | imm,                        0,    "ori"   },
                    { OP_XORI,  s ^ imm,                        0,    "xori"  },
                };

                for (size_t i = 0; i < sizeof q / sizeof q[0] && ok; i++) {
                    uint32_t prog[2] = { enc_i(q[i].op, 1, 5, imm), NOP };
                    for (int mi = 0; mi < 2 && ok; mi++) {
                        psx_t* m = p->m[mi];
                        cpu_reset(m->cpu);
                        write_prog(m, prog, 2);
                        m->cpu->r[1] = s;
                        m->cpu->r[5] = 0xcafef00du;
                        psx_cpu_cycle(m->cpu);
                        ++g_checked;
                        if (q[i].overflow) {
                            if (m->cpu->r[5] != 0xcafef00du) {
                                fail("case=alu-matrix mode=%s %s s=%08x imm=%04x overflow wrote rt=%08x",
                                     MODE_NAME[mi], q[i].n, s, imm, m->cpu->r[5]);
                                ok = 0;
                            }
                        } else if (m->cpu->r[5] != q[i].want) {
                            fail("case=alu-matrix mode=%s %s s=%08x imm=%04x want=%08x got=%08x",
                                 MODE_NAME[mi], q[i].n, s, imm, q[i].want, m->cpu->r[5]);
                            ok = 0;
                        }
                    }
                }
            }
        }
    }

    /* LUI: imm goes in the high half, low half is zero. */
    for (unsigned imm = 0; imm < 65536u && ok; imm += 13u) {
        uint32_t prog[2] = { enc_i(OP_LUI, 0, 5, (uint16_t)imm), NOP };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 2);
            m->cpu->r[5] = 0xcafef00du;
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[5] != (imm << 16)) {
                fail("case=alu-matrix mode=%s lui imm=%04x want=%08x got=%08x",
                     MODE_NAME[mi], imm, imm << 16, m->cpu->r[5]);
                ok = 0;
            }
        }
    }

    /* r0 must stay zero no matter what is written to it. */
    {
        uint32_t prog[2] = { enc_i(OP_ORI, 0, 0, 0xffff), NOP };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 2);
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[0] != 0u) {
                fail("case=alu-matrix mode=%s r0-written got=%08x", MODE_NAME[mi], m->cpu->r[0]);
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=alu-matrix\n");
    return ok;
}

/* ==================================================================================
   Case: MULT/MULTU/DIV/DIVU including the documented degenerate results.
   ================================================================================== */

static uint32_t xs_state = 0x13579bdfu;
static uint32_t xs(void) {
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 17;
    xs_state ^= xs_state << 5;
    return xs_state;
}

static int muldiv_one(pair_t* p, uint32_t s, uint32_t t) {
    int ok = 1;

    int64_t sm = (int64_t)(int32_t)s * (int64_t)(int32_t)t;
    uint64_t um = (uint64_t)s * (uint64_t)t;

    uint32_t div_hi, div_lo, divu_hi, divu_lo;
    if (t == 0u) {
        /* spec: LO = (S >= 0) ? 0xffffffff : 1 ; HI = S */
        div_hi = s;
        div_lo = ((int32_t)s >= 0) ? 0xffffffffu : 1u;
    } else if (s == 0x80000000u && t == 0xffffffffu) {
        div_hi = 0u;
        div_lo = 0x80000000u;
    } else {
        div_hi = (uint32_t)((int32_t)s % (int32_t)t);
        div_lo = (uint32_t)((int32_t)s / (int32_t)t);
    }
    if (t == 0u) {
        divu_hi = s;
        divu_lo = 0xffffffffu;
    } else {
        divu_hi = s % t;
        divu_lo = s / t;
    }

    struct { unsigned funct; uint32_t hi, lo; const char* n; } c[] = {
        { F_MULT,  (uint32_t)((uint64_t)sm >> 32), (uint32_t)((uint64_t)sm & 0xffffffffu), "mult"  },
        { F_MULTU, (uint32_t)(um >> 32),           (uint32_t)(um & 0xffffffffu),           "multu" },
        { F_DIV,   div_hi,                         div_lo,                                 "div"   },
        { F_DIVU,  divu_hi,                        divu_lo,                                "divu"  },
    };

    for (size_t i = 0; i < sizeof c / sizeof c[0] && ok; i++) {
        uint32_t prog[2] = { enc_r(1, 2, 0, 0, c[i].funct), NOP };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 2);
            m->cpu->r[1] = s;
            m->cpu->r[2] = t;
            m->cpu->hi = 0x11111111u;
            m->cpu->lo = 0x22222222u;
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->hi != c[i].hi || m->cpu->lo != c[i].lo) {
                fail("case=muldiv mode=%s %s s=%08x t=%08x want hi=%08x lo=%08x got hi=%08x lo=%08x",
                     MODE_NAME[mi], c[i].n, s, t, c[i].hi, c[i].lo, m->cpu->hi, m->cpu->lo);
                ok = 0;
            }
        }
    }
    return ok;
}

static int case_muldiv(pair_t* p) {
    printf("CPU_SPEC begin case=muldiv\n");
    int ok = 1;

    static const uint32_t vals[] = {
        0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu, 0x80000000u,
        0x00000002u, 0xfffffffeu, 0x0000ffffu, 0xffff0000u, 0x00010000u,
        0x7ffffffeu, 0x80000001u
    };

    for (size_t a = 0; a < sizeof vals / sizeof vals[0] && ok; a++)
        for (size_t b = 0; b < sizeof vals / sizeof vals[0] && ok; b++)
            ok = muldiv_one(p, vals[a], vals[b]);

    for (int i = 0; i < 4000 && ok; i++)
        ok = muldiv_one(p, xs(), xs());

    /* MFHI/MFLO/MTHI/MTLO round trip. */
    {
        uint32_t prog[5] = {
            enc_r(0, 0, 5, 0, F_MFHI),
            enc_r(0, 0, 6, 0, F_MFLO),
            enc_r(1, 0, 0, 0, F_MTHI),
            enc_r(2, 0, 0, 0, F_MTLO),
            NOP
        };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 5);
            m->cpu->hi = 0xaaaaaaaau;
            m->cpu->lo = 0xbbbbbbbbu;
            m->cpu->r[1] = 0x12345678u;
            m->cpu->r[2] = 0x9abcdef0u;
            for (int k = 0; k < 5; k++)
                psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->r[5] != 0xaaaaaaaau || m->cpu->r[6] != 0xbbbbbbbbu ||
                m->cpu->hi != 0x12345678u || m->cpu->lo != 0x9abcdef0u) {
                fail("case=muldiv mode=%s hi/lo move r5=%08x r6=%08x hi=%08x lo=%08x",
                     MODE_NAME[mi], m->cpu->r[5], m->cpu->r[6], m->cpu->hi, m->cpu->lo);
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=muldiv\n");
    return ok;
}

/* ==================================================================================
   Case: branch and jump semantics.
   ================================================================================== */

static int case_branch_matrix(pair_t* p) {
    printf("CPU_SPEC begin case=branch-matrix\n");
    int ok = 1;

    static const uint32_t vals[] = {
        0x00000000u, 0x00000001u, 0xffffffffu, 0x7fffffffu, 0x80000000u, 0x00000002u
    };

    /* Conditional branches: rs (and rt for BEQ/BNE) over the boundary set; both
       directions of the offset. The delay slot ALWAYS executes. */
    for (size_t a = 0; a < sizeof vals / sizeof vals[0] && ok; a++) {
        for (size_t b = 0; b < sizeof vals / sizeof vals[0] && ok; b++) {
            uint32_t s = vals[a], t = vals[b];
            int32_t si = (int32_t)s;

            struct { unsigned op; unsigned rt_field; int taken; int links; const char* n; } br[] = {
                { OP_BEQ,  2u, s == t,   0, "beq"     },
                { OP_BNE,  2u, s != t,   0, "bne"     },
                { OP_BLEZ, 0u, si <= 0,  0, "blez"    },
                { OP_BGTZ, 0u, si > 0,   0, "bgtz"    },
                { OP_BCOND, 0x00u, si < 0,  0, "bltz"   },
                { OP_BCOND, 0x01u, si >= 0, 0, "bgez"   },
                { OP_BCOND, 0x10u, si < 0,  1, "bltzal" },
                { OP_BCOND, 0x11u, si >= 0, 1, "bgezal" },
            };

            for (size_t i = 0; i < sizeof br / sizeof br[0] && ok; i++) {
                /* branch +4 instructions: target = TEST_PC + 4 + (4 << 2) */
                uint32_t prog[10];
                for (int k = 0; k < 10; k++)
                    prog[k] = NOP;
                prog[0] = enc_i(br[i].op, 1, br[i].rt_field, 4);
                prog[1] = enc_i(OP_ORI, 0, 6, 0x1111);   /* delay slot: always runs */
                prog[2] = enc_i(OP_ORI, 0, 7, 0x2222);   /* skipped when taken */
                prog[6] = enc_i(OP_ORI, 0, 8, 0x3333);   /* the branch target */

                uint32_t want_pc = br[i].taken ? (TEST_PC + 4u + (4u << 2)) : (TEST_PC + 8u);

                for (int mi = 0; mi < 2 && ok; mi++) {
                    psx_t* m = p->m[mi];
                    cpu_reset(m->cpu);
                    write_prog(m, prog, 10);
                    m->cpu->r[1] = s;
                    m->cpu->r[2] = t;
                    m->cpu->r[31] = 0xdeadbeefu;
                    psx_cpu_cycle(m->cpu);   /* the branch */
                    psx_cpu_cycle(m->cpu);   /* the delay slot */
                    ++g_checked;

                    if (m->cpu->r[6] != 0x1111u) {
                        fail("case=branch-matrix mode=%s %s s=%08x t=%08x delay slot did not run",
                             MODE_NAME[mi], br[i].n, s, t);
                        ok = 0;
                    }
                    if (m->cpu->pc != want_pc) {
                        fail("case=branch-matrix mode=%s %s s=%08x t=%08x taken=%d "
                             "pc want=%08x got=%08x",
                             MODE_NAME[mi], br[i].n, s, t, br[i].taken, want_pc, m->cpu->pc);
                        ok = 0;
                    }
                    /* BLTZAL/BGEZAL link UNCONDITIONALLY (spec) -- ra = addr of the
                       instruction after the delay slot. */
                    if (br[i].links) {
                        if (m->cpu->r[31] != TEST_PC + 8u) {
                            fail("case=branch-matrix mode=%s %s s=%08x taken=%d ra want=%08x got=%08x",
                                 MODE_NAME[mi], br[i].n, s, br[i].taken,
                                 TEST_PC + 8u, m->cpu->r[31]);
                            ok = 0;
                        }
                    } else if (m->cpu->r[31] != 0xdeadbeefu) {
                        fail("case=branch-matrix mode=%s %s clobbered ra=%08x",
                             MODE_NAME[mi], br[i].n, m->cpu->r[31]);
                        ok = 0;
                    }
                }
            }
        }
    }

    /* J / JAL: target = (address-of-delay-slot & 0xf0000000) | (imm26 << 2). */
    {
        uint32_t target = 0x80001800u;
        uint32_t imm26 = (target >> 2) & 0x3ffffffu;
        struct { unsigned op; int links; const char* n; } jj[] = {
            { OP_J, 0, "j" }, { OP_JAL, 1, "jal" }
        };
        for (size_t i = 0; i < 2 && ok; i++) {
            uint32_t prog[3] = { enc_j(jj[i].op, imm26), enc_i(OP_ORI, 0, 6, 0x1111), NOP };
            for (int mi = 0; mi < 2 && ok; mi++) {
                psx_t* m = p->m[mi];
                cpu_reset(m->cpu);
                write_prog(m, prog, 3);
                m->cpu->r[31] = 0xdeadbeefu;
                psx_cpu_cycle(m->cpu);
                psx_cpu_cycle(m->cpu);
                ++g_checked;
                if (m->cpu->pc != target) {
                    fail("case=branch-matrix mode=%s %s target want=%08x got=%08x",
                         MODE_NAME[mi], jj[i].n, target, m->cpu->pc);
                    ok = 0;
                }
                if (m->cpu->r[6] != 0x1111u) {
                    fail("case=branch-matrix mode=%s %s delay slot did not run", MODE_NAME[mi], jj[i].n);
                    ok = 0;
                }
                if (jj[i].links && m->cpu->r[31] != TEST_PC + 8u) {
                    fail("case=branch-matrix mode=%s jal ra want=%08x got=%08x",
                         MODE_NAME[mi], TEST_PC + 8u, m->cpu->r[31]);
                    ok = 0;
                }
            }
        }
    }

    /* JR / JALR, including JALR with rd == rs (the link must not disturb the jump). */
    {
        uint32_t target = 0x80001900u;
        struct { uint32_t op; int links; unsigned link_reg; const char* n; } jr[] = {
            { enc_r(1, 0, 0,  0, F_JR),   0, 0,  "jr"          },
            { enc_r(1, 0, 31, 0, F_JALR), 1, 31, "jalr"        },
            { enc_r(1, 0, 1,  0, F_JALR), 1, 1,  "jalr rd==rs" },
        };
        for (size_t i = 0; i < sizeof jr / sizeof jr[0] && ok; i++) {
            uint32_t prog[3] = { jr[i].op, enc_i(OP_ORI, 0, 6, 0x1111), NOP };
            for (int mi = 0; mi < 2 && ok; mi++) {
                psx_t* m = p->m[mi];
                cpu_reset(m->cpu);
                write_prog(m, prog, 3);
                m->cpu->r[1] = target;
                m->cpu->r[31] = 0xdeadbeefu;
                psx_cpu_cycle(m->cpu);
                psx_cpu_cycle(m->cpu);
                ++g_checked;
                if (m->cpu->pc != target) {
                    fail("case=branch-matrix mode=%s %s target want=%08x got=%08x",
                         MODE_NAME[mi], jr[i].n, target, m->cpu->pc);
                    ok = 0;
                }
                if (jr[i].links && m->cpu->r[jr[i].link_reg] != TEST_PC + 8u) {
                    fail("case=branch-matrix mode=%s %s link want=%08x got=%08x",
                         MODE_NAME[mi], jr[i].n, TEST_PC + 8u, m->cpu->r[jr[i].link_reg]);
                    ok = 0;
                }
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=branch-matrix\n");
    return ok;
}

/* ==================================================================================
   Case: the exception model -- EPC, CAUSE.BD, the SR mode stack, both vectors, RFE.
   ================================================================================== */

static int case_exception_model(pair_t* p) {
    printf("CPU_SPEC begin case=exception-model\n");
    int ok = 1;

    struct { uint32_t op; uint32_t excode; const char* n; } faults[] = {
        { enc_r(0, 0, 0, 0, F_SYSCALL), CAUSE_SYSCALL, "syscall" },
        { enc_r(0, 0, 0, 0, F_BREAK),   CAUSE_BP,      "break"   },
        { enc_i(OP_LW, 1, 2, 1),        CAUSE_ADEL,    "adel"    },
        { enc_i(OP_SW, 1, 2, 1),        CAUSE_ADES,    "ades"    },
    };

    for (size_t i = 0; i < sizeof faults / sizeof faults[0] && ok; i++) {
        for (int in_delay = 0; in_delay < 2 && ok; in_delay++) {
            for (int bev = 0; bev < 2 && ok; bev++) {
                uint32_t prog[6];
                for (int k = 0; k < 6; k++)
                    prog[k] = NOP;

                uint32_t fault_pc;
                if (in_delay) {
                    prog[0] = enc_i(OP_BEQ, 0, 0, 4);  /* always taken */
                    prog[1] = faults[i].op;            /* fault in the delay slot */
                    fault_pc = TEST_PC + 4u;
                } else {
                    prog[0] = faults[i].op;
                    fault_pc = TEST_PC;
                }

                uint32_t sr0 = 0x10000000u | (bev ? SR_BEV : 0u) | 0x0000003fu;
                uint32_t want_sr = (sr0 & 0xffffffc0u) | ((sr0 << 2) & 0x3fu);
                uint32_t want_epc = in_delay ? (fault_pc - 4u) : fault_pc;
                uint32_t want_vec = bev ? EXC_VEC_BEV : EXC_VEC_NORMAL;

                for (int mi = 0; mi < 2 && ok; mi++) {
                    psx_t* m = p->m[mi];
                    cpu_reset(m->cpu);
                    write_prog(m, prog, 6);
                    m->cpu->cop0_r[COP0_SR] = sr0;
                    m->cpu->r[1] = DATA_VA;
                    m->cpu->r[2] = 0x12345678u;
                    psx_cpu_cycle(m->cpu);
                    if (in_delay)
                        psx_cpu_cycle(m->cpu);
                    ++g_checked;

                    uint32_t cause = m->cpu->cop0_r[COP0_CAUSE];
                    if ((cause & 0x7cu) != faults[i].excode) {
                        fail("case=exception-model mode=%s %s delay=%d excode want=%02x got=%02x",
                             MODE_NAME[mi], faults[i].n, in_delay, faults[i].excode, cause & 0x7cu);
                        ok = 0;
                    }
                    if (((cause >> 31) & 1u) != (uint32_t)in_delay) {
                        fail("case=exception-model mode=%s %s delay=%d BD want=%d got=%u",
                             MODE_NAME[mi], faults[i].n, in_delay, in_delay, (cause >> 31) & 1u);
                        ok = 0;
                    }
                    if (m->cpu->cop0_r[COP0_EPC] != want_epc) {
                        fail("case=exception-model mode=%s %s delay=%d epc want=%08x got=%08x",
                             MODE_NAME[mi], faults[i].n, in_delay, want_epc,
                             m->cpu->cop0_r[COP0_EPC]);
                        ok = 0;
                    }
                    if (m->cpu->pc != want_vec) {
                        fail("case=exception-model mode=%s %s bev=%d vector want=%08x got=%08x",
                             MODE_NAME[mi], faults[i].n, bev, want_vec, m->cpu->pc);
                        ok = 0;
                    }
                    if (m->cpu->next_pc != want_vec + 4u) {
                        fail("case=exception-model mode=%s %s next_pc want=%08x got=%08x",
                             MODE_NAME[mi], faults[i].n, want_vec + 4u, m->cpu->next_pc);
                        ok = 0;
                    }
                    if (m->cpu->cop0_r[COP0_SR] != want_sr) {
                        fail("case=exception-model mode=%s %s sr-push want=%08x got=%08x",
                             MODE_NAME[mi], faults[i].n, want_sr, m->cpu->cop0_r[COP0_SR]);
                        ok = 0;
                    }
                }
            }
        }
    }

    /* RFE pops the mode stack: bits[3:0] <- bits[5:2]; bits 4-5 are LEFT ALONE. */
    for (unsigned mode = 0; mode < 64u && ok; mode++) {
        uint32_t sr0 = 0x10000000u | mode;
        uint32_t want = (sr0 & 0xfffffff0u) | ((sr0 >> 2) & 0xfu);
        uint32_t prog[2] = { 0x42000010u, NOP };  /* rfe */
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 2);
            m->cpu->cop0_r[COP0_SR] = sr0;
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (m->cpu->cop0_r[COP0_SR] != want) {
                fail("case=exception-model mode=%s rfe sr0=%08x want=%08x got=%08x",
                     MODE_NAME[mi], sr0, want, m->cpu->cop0_r[COP0_SR]);
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=exception-model\n");
    return ok;
}

/* ==================================================================================
   Case: COP0 register write masks (documented table).
   ================================================================================== */

static int case_cop0_masks(pair_t* p) {
    printf("CPU_SPEC begin case=cop0-masks\n");
    int ok = 1;

    /* Only IP0/IP1 (bits 8-9) of CAUSE are software-writable; EPC/BadVaddr/PRID/JUMPDEST
       are read-only; DCIC has reserved bits. */
    static const uint32_t mask[16] = {
        0x00000000u, 0x00000000u, 0x00000000u, 0xffffffffu,
        0x00000000u, 0xffffffffu, 0x00000000u, 0xffc0f03fu,
        0x00000000u, 0xffffffffu, 0x00000000u, 0xffffffffu,
        0xffffffffu, 0x00000300u, 0x00000000u, 0x00000000u
    };

    static const uint32_t writes[] = { 0xffffffffu, 0x00000000u, 0xa5a5a5a5u, 0x5a5a5a5au };

    for (unsigned reg = 0; reg < 16u && ok; reg++) {
        for (size_t w = 0; w < sizeof writes / sizeof writes[0] && ok; w++) {
            uint32_t seed = 0x00000000u;
            uint32_t want = (seed & ~mask[reg]) | (writes[w] & mask[reg]);
            /* mtc0 r2, copreg */
            uint32_t prog[3] = { ((OP_COP0) << 26) | (0x04u << 21) | (2u << 16) | (reg << 11), NOP, NOP };
            for (int mi = 0; mi < 2 && ok; mi++) {
                psx_t* m = p->m[mi];
                cpu_reset(m->cpu);
                write_prog(m, prog, 3);
                m->cpu->cop0_r[reg] = seed;
                m->cpu->r[2] = writes[w];
                psx_cpu_cycle(m->cpu);
                ++g_checked;
                if (m->cpu->cop0_r[reg] != want) {
                    fail("case=cop0-masks mode=%s mtc0 r%u <- %08x want=%08x got=%08x",
                         MODE_NAME[mi], reg, writes[w], want, m->cpu->cop0_r[reg]);
                    ok = 0;
                }
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=cop0-masks\n");
    return ok;
}

/* ==================================================================================
   Case: cache isolation. With SR.IsC set, no store may reach memory -- the store is
   absorbed by the (unimplemented) data cache. The BIOS flush routines rely on this, and a
   store that leaks through corrupts RAM at a point no game can defend against.
   ================================================================================== */

static int case_cache_isolation(pair_t* p) {
    printf("CPU_SPEC begin case=cache-isolation\n");
    int ok = 1;

    struct { uint32_t op; const char* n; } st[] = {
        { enc_i(OP_SB,  1, 2, 0), "sb"  },
        { enc_i(OP_SH,  1, 2, 0), "sh"  },
        { enc_i(OP_SW,  1, 2, 0), "sw"  },
        { enc_i(OP_SWL, 1, 2, 2), "swl" },
        { enc_i(OP_SWR, 1, 2, 1), "swr" },
    };

    for (size_t i = 0; i < sizeof st / sizeof st[0] && ok; i++) {
        uint32_t prog[2] = { st[i].op, NOP };
        for (int mi = 0; mi < 2 && ok; mi++) {
            psx_t* m = p->m[mi];
            cpu_reset(m->cpu);
            write_prog(m, prog, 2);
            write_data(m, 0, 0x11223344u);
            m->cpu->cop0_r[COP0_SR] |= SR_ISC;
            m->cpu->r[1] = DATA_VA;
            m->cpu->r[2] = 0xdeadbeefu;
            psx_cpu_cycle(m->cpu);
            ++g_checked;
            if (read_data(m, 0) != 0x11223344u) {
                fail("case=cache-isolation mode=%s %s leaked through IsC: mem=%08x (want 11223344)",
                     MODE_NAME[mi], st[i].n, read_data(m, 0));
                ok = 0;
            }
        }
    }

    if (ok)
        printf("CPU_SPEC passed case=cache-isolation\n");
    return ok;
}

/* ================================================================================== */

int main(void) {
    const char* bios_path = "build/tests/blank-bios.bin";
    if (!write_blank_bios(bios_path)) {
        fprintf(stderr, "CPU_SPEC failed reason=create-bios path=%s\n", bios_path);
        return 1;
    }

    pair_t p;
    memset(&p, 0, sizeof p);
    if (!pair_init(&p, bios_path)) {
        fprintf(stderr, "CPU_SPEC failed reason=init\n");
        return 1;
    }

    int ok = 1;
    ok &= case_load_extend(&p);
    ok &= case_store_width(&p);
    ok &= case_addr_error(&p);
    ok &= case_unaligned(&p);
    ok &= case_load_delay(&p);
    ok &= case_shift_matrix(&p);
    ok &= case_alu_matrix(&p);
    ok &= case_muldiv(&p);
    ok &= case_branch_matrix(&p);
    ok &= case_exception_model(&p);
    ok &= case_cop0_masks(&p);
    ok &= case_cache_isolation(&p);

    pair_destroy(&p);

    if (!ok || g_failures) {
        fprintf(stderr, "CPU_SPEC FAILED failures=%d checked=%lu\n", g_failures, g_checked);
        return 1;
    }

    printf("CPU_SPEC all cases passed checked=%lu\n", g_checked);
    return 0;
}
