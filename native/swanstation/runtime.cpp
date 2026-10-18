// SPDX-License-Identifier: GPL-3.0-or-later
// New libretro frontend. No current-DuckStation or post-LGPL ARMSX1 code is used.
#include "runtime.h"
#include "multiplayer.h"
#include "controller_devices.h"
#include "libretro.h"
#include <android/native_window_jni.h>
#include <android/log.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <dlfcn.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdarg>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <cstring>
#include <unistd.h>
#include <sys/sysinfo.h>
#include "png.h"
#include "catalogue.h"
#include "vulkan_host.h"
#include "presentation.h"
#include "achievements.h"
#include "shader_api.h"
#include "performance.h"
#include "diagnostics.h"
#include "statistics.h"
#include "disc_switch.h"
#include "audio_warmup.h"
#include "audio_queue.h"
#include "gl_presentation.h"
#ifndef ARMSX_SOURCE_COMMIT
#define ARMSX_SOURCE_COMMIT "unknown"
#endif
#define SWAN_GL_ROUTE
#include "gl_api.h"
namespace {
using Clock=std::chrono::steady_clock;
SwanDiagnostics::Journal diagnostic_session;
std::mutex state_mutex, session_mutex, audio_mutex;
std::condition_variable wake;
std::deque<std::function<void()>> work;
std::map<std::string,std::string> options;
std::set<std::string> unavailable;
std::string data_root, bios_root, config_path, save_root, game_path;
std::string state_identity, state_source, requested_identity, requested_source;
std::string current_serial;
std::array<std::string,2> last_card_paths;
std::map<std::string,std::string> config_values;
std::vector<SwanCatalogue::Group> cheat_groups;
std::atomic<int> cheats_armed{0};
std::deque<std::vector<unsigned char>> rewind_states;
std::atomic<bool> rewind_enabled{false}, rewind_active{false};
std::atomic<int64_t> rewind_bytes{0}, rewind_snapshot_bytes{0};
int rewind_seconds=10, rewind_frequency=2;
unsigned rewind_tick=0;
retro_savestate_context state_context=RETRO_SAVESTATE_CONTEXT_NORMAL;
std::atomic<bool> statistics_enabled{false};
std::mutex statistics_mutex;
std::vector<double> statistics;
double present_ms=0;
std::atomic<bool> active{false}, stop{false}, paused{false}, audio_suspended{false}, fast{false}, options_changed{false};
std::atomic<int> frames{0}, speed{100};
std::atomic<int64_t> audio_nonzero{0};
std::atomic<float> fps{0}, nominal{60}, fast_speed{2};
std::atomic<bool> limit_speed{true};
std::array<std::array<std::atomic<int16_t>,16>,8> buttons{};
std::array<std::array<std::atomic<int16_t>,4>,8> axes{};
unsigned multiplayer_mode=0;
std::array<unsigned,2> controller_devices{SwanControllers::DualShock,SwanControllers::DualShock};
std::thread::id emulation_thread;
uint64_t next_run_token=1, reserved_run_token=0, live_run_token=0, cancelled_run_token=0;
ANativeWindow* window=nullptr;
uint64_t window_owner=0;
std::atomic<bool> presentation_suspended{false};
unsigned window_revision=0;
JavaVM* java_vm=nullptr;
jclass java_class=nullptr;
jmethodID notice_method=nullptr, pause_method=nullptr;
EGLDisplay display=EGL_NO_DISPLAY;
EGLConfig egl_config=nullptr;
EGLContext context=EGL_NO_CONTEXT;
EGLSurface pbuffer=EGL_NO_SURFACE, window_surface=EGL_NO_SURFACE;
GLuint framebuffer=0, frame_texture=0, depth_buffer=0, upload_texture=0, shader=0, vao=0;
unsigned target_width=0, target_height=0;
unsigned video_width=0, video_height=0;
bool software_video=false;
bool requested_vulkan=false;
std::string requested_gl_driver;
SwanPresentation::Options display_options;
std::vector<uint32_t> software_pixels;
retro_hw_render_callback hardware{};
retro_system_av_info av{};
retro_disk_control_ext_callback disk{};
retro_pixel_format pixels=RETRO_PIXEL_FORMAT_0RGB1555;
std::string active_renderer="Stopped";
AAudioStream* audio=nullptr;
SwanAudio::Queue<> audio_queue;
SwanAudio::Warmup audio_warmup;
void* library=nullptr;
#define API(ret,name,args) ret (*name) args=nullptr
API(void,core_retro_init,()); API(void,core_retro_deinit,()); API(void,core_retro_run,()); API(void,core_retro_reset,());
API(bool,core_retro_load_game,(const retro_game_info*)); API(void,core_retro_unload_game,());
API(void,core_retro_get_system_av_info,(retro_system_av_info*));
API(void,core_retro_set_environment,(retro_environment_t)); API(void,core_retro_set_video_refresh,(retro_video_refresh_t));
API(void,core_retro_set_audio_sample,(retro_audio_sample_t)); API(void,core_retro_set_audio_sample_batch,(retro_audio_sample_batch_t));
API(void,core_retro_set_input_poll,(retro_input_poll_t)); API(void,core_retro_set_input_state,(retro_input_state_t));
API(void,core_retro_set_controller_port_device,(unsigned,unsigned));
API(size_t,core_retro_serialize_size,()); API(bool,core_retro_serialize,(void*,size_t)); API(bool,core_retro_unserialize,(const void*,size_t));
API(void,core_retro_cheat_reset,()); API(void,core_retro_cheat_set,(unsigned,bool,const char*));
API(bool,bridge_disc_serial,(const char*,char*,size_t));
API(bool,bridge_disc_serial_at,(const char*,unsigned,char*,size_t));
API(int,bridge_mounted_subimage,());
API(uint64_t,bridge_card_digest,()); API(bool,bridge_card_path,(unsigned,char*,size_t));
API(bool,bridge_validate_cheat,(const char*));
API(uint32_t,bridge_guest_frame,());
API(void,bridge_texture_options,(bool,bool,const char*));
API(uint64_t,bridge_texture_metric,(unsigned));
API(bool,bridge_companion_vram,(void*,size_t));
API(bool,bridge_ctr_hud,(bool)); API(const char*,bridge_ctr_hud_status,());
API(void,bridge_statistics_reset,());API(void,bridge_statistics_begin,(bool));
API(void,bridge_statistics_end,());API(void,bridge_statistics_read,(double*,size_t));
std::atomic<int64_t> ctr_hud_lease{0};
API(void*,core_retro_get_memory_data,(unsigned)); API(size_t,core_retro_get_memory_size,(unsigned));
#undef API
void Log(int priority,const char* message) {
  __android_log_write(priority,"EmuCorePSX",message);
  if(priority>=ANDROID_LOG_WARN)diagnostic_session.Core(priority>=ANDROID_LOG_ERROR,message);
}
void CoreLog(retro_log_level level,const char* fmt,...) {
  char buffer[4096]; va_list args; va_start(args,fmt); vsnprintf(buffer,sizeof(buffer),fmt,args); va_end(args);
  Log(level==RETRO_LOG_ERROR?ANDROID_LOG_ERROR:level==RETRO_LOG_WARN?ANDROID_LOG_WARN:ANDROID_LOG_INFO,buffer);
}
void JavaNotice(const std::string& message) {
  Log(ANDROID_LOG_WARN,message.c_str());
  if(!java_vm || !java_class || !notice_method) return;
  JNIEnv* env=nullptr; bool attached=java_vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK;
  if(attached && java_vm->AttachCurrentThread(&env,nullptr)!=JNI_OK) return;
  jstring text=env->NewStringUTF(message.c_str());
  if(text) {env->CallStaticVoidMethod(java_class,notice_method,text); env->DeleteLocalRef(text);}
  if(env->ExceptionCheck()) {env->ExceptionDescribe();env->ExceptionClear();}
  if(attached) java_vm->DetachCurrentThread();
}
void NotifyPause(bool value) {
  if(!java_vm || !java_class || !pause_method) return;
  JNIEnv* env=nullptr; bool attached=java_vm->GetEnv(reinterpret_cast<void**>(&env),JNI_VERSION_1_6)!=JNI_OK;
  if(attached && java_vm->AttachCurrentThread(&env,nullptr)!=JNI_OK) return;
  env->CallStaticVoidMethod(java_class,pause_method,static_cast<jboolean>(value));
  if(env->ExceptionCheck()) {env->ExceptionDescribe();env->ExceptionClear();}
  if(attached) java_vm->DetachCurrentThread();
}
std::string Trim(std::string s) {
  auto a=s.find_first_not_of(" \t\r\n"), b=s.find_last_not_of(" \t\r\n");
  return a==std::string::npos?std::string():s.substr(a,b-a+1);
}
void UpdateCoreAspect() {
  options["swanstation_Display_AspectRatio"]=display_options.aspect==1?"PAR 1:1":display_options.aspect==2?"16:9":display_options.aspect==3?"Custom":"4:3";
  // The core's widescreen geometry correction uses its own aspect setting,
  // not only the rectangle used by our Android presenter.
  int numerator=4,denominator=3;float error=100.f;
  for(int d=1;d<=50;d++)for(int n=1;n<=50;n++){
    const float candidate=std::abs(float(n)/d-display_options.custom);
    if(candidate<error){error=candidate;numerator=n;denominator=d;}
  }
  options["swanstation_Display_CustomAspectRatioNumerator"]=std::to_string(numerator);
  options["swanstation_Display_CustomAspectRatioDenominator"]=std::to_string(denominator);
  options_changed=true;
}
void ReadConfig() {
  std::ifstream stream(config_path); std::string line,section;
  std::map<std::string,std::string> values;
  while(std::getline(stream,line)) {
    line=Trim(line); if(line.empty() || line[0]=='#') continue;
    if(line[0]=='[' && line.back()==']') {section=line.substr(1,line.size()-2);continue;}
    auto equal=line.find('=');if(equal==std::string::npos)continue;
    auto v=Trim(line.substr(equal+1));if(v.size()>1 && v.front()=='"' && v.back()=='"')v=v.substr(1,v.size()-2);
    values[section+"."+Trim(line.substr(0,equal))]=v;
  }
  const auto put=[&](const char* input,const char* output,const char* fallback) {
    auto it=values.find(input);options[output]=it==values.end()?fallback:it->second;
  };
  config_values=values;
  const bool software=values["video.renderer"]=="software";
  requested_vulkan=!software && values["video.gpu_backend"]=="vulkan";
  options["swanstation_GPU_Renderer"]=software?"Software":requested_vulkan?"Vulkan":"OpenGL";
  options["swanstation_CPU_ExecutionMode"]=values["cpu.execution_mode"]=="interpreter"?"Interpreter":"Recompiler";
  put("video.internal_scale","swanstation_GPU_ResolutionScale","3");
  put("cpu.fast_boot","swanstation_BIOS_PatchFastBoot","true");
  put("video.pgxp","swanstation_GPU_PGXPEnable","false");
  options["swanstation_Console_Region"]=values["console.region"]=="pal"?"PAL":values["console.region"]=="ntsc"?"NTSC-U":"Auto";
  // Keep existing per-title cards in place. The manager resolves their real core paths.
  options["swanstation_MemoryCards_Card1Type"]="PerGameTitle";
  options["swanstation_MemoryCards_Card2Type"]="Shared";
  multiplayer_mode=SwanMultiplayer::FromPorts(values["input.multitap"]=="true",values["input.multitap_port2"]=="true");
  options["swanstation_ControllerPorts_MultitapMode"]=SwanMultiplayer::CoreMode(multiplayer_mode);
  controller_devices={SwanControllers::Parse(values["input.controller1_type"]),
                      SwanControllers::Parse(values["input.controller2_type"])};
  put("emulation.runahead","swanstation_Main_RunaheadFrameCount","0");
  options["swanstation_GPU_TextureFilter"]=values["video.texture_filter"]=="bilinear"?"Bilinear":values["video.texture_filter"]=="xbr"?"xBR":"Nearest";
  put("video.widescreen_hack","swanstation_GPU_WidescreenHack","false");
  const auto aspect=values["video.display_aspect"];
  display_options.aspect=aspect=="square"?1:aspect=="wide16x9"?2:aspect=="custom"?3:0;
  try{const float ratio=std::stof(values["video.display_aspect_custom"]);if(std::isfinite(ratio))display_options.custom=std::clamp(ratio,0.25f,4.f);}catch(...){}
  display_options.stretch=values["video.stretch_mode"]=="true";
  display_options.integer=values["video.integer_scaling"]=="true";
  try{display_options.rotation=SwanPresentation::Rotation(std::stoi(values["video.display_rotation"]));}catch(...){display_options.rotation=0;}
  options["swanstation_GPU_DisableInterlacing"]=values["video.deinterlace"]=="progressive"?"true":"false";
  options["swanstation_TextureReplacements_EnableVRAMWriteReplacements"]=(values["video.texture_dump"]=="true"||values["video.texture_replacements"]=="true")?"true":"false";
  UpdateCoreAspect();
  options["swanstation_Display_CropMode"]=values["video.overscan_crop"]=="small"?"Overscan":values["video.overscan_crop"]=="full"?"Borders":"None";
  options["swanstation_GPU_DownsampleMode"]=values["video.downsample"]=="true"?"Box":"Disabled";
  const auto bios=values["bios.override_file"];
  if(!bios.empty())for(const auto* key:{"swanstation_BIOS_PathNTSCU","swanstation_BIOS_PathNTSCJ","swanstation_BIOS_PathPAL"})
    options[key]=std::filesystem::path(bios).is_absolute()?std::filesystem::path(bios).lexically_relative(bios_root).string():bios;
  options["swanstation_Logging_LogLevel"]="Info";
}
std::string OptionValue(const char* key) {auto it=options.find(key);return it==options.end()?std::string():it->second;}
uintptr_t Framebuffer() {return framebuffer;}
retro_proc_address_t Procedure(const char* name) {return reinterpret_cast<retro_proc_address_t>(eglGetProcAddress(name));}
bool Environment(unsigned command,void* data) {
  switch(command) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY: *static_cast<const char**>(data)=bios_root.c_str();return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: *static_cast<const char**>(data)=save_root.c_str();return true;
    case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY: *static_cast<const char**>(data)=data_root.c_str();return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: static_cast<retro_log_callback*>(data)->log=CoreLog;return true;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION: *static_cast<unsigned*>(data)=2;return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2: {
      const auto* definitions=static_cast<retro_core_options_v2*>(data)->definitions;
      for(auto* d=definitions;d && d->key;d++) if(!options.count(d->key)) options[d->key]=d->default_value?d->default_value:d->values[0].value;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
      auto* v=static_cast<retro_core_options_v2_intl*>(data);return Environment(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2,v->us);
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS: {
      for(auto* d=static_cast<retro_core_option_definition*>(data);d && d->key;d++)
        if(!options.count(d->key))options[d->key]=d->default_value?d->default_value:d->values[0].value;
      return true;
    }
    case RETRO_ENVIRONMENT_SET_VARIABLES: {
      for(auto* v=static_cast<retro_variable*>(data);v && v->key;v++) if(!options.count(v->key)) {
        auto value=std::string(v->value?v->value:"");auto semi=value.find(';');
        if(semi!=std::string::npos)value=Trim(value.substr(semi+1));options[v->key]=value.substr(0,value.find('|'));
      }
      return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
      auto* v=static_cast<retro_variable*>(data);auto it=options.find(v->key);v->value=it==options.end()?nullptr:it->second.c_str();return v->value!=nullptr;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: *static_cast<bool*>(data)=options_changed.exchange(false);return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: pixels=*static_cast<retro_pixel_format*>(data);return pixels<=RETRO_PIXEL_FORMAT_RGB565;
    case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER: *static_cast<unsigned*>(data)=requested_vulkan?RETRO_HW_CONTEXT_VULKAN:RETRO_HW_CONTEXT_OPENGLES_VERSION;return true;
    case RETRO_ENVIRONMENT_SET_HW_RENDER: {
      auto* cb=static_cast<retro_hw_render_callback*>(data);
      if(cb->context_type==RETRO_HW_CONTEXT_VULKAN && requested_vulkan){hardware=*cb;return true;}
      if(cb->context_type!=RETRO_HW_CONTEXT_OPENGLES3 && cb->context_type!=RETRO_HW_CONTEXT_OPENGLES_VERSION)return false;
      if(cb->version_major>3 || (cb->version_major==3 && cb->version_minor>0))return false;
      cb->get_current_framebuffer=Framebuffer;cb->get_proc_address=Procedure;hardware=*cb;return true;
    }
    case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
      ANativeWindow* target=nullptr;{std::lock_guard lock(state_mutex);target=window;if(target)ANativeWindow_acquire(target);}
      const bool ready=SwanVulkan::Negotiate(static_cast<retro_hw_render_context_negotiation_interface_vulkan*>(data),target);
      if(target)ANativeWindow_release(target);
      if(!ready){JavaNotice("Vulkan initialization failed. Using software for this boot.");hardware={};requested_vulkan=false;
        std::lock_guard lock(state_mutex);window_revision++;}
      return ready;
    }
    case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
      auto* value=SwanVulkan::Interface();if(!value)return false;*static_cast<const retro_hw_render_interface_vulkan**>(data)=value;return true;
    }
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO: av=*static_cast<retro_system_av_info*>(data);nominal=av.timing.fps;return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY: av.geometry=*static_cast<retro_game_geometry*>(data);return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE: disk=*static_cast<retro_disk_control_ext_callback*>(data);return true;
    case RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION: *static_cast<unsigned*>(data)=1;return true;
    case RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE: {
      auto* d=static_cast<retro_disk_control_callback*>(data);disk.set_eject_state=d->set_eject_state;disk.get_eject_state=d->get_eject_state;
      disk.get_image_index=d->get_image_index;disk.set_image_index=d->set_image_index;disk.get_num_images=d->get_num_images;
      disk.replace_image_index=d->replace_image_index;disk.add_image_index=d->add_image_index;return true;
    }
    case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION: *static_cast<unsigned*>(data)=1;return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: CoreLog(RETRO_LOG_INFO,"%s",static_cast<retro_message*>(data)->msg);return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
      auto* m=static_cast<retro_message_ext*>(data);CoreLog(m->level,"%s",m->msg);
      if(m->level==RETRO_LOG_ERROR)JavaNotice(m->msg);return true;
    }
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE: *static_cast<int*>(data)=3;return true;
    case RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT: *static_cast<retro_savestate_context*>(data)=state_context;return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING: *static_cast<bool*>(data)=fast;return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE: *static_cast<bool*>(data)=true;return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE: *static_cast<unsigned*>(data)=RETRO_LANGUAGE_ENGLISH;return true;
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
    case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS: return true;
    case RETRO_ENVIRONMENT_SHUTDOWN:stop=true;return true;
    default:return false;
  }
}
void InputPoll() {}
int16_t Input(unsigned port,unsigned device,unsigned index,unsigned id) {
  const int player=SwanMultiplayer::PlayerForPort(multiplayer_mode,port);
  if(player<0)return 0;
  port=static_cast<unsigned>(player);
  if(port>=buttons.size())return 0;
  return SwanControllers::Read(device,index,id,buttons[port],axes[port]);
}
aaudio_data_callback_result_t AudioOutput(AAudioStream*,void*,void* output,int32_t count) {
  if(count<=0)return AAUDIO_CALLBACK_RESULT_CONTINUE;
  auto* out=static_cast<int16_t*>(output);std::lock_guard lock(audio_mutex);
  const bool muted=audio_suspended||paused;
  if(muted) {
    audio_queue.Clear();audio_warmup.Reset();
    std::fill_n(out,size_t(count)*2,int16_t(0));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  if(!audio_warmup.Ready(audio_queue.Size(),false)) {
    // Startup/resume silence is intentional: retain queued PCM until there is enough for
    // the next bursts. Previously the callback drained partial frame batches immediately.
    std::fill_n(out,size_t(count)*2,int16_t(0));
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
  }
  audio_queue.Read(out,size_t(count));
  for(int i=0;i<count*2;i++)if(out[i]!=0)audio_nonzero++;
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
size_t AudioBatch(const int16_t* data,size_t count) {
  std::lock_guard lock(audio_mutex);
  if(audio && !paused && !audio_suspended)audio_queue.Push(data,count);
  return count;
}
void AudioSample(int16_t left,int16_t right) {const int16_t s[]={left,right};AudioBatch(s,1);}
int ConfigInt(const char* key,int fallback,int minimum,int maximum);
void OpenAudio() {
  AAudioStreamBuilder* builder=nullptr;if(AAudio_createStreamBuilder(&builder)!=AAUDIO_OK)return;
  AAudioStreamBuilder_setDirection(builder,AAUDIO_DIRECTION_OUTPUT);
  AAudioStreamBuilder_setFormat(builder,AAUDIO_FORMAT_PCM_I16);AAudioStreamBuilder_setChannelCount(builder,2);
  AAudioStreamBuilder_setSampleRate(builder,static_cast<int>(av.timing.sample_rate));
  AAudioStreamBuilder_setPerformanceMode(builder,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
  AAudioStreamBuilder_setDataCallback(builder,AudioOutput,nullptr);
  if(AAudioStreamBuilder_openStream(builder,&audio)!=AAUDIO_OK)JavaNotice("Audio output could not be opened");
  AAudioStreamBuilder_delete(builder);
  {std::lock_guard lock(audio_mutex);
    audio_warmup.Configure(static_cast<unsigned>(av.timing.sample_rate),ConfigInt("audio.buffer_ms",20,20,100),
      av.timing.fps,audio?static_cast<unsigned>(std::max(0,AAudioStream_getFramesPerBurst(audio))):0);
    audio_queue.Configure(audio_warmup.Target());}
  if(audio)AAudioStream_requestStart(audio);
}
GLuint Compile(GLenum type,const char* text) {
  GLuint s=glCreateShader(type);glShaderSource(s,1,&text,nullptr);glCompileShader(s);
  GLint okay=0;glGetShaderiv(s,GL_COMPILE_STATUS,&okay);if(!okay){glDeleteShader(s);return 0;}return s;
}
void CloseGraphics();
bool CreateGraphics(bool angle) {
  if(!SwanGL::Load(angle))return false;
  display=SwanGL::Display();if(display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr))return false;
  EGLint attributes[]={EGL_SURFACE_TYPE,EGL_WINDOW_BIT|EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES3_BIT,
    EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
  EGLint count=0;if(!eglChooseConfig(display,attributes,&egl_config,1,&count)||!count)return false;
  EGLint ca[]={EGL_CONTEXT_CLIENT_VERSION,3,EGL_NONE};context=eglCreateContext(display,egl_config,EGL_NO_CONTEXT,ca);
  EGLint pa[]={EGL_WIDTH,1,EGL_HEIGHT,1,EGL_NONE};pbuffer=eglCreatePbufferSurface(display,egl_config,pa);
  if(context==EGL_NO_CONTEXT || pbuffer==EGL_NO_SURFACE || !eglMakeCurrent(display,pbuffer,pbuffer,context))return false;
  GLuint vs=Compile(GL_VERTEX_SHADER,SwanGLPresentation::VertexShader), fs=Compile(GL_FRAGMENT_SHADER,SwanGLPresentation::FragmentShader);if(!vs||!fs)return false;
  shader=glCreateProgram();glAttachShader(shader,vs);glAttachShader(shader,fs);glLinkProgram(shader);glDeleteShader(vs);glDeleteShader(fs);
  GLint okay=0;glGetProgramiv(shader,GL_LINK_STATUS,&okay);if(!okay)return false;
  glGenVertexArrays(1,&vao);glGenTextures(1,&upload_texture);return true;
}
bool OpenGraphics() {
  const bool angle=requested_gl_driver=="angle" || config_values["video.gpu_backend"]=="angle" || config_values["video.gl_driver"]=="angle";
  if(CreateGraphics(angle))return true;
  CloseGraphics();
  if(!angle)return false;
  JavaNotice("ANGLE could not initialize on this device; using system OpenGL ES for this boot.");
  return CreateGraphics(false);
}
void AllocateTarget() {
  if(SwanVulkan::Ready())return;
  const unsigned w=std::max(av.geometry.max_width,av.geometry.base_width),h=std::max(av.geometry.max_height,av.geometry.base_height);
  if(!w || !h || (w==target_width && h==target_height))return;
  if(framebuffer)glDeleteFramebuffers(1,&framebuffer);if(frame_texture)glDeleteTextures(1,&frame_texture);if(depth_buffer)glDeleteRenderbuffers(1,&depth_buffer);
  glGenTextures(1,&frame_texture);glBindTexture(GL_TEXTURE_2D,frame_texture);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  glGenFramebuffers(1,&framebuffer);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,frame_texture,0);
  if(hardware.depth || hardware.stencil) {
    glGenRenderbuffers(1,&depth_buffer);glBindRenderbuffer(GL_RENDERBUFFER,depth_buffer);glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH24_STENCIL8,w,h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_STENCIL_ATTACHMENT,GL_RENDERBUFFER,depth_buffer);
  }
  target_width=w;target_height=h;
  // New textures have undefined contents; never expose a previous session's pixels.
  glDisable(GL_SCISSOR_TEST);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
}
void PresentBlank() {
  if(SwanVulkan::Ready()){SwanVulkan::Blank();return;}
  if(window_surface==EGL_NO_SURFACE)return;
  glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
  glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
  eglSwapBuffers(display,window_surface);
  glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
}
void Video(const void* data,unsigned width,unsigned height,size_t pitch) {
  // Context destruction can request a final render. Do not republish the old game
  // after shutdown has blanked the persistent Android Surface.
  if(!data || stop)return;
  const auto present_start=Clock::now();
  video_width=width;video_height=height;
  if(SwanVulkan::Ready() && data==RETRO_HW_FRAME_BUFFER_VALID){
    const float aspect=SwanPresentation::Aspect(display_options,av.geometry.aspect_ratio);
    SwanVulkan::Presentation(display_options);
    if(!SwanVulkan::Present(width,height,aspect,presentation_suspended)){JavaNotice("Vulkan presentation failed; closing this session safely.");stop=true;}
    frames++;present_ms+=std::chrono::duration<double,std::milli>(Clock::now()-present_start).count();return;
  }
  software_video=data!=RETRO_HW_FRAME_BUFFER_VALID;
  GLuint texture=frame_texture;
  if(data!=RETRO_HW_FRAME_BUFFER_VALID) {
    software_pixels.resize(static_cast<size_t>(width)*height);
    auto& rgba=software_pixels;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
      auto* row=static_cast<const uint8_t*>(data)+y*pitch;uint32_t r,g,b;
      if(pixels==RETRO_PIXEL_FORMAT_XRGB8888){auto v=reinterpret_cast<const uint32_t*>(row)[x];r=(v>>16)&255;g=(v>>8)&255;b=v&255;}
      else {auto v=reinterpret_cast<const uint16_t*>(row)[x];r=((v>>(pixels==RETRO_PIXEL_FORMAT_RGB565?11:10))&31)*255/31;
        g=((v>>5)&(pixels==RETRO_PIXEL_FORMAT_RGB565?63:31))*255/(pixels==RETRO_PIXEL_FORMAT_RGB565?63:31);b=(v&31)*255/31;}
      rgba[y*width+x]=r|(g<<8)|(b<<16)|0xff000000u;
    }
    texture=upload_texture;glBindTexture(GL_TEXTURE_2D,texture);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,width,height,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
  }
  if(window_surface!=EGL_NO_SURFACE && !presentation_suspended) {
    EGLint ww=0,wh=0;eglQuerySurface(display,window_surface,EGL_WIDTH,&ww);eglQuerySurface(display,window_surface,EGL_HEIGHT,&wh);
    const auto filtered=SwanShaders::DrawGL(texture,framebuffer,width,height,ww,wh,software_video);
    if(filtered)texture=filtered;
    glBindFramebuffer(GL_FRAMEBUFFER,0);glDisable(GL_SCISSOR_TEST);glDisable(GL_DEPTH_TEST);glDisable(GL_BLEND);
    glDisable(GL_CULL_FACE);glDisable(GL_STENCIL_TEST);glDisable(GL_RASTERIZER_DISCARD);glBindSampler(0,0);
    glViewport(0,0,ww,wh);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
    const float aspect=SwanPresentation::Aspect(display_options,av.geometry.aspect_ratio);
    const auto rect=SwanPresentation::Fit(ww,wh,width,height,aspect,display_options);
    glViewport(rect.x,wh-rect.y-rect.height,rect.width,rect.height);
    glUseProgram(shader);glBindVertexArray(vao);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,texture);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glUniform1i(glGetUniformLocation(shader,"image"),0);glUniform1i(glGetUniformLocation(shader,"flip"),!filtered&&data!=RETRO_HW_FRAME_BUFFER_VALID);
    glUniform1i(glGetUniformLocation(shader,"rotation"),display_options.rotation);
    const auto sampling=filtered?SwanGLPresentation::SampleBounds(ww,wh,ww,wh):software_video?
      SwanGLPresentation::SampleBounds(width,height,width,height):SwanGLPresentation::SampleBounds(width,height,target_width,target_height);
    glUniform2f(glGetUniformLocation(shader,"uvScale"),sampling.scale_x,sampling.scale_y);
    glUniform2f(glGetUniformLocation(shader,"uvMin"),sampling.min_x,sampling.min_y);
    glUniform2f(glGetUniformLocation(shader,"uvMax"),sampling.max_x,sampling.max_y);
    glDrawArrays(GL_TRIANGLES,0,3);eglSwapBuffers(display,window_surface);
  }
  glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);frames++;
  present_ms+=std::chrono::duration<double,std::milli>(Clock::now()-present_start).count();
}
void UpdateWindow(unsigned& current) {
  std::lock_guard lock(state_mutex);if(current==window_revision)return;
  if(SwanVulkan::Ready()){if(!SwanVulkan::Window(window))stop=true;current=window_revision;return;}
  if(requested_vulkan){current=window_revision;return;}
  eglMakeCurrent(display,pbuffer,pbuffer,context);
  if(window_surface!=EGL_NO_SURFACE)eglDestroySurface(display,window_surface);window_surface=EGL_NO_SURFACE;
  if(window){ANativeWindow_setBuffersGeometry(window,0,0,WINDOW_FORMAT_RGBA_8888);window_surface=eglCreateWindowSurface(display,egl_config,window,nullptr);}
  if(window_surface!=EGL_NO_SURFACE){eglMakeCurrent(display,window_surface,window_surface,context);eglSwapInterval(display,0);PresentBlank();}
  current=window_revision;
}
void CloseGraphics() {
  if(display==EGL_NO_DISPLAY)return;
  if(context!=EGL_NO_CONTEXT){eglMakeCurrent(display,pbuffer,pbuffer,context);
    SwanShaders::CloseGL();
    if(framebuffer)glDeleteFramebuffers(1,&framebuffer);if(frame_texture)glDeleteTextures(1,&frame_texture);if(depth_buffer)glDeleteRenderbuffers(1,&depth_buffer);
    if(upload_texture)glDeleteTextures(1,&upload_texture);if(shader)glDeleteProgram(shader);if(vao)glDeleteVertexArrays(1,&vao);}
  eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
  if(window_surface!=EGL_NO_SURFACE)eglDestroySurface(display,window_surface);if(pbuffer!=EGL_NO_SURFACE)eglDestroySurface(display,pbuffer);
  if(context!=EGL_NO_CONTEXT)eglDestroyContext(display,context);
  // The default display is shared with Android HWUI: never eglTerminate it here.
  display=EGL_NO_DISPLAY;context=EGL_NO_CONTEXT;pbuffer=window_surface=EGL_NO_SURFACE;
  framebuffer=frame_texture=depth_buffer=upload_texture=shader=vao=0;target_width=target_height=0;
}
bool Invoke(std::function<bool()> fn) {
  bool same_thread;
  {std::lock_guard lock(state_mutex);same_thread=std::this_thread::get_id()==emulation_thread && active;}
  if(same_thread)return fn();
  auto promise=std::make_shared<std::promise<bool>>();auto result=promise->get_future();
  {std::lock_guard lock(state_mutex);if(!active || stop)return false;work.emplace_back([fn=std::move(fn),promise]{promise->set_value(fn());});}
  wake.notify_all();if(result.wait_for(std::chrono::seconds(10))!=std::future_status::ready)return false;return result.get();
}
// A disc/state mutation must not apply later after reporting a timeout. Cancel jobs
// which have not started; once mounting has started, await its real result on
// the caller's worker thread rather than claiming the original disc was kept.
bool InvokeDisc(std::function<bool()> fn){
  bool same_thread;
  {std::lock_guard lock(state_mutex);same_thread=std::this_thread::get_id()==emulation_thread&&active;}
  if(same_thread)return fn();
  struct Job {std::atomic<int> phase{0};std::promise<bool> done;};
  auto job=std::make_shared<Job>();auto result=job->done.get_future();
  {std::lock_guard lock(state_mutex);if(!active||stop)return false;
    const auto session=live_run_token;
    work.emplace_back([job,session,fn=std::move(fn)]{
      int pending=0;if(!job->phase.compare_exchange_strong(pending,1)){job->done.set_value(false);return;}
      if(!active||stop||live_run_token!=session){job->done.set_value(false);return;}
      try{job->done.set_value(fn());}catch(...){job->done.set_value(false);}
    });}
  wake.notify_all();
  if(result.wait_for(std::chrono::seconds(10))!=std::future_status::ready){
    int pending=0;if(job->phase.compare_exchange_strong(pending,2))return false;
  }
  return result.get();
}
void Drain() {std::deque<std::function<void()>> jobs;{std::lock_guard lock(state_mutex);jobs.swap(work);}for(auto& job:jobs)job();}
std::vector<unsigned char> ReadBytes(const std::string& path) {
  std::ifstream stream(path,std::ios::binary|std::ios::ate);if(!stream)return {};auto size=stream.tellg();if(size<0 || size>64*1024*1024)return {};
  std::vector<unsigned char> data(static_cast<size_t>(size));stream.seekg(0);if(!stream.read(reinterpret_cast<char*>(data.data()),data.size()))return {};return data;
}
int ConfigInt(const char* key,int fallback,int minimum,int maximum) {
  try { return std::clamp(std::stoi(config_values[key]),minimum,maximum); } catch(...) { return fallback; }
}
std::vector<std::string> StringArray(const std::string& text) {
  std::vector<std::string> out;std::string value;bool quoted=false,escaped=false;
  for(char c:text) {
    if(!quoted){if(c=='"'){quoted=true;value.clear();}continue;}
    if(escaped){value+=c;escaped=false;}else if(c=='\\')escaped=true;
    else if(c=='"'){out.push_back(value);quoted=false;}else value+=c;
  }
  return out;
}
uint64_t Hash(const std::string& text) {uint64_t h=14695981039346656037ull;for(unsigned char c:text)h=(h^c)*1099511628211ull;return h;}
void ClearRewind() {rewind_states.clear();rewind_bytes=0;rewind_tick=0;}
uint64_t rewind_card_epoch=0;
bool StepRewind() {
  if(!rewind_enabled || rewind_states.empty())return false;
  if(bridge_card_digest()!=rewind_card_epoch){ClearRewind();return false;}
  auto bytes=std::move(rewind_states.back());rewind_states.pop_back();rewind_bytes-=bytes.size();
  bridge_ctr_hud(false);
  state_context=RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE;
  const bool ok=core_retro_unserialize(bytes.data(),bytes.size());
  state_context=RETRO_SAVESTATE_CONTEXT_NORMAL;
  {std::lock_guard lock(audio_mutex);audio_queue.Clear();audio_warmup.Reset();}
  return ok;
}
void CaptureRewind() {
  if(!rewind_enabled)return;
  const auto interval=std::max(1,static_cast<int>(nominal.load()/rewind_frequency));
  if(++rewind_tick<interval)return;rewind_tick=0;
  bridge_ctr_hud(false); // Rewind/state files must never contain our temporary drawing patch.
  const auto size=core_retro_serialize_size();if(!size || size>64*1024*1024)return;
  const auto epoch=bridge_card_digest();
  if(epoch!=rewind_card_epoch){ClearRewind();rewind_card_epoch=epoch;}
  const auto capacity=std::max<size_t>(1,std::min<size_t>(rewind_seconds*rewind_frequency,128*1024*1024/size));
  while(rewind_states.size()>=capacity){rewind_bytes-=rewind_states.front().size();rewind_states.pop_front();}
  std::vector<unsigned char> bytes(size);if(!core_retro_serialize(bytes.data(),size))return;
  rewind_snapshot_bytes=size;rewind_bytes+=size;rewind_states.push_back(std::move(bytes));
}
int ApplyCheats(bool master,const std::vector<std::string>& names) {
  core_retro_cheat_reset();cheats_armed=0;if(!master)return 0;
  unsigned index=0;
  for(const auto& group:cheat_groups) {
    if(!group.lines || group.unsupported || std::find(names.begin(),names.end(),group.name)==names.end())continue;
    if(!bridge_validate_cheat(group.code.c_str()))continue;
    core_retro_cheat_set(index++,true,group.code.c_str());
  }
  cheats_armed=index;ClearRewind();return index;
}
void PublishStatistics(double frame,double emu,double present,double idle,double worst) {
  std::vector<double> out(SwanTelemetry::Count,-1);
  bridge_statistics_read(out.data(),out.size());
  out[0]=frame;out[1]=std::max(0.0,emu-present);out[2]=present;out[3]=idle;out[4]=worst;
  out[31]=video_width;out[32]=video_height;
  out[26]=sysconf(_SC_NPROCESSORS_ONLN);
  const double cpu=SwanPerformance::CpuPercent();
  // The process can use several cores. Match the OSD's whole-device 0..100 scale.
  out[25]=cpu>=0&&out[26]>0?std::clamp(cpu/out[26],0.0,100.0):-1;
  std::ifstream usage("/proc/self/statm");unsigned long pages=0,resident=0;
  if(usage>>pages>>resident)out[27]=double(resident)*sysconf(_SC_PAGESIZE)/(1024.0*1024.0);
  struct sysinfo info{};if(sysinfo(&info)==0)out[28]=double(info.freeram)*info.mem_unit/(1024.0*1024.0);
  std::lock_guard lock(statistics_mutex);statistics=std::move(out);
}
}
bool SwanHost::Initialize(JNIEnv* env,jclass cls,std::string root,std::string bios,std::string config) {
  std::unique_lock session_lock(session_mutex,std::try_to_lock);
  if(!session_lock.owns_lock())return false;
  data_root=std::move(root);bios_root=std::move(bios);config_path=std::move(config);save_root=data_root+"/swanstation/cards";
  SwanShaders::Initialize(data_root);
  std::error_code error;std::filesystem::create_directories(save_root,error);if(error)return false;
  std::filesystem::create_directories(data_root+"/swanstation/states",error);if(error)return false;
  for(int slot=0;slot<2;slot++) {
    std::ifstream remembered(save_root+"/.last-card-"+std::to_string(slot+1));std::string path;std::getline(remembered,path);
    // Never redirect card-manager mutations out of our own save namespace.
    const auto relative=std::filesystem::path(path).lexically_relative(save_root);
    last_card_paths[slot]=!path.empty() && !relative.empty() && *relative.begin()!=".."?path:std::string();
  }
  env->GetJavaVM(&java_vm);if(java_class)env->DeleteGlobalRef(java_class);java_class=static_cast<jclass>(env->NewGlobalRef(cls));
  notice_method=env->GetStaticMethodID(cls,"backendNotice","(Ljava/lang/String;)V");pause_method=env->GetStaticMethodID(cls,"vmSetPaused","(Z)V");
  if(!library)library=dlopen("libswanstation.so",RTLD_NOW|RTLD_LOCAL);
  if(!library){JavaNotice(dlerror());return false;}
#define LINK(name) name=reinterpret_cast<decltype(name)>(dlsym(library,&(#name)[5]));if(!name){JavaNotice("Missing core entry point: " #name);return false;}
  LINK(core_retro_init);LINK(core_retro_deinit);LINK(core_retro_run);LINK(core_retro_reset);LINK(core_retro_load_game);LINK(core_retro_unload_game);LINK(core_retro_get_system_av_info);
  LINK(core_retro_set_environment);LINK(core_retro_set_video_refresh);LINK(core_retro_set_audio_sample);LINK(core_retro_set_audio_sample_batch);LINK(core_retro_set_input_poll);LINK(core_retro_set_input_state);
  LINK(core_retro_set_controller_port_device);LINK(core_retro_serialize_size);LINK(core_retro_serialize);LINK(core_retro_unserialize);
  LINK(core_retro_cheat_reset);LINK(core_retro_cheat_set);
  LINK(core_retro_get_memory_data);LINK(core_retro_get_memory_size);
#undef LINK
#define BRIDGE(name,symbol) name=reinterpret_cast<decltype(name)>(dlsym(library,symbol));if(!name){JavaNotice("Missing SwanStation extension: " symbol);return false;}
  BRIDGE(bridge_disc_serial,"swan_disc_serial");BRIDGE(bridge_card_digest,"swan_card_digest");
  BRIDGE(bridge_disc_serial_at,"swan_disc_serial_at");BRIDGE(bridge_mounted_subimage,"swan_mounted_subimage");
  BRIDGE(bridge_card_path,"swan_card_path");BRIDGE(bridge_validate_cheat,"swan_validate_cheat");
  BRIDGE(bridge_guest_frame,"swan_guest_frame");
  BRIDGE(bridge_texture_options,"swan_texture_options");
  BRIDGE(bridge_texture_metric,"swan_texture_metric");
  BRIDGE(bridge_companion_vram,"swan_companion_vram");
  BRIDGE(bridge_ctr_hud,"swan_ctr_hud");BRIDGE(bridge_ctr_hud_status,"swan_ctr_hud_status");
  BRIDGE(bridge_statistics_reset,"swan_statistics_reset");BRIDGE(bridge_statistics_begin,"swan_statistics_begin");
  BRIDGE(bridge_statistics_end,"swan_statistics_end");BRIDGE(bridge_statistics_read,"swan_statistics_read");
#undef BRIDGE
  SwanAchievements::Initialize(env,[](uint32_t address,uint8_t* buffer,uint32_t count)->uint32_t {
    if(std::this_thread::get_id()!=emulation_thread||!active||!core_retro_get_memory_data||!buffer)return 0;
    auto* ram=static_cast<const uint8_t*>(core_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    const size_t length=core_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    if(!ram||address>length||count>length-address)return 0;std::memcpy(buffer,ram+address,count);return count;
  });
  return true;
}
uint64_t SwanHost::PrepareRun() {
  std::unique_lock session_lock(session_mutex,std::try_to_lock);
  if(!session_lock.owns_lock() || !library)return 0;
  std::lock_guard lock(state_mutex);
  if(reserved_run_token || live_run_token)return 0;
  reserved_run_token=next_run_token++;cancelled_run_token=0;
  return reserved_run_token;
}
void SwanHost::ReleaseRun(uint64_t token) {
  std::lock_guard lock(state_mutex);
  if(reserved_run_token==token && live_run_token!=token){reserved_run_token=0;cancelled_run_token=0;}
}
void SwanHost::StopRun(uint64_t token) {
  std::lock_guard lock(state_mutex);
  if(!token || reserved_run_token!=token)return;
  cancelled_run_token=token;
  if(live_run_token==token)stop=true;
  wake.notify_all();
}
bool SwanHost::Run(std::string path,uint64_t token) {
  std::unique_lock session_lock(session_mutex,std::try_to_lock);if(!session_lock.owns_lock() || !library)return false;
  {
    std::lock_guard lock(state_mutex);
    if(!token || reserved_run_token!=token || cancelled_run_token==token)return false;
    live_run_token=token;stop=false;emulation_thread=std::this_thread::get_id();game_path=std::move(path);
  }
  const auto epoch_ms=std::chrono::duration_cast<std::chrono::milliseconds>(
    std::chrono::system_clock::now().time_since_epoch()).count();
  std::string original_source;
  {std::lock_guard lock(state_mutex);original_source=requested_source.empty()?game_path:requested_source;}
  diagnostic_session.Begin(data_root+"/logs",std::to_string(getpid())+"-"+std::to_string(epoch_ms)+"-"+std::to_string(token),
    ARMSX_SOURCE_COMMIT,original_source);
  // Reset session bookkeeping even when EGL or BIOS loading fails.
  struct FinishRun { ~FinishRun(){diagnostic_session.State("ended");std::lock_guard lock(state_mutex);active=false;live_run_token=0;game_path.clear();current_serial.clear();emulation_thread={};} } finish;
  paused=false;audio_suspended=false;frames=0;fps=0;hardware={};disk={};av={};options.clear();
  video_width=video_height=0;software_pixels.clear();ResetButtons();audio_nonzero=0;
  ClearRewind();rewind_active=false;rewind_snapshot_bytes=0;cheat_groups.clear();cheats_armed=0;
  {std::lock_guard lock(statistics_mutex);statistics.clear();}
  ReadConfig();
  diagnostic_session.State("booting","requested_renderer="+SwanDiagnostics::Quote(OptionValue("swanstation_GPU_Renderer"))+
    " requested_scale="+OptionValue("swanstation_GPU_ResolutionScale")+
    " cpu_option="+SwanDiagnostics::Quote(OptionValue("swanstation_CPU_ExecutionMode")));
  if(!OpenGraphics()){JavaNotice("Cannot create an OpenGL ES 3 graphics context");CloseGraphics();return false;}
  // Clear the retained Surface before loading the BIOS/disc, not only on frame one.
  unsigned revision=~0u;UpdateWindow(revision);
  core_retro_set_environment(Environment);core_retro_set_video_refresh(Video);core_retro_set_audio_sample(AudioSample);core_retro_set_audio_sample_batch(AudioBatch);
  bridge_statistics_reset();
  core_retro_set_input_poll(InputPoll);core_retro_set_input_state(Input);
  // Set devices before Initialize loads controller settings, not after the BIOS is booted.
  for(unsigned port=0;port<buttons.size();++port)
    core_retro_set_controller_port_device(port,SwanControllers::ForPort(multiplayer_mode,port,controller_devices));
  core_retro_init();
  retro_game_info game{game_path.empty()?nullptr:game_path.c_str(),nullptr,0,nullptr};
  const bool loaded=core_retro_load_game(game_path.empty()?nullptr:&game);
  if(loaded) {
    char texture_serial[32]{};bridge_disc_serial(game_path.c_str(),texture_serial,sizeof(texture_serial));
    {std::lock_guard lock(state_mutex);current_serial=texture_serial;}
    auto texture_dir=config_values["video.texture_dir"];if(texture_dir.empty())texture_dir=data_root+"/textures/"+(texture_serial[0]?std::string(texture_serial):"_common");
    bridge_texture_options(config_values["video.texture_dump"]=="true",config_values["video.texture_replacements"]=="true",texture_dir.c_str());
    SwanPerformance::Start();
    core_retro_get_system_av_info(&av);nominal=av.timing.fps;AllocateTarget();
    if(hardware.context_reset)hardware.context_reset();
    {std::lock_guard lock(state_mutex);active_renderer=SwanVulkan::Ready()?"SwanStation Vulkan (hardware)":hardware.context_reset?
      (SwanGL::IsAngle()?"SwanStation ANGLE OpenGL ES / Vulkan (hardware)":"SwanStation OpenGL ES (hardware)"):
      (SwanGL::IsAngle()?"SwanStation Software (ANGLE presenter)":"SwanStation Software (OpenGL ES presenter)");active=true;}
    const auto* presenter_vendor=reinterpret_cast<const char*>(glGetString(GL_VENDOR));
    const auto* presenter_renderer=reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    diagnostic_session.State("running","renderer="+SwanDiagnostics::Quote(SwanHost::Renderer())+
      " serial="+SwanDiagnostics::Quote(texture_serial)+" nominal_hz="+std::to_string(nominal.load())+
      " presenter_gl_vendor="+SwanDiagnostics::Quote(presenter_vendor?presenter_vendor:"unavailable")+
      " presenter_gl_renderer="+SwanDiagnostics::Quote(presenter_renderer?presenter_renderer:"unavailable"));
    {
      char serial[32]{};bridge_disc_serial(game_path.c_str(),serial,sizeof(serial));
      std::lock_guard lock(state_mutex);
      state_source=requested_source.empty()?game_path:requested_source;
      auto name=requested_identity.empty()?(serial[0]?std::string(serial):std::filesystem::path(game_path).stem().string()):requested_identity;
      for(char& c:name)if(!std::isalnum(static_cast<unsigned char>(c)) && c!='-' && c!='_')c='_';
      std::ostringstream key;key<<name.substr(0,100)<<"-"<<std::hex<<Hash(state_source);
      state_identity=key.str();requested_source.clear();requested_identity.clear();
    }
    rewind_enabled=config_values["emulation.rewind"]=="true";
    for(unsigned slot=0;slot<2;slot++) {char path[4096]{};if(bridge_card_path(slot,path,sizeof(path))) {
      {std::lock_guard lock(state_mutex);last_card_paths[slot]=path;}
      std::ofstream remembered(save_root+"/.last-card-"+std::to_string(slot+1),std::ios::trunc);remembered<<path<<'\n';
    }}
    rewind_seconds=ConfigInt("emulation.rewind_seconds",10,1,60);rewind_frequency=ConfigInt("emulation.rewind_frequency",2,1,8);
    SwanCatalogue::Read(config_values["cheats.file"],cheat_groups);
    ApplyCheats(config_values["cheats.enabled"]=="true",StringArray(config_values["cheats.enabled_codes"]));
    SwanAchievements::BeginSession();
    if(cheats_armed==0)SwanAchievements::Game(game_path);else SwanAchievements::Unload();
    OpenAudio();NotifyPause(false);
    int measured_frames=0;auto measured_at=Clock::now(),deadline=Clock::now();
    bool ctr_hud_used=false;
    ctr_hud_lease=0;
    double sum_frame=0,sum_emu=0,sum_present=0,sum_idle=0,worst=0;int stat_frames=0;
    double diagnostic_window_ms=0;bool was_paused=false;
    auto diagnostic_frame_end=Clock::now();
    while(!stop) {
      const auto loop_start=Clock::now();
      Drain();UpdateWindow(revision);AllocateTarget();
      const int64_t hud_now=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
      const bool move_hud=!paused && !rewind_active && current_serial=="SCUS-94426" && hud_now<ctr_hud_lease.load();
      if(bridge_ctr_hud(move_hud) && !ctr_hud_used){ctr_hud_used=true;SwanAchievements::Unload();}
      if(paused!=was_paused){was_paused=paused;diagnostic_window_ms=0;
        diagnostic_session.State(was_paused?"paused":"running");measured_frames=0;measured_at=Clock::now();
        sum_frame=sum_emu=sum_present=sum_idle=worst=0;stat_frames=0;
        bridge_statistics_begin(false);}
      if(paused){fps=0;std::unique_lock lock(state_mutex);wake.wait_for(lock,std::chrono::milliseconds(10));deadline=Clock::now();diagnostic_frame_end=Clock::now();continue;}
      const auto frame_start=Clock::now();present_ms=0;
      bridge_statistics_begin(statistics_enabled.load()&&!rewind_active);
      if(rewind_active){StepRewind();SwanAchievements::Reset();}else {core_retro_run();if(cheats_armed==0 && !ctr_hud_used)SwanAchievements::Frame();CaptureRewind();}
      bridge_statistics_end();
      const auto emu_end=Clock::now();
      const double emu_ms=std::chrono::duration<double,std::milli>(emu_end-frame_start).count();
      SwanPerformance::Frame(std::chrono::duration_cast<std::chrono::nanoseconds>(emu_end-frame_start).count(),int64_t(1000000000.0/std::max(1.f,nominal.load())));
      measured_frames++;auto now=Clock::now();auto elapsed=std::chrono::duration<double>(now-measured_at).count();
      if(elapsed>=0.5){fps=measured_frames/elapsed;measured_frames=0;measured_at=now;}
      const float multiplier=rewind_active?std::max(0.01f,float(rewind_frequency)/nominal.load()):(fast?fast_speed.load():speed.load()/100.f);
      if(limit_speed && multiplier>0 && nominal>0){deadline+=std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0/(nominal*multiplier)));
        if(deadline<now-std::chrono::milliseconds(100))deadline=now;std::unique_lock lock(state_mutex);wake.wait_until(lock,deadline);}
      const auto frame_end=Clock::now();
      const double loop_ms=std::chrono::duration<double,std::milli>(frame_end-diagnostic_frame_end).count();
      diagnostic_frame_end=frame_end;
      const double idle_ms=std::chrono::duration<double,std::milli>(frame_end-emu_end).count();
      // Details are built only when a timing window is due, not once per frame.
      diagnostic_window_ms+=loop_ms;
      std::string diagnostic_details;
      if(diagnostic_window_ms>=2000) {
        const auto selected=SwanShaders::Selected();
        diagnostic_details="renderer="+SwanDiagnostics::Quote(SwanHost::Renderer())+
          " serial="+SwanDiagnostics::Quote(SwanHost::GameSerial())+" nominal_hz="+std::to_string(nominal.load())+
          " speed_percent="+std::to_string(speed.load())+" limited="+std::to_string(limit_speed.load())+
          " fast_forward="+std::to_string(fast.load())+" multiplier="+std::to_string(multiplier)+
          " resolution_scale_option="+OptionValue("swanstation_GPU_ResolutionScale")+
          " cpu_option="+SwanDiagnostics::Quote(OptionValue("swanstation_CPU_ExecutionMode"))+
          " runahead="+OptionValue("swanstation_Main_RunaheadFrameCount")+
          " rewind="+std::to_string(rewind_active.load())+
          " shader_requested="+std::to_string(selected.enabled)+
          " shader_active="+std::to_string(SwanShaders::ActiveGL()||SwanVulkan::ShaderActive())+
          " video="+std::to_string(video_width)+"x"+std::to_string(video_height);
        {std::lock_guard lock(audio_mutex);
          const double ms_per_frame=1000.0/std::max(1.0,av.timing.sample_rate);
          diagnostic_details+=" audio_queue_ms="+std::to_string(audio_queue.Size()*ms_per_frame)+
            " audio_target_ms="+std::to_string(audio_queue.Target()*ms_per_frame)+
            " audio_limit_ms="+std::to_string(audio_queue.Limit()*ms_per_frame)+
            " audio_trimmed_frames="+std::to_string(audio_queue.Dropped())+
            " audio_trim_events="+std::to_string(audio_queue.Trims())+
            " audio_underruns="+std::to_string(audio_queue.Underruns());}
        diagnostic_details+=" audio_device_buffer_frames="+std::to_string(audio?AAudioStream_getBufferSizeInFrames(audio):0)+
          " audio_device_rate="+std::to_string(audio?AAudioStream_getSampleRate(audio):0);
        diagnostic_window_ms=0;
      }
      diagnostic_session.Frame(loop_ms,emu_ms,present_ms,idle_ms,
        std::chrono::duration<double,std::milli>(frame_start-loop_start).count(),
        1000.0/(std::max(1.f,nominal.load())*std::max(0.01f,multiplier)),diagnostic_details);
      if(statistics_enabled) {
        sum_frame+=loop_ms;sum_emu+=emu_ms;sum_present+=present_ms;sum_idle+=idle_ms;worst=std::max(worst,loop_ms);stat_frames++;
        if(sum_frame>=500) {PublishStatistics(sum_frame/stat_frames,sum_emu/stat_frames,sum_present/stat_frames,sum_idle/stat_frames,worst);
          sum_frame=sum_emu=sum_present=sum_idle=worst=0;stat_frames=0;}
      }else {sum_frame=sum_emu=sum_present=sum_idle=worst=0;stat_frames=0;}
    }
    Drain();
    SwanAchievements::Unload();
    SwanPerformance::Close();
    PresentBlank();
    // SwanStation's destroy callback switches its live display to software.
    // Calling it after unload dereferences an already released host display.
    bridge_ctr_hud(false);ctr_hud_lease=0;
    if(hardware.context_destroy)hardware.context_destroy();
    core_retro_unload_game();
  } else JavaNotice("SwanStation could not boot this game. Check its image and BIOS.");
  // Also blank failed boots. The Surface outlives the EGL context and the VM.
  PresentBlank();
  SwanAchievements::EndSession();
  if(audio){AAudioStream_requestStop(audio);AAudioStream_close(audio);audio=nullptr;}
  {std::lock_guard lock(audio_mutex);audio_queue.Clear();audio_warmup.Reset();}
  core_retro_deinit();bridge_statistics_reset();SwanVulkan::Close();CloseGraphics();fps=0;ClearRewind();cheat_groups.clear();cheats_armed=0;
  // Keep hasActiveVM true until teardown really finishes, so the frontend does
  // not offer a second launch while the previous session still owns the core.
  {std::lock_guard lock(state_mutex);active=false;active_renderer="Stopped";game_path.clear();emulation_thread={};work.clear();}
  return loaded;
}
void SwanHost::Stop(){std::lock_guard lock(state_mutex);cancelled_run_token=reserved_run_token;stop=true;wake.notify_all();}
void SwanHost::Pause(bool value){paused=value;
  if(value){std::lock_guard lock(audio_mutex);audio_queue.Clear();audio_warmup.Reset();}
  NotifyPause(value);wake.notify_all();}
