#ifndef PSX_H
#define PSX_H

#include "cpu.h"
#include "log.h"
#include "exe.h"

#include <stdint.h>

#define STR1(m) #m
#define STR(m) STR1(m)

#define PSXE_VERSION STR(REP_VERSION)
#define PSXE_COMMIT STR(REP_COMMIT_HASH)
#define PSXE_BUILD_OS STR(OS_INFO)

/* Named (rather than anonymous) so headers that only need a handle — state.h —
   can forward declare `struct psx_t` without pulling the whole core in. */
typedef struct psx_t {
    psx_bios_t* bios;
    psx_ram_t* ram;
    psx_dma_t* dma;
    psx_exp1_t* exp1;
    psx_exp2_t* exp2;
    psx_mc1_t* mc1;
    psx_mc2_t* mc2;
    psx_mc3_t* mc3;
    psx_ic_t* ic;
    psx_scratchpad_t* scratchpad;
    psx_gpu_t* gpu;
    psx_spu_t* spu;
    psx_bus_t* bus;
    psx_cpu_t* cpu;
    psx_timer_t* timer;
    psx_cdrom_t* cdrom;
    psx_pad_t* pad;
    psx_mdec_t* mdec;
} psx_t;

psx_t* psx_create(void);
int psx_init(psx_t*, const char*, const char*);
int psx_load_expansion(psx_t*, const char*);
int psx_load_bios(psx_t*, const char*);
void psx_hard_reset(psx_t*);
void psx_soft_reset(psx_t*);
/* Save states. Return PSX_STATE_OK (0) or a negative PSX_STATE_ERR_* code from
   psx/state.h; psx_state_strerror() turns one into a message. Both must run on
   the emulation thread — hosts on another thread go through
   psx_state_request_slot(). psx_save_state_to_memory() hands back a malloc()ed
   buffer the caller frees. */
int psx_load_state(psx_t*, const char*);
int psx_save_state(psx_t*, const char*);
int psx_load_state_from_memory(psx_t*, const void*, size_t);
int psx_save_state_to_memory(psx_t*, void**, size_t*);
/* Capture with options, and with buffer reuse. *io_data / *io_capacity may carry
   a buffer a previous call returned (NULL/0 for a fresh one); it is grown in
   place when needed and handed back updated, and the caller always frees it.
   Used by rewind/runahead, which capture continuously and must not churn the
   allocator — see psx/rewind.h. `flags` is a mask of PSX_STATE_SAVE_*. */
int psx_save_state_to_memory_ex(psx_t*, void** io_data, size_t* io_capacity,
                                size_t* out_size, unsigned flags);
void psx_load_exe(psx_t*, const char*);

/* CPU overclock, as a percentage of the real R3000A. 100 = stock. Clamped to 10..1000.
   Implemented by charging the devices fewer cycles per instruction — see psx.c. */
void psx_set_cpu_overclock(unsigned percent);
unsigned psx_get_cpu_overclock(void);
void psx_update(psx_t*);
void psx_run_frame(psx_t*);
void* psx_get_display_buffer(psx_t*);
void* psx_get_vram(psx_t*);
uint32_t psx_get_dmode_width(psx_t*);
uint32_t psx_get_dmode_height(psx_t*);
uint32_t psx_get_display_width(psx_t*);
uint32_t psx_get_display_height(psx_t*);
uint32_t psx_get_display_format(psx_t*);
double psx_get_display_aspect(psx_t*);
uint32_t* psx_take_screenshot(psx_t*);
int psx_swap_disc(psx_t*, const char*);
psx_bios_t* psx_get_bios(psx_t*);
psx_ram_t* psx_get_ram(psx_t*);
psx_dma_t* psx_get_dma(psx_t*);
psx_exp1_t* psx_get_exp1(psx_t*);
psx_exp2_t* psx_get_exp2(psx_t*);
psx_mc1_t* psx_get_mc1(psx_t*);
psx_mc2_t* psx_get_mc2(psx_t*);
psx_mc3_t* psx_get_mc3(psx_t*);
psx_ic_t* psx_get_ic(psx_t*);
psx_scratchpad_t* psx_get_scratchpad(psx_t*);
psx_gpu_t* psx_get_gpu(psx_t*);
psx_spu_t* psx_get_spu(psx_t*);
psx_bus_t* psx_get_bus(psx_t*);
psx_timer_t* psx_get_timer(psx_t*);
psx_cdrom_t* psx_get_cdrom(psx_t*);
psx_pad_t* psx_get_pad(psx_t*);
psx_mdec_t* psx_get_mdec(psx_t*);
psx_cpu_t* psx_get_cpu(psx_t*);
void psx_destroy(psx_t*);

#endif