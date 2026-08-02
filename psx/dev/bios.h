#ifndef BIOS_H
#define BIOS_H

#include <stdint.h>

#include "../log.h"

#define PSX_BIOS_SIZE   0x80000
#define PSX_BIOS_BEGIN  0x1fc00000
#define PSX_BIOS_END    0x1fc7ffff

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    uint8_t* buf;
} psx_bios_t;

psx_bios_t* psx_bios_create(void);
void psx_bios_init(psx_bios_t*);
int psx_bios_load(psx_bios_t*, const char*);
uint32_t psx_bios_read32(psx_bios_t*, uint32_t);
uint16_t psx_bios_read16(psx_bios_t*, uint32_t);
uint8_t psx_bios_read8(psx_bios_t*, uint32_t);
void psx_bios_write32(psx_bios_t*, uint32_t, uint32_t);
void psx_bios_write16(psx_bios_t*, uint32_t, uint16_t);
void psx_bios_write8(psx_bios_t*, uint32_t, uint8_t);
void psx_bios_destroy(psx_bios_t*);

#endif