void SwanHost::SurfaceCreated(uint64_t owner){
  std::lock_guard lock(state_mutex);
  if(owner<=window_owner)return;
  if(window)ANativeWindow_release(window);
  window=nullptr;window_owner=owner;window_revision++;wake.notify_all();
}
void SwanHost::Surface(JNIEnv* env,jobject surface,uint64_t owner){
  ANativeWindow* next=surface?ANativeWindow_fromSurface(env,surface):nullptr;
  std::lock_guard lock(state_mutex);
  if(owner<window_owner){if(next)ANativeWindow_release(next);return;}
  window_owner=owner;if(window)ANativeWindow_release(window);window=next;window_revision++;wake.notify_all();
}
void SwanHost::PresentationSuspend(bool value){presentation_suspended=value;}
bool SwanHost::Active(){return active;}
float SwanHost::FPS(){return paused?0:fps.load();}
float SwanHost::NominalFPS(){return nominal;}
int SwanHost::Frames(){return frames;}
int64_t SwanHost::AudioNonZero(){return audio_nonzero;}
void SwanHost::Button(int player,int key,int range,bool down){
  if(player<0 || player>=static_cast<int>(buttons.size()))return;int id=-1;
  switch(key){case 19:id=RETRO_DEVICE_ID_JOYPAD_UP;break;case 20:id=RETRO_DEVICE_ID_JOYPAD_DOWN;break;case 21:id=RETRO_DEVICE_ID_JOYPAD_LEFT;break;case 22:id=RETRO_DEVICE_ID_JOYPAD_RIGHT;break;
    case 96:id=RETRO_DEVICE_ID_JOYPAD_B;break;case 97:id=RETRO_DEVICE_ID_JOYPAD_A;break;case 99:id=RETRO_DEVICE_ID_JOYPAD_Y;break;case 100:id=RETRO_DEVICE_ID_JOYPAD_X;break;
    case 102:id=RETRO_DEVICE_ID_JOYPAD_L;break;case 103:id=RETRO_DEVICE_ID_JOYPAD_R;break;case 104:id=RETRO_DEVICE_ID_JOYPAD_L2;break;case 105:id=RETRO_DEVICE_ID_JOYPAD_R2;break;
    case 106:id=RETRO_DEVICE_ID_JOYPAD_L3;break;case 107:id=RETRO_DEVICE_ID_JOYPAD_R3;break;case 108:id=RETRO_DEVICE_ID_JOYPAD_START;break;case 109:id=RETRO_DEVICE_ID_JOYPAD_SELECT;break;}
  if(id>=0)buttons[player][id]=down?1:0;
  if((key>=110 && key<=113)||(key>=120 && key<=123)){int base=key>=120?2:0,offset=key%10;int16_t value=down?std::clamp(range,0,32767):0;
    axes[player][base+(offset==0||offset==2?1:0)]=(offset==0||offset==3)?-value:value;}
}
void SwanHost::Analog(int player,int stick,int x,int y){if(player<0||player>=static_cast<int>(axes.size())||stick<0||stick>1)return;
  axes[player][stick*2]=std::clamp((x-128)*256,-32768,32767);axes[player][stick*2+1]=std::clamp((y-128)*256,-32768,32767);}
