#ifndef SCRATCHPAD_H
#define SCRATCHPAD_H

#include <stdint.h>

#include "mc1.h"
#include "../state.h"

#define PSX_SCRATCHPAD_BEGIN 0x1f800000
#define PSX_SCRATCHPAD_SIZE  0x400
#define PSX_SCRATCHPAD_END   0x1f8003ff

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint8_t* buf;
} psx_scratchpad_t;

psx_scratchpad_t* psx_scratchpad_create(void);
void psx_scratchpad_init(psx_scratchpad_t*);
uint32_t psx_scratchpad_read32(psx_scratchpad_t*, uint32_t);
uint16_t psx_scratchpad_read16(psx_scratchpad_t*, uint32_t);
uint8_t psx_scratchpad_read8(psx_scratchpad_t*, uint32_t);
void psx_scratchpad_write32(psx_scratchpad_t*, uint32_t, uint32_t);
void psx_scratchpad_write16(psx_scratchpad_t*, uint32_t, uint16_t);
void psx_scratchpad_write8(psx_scratchpad_t*, uint32_t, uint8_t);
/* The 1 KiB D-cache-as-scratchpad image. buf is a pointer and is not saved. */
void psx_scratchpad_save_state(psx_scratchpad_t*, psx_state_writer_t*);
int psx_scratchpad_load_state(psx_scratchpad_t*, psx_state_reader_t*);
void psx_scratchpad_destroy(psx_scratchpad_t*);

#endif