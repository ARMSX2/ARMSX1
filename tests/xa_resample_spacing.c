#include "psx/dev/cdrom/cdrom.h"
#include "psx/dev/cdrom/xa_resampler.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#undef puts
void cdrom_resample_xa_buf(psx_cdrom_t*, int16_t*, int16_t*, int, int16_t);
int main(void) {
    int16_t* input = calloc(4032, 2);
    int16_t* all = calloc(9408, 2);
    int16_t* split = calloc(9408, 2);
    int16_t h[32] = {0}, h2[32] = {0};
    assert(input && all && split);
    input[0] = 16384;
    assert(xa_resample_filtered(input, 2016, 0, h, all) == 2352);
    const int16_t expected[7] = {-1, 1, -3, 2, 5, 13, -34};
    for (int i=0;i<7;++i) assert(all[i] == expected[i]);
    for (int i=0;i<4032;++i) input[i] = (int16_t)((i*12347u) & 65535u);
    for (int half=0;half<2;++half) {
        memset(h,0,sizeof(h)); memset(h2,0,sizeof(h2));
        const size_t n = xa_resample_filtered(input,4032,half,h,all);
        const size_t a = xa_resample_filtered(input,2016,half,h2,split);
        const size_t b = xa_resample_filtered(input+2016,2016,half,h2,split+a);
        assert(n == (size_t)(4704*(half+1)) && n == a+b);
        assert(!memcmp(all,split,n*2));
        assert(!memcmp(h,h2,sizeof(h)));
    }
    psx_cdrom_t* cd = calloc(1,sizeof(*cd)); assert(cd);
    cd->xa_left_buf[0] = 16384;
    cdrom_resample_xa_buf(cd,cd->xa_left_resample_buf,cd->xa_left_buf,1,0);
    cdrom_resample_xa_buf(cd,cd->xa_right_resample_buf,cd->xa_right_buf,1,0);
    for (int i=0;i<2352;++i) assert(cd->xa_right_resample_buf[i] == 0);
    free(cd); free(input); free(all); free(split);
    puts("XA filter impulse, rate, continuity and stereo tests passed");
}
