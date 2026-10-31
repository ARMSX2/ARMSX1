#version 450
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma parameter Tint "Tint amount" 0.75 0.0 1.0 0.05
layout(push_constant) uniform Parameters {
    mat4 MVP;
    float Tint;
    uint TotalSubFrames;
    uint CurrentSubFrame;
} params;
#pragma stage vertex
layout(location = 0) in vec4 Position;
layout(location = 1) in vec2 TexCoord;
layout(location = 0) out vec2 uv;
void main() { gl_Position = params.MVP * Position; uv = TexCoord; }
#pragma stage fragment
layout(location = 0) in vec2 uv;
layout(binding = 2) uniform sampler2D Source;
layout(location = 0) out vec4 FragColor;
void main() {
    vec3 color = texture(Source, uv).rgb;
    if (params.TotalSubFrames > 1u) {
        color *= params.CurrentSubFrame == 1u ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    } else {
        color *= vec3(1.0, params.Tint, params.Tint);
    }
    FragColor = vec4(color, 1.0);
}
