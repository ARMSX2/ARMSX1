#ifndef PSX_INSTRUCTION_FETCH_H
#define PSX_INSTRUCTION_FETCH_H

#include <string.h>
#include "bus.h"
#include "bus_init.h"

/* Instruction reads overwhelmingly target RAM or BIOS. Keep those reads in
   the dispatch path, while preserving device dispatch for unusual addresses.
   Read the current bytes on every fetch: RAM mirrors, DMA writes and state
   restores must never reuse stale instruction data. */
static inline uint32_t psx_instruction_fetch(psx_bus_t* bus, uint32_t address) {
    const uint32_t physical = psx_bus_physical_address(address);
    uint32_t value;
    if (physical & 3u) return psx_bus_read32(bus, address);
    if (physical >= bus->ram->io_base &&
        physical - bus->ram->io_base < bus->ram->io_size) {
        uint32_t offset = physical - bus->ram->io_base;
        bus->access_cycles = bus->ram->bus_delay;
        if (((bus->ram->mc2->ram_size >> 9) & 7) == 3 && offset >= 0x400000)
            return 0xffffffffu;
        offset &= bus->ram->size - 1;
        memcpy(&value, bus->ram->buf + offset, sizeof(value));
        return value;
    }
    if (physical >= bus->bios->io_base &&
        physical - bus->bios->io_base < bus->bios->io_size) {
        bus->access_cycles = bus->bios->bus_delay;
        memcpy(&value, bus->bios->buf + physical - bus->bios->io_base, sizeof(value));
        return value;
    }
    return psx_bus_read32(bus, address);
}
#endif