void SwanHost::ResetButtons(){for(auto& pad:buttons)for(auto& b:pad)b=0;for(auto& pad:axes)for(auto& a:pad)a=0;}
void SwanHost::Multiplayer(unsigned mode){Invoke([mode]{
  const auto next=mode<=3?mode:0;
  if(multiplayer_mode!=next)ResetButtons();
  multiplayer_mode=next;options["swanstation_ControllerPorts_MultitapMode"]=SwanMultiplayer::CoreMode(next);
  for(unsigned port=0;port<buttons.size();++port)
    core_retro_set_controller_port_device(port,SwanControllers::ForPort(next,port,controller_devices));
  options_changed=true;return true;
});}
void SwanHost::ControllerTypes(unsigned first,unsigned second){Invoke([first,second]{
  const std::array<unsigned,2> next{SwanControllers::Sanitize(first),SwanControllers::Sanitize(second)};
  if(next==controller_devices)return true;
  controller_devices=next;ResetButtons();ClearRewind();
  for(unsigned port=0;port<buttons.size();++port)
    core_retro_set_controller_port_device(port,SwanControllers::ForPort(multiplayer_mode,port,controller_devices));
  return true;
});}
void SwanHost::Option(std::string key,std::string value){Invoke([key=std::move(key),value=std::move(value)]{options[key]=value;options_changed=true;return true;});}
void SwanHost::Reset(){Invoke([]{bridge_ctr_hud(false);core_retro_reset();SwanAchievements::Reset();ClearRewind();
  {std::lock_guard lock(audio_mutex);audio_queue.Clear();audio_warmup.Reset();}return true;});}
