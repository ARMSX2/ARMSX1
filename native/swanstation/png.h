// SPDX-License-Identifier: GPL-3.0-or-later
// Small independently authored RGBA PNG writer using Android's zlib.
#pragma once
#include <zlib.h>
#include <cstdint>
#include <fstream>
#include <vector>
inline void PngU32(std::vector<unsigned char>& out,uint32_t value) {
    for(int shift=24;shift>=0;shift-=8)out.push_back(static_cast<unsigned char>(value>>shift));
}
inline void PngChunk(std::vector<unsigned char>& out,const char* type,const std::vector<unsigned char>& bytes) {
    PngU32(out,static_cast<uint32_t>(bytes.size()));
    const size_t begin=out.size();
    out.insert(out.end(),type,type+4);out.insert(out.end(),bytes.begin(),bytes.end());
    PngU32(out,crc32(0,out.data()+begin,static_cast<uInt>(4+bytes.size())));
}
inline bool WritePng(const char* path,unsigned width,unsigned height,int channels,const void* data,unsigned stride) {
    if(!width||!height||width>8192||height>8192||channels!=4||!data||stride<width*4)return false;
    std::vector<unsigned char> rows((static_cast<size_t>(width)*4+1)*height);
    const auto* input=static_cast<const unsigned char*>(data);
    for(unsigned y=0;y<height;y++)
        std::copy(input+y*stride,input+y*stride+width*4,rows.begin()+y*(width*4+1)+1);
    uLongf size=compressBound(rows.size());std::vector<unsigned char> compressed(size);
    if(compress2(compressed.data(),&size,rows.data(),rows.size(),Z_BEST_SPEED)!=Z_OK)return false;
    compressed.resize(size);
    std::vector<unsigned char> png={137,80,78,71,13,10,26,10}, header;
    PngU32(header,width);PngU32(header,height);header.insert(header.end(),{8,6,0,0,0});
    PngChunk(png,"IHDR",header);PngChunk(png,"IDAT",compressed);PngChunk(png,"IEND",{});
    std::ofstream stream(path,std::ios::binary|std::ios::trunc);
    return static_cast<bool>(stream.write(reinterpret_cast<const char*>(png.data()),png.size()));
}
