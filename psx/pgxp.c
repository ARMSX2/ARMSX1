#include "pgxp.h"
#include "dev/gpu.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

#define PGXP_RAM_BYTES   0x200000u
#define PGXP_RAM_WORDS   (PGXP_RAM_BYTES >> 2)          /* 524288 */
#define PGXP_SPAD_WORDS  (0x400u >> 2)                  /* 256 */
#define PGXP_TOTAL_WORDS (PGXP_RAM_WORDS + PGXP_SPAD_WORDS)

#define PGXP_ADDR_NONE   0xffffffffu

/* GP0 buffered commands top out at 12 words (GP0(3E) shaded textured quad);
   gpu->buf[] itself is 16 deep. The mask also keeps the (pre-existing,
   unrelated) polyline buf_index overrun from walking outside our table. */
#define PGXP_GP0_SLOTS   16

typedef struct {
    float    x, y, w;
    uint32_t value;   /* the truncated 32-bit word these floats correspond to */
    uint32_t valid;
} pgxp_entry_t;

int g_psx_pgxp_enabled = 0;

static pgxp_entry_t* g_mem = NULL;               /* RAM + scratchpad shadow */
static pgxp_entry_t  g_fifo[3];                  /* SXY0/1/2 shadow */
static pgxp_entry_t  g_regs[32];                 /* CPU register shadow */
static pgxp_entry_t  g_load;
static pgxp_entry_t  g_store;
static uint32_t     g_load_reg;
static uint32_t      g_gp0_addr[PGXP_GP0_SLOTS]; /* source addr per gpu->buf slot */
static uint32_t      g_pending_addr = PGXP_ADDR_NONE;

/* ---- helpers ---------------------------------------------------------------------- */

/* Map a CPU/DMA address to its cache entry, or NULL when the address is not
   RAM or scratchpad (MMIO, BIOS, cache-isolated garbage...). Segment bits are
   stripped so KUSEG/KSEG0/KSEG1 aliases and the 4x RAM mirror all land on one
   entry — the same folding psx_bus_read32() itself performs. */
static inline pgxp_entry_t* pgxp_mem_entry(uint32_t addr) {
    if (!g_mem || (addr >= 0x20000000u && addr < 0x80000000u) || addr >= 0xc0000000u)
        return NULL;
    uint32_t phys = addr & 0x1fffffffu;

    if (phys < 0x00800000u)
        return &g_mem[(phys & (PGXP_RAM_BYTES - 1u)) >> 2];

    if ((phys & 0xfffffc00u) == 0x1f800000u)
        return &g_mem[PGXP_RAM_WORDS + ((phys & 0x3ffu) >> 2)];

    return NULL;
}

/* SXY register index (0..2) for a GTE data register, or -1. Register 15 is the
   SXY2 mirror on reads, which is the only context this is used in. */
static inline int pgxp_sxy_index(uint32_t reg) {
    if ((reg == 14) || (reg == 15))
        return 2;

    if ((reg == 12) || (reg == 13))
        return (int)reg - 12;

    return -1;
}

/* Store `src` at `addr` when the truncated words agree; otherwise make sure no
   stale precision survives at that address. Shared by SWC2 and SW. */
static inline void pgxp_mem_store(uint32_t addr, uint32_t value, const pgxp_entry_t* src) {
    pgxp_entry_t* m = pgxp_mem_entry(addr);

    if (!m)
        return;

    if (src && src->valid && (src->value == value)) {
        *m = *src;

        return;
    }

    /* Any other write: overwrite. Keeping an entry whose word still matches
       would also be safe (identical truncation), but "a write you did not
       produce kills the entry" is the simpler invariant to reason about. */
    m->valid = 0;
    m->value = value;
}

static void pgxp_clear_runtime(void) {
    int i;

    memset(g_fifo, 0, sizeof(g_fifo));
    memset(g_regs, 0, sizeof(g_regs));
    memset(&g_load, 0, sizeof(g_load));
    memset(&g_store, 0, sizeof(g_store));
    g_load_reg = 0;

    for (i = 0; i < PGXP_GP0_SLOTS; i++)
        g_gp0_addr[i] = PGXP_ADDR_NONE;

    g_pending_addr = PGXP_ADDR_NONE;
}

/* ---- control ---------------------------------------------------------------------- */

void psx_pgxp_set_enabled(int enabled) {
    enabled = enabled ? 1 : 0;

    if (enabled == g_psx_pgxp_enabled)
        return;

    if (enabled) {
        if (!g_mem) {
            g_mem = (pgxp_entry_t*)calloc(PGXP_TOTAL_WORDS, sizeof(pgxp_entry_t));

            if (!g_mem) {
                log_error("PGXP: vertex cache allocation failed; staying disabled");

                return;
            }
        }

        psx_pgxp_reset();
        g_psx_pgxp_enabled = 1;

        log_info("PGXP: enabled (address cache %u KiB)",
                 (unsigned)((PGXP_TOTAL_WORDS * sizeof(pgxp_entry_t)) >> 10));
    } else {
        g_psx_pgxp_enabled = 0;

        log_info("PGXP: disabled");
    }
}

int psx_pgxp_enabled(void) {
    return g_psx_pgxp_enabled;
}

void psx_pgxp_reset(void) {
    pgxp_clear_runtime();

    if (g_mem)
        memset(g_mem, 0, PGXP_TOTAL_WORDS * sizeof(pgxp_entry_t));
}

/* ---- capture ---------------------------------------------------------------------- */

