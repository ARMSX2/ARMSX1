// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/frame_presentation.h"
#include <cassert>
#include <limits>
int main(){
  using namespace SwanFramePresentation;
  assert(Count(0,2,120,60,true,true,true)==1);
  assert(Count(1,2,120,59.94f,true,true,false)==2);
  assert(Count(1,2,100,50,true,true,false)==2);
  assert(Count(1,2,120,50,true,true,false)==1);
  assert(Count(1,2,90,60,true,true,false)==1);
  assert(Count(2,3,180,60,true,true,true)==3);
  assert(Count(2,3,180,60,true,true,false)==1);
  assert(Count(1,2,120,60,false,true,true)==1);
  assert(Count(1,2,120,60,true,false,true)==1);
  assert(Count(1,2,std::numeric_limits<float>::quiet_NaN(),60,true,true,true)==1);
  assert(std::string(Downsample("-1"))=="Adaptive");
  for(auto value:{"true","1","2","3","4","8"})assert(std::string(Downsample(value))=="Box");
  for(auto value:{"false","0","","junk","2junk","-2"})assert(std::string(Downsample(value))=="Disabled");
}
