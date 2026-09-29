#include "psx/dev/cdrom/disc.h"
#include "psx/perf.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#undef puts
static int fake_read(void* context, uint32_t lba, void* out) {
    assert(context == (void*)123);
    if(lba==42) usleep(3000);
    *(uint8_t*)out=(uint8_t)lba;
    return 7;
}
int main(void) {
    psx_disc_t d={0}; d.udata=(void*)123;d.read_sector=fake_read;
    uint8_t out=0;
    g_psx_audio_diag_enabled=0;
    assert(psx_disc_read(&d,41,&out)==7 && out==41);
    assert(g_psx_disc_timing.reads==0);
    g_psx_audio_diag_enabled=1;
    assert(psx_disc_read(&d,42,&out)==7 && out==42);
    assert(g_psx_disc_timing.reads==1);
    assert(g_psx_disc_timing.total_ns>=3000000);
    assert(g_psx_disc_timing.max_ns==g_psx_disc_timing.total_ns);
    assert(g_psx_disc_timing.slowest_lba==42);
    assert(psx_disc_read(NULL,0,&out)==0 && g_psx_disc_timing.reads==1);
    puts("Disc timing probe tests passed");
}