void psx_pgxp_gte_vertex(uint32_t sxy, float fx, float fy, float fw) {
    if (!g_psx_pgxp_enabled)
        return;

    /* Mirror gte_clamp_sxy()'s 11-bit saturation so a saturated integer pairs
       with an equally saturated float instead of one far off screen. */
    if (fx < -1024.0f) fx = -1024.0f;
    if (fx >  1023.0f) fx =  1023.0f;
    if (fy < -1024.0f) fy = -1024.0f;
    if (fy >  1023.0f) fy =  1023.0f;

    g_fifo[0] = g_fifo[1];
    g_fifo[1] = g_fifo[2];

    g_fifo[2].x = fx;
    g_fifo[2].y = fy;
    g_fifo[2].w = fw;
    g_fifo[2].value = sxy;
    g_fifo[2].valid = 1;
}

void psx_pgxp_gte_reg_write(uint32_t reg, uint32_t value) {
    if (!g_psx_pgxp_enabled)
        return;

    (void)value;

    switch (reg) {
        case 12: g_fifo[0].valid = 0; break;
        case 13: g_fifo[1].valid = 0; break;
        case 14: g_fifo[2].valid = 0; break;
        case 15:
            /* SXYP: hardware shifts the FIFO and lands the new word in SXY2
               (gte_handle_sxyp_write). We shift alongside it; the pushed word
               has no precise counterpart. */
            g_fifo[0] = g_fifo[1];
            g_fifo[1] = g_fifo[2];
            g_fifo[2].valid = 0;
            break;

        default: break;
    }
}

/* ---- tracking --------------------------------------------------------------------- */

void psx_pgxp_cpu_swc2(uint32_t addr, uint32_t value, uint32_t reg) {
    int sxy = pgxp_sxy_index(reg);

    pgxp_mem_store(addr, value, (sxy >= 0) ? &g_fifo[sxy] : NULL);
}

void psx_pgxp_cpu_mfc2(uint32_t rt, uint32_t value, uint32_t reg) {
    int sxy = pgxp_sxy_index(reg);
    pgxp_entry_t* r = &g_load;
    g_load_reg = rt & 31u;

    if ((sxy >= 0) && g_fifo[sxy].valid && (g_fifo[sxy].value == value)) {
        *r = g_fifo[sxy];

        return;
    }

    r->valid = 0;
}

void psx_pgxp_cpu_sw(uint32_t addr, uint32_t value, uint32_t rt) {
    (void)rt;
    pgxp_mem_store(addr, value, &g_store);
}

void psx_pgxp_cpu_store_begin(uint32_t rt) {
    g_store = g_regs[rt & 31u];
    if (!rt)
        g_store.valid = 0;
}

void psx_pgxp_cpu_load_commit(uint32_t rt, uint32_t value) {
    pgxp_entry_t* r = &g_regs[rt & 31u];
    if (rt && g_load_reg == rt && g_load.valid && g_load.value == value)
        *r = g_load;
    else
        r->valid = 0;
    g_load.valid = 0;
}

void psx_pgxp_cpu_lw(uint32_t rt, uint32_t addr, uint32_t value) {
    const pgxp_entry_t* m = pgxp_mem_entry(addr);
    g_load_reg = rt & 31u;
    if (m && m->valid && m->value == value)
        g_load = *m;
    else
        g_load.valid = 0;
}

void psx_pgxp_cpu_instruction(uint32_t opcode) {
    const unsigned op = opcode >> 26;
    const unsigned rs = (opcode >> 21) & 31u;
    const unsigned rt = (opcode >> 16) & 31u;
    const unsigned rd = (opcode >> 11) & 31u;
    const unsigned fn = opcode & 63u;
    unsigned dest = 0;
    if (op == 0) {
        if (fn <= 7 || fn == 9 || fn == 16 || fn == 18 || (fn >= 32 && fn <= 43))
            dest = rd;
    } else if (op == 3 || (op == 1 && (rt == 16 || rt == 17))) {
        dest = 31;
    } else if (op >= 8 && op <= 15) {
        dest = rt;
    }
    g_regs[dest].valid = 0;
    if ((op >= 32 && op <= 38 && op != 35) ||
        ((op == 16 || op == 18) && rs <= 2 && !(op == 18 && rs == 0)))
        g_load.valid = 0;
}

void psx_pgxp_memory_written(uint32_t addr, uint32_t size) {
    for (uint32_t offset = 0; offset < size; ++offset) {
        pgxp_entry_t* m = pgxp_mem_entry(addr + offset);
        if (m)
            m->valid = 0;
    }
}

/* ---- submission ------------------------------------------------------------------- */

void psx_pgxp_note_gp0_word(uint32_t addr) {
    g_pending_addr = addr;
}

void psx_pgxp_gp0_slot(int buf_index) {
    g_gp0_addr[buf_index & (PGXP_GP0_SLOTS - 1)] = g_pending_addr;
    g_pending_addr = PGXP_ADDR_NONE;
}

void psx_pgxp_gp0_discard(void) {
    g_pending_addr = PGXP_ADDR_NONE;
}

/* ---- lookup ----------------------------------------------------------------------- */

void psx_pgxp_poly_vertex(struct vertex_t* v, uint32_t word, int buf_index) {
    uint32_t addr;
    const pgxp_entry_t* m;

    v->precise_valid = 0;
    v->px = 0.0f;
    v->py = 0.0f;
    v->pw = 1.0f;

    if (!g_psx_pgxp_enabled || !g_mem)
        return;

    addr = g_gp0_addr[buf_index & (PGXP_GP0_SLOTS - 1)];

    if (addr == PGXP_ADDR_NONE)
        return;

    m = pgxp_mem_entry(addr);

    if (!m)
        return;

    /* The validation rule. Full 32-bit compare: if the game so much as nudged
       one half of the word since the GTE produced it, the precision is stale
       and the vertex stays integer. */
    if (m->valid && (m->value == word)) {
        v->px = m->x;
        v->py = m->y;
        v->pw = m->w;
        v->precise_valid = 1;
    }
}