void SwanHost::AudioSuspend(bool value){audio_suspended=value;
  if(value){std::lock_guard lock(audio_mutex);audio_queue.Clear();audio_warmup.Reset();}}
void SwanHost::FastForward(bool value){fast=value;wake.notify_all();}
void SwanHost::Speed(bool limit,int percent,float ff){limit_speed=limit;speed=std::clamp(percent,10,1000);fast_speed=std::clamp(ff,0.f,16.f);wake.notify_all();}
void SwanHost::ConfigureSession(std::string identity,std::string original){std::lock_guard lock(state_mutex);if(live_run_token)return;
  requested_identity=std::move(identity);requested_source=std::move(original);}
std::string SwanHost::DiscSerial(std::string path){
  static std::mutex probe_mutex;std::lock_guard lock(probe_mutex);
  // Library scanning may happen before the first VM initialization. dlopen does not boot a VM.
  static void* probe_library=dlopen("libswanstation.so",RTLD_NOW|RTLD_LOCAL);
  if(!probe_library)return {};
  auto probe=reinterpret_cast<bool(*)(const char*,char*,size_t)>(dlsym(probe_library,"swan_disc_serial"));
  char serial[32]{};return probe && probe(path.c_str(),serial,sizeof(serial))?serial:std::string();
}
std::string SwanHost::CardPath(int slot){if(slot<1||slot>2)return {};
  auto result=std::make_shared<std::string>();
  if(Invoke([slot,result]{char path[4096]{};if(!bridge_card_path(slot-1,path,sizeof(path)))return false;*result=path;return true;}))return *result;
  std::lock_guard lock(state_mutex);return last_card_paths[slot-1].empty()?save_root+"/duckstation_shared_card_"+std::to_string(slot)+".mcd":last_card_paths[slot-1];
}
int SwanHost::CheatsLoad(std::string path){auto count=std::make_shared<int>(-1);
  if(!Invoke([path=std::move(path),count]{core_retro_cheat_reset();cheats_armed=0;ClearRewind();
    if(SwanCatalogue::Read(path,cheat_groups))*count=cheat_groups.size();return true;}))return -1;return *count;}
