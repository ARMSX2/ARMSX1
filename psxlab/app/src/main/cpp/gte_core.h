#pragma once
// Original native implementation of PSX Lab's geometry arithmetic.
#include <cstdint>
#include <algorithm>
#include <climits>
using i64=int64_t;
struct GteCore {
    int32_t* d;int32_t* c;int shift;bool limit;bool widescreen=false;
    static int clamp(i64 v,int lo,int hi){return int(std::max<i64>(lo,std::min<i64>(hi,v)));}
    void flag(int b){c[31]|=int32_t(uint32_t(1)<<b);}
    void summary(){c[31]&=0x7fffffff;if(c[31]&0x7f87e000)c[31]|=int32_t(0x80000000u);}
    i64 wrap(int i,i64 v){if(v>0x7ffffffffffLL)flag(31-i);if(v< -0x80000000000LL)flag(28-i);return i64(uint64_t(v)<<20)>>20;}
    int sat(i64 v,int lo,int hi,int b){if(v<lo||v>hi)flag(b);return clamp(v,lo,hi);}
    void result(int i,i64 v){int m=int(wrap(i,v)>>shift);d[24+i]=m;d[8+i]=sat(m,limit?0:-32768,32767,25-i);}
    void mac0(i64 v){if(v>INT_MAX)flag(16);if(v<INT_MIN)flag(15);d[24]=int(v);}
    void vector(int n,int* v){if(n==3){for(int i=0;i<3;i++)v[i]=int16_t(d[9+i]);}else{v[0]=int16_t(d[n*2]);v[1]=int16_t(uint32_t(d[n*2])>>16);v[2]=int16_t(d[n*2+1]);}}
    int matrix(int n,int row,int col){if(n==3){if(row==0){int red=(d[6]&255)*16;return col==0?-red:col==1?red:int16_t(d[8]);}return int16_t(c[row==1?1:2]);}int at=row*3+col;return int16_t(uint32_t(c[n*8+at/2])>>((at&1)*16));}
    int translation(int n,int row){return n==3?0:c[(n==0?5:n==1?13:21)+row];}
    void multiply(int m,const int* v,int t,bool bug,i64* values=nullptr){
        for(int row=0;row<3;row++){int i=row+1;i64 sum=wrap(i,i64(translation(t,row))*4096+i64(matrix(m,row,0))*v[0]);
            if(bug){sat(int(sum>>shift),-32768,32767,25-i);sum=0;}
            sum=wrap(i,sum+i64(matrix(m,row,1))*v[1]);sum=wrap(i,sum+i64(matrix(m,row,2))*v[2]);if(values)values[row]=sum;result(i,sum);
        }
    }
    void multiply(int m,int vi,int t,bool bug,i64* values=nullptr){int v[3];vector(vi,v);multiply(m,v,t,bug,values);}
    void pushColor(){int rgb=d[6]&int32_t(0xff000000u);for(int i=1;i<=3;i++)rgb|=sat(d[24+i]>>4,0,255,22-i)<<((i-1)*8);d[20]=d[21];d[21]=d[22];d[22]=rgb;}
    void interpolate(const i64* base){int factor=int16_t(d[8]),diff[3];for(int i=1;i<=3;i++)diff[i-1]=sat(int(wrap(i,i64(c[20+i])*4096-base[i-1])>>shift),-32768,32767,25-i);for(int i=1;i<=3;i++)result(i,base[i-1]+i64(diff[i-1])*factor);}
    void modulated(i64* out){for(int i=0;i<3;i++)out[i]=i64((uint32_t(d[6])>>(i*8))&255)*int16_t(d[9+i])*16;}
    void color(int v,bool modulate,bool depth){multiply(1,v,3,false);multiply(2,3,1,false);if(modulate){i64 base[3];modulated(base);if(depth)interpolate(base);else for(int i=1;i<=3;i++)result(i,base[i-1]);}pushColor();}
    int divide(int numerator,int denominator){if(numerator>=denominator*2){flag(17);return 0x1ffff;}int normalize=__builtin_clz(unsigned(denominator))-16;i64 n=i64(numerator)<<normalize,z=i64(denominator)<<normalize;int index=int((z-0x7fc0)>>7),table=std::max(0,((0x40000/(index+0x100)+1)/2)-0x101);i64 u=table+0x101;z=(0x2000080LL-z*u)>>8;z=(0x80+z*u)>>8;return int(std::min<i64>(0x1ffff,(n*z+0x8000)>>16));}
    void perspective(int v,bool last){int previous=c[31]&(1<<22);i64 transformed[3];multiply(0,v,0,false,transformed);c[31]=(c[31]&~(1<<22))|previous;sat(transformed[2]>>12,-32768,32767,22);d[16]=d[17];d[17]=d[18];d[18]=d[19];d[19]=sat(transformed[2]>>12,0,65535,18);int ratio=divide(c[26]&65535,d[19]);i64 projected=i64(ratio)*int16_t(d[9]);if(widescreen)projected=projected*3/4;i64 x=projected+c[24],y=i64(ratio)*int16_t(d[10])+c[25];mac0(x);int sx=sat(x>>16,-1024,1023,14);mac0(y);int sy=sat(y>>16,-1024,1023,13);d[12]=d[13];d[13]=d[14];d[14]=(sx&65535)|int32_t(uint32_t(sy)<<16);if(last){i64 depth=i64(ratio)*int16_t(c[27])+c[28];mac0(depth);d[8]=sat(depth>>12,0,4096,12);}}
    bool execute(int instruction){c[31]=0;shift=(instruction&0x80000)?12:0;limit=instruction&0x400;int op=instruction&63;
        switch(op){
            case 1:perspective(0,true);break;
            case 0x30:perspective(0,false);perspective(1,false);perspective(2,true);break;
            case 6:{i64 x0=int16_t(d[12]),y0=int16_t(uint32_t(d[12])>>16),x1=int16_t(d[13]),y1=int16_t(uint32_t(d[13])>>16),x2=int16_t(d[14]),y2=int16_t(uint32_t(d[14])>>16);mac0(x0*y1+x1*y2+x2*y0-x0*y2-x1*y0-x2*y1);break;}
            case 0x2d:case 0x2e:{i64 sum=(d[17]&65535)+(d[18]&65535)+(d[19]&65535)+(op==0x2e?(d[16]&65535):0),v=sum*int16_t(c[op==0x2e?30:29]);mac0(v);d[7]=sat(v>>12,0,65535,18);break;}
            case 0x12:multiply((instruction>>17)&3,(instruction>>15)&3,(instruction>>13)&3,((instruction>>13)&3)==2);break;
            case 0x28:{int v[3];vector(3,v);for(int i=1;i<=3;i++)result(i,i64(v[i-1])*v[i-1]);break;}
            case 0xc:{int v[3];vector(3,v);int a=int16_t(c[0]),b=int16_t(c[2]),z=int16_t(c[4]);result(1,i64(v[2])*b-i64(v[1])*z);result(2,i64(v[0])*z-i64(v[2])*a);result(3,i64(v[1])*a-i64(v[0])*b);break;}
            case 0x3d:case 0x3e:{int v[3];vector(3,v);int factor=int16_t(d[8]);for(int i=1;i<=3;i++)result(i,i64(v[i-1])*factor+(op==0x3e?i64(d[24+i])*(i64(1)<<shift):0));pushColor();break;}
            case 0x10:case 0x11:case 0x29:case 0x2a:{for(int k=0;k<(op==0x2a?3:1);k++){i64 base[3];int rgb=op==0x2a?d[20]:d[6];for(int i=0;i<3;i++)base[i]=op==0x11?i64(int16_t(d[9+i]))*4096:op==0x29?i64((uint32_t(rgb)>>(i*8))&255)*int16_t(d[9+i])*16:i64((uint32_t(rgb)>>(i*8))&255)*65536;interpolate(base);pushColor();}break;}
            case 0x1e:color(0,false,false);break;
            case 0x20:for(int i=0;i<3;i++)color(i,false,false);break;
            case 0x1b:color(0,true,false);break;
            case 0x3f:for(int i=0;i<3;i++)color(i,true,false);break;
            case 0x13:color(0,true,true);break;
            case 0x16:for(int i=0;i<3;i++)color(i,true,true);break;
            case 0x1c:case 0x14:{multiply(2,3,1,false);i64 base[3];modulated(base);if(op==0x14)interpolate(base);else for(int i=1;i<=3;i++)result(i,base[i-1]);pushColor();break;}
            default:return false;
        }summary();return true;
    }
};
