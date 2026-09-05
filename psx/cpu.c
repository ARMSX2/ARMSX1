#include "cpu.h"
#include "fastboot.h"
#include "bus.h"
#include "log.h"
#include "perf.h"
#include "pgxp.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "cpu_debug.h"

#define PSX_CPU_CACHE_ENTRY_COUNT 16384u
#define PSX_CPU_CACHE_ENTRY_MASK (PSX_CPU_CACHE_ENTRY_COUNT - 1u)

typedef int (*psx_cpu_cached_handler_t)(psx_cpu_t*);

typedef struct {
    uint32_t address;
    uint32_t opcode;
    psx_cpu_cached_handler_t handler;
    uint8_t valid;
} psx_cpu_cache_entry_t;

struct psx_cpu_cache_t {
    psx_cpu_cache_entry_t entries[PSX_CPU_CACHE_ENTRY_COUNT];
    psx_cpu_cache_stats_t stats;
};

static int psx_cpu_execute_cached(psx_cpu_t*);

static uint32_t psx_cpu_cache_index(uint32_t address) {
    return (address >> 2) & PSX_CPU_CACHE_ENTRY_MASK;
}

static void psx_cpu_bus_write_observer(void* udata, uint32_t address, uint32_t size) {
    psx_cpu_invalidate_range((psx_cpu_t*)udata, address, size);
}

static const uint32_t g_psx_cpu_cop0_write_mask_table[] = {
    0x00000000, // cop0r0   - N/A
    0x00000000, // cop0r1   - N/A
    0x00000000, // cop0r2   - N/A
    0xffffffff, // BPC      - Breakpoint on execute (R/W)
    0x00000000, // cop0r4   - N/A
    0xffffffff, // BDA      - Breakpoint on data access (R/W)
    0x00000000, // JUMPDEST - Randomly memorized jump address (R)
    0xffc0f03f, // DCIC     - Breakpoint control (R/W)
    0x00000000, // BadVaddr - Bad Virtual Address (R)
    0xffffffff, // BDAM     - Data Access breakpoint mask (R/W)
    0x00000000, // cop0r10  - N/A
    0xffffffff, // BPCM     - Execute breakpoint mask (R/W)
    0xffffffff, // SR       - System status register (R/W)
    0x00000300, // CAUSE    - Describes the most recently recognised exception (R)
    0x00000000, // EPC      - Return Address from Trap (R)
    0x00000000  // PRID     - Processor ID (R)
};

static const uint8_t g_psx_gte_unr_table[] = {
    0xff, 0xfd, 0xfb, 0xf9, 0xf7, 0xf5, 0xf3, 0xf1,
    0xef, 0xee, 0xec, 0xea, 0xe8, 0xe6, 0xe4, 0xe3,
    0xe1, 0xdf, 0xdd, 0xdc, 0xda, 0xd8, 0xd6, 0xd5,
    0xd3, 0xd1, 0xd0, 0xce, 0xcd, 0xcb, 0xc9, 0xc8,
    0xc6, 0xc5, 0xc3, 0xc1, 0xc0, 0xbe, 0xbd, 0xbb,
    0xba, 0xb8, 0xb7, 0xb5, 0xb4, 0xb2, 0xb1, 0xb0,
    0xae, 0xad, 0xab, 0xaa, 0xa9, 0xa7, 0xa6, 0xa4,
    0xa3, 0xa2, 0xa0, 0x9f, 0x9e, 0x9c, 0x9b, 0x9a,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90,
    0x8f, 0x8d, 0x8c, 0x8b, 0x8a, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7f, 0x7e, 0x7d,
    0x7c, 0x7b, 0x7a, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6f, 0x6e, 0x6d, 0x6c,
    0x6b, 0x6a, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5f, 0x5e, 0x5d, 0x5d,
    0x5c, 0x5b, 0x5a, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4f, 0x4e,
    0x4d, 0x4d, 0x4c, 0x4b, 0x4a, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41,
    0x40, 0x3f, 0x3f, 0x3e, 0x3d, 0x3c, 0x3c, 0x3b,
    0x3a, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35,
    0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2f,
    0x2e, 0x2e, 0x2d, 0x2c, 0x2c, 0x2b, 0x2a, 0x2a,
    0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24,
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1f,
    0x1e, 0x1e, 0x1d, 0x1d, 0x1c, 0x1b, 0x1b, 0x1a,
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15,
    0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11,
    0x10, 0x0f, 0x0f, 0x0e, 0x0e, 0x0d, 0x0d, 0x0c,
    0x0c, 0x0b, 0x0a, 0x0a, 0x09, 0x09, 0x08, 0x08,
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04,
    0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00,
    0x00
};

static inline void psx_gte_i_rtps(psx_cpu_t*);
static inline void psx_gte_i_nclip(psx_cpu_t*);
static inline void psx_gte_i_op(psx_cpu_t*);
static inline void psx_gte_i_dpcs(psx_cpu_t*);
static inline void psx_gte_i_intpl(psx_cpu_t*);
static inline void psx_gte_i_mvmva(psx_cpu_t*);
static inline void psx_gte_i_ncds(psx_cpu_t*);
static inline void psx_gte_i_cdp(psx_cpu_t*);
static inline void psx_gte_i_ncdt(psx_cpu_t*);
static inline void psx_gte_i_nccs(psx_cpu_t*);
static inline void psx_gte_i_cc(psx_cpu_t*);
static inline void psx_gte_i_ncs(psx_cpu_t*);
static inline void psx_gte_i_nct(psx_cpu_t*);
static inline void psx_gte_i_sqr(psx_cpu_t*);
static inline void psx_gte_i_dcpl(psx_cpu_t*);
static inline void psx_gte_i_dpct(psx_cpu_t*);
static inline void psx_gte_i_avsz3(psx_cpu_t*);
static inline void psx_gte_i_avsz4(psx_cpu_t*);
static inline void psx_gte_i_rtpt(psx_cpu_t*);
static inline void psx_gte_i_gpf(psx_cpu_t*);
static inline void psx_gte_i_gpl(psx_cpu_t*);
static inline void psx_gte_i_ncct(psx_cpu_t*);

#define OP ((cpu->opcode >> 26) & 0x3f)
#define S ((cpu->opcode >> 21) & 0x1f)
#define T ((cpu->opcode >> 16) & 0x1f)
#define D ((cpu->opcode >> 11) & 0x1f)
#define IMM5 ((cpu->opcode >> 6) & 0x1f)
#define CMT ((cpu->opcode >> 6) & 0xfffff)
#define SOP (cpu->opcode & 0x3f)
#define IMM26 (cpu->opcode & 0x3ffffff)
#define IMM16 (cpu->opcode & 0xffff)
#define IMM16S ((int32_t)((int16_t)IMM16))

#define COP2_DR(idx) ((uint32_t*)(&cpu->cop2_dr))[idx]
#define COP2_CR(idx) ((uint32_t*)(&cpu->cop2_cr))[idx]

#define R_R0 (cpu->r[0])
#define R_A0 (cpu->r[4])
#define R_RA (cpu->r[31])

#define DO_PENDING_LOAD { \
    cpu->r[cpu->load_d] = cpu->load_v; \
    R_R0 = 0; \
    cpu->load_v = 0xffffffff; \
    cpu->load_d = 0; }

#define SE8(v) ((int32_t)((int8_t)v))
#define SE16(v) ((int32_t)((int16_t)v))

#define BRANCH(offset) { \
    cpu->next_pc = cpu->next_pc + (offset); \
    cpu->next_pc = cpu->next_pc - 4; \
    cpu->branch = 1; \
    cpu->branch_taken = 1; }

void cpu_a_kcall_hook(psx_cpu_t* cpu) {
    switch (cpu->r[9]) {
        case 0x09: putc(R_A0, stdout); break;
        case 0x3c: putchar(R_A0); break;
        case 0x3e: {
            uint32_t src = R_A0;

            char c = psx_bus_read8(cpu->bus, src++);

            while (c) {
                putchar(c);

                c = psx_bus_read8(cpu->bus, src++);
            }
        } break;
    }
}

void cpu_b_kcall_hook(psx_cpu_t* cpu) {
    switch (cpu->r[9]) {
        case 0x3b: putc(R_A0, stdout); break;
        case 0x3d: putchar(R_A0); break;
        case 0x3f: {
            uint32_t src = R_A0;

            char c = psx_bus_read8(cpu->bus, src++);

            while (c) {
                putchar(c);

                c = psx_bus_read8(cpu->bus, src++);
            }
        } break;
    }
}

psx_cpu_t* psx_cpu_create(void) {
    psx_cpu_t* cpu = (psx_cpu_t*)calloc(1, sizeof(*cpu));

    if (!cpu)
        return NULL;

    cpu->cache = (struct psx_cpu_cache_t*)calloc(1, sizeof(*cpu->cache));
    if (!cpu->cache) {
        free(cpu);
        return NULL;
    }

    cpu->execution_mode = PSX_CPU_CACHED_INTERPRETER;
    return cpu;
}

void cpu_a_kcall_hook(psx_cpu_t*);
void cpu_b_kcall_hook(psx_cpu_t*);

void psx_cpu_destroy(psx_cpu_t* cpu) {
    if (!cpu)
        return;

    free(cpu->cache);
    free(cpu);
}

void psx_cpu_set_a_kcall_hook(psx_cpu_t* cpu, psx_cpu_kcall_hook_t hook) {
    cpu->a_function_hook = hook;
}

void psx_cpu_set_b_kcall_hook(psx_cpu_t* cpu, psx_cpu_kcall_hook_t hook) {
    cpu->b_function_hook = hook;
}

/*
    Save state.

    The old implementation fwrite()'d the raw struct up to offsetof(bus). That
    is not portable (padding, endianness, and the packed GTE unions) and it also
    wrote the GTE register file as an opaque blob. Everything is now emitted
    field by field as fixed-width little-endian values.

    Deliberately NOT saved: cpu->bus (host wiring), the A/B kcall hooks (host
    callbacks), cpu->execution_mode (a user setting, not machine state) and
    cpu->cache (a derived block cache — invalidated on load and rebuilt lazily).
*/

static void cpu_save_gte_vertex(psx_state_writer_t* w, const gte_vertex_t* v) {
    psx_sw_u32(w, v->xy);
    psx_sw_u16(w, (uint16_t)v->z);
}

static void cpu_load_gte_vertex(psx_state_reader_t* r, gte_vertex_t* v) {
    v->xy = psx_sr_u32(r);
    v->z = (int16_t)psx_sr_u16(r);
}

static void cpu_save_gte_matrix(psx_state_writer_t* w, const gte_matrix_t* m) {
    int i;

    for (i = 0; i < 4; i++)
        psx_sw_u32(w, m->m[i].u32);

    psx_sw_u16(w, (uint16_t)m->m33);
}

static void cpu_load_gte_matrix(psx_state_reader_t* r, gte_matrix_t* m) {
    int i;

    for (i = 0; i < 4; i++)
        m->m[i].u32 = psx_sr_u32(r);

    m->m33 = (int16_t)psx_sr_u16(r);
}

static void cpu_save_gte_vec3(psx_state_writer_t* w, const gte_vec3_t* v) {
    psx_sw_i32(w, v->x);
    psx_sw_i32(w, v->y);
    psx_sw_i32(w, v->z);
}

static void cpu_load_gte_vec3(psx_state_reader_t* r, gte_vec3_t* v) {
    v->x = psx_sr_i32(r);
    v->y = psx_sr_i32(r);
    v->z = psx_sr_i32(r);
}

void psx_cpu_save_state(psx_cpu_t* cpu, psx_state_writer_t* w) {
    int i;

    for (i = 0; i < 32; i++)
        psx_sw_u32(w, cpu->r[i]);

    psx_sw_u32(w, cpu->opcode);
    psx_sw_u32(w, cpu->pc);
    psx_sw_u32(w, cpu->next_pc);
    psx_sw_u32(w, cpu->saved_pc);
    psx_sw_u32(w, cpu->hi);
    psx_sw_u32(w, cpu->lo);
    psx_sw_u32(w, cpu->load_d);
    psx_sw_u32(w, cpu->load_v);
    psx_sw_u32(w, cpu->last_cycles);
    psx_sw_u32(w, cpu->total_cycles);
    psx_sw_i32(w, cpu->branch);
    psx_sw_i32(w, cpu->delay_slot);
    psx_sw_i32(w, cpu->branch_taken);

    for (i = 0; i < 16; i++)
        psx_sw_u32(w, cpu->cop0_r[i]);

    /* COP2 data registers */
    for (i = 0; i < 3; i++)
        cpu_save_gte_vertex(w, &cpu->cop2_dr.v[i]);

    psx_sw_u32(w, cpu->cop2_dr.rgbc.rgbc);
    psx_sw_u16(w, cpu->cop2_dr.otz);

    for (i = 0; i < 4; i++)
        psx_sw_u16(w, (uint16_t)cpu->cop2_dr.ir[i]);

    for (i = 0; i < 4; i++)
        psx_sw_u32(w, cpu->cop2_dr.sxy[i].xy);

    for (i = 0; i < 4; i++)
        psx_sw_u16(w, cpu->cop2_dr.sz[i]);

    for (i = 0; i < 3; i++)
        psx_sw_u32(w, cpu->cop2_dr.rgb[i].rgbc);

    psx_sw_u32(w, cpu->cop2_dr.res1);

    for (i = 0; i < 4; i++)
        psx_sw_i32(w, cpu->cop2_dr.mac[i]);

    psx_sw_u16(w, cpu->cop2_dr.irgb);
    psx_sw_u16(w, cpu->cop2_dr.orgb);
    psx_sw_i32(w, cpu->cop2_dr.lzcs);
    psx_sw_i32(w, cpu->cop2_dr.lzcr);

    /* COP2 control registers */
    cpu_save_gte_matrix(w, &cpu->cop2_cr.rt);
    cpu_save_gte_vec3(w, &cpu->cop2_cr.tr);
    cpu_save_gte_matrix(w, &cpu->cop2_cr.l);
    cpu_save_gte_vec3(w, &cpu->cop2_cr.bk);
    cpu_save_gte_matrix(w, &cpu->cop2_cr.lr);
    cpu_save_gte_vec3(w, &cpu->cop2_cr.fc);
    psx_sw_u32(w, cpu->cop2_cr.ofx);
    psx_sw_u32(w, cpu->cop2_cr.ofy);
    psx_sw_u32(w, cpu->cop2_cr.h);
    psx_sw_u16(w, (uint16_t)cpu->cop2_cr.dqa);
    psx_sw_i32(w, cpu->cop2_cr.dqb);
    psx_sw_u16(w, (uint16_t)cpu->cop2_cr.zsf3);
    psx_sw_u16(w, (uint16_t)cpu->cop2_cr.zsf4);
    psx_sw_u32(w, cpu->cop2_cr.flag);

    /* GTE instruction-decode latches */
    psx_sw_i32(w, cpu->gte_lm);
    psx_sw_i32(w, cpu->gte_sf);
    psx_sw_i32(w, cpu->gte_mx);
    psx_sw_i32(w, cpu->gte_v);
    psx_sw_i32(w, cpu->gte_cv);
    psx_sw_i64(w, cpu->s_mac0);
    psx_sw_i64(w, cpu->s_mac3);
}

