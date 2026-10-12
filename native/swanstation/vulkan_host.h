// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <android/native_window.h>
#include "libretro_vulkan.h"
#include <string>
#include <vector>
#include "presentation.h"
namespace SwanVulkan {
bool Negotiate(const retro_hw_render_context_negotiation_interface_vulkan*, ANativeWindow*);
const retro_hw_render_interface_vulkan* Interface();
bool Window(ANativeWindow*);
bool Present(unsigned width,unsigned height,float aspect,bool suspended);
void Blank();
void Close();
bool Ready();
std::vector<unsigned char> Pixels(unsigned width,unsigned height);
void Driver(std::string directory,std::string name,std::string redirect,std::string hooks);
void Presentation(SwanPresentation::Options options);
bool ShaderActive();
}
