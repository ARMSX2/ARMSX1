#pragma once
#include "raster_core.h"
#include "gpu_texture_range.h"
#include <array>
#include <stdexcept>

struct Rect {int x0,y0,x1,y1;bool valid()const{return x0<=x1&&y0<=y1;}bool intersects(const Rect& b)const{return valid()&&b.valid()&&x0<=b.x1&&x1>=b.x0&&y0<=b.y1&&y1>=b.y0;}};
// A wrapped texture rectangle and CLUT each split into at most four regions.
// This fixed scratch storage avoids a heap allocation for each primitive.
struct RectList {
    std::array<Rect,8> values{};size_t count=0;
    void push_back(const Rect& value){if(count==values.size())throw std::runtime_error("GPU texture region capacity exceeded");values[count++]=value;}
    const Rect* begin()const{return values.data();}
    const Rect* end()const{return values.data()+count;}
};
struct Primitive {int q[64]{};Rect bounds()const{return{q[4],q[5],q[6],q[7]};}};
struct GpuPrimitiveBuilder {
    template<class Regions> static void split(Regions& out,int x,int y,int w,int h){int firstW=std::min(w,1024-x),firstH=std::min(h,512-y);out.push_back({x,y,x+firstW-1,y+firstH-1});if(w>firstW)out.push_back({0,y,w-firstW-1,y+firstH-1});if(h>firstH){out.push_back({x,0,x+firstW-1,h-firstH-1});if(w>firstW)out.push_back({0,0,w-firstW-1,h-firstH-1});}}
    static std::pair<int,int> textureRange(const Primitive& p,int component,int mask,int offset,bool enhanced){
        return psxTextureRange(p.q,component,mask,offset,enhanced);
    }
    static RectList textureReads(const Primitive& p,bool enhanced){RectList out;if(!(p.q[1]&4)||!p.bounds().valid())return out;int page=p.q[9],depth=(page>>7)&3,window=p.q[10],mx=(window&31)*8,my=((window>>5)&31)*8,shift=depth>=2?0:depth==1?1:2;
        auto [u0,u1]=textureRange(p,3,mx,((window>>10)&31)*8&mx,enhanced);auto [v0,v1]=textureRange(p,4,my,((window>>15)&31)*8&my,enhanced);
        split(out,((page&15)*64+(u0>>shift))&1023,(((page>>4)&1)*256+v0)&511,(u1>>shift)-(u0>>shift)+1,v1-v0+1);
        if(depth<2){int clut=p.q[2];split(out,(clut&63)*16,(clut>>6)&511,depth==0?16:256,1);}return out;}
    static bool feedback(const Primitive& p,const RectList& ranges){for(const Rect& r:ranges)if(r.intersects(p.bounds()))return true;return false;}
    static Primitive base(int op,int clut,bool dither,const int* env,Rect bounds,int type){Primitive p;p.q[0]=type;p.q[1]=op;p.q[2]=clut;p.q[3]=dither;p.q[4]=bounds.x0;p.q[5]=bounds.y0;p.q[6]=bounds.x1;p.q[7]=bounds.y1;std::copy_n(env,7,p.q+8);return p;}
    static Rect clip(Rect bounds,const int* env){return{std::max(bounds.x0,env[3]&1023),std::max(bounds.y0,(env[3]>>10)&511),std::min(bounds.x1,env[4]&1023),std::min(bounds.y1,(env[4]>>10)&511)};}
    static Primitive triangle(Vertex a,Vertex b,Vertex c,int op,int clut,const int* env){Rect bounds{std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::max({a.x,b.x,c.x})-1,std::max({a.y,b.y,c.y})-1};i64 area=edge(a,b,c.x,c.y);if(!area||bounds.x1-bounds.x0+1>=1024||bounds.y1-bounds.y0+1>=512){Primitive p;p.q[6]=-1;return p;}if(area<0){std::swap(b,c);area=-area;}bounds=clip(bounds,env);bool shade=op&16,tex=op&4;Primitive p=base(op,clut,(env[1]&512)&&(shade||(tex&&!(op&1))),env,bounds,0);
        Vertex starts[3]={b,c,a},ends[3]={c,a,b};i64 weights[3];for(int i=0;i<3;i++){int at=16+i*4;weights[i]=edge(starts[i],ends[i],bounds.x0,bounds.y0);p.q[at]=int(weights[i]);p.q[at+1]=-(ends[i].y-starts[i].y);p.q[at+2]=ends[i].x-starts[i].x;p.q[at+3]=topLeft(starts[i],ends[i]);}
        // q[49..52]: the triangle's texture-coordinate range, which the HD shader clamps to (see HD_RASTER_FRAGMENT).
        p.q[49]=std::min({a.u,b.u,c.u});p.q[50]=std::max({a.u,b.u,c.u});p.q[51]=std::min({a.v,b.v,c.v});p.q[52]=std::max({a.v,b.v,c.v});
        p.q[43]=a.x;p.q[44]=a.y;p.q[45]=b.x;p.q[46]=b.y;p.q[47]=c.x;p.q[48]=c.y;int av[5]={a.r,a.g,a.b,a.u,a.v},bv[5]={b.r,b.g,b.b,b.u,b.v},cv[5]={c.r,c.g,c.b,c.u,c.v};for(int i=0;i<5;i++){bool interpolate=i<3?shade:tex;p.q[28+i]=interpolate?start(weights[0],weights[1],weights[2],av[i],bv[i],cv[i],area):i<3?av[i]*65536:0;p.q[33+i]=interpolate?gx(a,b,c,av[i],bv[i],cv[i],area):0;p.q[38+i]=interpolate?gy(a,b,c,av[i],bv[i],cv[i],area):0;}return p;
    }
    static Primitive rectangle(const int* packet,int op,const int* env){int size=(op>>3)&3;bool tex=op&4;int uv=tex?packet[2]:0,wh=size==0?packet[tex?3:2]:0,w=size==0?wh&1023:size==1?1:size==2?8:16,h=size==0?(uint32_t(wh)>>16)&511:size==1?1:size==2?8:16;Vertex a(packet[1],packet[0],uv,env[5]);Rect bounds=clip({a.x,a.y,a.x+w-1,a.y+h-1},env);Primitive p=base(op,uint32_t(uv)>>16,false,env,bounds,1);int dx=(env[1]&4096)?-1:1,dy=(env[1]&8192)?-1:1;p.q[28]=a.r*65536;p.q[29]=a.g*65536;p.q[30]=a.b*65536;p.q[31]=(a.u+(bounds.x0-a.x)*dx)*65536;p.q[32]=(a.v+(bounds.y0-a.y)*dy)*65536;p.q[36]=dx*65536;p.q[42]=dy*65536;p.q[49]=p.q[51]=-(1<<20);p.q[50]=p.q[52]=1<<20;return p;}
    static Primitive line(const int* packet,int op,const int* env){bool shade=op&16;Vertex a(packet[1],packet[0],0,env[5]),b(packet[shade?3:2],shade?packet[2]:packet[0],0,env[5]);int dx=b.x-a.x,dy=b.y-a.y;Rect bounds=clip({std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)},env);if(std::abs(dx)>=1024||std::abs(dy)>=512)bounds.x1=-1;Primitive p=base(op&~4,0,env[1]&512,env,bounds,2);p.q[16]=a.x;p.q[17]=a.y;p.q[18]=dx;p.q[19]=dy;p.q[20]=std::max(std::abs(dx),std::abs(dy));p.q[28]=a.r;p.q[29]=a.g;p.q[30]=a.b;p.q[33]=b.r-a.r;p.q[34]=b.g-a.g;p.q[35]=b.b-a.b;return p;}
};
