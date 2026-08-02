#include "bios.h"
#include "../log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

psx_bios_t* psx_bios_create(void) {
    return (psx_bios_t*)malloc(sizeof(psx_bios_t));
}

void psx_bios_init(psx_bios_t* bios) {
    memset(bios, 0, sizeof(psx_bios_t));

    bios->io_base = PSX_BIOS_BEGIN;
    bios->io_size = PSX_BIOS_SIZE;
    bios->bus_delay = 18;
}

int psx_bios_load(psx_bios_t* bios, const char* path) {
    if (!path)
        return 0;

    FILE* file = fopen(path, "rb");

    if (!file) {
        log_error("Failed to open BIOS at '%s': %s", path, strerror(errno));
        return 1;
    }

    // Almost all PS1 BIOS ROMs are 512 KiB in size.
    // There's (at least) one exception, and that is SCPH-5903.
    // This is a special asian model PS1 that had built-in support
    // for Video CD (VCD) playback. Its BIOS is double the normal
    // size
    fseek(file, 0, SEEK_END);

    size_t size = ftell(file);

    fseek(file, 0, SEEK_SET);

    bios->buf = malloc(size);
    bios->io_size = size;

    if (!fread(bios->buf, 1, size, file)) {
        log_error("Failed to read BIOS at '%s': %s", path, strerror(errno));
        return 2;
    }

    fclose(file);

    return 0;
}

uint32_t psx_bios_read32(psx_bios_t* bios, uint32_t offset) {
    return *((uint32_t*)(bios->buf + offset));
}

uint16_t psx_bios_read16(psx_bios_t* bios, uint32_t offset) {
    return *((uint16_t*)(bios->buf + offset));
}

uint8_t psx_bios_read8(psx_bios_t* bios, uint32_t offset) {
    return bios->buf[offset];
}

void psx_bios_write32(psx_bios_t* bios, uint32_t offset, uint32_t value) {
    log_warn("Unhandled 32-bit BIOS write at offset %08x (%08x)", offset, value);
}

void psx_bios_write16(psx_bios_t* bios, uint32_t offset, uint16_t value) {
    log_warn("Unhandled 16-bit BIOS write at offset %08x (%04x)", offset, value);
}

void psx_bios_write8(psx_bios_t* bios, uint32_t offset, uint8_t value) {
    log_warn("Unhandled 8-bit BIOS write at offset %08x (%02x)", offset, value);
}

void psx_bios_destroy(psx_bios_t* bios) {
    free(bios->buf);
    free(bios);
}
