#include "psx/dev/mcd.h"
#include <assert.h>
#include <stdio.h>

static unsigned char transfer(psx_mcd_t* c, unsigned char v) {
    psx_mcd_write(c, v);
    return psx_mcd_read(c);
}
static void start(psx_mcd_t* c, char mode, unsigned sector) {
    psx_mcd_reset(c);
    transfer(c, 0x81); transfer(c, mode);
    assert(transfer(c, 0) == 0x5a);
    assert(transfer(c, 0) == 0x5d);
    transfer(c, sector >> 8); transfer(c, sector);
}
int main(void) {
    psx_mcd_t* c = psx_mcd_create();
    assert(psx_mcd_init(c, NULL) == 0);
    for (unsigned sector = 0; sector < 1024; ++sector) {
        start(c, 'W', sector);
        unsigned checksum = (sector >> 8) ^ (sector & 255);
        for (unsigned i = 0; i < 128; ++i) {
            unsigned char byte = (unsigned char)(sector + (sector >> 8) * 17 + i);
            transfer(c, byte);
            checksum ^= byte;
        }
        transfer(c, checksum);
        assert(transfer(c, 0) == 0x5c);
        assert(transfer(c, 0) == 0x5d);
        assert(transfer(c, 0) == 'G');
    }
    for (unsigned sector = 0; sector < 1024; ++sector) {
        start(c, 'R', sector);
        assert(transfer(c, 0) == 0x5c);
        assert(transfer(c, 0) == 0x5d);
        transfer(c, 0); transfer(c, 0);
        for (unsigned i = 0; i < 128; ++i) {
            if (i == 63) {
                psx_state_writer_t w; psx_state_reader_t r;
                psx_sw_init(&w); psx_mcd_save_state(c, &w);
                psx_sr_init(&r, w.buf, w.size);
                assert(psx_mcd_load_state(c, &r) == PSX_STATE_OK);
                psx_sw_free(&w);
            }
            assert(transfer(c, 0) == (unsigned char)(sector + (sector >> 8) * 17 + i));
        }
    }
    psx_mcd_destroy(c);
    puts("Full-card write/read and mid-read restore: PASS (1024 sectors)");
    return 0;
}