int psx_cpu_load_state(psx_cpu_t* cpu, psx_state_reader_t* r) {
    int i;

    for (i = 0; i < 32; i++)
        cpu->r[i] = psx_sr_u32(r);

    cpu->opcode = psx_sr_u32(r);
    cpu->pc = psx_sr_u32(r);
    cpu->next_pc = psx_sr_u32(r);
    cpu->saved_pc = psx_sr_u32(r);
    cpu->hi = psx_sr_u32(r);
    cpu->lo = psx_sr_u32(r);
    cpu->load_d = psx_sr_u32(r);
    cpu->load_v = psx_sr_u32(r);
    cpu->last_cycles = psx_sr_u32(r);
    cpu->total_cycles = psx_sr_u32(r);
    cpu->branch = psx_sr_i32(r);
    cpu->delay_slot = psx_sr_i32(r);
    cpu->branch_taken = psx_sr_i32(r);

    for (i = 0; i < 16; i++)
        cpu->cop0_r[i] = psx_sr_u32(r);

    for (i = 0; i < 3; i++)
        cpu_load_gte_vertex(r, &cpu->cop2_dr.v[i]);

    cpu->cop2_dr.rgbc.rgbc = psx_sr_u32(r);
    cpu->cop2_dr.otz = psx_sr_u16(r);

    for (i = 0; i < 4; i++)
        cpu->cop2_dr.ir[i] = (int16_t)psx_sr_u16(r);

    for (i = 0; i < 4; i++)
        cpu->cop2_dr.sxy[i].xy = psx_sr_u32(r);

    for (i = 0; i < 4; i++)
        cpu->cop2_dr.sz[i] = psx_sr_u16(r);

    for (i = 0; i < 3; i++)
        cpu->cop2_dr.rgb[i].rgbc = psx_sr_u32(r);

    cpu->cop2_dr.res1 = psx_sr_u32(r);

    for (i = 0; i < 4; i++)
        cpu->cop2_dr.mac[i] = psx_sr_i32(r);

    cpu->cop2_dr.irgb = psx_sr_u16(r);
    cpu->cop2_dr.orgb = psx_sr_u16(r);
    cpu->cop2_dr.lzcs = psx_sr_i32(r);
    cpu->cop2_dr.lzcr = psx_sr_i32(r);

    cpu_load_gte_matrix(r, &cpu->cop2_cr.rt);
    cpu_load_gte_vec3(r, &cpu->cop2_cr.tr);
    cpu_load_gte_matrix(r, &cpu->cop2_cr.l);
    cpu_load_gte_vec3(r, &cpu->cop2_cr.bk);
    cpu_load_gte_matrix(r, &cpu->cop2_cr.lr);
    cpu_load_gte_vec3(r, &cpu->cop2_cr.fc);
    cpu->cop2_cr.ofx = psx_sr_u32(r);
    cpu->cop2_cr.ofy = psx_sr_u32(r);
    cpu->cop2_cr.h = psx_sr_u32(r);
    cpu->cop2_cr.dqa = (int16_t)psx_sr_u16(r);
    cpu->cop2_cr.dqb = psx_sr_i32(r);
    cpu->cop2_cr.zsf3 = (int16_t)psx_sr_u16(r);
    cpu->cop2_cr.zsf4 = (int16_t)psx_sr_u16(r);
    cpu->cop2_cr.flag = psx_sr_u32(r);

    cpu->gte_lm = psx_sr_i32(r);
    cpu->gte_sf = psx_sr_i32(r);
    cpu->gte_mx = psx_sr_i32(r);
    cpu->gte_v = psx_sr_i32(r);
    cpu->gte_cv = psx_sr_i32(r);
    cpu->s_mac0 = psx_sr_i64(r);
    cpu->s_mac3 = psx_sr_i64(r);

    if (r->error)
        return PSX_STATE_ERR_TRUNCATED;

    /* R0 is hardwired to zero; never let a corrupt state break that invariant. */
    cpu->r[0] = 0;

    psx_cpu_invalidate_cache(cpu);

    return PSX_STATE_OK;
}

void psx_cpu_init(psx_cpu_t* cpu, psx_bus_t* bus) {
    struct psx_cpu_cache_t* cache = cpu->cache;
    psx_cpu_execution_mode_t execution_mode = cpu->execution_mode;

    memset(cpu, 0, sizeof(psx_cpu_t));

    cpu->cache = cache;
    cpu->execution_mode = execution_mode == PSX_CPU_INTERPRETER
        ? PSX_CPU_INTERPRETER
        : PSX_CPU_CACHED_INTERPRETER;

    psx_cpu_set_a_kcall_hook(cpu, cpu_a_kcall_hook);
    psx_cpu_set_b_kcall_hook(cpu, cpu_b_kcall_hook);

    cpu->bus = bus;
    psx_bus_set_write_observer(bus, psx_cpu_bus_write_observer, cpu);
    psx_cpu_invalidate_cache(cpu);
    cpu->pc = 0xbfc00000;
    cpu->next_pc = cpu->pc + 4;

    cpu->cop0_r[COP0_SR] = 0x10900000;
    cpu->cop0_r[COP0_PRID] = 0x00000002;
}

void psx_cpu_set_execution_mode(psx_cpu_t* cpu, psx_cpu_execution_mode_t mode) {
    if (!cpu)
        return;

    cpu->execution_mode = mode == PSX_CPU_INTERPRETER
        ? PSX_CPU_INTERPRETER
        : PSX_CPU_CACHED_INTERPRETER;
}

psx_cpu_execution_mode_t psx_cpu_get_execution_mode(const psx_cpu_t* cpu) {
    return cpu ? cpu->execution_mode : PSX_CPU_CACHED_INTERPRETER;
}

void psx_cpu_invalidate_cache(psx_cpu_t* cpu) {
    if (!cpu || !cpu->cache)
        return;

    memset(cpu->cache->entries, 0, sizeof(cpu->cache->entries));
    cpu->cache->stats.invalidations++;
}

void psx_cpu_invalidate_range(psx_cpu_t* cpu, uint32_t address, uint32_t size) {
    if (!cpu || !cpu->cache || !size)
        return;

    uint32_t first = psx_bus_physical_address(address) & ~3u;
    uint32_t last = psx_bus_physical_address(address + size - 1u) & ~3u;

    if (last < first) {
        psx_cpu_invalidate_cache(cpu);
        return;
    }

    for (uint32_t current = first;; current += 4u) {
        psx_cpu_cache_entry_t* entry = &cpu->cache->entries[psx_cpu_cache_index(current)];
        if (entry->valid && entry->address == current) {
            entry->valid = 0;
            cpu->cache->stats.invalidations++;
        }

        if (current == last)
            break;
    }
}

psx_cpu_cache_stats_t psx_cpu_get_cache_stats(const psx_cpu_t* cpu) {
    psx_cpu_cache_stats_t empty = {0, 0, 0};
    return (cpu && cpu->cache) ? cpu->cache->stats : empty;
}

static inline int psx_cpu_check_irq(psx_cpu_t* cpu) {
    return (cpu->cop0_r[COP0_SR] & SR_IEC) &&
           (cpu->cop0_r[COP0_SR] & cpu->cop0_r[COP0_CAUSE] & 0x00000700);
}

static inline void psx_cpu_exception(psx_cpu_t* cpu, uint32_t cause) {
    // Set excode and clear 3 LSBs
    cpu->cop0_r[COP0_CAUSE] &= 0xffffff80;
    cpu->cop0_r[COP0_CAUSE] |= cause;

    cpu->cop0_r[COP0_EPC] = cpu->saved_pc;

    if (cpu->delay_slot) {
        cpu->cop0_r[COP0_EPC] -= 4;
        cpu->cop0_r[COP0_CAUSE] |= 0x80000000;
    }

    // Do exception stack push
    uint32_t mode = cpu->cop0_r[COP0_SR] & 0x3f;

    cpu->cop0_r[COP0_SR] &= 0xffffffc0;
    cpu->cop0_r[COP0_SR] |= (mode << 2) & 0x3f;

    // Set PC to the vector selected on BEV
    cpu->pc = (cpu->cop0_r[COP0_SR] & SR_BEV) ? 0xbfc00180 : 0x80000080;

    cpu->next_pc = cpu->pc + 4;
}

void psx_cpu_cycle(psx_cpu_t* cpu) {
    cpu->last_cycles = 0;

    if ((cpu->pc & 0x3fffffff) == 0x000000b4)
        if (cpu->b_function_hook)
            cpu->b_function_hook(cpu);

    /* Skip BIOS. This is the single fetch/dispatch point for BOTH execution modes, so one hook
       covers the interpreter and the cached interpreter alike — placing it in either of the two
       execute functions instead would work in one mode and silently do nothing in the other.
       psx_fastboot_try() rewrites PC/SP/GP and returns non-zero when it takes over, so the rest
       of this cycle must be abandoned: the opcode fetched below would otherwise come from the
       BIOS shell we just replaced. It disarms itself, so this compare is a no-op thereafter.

       Compared as a PHYSICAL address, not a raw PC. The BIOS may enter the shell through the
       cached KSEG0 mirror (0x80030000) or the uncached KSEG1 one (0xA0030000); an equality test
       against one literal silently misses the other, and which one a given BIOS revision uses is
       not something to guess at. The b_function_hook directly above masks for the same reason. */
    if (psx_bus_physical_address(cpu->pc) == PSX_FASTBOOT_SHELL_PHYS && psx_fastboot_try(cpu))
        return;

    cpu->saved_pc = cpu->pc;
    cpu->delay_slot = cpu->branch;
    cpu->branch = 0;
    cpu->branch_taken = 0;

    if (cpu->saved_pc & 3)
        psx_cpu_exception(cpu, CAUSE_ADEL);

    cpu->opcode = psx_bus_read32(cpu->bus, cpu->pc);
    cpu->last_cycles = psx_bus_get_access_cycles(cpu->bus);

    cpu->pc = cpu->next_pc;
    cpu->next_pc += 4;

    if (psx_cpu_check_irq(cpu)) {
        // GTE instructions "win" over interrupts (fast path)
        if ((cpu->opcode & 0xfe000000) == 0x4a000000) {
            DO_PENDING_LOAD;

            PSX_PERF_INC(gte_ops);

            cpu->gte_sf = ((cpu->opcode & 0x80000) != 0) * 12;
            cpu->gte_lm = (cpu->opcode & 0x400) != 0;
            cpu->gte_cv = (cpu->opcode >> 13) & 3;
            cpu->gte_v  = (cpu->opcode >> 15) & 3;
            cpu->gte_mx = (cpu->opcode >> 17) & 3;

            switch (cpu->opcode & 0x3f) {
                case 0x01: psx_gte_i_rtps(cpu); cpu->last_cycles += 15; break;
                case 0x06: psx_gte_i_nclip(cpu); cpu->last_cycles += 8; break;
                case 0x0c: psx_gte_i_op(cpu); cpu->last_cycles += 6; break;
                case 0x10: psx_gte_i_dpcs(cpu); cpu->last_cycles += 8; break;
                case 0x11: psx_gte_i_intpl(cpu); cpu->last_cycles += 8; break;
                case 0x12: psx_gte_i_mvmva(cpu); cpu->last_cycles += 8; break;
                case 0x13: psx_gte_i_ncds(cpu); cpu->last_cycles += 19; break;
                case 0x14: psx_gte_i_cdp(cpu); cpu->last_cycles += 13; break;
                case 0x16: psx_gte_i_ncdt(cpu); cpu->last_cycles += 44; break;
                case 0x1b: psx_gte_i_nccs(cpu); cpu->last_cycles += 17; break;
                case 0x1c: psx_gte_i_cc(cpu); cpu->last_cycles += 11; break;
                case 0x1e: psx_gte_i_ncs(cpu); cpu->last_cycles += 14; break;
                case 0x20: psx_gte_i_nct(cpu); cpu->last_cycles += 30; break;
                case 0x28: psx_gte_i_sqr(cpu); cpu->last_cycles += 5; break;
                case 0x29: psx_gte_i_dcpl(cpu); cpu->last_cycles += 8; break;
                case 0x2a: psx_gte_i_dpct(cpu); cpu->last_cycles += 17; break;
                case 0x2d: psx_gte_i_avsz3(cpu); cpu->last_cycles += 5; break;
                case 0x2e: psx_gte_i_avsz4(cpu); cpu->last_cycles += 6; break;
                case 0x30: psx_gte_i_rtpt(cpu); cpu->last_cycles += 23; break;
                case 0x3d: psx_gte_i_gpf(cpu); cpu->last_cycles += 5; break;
                case 0x3e: psx_gte_i_gpl(cpu); cpu->last_cycles += 5; break;
                case 0x3f: psx_gte_i_ncct(cpu); cpu->last_cycles += 39; break;
            }
        }

        cpu->total_cycles += cpu->last_cycles;

        cpu->r[0] = 0;

        psx_cpu_exception(cpu, CAUSE_INT);

        return;
    }

    int cyc = cpu->execution_mode == PSX_CPU_INTERPRETER
        ? psx_cpu_execute(cpu)
        : psx_cpu_execute_cached(cpu);

    if (!cyc) {
        printf("psxe: Illegal instruction %08x at %08x (next=%08x, saved=%08x)\n", cpu->opcode, cpu->pc, cpu->next_pc, cpu->saved_pc);

        psx_cpu_exception(cpu, CAUSE_RI);
    }

    cpu->last_cycles += cyc;
    cpu->total_cycles += cpu->last_cycles;

    cpu->r[0] = 0;
}

void psx_cpu_set_irq_pending(psx_cpu_t* cpu) {
    cpu->cop0_r[COP0_CAUSE] |= SR_IM2;
}

static inline void psx_cpu_i_invalid(psx_cpu_t* cpu) {
    log_fatal("%08x: Illegal instruction %08x", cpu->pc - 8, cpu->opcode);

    psx_cpu_exception(cpu, CAUSE_RI);
}

