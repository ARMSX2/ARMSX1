#include "psx/dev/spu.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void spu_kon(psx_spu_t*, uint32_t);
void psx_ic_irq(psx_ic_t* ic, int irq) { (void)ic; (void)irq; }

int main(void) {
    psx_spu_t* s = calloc(1, sizeof(*s));
    assert(s);
    s->ram = calloc(1, 0x80000);
    assert(s->ram);
    s->voice[0].adsaddr = 0x200;
    // A silent block with prediction enabled exposes stale decoder history.
    s->ram[0x1000] = 0x1c;
    s->data[1].counter = 12345;
    for (unsigned restart = 0; restart < 2; ++restart) {
        s->data[0].playing = restart;
        s->data[0].counter = (27u << 12) | 0xabc;
        s->data[0].prev_sample_index = 27;
        s->data[0].h[0] = 30000;
        s->data[0].h[1] = -20000;
        memset(s->data[0].s, 0x55, sizeof(s->data[0].s));
        s->voice[0].envcvol = 0x7fff;
        spu_kon(s, 1);
        assert(s->data[0].playing);
        assert(s->data[0].counter == 0 && s->data[0].prev_sample_index == 0);
        assert(s->voice[0].envcvol == 0 && s->data[0].cvol == 0);
        for (unsigned i = 0; i < 28; ++i) assert(s->data[0].buf[i] == 0);
        for (unsigned i = 0; i < 4; ++i) assert(s->data[0].s[i] == 0);
        assert(s->data[1].counter == 12345);
    }
    free(s->ram);
    free(s);
    puts("SPU voice restart tests passed");
}
