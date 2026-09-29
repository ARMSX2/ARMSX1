#include "psx/dev/spu.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

void adsr_load_attack(psx_spu_t*, int);
void adsr_load_sustain(psx_spu_t*, int);
void spu_handle_adsr(psx_spu_t*, int);

int main(void) {
    psx_spu_t* spu = calloc(1, sizeof(*spu));
    assert(spu);
    // Sustain/release bits must not turn a linear attack into exponential.
    spu->voice[0].envctl1 = 0;
    spu->voice[0].envctl2 = 0xffff;
    adsr_load_attack(spu, 0);
    assert(spu->data[0].adsr_mode == 0);
    spu->voice[0].envctl1 = 0x8000;
    adsr_load_attack(spu, 0);
    assert(spu->data[0].adsr_mode == 1);
    // Decay can step below a threshold; preserve the actual level.
    spu->voice[0].envctl1 = 3;
    spu->data[0].cvol = 0x1f00;
    adsr_load_sustain(spu, 0);
    assert(spu->data[0].cvol == 0x1f00);
    // Maximum sustain threshold is 0x8000, above the legal envelope maximum.
    spu->voice[0].envctl1 = 15;
    spu->data[0].cvol = 0x7fff;
    adsr_load_sustain(spu, 0);
    assert(spu->data[0].cvol == 0x7fff);
    free(spu);
    puts("SPU envelope transition tests passed");
}
