#pragma once
// Original enhanced rasterizer: native VRAM remains authoritative for emulation.
static const char* HD_NATIVE_RESOLVE_SHADER=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) writeonly buffer Vram {uint ram[];};
layout(r32ui,binding=1) readonly uniform highp uimage2D hdImage;
uniform ivec4 bounds;
uniform uvec2 dirtyRows[32];
uniform int scale;
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy)+bounds.xy;if(any(greaterThan(p,bounds.zw)))return;uvec2 row=dirtyRows[p.y>>4];uint mask=p.x<512?row.x:row.y;if((mask&(1u<<uint((p.x>>4)&31)))==0u)return;ram[p.y*1024+p.x]=imageLoad(hdImage,p*scale).r;}
)GLSL";
static const char* HD_DRAW_SHADER=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) readonly buffer Vram {uint ram[];};
layout(r32ui,binding=1) uniform highp uimage2D hdImage;
layout(std430,binding=1) readonly buffer Commands {int q[];};
layout(std430,binding=2) buffer Statistics {uint pixels;};
uniform ivec4 bounds;
uniform int count;
uniform int countPixels;
uniform int serialMode;
uniform int scale;
uint readHd(int at){return imageLoad(hdImage,ivec2(at%(1024*scale),at/(1024*scale))).r;}
void writeHd(int at,uint value){imageStore(hdImage,ivec2(at%(1024*scale),at/(1024*scale)),uvec4(value,0,0,0));}
int e(int at,int n){return q[at+8+n];}
int floorDiv(int a,int b){return a/b-((a%b)<0?1:0);}
int blend(int back,int front,int mode){if(mode==0)return(back>>1)+(front>>1);if(mode==1)return min(31,back+front);if(mode==2)return max(0,back-front);return min(31,back+(front>>2));}
int texturePixel(int at,int u,int v,int clut){int window=e(at,2),mx=(window&31)*8,my=((window>>5)&31)*8;
 u=((u&255)&~mx)|(((window>>10)&31)*8&mx);v=((v&255)&~my)|(((window>>15)&31)*8&my);
 int page=e(at,1),x=(page&15)*64,y=((page>>4)&1)*256+v,depth=(page>>7)&3;
 if(depth>=2)return int(ram[(y&511)*1024+((x+u)&1023)]);
 int packed=int(ram[(y&511)*1024+((x+(u>>(depth==0?2:1)))&1023)]);
 int index=depth==0?(packed>>((u&3)*4))&15:(packed>>((u&1)*8))&255;
 return int(ram[((clut>>6)&511)*1024+(((clut&63)*16+index)&1023)]);
}
void drawAt(int x,int y,uint liveMask){
 while(liveMask!=0u){int n=findLSB(liveMask);liveMask&=liveMask-1u;int at=n*64;if(x<q[at+4]*scale||y<q[at+5]*scale||x>=(q[at+6]+1)*scale||y>=(q[at+7]+1)*scale)continue;
  int dx=x/scale-q[at+4],dy=y/scale-q[at+5],sx=x%scale,sy=y%scale,type=q[at],op=q[at+1];ivec3 color;ivec2 uv=ivec2(0);
  if(type==0){bool inside=true;for(int edge=0;edge<3;edge++){int p=at+16+edge*4;int w=(q[p]+q[p+1]*dx+q[p+2]*dy)*scale+q[p+1]*sx+q[p+2]*sy;if(w<0||(w==0&&q[p+3]==0))inside=false;}if(!inside)continue;}
  if(type<2){int values[5];for(int i=0;i<5;i++)values[i]=int(uint(q[at+28+i])+uint(q[at+33+i])*uint(dx)+uint(q[at+38+i])*uint(dy)+uint((q[at+33+i]/scale)*sx+(q[at+33+i]%scale)*sx/scale+(q[at+38+i]/scale)*sy+(q[at+38+i]%scale)*sy/scale))>>16;color=ivec3(values[0],values[1],values[2]);uv=ivec2(values[3],values[4]);}
  else{int ax=q[at+16],ay=q[at+17],lx=q[at+18],ly=q[at+19],steps=q[at+20];int i=abs(lx)>=abs(ly)?(x/scale-ax)*(lx<0?-1:1):(y/scale-ay)*(ly<0?-1:1);if(i<0||i>steps)continue;
   int px=steps==0?ax:ax+floorDiv(lx*i+steps/2,steps),py=steps==0?ay:ay+floorDiv(ly*i+steps/2,steps);if(px!=x/scale||py!=y/scale)continue;
   for(int k=0;k<3;k++)color[k]=q[at+28+k]+(steps==0?0:q[at+33+k]*i/steps);
  }
  int address=y*(1024*scale)+x,back=int(readHd(address));if((e(at,6)&2)!=0&&(back&32768)!=0)continue;
  bool textured=(op&4)!=0,semi=(op&2)!=0;int mask=0;
  if(textured){int texel=texturePixel(at,uv.x,uv.y,q[at+2]);if(texel==0)continue;mask=texel&32768;semi=semi&&mask!=0;ivec3 tex=ivec3(texel&31,(texel>>5)&31,(texel>>10)&31);color=(op&1)!=0?tex*8:(tex*color)>>4;}
  const int dither[16]=int[16](-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2);int delta=q[at+3]!=0?dither[((y/scale)&3)*4+((x/scale)&3)]:0;color=clamp((color+delta)>>3,ivec3(0),ivec3(31));
  if(semi){int mode=(e(at,1)>>5)&3;for(int k=0;k<3;k++)color[k]=blend((back>>(k*5))&31,color[k],mode);}
  writeHd(address,uint(color.r|(color.g<<5)|(color.b<<10)|mask|((e(at,6)&1)<<15)));if(countPixels!=0)atomicAdd(pixels,1u);
 }
}
shared uint tileMask;
void main(){
 if(serialMode!=0){if(any(notEqual(gl_GlobalInvocationID,uvec3(0))))return;uint liveMask=count==32?0xffffffffu:(1u<<uint(count))-1u;for(int y=bounds.y;y<=bounds.w;y++)for(int x=bounds.x;x<=bounds.z;x++)drawAt(x,y,liveMask);return;}
 if(gl_LocalInvocationIndex==0u){ivec2 lo=ivec2(gl_WorkGroupID.xy)*8+bounds.xy,hi=lo+ivec2(7);uint liveMask=0u;for(int n=0;n<count;n++){int at=n*64;if(q[at+4]*scale<=hi.x&&(q[at+6]+1)*scale>lo.x&&q[at+5]*scale<=hi.y&&(q[at+7]+1)*scale>lo.y)liveMask|=1u<<uint(n);}tileMask=liveMask;}
 barrier();ivec2 xy=ivec2(gl_GlobalInvocationID.xy)+bounds.xy;if(any(greaterThan(xy,bounds.zw)))return;drawAt(xy.x,xy.y,tileMask);
})GLSL";
static const char* HD_TRANSFER_SHADER=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) readonly buffer NativeVram {uint ram[];};
layout(std430,binding=1) readonly buffer Data {uint words[];};
layout(r32ui,binding=1) uniform highp uimage2D hdImage;
uniform ivec4 shape;
uniform ivec4 data;
uniform int operation;
uniform int scale;
uint readHd(int at){return imageLoad(hdImage,ivec2(at%(1024*scale),at/(1024*scale))).r;}
void writeHd(int at,uint value){imageStore(hdImage,ivec2(at%(1024*scale),at/(1024*scale)),uvec4(value,0,0,0));}
int address(int x,int y){return ((y%(512*scale)+512*scale)%(512*scale))*(1024*scale)+(x%(1024*scale)+1024*scale)%(1024*scale);}
void copyPixel(int x,int y){int dst=address(shape.x*scale+x,shape.y*scale+y);if((data.z&2)!=0&&(readHd(dst)&32768u)!=0u)return;writeHd(dst,readHd(address(data.x*scale+x,data.y*scale+y))|uint((data.z&1)<<15));}
void main(){
 if(operation==3){if(gl_GlobalInvocationID.y!=0u)return;int i=int(gl_GlobalInvocationID.x);if(data.y==shape.y){if(i>=shape.w*scale)return;for(int x=0;x<shape.z*scale;x++)copyPixel(x,i);}else if(data.x==shape.x){if(i>=shape.z*scale)return;for(int y=0;y<shape.w*scale;y++)copyPixel(i,y);}return;}
 int x=int(gl_GlobalInvocationID.x),y=int(gl_GlobalInvocationID.y);if(x>=shape.z*scale||y>=shape.w*scale)return;
 int dst=address(shape.x*scale+x,shape.y*scale+y);uint value;int mask=0;
 if(operation==0)value=uint(data.x);
 else if(operation==1){int i=(y/scale)*shape.z+x/scale;if(i>=data.w)return;int index=data.z+i;dst=address(((shape.x+index%shape.z)&1023)*scale+x%scale,((shape.y+index/shape.z)&511)*scale+y%scale);value=(words[i/2]>>uint((i&1)*16))&65535u;mask=data.y;}
 else if(operation==4){int nx=(shape.x+x/scale)&1023,ny=(shape.y+y/scale)&511;value=ram[ny*1024+nx];}
 else{value=readHd(address(data.x*scale+x,data.y*scale+y));mask=data.z;}
 if((mask&2)!=0&&(readHd(dst)&32768u)!=0u)return;writeHd(dst,value|uint((mask&1)<<15));
})GLSL";
static const char* HD_SCANOUT_SHADER=R"GLSL(#version 310 es
precision highp int;precision highp float;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) readonly buffer NativeVram {uint ram[];};
layout(r32ui,binding=1) readonly uniform highp uimage2D hdImage;
layout(rgba8,binding=0) writeonly uniform highp image2D frameImage;
uniform ivec4 display;
uniform int status;
uniform int scale;
uint readByte(int row,int x){return(ram[row+((x>>1)&1023)]>>uint((x&1)*8))&255u;}
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=display.z||p.y>=display.w)return;vec3 rgb=vec3(0);
 if((status&0x800000)==0){if((status&0x200000)==0){int row=((display.y*scale+p.y)%(512*scale))*(1024*scale);uint v=imageLoad(hdImage,ivec2((display.x*scale+p.x)%(1024*scale),(display.y*scale+p.y)%(512*scale))).r;uvec3 c=uvec3(v&31u,(v>>5)&31u,(v>>10)&31u);rgb=vec3((c<<3)|(c>>2))/255.0;}
 else{int row=((display.y+p.y/scale)&511)*1024,x=display.x*2+(p.x/scale)*3;rgb=vec3(readByte(row,x),readByte(row,x+1),readByte(row,x+2))/255.0;}}
 imageStore(frameImage,p,vec4(rgb,1));
})GLSL";

