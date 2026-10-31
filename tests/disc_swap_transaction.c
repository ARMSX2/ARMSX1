#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../psx/psx.h"

// Link this against psx.c with function sections and --gc-sections. Stub only
// the drive IO so both success/failure exercise the real psx_swap_disc logic.
static int open_ok;
static int destroyed;
static int rewinds;
static psx_cdrom_t* mounted;
psx_cdrom_t* psx_cdrom_create(void) { return calloc(1, sizeof(psx_cdrom_t)); }
void psx_cdrom_init(psx_cdrom_t* drive, psx_ic_t* ic) { drive->ic = ic; }
int psx_cdrom_open(psx_cdrom_t* drive, const char* path) { (void)drive; (void)path; return open_ok; }
void psx_cdrom_destroy(psx_cdrom_t* drive) { ++destroyed; free(drive); }
void psx_bus_init_cdrom(psx_bus_t* bus, psx_cdrom_t* drive) { (void)bus; mounted = drive; }
void psx_rewind_reset(void) { ++rewinds; }

int main(void) {
    psx_t machine = {0};
    psx_cpu_t cpu = {0};
    machine.cpu = &cpu;
    memset(&cpu, 0x35, sizeof(cpu));
    psx_cpu_t before = cpu;
    machine.cdrom = psx_cdrom_create();
    machine.cdrom->ier = 0x1f;
    machine.cdrom->mode = 0x80;
    machine.cdrom->region = 2;
    psx_cdrom_t* original = machine.cdrom;
    assert(!psx_swap_disc(NULL, "disc2.chd"));
    assert(!psx_swap_disc(&machine, ""));
    assert(!psx_swap_disc(&machine, "missing.chd"));
    assert(machine.cdrom == original && destroyed == 1 && rewinds == 0);
    assert(memcmp(&cpu, &before, sizeof(cpu)) == 0);
    open_ok = 1;
    assert(psx_swap_disc(&machine, "disc2.chd"));
    assert(machine.cdrom != original && mounted == machine.cdrom);
    assert(destroyed == 2 && rewinds == 1);
    assert(machine.cdrom->ier == 0x1f && machine.cdrom->mode == 0x80 && machine.cdrom->region == 2);
    assert(machine.cdrom->shell_open_latched);
    assert(memcmp(&cpu, &before, sizeof(cpu)) == 0);
    psx_cdrom_destroy(machine.cdrom);
    return 0;
}
