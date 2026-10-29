// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <android/native_window.h>
#include "libretro_vulkan.h"
#include <string>
#include <vector>
#include "presentation.h"
#include "present_recovery.h"
namespace SwanVulkan {
bool Negotiate(const retro_hw_render_context_negotiation_interface_vulkan*, ANativeWindow*);
const retro_hw_render_interface_vulkan* Interface();
bool Window(ANativeWindow*);
bool Present(unsigned width,unsigned height,float aspect,bool suspended,bool black=false,unsigned subframe=1,unsigned total=1,float nominal=60,int64_t desired_time=0,int64_t submit_time=0,void (*wait)(int64_t)=nullptr,SwanFramePacing::PresentRequest recovery={});
void Blank();
void Close();
bool Ready();
std::vector<unsigned char> Pixels(unsigned width,unsigned height);
void Driver(std::string directory,std::string name,std::string redirect,std::string hooks);
bool DriverVariables(const std::string& variables);
void Presentation(SwanPresentation::Options options);
bool ShaderActive();
void ConfigurePacing(bool vsync,unsigned queued_frames);
double LastWaitMs();
bool Vsync(bool enabled,bool matched=true);
int64_t LastDisplayTime();
float RefreshHz();
std::string TimingSnapshot();
void ResetTimingWindow();
}