// BXX
static inline void psx_cpu_i_bltz(psx_cpu_t* cpu) {
    TRACE_B("bltz");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    if ((int32_t)s < (int32_t)0)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_bgez(psx_cpu_t* cpu) {
    TRACE_B("bgez");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    if ((int32_t)s >= (int32_t)0)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_bltzal(psx_cpu_t* cpu) {
    TRACE_B("bltzal");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    R_RA = cpu->next_pc;

    if ((int32_t)s < (int32_t)0)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_bgezal(psx_cpu_t* cpu) {
    TRACE_B("bgezal");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    R_RA = cpu->next_pc;

    if ((int32_t)s >= (int32_t)0)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_j(psx_cpu_t* cpu) {
    cpu->branch = 1;

    TRACE_I26("j");

    DO_PENDING_LOAD;

    cpu->next_pc = (cpu->next_pc & 0xf0000000) | (IMM26 << 2);
}

static inline void psx_cpu_i_jal(psx_cpu_t* cpu) {
    cpu->branch = 1;

    TRACE_I26("jal");

    DO_PENDING_LOAD;

    R_RA = cpu->next_pc;

    cpu->next_pc = (cpu->next_pc & 0xf0000000) | (IMM26 << 2);
}

static inline void psx_cpu_i_beq(psx_cpu_t* cpu) {
    cpu->branch = 1;
    cpu->branch_taken = 0;

    TRACE_B("beq");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    if (s == t)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_bne(psx_cpu_t* cpu) {
    cpu->branch = 1;
    cpu->branch_taken = 0;

    TRACE_B("bne");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    if (s != t)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_blez(psx_cpu_t* cpu) {
    cpu->branch = 1;
    cpu->branch_taken = 0;

    TRACE_B("blez");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    if ((int32_t)s <= (int32_t)0)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_bgtz(psx_cpu_t* cpu) {
    cpu->branch = 1;
    cpu->branch_taken = 0;

    TRACE_B("bgtz");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    if ((int32_t)s > (int32_t)0)
        BRANCH(IMM16S << 2);
}

static inline void psx_cpu_i_addi(psx_cpu_t* cpu) {
    TRACE_I16D("addi");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    uint32_t i = IMM16S;
    uint32_t r = s + i;
    uint32_t o = (s ^ r) & (i ^ r);

    if (o & 0x80000000) {
        psx_cpu_exception(cpu, CAUSE_OV);
    } else {
        cpu->r[T] = r;
    }
}

static inline void psx_cpu_i_addiu(psx_cpu_t* cpu) {
    TRACE_I16D("addiu");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[T] = s + IMM16S;
}

static inline void psx_cpu_i_slti(psx_cpu_t* cpu) {
    TRACE_I16D("slti");

    int32_t s = (int32_t)cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[T] = s < IMM16S;
}

static inline void psx_cpu_i_sltiu(psx_cpu_t* cpu) {
    TRACE_I16D("sltiu");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[T] = s < IMM16S;
}

static inline void psx_cpu_i_andi(psx_cpu_t* cpu) {
    TRACE_I16D("andi");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[T] = s & IMM16;
}

static inline void psx_cpu_i_ori(psx_cpu_t* cpu) {
    TRACE_I16D("ori");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[T] = s | IMM16;
}

static inline void psx_cpu_i_xori(psx_cpu_t* cpu) {
    TRACE_I16D("xori");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[T] = s ^ IMM16;
}

static inline void psx_cpu_i_lui(psx_cpu_t* cpu) {
    TRACE_I16S("lui");

    DO_PENDING_LOAD;

    cpu->r[T] = IMM16 << 16;
}

static inline void psx_cpu_i_lb(psx_cpu_t* cpu) {
    TRACE_M("lb");

    uint32_t s = cpu->r[S];

    if (cpu->load_d != T)
        DO_PENDING_LOAD;

    cpu->load_d = T;
    cpu->load_v = SE8(psx_bus_read8(cpu->bus, s + IMM16S));
}

static inline void psx_cpu_i_lh(psx_cpu_t* cpu) {
    TRACE_M("lh");

    uint32_t s = cpu->r[S];

    if (cpu->load_d != T)
        DO_PENDING_LOAD;

    uint32_t addr = s + IMM16S;

    if (addr & 0x1) {
        psx_cpu_exception(cpu, CAUSE_ADEL);
    } else {
        cpu->load_d = T;
        cpu->load_v = SE16(psx_bus_read16(cpu->bus, addr));
    }
}

static inline void psx_cpu_i_lwl(psx_cpu_t* cpu) {
    TRACE_M("lwl");

    uint32_t rt = T;
    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[rt];

    uint32_t addr = s + IMM16S;
    uint32_t load = psx_bus_read32(cpu->bus, addr & 0xfffffffc);

    if (rt == cpu->load_d) {
        t = cpu->load_v;
    } else {
        DO_PENDING_LOAD;
    }

    int shift = (int)((addr & 0x3) << 3);
    uint32_t mask = (uint32_t)0x00FFFFFF >> shift;
    uint32_t value = (t & mask) | (load << (24 - shift)); 

    cpu->load_d = rt;
    cpu->load_v = value;

    // printf("lwl rt=%u s=%08x t=%08x addr=%08x load=%08x (%08x) shift=%u mask=%08x value=%08x\n",
    //     rt, s, t, addr, load, addr & 0xfffffffc, shift, mask, value
    // );
}

static inline void psx_cpu_i_lw(psx_cpu_t* cpu) {
    TRACE_M("lw");

    uint32_t s = cpu->r[S];
    uint32_t addr = s + IMM16S;

    if (cpu->load_d != T)
        DO_PENDING_LOAD;

    if (addr & 0x3) {
        psx_cpu_exception(cpu, CAUSE_ADEL);
    } else {
        cpu->load_d = T;
        cpu->load_v = psx_bus_read32(cpu->bus, addr);
    }
}

static inline void psx_cpu_i_lbu(psx_cpu_t* cpu) {
    TRACE_M("lbu");

    uint32_t s = cpu->r[S];

    if (cpu->load_d != T)
        DO_PENDING_LOAD;

    cpu->load_d = T;
    cpu->load_v = psx_bus_read8(cpu->bus, s + IMM16S);
}

static inline void psx_cpu_i_lhu(psx_cpu_t* cpu) {
    TRACE_M("lhu");

    uint32_t s = cpu->r[S];
    uint32_t addr = s + IMM16S;

    if (cpu->load_d != T)
        DO_PENDING_LOAD;

    if (addr & 0x1) {
        psx_cpu_exception(cpu, CAUSE_ADEL);
    } else {
        cpu->load_d = T;
        cpu->load_v = psx_bus_read16(cpu->bus, addr);
    }
}

static inline void psx_cpu_i_lwr(psx_cpu_t* cpu) {
    TRACE_M("lwr");

    uint32_t rt = T;
    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[rt];

    uint32_t addr = s + IMM16S;
    uint32_t load = psx_bus_read32(cpu->bus, addr & 0xfffffffc);

    if (rt == cpu->load_d) {
        t = cpu->load_v;
    } else {
        DO_PENDING_LOAD;
    }

    int shift = (int)((addr & 0x3) << 3);
    uint32_t mask = 0xFFFFFF00 << (24 - shift);
    uint32_t value = (t & mask) | (load >> shift); 

    cpu->load_d = rt;
    cpu->load_v = value;

    // printf("lwr rt=%u s=%08x t=%08x addr=%08x load=%08x (%08x) shift=%u mask=%08x value=%08x\n",
    //     rt, s, t, addr, load, addr & 0xfffffffc, shift, mask, value
    // );
}

static inline void psx_cpu_i_sb(psx_cpu_t* cpu) {
    TRACE_M("sb");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    // Cache isolated
    if (cpu->cop0_r[COP0_SR] & SR_ISC) {
        return;
    }

    psx_bus_write8(cpu->bus, s + IMM16S, t);
}

static inline void psx_cpu_i_sh(psx_cpu_t* cpu) {
    TRACE_M("sh");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];
    uint32_t addr = s + IMM16S;

    DO_PENDING_LOAD;

    // Cache isolated
    if (cpu->cop0_r[COP0_SR] & SR_ISC) {
        return;
    }

    if (addr & 0x1) {
        psx_cpu_exception(cpu, CAUSE_ADES);
    } else {
        psx_bus_write16(cpu->bus, addr, t);
    }
}

static inline void psx_cpu_i_swl(psx_cpu_t* cpu) {
    TRACE_M("swl");

    /* BOTH source registers are sampled before the pending load is committed. Reading
       cpu->r[T] after DO_PENDING_LOAD made an SWL sitting in a load delay slot store the
       value the previous load is about to deliver, where the R3000A stores the OLD one --
       every other store in this file samples rt first, these two did not. */
    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    // Cache isolated
    if (cpu->cop0_r[COP0_SR] & SR_ISC) {
        return;
    }

    uint32_t addr = s + IMM16S;
    uint32_t aligned = addr & 0xfffffffc;
    uint32_t v = psx_bus_read32(cpu->bus, aligned);

    switch (addr & 0x3) {
        case 0: v = (v & 0xffffff00) | (t >> 24); break;
        case 1: v = (v & 0xffff0000) | (t >> 16); break;
        case 2: v = (v & 0xff000000) | (t >> 8 ); break;
        case 3: v =                     t       ; break;
    }

    psx_bus_write32(cpu->bus, aligned, v);
}

static inline void psx_cpu_i_sw(psx_cpu_t* cpu) {
    TRACE_M("sw");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];
    uint32_t addr = s + IMM16S;

    DO_PENDING_LOAD;

    // Cache isolated
    if (cpu->cop0_r[COP0_SR] & SR_ISC) {
        return;
    }

    if (addr & 0x3) {
        psx_cpu_exception(cpu, CAUSE_ADES);
    } else {
        /* PGXP: attach precision when rt still holds a tracked SXY word, and
           invalidate any stale entry this store overwrites either way. */
        if (psx_pgxp_active())
            psx_pgxp_cpu_sw(addr, t, T);

        psx_bus_write32(cpu->bus, addr, t);
    }
}

static inline void psx_cpu_i_swr(psx_cpu_t* cpu) {
    TRACE_M("swr");

    /* See psx_cpu_i_swl: rt is sampled before the pending load, and an isolated cache
       swallows this store like every other one. */
    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    // Cache isolated
    if (cpu->cop0_r[COP0_SR] & SR_ISC) {
        return;
    }

    uint32_t addr = s + IMM16S;
    uint32_t aligned = addr & 0xfffffffc;
    uint32_t v = psx_bus_read32(cpu->bus, aligned);

    switch (addr & 0x3) {
        case 0: v =                     t       ; break;
        case 1: v = (v & 0x000000ff) | (t << 8 ); break;
        case 2: v = (v & 0x0000ffff) | (t << 16); break;
        case 3: v = (v & 0x00ffffff) | (t << 24); break;
    }

    psx_bus_write32(cpu->bus, aligned, v);
}

static inline void psx_cpu_i_lwc0(psx_cpu_t* cpu) {
    psx_cpu_exception(cpu, CAUSE_CPU);
}

static inline void psx_cpu_i_lwc1(psx_cpu_t* cpu) {
    psx_cpu_exception(cpu, CAUSE_CPU);
}

static inline void psx_cpu_i_lwc3(psx_cpu_t* cpu) {
    psx_cpu_exception(cpu, CAUSE_CPU);
}

static inline void psx_cpu_i_swc0(psx_cpu_t* cpu) {
    psx_cpu_exception(cpu, CAUSE_CPU);
}

static inline void psx_cpu_i_swc1(psx_cpu_t* cpu) {
    psx_cpu_exception(cpu, CAUSE_CPU);
}

static inline void psx_cpu_i_swc3(psx_cpu_t* cpu) {
    psx_cpu_exception(cpu, CAUSE_CPU);
}

// Secondary
static inline void psx_cpu_i_sll(psx_cpu_t* cpu) {
    TRACE_I5D("sll");

    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = t << IMM5;
}

static inline void psx_cpu_i_srl(psx_cpu_t* cpu) {
    TRACE_I5D("srl");

    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = t >> IMM5;
}

static inline void psx_cpu_i_sra(psx_cpu_t* cpu) {
    TRACE_I5D("sra");

    int32_t t = (int32_t)cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = t >> IMM5;
}

static inline void psx_cpu_i_sllv(psx_cpu_t* cpu) {
    TRACE_RT("sllv");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = t << (s & 0x1f);
}

static inline void psx_cpu_i_srlv(psx_cpu_t* cpu) {
    TRACE_RT("srlv");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = t >> (s & 0x1f);
}

static inline void psx_cpu_i_srav(psx_cpu_t* cpu) {
    TRACE_RT("srav");

    uint32_t s = cpu->r[S];
    int32_t t = (int32_t)cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = t >> (s & 0x1f);
}

static inline void psx_cpu_i_jr(psx_cpu_t* cpu) {
    cpu->branch = 1;

    TRACE_RS("jr");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->next_pc = s;
}

static inline void psx_cpu_i_jalr(psx_cpu_t* cpu) {
    cpu->branch = 1;

    TRACE_RD("jalr");

    uint32_t s = cpu->r[S];

    DO_PENDING_LOAD;

    cpu->r[D] = cpu->next_pc;

    cpu->next_pc = s;
}

static inline void psx_cpu_i_syscall(psx_cpu_t* cpu) {
    TRACE_I20("syscall");
    
    DO_PENDING_LOAD;

    psx_cpu_exception(cpu, CAUSE_SYSCALL);
}

static inline void psx_cpu_i_break(psx_cpu_t* cpu) {
    TRACE_I20("break");

    DO_PENDING_LOAD;

    psx_cpu_exception(cpu, CAUSE_BP);
}

static inline void psx_cpu_i_mfhi(psx_cpu_t* cpu) {
    TRACE_MTF("mfhi");

    DO_PENDING_LOAD;

    cpu->r[D] = cpu->hi;
}

static inline void psx_cpu_i_mthi(psx_cpu_t* cpu) {
    TRACE_MTF("mthi");

    DO_PENDING_LOAD;

    cpu->hi = cpu->r[S];
}

static inline void psx_cpu_i_mflo(psx_cpu_t* cpu) {
    TRACE_MTF("mflo");

    DO_PENDING_LOAD;

    cpu->r[D] = cpu->lo;
}

static inline void psx_cpu_i_mtlo(psx_cpu_t* cpu) {
    TRACE_MTF("mtlo");

    DO_PENDING_LOAD;

    cpu->lo = cpu->r[S];
}

static inline void psx_cpu_i_mult(psx_cpu_t* cpu) {
    TRACE_MD("mult");

    int64_t s = (int64_t)((int32_t)cpu->r[S]);
    int64_t t = (int64_t)((int32_t)cpu->r[T]);

    DO_PENDING_LOAD;

    uint64_t r = s * t;

    cpu->hi = r >> 32;
    cpu->lo = r & 0xffffffff;
}

static inline void psx_cpu_i_multu(psx_cpu_t* cpu) {
    TRACE_MD("multu");

    uint64_t s = (uint64_t)cpu->r[S];
    uint64_t t = (uint64_t)cpu->r[T];

    DO_PENDING_LOAD;

    uint64_t r = s * t;

    cpu->hi = r >> 32;
    cpu->lo = r & 0xffffffff;
}

static inline void psx_cpu_i_div(psx_cpu_t* cpu) {
    TRACE_MD("div");

    int32_t s = (int32_t)cpu->r[S];
    int32_t t = (int32_t)cpu->r[T];

    DO_PENDING_LOAD;

    if (!t) {
        cpu->hi = s;
        cpu->lo = (s >= 0) ? 0xffffffff : 1;
    } else if ((((uint32_t)s) == 0x80000000) && (t == -1)) {
        cpu->hi = 0;
        cpu->lo = 0x80000000;
    } else {
        cpu->hi = (uint32_t)(s % t);
        cpu->lo = (uint32_t)(s / t);
    }
}

static inline void psx_cpu_i_divu(psx_cpu_t* cpu) {
    TRACE_MD("divu");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    if (!t) {
        cpu->hi = s;
        cpu->lo = 0xffffffff;
    } else {
        cpu->hi = s % t;
        cpu->lo = s / t;
    }
}

static inline void psx_cpu_i_add(psx_cpu_t* cpu) {
    TRACE_RT("add");

    int32_t s = cpu->r[S];
    int32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    int32_t r = s + t;
    uint32_t o = (s ^ r) & (t ^ r);

    if (o & 0x80000000) {
        psx_cpu_exception(cpu, CAUSE_OV);
    } else {
        cpu->r[D] = (uint32_t)r;
    }
}

static inline void psx_cpu_i_addu(psx_cpu_t* cpu) {
    TRACE_RT("addu");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = s + t;
}

static inline void psx_cpu_i_sub(psx_cpu_t* cpu) {
    TRACE_RT("sub");

    int32_t s = (int32_t)cpu->r[S];
    int32_t t = (int32_t)cpu->r[T];
    int32_t r;

    DO_PENDING_LOAD;

    int o = __builtin_ssub_overflow(s, t, &r);

    if (o) {
        psx_cpu_exception(cpu, CAUSE_OV);
    } else {
        cpu->r[D] = r;
    }
}

static inline void psx_cpu_i_subu(psx_cpu_t* cpu) {
    TRACE_RT("subu");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = s - t;
}

static inline void psx_cpu_i_and(psx_cpu_t* cpu) {
    TRACE_RT("and");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = s & t;
}

static inline void psx_cpu_i_or(psx_cpu_t* cpu) {
    TRACE_RT("or");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = s | t;
}

static inline void psx_cpu_i_xor(psx_cpu_t* cpu) {
    TRACE_RT("xor");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = (s ^ t);
}

static inline void psx_cpu_i_nor(psx_cpu_t* cpu) {
    TRACE_RT("nor");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = ~(s | t);
}

static inline void psx_cpu_i_slt(psx_cpu_t* cpu) {
    TRACE_RT("slt");

    int32_t s = (int32_t)cpu->r[S];
    int32_t t = (int32_t)cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = s < t;
}

static inline void psx_cpu_i_sltu(psx_cpu_t* cpu) {
    TRACE_RT("sltu");

    uint32_t s = cpu->r[S];
    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->r[D] = s < t;
}

// COP0
static inline void psx_cpu_i_mfc0(psx_cpu_t* cpu) {
    TRACE_C0M("mfc0");

    DO_PENDING_LOAD;

    cpu->load_v = cpu->cop0_r[D];
    cpu->load_d = T;
}

static inline void psx_cpu_i_mtc0(psx_cpu_t* cpu) {
    TRACE_C0M("mtc0");

    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    cpu->cop0_r[D] = t & g_psx_cpu_cop0_write_mask_table[D];
}

static inline void psx_cpu_i_rfe(psx_cpu_t* cpu) {
    TRACE_N("rfe");

    DO_PENDING_LOAD;

    uint32_t mode = cpu->cop0_r[COP0_SR] & 0x3f;

    cpu->cop0_r[COP0_SR] &= 0xfffffff0;
    cpu->cop0_r[COP0_SR] |= mode >> 2;
}

// COP2
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define CLAMP(v, a, b) (((v) < (a)) ? (a) : (((v) > (b)) ? (b) : (v)))

static inline void gte_handle_irgb_write(psx_cpu_t* cpu) {
    cpu->cop2_dr.ir[1] = ((cpu->cop2_dr.irgb >> 0) & 0x1f) * 0x80;
    cpu->cop2_dr.ir[2] = ((cpu->cop2_dr.irgb >> 5) & 0x1f) * 0x80;
    cpu->cop2_dr.ir[3] = ((cpu->cop2_dr.irgb >> 10) & 0x1f) * 0x80;
}

static inline void gte_handle_irgb_read(psx_cpu_t* cpu) {
    int r = CLAMP(cpu->cop2_dr.ir[1] >> 7, 0x00, 0x1f);
    int g = CLAMP(cpu->cop2_dr.ir[2] >> 7, 0x00, 0x1f);
    int b = CLAMP(cpu->cop2_dr.ir[3] >> 7, 0x00, 0x1f);

    cpu->cop2_dr.irgb = r | (g << 5) | (b << 10);
}

static inline void gte_handle_sxyp_write(psx_cpu_t* cpu) {
    cpu->cop2_dr.sxy[0] = cpu->cop2_dr.sxy[1];
    cpu->cop2_dr.sxy[1] = cpu->cop2_dr.sxy[2];
    cpu->cop2_dr.sxy[2] = cpu->cop2_dr.sxy[3];
}

static inline void gte_handle_lzcs_write(psx_cpu_t* cpu) {
    if ((cpu->cop2_dr.lzcs == 0xffffffff) || !cpu->cop2_dr.lzcs) {
        cpu->cop2_dr.lzcr = 32;

        return;
    }

    int b = (cpu->cop2_dr.lzcs >> 31) & 1;

    cpu->cop2_dr.lzcr = __builtin_clz(b ? ~cpu->cop2_dr.lzcs : cpu->cop2_dr.lzcs);
}

uint32_t gte_read_register(psx_cpu_t* cpu, uint32_t r) {
    switch (r) {
        case 0 : return cpu->cop2_dr.v[0].xy;
        case 1 : return (int32_t)cpu->cop2_dr.v[0].z;
        case 2 : return cpu->cop2_dr.v[1].xy;
        case 3 : return (int32_t)cpu->cop2_dr.v[1].z;
        case 4 : return cpu->cop2_dr.v[2].xy;
        case 5 : return (int32_t)cpu->cop2_dr.v[2].z;
        case 6 : return cpu->cop2_dr.rgbc.rgbc;
        case 7 : return cpu->cop2_dr.otz;
        case 8 : return (int32_t)cpu->cop2_dr.ir[0];
        case 9 : return (int32_t)cpu->cop2_dr.ir[1];
        case 10: return (int32_t)cpu->cop2_dr.ir[2];
        case 11: return (int32_t)cpu->cop2_dr.ir[3];
        case 12: return cpu->cop2_dr.sxy[0].xy;
        case 13: return cpu->cop2_dr.sxy[1].xy;
        case 14: return cpu->cop2_dr.sxy[2].xy;
        case 15: return cpu->cop2_dr.sxy[2].xy; // SXY2 Mirror
        case 16: return cpu->cop2_dr.sz[0];
        case 17: return cpu->cop2_dr.sz[1];
        case 18: return cpu->cop2_dr.sz[2];
        case 19: return cpu->cop2_dr.sz[3];
        case 20: return cpu->cop2_dr.rgb[0].rgbc;
        case 21: return cpu->cop2_dr.rgb[1].rgbc;
        case 22: return cpu->cop2_dr.rgb[2].rgbc;
        case 23: return cpu->cop2_dr.res1;
        case 24: return cpu->cop2_dr.mac[0];
        case 25: return cpu->cop2_dr.mac[1];
        case 26: return cpu->cop2_dr.mac[2];
        case 27: return cpu->cop2_dr.mac[3];
        case 28: gte_handle_irgb_read(cpu); return cpu->cop2_dr.irgb;
        /* ORGB is a read-only MIRROR of the IRGB conversion — psx-spx: reading it performs the
           IR1-3 -> 5:5:5 conversion, same as reading IRGB. Returning the stored word handed
           back whatever was last WRITTEN to IRGB (or last read through reg 28), i.e. a stale
           value whenever IR1-3 changed in between — a readback lying about live state, the
           same defect class as the GPUINFO(5) echo. Gate: gte-regfile in tests/gte_matrix.c. */
        case 29: gte_handle_irgb_read(cpu); return cpu->cop2_dr.irgb;
        case 30: return cpu->cop2_dr.lzcs;
        case 31: return cpu->cop2_dr.lzcr;
        case 32: return cpu->cop2_cr.rt.m[0].u32;
        case 33: return cpu->cop2_cr.rt.m[1].u32;
        case 34: return cpu->cop2_cr.rt.m[2].u32;
        case 35: return cpu->cop2_cr.rt.m[3].u32;
        case 36: return (int32_t)cpu->cop2_cr.rt.m33;
        case 37: return cpu->cop2_cr.tr.x;
        case 38: return cpu->cop2_cr.tr.y;
        case 39: return cpu->cop2_cr.tr.z;
        case 40: return cpu->cop2_cr.l.m[0].u32;
        case 41: return cpu->cop2_cr.l.m[1].u32;
        case 42: return cpu->cop2_cr.l.m[2].u32;
        case 43: return cpu->cop2_cr.l.m[3].u32;
        case 44: return (int32_t)cpu->cop2_cr.l.m33;
        case 45: return cpu->cop2_cr.bk.x;
        case 46: return cpu->cop2_cr.bk.y;
        case 47: return cpu->cop2_cr.bk.z;
        case 48: return cpu->cop2_cr.lr.m[0].u32;
        case 49: return cpu->cop2_cr.lr.m[1].u32;
        case 50: return cpu->cop2_cr.lr.m[2].u32;
        case 51: return cpu->cop2_cr.lr.m[3].u32;
        case 52: return (int32_t)cpu->cop2_cr.lr.m33;
        case 53: return cpu->cop2_cr.fc.x;
        case 54: return cpu->cop2_cr.fc.y;
        case 55: return cpu->cop2_cr.fc.z;
        case 56: return cpu->cop2_cr.ofx;
        case 57: return cpu->cop2_cr.ofy;
        case 58: return (int32_t)(int16_t)cpu->cop2_cr.h;
        case 59: return cpu->cop2_cr.dqa;
        case 60: return cpu->cop2_cr.dqb;
        case 61: return cpu->cop2_cr.zsf3;
        case 62: return cpu->cop2_cr.zsf4;
        case 63: return (cpu->cop2_cr.flag & 0x7ffff000) | 
                        (((cpu->cop2_cr.flag & 0x7f87e000) != 0) << 31);
    }

    return 0x00000000;
}

static inline void gte_write_register(psx_cpu_t* cpu, uint32_t r, uint32_t value) {
    /* PGXP: direct SXY writes (MTC2/LWC2, and the reg-15 FIFO push) must keep
       the precise-shadow FIFO in step — see psx/pgxp.c. */
    if (psx_pgxp_active())
        psx_pgxp_gte_reg_write(r, value);

    switch (r) {
        case 0 : cpu->cop2_dr.v[0].xy = value; break;
        case 1 : cpu->cop2_dr.v[0].z = value; break;
        case 2 : cpu->cop2_dr.v[1].xy = value; break;
        case 3 : cpu->cop2_dr.v[1].z = value; break;
        case 4 : cpu->cop2_dr.v[2].xy = value; break;
        case 5 : cpu->cop2_dr.v[2].z = value; break;
        case 6 : cpu->cop2_dr.rgbc.rgbc = value; break;
        case 7 : cpu->cop2_dr.otz = value; break;
        case 8 : cpu->cop2_dr.ir[0] = value; break;
        case 9 : cpu->cop2_dr.ir[1] = value; break;
        case 10: cpu->cop2_dr.ir[2] = value; break;
        case 11: cpu->cop2_dr.ir[3] = value; break;
        case 12: cpu->cop2_dr.sxy[0].xy = value; break;
        case 13: cpu->cop2_dr.sxy[1].xy = value; break;
        case 14: cpu->cop2_dr.sxy[2].xy = value; break;
        case 15: cpu->cop2_dr.sxy[3].xy = value; gte_handle_sxyp_write(cpu); break;
        case 16: cpu->cop2_dr.sz[0] = value; break;
        case 17: cpu->cop2_dr.sz[1] = value; break;
        case 18: cpu->cop2_dr.sz[2] = value; break;
        case 19: cpu->cop2_dr.sz[3] = value; break;
        case 20: cpu->cop2_dr.rgb[0].rgbc = value; break;
        case 21: cpu->cop2_dr.rgb[1].rgbc = value; break;
        case 22: cpu->cop2_dr.rgb[2].rgbc = value; break;
        case 23: cpu->cop2_dr.res1 = value; break;
        case 24: cpu->cop2_dr.mac[0] = value; break;
        case 25: cpu->cop2_dr.mac[1] = value; break;
        case 26: cpu->cop2_dr.mac[2] = value; break;
        case 27: cpu->cop2_dr.mac[3] = value; break;
        case 28: cpu->cop2_dr.irgb = value & 0x7fff; gte_handle_irgb_write(cpu); break;
        case 29: /* ORGB RO */ break;
        case 30: cpu->cop2_dr.lzcs = value; gte_handle_lzcs_write(cpu); break;
        case 31: /* LZCR RO */ break;
        case 32: cpu->cop2_cr.rt.m[0].u32 = value; break;
        case 33: cpu->cop2_cr.rt.m[1].u32 = value; break;
        case 34: cpu->cop2_cr.rt.m[2].u32 = value; break;
        case 35: cpu->cop2_cr.rt.m[3].u32 = value; break;
        case 36: cpu->cop2_cr.rt.m33 = value; break;
        case 37: cpu->cop2_cr.tr.x = value; break;
        case 38: cpu->cop2_cr.tr.y = value; break;
        case 39: cpu->cop2_cr.tr.z = value; break;
        case 40: cpu->cop2_cr.l.m[0].u32 = value; break;
        case 41: cpu->cop2_cr.l.m[1].u32 = value; break;
        case 42: cpu->cop2_cr.l.m[2].u32 = value; break;
        case 43: cpu->cop2_cr.l.m[3].u32 = value; break;
        case 44: cpu->cop2_cr.l.m33 = value; break;
        case 45: cpu->cop2_cr.bk.x = value; break;
        case 46: cpu->cop2_cr.bk.y = value; break;
        case 47: cpu->cop2_cr.bk.z = value; break;
        case 48: cpu->cop2_cr.lr.m[0].u32 = value; break;
        case 49: cpu->cop2_cr.lr.m[1].u32 = value; break;
        case 50: cpu->cop2_cr.lr.m[2].u32 = value; break;
        case 51: cpu->cop2_cr.lr.m[3].u32 = value; break;
        case 52: cpu->cop2_cr.lr.m33 = value; break;
        case 53: cpu->cop2_cr.fc.x = value; break;
        case 54: cpu->cop2_cr.fc.y = value; break;
        case 55: cpu->cop2_cr.fc.z = value; break;
        case 56: cpu->cop2_cr.ofx = value; break;
        case 57: cpu->cop2_cr.ofy = value; break;
        case 58: cpu->cop2_cr.h = value; break;
        case 59: cpu->cop2_cr.dqa = value; break;
        case 60: cpu->cop2_cr.dqb = value; break;
        case 61: cpu->cop2_cr.zsf3 = value; break;
        case 62: cpu->cop2_cr.zsf4 = value; break;
        case 63: cpu->cop2_cr.flag = value & 0x7ffff000; break;
    }
}

static inline void psx_cpu_i_lwc2(psx_cpu_t* cpu) {
    uint32_t s = cpu->r[S];
    uint32_t addr = s + IMM16S;

    DO_PENDING_LOAD;

    if (addr & 0x3) {
        psx_cpu_exception(cpu, CAUSE_ADEL);
    } else {
        gte_write_register(cpu, T, psx_bus_read32(cpu->bus, addr));
    }
}

static inline void psx_cpu_i_swc2(psx_cpu_t* cpu) {
    uint32_t s = cpu->r[S];
    uint32_t addr = s + IMM16S;

    DO_PENDING_LOAD;

    // Cache isolated
    if (cpu->cop0_r[COP0_SR] & SR_ISC) {
        return;
    }

    if (addr & 0x3) {
        psx_cpu_exception(cpu, CAUSE_ADES);
    } else {
        uint32_t value = gte_read_register(cpu, T);

        /* PGXP: an SXY store is the main road a precise vertex takes into RAM. */
        if (psx_pgxp_active())
            psx_pgxp_cpu_swc2(addr, value, T);

        psx_bus_write32(cpu->bus, addr, value);
    }
}

static inline void psx_cpu_i_mfc2(psx_cpu_t* cpu) {
    TRACE_C2M("mfc2");

    DO_PENDING_LOAD;

    cpu->load_v = gte_read_register(cpu, D);
    cpu->load_d = T;

    /* PGXP: remember which CPU register now holds an SXY word so a later plain
       SW can carry the precision (validated against the stored value there). */
    if (psx_pgxp_active())
        psx_pgxp_cpu_mfc2(T, cpu->load_v, D);
}

static inline void psx_cpu_i_cfc2(psx_cpu_t* cpu) {
    TRACE_C2MC("cfc2");

    DO_PENDING_LOAD;

    cpu->load_v = gte_read_register(cpu, D + 32);
    cpu->load_d = T;
}

static inline void psx_cpu_i_mtc2(psx_cpu_t* cpu) {
    TRACE_C2M("mtc2");

    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    gte_write_register(cpu, D, t);
}

static inline void psx_cpu_i_ctc2(psx_cpu_t* cpu) {
    TRACE_C2MC("ctc2");

    uint32_t t = cpu->r[T];

    DO_PENDING_LOAD;

    gte_write_register(cpu, D + 32, t);
}

#define R_FLAG cpu->cop2_cr.flag

static inline int64_t gte_clamp_mac0(psx_cpu_t* cpu, int64_t value) {
    cpu->s_mac0 = value;

    if (value < (-0x80000000ll)) {
        R_FLAG |= 0x8000;
    } else if (value > (0x7fffffffll)) {
        R_FLAG |= 0x10000;
    }

    return value;
}

static inline int32_t gte_clamp_mac(psx_cpu_t* cpu, int i, int64_t value) {
    if (i == 3)
        cpu->s_mac3 = value;

    if (value < -0x80000000000ll) {
        R_FLAG |= 0x8000000 >> (i - 1);
    } else if (value > 0x7ffffffffffll) {
        R_FLAG |= 0x40000000 >> (i - 1);
    }

    return (int32_t)(((value << 20) >> 20) >> cpu->gte_sf);
}

static inline int64_t gte_check_mac(psx_cpu_t* cpu, int i, int64_t value) {
    if (value < -0x80000000000ll) {
        R_FLAG |= 0x8000000 >> (i - 1);
    } else if (value > 0x7ffffffffffll) {
        R_FLAG |= 0x40000000 >> (i - 1);
    }

    return (value << 20) >> 20;
}

static inline int32_t gte_clamp_ir0(psx_cpu_t* cpu, int32_t value) {
    if (value < 0) {
        R_FLAG |= 0x1000;

        return 0;
    } else if (value > 0x1000) {
        R_FLAG |= 0x1000;

        return 0x1000;
    }

    return value;
}

static inline int64_t gte_clamp_sxy(psx_cpu_t* cpu, int i, int64_t value) {
    if (value < -0x400) {
        R_FLAG |= (uint32_t)(0x4000 >> (i - 1));

        return -0x400;
    } else if (value > 0x3ff) {
        R_FLAG |= (uint32_t)(0x4000 >> (i - 1));

        return 0x3ff;
    }

    return value;
}

static inline int32_t gte_clamp_sz3(psx_cpu_t* cpu, int32_t value) {
    if (value < 0) {
        R_FLAG |= 0x40000;

        return 0;
    } else if (value > 0xffff) {
        R_FLAG |= 0x40000;

        return 0xffff;
    }

    return value;
}

static inline uint8_t gte_clamp_rgb(psx_cpu_t* cpu, int i, int value) {
    if (value < 0) {
        R_FLAG |= (uint32_t)0x200000 >> (i - 1);

        return 0;
    } else if (value > 0xff) {
        R_FLAG |= (uint32_t)0x200000 >> (i - 1);

        return 0xff;
    }

    return (uint8_t)value;
}

static inline int32_t gte_clamp_ir(psx_cpu_t* cpu, int i, int64_t value, int lm) {
    if (lm && (value < 0)) {
        R_FLAG |= (uint32_t)(0x1000000 >> (i - 1));

        return 0;
    } else if ((value < -0x8000) && !lm) {
        R_FLAG |= (uint32_t)(0x1000000 >> (i - 1));

        return -0x8000;
    } else if (value > 0x7fff) {
        R_FLAG |= (uint32_t)(0x1000000 >> (i - 1));

        return 0x7fff;
    }

    return (int32_t)value;
}

static inline int32_t gte_clamp_ir_z(psx_cpu_t* cpu, int64_t value, int sf, int lm) {
    int32_t value_sf = value >> sf;
    int32_t value_12 = value >> 12;
    int32_t min = 0;

    if (lm == 0)
        min = -((int32_t)0x8000);

    if (value_12 < (-((int32_t)0x8000)) || value_12 > 0x7fffl)
        R_FLAG |= (1 << 22);

    return (int32_t)CLAMP(value_sf, min, 0x7fffl);
}

static inline int clz(uint32_t value) {
    if (!value)
        return 32;
    
    return __builtin_clz(value);
}

static inline uint32_t gte_divide(psx_cpu_t* cpu, uint16_t n, uint16_t d) {
    // Overflow
    if (n >= d * 2) {
        R_FLAG |= (1 << 31) | (1 << 17);

        return 0x1ffff;
    }

    int shift = clz(d) - 16;

    int r1 = (d << shift) & 0x7fff;
    int r2 = g_psx_gte_unr_table[((r1 + 0x40) >> 7)] + 0x101;
    int r3 = ((0x80 - (r2 * (r1 + 0x8000))) >> 8) & 0x1ffff;

    uint32_t reciprocal = ((r2 * r3) + 0x80) >> 8;
    uint32_t res = ((((uint64_t)reciprocal * (n << shift)) + 0x8000) >> 16);

    return MIN(0x1ffff, res);
}

static inline void psx_gte_i_invalid(psx_cpu_t* cpu) {
    log_fatal("invalid: Unimplemented GTE instruction %02x, %02x", cpu->opcode & 0x3f, cpu->opcode >> 25);
}

#define I64(v) ((int64_t)v)
#define R_TRX cpu->cop2_cr.tr.x
#define R_TRY cpu->cop2_cr.tr.y
#define R_TRZ cpu->cop2_cr.tr.z
#define R_RT11 cpu->cop2_cr.rt.m[0].c[0]
#define R_RT11 cpu->cop2_cr.rt.m[0].c[0]
#define R_RT12 cpu->cop2_cr.rt.m[0].c[1]
#define R_RT13 cpu->cop2_cr.rt.m[1].c[0]
#define R_RT21 cpu->cop2_cr.rt.m[1].c[1]
#define R_RT22 cpu->cop2_cr.rt.m[2].c[0]
#define R_RT23 cpu->cop2_cr.rt.m[2].c[1]
#define R_RT31 cpu->cop2_cr.rt.m[3].c[0]
#define R_RT32 cpu->cop2_cr.rt.m[3].c[1]
#define R_RT33 cpu->cop2_cr.rt.m33
#define R_MAC0 cpu->cop2_dr.mac[0]
#define R_MAC1 cpu->cop2_dr.mac[1]
#define R_MAC2 cpu->cop2_dr.mac[2]
#define R_MAC3 cpu->cop2_dr.mac[3]
#define R_OFX cpu->cop2_cr.ofx
#define R_OFY cpu->cop2_cr.ofy
#define R_IR0 cpu->cop2_dr.ir[0]
#define R_IR1 cpu->cop2_dr.ir[1]
#define R_IR2 cpu->cop2_dr.ir[2]
#define R_IR3 cpu->cop2_dr.ir[3]
#define R_SXY0 cpu->cop2_dr.sxy[0].xy
#define R_SX0 cpu->cop2_dr.sxy[0].p[0]
#define R_SY0 cpu->cop2_dr.sxy[0].p[1]
#define R_SZ0 cpu->cop2_dr.sz[0]
#define R_SXY1 cpu->cop2_dr.sxy[1].xy
#define R_SX1 cpu->cop2_dr.sxy[1].p[0]
#define R_SY1 cpu->cop2_dr.sxy[1].p[1]
#define R_SZ1 cpu->cop2_dr.sz[1]
#define R_SXY2 cpu->cop2_dr.sxy[2].xy
#define R_SX2 cpu->cop2_dr.sxy[2].p[0]
#define R_SY2 cpu->cop2_dr.sxy[2].p[1]
#define R_SZ2 cpu->cop2_dr.sz[2]
#define R_SZ3 cpu->cop2_dr.sz[3]
#define R_DQA cpu->cop2_cr.dqa
#define R_DQB cpu->cop2_cr.dqb
#define R_ZSF3 cpu->cop2_cr.zsf3
#define R_ZSF4 cpu->cop2_cr.zsf4
#define R_OTZ cpu->cop2_dr.otz
#define R_H cpu->cop2_cr.h
#define R_RC cpu->cop2_dr.rgbc.c[0]
#define R_GC cpu->cop2_dr.rgbc.c[1]
#define R_BC cpu->cop2_dr.rgbc.c[2]
#define R_CODE cpu->cop2_dr.rgbc.c[3]
#define R_RGBC cpu->cop2_dr.rgbc.rgbc
#define R_RFC cpu->cop2_cr.fc.x
#define R_GFC cpu->cop2_cr.fc.y
#define R_BFC cpu->cop2_cr.fc.z
#define R_RGB0 cpu->cop2_dr.rgb[0].rgbc
#define R_RGB1 cpu->cop2_dr.rgb[1].rgbc
#define R_RGB2 cpu->cop2_dr.rgb[2].rgbc
#define R_RC0 cpu->cop2_dr.rgb[0].c[0]
#define R_GC0 cpu->cop2_dr.rgb[0].c[1]
#define R_BC0 cpu->cop2_dr.rgb[0].c[2]
#define R_CD0 cpu->cop2_dr.rgb[0].c[3]
#define R_RC1 cpu->cop2_dr.rgb[1].c[0]
#define R_GC1 cpu->cop2_dr.rgb[1].c[1]
#define R_BC1 cpu->cop2_dr.rgb[1].c[2]
#define R_CD1 cpu->cop2_dr.rgb[1].c[3]
#define R_RC2 cpu->cop2_dr.rgb[2].c[0]
#define R_GC2 cpu->cop2_dr.rgb[2].c[1]
#define R_BC2 cpu->cop2_dr.rgb[2].c[2]
#define R_CD2 cpu->cop2_dr.rgb[2].c[3]
#define R_L11 cpu->cop2_cr.l.m[0].c[0]
#define R_L12 cpu->cop2_cr.l.m[0].c[1]
#define R_L13 cpu->cop2_cr.l.m[1].c[0]
#define R_L21 cpu->cop2_cr.l.m[1].c[1]
#define R_L22 cpu->cop2_cr.l.m[2].c[0]
#define R_L23 cpu->cop2_cr.l.m[2].c[1]
#define R_L31 cpu->cop2_cr.l.m[3].c[0]
#define R_L32 cpu->cop2_cr.l.m[3].c[1]
#define R_L33 cpu->cop2_cr.l.m33
#define R_RBK cpu->cop2_cr.bk.x
#define R_GBK cpu->cop2_cr.bk.y
#define R_BBK cpu->cop2_cr.bk.z
#define R_LR1 cpu->cop2_cr.lr.m[0].c[0]
#define R_LR2 cpu->cop2_cr.lr.m[0].c[1]
#define R_LR3 cpu->cop2_cr.lr.m[1].c[0]
#define R_LG1 cpu->cop2_cr.lr.m[1].c[1]
#define R_LG2 cpu->cop2_cr.lr.m[2].c[0]
#define R_LG3 cpu->cop2_cr.lr.m[2].c[1]
#define R_LB1 cpu->cop2_cr.lr.m[3].c[0]
#define R_LB2 cpu->cop2_cr.lr.m[3].c[1]
#define R_LB3 cpu->cop2_cr.lr.m33

/*
    Widescreen hack. See psx/cpu.h for why scaling the IR1 product (and nothing else) is
    the correct place: OFX is the projection centre, so this shrinks X about the centre.

    Fixed 3/4 = (4:3)/(16:9). Integer arithmetic on purpose — |IR1| < 2^15 and |div| < 2^17,
    so the product is under 2^32 and tripling it cannot come close to overflowing int64_t.
    The truncation is at 1/65536 of a pixel (the result is >> 16 immediately afterwards), so
    rounding direction is not observable.

    Applied INSIDE gte_clamp_mac0()'s argument so the MAC0 overflow flags describe the value
    actually produced. With the hack off the expression is textually the original one.
*/
#define GTE_WIDE(prod) (psx_widescreen_active() ? (((prod) * 3) / 4) : (prod))

int g_psx_widescreen_hack = 0;

void psx_cpu_set_widescreen_hack(int enabled) {
    g_psx_widescreen_hack = enabled ? 1 : 0;
}

int psx_cpu_widescreen_hack(void) {
    return g_psx_widescreen_hack;
}

/*
    PGXP capture, shared by GTE_RTP and GTE_RTP_DQ. Runs right after the SXY2
    assignments, with the macro-local `div` still in scope. The pre-truncation
    screen coordinate the hardware computes is (OFX + IR1*div) as a 48-bit
    16.16-ish value that gte_clamp_sxy() then shifts and saturates; dividing
    the same product by 65536.0 in double is exact (|IR1*div| < 2^33 fits a
    53-bit mantissa), so the float we hand PGXP truncates to exactly the
    integer the game will read whenever no saturation fired. SZ3 is the depth
    the projection divide used, kept for perspective-correct interpolation.
*/
#define GTE_PGXP_CAPTURE() { \
    if (psx_pgxp_active()) { \
        psx_pgxp_gte_vertex(R_SXY2, \
            (float)(((double)((int32_t)R_OFX) + GTE_WIDE((double)R_IR1 * (double)div)) / 65536.0), \
            (float)(((double)((int32_t)R_OFY) + (double)R_IR2 * (double)div) / 65536.0), \
            (float)R_SZ3); \
    } }

#define GTE_RTP_DQ(i) { \
    int64_t vx = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[0]); \
    int64_t vy = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[1]); \
    int64_t vz = (int64_t)cpu->cop2_dr.v[i].z; \
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (((int64_t)R_TRX) << 12) + (I64((int16_t)R_RT11) * vx)) + (I64((int16_t)R_RT12) * vy)) + (I64((int16_t)R_RT13) * vz)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (((int64_t)R_TRY) << 12) + (I64((int16_t)R_RT21) * vx)) + (I64((int16_t)R_RT22) * vy)) + (I64((int16_t)R_RT23) * vz)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (((int64_t)R_TRZ) << 12) + (I64((int16_t)R_RT31) * vx)) + (I64((int16_t)R_RT32) * vy)) + (I64((int16_t)R_RT33) * vz)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir_z(cpu, cpu->s_mac3, cpu->gte_sf, cpu->gte_lm); \
    R_SZ0 = R_SZ1; \
    R_SZ1 = R_SZ2; \
    R_SZ2 = R_SZ3; \
    R_SZ3 = gte_clamp_sz3(cpu, cpu->s_mac3 >> 12); \
    int32_t div = gte_divide(cpu, R_H, R_SZ3); \
    R_SXY0 = R_SXY1; \
    R_SXY1 = R_SXY2; \
    R_SX2 = gte_clamp_sxy(cpu, 1, (gte_clamp_mac0(cpu, (int64_t)((int32_t)R_OFX) + GTE_WIDE((int64_t)R_IR1 * div)) >> 16)); \
    R_SY2 = gte_clamp_sxy(cpu, 2, (gte_clamp_mac0(cpu, (int64_t)((int32_t)R_OFY) + ((int64_t)R_IR2 * div)) >> 16)); \
    GTE_PGXP_CAPTURE(); \
    R_MAC0 = gte_clamp_mac0(cpu, ((int64_t)R_DQB) + (((int64_t)R_DQA) * div)); \
    R_IR0 = gte_clamp_ir0(cpu, cpu->s_mac0 >> 12); }

