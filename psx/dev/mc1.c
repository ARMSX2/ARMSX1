#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "mc1.h"
#include "../log.h"

/*
  0-3   Write Delay        (00h..0Fh=01h..10h Cycles)
  4-7   Read Delay         (00h..0Fh=01h..10h Cycles)
  8     Recovery Period    (0=No, 1=Yes, uses COM0 timings)
  9     Hold Period        (0=No, 1=Yes, uses COM1 timings)
  10    Floating Period    (0=No, 1=Yes, uses COM2 timings)
  11    Pre-strobe Period  (0=No, 1=Yes, uses COM3 timings)
  12    Data Bus-width     (0=8bits, 1=16bits)
  13    Auto Increment     (0=No, 1=Yes)
  14-15 Unknown (R/W)
  16-20 Memory Window Size (1 SHL N bytes) (0..1Fh = 1 byte ... 2 gigabytes)
  21-23 Unknown (always zero)
  24-27 DMA timing override
  28    Address error flag. Write 1 to it to clear it.
  29    DMA timing select  (0=use normal timings, 1=use bits 24-27)
  30    Wide DMA           (0=use bit 12, 1=override to full 32 bits)
  31    Wait               (1=wait on external device before being ready)
*/

#define DEFAULT_DLY 2

// #define WRITE_DLY(dev) ((mc1-> ## dev ## _delay & 0xf) + 1)
// #define READ_DLY(dev) (((mc1-> ## dev ## _delay >> 4) & 0xf) + 1)
// #define USE_COM0(dev) (mc1-> ## dev ## _delay >> 8)
// #define USE_COM1(dev) (mc1-> ## dev ## _delay >> 9)
// #define USE_COM2(dev) (mc1-> ## dev ## _delay >> 10)
// #define USE_COM3(dev) (mc1-> ## dev ## _delay >> 11)

// #define COM0_DLY (mc1->com_delay )

// psx_access_delay_t mc1_get_com_delay(int uc0, int uc1, int uc2, int uc3) {
//     int fst = 0, seq = 0, min = 0;
// }

// #define COM_DLY(dev)

psx_mc1_t* psx_mc1_create(void) {
    return (psx_mc1_t*)calloc(1, sizeof(psx_mc1_t));
}

/*
  1F801000h 4    Expansion 1 Base Address (usually 1F000000h)
  1F801004h 4    Expansion 2 Base Address (usually 1F802000h)
  1F801008h 4    Expansion 1 Delay/Size (usually 0013243Fh; 512Kbytes 8bit-bus)
  1F80100Ch 4    Expansion 3 Delay/Size (usually 00003022h; 1 byte)
  1F801010h 4    BIOS ROM    Delay/Size (usually 0013243Fh; 512Kbytes 8bit-bus)
  1F801014h 4    SPU_DELAY   Delay/Size (usually 200931E1h)
  1F801018h 4    CDROM_DELAY Delay/Size (usually 00020843h or 00020943h)
  1F80101Ch 4    Expansion 2 Delay/Size (usually 00070777h; 128-bytes 8bit-bus)
  1F801020h 4    COM_DELAY / COMMON_DELAY (00031125h or 0000132Ch or 00001325h)
*/
void psx_mc1_init(psx_mc1_t* mc1) {
    memset(mc1, 0, sizeof(psx_mc1_t));

    mc1->io_base = PSX_MC1_BEGIN;
    mc1->io_size = PSX_MC1_SIZE;

    mc1->exp1_base   = 0x1f000000;
    mc1->exp2_base   = 0x1f802000;
    mc1->exp1_delay  = 0x0013243f;
    mc1->exp3_delay  = 0x00003022;
    mc1->bios_delay  = 0x0013243f;
    mc1->spu_delay   = 0x200931e1;
    mc1->cdrom_delay = 0x00020843;
    mc1->exp2_delay  = 0x00070777;
    mc1->com_delay   = 0x00031125;
}

