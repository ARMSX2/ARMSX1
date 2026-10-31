#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma parameter Tint "Tint amount" 0.75 0.0 1.0 0.05
layout(push_constant) uniform Parameters { mat4 MVP; float Tint; } params;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = params.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(binding = 2) uniform sampler2D Source;
layout(location = 0) out vec4 FragColor;
void main() { FragColor = vec4(texture(Source, uv).rgb * vec3(1.0, params.Tint, params.Tint), 1.0); }
