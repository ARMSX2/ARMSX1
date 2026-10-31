#include "display_layout.h"
#include "native_timing.h"
// Original OpenGL ES compute backend for PSX Lab's integer raster rules.
#include "raster_core.h"
#include "gpu_shaders.h"
#include "hd_gpu_shaders.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl31.h>
#include <android/native_window_jni.h>
#include <array>
#include <string>
#include <stdexcept>
#include <mutex>
#include <cstring>
#include <atomic>
#include "shared_output.h"
extern bool psxVulkanSharedSupported(jlong);
extern bool psxVulkanSharedAsyncSupported(jlong);
extern bool psxVulkanPresentShared(JNIEnv*,jlong,jobject,AHardwareBuffer*,int,int,bool,int);
extern bool psxVulkanPresentRgba(JNIEnv*,jlong,jobject,const uint32_t*,int,int,bool);

bool psxVulkanNeedsPresent(jlong handle);
void psxVulkanNativeDimensions(jlong handle,int w,int h);
namespace {
std::mutex eglMutex;EGLDisplay sharedDisplay=EGL_NO_DISPLAY;int displayUsers=0;
EGLDisplay acquireDisplay(){std::lock_guard<std::mutex> lock(eglMutex);if(displayUsers==0){sharedDisplay=eglGetDisplay(EGL_DEFAULT_DISPLAY);if(sharedDisplay==EGL_NO_DISPLAY||!eglInitialize(sharedDisplay,nullptr,nullptr))throw std::runtime_error("EGL display initialization failed");}displayUsers++;return sharedDisplay;}
void releaseDisplay(){std::lock_guard<std::mutex> lock(eglMutex);if(--displayUsers==0){eglTerminate(sharedDisplay);sharedDisplay=EGL_NO_DISPLAY;}}
GLuint shader(GLenum type,const char* source){GLuint s=glCreateShader(type);glShaderSource(s,1,&source,nullptr);glCompileShader(s);GLint ok;glGetShaderiv(s,GL_COMPILE_STATUS,&ok);if(!ok){char text[4096]{};glGetShaderInfoLog(s,sizeof(text),nullptr,text);glDeleteShader(s);throw std::runtime_error(std::string("GPU shader: ")+text);}return s;}
GLuint program(const char* compute,const char* vertex=nullptr,const char* fragment=nullptr){
    GLuint p=glCreateProgram(),a=0,b=0;
    try{
        a=shader(compute?GL_COMPUTE_SHADER:GL_VERTEX_SHADER,compute?compute:vertex);glAttachShader(p,a);
        if(!compute){b=shader(GL_FRAGMENT_SHADER,fragment);glAttachShader(p,b);}
        glLinkProgram(p);glDeleteShader(a);a=0;if(b){glDeleteShader(b);b=0;}
        GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
        if(!ok){char text[4096]{};glGetProgramInfoLog(p,sizeof(text),nullptr,text);throw std::runtime_error(std::string("GPU program: ")+text);}
        return p;
    }catch(...){if(a)glDeleteShader(a);if(b)glDeleteShader(b);glDeleteProgram(p);throw;}
}
std::string scaledShader(const char* source,int scale){std::string text(source),uniform="uniform int scale;";size_t at=text.find(uniform);if(at==std::string::npos)throw std::runtime_error("HD scale declaration missing");text.replace(at,uniform.size(),"const int scale="+std::to_string(scale)+";");return text;}
void glCheck(){GLenum error=glGetError();if(error!=GL_NO_ERROR)throw std::runtime_error("OpenGL ES error "+std::to_string(error));}
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
class HardwareGpu {
    NativeTiming timing;
    // Uniform locations belong to a linked program and remain valid for its lifetime.
    struct Uniforms { GLint dirtyRows=-1,bounds=-1,count=-1,countPixels=-1,data=-1,dimensions=-1,display=-1,enhanced=-1,flip=-1,frameTexture=-1,operation=-1,serialMode=-1,shape=-1,smoothImage=-1,status=-1,replacementTextures=-1; };
    Uniforms transferProgramUniforms,hdNativeResolveProgramUniforms,hdTransferProgramUniforms,hdDrawProgramUniforms,drawProgramUniforms,hdPresentProgramUniforms,presentProgramUniforms,scanoutProgramUniforms,hdScanoutProgramUniforms;
    EGLDisplay display=EGL_NO_DISPLAY;EGLConfig config=nullptr;EGLContext context=EGL_NO_CONTEXT;EGLSurface pbuffer=EGL_NO_SURFACE,windowSurface=EGL_NO_SURFACE;ANativeWindow* window=nullptr;
    GLuint ramBuffer=0,commandBuffer=0,counterBuffer=0,drawProgram=0,transferProgram=0,scanoutProgram=0,presentProgram=0,frameTexture=0,vao=0;
    std::array<Primitive,32> pending{};int pendingCount=0;std::vector<Rect> reads,writes;bool countEnabled=false;
    std::array<int,1024*512> cpu{};i64 fallbackPixels=0;
    std::atomic<i64> draws{0},feedbackDraws{0},readbacks{0},passes{0},presented{0},nativeCopies{0},sharedFrames{0},asyncFrames{0};
    bool serialDispatch=false,windowSwapConfigured=false;
    double outputAspect=4.0/3.0;bool outputStretch=false,outputInteger=false,outputVsync=false;
    std::atomic<i64> uploadBytes{0},scanouts{0},reusedFrames{0},nativeMovies{0};
    bool softwareUploadKnown=false;jlong softwareRevision=0;std::array<int,8> softwareState{};
    const int scale;GLuint hdTexture=0,hdDrawProgram=0,hdTransferProgram=0,hdScanoutProgram=0,hdRasterProgram=0,hdFramebuffer=0,hdCommandBuffer=0,hdPresentProgram=0,resolveFramebuffer=0,hdNativeResolveProgram=0;
    int frameWidth=0,frameHeight=0;std::vector<uint32_t> resolved;ANativeWindow* softwareWindow=nullptr;SharedOutput sharedOutput,sharedOutputNext;SharedOutput* activeShared=&sharedOutput;bool sharedFlip=false;int sharedReadyFd=-1;
    std::vector<Primitive> hdPending;std::vector<Rect> hdReads,hdWrites;Rect nativeDirty{1023,511,0,0};std::array<uint64_t,32> nativeDirtyTiles{};bool fastHd=false,texturePacks=false;
    uint64_t contentRevision=0,presentRevision=~uint64_t(0);std::array<int,8> presentedState{};std::vector<Rect> presentedReads;bool displayChanged=true;ANativeWindow* presentedWindow=nullptr;int presentedWidth=0,presentedHeight=0;jweak presentedSurface=nullptr;
    bool unchanged(JNIEnv* env,jobject surface,ANativeWindow* target,const int* state,int w,int h)const{return presentRevision!=~uint64_t(0)&&!displayChanged&&presentedWindow==target&&presentedWidth==w&&presentedHeight==h&&presentedSurface&&env->IsSameObject(presentedSurface,surface)&&((presentedState[0]^state[0])&0x00ff0000)==0&&std::equal(presentedState.begin()+1,presentedState.end(),state+1);}
    void remember(JNIEnv* env,jobject surface,ANativeWindow* target,const int* state,int w,int h){if(!presentedSurface||!env->IsSameObject(presentedSurface,surface)){if(presentedSurface)env->DeleteWeakGlobalRef(presentedSurface);presentedSurface=env->NewWeakGlobalRef(surface);}presentRevision=contentRevision;presentedWindow=target;presentedWidth=w;presentedHeight=h;std::copy_n(state,8,presentedState.begin());displayChanged=false;presentedReads.clear();if(state[0]&0x800000)return;auto [width,height]=enhancedDimensions(state);int words=width/scale;if(state[0]&0x200000)words=std::min(1024,(words*3+1)/2);split(presentedReads,state[1]&1023,state[2]&511,words,height/scale);}
    void dirtyDisplay(const Rect& bounds){softwareUploadKnown=false;if(displayChanged||!bounds.valid())return;for(const auto& read:presentedReads)if(bounds.intersects(read)){displayChanged=true;return;}}
    void dirtyDisplayRegion(int x,int y,int w,int h){softwareUploadKnown=false;if(displayChanged||w<1||h<1)return;x&=1023;y&=511;w=std::min(w,1024);h=std::min(h,512);int firstW=std::min(w,1024-x),firstH=std::min(h,512-y);dirtyDisplay({x,y,x+firstW-1,y+firstH-1});if(w>firstW)dirtyDisplay({0,y,w-firstW-1,y+firstH-1});if(h>firstH){dirtyDisplay({x,0,x+firstW-1,h-firstH-1});if(w>firstW)dirtyDisplay({0,0,w-firstW-1,h-firstH-1});}}
    bool deferredNative()const{return scale>1&&fastHd&&!countEnabled;}
    static void include(Rect& a,const Rect& b){if(!b.valid())return;a={std::min(a.x0,b.x0),std::min(a.y0,b.y0),std::max(a.x1,b.x1),std::max(a.y1,b.y1)};}
    static uint64_t tileMask(const Rect& bounds){int first=bounds.x0>>4,last=bounds.x1>>4;return (~uint64_t(0)<<first)&(~uint64_t(0)>>(63-last));}
    void markNativeDirty(const Rect& bounds){if(!bounds.valid())return;include(nativeDirty,bounds);uint64_t mask=tileMask(bounds);for(int row=bounds.y0>>4;row<=bounds.y1>>4;row++)nativeDirtyTiles[row]|=mask;}
    bool nativeIntersects(const Rect& bounds)const{if(!bounds.intersects(nativeDirty))return false;uint64_t mask=tileMask(bounds);for(int row=bounds.y0>>4;row<=bounds.y1>>4;row++)if(nativeDirtyTiles[row]&mask)return true;return false;}
    void current(){if(eglGetCurrentContext()!=context&&!eglMakeCurrent(display,pbuffer,pbuffer,context))throw std::runtime_error("EGL context unavailable");}
    void bindBuffers(){glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,ramBuffer);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,commandBuffer);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,2,counterBuffer);if(hdTexture)glBindImageTexture(1,hdTexture,0,GL_FALSE,0,GL_READ_WRITE,GL_R32UI);}
    void barrier(){glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT|GL_BUFFER_UPDATE_BARRIER_BIT|GL_SHADER_IMAGE_ACCESS_BARRIER_BIT|GL_FRAMEBUFFER_BARRIER_BIT|GL_TEXTURE_FETCH_BARRIER_BIT);}
    void download(){NativeTiming::Scope measured(timing,NativeTiming::GPU_READBACK);flush();resolveNative();readbacks++;barrier();glBindBuffer(GL_SHADER_STORAGE_BUFFER,ramBuffer);void* p=glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,sizeof(cpu),GL_MAP_READ_BIT);if(!p)throw std::runtime_error("VRAM readback failed");std::memcpy(cpu.data(),p,sizeof(cpu));glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);}
    void uploadCpu(bool seedHd=true){softwareUploadKnown=false;uploadBytes+=sizeof(cpu);contentRevision++;displayChanged=true;flushHd();nativeDirty={1023,511,0,0};nativeDirtyTiles.fill(0);glBindBuffer(GL_SHADER_STORAGE_BUFFER,ramBuffer);glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizeof(cpu),cpu.data());barrier();if(seedHd&&hdTexture)transferHd(4,0,0,1024,512,0,0,0,0);}
    template<class Regions> static void split(Regions& out,int x,int y,int w,int h){int firstW=std::min(w,1024-x),firstH=std::min(h,512-y);out.push_back({x,y,x+firstW-1,y+firstH-1});if(w>firstW)out.push_back({0,y,w-firstW-1,y+firstH-1});if(h>firstH){out.push_back({x,0,x+firstW-1,h-firstH-1});if(w>firstW)out.push_back({0,0,w-firstW-1,h-firstH-1});}}
    static std::pair<int,int> textureRange(const Primitive& p,int component,int mask,int offset,bool enhanced){
        i64 a=p.q[28+component],dx=i64(p.q[33+component])*(p.q[6]-p.q[4]+int(enhanced)),dy=i64(p.q[38+component])*(p.q[7]-p.q[5]+int(enhanced));
        i64 lo=std::min({a,a+dx,a+dy,a+dx+dy})>>16,hi=std::max({a,a+dx,a+dy,a+dx+dy})>>16;
        if(hi-lo>=255)return{offset,(255&~mask)|offset};
        if(mask==0&&(lo>>8)==(hi>>8))return{int(lo)&255,int(hi)&255};
        int low=255,high=0;for(i64 v=lo;v<=hi;v++){int mapped=((int(v)&255)&~mask)|offset;low=std::min(low,mapped);high=std::max(high,mapped);}return{low,high};
    }
    RectList textureReads(const Primitive& p)const{RectList out;if(!(p.q[1]&4)||!p.bounds().valid())return out;int page=p.q[9],depth=(page>>7)&3,window=p.q[10],mx=(window&31)*8,my=((window>>5)&31)*8,shift=depth>=2?0:depth==1?1:2;
        auto [u0,u1]=textureRange(p,3,mx,((window>>10)&31)*8&mx,scale>1);auto [v0,v1]=textureRange(p,4,my,((window>>15)&31)*8&my,scale>1);
        split(out,((page&15)*64+(u0>>shift))&1023,(((page>>4)&1)*256+v0)&511,(u1>>shift)-(u0>>shift)+1,v1-v0+1);
        if(depth<2){int clut=p.q[2];split(out,(clut&63)*16,(clut>>6)&511,depth==0?16:256,1);}return out;}
    bool feedback(const Primitive& p,const RectList& ranges)const{for(const Rect& r:ranges)if(r.intersects(p.bounds()))return true;return false;}
    void add(const Primitive& p,const RectList& newReads){if(!p.bounds().valid())return;bool hazard=false;for(const auto& r:newReads)for(const auto& w:writes)hazard|=r.intersects(w);for(const auto& w:reads)hazard|=w.intersects(p.bounds());if(hazard||pendingCount==32)flush();pending[pendingCount++]=p;reads.insert(reads.end(),newReads.begin(),newReads.end());writes.push_back(p.bounds());}
    Primitive base(int op,int clut,bool dither,const int* env,Rect bounds,int type){Primitive p;p.q[0]=type;p.q[1]=op;p.q[2]=clut;p.q[3]=dither;p.q[4]=bounds.x0;p.q[5]=bounds.y0;p.q[6]=bounds.x1;p.q[7]=bounds.y1;std::copy_n(env,7,p.q+8);return p;}
    static Rect clip(Rect bounds,const int* env){return{std::max(bounds.x0,env[3]&1023),std::max(bounds.y0,(env[3]>>10)&511),std::min(bounds.x1,env[4]&1023),std::min(bounds.y1,(env[4]>>10)&511)};}
    Primitive triangle(Vertex a,Vertex b,Vertex c,int op,int clut,const int* env){Rect bounds{std::min({a.x,b.x,c.x}),std::min({a.y,b.y,c.y}),std::max({a.x,b.x,c.x})-1,std::max({a.y,b.y,c.y})-1};i64 area=edge(a,b,c.x,c.y);if(!area||bounds.x1-bounds.x0+1>=1024||bounds.y1-bounds.y0+1>=512){Primitive p;p.q[6]=-1;return p;}if(area<0){std::swap(b,c);area=-area;}bounds=clip(bounds,env);bool shade=op&16,tex=op&4;Primitive p=base(op,clut,(env[1]&512)&&(shade||(tex&&!(op&1))),env,bounds,0);
        Vertex starts[3]={b,c,a},ends[3]={c,a,b};i64 weights[3];for(int i=0;i<3;i++){int at=16+i*4;weights[i]=edge(starts[i],ends[i],bounds.x0,bounds.y0);p.q[at]=int(weights[i]);p.q[at+1]=-(ends[i].y-starts[i].y);p.q[at+2]=ends[i].x-starts[i].x;p.q[at+3]=topLeft(starts[i],ends[i]);}
        p.q[43]=a.x;p.q[44]=a.y;p.q[45]=b.x;p.q[46]=b.y;p.q[47]=c.x;p.q[48]=c.y;int av[5]={a.r,a.g,a.b,a.u,a.v},bv[5]={b.r,b.g,b.b,b.u,b.v},cv[5]={c.r,c.g,c.b,c.u,c.v};for(int i=0;i<5;i++){bool interpolate=i<3?shade:tex;p.q[28+i]=interpolate?start(weights[0],weights[1],weights[2],av[i],bv[i],cv[i],area):i<3?av[i]*65536:0;p.q[33+i]=interpolate?gx(a,b,c,av[i],bv[i],cv[i],area):0;p.q[38+i]=interpolate?gy(a,b,c,av[i],bv[i],cv[i],area):0;}return p;
    }
    Primitive rectangle(const int* packet,int op,const int* env){int size=(op>>3)&3;bool tex=op&4;int uv=tex?packet[2]:0,wh=size==0?packet[tex?3:2]:0,w=size==0?wh&1023:size==1?1:size==2?8:16,h=size==0?(uint32_t(wh)>>16)&511:size==1?1:size==2?8:16;Vertex a(packet[1],packet[0],uv,env[5]);Rect bounds=clip({a.x,a.y,a.x+w-1,a.y+h-1},env);Primitive p=base(op,uint32_t(uv)>>16,false,env,bounds,1);int dx=(env[1]&4096)?-1:1,dy=(env[1]&8192)?-1:1;p.q[28]=a.r*65536;p.q[29]=a.g*65536;p.q[30]=a.b*65536;p.q[31]=(a.u+(bounds.x0-a.x)*dx)*65536;p.q[32]=(a.v+(bounds.y0-a.y)*dy)*65536;p.q[36]=dx*65536;p.q[42]=dy*65536;return p;}
    Primitive line(const int* packet,int op,const int* env){bool shade=op&16;Vertex a(packet[1],packet[0],0,env[5]),b(packet[shade?3:2],shade?packet[2]:packet[0],0,env[5]);int dx=b.x-a.x,dy=b.y-a.y;Rect bounds=clip({std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x),std::max(a.y,b.y)},env);if(std::abs(dx)>=1024||std::abs(dy)>=512)bounds.x1=-1;Primitive p=base(op&~4,0,env[1]&512,env,bounds,2);p.q[16]=a.x;p.q[17]=a.y;p.q[18]=dx;p.q[19]=dy;p.q[20]=std::max(std::abs(dx),std::abs(dy));p.q[28]=a.r;p.q[29]=a.g;p.q[30]=a.b;p.q[33]=b.r-a.r;p.q[34]=b.g-a.g;p.q[35]=b.b-a.b;return p;}
    void transfer(int op,int x,int y,int w,int h,int a,int b,int c,int d){NativeTiming::Scope measured(timing,NativeTiming::GPU_TRANSFER);contentRevision++;if(op!=1)dirtyDisplayRegion(x,y,w,h);resolveNative();glUseProgram(transferProgram);bindBuffers();glUniform4i(transferProgramUniforms.shape,x,y,w,h);glUniform4i(transferProgramUniforms.data,a,b,c,d);glUniform1i(transferProgramUniforms.operation,op);glDispatchCompute(op==3?(std::max(w,h)+7)/8:(w+7)/8,op==3?1:(h+7)/8,1);barrier();transferHd(op,x,y,w,h,a,b,c,d);}
    void flushHd(){
        if(!fastHd||hdPending.empty())return;current();bindBuffers();glBindBuffer(GL_SHADER_STORAGE_BUFFER,hdCommandBuffer);glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,hdPending.size()*sizeof(Primitive),hdPending.data());glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,hdCommandBuffer);glBindFramebuffer(GL_FRAMEBUFFER,hdFramebuffer);glViewport(0,0,1024*scale,512*scale);glDisable(GL_BLEND);glDisable(GL_DEPTH_TEST);glDisable(GL_STENCIL_TEST);glDisable(GL_SCISSOR_TEST);glUseProgram(hdRasterProgram);glBindVertexArray(vao);glDrawArraysInstanced(GL_TRIANGLES,0,6,int(hdPending.size()));glBindFramebuffer(GL_FRAMEBUFFER,0);barrier();for(const auto& bounds:hdWrites)markNativeDirty(bounds);hdPending.clear();hdReads.clear();hdWrites.clear();bindBuffers();
    }
    void resolveNative(){
        flushHd();if(!nativeDirty.valid())return;NativeTiming::Scope measured(timing,NativeTiming::GPU_RESOLVE);current();bindBuffers();glUseProgram(hdNativeResolveProgram);glUniform4i(hdNativeResolveProgramUniforms.bounds,nativeDirty.x0,nativeDirty.y0,nativeDirty.x1,nativeDirty.y1);std::array<GLuint,64> rows;for(int row=0;row<32;row++){rows[row*2]=GLuint(nativeDirtyTiles[row]);rows[row*2+1]=GLuint(nativeDirtyTiles[row]>>32);}glUniform2uiv(hdNativeResolveProgramUniforms.dirtyRows,32,rows.data());glDispatchCompute((nativeDirty.x1-nativeDirty.x0+8)/8,(nativeDirty.y1-nativeDirty.y0+8)/8,1);barrier();nativeDirty={1023,511,0,0};nativeDirtyTiles.fill(0);nativeCopies++;
    }
    void queueHd(){
        bool textureHazard=false,writeHazard=false;
        if(deferredNative()){
            for(int i=0;i<pendingCount;i++)for(const Rect& read:textureReads(pending[i])){textureHazard|=nativeIntersects(read);for(const Rect& written:hdWrites)textureHazard|=read.intersects(written);}
        }else for(int i=0;i<pendingCount;i++)for(const Rect& read:hdReads)writeHazard|=read.intersects(pending[i].bounds());
        if(textureHazard)resolveNative();else if(writeHazard||hdPending.size()+pendingCount>512)flushHd();
        for(int i=0;i<pendingCount;i++){hdPending.push_back(pending[i]);if(deferredNative())hdWrites.push_back(pending[i].bounds());else{auto ranges=textureReads(pending[i]);hdReads.insert(hdReads.end(),ranges.begin(),ranges.end());}}
    }
    void transferHd(int op,int x,int y,int w,int h,int a,int b,int c,int d){if(!hdTexture)return;flushHd();glUseProgram(hdTransferProgram);bindBuffers();glUniform4i(hdTransferProgramUniforms.shape,x,y,w,h);glUniform4i(hdTransferProgramUniforms.data,a,b,c,d);glUniform1i(hdTransferProgramUniforms.operation,op);glDispatchCompute(op==3?(std::max(w,h)*scale+7)/8:(w*scale+7)/8,op==3?1:(h*scale+7)/8,1);barrier();}
    bool enhanced()const{return scale>1||texturePacks;}
    void ensureHd(){if(hdTexture)return;try{
GLint maxTexture=0;GLint64 maxStorage=0;glGetIntegerv(GL_MAX_TEXTURE_SIZE,&maxTexture);glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE,&maxStorage);if(maxTexture<1024*scale||maxStorage<GLint64(sizeof(Primitive))*512)throw std::runtime_error("GPU does not support the selected HD resolution");std::string drawSource=scaledShader(HD_DRAW_SHADER,scale),transferSource=scaledShader(HD_TRANSFER_SHADER,scale),scanoutSource=scaledShader(HD_SCANOUT_SHADER,scale);hdDrawProgram=program(drawSource.c_str());hdDrawProgramUniforms.replacementTextures=glGetUniformLocation(hdDrawProgram,"replacementTextures");hdDrawProgramUniforms.bounds=glGetUniformLocation(hdDrawProgram,"bounds");hdDrawProgramUniforms.count=glGetUniformLocation(hdDrawProgram,"count");hdDrawProgramUniforms.countPixels=glGetUniformLocation(hdDrawProgram,"countPixels");hdDrawProgramUniforms.serialMode=glGetUniformLocation(hdDrawProgram,"serialMode");hdTransferProgram=program(transferSource.c_str());hdTransferProgramUniforms.data=glGetUniformLocation(hdTransferProgram,"data");hdTransferProgramUniforms.operation=glGetUniformLocation(hdTransferProgram,"operation");hdTransferProgramUniforms.shape=glGetUniformLocation(hdTransferProgram,"shape");hdScanoutProgram=program(scanoutSource.c_str());hdScanoutProgramUniforms.display=glGetUniformLocation(hdScanoutProgram,"display");hdScanoutProgramUniforms.status=glGetUniformLocation(hdScanoutProgram,"status");std::string nativeSource=scaledShader(HD_NATIVE_RESOLVE_SHADER,scale);hdNativeResolveProgram=program(nativeSource.c_str());hdNativeResolveProgramUniforms.bounds=glGetUniformLocation(hdNativeResolveProgram,"bounds");hdNativeResolveProgramUniforms.dirtyRows=glGetUniformLocation(hdNativeResolveProgram,"dirtyRows[0]");std::string presentVertex=HD_PRESENT_VERTEX,presentFragment=scaledShader(HD_PRESENT_FRAGMENT,scale);hdPresentProgram=program(nullptr,presentVertex.c_str(),presentFragment.c_str());hdPresentProgramUniforms.display=glGetUniformLocation(hdPresentProgram,"display");hdPresentProgramUniforms.enhanced=glGetUniformLocation(hdPresentProgram,"enhanced");hdPresentProgramUniforms.flip=glGetUniformLocation(hdPresentProgram,"flip");hdPresentProgramUniforms.smoothImage=glGetUniformLocation(hdPresentProgram,"smoothImage");hdPresentProgramUniforms.status=glGetUniformLocation(hdPresentProgram,"status");
        glGenTextures(1,&hdTexture);glBindTexture(GL_TEXTURE_2D,hdTexture);glTexStorage2D(GL_TEXTURE_2D,1,GL_R32UI,1024*scale,512*scale);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        std::string extensions=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));fastHd=scale>1&&extensions.find("GL_EXT_shader_framebuffer_fetch ")!=std::string::npos;
        if(fastHd){std::string vertex=scaledShader(HD_RASTER_VERTEX,scale),fragment=scaledShader(HD_RASTER_FRAGMENT,scale);hdRasterProgram=program(nullptr,vertex.c_str(),fragment.c_str());glGenFramebuffers(1,&hdFramebuffer);glBindFramebuffer(GL_FRAMEBUFFER,hdFramebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,hdTexture,0);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("HD integer framebuffer unavailable");glBindFramebuffer(GL_FRAMEBUFFER,0);glGenBuffers(1,&hdCommandBuffer);glBindBuffer(GL_SHADER_STORAGE_BUFFER,hdCommandBuffer);glBufferData(GL_SHADER_STORAGE_BUFFER,sizeof(Primitive)*512,nullptr,GL_DYNAMIC_DRAW);hdPending.reserve(512);hdReads.reserve(2048);}
        transferHd(4,0,0,1024,512,0,0,0,0);
        }catch(...){dropHd();throw;}
    }
    void dropHd(){
        GLuint programs[]={hdDrawProgram,hdTransferProgram,hdScanoutProgram,hdRasterProgram,hdPresentProgram,hdNativeResolveProgram};for(GLuint p:programs)if(p)glDeleteProgram(p);
        hdDrawProgram=hdTransferProgram=hdScanoutProgram=hdRasterProgram=hdPresentProgram=hdNativeResolveProgram=0;
        if(hdTexture)glDeleteTextures(1,&hdTexture);hdTexture=0;
        if(hdFramebuffer)glDeleteFramebuffers(1,&hdFramebuffer);hdFramebuffer=0;
        if(hdCommandBuffer)glDeleteBuffers(1,&hdCommandBuffer);hdCommandBuffer=0;
        fastHd=false;std::vector<Primitive>().swap(hdPending);std::vector<Rect>().swap(hdReads);std::vector<Rect>().swap(hdWrites);
    }
    void detachWindow(){windowSwapConfigured=false;if(windowSurface!=EGL_NO_SURFACE){eglMakeCurrent(display,pbuffer,pbuffer,context);eglDestroySurface(display,windowSurface);windowSurface=EGL_NO_SURFACE;}if(window){ANativeWindow_release(window);window=nullptr;}}
