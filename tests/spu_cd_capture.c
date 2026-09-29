#include "psx/dev/spu.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void psx_ic_irq(psx_ic_t* ic, int irq) { (void)ic; (void)irq; }
int main(void) {
    psx_spu_t* s = calloc(1, sizeof(*s));
    s->ram = malloc(0x80000);
    memset(s->ram, 0x5a, 0x80000);
    int16_t input[1476];
    for (int i = 0; i < 738; ++i) {
        input[2*i] = i * 31; input[2*i+1] = -i * 29;
    }
    psx_spu_capture_cd_audio(s, input, 738);
    for (int i = 0; i < 512; ++i) {
        int16_t left, right;
        memcpy(&left, s->ram + 2*i, 2);
        memcpy(&right, s->ram + 0x400 + 2*i, 2);
        assert(left == input[2*(i+226)]);
        assert(right == input[2*(i+226)+1]);
    }
    for (int i = 0x800; i < 0x80000; ++i) assert(s->ram[i] == 0x5a);
    int16_t short_input[2] = {123, -456};
    psx_spu_capture_cd_audio(s, short_input, 1);
    assert(((int16_t*)s->ram)[0] == 123);
    assert(((int16_t*)s->ram)[512] == -456);
    memset(input, 0, sizeof(input));
    psx_spu_capture_cd_audio(s, input, 738);
    for (int i = 0; i < 0x800; ++i) assert(s->ram[i] == 0);
    free(s->ram); free(s);
    puts("CD capture channels, bounds, short input and silence: PASS");
}
