// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_api.h"
#define SWAN_GL_ROUTE
#include "gl_api.h"
#include <atomic>
#include <android/log.h>
namespace {
SwanShaders::Handle chain=nullptr;
GLuint input_texture=0,output_texture=0,read_fbo=0,input_fbo=0,output_fbo=0;
unsigned iw=0,ih=0,ow=0,oh=0;
uint64_t generation=0;size_t counter=0;
std::atomic<bool> active{false};
const void* Procedure(const char* name){return reinterpret_cast<const void*>(eglGetProcAddress(name));}
void Texture(GLuint& texture,unsigned width,unsigned height){if(!texture)glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
  glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
  // librashader binds its own sampler, which can use a mipmapped minification
  // filter. The host images contain exactly one level; bound their range so
  // GLES considers them complete rather than sampling opaque black.
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_BASE_LEVEL,0);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAX_LEVEL,0);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);}
void Framebuffer(GLuint& fbo,GLuint texture){if(!fbo)glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);}
void FreeChain(){if(chain){auto* api=SwanShaders::Functions();if(api)SwanShaders::Check(api->gl_free(&chain));chain=nullptr;}active=false;}
}
uint32_t SwanShaders::DrawGL(uint32_t source,uint32_t core_framebuffer,unsigned width,unsigned height,unsigned window_width,unsigned window_height,bool software,unsigned subframe,unsigned total,float nominal){
  const auto selection=Selected();
  if(selection.generation!=generation){FreeChain();generation=selection.generation;
    if(selection.enabled&&!selection.path.empty()){auto* api=Functions();Handle preset=nullptr;
      if(api&&Check(api->preset_create(selection.path.c_str(),&preset))){GlOptions options{5,300,false,true,true};
        if(Check(api->gl_create(&preset,Procedure,&options,&chain))){for(const auto& p:selection.parameters)Check(api->gl_parameter(&chain,p.first.c_str(),p.second));}
        if(preset)Check(api->preset_free(&preset));}
      if(!chain)__android_log_write(ANDROID_LOG_WARN,"SwanShaders","GLES preset failed; keeping plain presentation.");
    }
  }
  if(!chain||!width||!height||window_width>8192||window_height>8192)return 0;
  if(iw!=width||ih!=height){Texture(input_texture,width,height);Framebuffer(input_fbo,input_texture);iw=width;ih=height;}
  if(ow!=window_width||oh!=window_height){Texture(output_texture,window_width,window_height);Framebuffer(output_fbo,output_texture);ow=window_width;oh=window_height;}
  if(software)Framebuffer(read_fbo,source);
  glBindFramebuffer(GL_READ_FRAMEBUFFER,software?read_fbo:core_framebuffer);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,input_fbo);
  glDisable(GL_SCISSOR_TEST);glBlitFramebuffer(0,0,width,height,0,software?height:0,width,software?0:height,GL_COLOR_BUFFER_BIT,GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER,output_fbo);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  auto* api=Functions();const GlImage input{input_texture,GL_RGBA8,width,height},output{output_texture,GL_RGBA8,window_width,window_height};
  FrameOptions frame;frame.total_subframes=total;frame.current_subframe=subframe;
  frame.frames_per_second=nominal;frame.frametime_delta=uint32_t(1000.f/nominal);
  const bool okay=Check(api->gl_frame(&chain,counter,input,output,nullptr,nullptr,&frame));active=okay;
  if(subframe==total)counter++;
  if(!okay){FreeChain();__android_log_write(ANDROID_LOG_WARN,"SwanShaders","GLES frame failed; keeping plain presentation.");return 0;}
  return output_texture;
}
void SwanShaders::CloseGL(){FreeChain();generation=0;counter=0;
  for(auto texture:{input_texture,output_texture})if(texture)glDeleteTextures(1,&texture);
  for(auto fbo:{read_fbo,input_fbo,output_fbo})if(fbo)glDeleteFramebuffers(1,&fbo);
  input_texture=output_texture=read_fbo=input_fbo=output_fbo=0;iw=ih=ow=oh=0;
}
bool SwanShaders::ActiveGL(){return active;}
