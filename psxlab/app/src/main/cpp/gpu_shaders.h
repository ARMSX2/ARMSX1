#pragma once
// Original integer compute renderer. Coefficients come from our existing raster rules.
static const char* DRAW_SHADER=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) buffer Vram {uint ram[];};
layout(std430,binding=1) readonly buffer Commands {int q[];};
layout(std430,binding=2) buffer Statistics {uint pixels;};
uniform ivec4 bounds;
uniform int count;
uniform int countPixels;
uniform int serialMode;
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
 while(liveMask!=0u){int n=findLSB(liveMask);liveMask&=liveMask-1u;int at=n*64;if(x<q[at+4]||y<q[at+5]||x>q[at+6]||y>q[at+7])continue;
  int dx=x-q[at+4],dy=y-q[at+5],type=q[at],op=q[at+1];ivec3 color;ivec2 uv=ivec2(0);
  if(type==0){bool inside=true;for(int edge=0;edge<3;edge++){int p=at+16+edge*4;int w=q[p]+q[p+1]*dx+q[p+2]*dy;if(w<0||(w==0&&q[p+3]==0))inside=false;}if(!inside)continue;}
  if(type<2){int values[5];for(int i=0;i<5;i++)values[i]=int(uint(q[at+28+i])+uint(q[at+33+i])*uint(dx)+uint(q[at+38+i])*uint(dy))>>16;color=ivec3(values[0],values[1],values[2]);uv=ivec2(values[3],values[4]);}
  else{int ax=q[at+16],ay=q[at+17],lx=q[at+18],ly=q[at+19],steps=q[at+20];int i=abs(lx)>=abs(ly)?(x-ax)*(lx<0?-1:1):(y-ay)*(ly<0?-1:1);if(i<0||i>steps)continue;
   int px=steps==0?ax:ax+floorDiv(lx*i+steps/2,steps),py=steps==0?ay:ay+floorDiv(ly*i+steps/2,steps);if(px!=x||py!=y)continue;
   for(int k=0;k<3;k++)color[k]=q[at+28+k]+(steps==0?0:q[at+33+k]*i/steps);
  }
  int address=y*1024+x,back=int(ram[address]);if((e(at,6)&2)!=0&&(back&32768)!=0)continue;
  bool textured=(op&4)!=0,semi=(op&2)!=0;int mask=0;
  if(textured){int texel=texturePixel(at,uv.x,uv.y,q[at+2]);if(texel==0)continue;mask=texel&32768;semi=semi&&mask!=0;ivec3 tex=ivec3(texel&31,(texel>>5)&31,(texel>>10)&31);color=(op&1)!=0?tex*8:(tex*color)>>4;}
  const int dither[16]=int[16](-4,0,-3,1,2,-2,3,-1,-3,1,-4,0,3,-1,2,-2);int delta=q[at+3]!=0?dither[(y&3)*4+(x&3)]:0;color=clamp((color+delta)>>3,ivec3(0),ivec3(31));
  if(semi){int mode=(e(at,1)>>5)&3;for(int k=0;k<3;k++)color[k]=blend((back>>(k*5))&31,color[k],mode);}
  ram[address]=uint(color.r|(color.g<<5)|(color.b<<10)|mask|((e(at,6)&1)<<15));if(countPixels!=0)atomicAdd(pixels,1u);
 }
}
shared uint tileMask;
void main(){
 if(serialMode!=0){if(any(notEqual(gl_GlobalInvocationID,uvec3(0))))return;uint liveMask=count==32?0xffffffffu:(1u<<uint(count))-1u;for(int y=bounds.y;y<=bounds.w;y++)for(int x=bounds.x;x<=bounds.z;x++)drawAt(x,y,liveMask);return;}
 if(gl_LocalInvocationIndex==0u){ivec2 lo=ivec2(gl_WorkGroupID.xy)*8+bounds.xy,hi=lo+ivec2(7);uint liveMask=0u;for(int n=0;n<count;n++){int at=n*64;if(q[at+4]<=hi.x&&q[at+6]>=lo.x&&q[at+5]<=hi.y&&q[at+7]>=lo.y)liveMask|=1u<<uint(n);}tileMask=liveMask;}
 barrier();ivec2 xy=ivec2(gl_GlobalInvocationID.xy)+bounds.xy;if(any(greaterThan(xy,bounds.zw)))return;drawAt(xy.x,xy.y,tileMask);
})GLSL";
static const char* TRANSFER_SHADER=R"GLSL(#version 310 es
precision highp int;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) buffer Vram {uint ram[];};
layout(std430,binding=1) readonly buffer Data {uint words[];};
uniform ivec4 shape;
uniform ivec4 data;
uniform int operation;
void copyPixel(int x,int y){int address=((shape.y+y)&511)*1024+((shape.x+x)&1023);if((data.z&2)!=0&&(ram[address]&32768u)!=0u)return;ram[address]=ram[((data.y+y)&511)*1024+((data.x+x)&1023)]|uint((data.z&1)<<15);}
void main(){
 if(operation==3){if(gl_GlobalInvocationID.y!=0u)return;int i=int(gl_GlobalInvocationID.x);if(data.y==shape.y){if(i>=shape.w)return;for(int x=0;x<shape.z;x++)copyPixel(x,i);}else if(data.x==shape.x){if(i>=shape.z)return;for(int y=0;y<shape.w;y++)copyPixel(i,y);}return;}
 int x=int(gl_GlobalInvocationID.x),y=int(gl_GlobalInvocationID.y);if(x>=shape.z||y>=shape.w)return;int address=((shape.y+y)&511)*1024+((shape.x+x)&1023);uint value;
 if(operation==0)value=uint(data.x);
 else if(operation==1){int i=y*shape.z+x;if(i>=data.w)return;int index=data.z+i;address=((shape.y+index/shape.z)&511)*1024+((shape.x+index%shape.z)&1023);value=(words[i/2]>>uint((i&1)*16))&65535u;}
 else value=ram[((data.y+y)&511)*1024+((data.x+x)&1023)];
 if(operation!=0){if((data.z&2)!=0&&operation==2&&(ram[address]&32768u)!=0u)return;if(operation==1&&(data.y&2)!=0&&(ram[address]&32768u)!=0u)return;value|=uint(((operation==1?data.y:data.z)&1)<<15);}
 ram[address]=value;
})GLSL";
static const char* SCANOUT_SHADER=R"GLSL(#version 310 es
precision highp int;precision highp float;
layout(local_size_x=8,local_size_y=8) in;
layout(std430,binding=0) readonly buffer Vram {uint ram[];};
layout(rgba8,binding=0) writeonly uniform highp image2D frameImage;
uniform ivec4 display;
uniform int status;
uint readByte(int row,int x){return(ram[row+((x>>1)&1023)]>>uint((x&1)*8))&255u;}
void main(){ivec2 p=ivec2(gl_GlobalInvocationID.xy);if(p.x>=display.z||p.y>=display.w)return;vec3 rgb=vec3(0);int row=((display.y+p.y)&511)*1024;
 if((status&0x800000)==0){if((status&0x200000)==0){uint v=ram[row+((display.x+p.x)&1023)];uvec3 c=uvec3(v&31u,(v>>5)&31u,(v>>10)&31u);rgb=vec3((c<<3)|(c>>2))/255.0;}
 else{int x=display.x*2+p.x*3;rgb=vec3(readByte(row,x),readByte(row,x+1),readByte(row,x+2))/255.0;}}
 imageStore(frameImage,p,vec4(rgb,1));
})GLSL";
static const char* PRESENT_VERTEX=R"GLSL(#version 310 es
out highp vec2 uv;
void main(){vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);gl_Position=vec4(p*2.0-1.0,0,1);uv=vec2(p.x,1.0-p.y);}
)GLSL";
static const char* PRESENT_FRAGMENT=R"GLSL(#version 310 es
precision highp float;
in highp vec2 uv;uniform sampler2D frameTexture;uniform vec2 dimensions;out vec4 color;
void main(){vec2 at=clamp(uv*dimensions,vec2(0.5),dimensions-vec2(0.5));color=texture(frameTexture,at/vec2(textureSize(frameTexture,0)));}
)GLSL";
