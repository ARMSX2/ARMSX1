// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/presentation.h"
#include <cassert>
#include <cstdio>
int main() {
  using namespace SwanPresentation;
  Options o;
  auto r=Fit(1920,1080,320,240,Aspect(o,4.f/3),o);
  assert(r.x==240&&r.y==0&&r.width==1440&&r.height==1080);
  o.aspect=2;r=Fit(1920,1080,320,240,Aspect(o,4.f/3),o);
  assert(r.x==0&&r.y==0&&r.width==1920&&r.height==1080);
  o.aspect=1;r=Fit(1920,1080,320,240,Aspect(o,4.f/3),o);
  assert(r.x==420&&r.width==1080);
  o.aspect=0;o.integer=true;r=Fit(1920,1080,320,240,Aspect(o,4.f/3),o);
  assert(r.width==1280&&r.height==960&&r.y==60);
  o.stretch=true;r=Fit(1920,1080,320,240,Aspect(o,4.f/3),o);
  assert(r.x==0&&r.y==0&&r.width==1920&&r.height==1080);
  o={};o.portrait_top=true;o.inset=40;r=Fit(1080,1920,320,240,Aspect(o,4.f/3),o);
  assert(r.x==0&&r.y==40&&r.width==1080&&r.height==810);
  o.inset=4000;r=Fit(1080,1920,320,240,Aspect(o,4.f/3),o);
  assert(r.y==1919&&r.height==1&&r.width>=1);
  o={};r=Fit(120,80,640,480,Aspect(o,4.f/3),o);assert(r.width<=120&&r.height<=80);
  o.rotation=90;r=Fit(960,720,320,240,4.f/3,o);assert(r.width==540&&r.height==720&&r.x==210);
  o.rotation=270;r=Fit(720,960,320,240,4.f/3,o);assert(r.width==720&&r.height==960);
  o.rotation=180;r=Fit(960,720,320,240,4.f/3,o);assert(r.width==960&&r.height==720);
  auto m=Matrix(90);assert(m[1]==2&&m[4]==-2&&m[12]==1&&m[13]==-1);
  assert(Rotation(-90)==0&&Rotation(45)==0&&Rotation(270)==270);
  puts("Shared Vulkan/OpenGL/ANGLE presentation: 13 cases passed");
}
