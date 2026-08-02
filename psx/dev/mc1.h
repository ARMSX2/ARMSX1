#ifndef MC1_H
#define MC1_H

#include <stdint.h>

#include "../state.h"

#define PSX_MC1_BEGIN 0x1f801000
#define PSX_MC1_SIZE  0x24
#define PSX_MC1_END   0x1f801023

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint32_t exp1_base;
    uint32_t exp2_base;
    uint32_t exp1_delay;
    uint32_t exp3_delay;
    uint32_t bios_delay;
    uint32_t spu_delay;
    uint32_t cdrom_delay;
    uint32_t exp2_delay;
    uint32_t com_delay;
} psx_mc1_t;

typedef struct {
    int fst;
    int seq;
} psx_access_delay_t;

psx_mc1_t* psx_mc1_create(void);
void psx_mc1_init(psx_mc1_t*);
uint32_t psx_mc1_read32(psx_mc1_t*, uint32_t);
uint16_t psx_mc1_read16(psx_mc1_t*, uint32_t);
uint8_t psx_mc1_read8(psx_mc1_t*, uint32_t);
void psx_mc1_write32(psx_mc1_t*, uint32_t, uint32_t);
void psx_mc1_write16(psx_mc1_t*, uint32_t, uint16_t);
void psx_mc1_write8(psx_mc1_t*, uint32_t, uint8_t);
/* The nine memory-control latches. */
void psx_mc1_save_state(psx_mc1_t*, psx_state_writer_t*);
int psx_mc1_load_state(psx_mc1_t*, psx_state_reader_t*);
void psx_mc1_destroy(psx_mc1_t*);
uint32_t psx_mc1_get_bios_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_ram_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_dma_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_exp1_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_mc1_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_mc2_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_mc3_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_ic_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_scratchpad_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_gpu_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_spu_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_timer_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_cdrom_read_delay(psx_mc1_t*);
uint32_t psx_mc1_get_pad_read_delay(psx_mc1_t*);

uint32_t psx_mc1_get_bios_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_ram_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_dma_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_exp1_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_mc1_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_mc2_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_mc3_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_ic_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_scratchpad_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_gpu_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_spu_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_timer_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_cdrom_write_delay(psx_mc1_t*);
uint32_t psx_mc1_get_pad_write_delay(psx_mc1_t*);

#endif