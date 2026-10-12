// SPDX-License-Identifier: GPL-3.0-or-later
#include "shader_api.h"
#include <dlfcn.h>
#include <mutex>
#include <sstream>
#include <cmath>
#include <android/log.h>
#include <fstream>
#include <filesystem>
namespace {std::mutex mutex;SwanShaders::Api api;bool loaded=false;
SwanShaders::Selection selected;
std::string presentation_preset;
std::map<std::string,std::map<std::string,float>> preset_parameters;
std::string Quote(const char* text){std::string out="\"";for(unsigned char c:std::string(text?text:"")){
  if(c=='"'||c=='\\'){out+='\\';out+=c;}else if(c<32){char buffer[7];snprintf(buffer,sizeof(buffer),"\\u%04x",c);out+=buffer;}else out+=c;}return out+'"';}
}
void SwanShaders::Initialize(const std::string& root){std::lock_guard lock(mutex);
  const auto dir=root+"/swanstation/presentation";std::error_code error;std::filesystem::create_directories(dir,error);if(error)return;
  std::ofstream(dir+"/rotate.slang")<<R"(#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
layout(push_constant) uniform Parameters { mat4 MVP; } params;
#pragma stage vertex
layout(location=0) in vec4 Position;layout(location=1) in vec2 TexCoord;layout(location=0) out vec2 uv;
void main(){gl_Position=params.MVP*Position;uv=TexCoord;}
#pragma stage fragment
layout(location=0) in vec2 uv;layout(binding=2) uniform sampler2D Source;layout(location=0) out vec4 FragColor;
void main(){FragColor=texture(Source,uv);}
)";
  std::ofstream(dir+"/rotate.slangp")<<"shaders = 1\nshader0 = \"rotate.slang\"\nfilter_linear0 = false\n";
  presentation_preset=dir+"/rotate.slangp";
}
std::string SwanShaders::PresentationPreset(){std::lock_guard lock(mutex);return presentation_preset;}
SwanShaders::Api* SwanShaders::Functions(){std::lock_guard lock(mutex);if(loaded)return &api;
  static void* library=dlopen("liblibrashader_capi.so",RTLD_NOW|RTLD_LOCAL);if(!library){__android_log_write(ANDROID_LOG_WARN,"SwanShaders",dlerror());return nullptr;}
#define LOAD(field,name) api.field=reinterpret_cast<decltype(api.field)>(dlsym(library,name));if(!api.field)return nullptr;
  LOAD(abi,"libra_instance_abi_version") LOAD(version,"libra_instance_api_version")
  LOAD(preset_create,"libra_preset_create") LOAD(preset_free,"libra_preset_free")
  LOAD(preset_params,"libra_preset_get_runtime_params") LOAD(free_params,"libra_preset_free_runtime_params")
  LOAD(create,"libra_vk_filter_chain_create") LOAD(frame,"libra_vk_filter_chain_frame")
  LOAD(parameter,"libra_vk_filter_chain_set_param") LOAD(free,"libra_vk_filter_chain_free") LOAD(error_free,"libra_error_free")
  LOAD(gl_create,"libra_gl_filter_chain_create") LOAD(gl_frame,"libra_gl_filter_chain_frame")
  LOAD(gl_parameter,"libra_gl_filter_chain_set_param") LOAD(gl_free,"libra_gl_filter_chain_free")
  LOAD(error_write,"libra_error_write") LOAD(error_string_free,"libra_error_free_string")
#undef LOAD
  if(api.abi()!=2||api.version()<5)return nullptr;loaded=true;return &api;
}
bool SwanShaders::Check(Error error){if(!error)return true;auto* api=Functions();if(api){char* text=nullptr;
  if(api->error_write(error,&text)==0&&text){__android_log_write(ANDROID_LOG_WARN,"SwanShaders",text);api->error_string_free(&text);}api->error_free(&error);}return false;}
SwanShaders::Selection SwanShaders::Selected(){std::lock_guard lock(mutex);return selected;}
void SwanShaders::Select(bool enabled,std::string path){std::lock_guard lock(mutex);selected.enabled=enabled;selected.path=std::move(path);selected.parameters=preset_parameters[selected.path];selected.generation++;}
void SwanShaders::SetParameters(std::string path,std::map<std::string,float> parameters){std::lock_guard lock(mutex);preset_parameters[path]=parameters;if(selected.path==path){selected.parameters=std::move(parameters);selected.generation++;}}
std::string SwanShaders::ParametersJSON(const std::string& path){auto* api=Functions();if(!api)return "[]";
  Handle preset=nullptr;if(!Check(api->preset_create(path.c_str(),&preset)))return "[]";
  Parameters list{};std::ostringstream out;out<<'[';
  if(Check(api->preset_params(&preset,&list))){for(uint64_t i=0;i<list.length&&i<2048;i++){const auto& p=list.parameters[i];if(i)out<<',';
    out<<"{\"name\":"<<Quote(p.name)<<",\"description\":"<<Quote(p.description)<<",\"initial\":"<<p.initial<<",\"minimum\":"<<p.minimum<<",\"maximum\":"<<p.maximum<<",\"step\":"<<p.step<<'}';}Check(api->free_params(list));}
  Check(api->preset_free(&preset));out<<']';return out.str();
}