public:
    void textures(bool enabled){
        if(enabled==texturePacks)return;current();flush();resolveNative();
        if(enabled){ensureHd();fastHd=false;texturePacks=true;}
        else{texturePacks=false;fastHd=hdRasterProgram!=0;}
        transferHd(4,0,0,1024,512,0,0,0,0);displayChanged=true;contentRevision++;
        if(!enabled&&scale==1)dropHd();
    }
    // The enhanced layer is host-only: no changed pixels enter canonical guest VRAM.
    void replacement(JNIEnv* env,jintArray image,int iw,int ih,int x,int y,int w,int h,int mask){
        if(!texturePacks)return;
        if(iw<1||ih<1||w<1||h<1||w>1024||h>512||iw%w||ih%h||iw/w!=ih/h||iw/w>8||int64_t(iw)*ih>4*1024*1024||env->GetArrayLength(image)!=int64_t(iw)*ih)throw std::runtime_error("Invalid texture replacement dimensions");
        current();flush();bindBuffers();barrier();glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);glBindTexture(GL_TEXTURE_2D,hdTexture);
        std::vector<jint> source(iw);std::vector<uint32_t> row(w*scale);int previous=-1;
        for(int dy=0;dy<h*scale;dy++){
            int sy=int(int64_t(dy)*ih/(h*scale));
            if(sy!=previous){env->GetIntArrayRegion(image,sy*iw,iw,source.data());if(env->ExceptionCheck())return;previous=sy;
                for(int dx=0;dx<w*scale;dx++){uint32_t p=uint32_t(source[int(int64_t(dx)*iw/(w*scale))]);int alpha=p>>24;uint32_t value=alpha?((p>>19)&31)|(((p>>11)&31)<<5)|(((p>>3)&31)<<10)|(alpha<192?32768:0):0;
                    if(alpha&&value==0)value=32768;row[dx]=value|((mask&1)<<15);}}
            int ox=(x&1023)*scale,oy=((y*scale+dy)%(512*scale)),first=std::min(w*scale,1024*scale-ox);
            glTexSubImage2D(GL_TEXTURE_2D,0,ox,oy,first,1,GL_RED_INTEGER,GL_UNSIGNED_INT,row.data());
            if(first<w*scale)glTexSubImage2D(GL_TEXTURE_2D,0,0,oy,w*scale-first,1,GL_RED_INTEGER,GL_UNSIGNED_INT,row.data()+first);
        }
        barrier();dirtyDisplayRegion(x,y,w,h);contentRevision++;glCheck();
    }
    void profile(bool enabled){timing.enabled.store(enabled);}
    jlongArray timings(JNIEnv* env){return timing.snapshot(env);}
    void configureDisplay(float aspect,bool stretch,bool integerScale){
        double next=std::isfinite(aspect)&&aspect>=0.1f&&aspect<=10.f?aspect:4.0/3.0;
        if(outputAspect==next&&outputStretch==stretch&&outputInteger==integerScale)return;
        outputAspect=next;outputStretch=stretch;outputInteger=integerScale;displayChanged=true;
    }
    HardwareGpu(int requestedScale=1):scale(requestedScale){if(scale<1||scale>5)throw std::runtime_error("Invalid internal resolution scale");reads.reserve(128);writes.reserve(32);try{display=acquireDisplay();const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT|EGL_WINDOW_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};EGLint count=0;if(!eglChooseConfig(display,attrs,&config,1,&count)||!count)throw std::runtime_error("No GLES3 EGL config");const EGLint contextAttrs[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};context=eglCreateContext(display,config,EGL_NO_CONTEXT,contextAttrs);if(context==EGL_NO_CONTEXT)throw std::runtime_error("GLES3 context unavailable");const EGLint surfaceAttrs[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};pbuffer=eglCreatePbufferSurface(display,config,surfaceAttrs);current();GLint major=0,minor=0;glGetIntegerv(GL_MAJOR_VERSION,&major);glGetIntegerv(GL_MINOR_VERSION,&minor);if(major<3||(major==3&&minor<1))throw std::runtime_error("OpenGL ES 3.1 is required");
        drawProgram=program(DRAW_SHADER);drawProgramUniforms.bounds=glGetUniformLocation(drawProgram,"bounds");drawProgramUniforms.count=glGetUniformLocation(drawProgram,"count");drawProgramUniforms.countPixels=glGetUniformLocation(drawProgram,"countPixels");drawProgramUniforms.serialMode=glGetUniformLocation(drawProgram,"serialMode");transferProgram=program(TRANSFER_SHADER);transferProgramUniforms.data=glGetUniformLocation(transferProgram,"data");transferProgramUniforms.operation=glGetUniformLocation(transferProgram,"operation");transferProgramUniforms.shape=glGetUniformLocation(transferProgram,"shape");scanoutProgram=program(SCANOUT_SHADER);scanoutProgramUniforms.display=glGetUniformLocation(scanoutProgram,"display");scanoutProgramUniforms.status=glGetUniformLocation(scanoutProgram,"status");presentProgram=program(nullptr,PRESENT_VERTEX,PRESENT_FRAGMENT);presentProgramUniforms.dimensions=glGetUniformLocation(presentProgram,"dimensions");presentProgramUniforms.frameTexture=glGetUniformLocation(presentProgram,"frameTexture");
        glGenBuffers(1,&ramBuffer);glBindBuffer(GL_SHADER_STORAGE_BUFFER,ramBuffer);glBufferData(GL_SHADER_STORAGE_BUFFER,sizeof(cpu),cpu.data(),GL_DYNAMIC_COPY);glGenBuffers(1,&commandBuffer);glBindBuffer(GL_SHADER_STORAGE_BUFFER,commandBuffer);glBufferData(GL_SHADER_STORAGE_BUFFER,sizeof(pending),nullptr,GL_DYNAMIC_DRAW);glGenBuffers(1,&counterBuffer);glBindBuffer(GL_SHADER_STORAGE_BUFFER,counterBuffer);uint32_t zero=0;glBufferData(GL_SHADER_STORAGE_BUFFER,4,&zero,GL_DYNAMIC_READ);
        if(scale>1)ensureHd();
        glGenTextures(1,&frameTexture);glBindTexture(GL_TEXTURE_2D,frameTexture);frameWidth=1024*scale;frameHeight=512*scale;glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,frameWidth,frameHeight);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);glGenVertexArrays(1,&vao);bindBuffers();glCheck();
        }catch(...){destroy();throw;}}
    ~HardwareGpu(){destroy();}
    void destroy(){if(display==EGL_NO_DISPLAY)return;if(context!=EGL_NO_CONTEXT){if(pbuffer!=EGL_NO_SURFACE)eglMakeCurrent(display,pbuffer,pbuffer,context);detachWindow();sharedOutput.destroy(display);sharedOutputNext.destroy(display);GLuint buffers[]={ramBuffer,commandBuffer,counterBuffer,hdCommandBuffer};glDeleteBuffers(4,buffers);GLuint programs[]={drawProgram,transferProgram,scanoutProgram,presentProgram,hdDrawProgram,hdTransferProgram,hdScanoutProgram,hdRasterProgram,hdPresentProgram,hdNativeResolveProgram};for(GLuint p:programs)if(p)glDeleteProgram(p);if(frameTexture)glDeleteTextures(1,&frameTexture);if(hdTexture)glDeleteTextures(1,&hdTexture);if(hdFramebuffer)glDeleteFramebuffers(1,&hdFramebuffer);if(resolveFramebuffer)glDeleteFramebuffers(1,&resolveFramebuffer);if(softwareWindow){ANativeWindow_release(softwareWindow);softwareWindow=nullptr;}if(vao)glDeleteVertexArrays(1,&vao);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroyContext(display,context);context=EGL_NO_CONTEXT;}if(pbuffer!=EGL_NO_SURFACE)eglDestroySurface(display,pbuffer);display=EGL_NO_DISPLAY;releaseDisplay();}
    void flush(){
        current();if(!pendingCount)return;NativeTiming::Scope measured(timing,NativeTiming::GPU_DRAW);passes++;
        if(scale>1&&fastHd&&!serialDispatch){queueHd();if(deferredNative()){pendingCount=0;reads.clear();writes.clear();return;}}if(enhanced()&&serialDispatch)resolveNative();
        bindBuffers();glBindBuffer(GL_SHADER_STORAGE_BUFFER,commandBuffer);glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,pendingCount*sizeof(Primitive),pending.data());
        Rect bounds{1023,511,0,0};for(int i=0;i<pendingCount;i++)include(bounds,pending[i].bounds());
        if(enhanced()&&!fastHd&&!serialDispatch){glUseProgram(hdDrawProgram);glUniform4i(hdDrawProgramUniforms.bounds,bounds.x0*scale,bounds.y0*scale,(bounds.x1+1)*scale-1,(bounds.y1+1)*scale-1);glUniform1i(hdDrawProgramUniforms.count,pendingCount);glUniform1i(hdDrawProgramUniforms.countPixels,0);glUniform1i(hdDrawProgramUniforms.serialMode,0);glUniform1i(hdDrawProgramUniforms.replacementTextures,texturePacks);glDispatchCompute(((bounds.x1-bounds.x0+1)*scale+7)/8,((bounds.y1-bounds.y0+1)*scale+7)/8,1);barrier();}
        glUseProgram(drawProgram);glUniform4i(drawProgramUniforms.bounds,bounds.x0,bounds.y0,bounds.x1,bounds.y1);glUniform1i(drawProgramUniforms.count,pendingCount);glUniform1i(drawProgramUniforms.countPixels,countEnabled);glUniform1i(drawProgramUniforms.serialMode,serialDispatch);glDispatchCompute(serialDispatch?1:(bounds.x1-bounds.x0+8)/8,serialDispatch?1:(bounds.y1-bounds.y0+8)/8,1);barrier();
        // Self-feedback keeps exact native serial ordering; refresh just that region.
        if(enhanced()&&serialDispatch)transferHd(4,bounds.x0,bounds.y0,bounds.x1-bounds.x0+1,bounds.y1-bounds.y0+1,0,0,0,0);
        pendingCount=0;reads.clear();writes.clear();
    }
    void draw(const int* packet,const int* env){current();int op=uint32_t(packet[0])>>24;Primitive p[2];int n=1;if(op<0x40){int at=1,color=packet[0],clut=0;bool shade=op&16,tex=op&4;Vertex v[4];int vertices=(op&8)?4:3;for(int i=0;i<vertices;i++){if(i&&shade)color=packet[at++];int xy=packet[at++],uv=tex?packet[at++]:0;if(tex&&i==0)clut=uint32_t(uv)>>16;v[i]=Vertex(xy,color,uv,env[5]);}p[0]=triangle(v[0],v[1],v[2],op,clut,env);if(vertices==4){p[1]=triangle(v[1],v[2],v[3],op,clut,env);n=2;}}else if(op<0x60)p[0]=line(packet,op,env);else p[0]=rectangle(packet,op,env);
        draws++;contentRevision++;for(int i=0;i<n;i++)dirtyDisplay(p[i].bounds());RectList ranges[2];bool overlap=false;for(int i=0;i<n;i++)if(p[i].bounds().valid()){ranges[i]=textureReads(p[i]);overlap|=this->feedback(p[i],ranges[i]);}if(overlap){feedbackDraws++;flush();serialDispatch=true;for(int i=0;i<n;i++)if(p[i].bounds().valid()){pending[0]=p[i];pendingCount=1;flush();}serialDispatch=false;}else for(int i=0;i<n;i++)add(p[i],ranges[i]);
    }
    void batch(const int* words,int length){for(int at=0;at<length;){int n=words[at++];if(n<2||n>12||at+n+7>length)throw std::runtime_error("Invalid hardware raster batch");int op=uint32_t(words[at])>>24,expected=op>=0x20&&op<0x40?((op&16)?((op&8)?4:3)*(2+((op>>2)&1)):1+((op&8)?4:3)*(1+((op>>2)&1))):op>=0x40&&op<0x60?((op&16)?4:3):op>=0x60&&op<0x80?2+((op>>2)&1)+((op&24)==0?1:0):0;if(n!=expected)throw std::runtime_error("Invalid hardware packet length");draw(words+at,words+at+n);at+=n+7;}} // Keep pending primitives across JNI batches; hazards, capacity, transfers and scanout flush them.
    void readRam(int* out){current();download();std::copy(cpu.begin(),cpu.end(),out);}
    void setRam(const int* in){current();flush();std::copy_n(in,cpu.size(),cpu.begin());uploadCpu();}
    void readToJava(JNIEnv* env,jintArray out){current();download();env->SetIntArrayRegion(out,0,cpu.size(),cpu.data());}
    void setFromJava(JNIEnv* env,jintArray in){current();flush();env->GetIntArrayRegion(in,0,cpu.size(),cpu.data());if(!env->ExceptionCheck())uploadCpu();}
    // Software rendering owns canonical VRAM. Scanout needs only the selected rows,
    // not a 2 MiB copy/upload of all 512 rows on every vblank.
    void setDisplayFromJava(JNIEnv* env,jintArray in,const int* state,jlong revision){
        if(scale!=1)throw std::runtime_error("Software display upload requires native scale");
        if(softwareUploadKnown&&softwareRevision==revision&&((softwareState[0]^state[0])&0x00ff0000)==0&&std::equal(softwareState.begin()+1,softwareState.begin()+7,state+1))return;
        current();flush();
        if(!(state[0]&0x800000)){
            int height=std::clamp((state[6]-state[5])*((state[0]&0x480000)==0x480000?2:1),1,512);
            int y=state[2]&511,first=std::min(height,512-y);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER,ramBuffer);
            auto rows=[&](int start,int count){if(!count)return;int offset=start*1024,length=count*1024;env->GetIntArrayRegion(in,offset,length,cpu.data()+offset);if(env->ExceptionCheck())return;glBufferSubData(GL_SHADER_STORAGE_BUFFER,offset*sizeof(int),length*sizeof(int),cpu.data()+offset);uploadBytes+=length*sizeof(int);};
            rows(y,first);if(env->ExceptionCheck())return;rows(0,height-first);if(env->ExceptionCheck())return;barrier();
        }
        contentRevision++;displayChanged=true;softwareRevision=revision;std::copy_n(state,8,softwareState.begin());softwareUploadKnown=true;
    }
    void releaseSurface(JNIEnv* env){if(presentedSurface)env->DeleteWeakGlobalRef(presentedSurface);presentedSurface=nullptr;}
    void stats(jlong* out)const{out[0]=draws.load();out[1]=feedbackDraws.load();out[2]=readbacks.load();out[3]=passes.load();out[4]=presented.load();out[5]=nativeCopies.load();out[6]=sharedFrames.load();out[7]=asyncFrames.load();out[8]=uploadBytes.load();out[9]=scanouts.load();out[10]=reusedFrames.load();out[11]=nativeMovies.load();}
    void verifyComputePath(){current();flush();resolveNative();fastHd=false;}
    void fill(int color,int xy,int wh){current();flush();int x=xy&0x3f0,y=(uint32_t(xy)>>16)&511,w=((wh&1023)+15)&~15,h=(uint32_t(wh)>>16)&511;if(!w||!h)return;int value=((color&255)>>3)|(((color>>8)&255)>>3<<5)|(((color>>16)&255)>>3<<10);transfer(0,x,y,w,h,value,0,0,0);}
    void upload(const int* words,int n,int start,int x,int y,int w,int total,int mask){current();flush();int count=std::min(n*2,total-start);if(count<=0)return;dirtyDisplayRegion(x,y,w,(total+w-1)/w);glBindBuffer(GL_SHADER_STORAGE_BUFFER,commandBuffer);glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,n*4,words);transfer(1,x,y,w,(count+w-1)/w,0,mask,start,count);}
    void copy(int from,int to,int wh,int mask){current();flush();int x=to&1023,y=(uint32_t(to)>>16)&511,sx=from&1023,sy=(uint32_t(from)>>16)&511,w=((wh-1)&1023)+1,h=(((uint32_t(wh)>>16)-1)&511)+1;RectList source,dest;split(source,sx,sy,w,h);split(dest,x,y,w,h);bool overlap=false;for(const auto& a:source)for(const auto& b:dest)overlap|=a.intersects(b);if(overlap&&(sx==x||sy==y)){transfer(3,x,y,w,h,sx,sy,mask,0);}else if(overlap){download();for(int row=0;row<h;row++)for(int col=0;col<w;col++){int at=((y+row)&511)*1024+((x+col)&1023);if(!(mask&2)||!(cpu[at]&32768))cpu[at]=cpu[((sy+row)&511)*1024+((sx+col)&1023)]|((mask&1)<<15);}uploadCpu(false);transferHd(4,x,y,w,h,0,0,0,0);}else transfer(2,x,y,w,h,sx,sy,mask,0);}
    void setCounting(bool on){current();flush();resolveNative();countEnabled=on;fallbackPixels=0;uint32_t zero=0;glBindBuffer(GL_SHADER_STORAGE_BUFFER,counterBuffer);glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,4,&zero);}
    i64 pixels(){current();flush();barrier();glBindBuffer(GL_SHADER_STORAGE_BUFFER,counterBuffer);auto* p=static_cast<uint32_t*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,4,GL_MAP_READ_BIT));if(!p)throw std::runtime_error("Counter readback failed");uint32_t v=*p;glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);return fallbackPixels+v;}
    std::pair<int,int> scanout(const int* state,bool nativeMovie=false){
        scanouts++;
        flushHd();if(scale>1&&(state[0]&0x200000))resolveNative();int status=state[0],divider=(status&0x10000)?7:((status>>17)&3)==0?10:((status>>17)&3)==1?8:((status>>17)&3)==2?5:4,width=std::clamp((state[4]-state[3])/divider,1,1024),height=std::clamp((state[6]-state[5])*((status&0x480000)==0x480000?2:1),1,512);
        const int sampleScale=nativeMovie?1:scale;const bool enhancedScan=enhanced()&&!nativeMovie;width*=sampleScale;height*=sampleScale;GLuint scan=enhancedScan?hdScanoutProgram:scanoutProgram;bindBuffers();glUseProgram(scan);glBindImageTexture(0,frameTexture,0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);glUniform4i((enhancedScan?hdScanoutProgramUniforms:scanoutProgramUniforms).display,state[1],state[2],width,height);glUniform1i((enhancedScan?hdScanoutProgramUniforms:scanoutProgramUniforms).status,status);glDispatchCompute((width+7)/8,(height+7)/8,1);glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT|GL_FRAMEBUFFER_BARRIER_BIT);
        return{width,height};
    }
    std::pair<int,int> enhancedDimensions(const int* state){int status=state[0],divider=(status&0x10000)?7:((status>>17)&3)==0?10:((status>>17)&3)==1?8:((status>>17)&3)==2?5:4;return{std::clamp((state[4]-state[3])/divider,1,1024)*scale,std::clamp((state[6]-state[5])*((status&0x480000)==0x480000?2:1),1,512)*scale};}
    void drawEnhanced(const int* state,bool flip){if(state[0]&0x200000)resolveNative();auto [w,h]=enhancedDimensions(state);bindBuffers();glUseProgram(hdPresentProgram);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,hdTexture);glUniform1i(hdPresentProgramUniforms.enhanced,0);glUniform4i(hdPresentProgramUniforms.display,state[1],state[2],w,h);glUniform1i(hdPresentProgramUniforms.status,state[0]);glUniform1i(hdPresentProgramUniforms.smoothImage,state[7]);glUniform1i(hdPresentProgramUniforms.flip,flip);glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,3);}
    void resolve(const int* state,int w,int h){
        current();flush();flushHd();
        auto nativeSize=enhancedDimensions(state);bool nativeMovie=(state[0]&0x200000)!=0;
        if(scale==1||(nativeMovie&&w==nativeSize.first/scale&&h==nativeSize.second/scale)){
            auto native=scanout(state,nativeMovie);if(native.first!=w||native.second!=h)throw std::runtime_error("Native shader input dimensions changed");
            if(!resolveFramebuffer)glGenFramebuffers(1,&resolveFramebuffer);glBindFramebuffer(GL_FRAMEBUFFER,resolveFramebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,frameTexture,0);
            if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("Native shader input framebuffer unavailable");
            resolved.resize(size_t(w)*h);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,resolved.data());glBindFramebuffer(GL_FRAMEBUFFER,0);glCheck();return;
        }
        if(w>frameWidth||h>frameHeight){frameWidth=std::max(w,frameWidth);frameHeight=std::max(h,frameHeight);glDeleteTextures(1,&frameTexture);glGenTextures(1,&frameTexture);glBindTexture(GL_TEXTURE_2D,frameTexture);glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,frameWidth,frameHeight);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);}
        if(!resolveFramebuffer)glGenFramebuffers(1,&resolveFramebuffer);glBindFramebuffer(GL_FRAMEBUFFER,resolveFramebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,frameTexture,0);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)throw std::runtime_error("HD output framebuffer unavailable");glViewport(0,0,w,h);drawEnhanced(state,false);resolved.resize(size_t(w)*h);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,resolved.data());glBindFramebuffer(GL_FRAMEBUFFER,0);glCheck();
    }
    bool renderShared(const int* state,int w,int h,bool cpu,bool asynchronous){NativeTiming::Scope measured(timing,NativeTiming::GPU_SCANOUT);
        current();flush();flushHd();if(!activeShared->ensure(display,w,h,cpu))return false;
        if(scale==1||(state[0]&0x200000)!=0){
            auto native=scanout(state,(state[0]&0x200000)!=0);
            if(!resolveFramebuffer)glGenFramebuffers(1,&resolveFramebuffer);glBindFramebuffer(GL_READ_FRAMEBUFFER,resolveFramebuffer);glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,frameTexture,0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER,activeShared->framebuffer);glBlitFramebuffer(0,0,native.first,native.second,0,0,w,h,GL_COLOR_BUFFER_BIT,state[7]?GL_LINEAR:GL_NEAREST);glBindFramebuffer(GL_FRAMEBUFFER,0);
        }else{glBindFramebuffer(GL_FRAMEBUFFER,activeShared->framebuffer);glViewport(0,0,w,h);drawEnhanced(state,false);glBindFramebuffer(GL_FRAMEBUFFER,0);}
        glCheck();sharedReadyFd=-1;
        if(asynchronous){auto create=reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));auto duplicate=reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));auto release=reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));if(create&&duplicate&&release){EGLSyncKHR fence=create(display,EGL_SYNC_NATIVE_FENCE_ANDROID,nullptr);if(fence!=EGL_NO_SYNC_KHR){glFlush();sharedReadyFd=duplicate(display,fence);release(display,fence);}}}
        // A sync fd lets Vulkan wait on the GPU; older drivers retain the blocking fallback.
        if(sharedReadyFd<0){NativeTiming::Scope measured(timing,NativeTiming::GL_PRODUCER);glFinish();}return true;
    }
    bool presentResolved(JNIEnv* env,jobject surface,const int* state,jlong vulkan){
        ANativeWindow* target=ANativeWindow_fromSurface(env,surface);if(!target)return false;int sw=ANativeWindow_getWidth(target),sh=ANativeWindow_getHeight(target);auto dimensions=enhancedDimensions(state);auto rect=psxDisplayRect(sw,sh,dimensions.first,dimensions.second,outputAspect,outputStretch,outputInteger);bool nativeMovie=(state[0]&0x200000)!=0;int w=(scale==1||nativeMovie)&&vulkan?dimensions.first/scale:rect.w,h=(scale==1||nativeMovie)&&vulkan?dimensions.second/scale:rect.h;if(w<1||h<1){ANativeWindow_release(target);throw std::runtime_error("HD target surface has no dimensions");}
        if(unchanged(env,surface,target,state,sw,sh)&&(!vulkan||!psxVulkanNeedsPresent(vulkan))){reusedFrames++;ANativeWindow_release(target);return true;}
        ANativeWindow* identity=target;
        sharedFlip=!sharedFlip;activeShared=sharedFlip?&sharedOutput:&sharedOutputNext;bool shared=(!vulkan||psxVulkanSharedSupported(vulkan))&&renderShared(state,w,h,!vulkan,vulkan&&psxVulkanSharedAsyncSupported(vulkan));
        if(vulkan){auto native=enhancedDimensions(state);psxVulkanNativeDimensions(vulkan,native.first/scale,native.second/scale);ANativeWindow_release(target);bool shown;if(shared)shown=psxVulkanPresentShared(env,vulkan,surface,activeShared->buffer,w,h,state[7],sharedReadyFd);else{resolve(state,w,h);shown=psxVulkanPresentRgba(env,vulkan,surface,resolved.data(),w,h,state[7]);}if(shown){remember(env,surface,identity,state,sw,sh);presented++;if(nativeMovie)nativeMovies++;if(shared){sharedFrames++;if(sharedReadyFd>=0)asyncFrames++;}}return shown;}
        SharedOutputRead read(shared?activeShared->buffer:nullptr);const uint32_t* pixels=read.pixels;int stride=activeShared->stride;
        if(!pixels){resolve(state,w,h);pixels=resolved.data();stride=w;shared=false;}
        if(target!=softwareWindow){if(softwareWindow)ANativeWindow_release(softwareWindow);softwareWindow=target;ANativeWindow_setBuffersGeometry(softwareWindow,0,0,WINDOW_FORMAT_RGBA_8888);}else ANativeWindow_release(target);
        ANativeWindow_Buffer buffer{};int lockResult=ANativeWindow_lock(softwareWindow,&buffer,nullptr);if(lockResult!=0)throw std::runtime_error("Software HD window lock failed: "+std::to_string(lockResult));int left=(buffer.width-w)/2,top=(buffer.height-h)/2;if(left<0||top<0){ANativeWindow_unlockAndPost(softwareWindow);throw std::runtime_error("Software HD buffer dimensions changed");}
        auto* out=static_cast<uint32_t*>(buffer.bits);for(int y=0;y<buffer.height;y++){auto* row=out+size_t(y)*buffer.stride;if(y<top||y>=top+h)std::fill_n(row,buffer.width,0xff000000u);else{std::fill_n(row,left,0xff000000u);std::memcpy(row+left,pixels+size_t(y-top)*stride,size_t(w)*4);std::fill_n(row+left+w,buffer.width-left-w,0xff000000u);}}
        ANativeWindow_unlockAndPost(softwareWindow);remember(env,surface,identity,state,sw,sh);presented++;if(shared)sharedFrames++;return true;
    }
    jintArray capture(JNIEnv* env,const int* state){current();flush();auto [w,h]=scanout(state);GLuint fbo=0;glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,frameTexture,0);if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE){glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);throw std::runtime_error("Scanout framebuffer incomplete");}std::vector<uint32_t> rgba(w*h);glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());glBindFramebuffer(GL_FRAMEBUFFER,0);glDeleteFramebuffers(1,&fbo);glCheck();std::vector<jint> argb(w*h);for(size_t i=0;i<rgba.size();i++){uint32_t v=rgba[i];argb[i]=int32_t((v&0xff00ff00u)|((v&255)<<16)|((v>>16)&255));}jintArray result=env->NewIntArray(w*h);if(result)env->SetIntArrayRegion(result,0,w*h,argb.data());return result;}
    void setOutputVsync(bool enabled){if(outputVsync!=enabled){outputVsync=enabled;windowSwapConfigured=false;}}
    void detachPresentation(){detachWindow();displayChanged=true;}
    bool beginWindow(JNIEnv* env,jobject surface){current();flush();ANativeWindow* requested=ANativeWindow_fromSurface(env,surface);if(!requested)return false;if(requested!=window){detachWindow();window=requested;EGLint format=0;eglGetConfigAttrib(display,config,EGL_NATIVE_VISUAL_ID,&format);ANativeWindow_setBuffersGeometry(window,0,0,format);windowSurface=eglCreateWindowSurface(display,config,window,nullptr);if(windowSurface==EGL_NO_SURFACE){detachWindow();return false;}}else ANativeWindow_release(requested);
        if(!eglMakeCurrent(display,windowSurface,windowSurface,context)){detachWindow();return false;}if(!windowSwapConfigured){eglSwapInterval(display,outputVsync?1:0);windowSwapConfigured=true;}
return true;}
    bool presentBlack(JNIEnv* env,jobject surface){if(!beginWindow(env,surface))return false;EGLint w=0,h=0;eglQuerySurface(display,windowSurface,EGL_WIDTH,&w);eglQuerySurface(display,windowSurface,EGL_HEIGHT,&h);glDisable(GL_SCISSOR_TEST);glViewport(0,0,w,h);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);if(!eglSwapBuffers(display,windowSurface)){detachWindow();return false;}displayChanged=true;presented++;return true;}
    bool present(JNIEnv* env,jobject surface,const int* state){NativeTiming::Scope measured(timing,NativeTiming::GLES_PRESENT);if(!beginWindow(env,surface))return false;
        EGLint sw=0,sh=0;eglQuerySurface(display,windowSurface,EGL_WIDTH,&sw);eglQuerySurface(display,windowSurface,EGL_HEIGHT,&sh);if(unchanged(env,surface,window,state,sw,sh)){reusedFrames++;return true;}
        bool nativeMovie=(state[0]&0x200000)!=0;std::pair<int,int> size;if(enhanced()&&!nativeMovie){flushHd();size=enhancedDimensions(state);}else size=scanout(state,nativeMovie);auto [width,height]=size;
        auto rect=psxDisplayRect(sw,sh,width,height,outputAspect,outputStretch,outputInteger);int dw=rect.w,dh=rect.h;glViewport(0,0,sw,sh);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);glViewport((sw-dw)/2,(sh-dh)/2,dw,dh);if(enhanced()&&!nativeMovie)drawEnhanced(state,true);else{glUseProgram(presentProgram);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,frameTexture);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,state[7]?GL_LINEAR:GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,state[7]?GL_LINEAR:GL_NEAREST);glUniform1i(presentProgramUniforms.frameTexture,0);glUniform2f(presentProgramUniforms.dimensions,float(width),float(height));glBindVertexArray(vao);glDrawArrays(GL_TRIANGLES,0,3);}glCheck();if(!eglSwapBuffers(display,windowSurface)){detachWindow();return false;}remember(env,surface,window,state,sw,sh);presented++;if(nativeMovie)nativeMovies++;return true;
    }
};
void failure(JNIEnv* env,const std::exception& e){env->ThrowNew(env->FindClass("java/lang/IllegalStateException"),e.what());}
HardwareGpu* gpu(jlong handle){return reinterpret_cast<HardwareGpu*>(handle);}
}
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_HardwareRasterizer_create(JNIEnv* env,jclass,jint scale){try{return reinterpret_cast<jlong>(new HardwareGpu(scale));}catch(const std::exception& e){failure(env,e);return 0;}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_destroy(JNIEnv* env,jclass,jlong handle){gpu(handle)->releaseSurface(env);delete gpu(handle);}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_verifyComputePathNative(JNIEnv* env,jclass,jlong handle){try{gpu(handle)->verifyComputePath();}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_batchNative(JNIEnv* env,jclass,jlong handle,jintArray data){try{int n=env->GetArrayLength(data);if(n<=0||n>512)throw std::runtime_error("Invalid GPU batch length");int words[512];env->GetIntArrayRegion(data,0,n,words);if(!env->ExceptionCheck())gpu(handle)->batch(words,n);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_readNative(JNIEnv* env,jclass,jlong handle,jintArray out){try{if(env->GetArrayLength(out)!=524288)throw std::runtime_error("Invalid VRAM readback size");gpu(handle)->readToJava(env,out);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_setNative(JNIEnv* env,jclass,jlong handle,jintArray in){try{if(env->GetArrayLength(in)!=524288)throw std::runtime_error("Invalid VRAM upload size");gpu(handle)->setFromJava(env,in);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_setDisplayNative(JNIEnv* env,jclass,jlong h,jintArray in,jintArray state,jlong revision){try{if(env->GetArrayLength(in)!=524288||env->GetArrayLength(state)!=8)throw std::runtime_error("Invalid software display upload");int values[8];env->GetIntArrayRegion(state,0,8,values);if(!env->ExceptionCheck())gpu(h)->setDisplayFromJava(env,in,values,revision);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_fillNative(JNIEnv* env,jclass,jlong h,jint c,jint xy,jint wh){try{gpu(h)->fill(c,xy,wh);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_copyNative(JNIEnv* env,jclass,jlong h,jint a,jint b,jint wh,jint mask){try{gpu(h)->copy(a,b,wh,mask);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_uploadNative(JNIEnv* env,jclass,jlong h,jintArray input,jint start,jint x,jint y,jint w,jint total,jint mask){try{int n=env->GetArrayLength(input);if(n<1||n>512||w<1||w>1024||start<0||start>=total)throw std::runtime_error("Invalid GPU upload");int words[512];env->GetIntArrayRegion(input,0,n,words);if(!env->ExceptionCheck())gpu(h)->upload(words,n,start,x,y,w,total,mask);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_countNative(JNIEnv* env,jclass,jlong h,jboolean enabled){try{gpu(h)->setCounting(enabled);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT jlong JNICALL Java_dev_psxlab_core_HardwareRasterizer_pixelsNative(JNIEnv* env,jclass,jlong h){try{return gpu(h)->pixels();}catch(const std::exception& e){failure(env,e);return 0;}}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_HardwareRasterizer_presentNative(JNIEnv* env,jclass,jlong h,jobject surface,jintArray state){try{if(env->GetArrayLength(state)!=8)throw std::runtime_error("Invalid display state");int values[8];env->GetIntArrayRegion(state,0,8,values);return !env->ExceptionCheck()&&gpu(h)->present(env,surface,values);}catch(const std::exception& e){failure(env,e);return false;}}
extern "C" JNIEXPORT jintArray JNICALL Java_dev_psxlab_core_HardwareRasterizer_captureNative(JNIEnv* env,jclass,jlong h,jintArray state){try{if(env->GetArrayLength(state)!=8)throw std::runtime_error("Invalid capture state");int values[8];env->GetIntArrayRegion(state,0,8,values);return env->ExceptionCheck()?nullptr:gpu(h)->capture(env,values);}catch(const std::exception& e){failure(env,e);return nullptr;}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_statsNative(JNIEnv* env,jclass,jlong h,jlongArray out){int n=env->GetArrayLength(out);if(n!=8&&n!=12){env->ThrowNew(env->FindClass("java/lang/IllegalArgumentException"),"Invalid GPU statistics");return;}jlong values[12];gpu(h)->stats(values);env->SetLongArrayRegion(out,0,n,values);}

extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_HardwareRasterizer_presentResolvedNative(JNIEnv* env,jclass,jlong h,jobject surface,jintArray state,jlong vk){try{if(env->GetArrayLength(state)!=8)throw std::runtime_error("Invalid HD display state");int values[8];env->GetIntArrayRegion(state,0,8,values);return !env->ExceptionCheck()&&gpu(h)->presentResolved(env,surface,values,vk);}catch(const std::exception& e){failure(env,e);return false;}}

extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_configureDisplayNative(JNIEnv*,jclass,jlong h,jfloat aspect,jboolean stretch,jboolean integerScale){gpu(h)->configureDisplay(aspect,stretch,integerScale);}

extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_setVsyncNative(JNIEnv*,jclass,jlong h,jboolean enabled){gpu(h)->setOutputVsync(enabled);}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_detachPresentationNative(JNIEnv*,jclass,jlong h){gpu(h)->detachPresentation();}
extern "C" JNIEXPORT jboolean JNICALL Java_dev_psxlab_core_HardwareRasterizer_presentBlackNative(JNIEnv* env,jclass,jlong h,jobject surface){try{return gpu(h)->presentBlack(env,surface);}catch(const std::exception& e){failure(env,e);return false;}}

extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_setProfilingNative(JNIEnv*,jclass,jlong handle,jboolean enabled){gpu(handle)->profile(enabled);}
extern "C" JNIEXPORT jlongArray JNICALL Java_dev_psxlab_core_HardwareRasterizer_timingsNative(JNIEnv* env,jclass,jlong handle){return gpu(handle)->timings(env);}

extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_texturesNative(JNIEnv* env,jclass,jlong h,jboolean enabled){try{gpu(h)->textures(enabled);}catch(const std::exception& e){failure(env,e);}}
extern "C" JNIEXPORT void JNICALL Java_dev_psxlab_core_HardwareRasterizer_replacementNative(JNIEnv* env,jclass,jlong h,jintArray pixels,jint iw,jint ih,jint x,jint y,jint w,jint height,jint mask){try{gpu(h)->replacement(env,pixels,iw,ih,x,y,w,height,mask);}catch(const std::exception& e){failure(env,e);}}