std::string SwanHost::CheatsList(){auto result=std::make_shared<std::string>();
  if(!Invoke([result]{for(const auto& g:cheat_groups){if(!result->empty())*result+='\x1e';
    *result+=g.name+'\x1f'+std::to_string(g.lines)+'\x1f'+(g.unsupported?"1":"0")+'\x1f'+g.description;}return true;}))return {};return *result;}
int SwanHost::CheatsApply(bool master,std::vector<std::string> names){auto count=std::make_shared<int>(-1);
  if(!Invoke([master,names=std::move(names),count]{*count=ApplyCheats(master,names);if(cheats_armed>0)SwanAchievements::Unload();return true;}))return -1;return *count;}
int SwanHost::CheatsArmed(){return cheats_armed;}
void SwanHost::Rewind(bool enabled,int seconds,int frequency){Invoke([enabled,seconds,frequency]{
  rewind_enabled=enabled;rewind_active=false;rewind_seconds=std::clamp(seconds,1,60);rewind_frequency=std::clamp(frequency,1,8);ClearRewind();return true;});}
void SwanHost::RewindActive(bool enabled){rewind_active=enabled&&rewind_enabled;wake.notify_all();}
void SwanHost::RewindStep(){Invoke([]{return StepRewind();});}
int64_t SwanHost::RewindSnapshotBytes(){return rewind_snapshot_bytes;}
int64_t SwanHost::RewindBytesHeld(){return rewind_bytes;}
void SwanHost::StatisticsEnabled(bool enabled){statistics_enabled=enabled;if(!enabled){std::lock_guard lock(statistics_mutex);statistics.clear();}}
std::vector<double> SwanHost::Statistics(){std::lock_guard lock(statistics_mutex);return statistics_enabled&&active?statistics:std::vector<double>();}
int SwanHost::GuestFrame(){auto value=std::make_shared<int>(-1);if(!Invoke([value]{*value=bridge_guest_frame();return true;}))return -1;return *value;}
int SwanHost::MemoryByte(int address){auto value=std::make_shared<int>(-1);if(!Invoke([value,address]{
  const auto size=core_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);auto* data=static_cast<unsigned char*>(core_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
  if(address<0 || static_cast<size_t>(address)>=size || !data)return false;*value=data[address];return true;}))return -1;return *value;}
