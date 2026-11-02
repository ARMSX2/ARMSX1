#pragma once
#include "cpu_core.h"
#include "gte_core.h"

inline int cpuGteRead(CpuState* s,int r){int32_t* d=s->gte;switch(r){case 1:case 3:case 5:case 8:case 9:case 10:case 11:return int16_t(d[r]);case 7:case 16:case 17:case 18:case 19:return d[r]&65535;case 15:return d[14];case 28:case 29:{int v=0;for(int i=0;i<3;i++)v|=std::max(0,std::min(31,int(int16_t(d[9+i]))>>7))<<(i*5);return v;}default:return d[r];}}
inline void cpuGteWrite(CpuState* s,int r,int v){int32_t* d=s->gte;switch(r){case 1:case 3:case 5:case 8:case 9:case 10:case 11:d[r]=int16_t(v);break;case 7:case 16:case 17:case 18:case 19:d[r]=v&65535;break;case 15:d[12]=d[13];d[13]=d[14];d[14]=v;break;case 28:d[9]=(v&31)<<7;d[10]=((uint32_t(v)>>5)&31)<<7;d[11]=((uint32_t(v)>>10)&31)<<7;break;case 29:case 31:break;case 30:d[30]=v;d[31]=v==0||v==-1?32:__builtin_clz(v<0?uint32_t(~v):uint32_t(v));break;default:d[r]=v;}}
inline void cpuGteControl(CpuState* s,int r,int v){switch(r){case 4:case 12:case 20:case 26:case 27:case 29:case 30:v=int16_t(v);break;case 31:v&=0x7ffff000;if(v&0x7f87e000)v|=int32_t(0x80000000u);break;default:break;}s->gte[32+r]=v;}
// Leaf helper shared with the block translator: no JNI, clocks, PC or GPR writes.
inline void executeCompiledGte(CpuState* s,uint32_t word,int projectionScale,int value){
    Decoded d(word,0);
    if(d.s>=16){GteCore g{s->gte,s->gte+32,0,false,projectionScale};g.execute(int(word));}
    else switch(d.s){case 0:s->loadValue=cpuGteRead(s,d.d);break;case 2:s->loadValue=s->gte[32+d.d];break;case 4:cpuGteWrite(s,d.d,value);break;case 6:cpuGteControl(s,d.d,value);break;}
}