static const char* HD_RASTER_VERTEX=R"GLSL(#version 310 es
precision highp int;precision highp float;
layout(std430,binding=1) readonly buffer Commands {int q[];};
flat out int primitive;
uniform int scale;
void main(){primitive=gl_InstanceID;int at=primitive*64;const ivec2 corners[6]=ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));ivec2 c=corners[gl_VertexID];vec2 p=vec2(c.x==0?q[at+4]*scale:(q[at+6]+1)*scale,c.y==0?q[at+5]*scale:(q[at+7]+1)*scale);if(q[at]==0){int vertex=gl_VertexID<3?gl_VertexID:0;vec2 a=vec2(q[at+43],q[at+44]),b=vec2(q[at+45],q[at+46]),d=vec2(q[at+47],q[at+48]);vec2 ab=b-a,ad=d-a,bd=d-b;float area=ab.x*ad.y-ab.y*ad.x,longest=max(dot(ab,ab),max(dot(ad,ad),dot(bd,bd)));
 // Very thin triangles retain their bounding quad to avoid precision loss in mitres.
 if(area*area>=longest*longest*0.0004){vec2 v=vertex==0?a:vertex==1?b:d,previous=vertex==0?d:vertex==1?a:b,next=vertex==0?b:vertex==1?d:a;
 // Rasterize the triangle conservatively, then retain the exact integer edge tests.
 // The half-pixel shift aligns GLES sample centres with PS1 integer sample positions.
 vec2 before=normalize(v-previous),after=normalize(next-v),n1=vec2(before.y,-before.x),n2=vec2(after.y,-after.x),outward=n1+n2;
 p=v*float(scale)+vec2(0.5)+outward*(0.5/dot(outward,n1));}}
 gl_Position=vec4(p/vec2(1024*scale,512*scale)*2.0-1.0,0,1);}
)GLSL";
static const char* HD_PRESENT_VERTEX=R"GLSL(#version 310 es
precision highp float;
out highp vec2 uv;
uniform int flip;
void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);uv=vec2(p.x,flip!=0?1.0-p.y:p.y);}
)GLSL";
static const char* HD_PRESENT_FRAGMENT=R"GLSL(#version 310 es
precision highp int;precision highp float;
layout(std430,binding=0) readonly buffer NativeVram {uint ram[];};
uniform highp usampler2D enhanced;
uniform ivec4 display;
uniform int status;
uniform int smoothImage;
uniform int scale;
in highp vec2 uv;
out vec4 color;
uint readByte(int row,int x){return(ram[row+((x>>1)&1023)]>>uint((x&1)*8))&255u;}
vec3 samplePixel(ivec2 p){p=clamp(p,ivec2(0),display.zw-ivec2(1));if((status&0x200000)!=0){int row=((display.y+p.y/scale)&511)*1024,x=display.x*2+(p.x/scale)*3;return vec3(readByte(row,x),readByte(row,x+1),readByte(row,x+2))/255.0;}
 ivec2 at=(display.xy*scale+p)%ivec2(1024*scale,512*scale);uint v=texelFetch(enhanced,at,0).r;uvec3 c=uvec3(v&31u,(v>>5)&31u,(v>>10)&31u);return vec3((c<<3)|(c>>2))/255.0;}