std::vector<unsigned char> SwanHost::MemorySnapshot(){
  auto result=std::make_shared<std::vector<unsigned char>>();
  // Copy on the emulation thread: never expose the mutable core pointer to Java.
  if(!Invoke([result]{
    if(!core_retro_get_memory_size || !core_retro_get_memory_data)return false;
    const auto size=core_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
    const auto* data=static_cast<const unsigned char*>(core_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    if(!data || size==0 || size>8*1024*1024)return false;
    result->assign(data,data+size);return true;
  }))return {};
  return std::move(*result);
}
void SwanHost::CtrHudMoved(bool enabled){
  const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count();
  ctr_hud_lease=enabled?now+600:0;wake.notify_all();
}
std::string SwanHost::CtrHudStatus(){auto result=std::make_shared<std::string>();
  if(!Invoke([result]{*result=bridge_ctr_hud_status();return true;}))return "HUD status unavailable";
  return *result;}
std::vector<unsigned char> SwanHost::CompanionSnapshot(){
  auto result=std::make_shared<std::vector<unsigned char>>();
  if(!Invoke([result]{
    {std::lock_guard lock(state_mutex);if(current_serial!="SCUS-94426")return false;}
    constexpr size_t ram_bytes=2*1024*1024, vram_bytes=1024*512*2;
    if(!bridge_companion_vram || !core_retro_get_memory_data || !core_retro_get_memory_size ||
       core_retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM)!=ram_bytes)return false;
    const auto* ram=static_cast<const unsigned char*>(core_retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM));
    if(!ram)return false;
    result->resize(ram_bytes+vram_bytes);
    if(!bridge_companion_vram(result->data()+ram_bytes,vram_bytes))return false;
    // Copy both on this emulation-thread job, so texture descriptors and pixels agree.
    std::memcpy(result->data(),ram,ram_bytes);
    return true;
  }))return {};
  return std::move(*result);
}
std::string SwanHost::StatePath(int slot){std::lock_guard lock(state_mutex);if(slot< -1||slot>=10||state_identity.empty())return {};
  const auto suffix=slot== -1?std::string("autosave"):std::to_string(slot);
  return data_root+"/swanstation/states/"+state_identity+"-"+suffix+".swanstate";}
