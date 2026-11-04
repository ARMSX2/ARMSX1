#pragma once
#include <algorithm>
#include <cmath>

// Shared by GLES and Vulkan presentation. Guest drawing coordinates are unchanged.
struct PsxDisplayRect { int x,y,w,h; };
// Where the guest's display range sits in a standard TV picture, as fractions of it; the default fills the picture.
struct PsxDisplayWindow { double x=0,y=0,w=1,h=1; bool full()const{return x<=0&&y<=0&&w>=1&&h>=1;} };
inline PsxDisplayRect psxDisplayRect(int sw,int sh,int sourceW,int sourceH,double aspect,bool stretch,bool integerScale,PsxDisplayWindow window={}){
    sw=std::max(1,sw);sh=std::max(1,sh);if(!std::isfinite(aspect)||aspect<0.1||aspect>10)aspect=4.0/3.0;
    int w=sw,h=sh;
    if(!stretch){w=std::min(sw,std::max(1,int(std::lround(sh*aspect))));h=std::min(sh,std::max(1,int(std::lround(w/aspect))));}
    if(integerScale&&!stretch&&window.full()&&sourceW>0&&sourceH>0){int k=std::min(w/sourceW,h/sourceH);if(k>=1){w=sourceW*k;h=sourceH*k;}}
    if(window.full())return {(sw-w)/2,(sh-h)/2,w,h};
    // A small display range (a loading screen, say) shows in its place inside a black picture, not stretched over it.
    return {(sw-w)/2+int(std::lround(window.x*w)),(sh-h)/2+int(std::lround(window.y*h)),std::max(1,int(std::lround(window.w*w))),std::max(1,int(std::lround(window.h*h)))};
}
