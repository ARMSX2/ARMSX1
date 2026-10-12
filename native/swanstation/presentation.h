// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <array>
namespace SwanPresentation {
struct Options { int aspect=0; float custom=4.f/3; bool stretch=false, integer=false, portrait_top=false; int inset=0; int rotation=0; };
inline int Rotation(int degrees){return degrees==90||degrees==180||degrees==270?degrees:0;}
inline std::array<float,16> Matrix(int degrees){const int turns=Rotation(degrees)/90;
  const float c=turns==0?1:turns==2?-1:0,s=turns==1?1:turns==3?-1:0;
  return {2*c,2*s,0,0,-2*s,2*c,0,0,0,0,0,0,-c+s,-s-c,0,1};}
struct Rect {int x,y,width,height;};
inline float Aspect(const Options& o,float core) {
  return o.aspect==1?1.f:o.aspect==2?16.f/9:o.aspect==3?o.custom:core>0?core:4.f/3;
}
inline Rect Fit(int width,int height,unsigned source_width,unsigned source_height,float aspect,const Options& o) {
  if(o.rotation==90||o.rotation==270){aspect=1.f/aspect;std::swap(source_width,source_height);}
  width=std::max(1,width);height=std::max(1,height);
  const int inset=height>width&&o.portrait_top?std::clamp(o.inset,0,height-1):0;
  const int available=height-inset;
  int dw=width,dh=available;
  if(!o.stretch) {
    dh=std::max(1,int(width/aspect));
    if(dh>available){dh=available;dw=std::max(1,int(dh*aspect));}
    if(o.integer&&source_width&&source_height) {
      const int base_height=int(source_height),base_width=std::max(1,int(base_height*aspect));
      const int multiple=std::min(width/base_width,available/base_height);
      if(multiple>0){dw=base_width*multiple;dh=base_height*multiple;}
    }
  }
  dw=std::clamp(dw,1,width);dh=std::clamp(dh,1,available);
  return {(width-dw)/2,inset+(height>width&&o.portrait_top?0:(available-dh)/2),dw,dh};
}
}
