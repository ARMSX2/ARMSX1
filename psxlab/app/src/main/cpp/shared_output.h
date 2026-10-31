#pragma once
#include <android/hardware_buffer.h>
#include <GLES2/gl2ext.h>

// EGL owns the rendering view; Vulkan/CPU access uses the same allocation.
// Producer/consumer work is ordered with completion waits or imported sync fds.
struct SharedOutput {
    AHardwareBuffer* buffer=nullptr;
    EGLImageKHR image=EGL_NO_IMAGE_KHR;
    GLuint texture=0,framebuffer=0;
    int width=0,height=0,stride=0;
    bool cpuReadable=false,available=true;
    void destroy(EGLDisplay display){
        if(framebuffer)glDeleteFramebuffers(1,&framebuffer);framebuffer=0;
        if(texture)glDeleteTextures(1,&texture);texture=0;
        if(image!=EGL_NO_IMAGE_KHR){auto release=reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));if(release)release(display,image);image=EGL_NO_IMAGE_KHR;}
        if(buffer)AHardwareBuffer_release(buffer);buffer=nullptr;width=height=stride=0;
    }
    bool ensure(EGLDisplay display,int w,int h,bool cpu){
        if(!available)return false;
        if(buffer&&w==width&&h==height&&cpu==cpuReadable)return true;
        destroy(display);
        auto client=reinterpret_cast<PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC>(eglGetProcAddress("eglGetNativeClientBufferANDROID"));
        auto create=reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
        auto target=reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if(!client||!create||!target){available=false;return false;}
        AHardwareBuffer_Desc desc{};desc.width=w;desc.height=h;desc.layers=1;desc.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage=AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT|AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|(cpu?AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN:0);
        if(AHardwareBuffer_allocate(&desc,&buffer)!=0){available=false;return false;}
        const EGLint attributes[]={EGL_IMAGE_PRESERVED_KHR,EGL_TRUE,EGL_NONE};image=create(display,EGL_NO_CONTEXT,EGL_NATIVE_BUFFER_ANDROID,client(buffer),attributes);
        if(image!=EGL_NO_IMAGE_KHR){glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);target(GL_TEXTURE_2D,image);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);}
        bool complete=image!=EGL_NO_IMAGE_KHR&&glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;glBindFramebuffer(GL_FRAMEBUFFER,0);
        if(!complete||glGetError()!=GL_NO_ERROR){destroy(display);available=false;while(glGetError()!=GL_NO_ERROR){}return false;}
        AHardwareBuffer_describe(buffer,&desc);width=w;height=h;stride=desc.stride;cpuReadable=cpu;return true;
    }
};
struct SharedOutputRead {
    AHardwareBuffer* buffer=nullptr;
    const uint32_t* pixels=nullptr;
    explicit SharedOutputRead(AHardwareBuffer* source){void* data=nullptr;if(source&&AHardwareBuffer_lock(source,AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN,-1,nullptr,&data)==0){buffer=source;pixels=static_cast<uint32_t*>(data);}}
    ~SharedOutputRead(){if(buffer)AHardwareBuffer_unlock(buffer,nullptr);}
};
