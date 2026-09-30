#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
void real_idct(int16_t*, int16_t*);
static void reference(int16_t* blk, int16_t* scale) {
    int16_t buf[64], *src=blk, *dst=buf;
    for (int pass=0; pass<2; ++pass) {
        for (int x=0; x<8; ++x) for (int y=0; y<8; ++y) {
            int sum=0;
            for (int z=0; z<8; ++z)
                sum += (int32_t)src[y+z*8] * ((int32_t)scale[x+z*8]/8);
            dst[x+y*8]=(sum+0xfff)/0x2000;
        }
        int16_t* tmp=src; src=dst; dst=tmp;
    }
}
static uint32_t seed=1234567;
static uint32_t next(void) { seed=seed*1664525u+1013904223u; return seed; }
static volatile unsigned checksum;
static double bench(void (*fn)(int16_t*,int16_t*),int16_t* input,int16_t* scale) {
    clock_t start=clock();
    for (int n=0; n<200000; ++n) {
        int16_t b[64]; memcpy(b,input,sizeof(b)); fn(b,scale); checksum+=(uint16_t)b[n&63];
    }
    return (double)(clock()-start)/CLOCKS_PER_SEC;
}
int main(void) {
    int16_t a[64], b[64], scale[64], input[64];
    for (int n=0; n<30000; ++n) {
        for(int i=0;i<64;++i) {
            scale[i]=(int16_t)next();
            input[i]=(int16_t)((next()>>16)%2048)-1024;
            if(n%3==0 && i) input[i]=0; /* sparse/DC */
            if(n%11==0) scale[i]=(i&1)?-32768:32767;
        }
        memcpy(a,input,sizeof(a)); memcpy(b,input,sizeof(b));
        reference(a,scale); real_idct(b,scale);
        if(memcmp(a,b,sizeof(a))) { fprintf(stderr,"IDCT mismatch %d\n",n); return 1; }
    }
    printf("IDCT: 30000 custom-scale/sparse/dense blocks identical\n");
    double old=bench(reference,input,scale), fresh=bench(real_idct,input,scale);
    printf("IDCT benchmark reference=%.4fs optimized=%.4fs ratio=%.2fx checksum=%u\n",old,fresh,old/fresh,checksum);
    return 0;
}
