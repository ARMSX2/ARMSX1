#ifndef MC3_H
#define MC3_H

#include <stdint.h>

#include "../state.h"

#define PSX_MC3_BEGIN 0xfffe0130
#define PSX_MC3_SIZE  0x4
#define PSX_MC3_END   0xfffe0133

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint32_t cache_control;
} psx_mc3_t;

psx_mc3_t* psx_mc3_create(void);
void psx_mc3_init(psx_mc3_t*);
uint32_t psx_mc3_read32(psx_mc3_t*, uint32_t);
uint16_t psx_mc3_read16(psx_mc3_t*, uint32_t);
uint8_t psx_mc3_read8(psx_mc3_t*, uint32_t);
void psx_mc3_write32(psx_mc3_t*, uint32_t, uint32_t);
void psx_mc3_write16(psx_mc3_t*, uint32_t, uint16_t);
void psx_mc3_write8(psx_mc3_t*, uint32_t, uint8_t);
/* CACHE_CONTROL (FFFE0130h). */
void psx_mc3_save_state(psx_mc3_t*, psx_state_writer_t*);
int psx_mc3_load_state(psx_mc3_t*, psx_state_reader_t*);
void psx_mc3_destroy(psx_mc3_t*);

#endif