void main(){if((status&0x800000)!=0){color=vec4(0,0,0,1);return;}vec2 p=clamp(uv*vec2(display.zw),vec2(0.5),vec2(display.zw)-vec2(0.5));vec3 rgb;
 if(smoothImage==0)rgb=samplePixel(ivec2(p));else{p-=vec2(0.5);ivec2 a=ivec2(floor(p));vec2 t=fract(p);rgb=mix(mix(samplePixel(a),samplePixel(a+ivec2(1,0)),t.x),mix(samplePixel(a+ivec2(0,1)),samplePixel(a+ivec2(1,1)),t.x),t.y);}color=vec4(rgb,1);}
)GLSL";
static const char* HD_RASTER_FRAGMENT=R"GLSL(#version 310 es
#extension GL_EXT_shader_framebuffer_fetch : require
precision highp int;precision highp float;
layout(std430,binding=0) readonly buffer Vram {uint ram[];};
layout(std430,binding=1) readonly buffer Commands {int q[];};
layout(location=0) inout highp uvec4 destination;
flat in int primitive;
uniform int scale;
int e(int at,int n){return q[at+8+n];}
int floorDiv(int a,int b){return a/b-((a%b)<0?1:0);}
int blend(int back,int front,int mode){if(mode==0)return(back>>1)+(front>>1);if(mode==1)return min(31,back+front);if(mode==2)return max(0,back-front);return min(31,back+(front>>2));}
int texturePixel(int at,int u,int v,int clut){int window=e(at,2),mx=(window&31)*8,my=((window>>5)&31)*8;
 u=((u&255)&~mx)|(((window>>10)&31)*8&mx);v=((v&255)&~my)|(((window>>15)&31)*8&my);
 int page=e(at,1),x=(page&15)*64,y=((page>>4)&1)*256+v,depth=(page>>7)&3;
 if(depth>=2)return int(ram[(y&511)*1024+((x+u)&1023)]);
 int packed=int(ram[(y&511)*1024+((x+(u>>(depth==0?2:1)))&1023)]);
 int index=depth==0?(packed>>((u&3)*4))&15:(packed>>((u&1)*8))&255;
 return int(ram[((clut>>6)&511)*1024+(((clut&63)*16+index)&1023)]);
}
void drawAt(int x,int y){
 {int at=primitive*64;if(x<q[at+4]*scale||y<q[at+5]*scale||x>=(q[at+6]+1)*scale||y>=(q[at+7]+1)*scale)discard;
  int dx=x/scale-q[at+4],dy=y/scale-q[at+5],sx=x%scale,sy=y%scale,type=q[at],op=q[at+1];ivec3 color;ivec2 uv=ivec2(0);
  if(type==0){bool inside=true;for(int edge=0;edge<3;edge++){int p=at+16+edge*4;int w=(q[p]+q[p+1]*dx+q[p+2]*dy)*scale+q[p+1]*sx+q[p+2]*sy;if(w<0||(w==0&&q[p+3]==0))inside=false;}if(!inside)discard;}
  if(type<2){int values[5];for(int i=0;i<5;i++)values[i]=int(uint(q[at+28+i])+uint(q[at+33+i])*uint(dx)+uint(q[at+38+i])*uint(dy)+uint((q[at+33+i]/scale)*sx+(q[at+33+i]%scale)*sx/scale+(q[at+38+i]/scale)*sy+(q[at+38+i]%scale)*sy/scale))>>16;color=ivec3(values[0],values[1],values[2]);uv=ivec2(values[3],values[4]);}
  else{int ax=q[at+16],ay=q[at+17],lx=q[at+18],ly=q[at+19],steps=q[at+20];int i=abs(lx)>=abs(ly)?(x/scale-ax)*(lx<0?-1:1):(y/scale-ay)*(ly<0?-1:1);if(i<0||i>steps)discard;
   int px=steps==0?ax:ax+floorDiv(lx*i+steps/2,steps),py=steps==0?ay:ay+floorDiv(ly*i+steps/2,steps);if(px!=x/scale||py!=y/scale)discard;
   for(int k=0;k<3;k++)color[k]=q[at+28+k]+(steps==0?0:q[at+33+k]*i/steps);
  }
  int address=y*(1024*scale)+x,back=int(destination.r);if((e(at,6)&2)!=0&&(back&32768)!=0)discard;
  bool textured=(op&4)!=0,semi=(op&2)!=0;int mask=0;
  if(textured){int texel=texturePixel(at,uv.x,uv.y,q[at+2]);if(texel==0)discard;mask=texel&32768;semi=semi&&mask!=0;ivec3 tex=ivec3(texel&31,(texel>>5)&31,(texel>>10)&31);color=(op&1)!=0?tex*8:(tex*color)>>4;}
  const int dither[16]=int[16](-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2);int delta=q[at+3]!=0?dither[((y/scale)&3)*4+((x/scale)&3)]:0;color=clamp((color+delta)>>3,ivec3(0),ivec3(31));
  if(semi){int mode=(e(at,1)>>5)&3;for(int k=0;k<3;k++)color[k]=blend((back>>(k*5))&31,color[k],mode);}
  destination=uvec4(uint(color.r|(color.g<<5)|(color.b<<10)|mask|((e(at,6)&1)<<15)),0,0,0);
 }
}

void main(){drawAt(int(gl_FragCoord.x),int(gl_FragCoord.y));}
)GLSL";