namespace {
constexpr const char* StateCore="dd4805619bbbfc6e0d1776372cb16377088b448e";
uint64_t Read64(const unsigned char* bytes){uint64_t result=0;for(int i=0;i<8;i++)result|=uint64_t(bytes[i])<<(8*i);return result;}
void Write64(unsigned char* bytes,uint64_t value){for(int i=0;i<8;i++)bytes[i]=(value>>(8*i))&255;}
bool AtomicBytes(const std::string& path,const unsigned char* bytes,size_t size) {
  auto* file=fopen((path+".tmp").c_str(),"wb");if(!file)return false;
  bool ok=fwrite(bytes,1,size,file)==size && fflush(file)==0 && fsync(fileno(file))==0;
  if(fclose(file)!=0)ok=false;
  if(!ok){std::remove((path+".tmp").c_str());return false;}
  std::error_code error;std::filesystem::rename(path+".tmp",path,error);return !error;
}
}
bool SwanHost::Save(int slot){auto path=StatePath(slot);if(path.empty())return false;
  return InvokeDisc([path]{bridge_ctr_hud(false);auto size=core_retro_serialize_size();if(!size || size>63*1024*1024)return false;std::vector<unsigned char> bytes(size+80,0);
    std::memcpy(bytes.data(),"SWANST02",8);Write64(bytes.data()+8,bridge_card_digest());Write64(bytes.data()+16,size);
    Write64(bytes.data()+24,Hash(state_identity));std::memcpy(bytes.data()+32,StateCore,40);
    if(!core_retro_serialize(bytes.data()+80,size))return false;
    const auto checksum=crc32(0,bytes.data()+80,size);Write64(bytes.data()+72,checksum);
    const auto metadata=state_source+"\n";
    if(!AtomicBytes(path+".game",reinterpret_cast<const unsigned char*>(metadata.data()),metadata.size()))return false;
    if(!AtomicBytes(path,bytes.data(),bytes.size()))return false;
    SwanHost::Screenshot(path+".png");return true;});}
