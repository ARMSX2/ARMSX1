#pragma once
#include <cstdint>
#include <algorithm>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace PsxMdec {
inline int32_t rounded(int64_t value){return int32_t(uint32_t((value+4095)>>13));}
inline void scalar(const int32_t* coefficients,const int32_t* scale,int32_t* destination){
    int32_t temporary[64];const int32_t* source=coefficients;int32_t* target=temporary;
    for(int pass=0;pass<2;pass++){
        for(int y=0;y<8;y++)for(int x=0;x<8;x++){
            int64_t sum=0;for(int z=0;z<8;z++)sum+=int64_t(source[y+z*8])*(scale[x+z*8]>>3);
            target[x+y*8]=rounded(sum);
        }
        source=target;target=destination;
    }
}
inline void transform(const int32_t* coefficients,const int32_t* scale,int32_t* destination){
    bool dcOnly=true;for(int i=1;i<64;i++)dcOnly&=coefficients[i]==0;
    if(dcOnly){
        int32_t first[8];for(int i=0;i<8;i++)first[i]=rounded(int64_t(coefficients[0])*(scale[i]>>3));
        for(int y=0;y<8;y++)for(int x=0;x<8;x++)destination[y*8+x]=rounded(int64_t(first[y])*(scale[x]>>3));
        return;
    }
#if defined(__aarch64__)
    // With decoded coefficients in [-1024,1023] and signed-16 scale RAM,
    // sum magnitudes are at most 2^25 and 2^27 in the two passes. The four-lane
    // integer multiply-add preserves the original 64-bit result exactly.
    bool bounded=true;for(int i=0;i<64;i++)bounded&=coefficients[i]>=-1024&&coefficients[i]<=1023&&scale[i]>=-32768&&scale[i]<=32767;
    if(bounded){
        int32x4_t factors[8][2];for(int z=0;z<8;z++)for(int half=0;half<2;half++)factors[z][half]=vshrq_n_s32(vld1q_s32(scale+z*8+half*4),3);
        int32_t temporary[64];const int32_t* source=coefficients;int32_t* target=temporary;const auto bias=vdupq_n_s32(4095);
        for(int pass=0;pass<2;pass++){
            for(int y=0;y<8;y++){
                int32x4_t lo=vdupq_n_s32(0),hi=vdupq_n_s32(0);
                for(int z=0;z<8;z++){int32_t value=source[y+z*8];lo=vmlaq_n_s32(lo,factors[z][0],value);hi=vmlaq_n_s32(hi,factors[z][1],value);}
                vst1q_s32(target+y*8,vshrq_n_s32(vaddq_s32(lo,bias),13));vst1q_s32(target+y*8+4,vshrq_n_s32(vaddq_s32(hi,bias),13));
            }
            source=target;target=destination;
        }
        return;
    }
#endif
    // Also retain the original arithmetic for unusually edited/legacy states.
    scalar(coefficients,scale,destination);
}
}