#define GTE_RTP(i) { \
    int64_t vx = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[0]); \
    int64_t vy = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[1]); \
    int64_t vz = (int64_t)cpu->cop2_dr.v[i].z; \
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (((int64_t)R_TRX) << 12) + (I64((int16_t)R_RT11) * vx)) + (I64((int16_t)R_RT12) * vy)) + (I64((int16_t)R_RT13) * vz)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (((int64_t)R_TRY) << 12) + (I64((int16_t)R_RT21) * vx)) + (I64((int16_t)R_RT22) * vy)) + (I64((int16_t)R_RT23) * vz)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (((int64_t)R_TRZ) << 12) + (I64((int16_t)R_RT31) * vx)) + (I64((int16_t)R_RT32) * vy)) + (I64((int16_t)R_RT33) * vz)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir_z(cpu, cpu->s_mac3, cpu->gte_sf, cpu->gte_lm); \
    R_SZ0 = R_SZ1; \
    R_SZ1 = R_SZ2; \
    R_SZ2 = R_SZ3; \
    R_SZ3 = gte_clamp_sz3(cpu, cpu->s_mac3 >> 12); \
    int32_t div = gte_divide(cpu, R_H, R_SZ3); \
    R_SXY0 = R_SXY1; \
    R_SXY1 = R_SXY2; \
    R_SX2 = gte_clamp_sxy(cpu, 1, (gte_clamp_mac0(cpu, (int64_t)((int32_t)R_OFX) + GTE_WIDE((int64_t)R_IR1 * div)) >> 16)); \
    R_SY2 = gte_clamp_sxy(cpu, 2, (gte_clamp_mac0(cpu, (int64_t)((int32_t)R_OFY) + ((int64_t)R_IR2 * div)) >> 16)); \
    GTE_PGXP_CAPTURE(); }