uint32_t psx_mc1_read32(psx_mc1_t* mc1, uint32_t offset) {
    switch (offset) {
        case 0x00: return mc1->exp1_base;
        case 0x04: return mc1->exp2_base;
        case 0x08: return mc1->exp1_delay;
        case 0x0c: return mc1->exp3_delay;
        case 0x10: return mc1->bios_delay;
        case 0x14: return mc1->spu_delay;
        case 0x18: return mc1->cdrom_delay;
        case 0x1c: return mc1->exp2_delay;
        case 0x20: return mc1->com_delay;
    }

    log_warn("Unhandled 32-bit MC1 read at offset %08x", offset);

    return 0x0;
}

uint16_t psx_mc1_read16(psx_mc1_t* mc1, uint32_t offset) {
    log_warn("Unhandled 16-bit MC1 read at offset %08x", offset);

    return 0x0;
}

uint8_t psx_mc1_read8(psx_mc1_t* mc1, uint32_t offset) {
    log_warn("Unhandled 8-bit MC1 read at offset %08x", offset);

    return 0x0;
}

void psx_mc1_write32(psx_mc1_t* mc1, uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: mc1->exp1_base = value; break;
        case 0x04: mc1->exp2_base = value; break;
        case 0x08: mc1->exp1_delay = value; break;
        case 0x0c: mc1->exp3_delay = value; break;
        case 0x10: mc1->bios_delay = value; break;
        case 0x14: mc1->spu_delay = value; break;
        case 0x18: mc1->cdrom_delay = value; break;
        case 0x1c: mc1->exp2_delay = value; break;
        case 0x20: mc1->com_delay = value; break;

        default: {
            log_warn("Unhandled 32-bit MC1 write at offset %08x (%08x)", offset, value);
        } break;
    }
}

void psx_mc1_write16(psx_mc1_t* mc1, uint32_t offset, uint16_t value) {
    log_warn("Unhandled 16-bit MC1 write at offset %08x (%04x)", offset, value);
}

void psx_mc1_write8(psx_mc1_t* mc1, uint32_t offset, uint8_t value) {
    log_warn("Unhandled 8-bit MC1 write at offset %08x (%02x)", offset, value);
}

uint32_t psx_mc1_get_bios_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_ram_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_dma_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_exp1_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_mc1_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_mc2_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_mc3_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_ic_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_scratchpad_read_delay(psx_mc1_t* mc1) {
    return 1;
}

uint32_t psx_mc1_get_gpu_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_spu_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_timer_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_cdrom_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_pad_read_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_bios_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_ram_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_dma_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_exp1_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_mc1_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_mc2_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_mc3_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_ic_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_scratchpad_write_delay(psx_mc1_t* mc1) {
    return 1;
}

uint32_t psx_mc1_get_gpu_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_spu_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_timer_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_cdrom_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

uint32_t psx_mc1_get_pad_write_delay(psx_mc1_t* mc1) {
    return DEFAULT_DLY;
}

void psx_mc1_save_state(psx_mc1_t* mc1, psx_state_writer_t* w) {
    psx_sw_u32(w, mc1->exp1_base);
    psx_sw_u32(w, mc1->exp2_base);
    psx_sw_u32(w, mc1->exp1_delay);
    psx_sw_u32(w, mc1->exp3_delay);
    psx_sw_u32(w, mc1->bios_delay);
    psx_sw_u32(w, mc1->spu_delay);
    psx_sw_u32(w, mc1->cdrom_delay);
    psx_sw_u32(w, mc1->exp2_delay);
    psx_sw_u32(w, mc1->com_delay);
}

int psx_mc1_load_state(psx_mc1_t* mc1, psx_state_reader_t* r) {
    mc1->exp1_base = psx_sr_u32(r);
    mc1->exp2_base = psx_sr_u32(r);
    mc1->exp1_delay = psx_sr_u32(r);
    mc1->exp3_delay = psx_sr_u32(r);
    mc1->bios_delay = psx_sr_u32(r);
    mc1->spu_delay = psx_sr_u32(r);
    mc1->cdrom_delay = psx_sr_u32(r);
    mc1->exp2_delay = psx_sr_u32(r);
    mc1->com_delay = psx_sr_u32(r);

    return r->error ? PSX_STATE_ERR_TRUNCATED : PSX_STATE_OK;
}

void psx_mc1_destroy(psx_mc1_t* mc1) {
    free(mc1);
}