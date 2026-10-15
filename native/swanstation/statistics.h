// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstdint>

// Emulation-thread-only counters. Never enable the debug CPU dispatcher or
// instrument generated CPU instructions just to draw an OSD. Ordinals 0..30
// retain the existing Android ABI; 31/32 are the actual video callback size.
namespace SwanTelemetry {
constexpr unsigned Count=33;
struct Window {
  bool enabled=false;
  unsigned scale=0;
  uint32_t tick_start=0;
  uint64_t frames=0,voice_samples=0;
  bool ticks_valid=true;
  std::array<uint64_t,Count> totals{};
  void Clear(){totals.fill(0);frames=voice_samples=0;ticks_valid=true;}
  void Reset(){enabled=false;scale=0;Clear();}
  void Begin(bool on,uint32_t ticks){if(on!=enabled){Clear();enabled=on;}tick_start=ticks;}
  void End(uint32_t ticks){if(!enabled)return;frames++;
    const uint32_t elapsed=ticks-tick_start; // unsigned wrap is intentional
    if(elapsed<(1u<<24))totals[6]+=elapsed;else ticks_valid=false;
  }
  void Add(unsigned index,uint64_t value){if(enabled&&index<Count)totals[index]+=value;}
  void Primitive(unsigned type,bool quad,unsigned vertices){
    if(!enabled)return;
    if(type==1)totals[8]+=quad?2:1;
    else if(type==2)totals[10]+=vertices>1?vertices-1:1;
    else if(type==3)totals[9]++;
  }
  void Voices(unsigned count){if(enabled){totals[13]+=count;voice_samples++;}}
  void Dma(unsigned channel,uint32_t words){
    if(!enabled)return;totals[17]+=words;
    const unsigned index=channel==2?18:channel==4?19:channel<2?20:channel==3?21:channel==6?22:Count;
    if(index<Count)totals[index]+=words;
  }
  std::array<double,Count> Take(){
    std::array<double,Count> result;result.fill(-1);
    if(enabled&&frames){
      for(const unsigned index:{6u,8u,9u,10u,14u,15u,16u,17u,18u,19u,20u,21u,22u})
        result[index]=double(totals[index])/frames;
      if(!ticks_valid)result[6]=-1;
      if(voice_samples)result[13]=double(totals[13])/voice_samples;
    }
    Clear();return result;
  }
};
extern Window counters;
}