#define DPCT1 { \
    int64_t mac1 = gte_clamp_mac(cpu, 1, (((int64_t)R_RFC) << 12) - (((int64_t)cpu->cop2_dr.rgb[0].c[0]) << 16)); \
    int64_t mac2 = gte_clamp_mac(cpu, 2, (((int64_t)R_GFC) << 12) - (((int64_t)cpu->cop2_dr.rgb[0].c[1]) << 16)); \
    int64_t mac3 = gte_clamp_mac(cpu, 3, (((int64_t)R_BFC) << 12) - (((int64_t)cpu->cop2_dr.rgb[0].c[2]) << 16)); \
    int64_t ir1 = gte_clamp_ir(cpu, 1, mac1, 0); \
    int64_t ir2 = gte_clamp_ir(cpu, 2, mac2, 0); \
    int64_t ir3 = gte_clamp_ir(cpu, 3, mac3, 0); \
    R_MAC1 = gte_clamp_mac(cpu, 1, (((int64_t)cpu->cop2_dr.rgb[0].c[0]) << 16) + (R_IR0 * ir1)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, (((int64_t)cpu->cop2_dr.rgb[0].c[1]) << 16) + (R_IR0 * ir2)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, (((int64_t)cpu->cop2_dr.rgb[0].c[2]) << 16) + (R_IR0 * ir3)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_RGB0 = R_RGB1; \
    R_RGB1 = R_RGB2; \
    R_CD2 = R_CODE; \
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4); \
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4); \
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4); }

