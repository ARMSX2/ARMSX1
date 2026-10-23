// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cmath>
#include <string>
namespace SwanFramePresentation {
inline unsigned Count(int mode, int multiple, float refresh, float nominal,
                      bool normal_speed, bool visible, bool shader_enabled) {
  if(mode<1||mode>2||multiple<2||multiple>4||!normal_speed||!visible||
     !std::isfinite(refresh)||!std::isfinite(nominal)||nominal<40||nominal>65||
     (mode==2&&!shader_enabled))return 1;
  // Accommodate 59.94 fps on 120 Hz, but never 50 fps on 120 Hz / 60 on 90.
  return std::abs(refresh-nominal*multiple)<=0.6f?unsigned(multiple):1;
}
inline const char* Downsample(const std::string& value) {
  if(value=="-1")return "Adaptive";
  if(value=="true")return "Box";
  try { size_t used=0;const int n=std::stoi(value,&used);
    if(used==value.size()&&n>0)return "Box";
  }catch(...){}
  return "Disabled";
}
}
