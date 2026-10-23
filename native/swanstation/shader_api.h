// SPDX-License-Identifier: GPL-3.0-or-later
// ABI declarations independently transcribed from official librashader-capi 0.12.0 repr(C).
#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <cstddef>
#include <string>
#include <map>
namespace SwanShaders {
using Handle=void*;
using Error=void*;
struct Image {VkImage handle;VkFormat format;uint32_t width,height;};
struct Device {VkPhysicalDevice gpu;VkInstance instance;VkDevice device;VkQueue queue;PFN_vkGetInstanceProcAddr entry;};
struct Options {size_t version;uint32_t frames_in_flight;bool force_no_mipmaps,use_dynamic_rendering,disable_cache;};
struct GlOptions {size_t version;uint16_t glsl_version;bool use_dsa,force_no_mipmaps,disable_cache;};
struct GlImage {uint32_t handle,format,width,height;};
struct Viewport {float x,y;uint32_t width,height;};
// Full official 0.12.0 repr(C) frame options, shared by GL and Vulkan.
struct FrameOptions {
  size_t version=5;bool clear_history=false;int32_t frame_direction=1;
  uint32_t rotation=0,total_subframes=1,current_subframe=1;
  float aspect_ratio=0,frames_per_second=60;uint32_t frametime_delta=0;
  uint32_t color_space=0;float brightness_nits=200;uint32_t expand_gamut=0;
  float gyroscope[3]{},accelerometer[3]{},accelerometer_rest[3]{};
};
struct Parameter {const char* name;const char* description;float initial,minimum,maximum,step;};
struct Parameters {const Parameter* parameters;uint64_t length;};
struct Api {
  size_t(*abi)();size_t(*version)();
  Error(*preset_create)(const char*,Handle*);Error(*preset_free)(Handle*);
  Error(*preset_params)(const Handle*,Parameters*);Error(*free_params)(Parameters);
  Error(*create)(Handle*,Device,const Options*,Handle*);
  Error(*frame)(Handle*,VkCommandBuffer,size_t,Image,Image,const Viewport*,const float*,const void*);
  Error(*parameter)(Handle*,const char*,float);Error(*free)(Handle*);int32_t(*error_free)(Error*);
  Error(*gl_create)(Handle*,const void*(*)(const char*),const GlOptions*,Handle*);
  Error(*gl_frame)(Handle*,size_t,GlImage,GlImage,const Viewport*,const float*,const void*);
  Error(*gl_parameter)(Handle*,const char*,float);Error(*gl_free)(Handle*);
  int32_t(*error_write)(Error,char**);int32_t(*error_string_free)(char**);
};
Api* Functions();
bool Check(Error error);
void Initialize(const std::string& root);
std::string PresentationPreset();
std::string ParametersJSON(const std::string& path);
struct Selection {uint64_t generation=1;bool enabled=false;std::string path;std::map<std::string,float> parameters;};
Selection Selected();
void Select(bool enabled,std::string path);
void SetParameters(std::string path,std::map<std::string,float> parameters);
uint32_t DrawGL(uint32_t source,uint32_t core_framebuffer,unsigned width,unsigned height,unsigned window_width,unsigned window_height,bool software,unsigned subframe=1,unsigned total=1,float nominal=60);
void CloseGL();
bool ActiveGL();
}