#define NCCS(i) { \
    int64_t vx = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[0]); \
    int64_t vy = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[1]); \
    int64_t vz = (int64_t)cpu->cop2_dr.v[i].z; \
    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_L11) * vx) + (I64(R_L12) * vy) + (I64(R_L13) * vz)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_L21) * vx) + (I64(R_L22) * vy) + (I64(R_L23) * vz)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_L31) * vx) + (I64(R_L32) * vy) + (I64(R_L33) * vz)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (I64(R_RBK) << 12) + (I64(R_LR1) * I64(R_IR1))) + (I64(R_LR2) * I64(R_IR2))) + (I64(R_LR3) * I64(R_IR3))); \
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (I64(R_GBK) << 12) + (I64(R_LG1) * I64(R_IR1))) + (I64(R_LG2) * I64(R_IR2))) + (I64(R_LG3) * I64(R_IR3))); \
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (I64(R_BBK) << 12) + (I64(R_LB1) * I64(R_IR1))) + (I64(R_LB2) * I64(R_IR2))) + (I64(R_LB3) * I64(R_IR3))); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_RC) * I64(R_IR1)) << 4); \
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_GC) * I64(R_IR2)) << 4); \
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_BC) * I64(R_IR3)) << 4); \
    R_RGB0 = R_RGB1; \
    R_RGB1 = R_RGB2; \
    R_CD2 = R_CODE; \
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4); \
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4); \
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); }

#define NCS(i) { \
    int64_t vx = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[0]); \
    int64_t vy = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[1]); \
    int64_t vz = (int64_t)cpu->cop2_dr.v[i].z; \
    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_L11) * vx) + (I64(R_L12) * vy) + (I64(R_L13) * vz)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_L21) * vx) + (I64(R_L22) * vy) + (I64(R_L23) * vz)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_L31) * vx) + (I64(R_L32) * vy) + (I64(R_L33) * vz)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (I64(R_RBK) << 12) + (I64(R_LR1) * I64(R_IR1))) + (I64(R_LR2) * I64(R_IR2))) + (I64(R_LR3) * I64(R_IR3))); \
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (I64(R_GBK) << 12) + (I64(R_LG1) * I64(R_IR1))) + (I64(R_LG2) * I64(R_IR2))) + (I64(R_LG3) * I64(R_IR3))); \
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (I64(R_BBK) << 12) + (I64(R_LB1) * I64(R_IR1))) + (I64(R_LB2) * I64(R_IR2))) + (I64(R_LB3) * I64(R_IR3))); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_RGB0 = R_RGB1; \
    R_RGB1 = R_RGB2; \
    R_CD2 = R_CODE; \
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4); \
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4); \
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); }

#define NCDS(i) { \
    int64_t vx = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[0]); \
    int64_t vy = (int64_t)((int16_t)cpu->cop2_dr.v[i].p[1]); \
    int64_t vz = (int64_t)cpu->cop2_dr.v[i].z; \
    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_L11) * vx) + (I64(R_L12) * vy) + (I64(R_L13) * vz)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_L21) * vx) + (I64(R_L22) * vy) + (I64(R_L23) * vz)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_L31) * vx) + (I64(R_L32) * vy) + (I64(R_L33) * vz)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (I64(R_RBK) << 12) + (I64(R_LR1) * I64(R_IR1))) + (I64(R_LR2) * I64(R_IR2))) + (I64(R_LR3) * I64(R_IR3))); \
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (I64(R_GBK) << 12) + (I64(R_LG1) * I64(R_IR1))) + (I64(R_LG2) * I64(R_IR2))) + (I64(R_LG3) * I64(R_IR3))); \
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (I64(R_BBK) << 12) + (I64(R_LB1) * I64(R_IR1))) + (I64(R_LB2) * I64(R_IR2))) + (I64(R_LB3) * I64(R_IR3))); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    int64_t ir1 = gte_clamp_ir(cpu, 1, gte_clamp_mac(cpu, 1, ((I64(R_RFC) << 12) - ((I64(R_RC << 4)) * I64(R_IR1)))), 0); \
    int64_t ir2 = gte_clamp_ir(cpu, 2, gte_clamp_mac(cpu, 2, ((I64(R_GFC) << 12) - ((I64(R_GC << 4)) * I64(R_IR2)))), 0); \
    int64_t ir3 = gte_clamp_ir(cpu, 3, gte_clamp_mac(cpu, 3, ((I64(R_BFC) << 12) - ((I64(R_BC << 4)) * I64(R_IR3)))), 0); \
    R_MAC1 = gte_clamp_mac(cpu, 1, ((I64(R_RC << 4)) * I64(R_IR1)) + (I64(R_IR0) * ir1)); \
    R_MAC2 = gte_clamp_mac(cpu, 2, ((I64(R_GC << 4)) * I64(R_IR2)) + (I64(R_IR0) * ir2)); \
    R_MAC3 = gte_clamp_mac(cpu, 3, ((I64(R_BC << 4)) * I64(R_IR3)) + (I64(R_IR0) * ir3)); \
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm); \
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm); \
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm); \
    R_RGB0 = R_RGB1; \
    R_RGB1 = R_RGB2; \
    R_CD2 = R_CODE; \
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4); \
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4); \
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4); }

static inline void psx_gte_i_rtps(psx_cpu_t* cpu) {
    R_FLAG = 0;
    GTE_RTP_DQ(0);
}

static inline void psx_gte_i_nclip(psx_cpu_t* cpu) {
    R_FLAG = 0;

    int64_t value = I64((int16_t)R_SX0) * (I64((int16_t)R_SY1) - I64((int16_t)R_SY2));
    value += I64((int16_t)R_SX1) * (I64((int16_t)R_SY2) - I64((int16_t)R_SY0));
    value += I64((int16_t)R_SX2) * (I64((int16_t)R_SY0) - I64((int16_t)R_SY1));

    R_MAC0 = (int)gte_clamp_mac0(cpu, value);
}

static inline void psx_gte_i_op(psx_cpu_t* cpu) {
    R_FLAG = 0;

    R_MAC1 = gte_clamp_mac(cpu, 1, I64(I64((int16_t)R_RT22) * I64(R_IR3)) - I64((I64((int16_t)R_RT33) * I64(R_IR2))));
    R_MAC2 = gte_clamp_mac(cpu, 2, I64(I64((int16_t)R_RT33) * I64(R_IR1)) - I64((I64((int16_t)R_RT11) * I64(R_IR3))));
    R_MAC3 = gte_clamp_mac(cpu, 3, I64(I64((int16_t)R_RT11) * I64(R_IR2)) - I64((I64((int16_t)R_RT22) * I64(R_IR1))));

    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
}

static inline void psx_gte_i_dpcs(psx_cpu_t* cpu) {
    R_FLAG = 0;

    int64_t mac1 = gte_clamp_mac(cpu, 1, (((int64_t)R_RFC) << 12) - (((int64_t)R_RC) << 16));
    int64_t mac2 = gte_clamp_mac(cpu, 2, (((int64_t)R_GFC) << 12) - (((int64_t)R_GC) << 16));
    int64_t mac3 = gte_clamp_mac(cpu, 3, (((int64_t)R_BFC) << 12) - (((int64_t)R_BC) << 16));

    int64_t ir1 = gte_clamp_ir(cpu, 1, mac1, 0);
    int64_t ir2 = gte_clamp_ir(cpu, 2, mac2, 0);
    int64_t ir3 = gte_clamp_ir(cpu, 3, mac3, 0);

    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_RC) << 16) + (R_IR0 * ir1));
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_GC) << 16) + (R_IR0 * ir2));
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_BC) << 16) + (R_IR0 * ir3));

    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);

    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_CD2 = R_CODE;

    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
}

static inline void psx_gte_i_intpl(psx_cpu_t* cpu) {
    R_FLAG = 0;

    int64_t mac1 = gte_clamp_mac(cpu, 1, (((int64_t)R_RFC) << 12) - (I64(R_IR1) << 12));
    int64_t mac2 = gte_clamp_mac(cpu, 2, (((int64_t)R_GFC) << 12) - (I64(R_IR2) << 12));
    int64_t mac3 = gte_clamp_mac(cpu, 3, (((int64_t)R_BFC) << 12) - (I64(R_IR3) << 12));

    int64_t ir1 = gte_clamp_ir(cpu, 1, mac1, 0);
    int64_t ir2 = gte_clamp_ir(cpu, 2, mac2, 0);
    int64_t ir3 = gte_clamp_ir(cpu, 3, mac3, 0);

    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_IR1) << 12) + (I64(R_IR0) * ir1));
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_IR2) << 12) + (I64(R_IR0) * ir2));
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_IR3) << 12) + (I64(R_IR0) * ir3));

    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);

    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_CD2 = R_CODE;

    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
}

#define R_VX v.p[0]
#define R_VY v.p[1]
#define R_VZ v.z
#define R_MX11 mx.m[0].c[0]
#define R_MX11 mx.m[0].c[0]
#define R_MX12 mx.m[0].c[1]
#define R_MX13 mx.m[1].c[0]
#define R_MX21 mx.m[1].c[1]
#define R_MX22 mx.m[2].c[0]
#define R_MX23 mx.m[2].c[1]
#define R_MX31 mx.m[3].c[0]
#define R_MX32 mx.m[3].c[1]
#define R_MX33 mx.m33
#define R_CV1 cv.x
#define R_CV2 cv.y
#define R_CV3 cv.z

static inline void psx_gte_i_mvmva(psx_cpu_t* cpu) {
    R_FLAG = 0;

    gte_matrix_t mx = { 0 };
    gte_vertex_t v = { 0 };
    gte_vec3_t cv = { 0 };

    switch (cpu->gte_mx) {
        case 0: mx = cpu->cop2_cr.rt; break;
        case 1: mx = cpu->cop2_cr.l; break;
        case 2: mx = cpu->cop2_cr.lr; break;
        case 3: {
            R_MX11 = -(R_RC << 4);
            R_MX12 = R_RC << 4;
            R_MX13 = R_IR0;
            R_MX21 = R_RT13;
            R_MX22 = R_RT13;
            R_MX23 = R_RT13;
            R_MX31 = R_RT22;
            R_MX32 = R_RT22;
            R_MX33 = R_RT22;
        } break;
    }

    switch (cpu->gte_v) {
        case 0: case 1: case 2:
            v = cpu->cop2_dr.v[cpu->gte_v];
        break;

        case 3: {
            v.p[0] = R_IR1;
            v.p[1] = R_IR2;
            v.z = R_IR3;
        } break;
    }

    switch (cpu->gte_cv) {
        case 0: cv = cpu->cop2_cr.tr; break;
        case 1: cv = cpu->cop2_cr.bk; break;
        case 2: cv = cpu->cop2_cr.fc; break;
        case 3: {
            cv.x = 0;
            cv.y = 0;
            cv.z = 0;
        } break;
    }

    // Bugged case (CV=FC)
    if (cpu->gte_cv == 2) {
        R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, I64(R_MX12) * I64(R_VY)) + (I64(R_MX13) * I64(R_VZ)));
        R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, I64(R_MX22) * I64(R_VY)) + (I64(R_MX23) * I64(R_VZ)));
        R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, I64(R_MX32) * I64(R_VY)) + (I64(R_MX33) * I64(R_VZ)));

        int64_t mac1 = gte_clamp_mac(cpu, 1, (I64(R_CV1) << 12) + (I64(R_MX11) * I64(R_VX))); 
        int64_t mac2 = gte_clamp_mac(cpu, 2, (I64(R_CV2) << 12) + (I64(R_MX21) * I64(R_VX))); 
        int64_t mac3 = gte_clamp_mac(cpu, 3, (I64(R_CV3) << 12) + (I64(R_MX31) * I64(R_VX))); 

        gte_clamp_ir(cpu, 1, mac1, 0);
        gte_clamp_ir(cpu, 2, mac2, 0);
        gte_clamp_ir(cpu, 3, mac3, 0);
    } else {
        R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (I64(R_CV1) << 12) + (I64(R_MX11) * I64(R_VX))) + (I64(R_MX12) * I64(R_VY))) + (I64(R_MX13) * I64(R_VZ)));
        R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (I64(R_CV2) << 12) + (I64(R_MX21) * I64(R_VX))) + (I64(R_MX22) * I64(R_VY))) + (I64(R_MX23) * I64(R_VZ)));
        R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (I64(R_CV3) << 12) + (I64(R_MX31) * I64(R_VX))) + (I64(R_MX32) * I64(R_VY))) + (I64(R_MX33) * I64(R_VZ)));
    }

    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
}

#undef R_VX
#undef R_VY
#undef R_VZ
#undef R_MX11
#undef R_MX11
#undef R_MX12
#undef R_MX13
#undef R_MX21
#undef R_MX22
#undef R_MX23
#undef R_MX31
#undef R_MX32
#undef R_MX33
#undef R_CV1
#undef R_CV2
#undef R_CV3

// To-do: Fix flags
static inline void psx_gte_i_ncds(psx_cpu_t* cpu) {
    R_FLAG = 0;

    NCDS(0);
}

static inline void psx_gte_i_cdp(psx_cpu_t* cpu) {
    R_FLAG = 0;
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (I64(R_RBK) << 12) + (I64(R_LR1) * I64(R_IR1))) + (I64(R_LR2) * I64(R_IR2))) + (I64(R_LR3) * I64(R_IR3)));
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (I64(R_GBK) << 12) + (I64(R_LG1) * I64(R_IR1))) + (I64(R_LG2) * I64(R_IR2))) + (I64(R_LG3) * I64(R_IR3)));
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (I64(R_BBK) << 12) + (I64(R_LB1) * I64(R_IR1))) + (I64(R_LB2) * I64(R_IR2))) + (I64(R_LB3) * I64(R_IR3)));
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
    int64_t ir1 = gte_clamp_ir(cpu, 1, gte_clamp_mac(cpu, 1, ((I64(R_RFC) << 12) - ((I64(R_RC << 4)) * I64(R_IR1)))), 0);
    int64_t ir2 = gte_clamp_ir(cpu, 2, gte_clamp_mac(cpu, 2, ((I64(R_GFC) << 12) - ((I64(R_GC << 4)) * I64(R_IR2)))), 0);
    int64_t ir3 = gte_clamp_ir(cpu, 3, gte_clamp_mac(cpu, 3, ((I64(R_BFC) << 12) - ((I64(R_BC << 4)) * I64(R_IR3)))), 0);
    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_RC << 4) * I64(R_IR1)) + (I64(R_IR0) * ir1));
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_GC << 4) * I64(R_IR2)) + (I64(R_IR0) * ir2));
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_BC << 4) * I64(R_IR3)) + (I64(R_IR0) * ir3));
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
    R_CD2 = R_CODE;
}

static inline void psx_gte_i_ncdt(psx_cpu_t* cpu) {
    R_FLAG = 0;
    NCDS(0);
    NCDS(1);
    NCDS(2);
}

static inline void psx_gte_i_nccs(psx_cpu_t* cpu) {
    R_FLAG = 0;
    NCCS(0);
}

