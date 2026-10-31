#pragma once
// Original native translation of PSX Lab's Java software rasterizer.
#include <jni.h>
#include <cstdint>
#include <algorithm>
#include <cstdlib>
#include <vector>
#ifdef __aarch64__
#include <arm_neon.h>
#endif
using i64=int64_t;
static int s11(int v){v&=2047;return v>=1024?v-2048:v;}
struct Vertex{int x,y,r,g,b,u,v;Vertex(int xy=0,int color=0,int uv=0,int offset=0):x(s11(xy)+s11(offset)),y(s11(uint32_t(xy)>>16)+s11(uint32_t(offset)>>11)),r(color&255),g((color>>8)&255),b((color>>16)&255),u(uv&255),v((uv>>8)&255){}};
static i64 edge(Vertex a,Vertex b,int x,int y){return i64(b.x-a.x)*(y-a.y)-i64(b.y-a.y)*(x-a.x);}
static bool topLeft(Vertex a,Vertex b){return b.y<a.y||(b.y==a.y&&b.x>a.x);}
static int gx(Vertex a,Vertex b,Vertex c,int va,int vb,int vc,i64 area){return int((i64(vb-va)*(c.y-a.y)-i64(vc-va)*(b.y-a.y))*65536/area);}
static int gy(Vertex a,Vertex b,Vertex c,int va,int vb,int vc,i64 area){return int((i64(b.x-a.x)*(vc-va)-i64(c.x-a.x)*(vb-va))*65536/area);}
static int start(i64 wa,i64 wb,i64 wc,int a,int b,int c,i64 area){return int((wa*a+wb*b+wc*c)*65536/area);}
static int floorDiv(i64 a,int b){i64 q=a/b,r=a%b;return int(q-(r<0));}
struct Raster {
 int* ram;const int* e;i64 pixels=0;
 int texture(int u,int v,int clut){int window=e[2],mx=(window&31)*8,my=((window>>5)&31)*8;
 u=((u&255)&~mx)|(((window>>10)&31)*8&mx);v=((v&255)&~my)|(((window>>15)&31)*8&my);
 int page=e[1],x=(page&15)*64,y=((page>>4)&1)*256+v,depth=(page>>7)&3;
 if(depth>=2)return ram[(y&511)*1024+((x+u)&1023)];
 int packed=ram[(y&511)*1024+((x+(u>>(depth==0?2:1)))&1023)];
 int index=depth==0?(packed>>((u&3)*4))&15:(packed>>((u&1)*8))&255;
 return ram[((clut>>6)&511)*1024+(((clut&63)*16+index)&1023)];}
 static int blend(int back,int front,int mode){switch(mode){case 0:return(back>>1)+(front>>1);case 1:return std::min(31,back+front);case 2:return std::max(0,back-front);default:return std::min(31,back+(front>>2));}}
 void pixel(int x,int y,int r,int g,int b,int u,int v,int op,int clut,bool dither){
 int address=y*1024+x,back=ram[address];if((e[6]&2)&&(back&0x8000))return;
 bool textured=op&4,semi=op&2;int mask=0;
 if(textured){int texel=texture(u,v,clut);if(!texel)return;mask=texel&0x8000;semi=semi&&mask;
 if(op&1){r=(texel&31)*8;g=((texel>>5)&31)*8;b=((texel>>10)&31)*8;}
 else{r=((texel&31)*r)>>4;g=(((texel>>5)&31)*g)>>4;b=(((texel>>10)&31)*b)>>4;}}
 static constexpr int d[16]={-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2};int delta=dither?d[(y&3)*4+(x&3)]:0;
 r=std::clamp((r+delta)>>3,0,31);g=std::clamp((g+delta)>>3,0,31);b=std::clamp((b+delta)>>3,0,31);
 if(semi){int mode=(e[1]>>5)&3;r=blend(back&31,r,mode);g=blend((back>>5)&31,g,mode);b=blend((back>>10)&31,b,mode);}
 ram[address]=r|(g<<5)|(b<<10)|mask|((e[6]&1)<<15);pixels++;}
 void triangle(Vertex a,Vertex b,Vertex c,int op,int clut){
 int minX=std::min({a.x,b.x,c.x}),maxX=std::max({a.x,b.x,c.x}),minY=std::min({a.y,b.y,c.y}),maxY=std::max({a.y,b.y,c.y});
 if(maxX-minX>=1024||maxY-minY>=512)return;i64 area=edge(a,b,c.x,c.y);if(!area)return;if(area<0){std::swap(b,c);area=-area;}
 minX=std::max(minX,e[3]&1023);maxX=std::min(maxX-1,e[4]&1023);minY=std::max(minY,(e[3]>>10)&511);maxY=std::min(maxY-1,(e[4]>>10)&511);
 bool ta=topLeft(b,c),tb=topLeft(c,a),tc=topLeft(a,b),tex=op&4,shade=op&16,dither=(e[1]&512)&&(shade||(tex&&!(op&1)));
 i64 aw=edge(b,c,minX,minY),bw=edge(c,a,minX,minY),cw=edge(a,b,minX,minY);
 int rx=shade?gx(a,b,c,a.r,b.r,c.r,area):0,ry=shade?gy(a,b,c,a.r,b.r,c.r,area):0;
 int ggx=shade?gx(a,b,c,a.g,b.g,c.g,area):0,ggy=shade?gy(a,b,c,a.g,b.g,c.g,area):0;
 int bx=shade?gx(a,b,c,a.b,b.b,c.b,area):0,by=shade?gy(a,b,c,a.b,b.b,c.b,area):0;
 int ux=tex?gx(a,b,c,a.u,b.u,c.u,area):0,uy=tex?gy(a,b,c,a.u,b.u,c.u,area):0;
 int vx=tex?gx(a,b,c,a.v,b.v,c.v,area):0,vy=tex?gy(a,b,c,a.v,b.v,c.v,area):0;
 int rr=shade?start(aw,bw,cw,a.r,b.r,c.r,area):a.r*65536,gr=shade?start(aw,bw,cw,a.g,b.g,c.g,area):a.g*65536,br=shade?start(aw,bw,cw,a.b,b.b,c.b,area):a.b*65536;
 int ur=tex?start(aw,bw,cw,a.u,b.u,c.u,area):0,vr=tex?start(aw,bw,cw,a.v,b.v,c.v,area):0;
 for(int y=minY;y<=maxY;y++,rr+=ry,gr+=ggy,br+=by,ur+=uy,vr+=vy){i64 wa=edge(b,c,minX,y),wb=edge(c,a,minX,y),wc=edge(a,b,minX,y);int r=rr,g=gr,blue=br,u=ur,v=vr;
 int x=minX;
#ifdef __aarch64__
 const int lanes[4]={0,1,2,3};int32x4_t lane=vld1q_s32(lanes),zero=vdupq_n_s32(0);
 for(;x+3<=maxX;x+=4,wa-=4*(c.y-b.y),wb-=4*(a.y-c.y),wc-=4*(b.y-a.y),r+=rx*4,g+=ggx*4,blue+=bx*4,u+=ux*4,v+=vx*4){
  int32x4_t ea=vmlaq_n_s32(vdupq_n_s32(int(wa)),lane,-(c.y-b.y)),eb=vmlaq_n_s32(vdupq_n_s32(int(wb)),lane,-(a.y-c.y)),ec=vmlaq_n_s32(vdupq_n_s32(int(wc)),lane,-(b.y-a.y));
  uint32x4_t ma=vorrq_u32(vcgtq_s32(ea,zero),vandq_u32(vceqq_s32(ea,zero),vdupq_n_u32(ta?~0u:0))),mb=vorrq_u32(vcgtq_s32(eb,zero),vandq_u32(vceqq_s32(eb,zero),vdupq_n_u32(tb?~0u:0))),mc=vorrq_u32(vcgtq_s32(ec,zero),vandq_u32(vceqq_s32(ec,zero),vdupq_n_u32(tc?~0u:0)));
  uint32x4_t coverage=vandq_u32(vandq_u32(ma,mb),mc);if(!vmaxvq_u32(coverage))continue;uint32_t covered[4];vst1q_u32(covered,coverage);
  // Preserve left-to-right pixel order for framebuffer texture feedback.
  for(int k=0;k<4;k++)if(covered[k])pixel(x+k,y,(r+rx*k)>>16,(g+ggx*k)>>16,(blue+bx*k)>>16,(u+ux*k)>>16,(v+vx*k)>>16,op,clut,dither);
 }
#endif
 for(;x<=maxX;x++,wa-=c.y-b.y,wb-=a.y-c.y,wc-=b.y-a.y,r+=rx,g+=ggx,blue+=bx,u+=ux,v+=vx)
 if((wa>0||(wa==0&&ta))&&(wb>0||(wb==0&&tb))&&(wc>0||(wc==0&&tc)))pixel(x,y,r>>16,g>>16,blue>>16,u>>16,v>>16,op,clut,dither);}}
 void polygon(const int* p,int op){int n=(op&8)?4:3,at=1,color=p[0],clut=0;bool shade=op&16,tex=op&4;Vertex v[4];
 for(int i=0;i<n;i++){if(i&&shade)color=p[at++];int xy=p[at++],uv=tex?p[at++]:0;if(tex&&i==0)clut=uint32_t(uv)>>16;v[i]=Vertex(xy,color,uv,e[5]);}
 triangle(v[0],v[1],v[2],op,clut);if(n==4)triangle(v[1],v[2],v[3],op,clut);}
 void rectangle(const int* p,int op){int size=(op>>3)&3;bool tex=op&4;int uv=tex?p[2]:0,wh=size==0?p[tex?3:2]:0;
 int w=size==0?wh&1023:size==1?1:size==2?8:16,h=size==0?(wh>>16)&511:size==1?1:size==2?8:16;Vertex a(p[1],p[0],uv,e[5]);
 int minX=std::max(a.x,e[3]&1023),maxX=std::min(a.x+w-1,e[4]&1023),minY=std::max(a.y,(e[3]>>10)&511),maxY=std::min(a.y+h-1,(e[4]>>10)&511),dx=(e[1]&4096)?-1:1,dy=(e[1]&8192)?-1:1;
#ifdef __aarch64__
 if(!(op&6)){
  int value=(a.r>>3)|((a.g>>3)<<5)|((a.b>>3)<<10)|((e[6]&1)<<15);int32x4_t color=vdupq_n_s32(value),mask=vdupq_n_s32(0x8000);
  for(int y=minY;y<=maxY;y++){int x=minX;for(;x+3<=maxX;x+=4){int* out=ram+y*1024+x;
   if(e[6]&2){int32x4_t old=vld1q_s32(out);uint32x4_t allowed=vceqq_s32(vandq_s32(old,mask),vdupq_n_s32(0));vst1q_s32(out,vbslq_s32(allowed,color,old));pixels+=vaddvq_u32(vshrq_n_u32(allowed,31));}
   else{vst1q_s32(out,color);pixels+=4;}}
   for(;x<=maxX;x++)pixel(x,y,a.r,a.g,a.b,0,0,op,0,false);
  }return;
 }
#endif
 for(int y=minY;y<=maxY;y++)for(int x=minX;x<=maxX;x++)pixel(x,y,a.r,a.g,a.b,a.u+(x-a.x)*dx,a.v+(y-a.y)*dy,op,uint32_t(uv)>>16,false);}
 void line(const int* p,int op){bool shade=op&16;Vertex a(p[1],p[0],0,e[5]),b(p[shade?3:2],shade?p[2]:p[0],0,e[5]);int dx=b.x-a.x,dy=b.y-a.y,steps=std::max(std::abs(dx),std::abs(dy));
 if(std::abs(dx)>=1024||std::abs(dy)>=512)return;
 for(int i=0;i<=steps;i++){int x=steps==0?a.x:a.x+floorDiv(i64(dx)*i+steps/2,steps),y=steps==0?a.y:a.y+floorDiv(i64(dy)*i+steps/2,steps);
 if(x<(e[3]&1023)||x>(e[4]&1023)||y<((e[3]>>10)&511)||y>((e[4]>>10)&511))continue;
 int r=steps==0?a.r:a.r+(b.r-a.r)*i/steps,g=steps==0?a.g:a.g+(b.g-a.g)*i/steps,blue=steps==0?a.b:a.b+(b.b-a.b)*i/steps;
 pixel(x,y,r,g,blue,0,0,op&~4,0,e[1]&512);}}
};
