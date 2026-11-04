// Actual GLES execution test for the production upload helper. Uses desktop EGL/ANGLE.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "gpu_command_upload.h"
#include <cassert>
#include <cstdio>
#include <vector>

int main(){
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);assert(display!=EGL_NO_DISPLAY);assert(eglInitialize(display,nullptr,nullptr));assert(eglBindAPI(EGL_OPENGL_ES_API));
    EGLint attributes[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT_KHR,EGL_NONE};EGLConfig config;EGLint count=0;assert(eglChooseConfig(display,attributes,&config,1,&count)&&count);
    EGLint contextAttributes[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_CONTEXT_MINOR_VERSION_KHR,1,EGL_NONE};EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,contextAttributes);assert(context!=EGL_NO_CONTEXT);EGLint surfaceAttributes[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};EGLSurface surface=eglCreatePbufferSurface(display,config,surfaceAttributes);assert(surface!=EGL_NO_SURFACE);assert(eglMakeCurrent(display,surface,surface,context));
    std::printf("GLES upload validation: %s / %s\n",glGetString(GL_RENDERER),glGetString(GL_VERSION));
    const char* source="#version 310 es\nlayout(local_size_x=1) in;layout(std430,binding=0) readonly buffer Input {uint value;};layout(std430,binding=1) buffer Output {uint result[];};uniform int at;void main(){result[at]=value;}";
    GLuint shader=glCreateShader(GL_COMPUTE_SHADER);glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);GLint compiled=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&compiled);assert(compiled);GLuint program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glGetProgramiv(program,GL_LINK_STATUS,&compiled);assert(compiled);glUseProgram(program);GLint at=glGetUniformLocation(program,"at");
    GLuint buffers[2];glGenBuffers(2,buffers);glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[0]);glBufferData(GL_SHADER_STORAGE_BUFFER,8192,nullptr,GL_STREAM_DRAW);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,0,buffers[0]);
    std::vector<unsigned> expected(4096);glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);glBufferData(GL_SHADER_STORAGE_BUFFER,expected.size()*4,nullptr,GL_DYNAMIC_READ);glBindBufferBase(GL_SHADER_STORAGE_BUFFER,1,buffers[1]);
    GpuCommandUpload upload;
    for(unsigned i=0;i<expected.size();i++){expected[i]=i*123457u;upload.upload(buffers[0],8192,4,&expected[i]);glUniform1i(at,i);glDispatchCompute(1,1,1);}
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);const auto* actual=static_cast<const unsigned*>(glMapBufferRange(GL_SHADER_STORAGE_BUFFER,0,expected.size()*4,GL_MAP_READ_BIT));assert(actual);for(unsigned i=0;i<expected.size();i++)assert(actual[i]==expected[i]);glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);assert(glGetError()==GL_NO_ERROR);
    glDeleteBuffers(2,buffers);glDeleteProgram(program);glDeleteShader(shader);eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);eglDestroySurface(display,surface);eglDestroyContext(display,context);eglTerminate(display);std::puts("4,096 actual GLES compute dispatches retained their own uploaded command data.");
}