static inline void psx_gte_i_cc(psx_cpu_t* cpu) {
    R_FLAG = 0;
    R_MAC1 = gte_clamp_mac(cpu, 1, gte_check_mac(cpu, 1, gte_check_mac(cpu, 1, (I64(R_RBK) << 12) + (I64(R_LR1) * I64(R_IR1))) + (I64(R_LR2) * I64(R_IR2))) + (I64(R_LR3) * I64(R_IR3)));
    R_MAC2 = gte_clamp_mac(cpu, 2, gte_check_mac(cpu, 2, gte_check_mac(cpu, 2, (I64(R_GBK) << 12) + (I64(R_LG1) * I64(R_IR1))) + (I64(R_LG2) * I64(R_IR2))) + (I64(R_LG3) * I64(R_IR3)));
    R_MAC3 = gte_clamp_mac(cpu, 3, gte_check_mac(cpu, 3, gte_check_mac(cpu, 3, (I64(R_BBK) << 12) + (I64(R_LB1) * I64(R_IR1))) + (I64(R_LB2) * I64(R_IR2))) + (I64(R_LB3) * I64(R_IR3)));
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_RC) * I64(R_IR1)) << 4);
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_GC) * I64(R_IR2)) << 4);
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_BC) * I64(R_IR3)) << 4);
    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_CD2 = R_CODE;
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
}

static inline void psx_gte_i_ncs(psx_cpu_t* cpu) {
    R_FLAG = 0;
    NCS(0);
}

static inline void psx_gte_i_nct(psx_cpu_t* cpu) {
    R_FLAG = 0;
    NCS(0);
    NCS(1);
    NCS(2);
}

static inline void psx_gte_i_sqr(psx_cpu_t* cpu) {
    R_FLAG = 0;

    R_MAC1 = gte_clamp_mac(cpu, 1, I64(R_IR1) * I64(R_IR1));
    R_MAC2 = gte_clamp_mac(cpu, 2, I64(R_IR2) * I64(R_IR2));
    R_MAC3 = gte_clamp_mac(cpu, 3, I64(R_IR3) * I64(R_IR3));

    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
}

static inline void psx_gte_i_dcpl(psx_cpu_t* cpu) {
    R_FLAG = 0;

    R_MAC1 = gte_clamp_mac(cpu, 1, I64(R_RC) * I64(R_IR1)) << 4;
    R_MAC2 = gte_clamp_mac(cpu, 2, I64(R_GC) * I64(R_IR2)) << 4;
    R_MAC3 = gte_clamp_mac(cpu, 3, I64(R_BC) * I64(R_IR3)) << 4;
    int64_t ir1 = gte_clamp_ir(cpu, 1, gte_clamp_mac(cpu, 1, ((I64(R_RFC) << 12) - ((I64(R_RC << 4)) * I64(R_IR1)))), 0);
    int64_t ir2 = gte_clamp_ir(cpu, 2, gte_clamp_mac(cpu, 2, ((I64(R_GFC) << 12) - ((I64(R_GC << 4)) * I64(R_IR2)))), 0);
    int64_t ir3 = gte_clamp_ir(cpu, 3, gte_clamp_mac(cpu, 3, ((I64(R_BFC) << 12) - ((I64(R_BC << 4)) * I64(R_IR3)))), 0);
    R_MAC1 = gte_clamp_mac(cpu, 1, ((I64(R_RC << 4)) * I64(R_IR1)) + (I64(R_IR0) * ir1));
    R_MAC2 = gte_clamp_mac(cpu, 2, ((I64(R_GC << 4)) * I64(R_IR2)) + (I64(R_IR0) * ir2));
    R_MAC3 = gte_clamp_mac(cpu, 3, ((I64(R_BC << 4)) * I64(R_IR3)) + (I64(R_IR0) * ir3));
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_CD2 = R_CODE;
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
}

static inline void psx_gte_i_dpct(psx_cpu_t* cpu) {
    R_FLAG = 0;
    DPCT1;
    DPCT1;
    DPCT1;
}

static inline void psx_gte_i_avsz3(psx_cpu_t* cpu) {
    R_FLAG = 0;

    int64_t avg = I64(R_ZSF3) * (R_SZ1 + R_SZ2 + R_SZ3);

    R_MAC0 = (int)gte_clamp_mac0(cpu, avg);
    R_OTZ = gte_clamp_sz3(cpu, avg >> 12);
}

static inline void psx_gte_i_avsz4(psx_cpu_t* cpu) {
    R_FLAG = 0;

    int64_t avg = I64(R_ZSF4) * (R_SZ0 + R_SZ1 + R_SZ2 + R_SZ3);

    R_MAC0 = (int)gte_clamp_mac0(cpu, avg);
    R_OTZ = gte_clamp_sz3(cpu, avg >> 12);
}

static inline void psx_gte_i_rtpt(psx_cpu_t* cpu) {
    R_FLAG = 0;
    GTE_RTP(0);
    GTE_RTP(1);
    GTE_RTP_DQ(2);
}

static inline void psx_gte_i_gpf(psx_cpu_t* cpu) {
    R_FLAG = 0;

    R_MAC1 = gte_clamp_mac(cpu, 1, R_IR0 * R_IR1);
    R_MAC2 = gte_clamp_mac(cpu, 2, R_IR0 * R_IR2);
    R_MAC3 = gte_clamp_mac(cpu, 3, R_IR0 * R_IR3);
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_CD2 = R_CODE;
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
}

static inline void psx_gte_i_gpl(psx_cpu_t* cpu) {
    R_FLAG = 0;

    R_MAC1 = gte_clamp_mac(cpu, 1, (I64(R_MAC1) << cpu->gte_sf) + (R_IR0 * R_IR1));
    R_MAC2 = gte_clamp_mac(cpu, 2, (I64(R_MAC2) << cpu->gte_sf) + (R_IR0 * R_IR2));
    R_MAC3 = gte_clamp_mac(cpu, 3, (I64(R_MAC3) << cpu->gte_sf) + (R_IR0 * R_IR3));
    R_IR1 = gte_clamp_ir(cpu, 1, R_MAC1, cpu->gte_lm);
    R_IR2 = gte_clamp_ir(cpu, 2, R_MAC2, cpu->gte_lm);
    R_IR3 = gte_clamp_ir(cpu, 3, R_MAC3, cpu->gte_lm);
    R_RGB0 = R_RGB1;
    R_RGB1 = R_RGB2;
    R_CD2 = R_CODE;
    R_RC2 = gte_clamp_rgb(cpu, 1, R_MAC1 >> 4);
    R_GC2 = gte_clamp_rgb(cpu, 2, R_MAC2 >> 4);
    R_BC2 = gte_clamp_rgb(cpu, 3, R_MAC3 >> 4);
}

static inline void psx_gte_i_ncct(psx_cpu_t* cpu) {
    R_FLAG = 0;
    NCCS(0);
    NCCS(1);
    NCCS(2);
}

#define PSX_CPU_CACHED_WRAPPER(name, cycles) \
    static int psx_cpu_cached_##name(psx_cpu_t* cpu) { \
        psx_cpu_i_##name(cpu); \
        return (cycles); \
    }

PSX_CPU_CACHED_WRAPPER(sll, 2)
PSX_CPU_CACHED_WRAPPER(srl, 2)
PSX_CPU_CACHED_WRAPPER(sra, 2)
PSX_CPU_CACHED_WRAPPER(sllv, 2)
PSX_CPU_CACHED_WRAPPER(srlv, 2)
PSX_CPU_CACHED_WRAPPER(srav, 2)
PSX_CPU_CACHED_WRAPPER(jr, 2)
PSX_CPU_CACHED_WRAPPER(jalr, 2)
PSX_CPU_CACHED_WRAPPER(syscall, 2)
PSX_CPU_CACHED_WRAPPER(break, 2)
PSX_CPU_CACHED_WRAPPER(mfhi, 2)
PSX_CPU_CACHED_WRAPPER(mthi, 2)
PSX_CPU_CACHED_WRAPPER(mflo, 2)
PSX_CPU_CACHED_WRAPPER(mtlo, 2)
PSX_CPU_CACHED_WRAPPER(mult, 2)
PSX_CPU_CACHED_WRAPPER(multu, 2)
PSX_CPU_CACHED_WRAPPER(div, 2)
PSX_CPU_CACHED_WRAPPER(divu, 2)
PSX_CPU_CACHED_WRAPPER(add, 2)
PSX_CPU_CACHED_WRAPPER(addu, 2)
PSX_CPU_CACHED_WRAPPER(sub, 2)
PSX_CPU_CACHED_WRAPPER(subu, 2)
PSX_CPU_CACHED_WRAPPER(and, 2)
PSX_CPU_CACHED_WRAPPER(or, 2)
PSX_CPU_CACHED_WRAPPER(xor, 2)
PSX_CPU_CACHED_WRAPPER(nor, 2)
PSX_CPU_CACHED_WRAPPER(slt, 2)
PSX_CPU_CACHED_WRAPPER(sltu, 2)
PSX_CPU_CACHED_WRAPPER(j, 2)
PSX_CPU_CACHED_WRAPPER(jal, 2)
PSX_CPU_CACHED_WRAPPER(beq, 2)
PSX_CPU_CACHED_WRAPPER(bne, 2)
PSX_CPU_CACHED_WRAPPER(blez, 2)
PSX_CPU_CACHED_WRAPPER(bgtz, 2)
PSX_CPU_CACHED_WRAPPER(addi, 2)
PSX_CPU_CACHED_WRAPPER(addiu, 2)
PSX_CPU_CACHED_WRAPPER(slti, 2)
PSX_CPU_CACHED_WRAPPER(sltiu, 2)
PSX_CPU_CACHED_WRAPPER(andi, 2)
PSX_CPU_CACHED_WRAPPER(ori, 2)
PSX_CPU_CACHED_WRAPPER(xori, 2)
PSX_CPU_CACHED_WRAPPER(lui, 2)
PSX_CPU_CACHED_WRAPPER(mfc0, 2)
PSX_CPU_CACHED_WRAPPER(mtc0, 2)
PSX_CPU_CACHED_WRAPPER(rfe, 2)
PSX_CPU_CACHED_WRAPPER(mfc2, 2)
PSX_CPU_CACHED_WRAPPER(cfc2, 2)
PSX_CPU_CACHED_WRAPPER(mtc2, 2)
PSX_CPU_CACHED_WRAPPER(ctc2, 2)
PSX_CPU_CACHED_WRAPPER(lb, 2)
PSX_CPU_CACHED_WRAPPER(lh, 2)
PSX_CPU_CACHED_WRAPPER(lwl, 2)
PSX_CPU_CACHED_WRAPPER(lw, 2)
PSX_CPU_CACHED_WRAPPER(lbu, 2)
PSX_CPU_CACHED_WRAPPER(lhu, 2)
PSX_CPU_CACHED_WRAPPER(lwr, 2)
PSX_CPU_CACHED_WRAPPER(sb, 2)
PSX_CPU_CACHED_WRAPPER(sh, 2)
PSX_CPU_CACHED_WRAPPER(swl, 2)
PSX_CPU_CACHED_WRAPPER(sw, 2)
PSX_CPU_CACHED_WRAPPER(swr, 2)
PSX_CPU_CACHED_WRAPPER(lwc0, 2)
PSX_CPU_CACHED_WRAPPER(lwc1, 2)
PSX_CPU_CACHED_WRAPPER(lwc2, 2)
PSX_CPU_CACHED_WRAPPER(lwc3, 2)
PSX_CPU_CACHED_WRAPPER(swc0, 2)
PSX_CPU_CACHED_WRAPPER(swc1, 2)
PSX_CPU_CACHED_WRAPPER(swc2, 2)
PSX_CPU_CACHED_WRAPPER(swc3, 2)

#define PSX_CPU_CACHED_BXX_WRAPPER(name) \
    static int psx_cpu_cached_##name(psx_cpu_t* cpu) { \
        cpu->branch = 1; \
        cpu->branch_taken = 0; \
        psx_cpu_i_##name(cpu); \
        return 2; \
    }

PSX_CPU_CACHED_BXX_WRAPPER(bltz)
PSX_CPU_CACHED_BXX_WRAPPER(bgez)
PSX_CPU_CACHED_BXX_WRAPPER(bltzal)
PSX_CPU_CACHED_BXX_WRAPPER(bgezal)

#define PSX_CPU_CACHED_GTE_WRAPPER(name, cycles) \
    static int psx_cpu_cached_gte_##name(psx_cpu_t* cpu) { \
        DO_PENDING_LOAD; \
        PSX_PERF_INC(gte_ops); \
        cpu->gte_sf = ((cpu->opcode & 0x80000) != 0) * 12; \
        cpu->gte_lm = (cpu->opcode & 0x400) != 0; \
        cpu->gte_cv = (cpu->opcode >> 13) & 3; \
        cpu->gte_v = (cpu->opcode >> 15) & 3; \
        cpu->gte_mx = (cpu->opcode >> 17) & 3; \
        psx_gte_i_##name(cpu); \
        return (cycles); \
    }

PSX_CPU_CACHED_GTE_WRAPPER(rtps, 15)
PSX_CPU_CACHED_GTE_WRAPPER(nclip, 8)
PSX_CPU_CACHED_GTE_WRAPPER(op, 6)
PSX_CPU_CACHED_GTE_WRAPPER(dpcs, 8)
PSX_CPU_CACHED_GTE_WRAPPER(intpl, 8)
PSX_CPU_CACHED_GTE_WRAPPER(mvmva, 8)
PSX_CPU_CACHED_GTE_WRAPPER(ncds, 19)
PSX_CPU_CACHED_GTE_WRAPPER(cdp, 13)
PSX_CPU_CACHED_GTE_WRAPPER(ncdt, 44)
PSX_CPU_CACHED_GTE_WRAPPER(nccs, 17)
PSX_CPU_CACHED_GTE_WRAPPER(cc, 11)
PSX_CPU_CACHED_GTE_WRAPPER(ncs, 14)
PSX_CPU_CACHED_GTE_WRAPPER(nct, 30)
PSX_CPU_CACHED_GTE_WRAPPER(sqr, 5)
PSX_CPU_CACHED_GTE_WRAPPER(dcpl, 8)
PSX_CPU_CACHED_GTE_WRAPPER(dpct, 17)
PSX_CPU_CACHED_GTE_WRAPPER(avsz3, 5)
PSX_CPU_CACHED_GTE_WRAPPER(avsz4, 6)
PSX_CPU_CACHED_GTE_WRAPPER(rtpt, 23)
PSX_CPU_CACHED_GTE_WRAPPER(gpf, 5)
PSX_CPU_CACHED_GTE_WRAPPER(gpl, 5)
PSX_CPU_CACHED_GTE_WRAPPER(ncct, 39)

static int psx_cpu_cached_gte_invalid(psx_cpu_t* cpu) {
    DO_PENDING_LOAD;
    psx_gte_i_invalid(cpu);
    return 0;
}

