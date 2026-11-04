#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

// Conservative texture coordinates for the samples a primitive can actually cover.
// Keep the rounded plane used by the rasterizers, rather than substituting raw UVs.
inline std::pair<int,int> psxTextureRange(const int* q,int component,int mask,int offset,bool enhanced){
    using Wide=int64_t;
    const Wide a=q[28+component],gx=q[33+component],gy=q[38+component];
    const Wide dx=gx*(q[6]-q[4]+int(enhanced)),dy=gy*(q[7]-q[5]+int(enhanced));
    Wide low=std::min({a,a+dx,a+dy,a+dx+dy}),high=std::max({a,a+dx,a+dy,a+dx+dy});
    // The bounding rectangle includes corners outside a triangle. Those corners
    // can spuriously cross UV wrapping and turn a small texture into a whole page.
    // Extreme gradients retain the old bound: HD floating interpolation can lose
    // precision when its terms are large even if they cancel to a small UV value.
    constexpr Wide preciseTerm=Wide(1)<<26;
    if(q[0]==0&&(!enhanced||(std::abs(a)<=preciseTerm&&std::abs(dx)<=preciseTerm&&std::abs(dy)<=preciseTerm))){
        Wide vertexLow=INT64_MAX,vertexHigh=INT64_MIN;bool precise=true;
        for(int vertex=0;vertex<3;vertex++){
            Wide vx=gx*(q[43+vertex*2]-q[4]),vy=gy*(q[44+vertex*2]-q[5]);
            if(enhanced&&(std::abs(vx)>preciseTerm||std::abs(vy)>preciseTerm))precise=false;
            Wide value=a+vx+vy;
            vertexLow=std::min(vertexLow,value);vertexHigh=std::max(vertexHigh,value);
        }
        // One HD texel covers the fragment shader's interpolation/bias rounding.
        const Wide padding=enhanced?65536:0;
        Wide tighterLow=std::max(low,vertexLow-padding),tighterHigh=std::min(high,vertexHigh+padding);
        // A clipped bounding box can contain no triangle samples at all. Preserve
        // a valid conservative range instead of emitting inverted rectangles.
        if(precise&&tighterLow<=tighterHigh){low=tighterLow;high=tighterHigh;}
    }
    const Wide lo=low>>16,hi=high>>16;
    if(hi-lo>=255)return{offset,(255&~mask)|offset};
    if(mask==0&&(lo>>8)==(hi>>8))return{int(lo)&255,int(hi)&255};
    int first=255,last=0;
    for(Wide value=lo;value<=hi;value++){
        int mapped=((int(value)&255)&~mask)|offset;
        first=std::min(first,mapped);last=std::max(last,mapped);
    }
    return{first,last};
}
