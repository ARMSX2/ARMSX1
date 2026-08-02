#ifndef EXP1_H
#define EXP1_H

#include <stdint.h>

#include "mc1.h"

#define PSX_EXP1_BEGIN 0x1f000000
#define PSX_EXP1_SIZE  0x80000
#define PSX_EXP1_END   0x1f080000

typedef struct {
    uint32_t bus_delay;
    uint32_t io_base, io_size;

    psx_mc1_t* mc1;
    uint8_t* rom;
} psx_exp1_t;

psx_exp1_t* psx_exp1_create(void);
int psx_exp1_init(psx_exp1_t*, psx_mc1_t*, const char*);
int psx_exp1_load(psx_exp1_t*, const char*);
uint32_t psx_exp1_read32(psx_exp1_t*, uint32_t);
uint16_t psx_exp1_read16(psx_exp1_t*, uint32_t);
uint8_t psx_exp1_read8(psx_exp1_t*, uint32_t);
void psx_exp1_write32(psx_exp1_t*, uint32_t, uint32_t);
void psx_exp1_write16(psx_exp1_t*, uint32_t, uint16_t);
void psx_exp1_write8(psx_exp1_t*, uint32_t, uint8_t);
void psx_exp1_destroy(psx_exp1_t*);

#endif