static psx_cpu_cached_handler_t psx_cpu_decode(uint32_t opcode) {
    switch (opcode >> 26) {
        case 0x00:
            switch (opcode & 0x3f) {
                case 0x00: return psx_cpu_cached_sll;
                case 0x02: return psx_cpu_cached_srl;
                case 0x03: return psx_cpu_cached_sra;
                case 0x04: return psx_cpu_cached_sllv;
                case 0x06: return psx_cpu_cached_srlv;
                case 0x07: return psx_cpu_cached_srav;
                case 0x08: return psx_cpu_cached_jr;
                case 0x09: return psx_cpu_cached_jalr;
                case 0x0c: return psx_cpu_cached_syscall;
                case 0x0d: return psx_cpu_cached_break;
                case 0x10: return psx_cpu_cached_mfhi;
                case 0x11: return psx_cpu_cached_mthi;
                case 0x12: return psx_cpu_cached_mflo;
                case 0x13: return psx_cpu_cached_mtlo;
                case 0x18: return psx_cpu_cached_mult;
                case 0x19: return psx_cpu_cached_multu;
                case 0x1a: return psx_cpu_cached_div;
                case 0x1b: return psx_cpu_cached_divu;
                case 0x20: return psx_cpu_cached_add;
                case 0x21: return psx_cpu_cached_addu;
                case 0x22: return psx_cpu_cached_sub;
                case 0x23: return psx_cpu_cached_subu;
                case 0x24: return psx_cpu_cached_and;
                case 0x25: return psx_cpu_cached_or;
                case 0x26: return psx_cpu_cached_xor;
                case 0x27: return psx_cpu_cached_nor;
                case 0x2a: return psx_cpu_cached_slt;
                case 0x2b: return psx_cpu_cached_sltu;
                default: return NULL;
            }
        case 0x01:
            switch ((opcode >> 16) & 0x1f) {
                case 0x00: return psx_cpu_cached_bltz;
                case 0x01: return psx_cpu_cached_bgez;
                case 0x10: return psx_cpu_cached_bltzal;
                case 0x11: return psx_cpu_cached_bgezal;
                default: return (opcode & 0x00010000)
                    ? psx_cpu_cached_bgez
                    : psx_cpu_cached_bltz;
            }
        case 0x02: return psx_cpu_cached_j;
        case 0x03: return psx_cpu_cached_jal;
        case 0x04: return psx_cpu_cached_beq;
        case 0x05: return psx_cpu_cached_bne;
        case 0x06: return psx_cpu_cached_blez;
        case 0x07: return psx_cpu_cached_bgtz;
        case 0x08: return psx_cpu_cached_addi;
        case 0x09: return psx_cpu_cached_addiu;
        case 0x0a: return psx_cpu_cached_slti;
        case 0x0b: return psx_cpu_cached_sltiu;
        case 0x0c: return psx_cpu_cached_andi;
        case 0x0d: return psx_cpu_cached_ori;
        case 0x0e: return psx_cpu_cached_xori;
        case 0x0f: return psx_cpu_cached_lui;
        case 0x10:
            switch ((opcode >> 21) & 0x1f) {
                case 0x00: return psx_cpu_cached_mfc0;
                case 0x04: return psx_cpu_cached_mtc0;
                case 0x10: return psx_cpu_cached_rfe;
                default: return NULL;
            }
        case 0x12:
            switch ((opcode >> 21) & 0x1f) {
                case 0x00: return psx_cpu_cached_mfc2;
                case 0x02: return psx_cpu_cached_cfc2;
                case 0x04: return psx_cpu_cached_mtc2;
                case 0x06: return psx_cpu_cached_ctc2;
                default:
                    switch (opcode & 0x3f) {
                        case 0x01: return psx_cpu_cached_gte_rtps;
                        case 0x06: return psx_cpu_cached_gte_nclip;
                        case 0x0c: return psx_cpu_cached_gte_op;
                        case 0x10: return psx_cpu_cached_gte_dpcs;
                        case 0x11: return psx_cpu_cached_gte_intpl;
                        case 0x12: return psx_cpu_cached_gte_mvmva;
                        case 0x13: return psx_cpu_cached_gte_ncds;
                        case 0x14: return psx_cpu_cached_gte_cdp;
                        case 0x16: return psx_cpu_cached_gte_ncdt;
                        case 0x1b: return psx_cpu_cached_gte_nccs;
                        case 0x1c: return psx_cpu_cached_gte_cc;
                        case 0x1e: return psx_cpu_cached_gte_ncs;
                        case 0x20: return psx_cpu_cached_gte_nct;
                        case 0x28: return psx_cpu_cached_gte_sqr;
                        case 0x29: return psx_cpu_cached_gte_dcpl;
                        case 0x2a: return psx_cpu_cached_gte_dpct;
                        case 0x2d: return psx_cpu_cached_gte_avsz3;
                        case 0x2e: return psx_cpu_cached_gte_avsz4;
                        case 0x30: return psx_cpu_cached_gte_rtpt;
                        case 0x3d: return psx_cpu_cached_gte_gpf;
                        case 0x3e: return psx_cpu_cached_gte_gpl;
                        case 0x3f: return psx_cpu_cached_gte_ncct;
                        default: return psx_cpu_cached_gte_invalid;
                    }
            }
        case 0x20: return psx_cpu_cached_lb;
        case 0x21: return psx_cpu_cached_lh;
        case 0x22: return psx_cpu_cached_lwl;
        case 0x23: return psx_cpu_cached_lw;
        case 0x24: return psx_cpu_cached_lbu;
        case 0x25: return psx_cpu_cached_lhu;
        case 0x26: return psx_cpu_cached_lwr;
        case 0x28: return psx_cpu_cached_sb;
        case 0x29: return psx_cpu_cached_sh;
        case 0x2a: return psx_cpu_cached_swl;
        case 0x2b: return psx_cpu_cached_sw;
        case 0x2e: return psx_cpu_cached_swr;
        case 0x30: return psx_cpu_cached_lwc0;
        case 0x31: return psx_cpu_cached_lwc1;
        case 0x32: return psx_cpu_cached_lwc2;
        case 0x33: return psx_cpu_cached_lwc3;
        case 0x38: return psx_cpu_cached_swc0;
        case 0x39: return psx_cpu_cached_swc1;
        case 0x3a: return psx_cpu_cached_swc2;
        case 0x3b: return psx_cpu_cached_swc3;
        default: return NULL;
    }
}

static int psx_cpu_execute_cached(psx_cpu_t* cpu) {
    if (!cpu->cache)
        return psx_cpu_execute(cpu);

    const uint32_t address = psx_bus_physical_address(cpu->saved_pc);
    psx_cpu_cache_entry_t* entry = &cpu->cache->entries[psx_cpu_cache_index(address)];

    if (entry->valid && entry->address == address && entry->opcode == cpu->opcode) {
        cpu->cache->stats.hits++;
    } else {
        cpu->cache->stats.misses++;
        entry->address = address;
        entry->opcode = cpu->opcode;
        entry->handler = psx_cpu_decode(cpu->opcode);
        entry->valid = 1;
    }

    return entry->handler ? entry->handler(cpu) : 0;
}

int psx_cpu_execute(psx_cpu_t* cpu) {
    switch ((cpu->opcode & 0xfc000000) >> 26) {
        case 0x00000000 >> 26: {
            switch (cpu->opcode & 0x0000003f) {
                case 0x00000000: psx_cpu_i_sll(cpu); return 2;
                case 0x00000002: psx_cpu_i_srl(cpu); return 2;
                case 0x00000003: psx_cpu_i_sra(cpu); return 2;
                case 0x00000004: psx_cpu_i_sllv(cpu); return 2;
                case 0x00000006: psx_cpu_i_srlv(cpu); return 2;
                case 0x00000007: psx_cpu_i_srav(cpu); return 2;
                case 0x00000008: psx_cpu_i_jr(cpu); return 2;
                case 0x00000009: psx_cpu_i_jalr(cpu); return 2;
                case 0x0000000c: psx_cpu_i_syscall(cpu); return 2;
                case 0x0000000d: psx_cpu_i_break(cpu); return 2;
                case 0x00000010: psx_cpu_i_mfhi(cpu); return 2;
                case 0x00000011: psx_cpu_i_mthi(cpu); return 2;
                case 0x00000012: psx_cpu_i_mflo(cpu); return 2;
                case 0x00000013: psx_cpu_i_mtlo(cpu); return 2;
                case 0x00000018: psx_cpu_i_mult(cpu); return 2;
                case 0x00000019: psx_cpu_i_multu(cpu); return 2;
                case 0x0000001a: psx_cpu_i_div(cpu); return 2;
                case 0x0000001b: psx_cpu_i_divu(cpu); return 2;
                case 0x00000020: psx_cpu_i_add(cpu); return 2;
                case 0x00000021: psx_cpu_i_addu(cpu); return 2;
                case 0x00000022: psx_cpu_i_sub(cpu); return 2;
                case 0x00000023: psx_cpu_i_subu(cpu); return 2;
                case 0x00000024: psx_cpu_i_and(cpu); return 2;
                case 0x00000025: psx_cpu_i_or(cpu); return 2;
                case 0x00000026: psx_cpu_i_xor(cpu); return 2;
                case 0x00000027: psx_cpu_i_nor(cpu); return 2;
                case 0x0000002a: psx_cpu_i_slt(cpu); return 2;
                case 0x0000002b: psx_cpu_i_sltu(cpu); return 2;
            } break;
        } break;
        case 0x04000000 >> 26: {
            cpu->branch = 1;
            cpu->branch_taken = 0;

            switch ((cpu->opcode & 0x001f0000) >> 16) {
                case 0x00000000 >> 16: psx_cpu_i_bltz(cpu); return 2;
                case 0x00010000 >> 16: psx_cpu_i_bgez(cpu); return 2;
                case 0x00100000 >> 16: psx_cpu_i_bltzal(cpu); return 2;
                case 0x00110000 >> 16: psx_cpu_i_bgezal(cpu); return 2;
                // bltz/bgez dupes
                default: {
                    if (cpu->opcode & 0x00010000) {
                        psx_cpu_i_bgez(cpu);
                    } else {
                        psx_cpu_i_bltz(cpu);
                    }
                } return 2;
            } break;
        } break;
        case 0x08000000 >> 26: psx_cpu_i_j(cpu); return 2;
        case 0x0c000000 >> 26: psx_cpu_i_jal(cpu); return 2;
        case 0x10000000 >> 26: psx_cpu_i_beq(cpu); return 2;
        case 0x14000000 >> 26: psx_cpu_i_bne(cpu); return 2;
        case 0x18000000 >> 26: psx_cpu_i_blez(cpu); return 2;
        case 0x1c000000 >> 26: psx_cpu_i_bgtz(cpu); return 2;
        case 0x20000000 >> 26: psx_cpu_i_addi(cpu); return 2;
        case 0x24000000 >> 26: psx_cpu_i_addiu(cpu); return 2;
        case 0x28000000 >> 26: psx_cpu_i_slti(cpu); return 2;
        case 0x2c000000 >> 26: psx_cpu_i_sltiu(cpu); return 2;
        case 0x30000000 >> 26: psx_cpu_i_andi(cpu); return 2;
        case 0x34000000 >> 26: psx_cpu_i_ori(cpu); return 2;
        case 0x38000000 >> 26: psx_cpu_i_xori(cpu); return 2;
        case 0x3c000000 >> 26: psx_cpu_i_lui(cpu); return 2;
        case 0x40000000 >> 26: {
            switch ((cpu->opcode & 0x03e00000) >> 21) {
                case 0x00000000 >> 21: psx_cpu_i_mfc0(cpu); return 2;
                case 0x00800000 >> 21: psx_cpu_i_mtc0(cpu); return 2;
                case 0x02000000 >> 21: psx_cpu_i_rfe(cpu); return 2;
            }
        } break;
        case 0x48000000 >> 26: {
            switch ((cpu->opcode & 0x03e00000) >> 21) {
                case 0x00000000 >> 21: psx_cpu_i_mfc2(cpu); return 2;
                case 0x00400000 >> 21: psx_cpu_i_cfc2(cpu); return 2;
                case 0x00800000 >> 21: psx_cpu_i_mtc2(cpu); return 2;
                case 0x00c00000 >> 21: psx_cpu_i_ctc2(cpu); return 2;
                default: {
                    DO_PENDING_LOAD;

                    PSX_PERF_INC(gte_ops);

                    cpu->gte_sf = ((cpu->opcode & 0x80000) != 0) * 12;
                    cpu->gte_lm = (cpu->opcode & 0x400) != 0;
                    cpu->gte_cv = (cpu->opcode >> 13) & 3;
                    cpu->gte_v  = (cpu->opcode >> 15) & 3;
                    cpu->gte_mx = (cpu->opcode >> 17) & 3;

                    switch (cpu->opcode & 0x3f) {
                        case 0x01: psx_gte_i_rtps(cpu); return 15;
                        case 0x06: psx_gte_i_nclip(cpu); return 8;
                        case 0x0c: psx_gte_i_op(cpu); return 6;
                        case 0x10: psx_gte_i_dpcs(cpu); return 8;
                        case 0x11: psx_gte_i_intpl(cpu); return 8;
                        case 0x12: psx_gte_i_mvmva(cpu); return 8;
                        case 0x13: psx_gte_i_ncds(cpu); return 19;
                        case 0x14: psx_gte_i_cdp(cpu); return 13;
                        case 0x16: psx_gte_i_ncdt(cpu); return 44;
                        case 0x1b: psx_gte_i_nccs(cpu); return 17;
                        case 0x1c: psx_gte_i_cc(cpu); return 11;
                        case 0x1e: psx_gte_i_ncs(cpu); return 14;
                        case 0x20: psx_gte_i_nct(cpu); return 30;
                        case 0x28: psx_gte_i_sqr(cpu); return 5;
                        case 0x29: psx_gte_i_dcpl(cpu); return 8;
                        case 0x2a: psx_gte_i_dpct(cpu); return 17;
                        case 0x2d: psx_gte_i_avsz3(cpu); return 5;
                        case 0x2e: psx_gte_i_avsz4(cpu); return 6;
                        case 0x30: psx_gte_i_rtpt(cpu); return 23;
                        case 0x3d: psx_gte_i_gpf(cpu); return 5;
                        case 0x3e: psx_gte_i_gpl(cpu); return 5;
                        case 0x3f: psx_gte_i_ncct(cpu); return 39;
                        default: psx_gte_i_invalid(cpu); return 0;
                    }
                } break;
            }
        } break;
        case 0x80000000 >> 26: psx_cpu_i_lb(cpu); return 2;
        case 0x84000000 >> 26: psx_cpu_i_lh(cpu); return 2;
        case 0x88000000 >> 26: psx_cpu_i_lwl(cpu); return 2;
        case 0x8c000000 >> 26: psx_cpu_i_lw(cpu); return 2;
        case 0x90000000 >> 26: psx_cpu_i_lbu(cpu); return 2;
        case 0x94000000 >> 26: psx_cpu_i_lhu(cpu); return 2;
        case 0x98000000 >> 26: psx_cpu_i_lwr(cpu); return 2;
        case 0xa0000000 >> 26: psx_cpu_i_sb(cpu); return 2;
        case 0xa4000000 >> 26: psx_cpu_i_sh(cpu); return 2;
        case 0xa8000000 >> 26: psx_cpu_i_swl(cpu); return 2;
        case 0xac000000 >> 26: psx_cpu_i_sw(cpu); return 2;
        case 0xb8000000 >> 26: psx_cpu_i_swr(cpu); return 2;
        case 0xc0000000 >> 26: psx_cpu_i_lwc0(cpu); return 2;
        case 0xc4000000 >> 26: psx_cpu_i_lwc1(cpu); return 2;
        case 0xc8000000 >> 26: psx_cpu_i_lwc2(cpu); return 2;
        case 0xcc000000 >> 26: psx_cpu_i_lwc3(cpu); return 2;
        case 0xe0000000 >> 26: psx_cpu_i_swc0(cpu); return 2;
        case 0xe4000000 >> 26: psx_cpu_i_swc1(cpu); return 2;
        case 0xe8000000 >> 26: psx_cpu_i_swc2(cpu); return 2;
        case 0xec000000 >> 26: psx_cpu_i_swc3(cpu); return 2;
    }

    return 0;
}

#undef R_R0
#undef R_A0
#undef R_RA

#undef OP
#undef S
#undef T
#undef D
#undef IMM5
#undef CMT
#undef SOP
#undef IMM26
#undef IMM16
#undef IMM16S

#undef TRACE_M
#undef TRACE_I16S
#undef TRACE_I16D
#undef TRACE_I5D
#undef TRACE_I26
#undef TRACE_RT
#undef TRACE_C0M
#undef TRACE_C2M
#undef TRACE_C2MC
#undef TRACE_B
#undef TRACE_RS
#undef TRACE_MTF
#undef TRACE_RD
#undef TRACE_MD
#undef TRACE_I20
#undef TRACE_N

#undef DO_PENDING_LOAD

#undef DEBUG_ALL

#undef SE8
#undef SE16
