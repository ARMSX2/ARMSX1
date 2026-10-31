#pragma once
#include <algorithm>
#include <cmath>

// Shared by GLES and Vulkan presentation. Guest drawing coordinates are unchanged.
struct PsxDisplayRect { int x,y,w,h; };
inline PsxDisplayRect psxDisplayRect(int sw,int sh,int sourceW,int sourceH,double aspect,bool stretch,bool integerScale){
    sw=std::max(1,sw);sh=std::max(1,sh);if(!std::isfinite(aspect)||aspect<0.1||aspect>10)aspect=4.0/3.0;
    int w=sw,h=sh;
    if(!stretch){w=std::min(sw,std::max(1,int(std::lround(sh*aspect))));h=std::min(sh,std::max(1,int(std::lround(w/aspect))));}
    if(integerScale&&!stretch&&sourceW>0&&sourceH>0){int k=std::min(w/sourceW,h/sourceH);if(k>=1){w=sourceW*k;h=sourceH*k;}}
    return {(sw-w)/2,(sh-h)/2,w,h};
}
