// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_api.h"
#include <dlfcn.h>
#include <EGL/eglext.h>
namespace {SwanGL::Api api;bool using_angle=false;}
SwanGL::Api& SwanGL::Functions(){return api;}
bool SwanGL::IsAngle(){return using_angle;}
bool SwanGL::Load(bool angle) {
  // Separate handles keep the system HWUI context completely outside our ANGLE dispatch.
  static void* system_egl=dlopen("libEGL.so",RTLD_NOW|RTLD_LOCAL);
  static void* system_gl=dlopen("libGLESv3.so",RTLD_NOW|RTLD_LOCAL);
  static void* angle_gl=nullptr;static void* angle_egl=nullptr;
  if(angle){if(!angle_gl)angle_gl=dlopen("libGLESv2_angle.so",RTLD_NOW|RTLD_LOCAL);if(!angle_egl)angle_egl=dlopen("libEGL_angle.so",RTLD_NOW|RTLD_LOCAL);}
  void* egl=angle?angle_egl:system_egl;void* gl=angle?angle_gl:system_gl;
  if(!egl||!gl)return false;
  Api candidate;
#define EGL_LOAD(name) candidate.name=reinterpret_cast<decltype(candidate.name)>(dlsym(egl,#name));if(!candidate.name)return false;
  SWAN_EGL_FUNCTIONS(EGL_LOAD)
#undef EGL_LOAD
#define GL_LOAD(name) candidate.name=reinterpret_cast<decltype(candidate.name)>(dlsym(gl,#name));if(!candidate.name)candidate.name=reinterpret_cast<decltype(candidate.name)>(candidate.eglGetProcAddress(#name));if(!candidate.name)return false;
  SWAN_GL_FUNCTIONS(GL_LOAD)
#undef GL_LOAD
  candidate.glBlitFramebuffer=reinterpret_cast<decltype(candidate.glBlitFramebuffer)>(candidate.eglGetProcAddress("glBlitFramebuffer"));if(!candidate.glBlitFramebuffer)return false;
  candidate.glBindSampler=reinterpret_cast<decltype(candidate.glBindSampler)>(candidate.eglGetProcAddress("glBindSampler"));if(!candidate.glBindSampler)return false;
  api=candidate;using_angle=angle;return true;
}
EGLDisplay SwanGL::Display(){
  if(using_angle){
    // EGL_EXT_platform_base + ANGLE's explicitly Vulkan-backed display, not a guessed default.
    using Platform=EGLDisplay(*)(EGLenum,void*,const EGLint*);
    auto platform=reinterpret_cast<Platform>(api.eglGetProcAddress("eglGetPlatformDisplayEXT"));
    constexpr EGLint PlatformAngle=0x3202,TypeAngle=0x3203,TypeVulkan=0x3450;
    const EGLint attributes[]={TypeAngle,TypeVulkan,EGL_NONE};
    if(platform)return platform(PlatformAngle,EGL_DEFAULT_DISPLAY,attributes);
    return EGL_NO_DISPLAY;
  }
  return api.eglGetDisplay(EGL_DEFAULT_DISPLAY);
}
