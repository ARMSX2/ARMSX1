#ifndef BUS_H
#define BUS_H

#include <stdint.h>

#include "state.h"

struct psx_bus_t;

typedef struct psx_bus_t psx_bus_t;
typedef void (*psx_bus_write_observer_t)(void*, uint32_t, uint32_t);

psx_bus_t* psx_bus_create(void);
void psx_bus_init(psx_bus_t*);
uint32_t psx_bus_read32(psx_bus_t*, uint32_t);
uint16_t psx_bus_read16(psx_bus_t*, uint32_t);
uint8_t psx_bus_read8(psx_bus_t*, uint32_t);
void psx_bus_write32(psx_bus_t*, uint32_t, uint32_t);
void psx_bus_write16(psx_bus_t*, uint32_t, uint32_t);
void psx_bus_write8(psx_bus_t*, uint32_t, uint32_t);
uint32_t psx_bus_get_access_cycles(psx_bus_t*);
uint32_t psx_bus_physical_address(uint32_t);
void psx_bus_set_write_observer(psx_bus_t*, psx_bus_write_observer_t, void*);
/* Only access_cycles is machine state. The device pointers and the write
   observer are host wiring and are left in place across a load. */
void psx_bus_save_state(psx_bus_t*, psx_state_writer_t*);
int psx_bus_load_state(psx_bus_t*, psx_state_reader_t*);
void psx_bus_destroy(psx_bus_t*);

#endif