bool SwanHost::Load(int slot){return LoadChecked(slot,true)==0;}
int SwanHost::LoadChecked(int slot,bool ignore){auto path=StatePath(slot);if(path.empty())return -1;
  auto result=std::make_shared<int>(-1);
  if(!InvokeDisc([path,ignore,result]{auto bytes=ReadBytes(path);
    if(bytes.size()<80 || std::memcmp(bytes.data(),"SWANST02",8)!=0 || std::memcmp(bytes.data()+32,StateCore,40)!=0 ||
       Read64(bytes.data()+16)!=bytes.size()-80 || Read64(bytes.data()+24)!=Hash(state_identity) ||
       Read64(bytes.data()+72)!=crc32(0,bytes.data()+80,bytes.size()-80))return true;
    if(!ignore && Read64(bytes.data()+8)!=bridge_card_digest()){*result=-16;return true;}
    state_context=RETRO_SAVESTATE_CONTEXT_NORMAL;
    bridge_ctr_hud(false);
    if(!core_retro_unserialize(bytes.data()+80,bytes.size()-80))return true;
    SwanAchievements::Reset();
    ClearRewind();{std::lock_guard lock(audio_mutex);audio_queue.Clear();audio_warmup.Reset();}*result=0;return true;
  }))return -1;return *result;}
std::string SwanHost::StateGame(int slot){auto path=StatePath(slot);if(path.empty()||!std::filesystem::is_regular_file(path))return {};
  std::lock_guard lock(state_mutex);return state_source;}
std::vector<unsigned char> SwanHost::Preview(std::string path){return ReadBytes(path+".png");}
void SwanHost::Screenshot(std::string path){Invoke([path]{
  const unsigned w=video_width,h=video_height;if(!w||!h)return false;
  if(SwanVulkan::Ready()){auto pixels=SwanVulkan::Pixels(w,h);return !pixels.empty()&&WritePng(path.c_str(),w,h,4,pixels.data(),w*4);}
  if(!framebuffer)return false;
  if(software_video)return WritePng(path.c_str(),w,h,4,software_pixels.data(),w*4);
  if(w>target_width||h>target_height)return false;
  std::vector<unsigned char> rgba(static_cast<size_t>(w)*h*4);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);glPixelStorei(GL_PACK_ALIGNMENT,1);
  glReadPixels(0,0,w,h,GL_RGBA,GL_UNSIGNED_BYTE,rgba.data());
  for(unsigned y=0;y<h/2;y++)for(unsigned x=0;x<w*4;x++)std::swap(rgba[y*w*4+x],rgba[(h-y-1)*w*4+x]);
  return WritePng(path.c_str(),w,h,4,rgba.data(),w*4);});}
std::string SwanHost::EmbeddedDiscs(uint64_t session){
  auto result=std::make_shared<std::string>();
  if(!Invoke([session,result]{
    if(!session||session!=live_run_token||stop||!active||!disk.get_num_images)return false;
    auto extension=std::filesystem::path(game_path).extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return std::tolower(c);});
    if(extension!=".pbp")return true;
    const unsigned count=disk.get_num_images();const int current=bridge_mounted_subimage();
    if(count==1)return true;
    if(count<2||count>5||current<0||unsigned(current)>=count)return false;
    *result=std::to_string(session)+"\t"+std::to_string(current);
    for(unsigned i=0;i<count;i++){
      char buffer[512]{};std::string label;
      if(disk.get_image_label&&disk.get_image_label(i,buffer,sizeof(buffer)))label=buffer;
      for(char& c:label)if(static_cast<unsigned char>(c)<32)c=' ';
      if(label.empty())label="Disc "+std::to_string(i+1);
      *result+='\n'+label;
    }
    return true;
  }))return "!";
  return *result;
}
bool SwanHost::EmbeddedDisc(int index,uint64_t session){
  return InvokeDisc([index,session]{
    if(!session||session!=live_run_token||stop||!active)return false;
    auto extension=std::filesystem::path(game_path).extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return std::tolower(c);});
    if(extension!=".pbp")return false;
    bridge_ctr_hud(false);ctr_hud_lease=0;
    const auto outcome=SwanDisc::Switch({disk.set_eject_state,disk.get_eject_state,disk.set_image_index,
      disk.get_num_images,bridge_mounted_subimage},index);
    if(outcome==SwanDisc::Result::Unchanged)return true;
    if(outcome!=SwanDisc::Result::Changed){
      JavaNotice(outcome==SwanDisc::Result::FailedUnrestored?"Disc switch failed; the original disc could not be restored.":
        "Could not switch embedded disc. The original disc remains selected.");return false;
    }
    ClearRewind();SwanAchievements::Unload();
    if(cheats_armed==0)SwanAchievements::Game(game_path,unsigned(index));
    char serial[32]{};bridge_disc_serial_at(game_path.c_str(),unsigned(index),serial,sizeof(serial));
    {std::lock_guard lock(state_mutex);current_serial=serial;}
    JavaNotice("Disc changed to Disc "+std::to_string(index+1));return true;
  });
}
bool SwanHost::Disc(std::string path){if(!std::filesystem::is_regular_file(path))return false;
  return Invoke([path]{if(!disk.set_eject_state||!disk.replace_image_index||!disk.get_image_index)return false;
    bridge_ctr_hud(false);ctr_hud_lease=0;
    const auto index=disk.get_image_index();retro_game_info next{path.c_str(),nullptr,0,nullptr};
    if(!disk.set_eject_state(true))return false;const bool replaced=disk.replace_image_index(index,&next);
    const bool closed=disk.set_eject_state(false);if(replaced&&closed){ClearRewind();SwanAchievements::Unload();if(cheats_armed==0)SwanAchievements::Game(path);
      char serial[32]{};bridge_disc_serial(path.c_str(),serial,sizeof(serial));std::lock_guard lock(state_mutex);game_path=path;current_serial=serial;}return replaced&&closed;});}
std::string SwanHost::DiagnosticsSnapshot(){return diagnostic_session.Snapshot();}
std::string SwanHost::Renderer(){std::lock_guard lock(state_mutex);return active_renderer+(active&&(SwanShaders::ActiveGL()||SwanVulkan::ShaderActive())?" + shader chain":"");}
void SwanHost::GlDriver(std::string driver){std::lock_guard lock(state_mutex);if(!live_run_token)requested_gl_driver=driver=="angle"?"angle":"system";}
bool SwanHost::CustomDriver(std::string directory,std::string name,std::string redirect,std::string hooks){std::lock_guard lock(state_mutex);
  if(live_run_token)return false;SwanVulkan::Driver(std::move(directory),std::move(name),std::move(redirect),std::move(hooks));return true;}
std::string SwanHost::GlDriver(){std::lock_guard lock(state_mutex);return requested_gl_driver=="angle"?"angle":"system";}
bool SwanHost::DriverVariables(std::string variables){std::lock_guard lock(state_mutex);
  return !live_run_token&&SwanVulkan::DriverVariables(variables);}
void SwanHost::DisplayAspect(int mode){Invoke([mode]{display_options.aspect=std::clamp(mode,0,3);UpdateCoreAspect();return true;});}
void SwanHost::DisplayCustom(float ratio){if(std::isfinite(ratio))Invoke([ratio]{display_options.custom=std::clamp(ratio,0.25f,4.f);UpdateCoreAspect();return true;});}
void SwanHost::DisplayStretch(bool value){Invoke([value]{display_options.stretch=value;return true;});}
void SwanHost::DisplayInteger(bool value){Invoke([value]{display_options.integer=value;return true;});}
void SwanHost::DisplayRotation(int degrees){Invoke([degrees]{display_options.rotation=SwanPresentation::Rotation(degrees);return true;});}
void SwanHost::Textures(bool dump,bool replace,std::string directory){Invoke([dump,replace,directory=std::move(directory)]{
  const auto dir=directory.empty()?data_root+"/textures/"+(current_serial.empty()?"_common":current_serial):directory;
  options["swanstation_TextureReplacements_EnableVRAMWriteReplacements"]=dump||replace?"true":"false";options_changed=true;
  bridge_texture_options(dump,replace,dir.c_str());return true;});}
std::string SwanHost::GameSerial(){std::lock_guard lock(state_mutex);return current_serial;}
std::vector<int64_t> SwanHost::TextureMetrics(){auto values=std::make_shared<std::vector<int64_t>>(5,0);
  if(!Invoke([values]{for(unsigned i=0;i<5;i++)(*values)[i]=bridge_texture_metric(i);return true;}))return std::vector<int64_t>(5,0);
  return *values;}
void SwanHost::PortraitTop(bool value){Invoke([value]{display_options.portrait_top=value;return true;});}
void SwanHost::PortraitInset(int pixels){Invoke([pixels]{display_options.inset=std::clamp(pixels,0,8192);return true;});}
void SwanHost::Unsupported(std::string feature){std::lock_guard lock(state_mutex);
  if(unavailable.insert(feature).second)Log(ANDROID_LOG_WARN,("Not yet connected in SwanStation adapter: "+feature).c_str());}
