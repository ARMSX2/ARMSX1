// SPDX-License-Identifier: GPL-3.0-or-later
#include "pacing_probe.h"
#if ARMSX_PACING_DIAGNOSTICS
#include "pacing_metrics.h"
#include "gl_api.h"
#include <EGL/eglext.h>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <array>
#include <cstring>
namespace {
using Clock=std::chrono::steady_clock;
int64_t Now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count();}
std::atomic<int> requested{0};
std::mutex mutex;
std::string session,status="No EGL game surface",backend;
std::deque<std::string> history;
struct Pending{EGLuint64KHR id;int64_t queued;};
struct Raw{EGLuint64KHR id;int64_t submit,duration;int mode,interval;};
struct Presented{EGLuint64KHR id;int64_t present,latch,ready;};
std::deque<Pending> pending;
std::deque<Raw> raw;
std::deque<Presented> scanout;
SwanPacing::Series submit,swap_wait,present,latch,ready_to_present;
SwanPacing::Cadence submit_clock,present_clock,latch_clock;
int64_t window_start=0;
unsigned submitted=0,query_errors=0,invalid=0,expired=0,swap_errors=0,interval_errors=0;
int applied=-1,last_requested=-1,min_interval=-1,max_interval=-1;
float game_hz=0,display_hz=0;
double composite_ms=0;
bool enabled=false;
PFNEGLGETNEXTFRAMEIDANDROIDPROC next_id=nullptr;
PFNEGLGETFRAMETIMESTAMPSANDROIDPROC timestamps=nullptr;
PFNEGLGETCOMPOSITORTIMINGANDROIDPROC compositor_timing=nullptr;
PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC compositor_supported=nullptr;
std::array<EGLint,3> names{};int name_count=0,present_index=-1,render_index=-1,latch_index=-1;
void Remember(std::string text){history.push_back(std::move(text));while(history.size()>64)history.pop_front();}
std::string Summary(){
  std::ostringstream out;out<<std::fixed<<std::setprecision(3)
    <<"requested_mode="<<(last_requested==1?"vsync_plus_timer":"baseline_timer")
    <<" swap_interval_request="<<applied<<" game_hz="<<game_hz<<" display_hz="<<display_hz
    <<" compositor_interval_ms="<<composite_ms<<" swaps="<<submitted
    <<" swap_errors="<<swap_errors<<" interval_request_errors="<<interval_errors
    <<" timestamp_query_errors="<<query_errors<<" invalid_present_events="<<invalid
    <<" expired_queries="<<expired<<" pending_queries="<<pending.size()<<" "
    <<submit.Text("cpu_submit_interval")<<" "<<swap_wait.Text("swap_call")<<" "
    <<present.Text("display_present_interval")<<" "<<latch.Text("composition_latch_interval")<<" "
    <<ready_to_present.Text("render_ready_to_present");return out.str();
}
void ClearWindow(){
  submit.Clear();swap_wait.Clear();present.Clear();latch.Clear();ready_to_present.Clear();
  submitted=query_errors=invalid=expired=swap_errors=interval_errors=0;window_start=Now();
}
void BoundaryLocked(const char* reason){
  if(submitted)Remember(Summary());Remember(std::string("boundary=")+reason);
  ClearWindow();submit_clock.Reset();present_clock.Reset();latch_clock.Reset();pending.clear();
}
void RefreshComposite(EGLDisplay display,EGLSurface surface){
  EGLint name=EGL_COMPOSITE_INTERVAL_ANDROID;EGLnsecsANDROID value=0;composite_ms=0;
  if(compositor_timing&&compositor_supported&&compositor_supported(display,surface,name)&&
     compositor_timing(display,surface,1,&name,&value)&&value>0)composite_ms=double(value)/1e6;
}
void Poll(EGLDisplay display,EGLSurface surface){
  if(!enabled)return;
  const auto now=Now();
  // Never wait for the GPU. Check at most four old frames per submitted image.
  for(int count=0;count<4&&!pending.empty();count++){
    if(now-pending.front().queued<50000000)break;
    std::array<EGLnsecsANDROID,3> values{};
    const auto frame=pending.front();
    if(!timestamps(display,surface,frame.id,name_count,names.data(),values.data())){
      query_errors++;pending.pop_front();continue;
    }
    bool waiting=false;for(int i=0;i<name_count;i++)waiting|=values[i]==EGL_TIMESTAMP_PENDING_ANDROID;
    if(waiting&&now-frame.queued<2000000000)break;
    pending.pop_front();if(waiting){expired++;continue;}
    scanout.push_back({frame.id,present_index>=0?values[present_index]:-1,
      latch_index>=0?values[latch_index]:-1,render_index>=0?values[render_index]:-1});
    while(scanout.size()>240)scanout.pop_front();
    if(present_index>=0){const auto time=values[present_index];
      if(time==EGL_TIMESTAMP_INVALID_ANDROID)invalid++;
      else if(time>0){present_clock.Add(time,present);
        if(render_index>=0&&values[render_index]>0&&time>=values[render_index])
          ready_to_present.Add(double(time-values[render_index])/1e6);
      }
    }
    if(latch_index>=0)latch_clock.Add(values[latch_index],latch);
  }
}
}
void SwanPacingProbe::Mode(int value){requested=value==1?1:0;}
void SwanPacingProbe::Begin(const std::string& id){
  std::lock_guard lock(mutex);session=id;history.clear();raw.clear();scanout.clear();pending.clear();
  enabled=false;status="No EGL game surface (Vulkan scanout timestamps are not collected)";
  backend.clear();applied=last_requested=-1;game_hz=display_hz=0;composite_ms=0;
  ClearWindow();submit_clock.Reset();present_clock.Reset();latch_clock.Reset();
}
void SwanPacingProbe::Boundary(const char* reason){std::lock_guard lock(mutex);BoundaryLocked(reason);}
int SwanPacingProbe::Interval(int baseline,bool normal_speed,unsigned subframes){
  return SwanPacing::SwapInterval(baseline,requested.load(),normal_speed,subframes);
}
void SwanPacingProbe::Surface(EGLDisplay display,EGLSurface surface,EGLConfig config){
  std::lock_guard lock(mutex);BoundaryLocked("surface_changed");enabled=false;name_count=0;
  raw.clear();scanout.clear(); // frame IDs belong to a surface; never correlate across recreation.
  present_index=render_index=latch_index=-1;next_id=nullptr;timestamps=nullptr;
  compositor_timing=nullptr;compositor_supported=nullptr;composite_ms=0;
  backend=SwanGL::IsAngle()?"ANGLE":"system_EGL";min_interval=max_interval=-1;
  const auto& api=SwanGL::Functions();
  if(api.eglGetConfigAttrib){api.eglGetConfigAttrib(display,config,EGL_MIN_SWAP_INTERVAL,&min_interval);
    api.eglGetConfigAttrib(display,config,EGL_MAX_SWAP_INTERVAL,&max_interval);}
  if(surface==EGL_NO_SURFACE){status="No EGL game surface";return;}
  const char* extensions=api.eglQueryString?api.eglQueryString(display,EGL_EXTENSIONS):nullptr;
  if(!extensions||!std::strstr(extensions,"EGL_ANDROID_get_frame_timestamps")){
    status="EGL frame timestamps unsupported; CPU swap timings only (not physical scanout)";return;}
  next_id=reinterpret_cast<PFNEGLGETNEXTFRAMEIDANDROIDPROC>(api.eglGetProcAddress("eglGetNextFrameIdANDROID"));
  timestamps=reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSANDROIDPROC>(api.eglGetProcAddress("eglGetFrameTimestampsANDROID"));
  const auto supported=reinterpret_cast<PFNEGLGETFRAMETIMESTAMPSUPPORTEDANDROIDPROC>(api.eglGetProcAddress("eglGetFrameTimestampSupportedANDROID"));
  if(!next_id||!timestamps||!supported||!api.eglSurfaceAttrib){status="EGL timestamp entry points unavailable";return;}
  for(const auto name:{EGL_DISPLAY_PRESENT_TIME_ANDROID,EGL_RENDERING_COMPLETE_TIME_ANDROID,EGL_COMPOSITION_LATCH_TIME_ANDROID}){
    if(!supported(display,surface,name))continue;
    if(name==EGL_DISPLAY_PRESENT_TIME_ANDROID)present_index=name_count;
    if(name==EGL_RENDERING_COMPLETE_TIME_ANDROID)render_index=name_count;
    if(name==EGL_COMPOSITION_LATCH_TIME_ANDROID)latch_index=name_count;
    names[name_count++]=name;
  }
  if(!name_count||!api.eglSurfaceAttrib(display,surface,EGL_TIMESTAMPS_ANDROID,EGL_TRUE)){
    status="EGL timestamp collection could not be enabled";return;}
  compositor_timing=reinterpret_cast<PFNEGLGETCOMPOSITORTIMINGANDROIDPROC>(api.eglGetProcAddress("eglGetCompositorTimingANDROID"));
  compositor_supported=reinterpret_cast<PFNEGLGETCOMPOSITORTIMINGSUPPORTEDANDROIDPROC>(api.eglGetProcAddress("eglGetCompositorTimingSupportedANDROID"));
  RefreshComposite(display,surface);
  enabled=true;status=present_index>=0?"Physical display-present timestamps available":"Latch/render timestamps only; physical scanout unavailable";
}
EGLBoolean SwanPacingProbe::Swap(EGLDisplay display,EGLSurface surface,int interval,EGLBoolean interval_ok,float nominal,float refresh){
  std::unique_lock lock(mutex);
  const int mode=requested.load();
  if(interval!=applied||mode!=last_requested||std::abs(nominal-game_hz)>.1f||std::abs(refresh-display_hz)>.1f){
    BoundaryLocked("mode_or_rate_changed");applied=interval;last_requested=mode;
    if(enabled)RefreshComposite(display,surface);
  }
  game_hz=nominal;display_hz=refresh;if(!interval_ok)interval_errors++;
  Poll(display,surface);
  EGLuint64KHR id=0;const bool got_id=enabled&&next_id(display,surface,&id);
  const auto begin=Now();lock.unlock();
  const auto okay=SwanGL::Functions().eglSwapBuffers(display,surface);const auto end=Now();lock.lock();
  submitted++;if(!okay)swap_errors++;
  submit_clock.Add(begin,submit);swap_wait.Add(double(end-begin)/1e6);
  raw.push_back({id,begin,end-begin,mode,interval});while(raw.size()>240)raw.pop_front();
  if(got_id&&okay){pending.push_back({id,end});if(pending.size()>120){pending.pop_front();expired++;}}
  if(end-window_start>=2000000000){if(enabled)RefreshComposite(display,surface);Remember(Summary());ClearWindow();}
  return okay;
}
std::string SwanPacingProbe::Snapshot(){
  std::lock_guard lock(mutex);std::ostringstream out;
  out<<"\n===== pacing-probe-v1 (diagnostic-only; not a confirmed fix) =====\n"
    <<"session="<<session<<" backend="<<backend<<" timestamps="<<status
    <<" config_swap_interval_min/max="<<min_interval<<"/"<<max_interval<<"\n"
    <<"Only display_present_interval uses physical scanout timestamps. CPU submission FPS is not display FPS.\n"
    <<"VSync test keeps the normal speed limiter; double waiting can reduce speed. Raw entries are monotonic ns.\n";
  for(const auto& line:history)out<<line<<"\n";
  out<<"current_window "<<Summary()<<"\nraw_swap_tail id,submit_ns,swap_call_ns,mode,swap_interval_request\n";
  for(const auto& sample:raw)out<<sample.id<<","<<sample.submit<<","<<sample.duration<<","<<sample.mode<<","<<sample.interval<<"\n";
  out<<"raw_timestamp_tail id,display_present_ns,latch_ns,render_complete_ns (-1=unavailable_or_invalid)\n";
  for(const auto& sample:scanout)out<<sample.id<<","<<sample.present<<","<<sample.latch<<","<<sample.ready<<"\n";
  return out.str();
}
#else
void SwanPacingProbe::Mode(int){}
std::string SwanPacingProbe::Snapshot(){return {};}
#endif
