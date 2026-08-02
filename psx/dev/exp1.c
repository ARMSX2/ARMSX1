#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../log.h"
#include "exp1.h"

psx_exp1_t* psx_exp1_create(void) {
    return (psx_exp1_t*)malloc(sizeof(psx_exp1_t));
}

int psx_exp1_init(psx_exp1_t* exp1, psx_mc1_t* mc1, const char* path) {
    memset(exp1, 0, sizeof(psx_exp1_t));

    exp1->io_base = PSX_EXP1_BEGIN;
    exp1->io_size = PSX_EXP1_SIZE;

    exp1->mc1 = mc1;
    exp1->rom = (uint8_t*)malloc(PSX_EXP1_SIZE);

    memset(exp1->rom, 0xff, PSX_EXP1_SIZE);

    if (path)
        return psx_exp1_load(exp1, path);

    return 0;
}

int psx_exp1_load(psx_exp1_t* exp1, const char* path) {
    if (!path)
        return 0;

    FILE* file = fopen(path, "rb");

    if (!file)
        return 1;

    if (!fread(exp1->rom, 1, PSX_EXP1_SIZE, file))
        return 2;

    fclose(file);

    return 0;
}

uint32_t psx_exp1_read32(psx_exp1_t* exp1, uint32_t offset) {
    return *((uint32_t*)(exp1->rom + offset));
}

uint16_t psx_exp1_read16(psx_exp1_t* exp1, uint32_t offset) {
    return *((uint16_t*)(exp1->rom + offset));
}

uint8_t psx_exp1_read8(psx_exp1_t* exp1, uint32_t offset) {
    return exp1->rom[offset];
}

void psx_exp1_write32(psx_exp1_t* exp1, uint32_t offset, uint32_t value) {
    log_warn("Unhandled 32-bit EXP1 write at offset %08x (%08x)", offset, value);
}

void psx_exp1_write16(psx_exp1_t* exp1, uint32_t offset, uint16_t value) {
    log_warn("Unhandled 16-bit EXP1 write at offset %08x (%04x)", offset, value);
}

void psx_exp1_write8(psx_exp1_t* exp1, uint32_t offset, uint8_t value) {
    log_warn("Unhandled 8-bit EXP1 write at offset %08x (%02x)", offset, value);
}

void psx_exp1_destroy(psx_exp1_t* exp1) {
    free(exp1->rom);
    free(exp1);
}