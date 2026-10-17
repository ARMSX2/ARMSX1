// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>

namespace SwanGLPresentation {
// Clearing the Y low bit after flipping interleaved field parity rounds odd
// bottom-origin coordinates down. Compensate so both fields address the same
// source rows as top-origin Vulkan. Separate-field scanout already uses /2.
inline constexpr unsigned DisplayVramOffsetY(unsigned texture_height,unsigned vram_top,
    unsigned display_height,unsigned field,bool separate_fields,bool interleaved_fields) {
  return texture_height-vram_top-field-(display_height>>unsigned(separate_fields))+
    unsigned(interleaved_fields);
}
struct Sampling { float scale_x,scale_y,min_x,min_y,max_x,max_y; };
inline Sampling SampleBounds(unsigned width,unsigned height,unsigned texture_width,unsigned texture_height) {
  texture_width=std::max(1u,texture_width);texture_height=std::max(1u,texture_height);
  width=std::clamp(width,1u,texture_width);height=std::clamp(height,1u,texture_height);
  // Address the centers of the first/last VALID texels, not the larger backing allocation.
  // This clamps the sampling footprint without removing or zooming any source pixels.
  return {float(width)/texture_width,float(height)/texture_height,
    0.5f/texture_width,0.5f/texture_height,
    (float(width)-0.5f)/texture_width,(float(height)-0.5f)/texture_height};
}
inline constexpr const char* VertexShader=R"(#version 300 es
precision highp float;
out highp vec2 uv;
uniform bool flip;
uniform vec2 uvScale;
uniform int rotation;
void main(){
  vec2 p=vec2((gl_VertexID<<1)&2,gl_VertexID&2);
  vec2 q=rotation==90?vec2(1.-p.y,p.x):rotation==180?1.-p:rotation==270?vec2(p.y,1.-p.x):p;
  uv=vec2(q.x,flip?1.-q.y:q.y)*uvScale;
  gl_Position=vec4(p*2.-1.,0.,1.);
}
)";
inline constexpr const char* FragmentShader=R"(#version 300 es
precision highp float;
in highp vec2 uv;
uniform highp sampler2D image;
uniform vec2 uvMin;
uniform vec2 uvMax;
out vec4 color;
void main(){color=texture(image,clamp(uv,uvMin,uvMax));}
)";
}
