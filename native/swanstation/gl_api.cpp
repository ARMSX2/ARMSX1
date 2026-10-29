// SPDX-License-Identifier: GPL-3.0-or-later
#include "gl_api.h"
#include "gl_worker.h"
#include <dlfcn.h>
#include <EGL/eglext.h>
#include <atomic>
#include <string>
#include <deque>
namespace {SwanGL::Api api;bool using_angle=false;
std::atomic<bool> presentation_time_ready{false};
bool timestamp_extension=false,timestamp_surface_ready=false;
EGLSurface timestamp_surface=EGL_NO_SURFACE;
std::deque<uint64_t> timestamp_frames;
std::atomic<int64_t> compositor_interval{0};
#define EGL_TAG(name) struct name##Tag {};
SWAN_EGL_FUNCTIONS(EGL_TAG)
EGL_TAG(eglQueryString) EGL_TAG(eglSurfaceAttrib) EGL_TAG(eglGetConfigAttrib)
EGL_TAG(eglPresentationTimeANDROID)
EGL_TAG(eglGetNextFrameIdANDROID) EGL_TAG(eglGetFrameTimestampsANDROID)
EGL_TAG(eglGetFrameTimestampSupportedANDROID) EGL_TAG(eglGetCompositorTimingANDROID)
#undef EGL_TAG
}
SwanGL::Api& SwanGL::Functions(){return api;}
bool SwanGL::IsAngle(){return using_angle;}
bool SwanGL::Load(bool angle, bool threaded) {
  // Separate handles keep the system HWUI context completely outside our ANGLE dispatch.
  static void* system_egl=dlopen("libEGL.so",RTLD_NOW|RTLD_LOCAL);
  static void* system_gl=dlopen("libGLESv3.so",RTLD_NOW|RTLD_LOCAL);
  static void* angle_gl=nullptr;static void* angle_egl=nullptr;
  if(angle){if(!angle_gl)angle_gl=dlopen("libGLESv2_angle.so",RTLD_NOW|RTLD_LOCAL);if(!angle_egl)angle_egl=dlopen("libEGL_angle.so",RTLD_NOW|RTLD_LOCAL);}
  void* egl=angle?angle_egl:system_egl;void* gl=angle?angle_gl:system_gl;
  if(!egl||!gl)return false;
  Api candidate;
  presentation_time_ready=false;
  candidate.eglQueryString=reinterpret_cast<decltype(candidate.eglQueryString)>(dlsym(egl,"eglQueryString"));
  candidate.eglSurfaceAttrib=reinterpret_cast<decltype(candidate.eglSurfaceAttrib)>(dlsym(egl,"eglSurfaceAttrib"));
#if ARMSX_PACING_DIAGNOSTICS
  candidate.eglGetConfigAttrib=reinterpret_cast<decltype(candidate.eglGetConfigAttrib)>(dlsym(egl,"eglGetConfigAttrib"));
#endif
#define EGL_LOAD(name) candidate.name=reinterpret_cast<decltype(candidate.name)>(dlsym(egl,#name));if(!candidate.name)return false;
  SWAN_EGL_FUNCTIONS(EGL_LOAD)
#undef EGL_LOAD
  candidate.presentationTime=reinterpret_cast<PFNEGLPRESENTATIONTIMEANDROIDPROC>(candidate.eglGetProcAddress("eglPresentationTimeANDROID"));
  candidate.nextFrameId=reinterpret_cast<PFNEGLGETNEXTFRAMEIDANDROIDPROC>(candidate.eglGetProcAddress("eglGetNextFrameIdANDROID"));
  candidate.frameTimestamps=reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSANDROIDPROC>(candidate.eglGetProcAddress("eglGetFrameTimestampsANDROID"));
  candidate.timestampSupported=reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC>(candidate.eglGetProcAddress("eglGetFrameTimestampSupportedANDROID"));
  candidate.compositorTiming=reinterpret_cast<PFNEGLGETCOMPOSITORTIMINGANDROIDPROC>(candidate.eglGetProcAddress("eglGetCompositorTimingANDROID"));
#define GL_LOAD(name) candidate.name=reinterpret_cast<decltype(candidate.name)>(dlsym(gl,#name));if(!candidate.name)candidate.name=reinterpret_cast<decltype(candidate.name)>(candidate.eglGetProcAddress(#name));if(!candidate.name)return false;
  SWAN_GL_FUNCTIONS(GL_LOAD)
  GL_LOAD(glGetString) GL_LOAD(glFlush)
#undef GL_LOAD
  candidate.glBlitFramebuffer=reinterpret_cast<decltype(candidate.glBlitFramebuffer)>(candidate.eglGetProcAddress("glBlitFramebuffer"));if(!candidate.glBlitFramebuffer)return false;
  candidate.glBindSampler=reinterpret_cast<decltype(candidate.glBindSampler)>(candidate.eglGetProcAddress("glBindSampler"));if(!candidate.glBindSampler)return false;
  // All EGL operations and host GL operations share the same owner as the
  // core's GLAD calls. The original pointers always come from this driver.
  if(threaded) {
#define EGL_ROUTE(name) candidate.name=SwanGLWorker::Wrap<name##Tag>(candidate.name);
  SWAN_EGL_FUNCTIONS(EGL_ROUTE)
  EGL_ROUTE(eglQueryString) EGL_ROUTE(eglSurfaceAttrib) EGL_ROUTE(eglGetConfigAttrib)
  candidate.presentationTime=SwanGLWorker::Wrap<eglPresentationTimeANDROIDTag>(candidate.presentationTime);
  candidate.nextFrameId=SwanGLWorker::Wrap<eglGetNextFrameIdANDROIDTag>(candidate.nextFrameId);
  candidate.frameTimestamps=SwanGLWorker::Wrap<eglGetFrameTimestampsANDROIDTag>(candidate.frameTimestamps);
  candidate.timestampSupported=SwanGLWorker::Wrap<eglGetFrameTimestampSupportedANDROIDTag>(candidate.timestampSupported);
  candidate.compositorTiming=SwanGLWorker::Wrap<eglGetCompositorTimingANDROIDTag>(candidate.compositorTiming);
#undef EGL_ROUTE
#define GL_ROUTE(name) candidate.name=reinterpret_cast<decltype(candidate.name)>(SwanGLWorker::Procedure(#name,reinterpret_cast<void*>(candidate.name)));if(!candidate.name)return false;
  SWAN_GL_FUNCTIONS(GL_ROUTE)
  GL_ROUTE(glBlitFramebuffer) GL_ROUTE(glBindSampler) GL_ROUTE(glGetString) GL_ROUTE(glFlush)
#undef GL_ROUTE
  }
  api=candidate;using_angle=angle;return true;
}
void SwanGL::ProbePresentationTime(EGLDisplay display) {
  const char* value=api.eglQueryString?api.eglQueryString(display,EGL_EXTENSIONS):nullptr;
  const std::string extensions=value?value:"";
  presentation_time_ready=api.presentationTime && (" "+extensions+" ").find(" EGL_ANDROID_presentation_time ")!=std::string::npos;
  timestamp_extension=(" "+extensions+" ").find(" EGL_ANDROID_get_frame_timestamps ")!=std::string::npos;
  ResetFrameTiming();
}
bool SwanGL::PresentAt(EGLDisplay display,EGLSurface surface,int64_t timestamp) {
  if(timestamp<=0||!presentation_time_ready.load())return false;
  const bool ok=api.presentationTime(display,surface,EGLnsecsANDROID(timestamp))==EGL_TRUE;
  if(!ok)presentation_time_ready=false;
  return ok;
}
bool SwanGL::PresentationTimeAvailable(){return presentation_time_ready.load();}
void SwanGL::ResetFrameTiming(){timestamp_frames.clear();timestamp_surface=EGL_NO_SURFACE;timestamp_surface_ready=false;compositor_interval=0;}
void SwanGL::PollFrameTiming(EGLDisplay display,EGLSurface surface) {
  if(surface==EGL_NO_SURFACE)return;
  if(surface!=timestamp_surface) {
    ResetFrameTiming();timestamp_surface=surface;
    timestamp_surface_ready=timestamp_extension&&api.nextFrameId&&api.frameTimestamps&&api.timestampSupported&&api.eglSurfaceAttrib&&
      api.timestampSupported(display,surface,EGL_DISPLAY_PRESENT_TIME_ANDROID)==EGL_TRUE&&
      api.eglSurfaceAttrib(display,surface,EGL_TIMESTAMPS_ANDROID,EGL_TRUE)==EGL_TRUE;
  }
  if(!timestamp_surface_ready)return;
  if(api.compositorTiming){const EGLint name=EGL_COMPOSITE_INTERVAL_ANDROID;EGLnsecsANDROID value=0;
    if(api.compositorTiming(display,surface,1,&name,&value)==EGL_TRUE&&value>=2000000&&value<=40000000)compositor_interval=value;}
  const EGLint name=EGL_DISPLAY_PRESENT_TIME_ANDROID;
  for(auto it=timestamp_frames.begin();it!=timestamp_frames.end();) {
    EGLnsecsANDROID value=EGL_TIMESTAMP_PENDING_ANDROID;
    const bool ok=api.frameTimestamps(display,surface,*it,1,&name,&value)==EGL_TRUE;
    if(ok&&value>0)SwanFramePacing::PresentationRecovery().Observe(*it,value);
    if(!ok||value!=EGL_TIMESTAMP_PENDING_ANDROID)it=timestamp_frames.erase(it);else ++it;
  }
}
void SwanGL::BeginTimedFrame(EGLDisplay display,EGLSurface surface,SwanFramePacing::PresentRequest request,int64_t now) {
  if(!timestamp_surface_ready||surface!=timestamp_surface)return;
  EGLuint64KHR id=0;
  if(api.nextFrameId(display,surface,&id)!=EGL_TRUE)return;
  timestamp_frames.push_back(id);if(timestamp_frames.size()>64)timestamp_frames.pop_front();
  SwanFramePacing::PresentationRecovery().Submitted(id,request,now);
}
float SwanGL::FrameRefreshHz(){const auto ns=compositor_interval.load();return ns?float(1e9/double(ns)):0;}
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
