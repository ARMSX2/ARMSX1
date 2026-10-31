#include "psx/dev/gpu.h"
#include <math.h>
#include <stdio.h>
static uint16_t reference(uint16_t back,uint16_t front,int mode) {
    uint16_t result=0;
    for(int shift=0;shift<=10;shift+=5) {
        float b=((back>>shift)&31)*8, f=((front>>shift)&31)*8, v;
        switch(mode) {
            case 0:v=0.5f*b+0.5f*f;break;
            case 1:v=b+f;break;
            case 2:v=b-f;break;
            default:v=b+0.25f*f;break;
        }
        if(v<0)v=0; if(v>255)v=255;
        result|=((unsigned)roundf(v)>>3)<<shift;
    }
    return result;
}
int main(void) {
    unsigned checks=0;
    for(int m=0;m<4;++m) for(int b=0;b<32;++b) for(int f=0;f<32;++f)
    for(int mask=0;mask<4;++mask) {
        uint16_t back=(b|(b<<5)|(b<<10))|((mask&1)?0x8000:0);
        uint16_t front=(f|((31-f)<<5)|(((f+7)&31)<<10))|((mask&2)?0x8000:0);
        if(reference(back,front,m)!=psx_gpu_blend_rgb555(back,front,m)) return 1;
        ++checks;
    }
    printf("GPU blend: %u exhaustive channel/mode/mask checks passed\n",checks);
    return 0;
}
