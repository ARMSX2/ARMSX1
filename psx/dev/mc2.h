#ifndef MC2_H
#define MC2_H

#include <stdint.h>

#include "../state.h"

#define PSX_MC2_BEGIN 0x1f801060
#define PSX_MC2_SIZE  0x4
#define PSX_MC2_END   0x1F801063

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint32_t ram_size;
} psx_mc2_t;

psx_mc2_t* psx_mc2_create(void);
void psx_mc2_init(psx_mc2_t*);
uint32_t psx_mc2_read32(psx_mc2_t*, uint32_t);
uint16_t psx_mc2_read16(psx_mc2_t*, uint32_t);
uint8_t psx_mc2_read8(psx_mc2_t*, uint32_t);
void psx_mc2_write32(psx_mc2_t*, uint32_t, uint32_t);
void psx_mc2_write16(psx_mc2_t*, uint32_t, uint16_t);
void psx_mc2_write8(psx_mc2_t*, uint32_t, uint8_t);
/* RAM_SIZE. Drives psx_ram_read*'s 4 MB mirror decision, so it is real state. */
void psx_mc2_save_state(psx_mc2_t*, psx_state_writer_t*);
int psx_mc2_load_state(psx_mc2_t*, psx_state_reader_t*);
void psx_mc2_destroy(psx_mc2_t*);

#endif