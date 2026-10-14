// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
// Drawing/layout only. UI_RenderFrame_Racing also processes gameplay; never skip it.
namespace SwanCtrHud {
struct Patch { uint32_t address, original, replacement; };
using Read=std::function<bool(uint32_t,uint32_t&)>;
using Write=std::function<bool(uint32_t,uint32_t)>;
class Controller {
 public:
  std::string status="Original HUD";
  bool Tick(bool wanted,const Read& read,const Write& write) {
    uint32_t gt=0,mode=0,track=99,players=0;
    bool valid=read(0x8008d2ac,gt)&&gt==0x80096b20&&read(gt,mode)&&
      !(mode&(0x20|0x2000|0x100000|0x200000|0x20000000|0x40000000|0xf))&&
      read(gt+0x1a10,track)&&track<18&&read(gt+0x1ca8,players)&&(players&255)==1;
    if(!wanted||!valid){Restore(read,write);status="Original HUD";return false;}
    if(!owned.empty()){
      for(const auto& p:owned){uint32_t value=0;if(!read(p.address,value)||
        (value!=p.replacement&&value!=p.original)){
        Restore(read,write);status="HUD data changed; restored";return false;
      }}
      return Apply(read,write);
    }
    std::vector<Patch> plan;
    uint32_t table=0;
    if(!read(0x8008625c,table)||!Pointer(table,20*8)){
      status="HUD layout pointer unsupported";return false;
    }
    for(uint32_t i=0;i<20;i++){
      uint32_t xy=0;if(!read(table+i*8,xy)){status="HUD coordinates unavailable";return false;}
      int x=static_cast<int16_t>(xy),y=static_cast<int16_t>(xy>>16);
      if(x< -512||x>768||y< -512||y>512){status="HUD layout differs from retail USA";return false;}
      // Only XY, not scale/depth. (-800,-800) is outside the PS1 viewport.
      plan.push_back({table+i*8,xy,0xfce0fce0u});
    }
    // Pure glyph/portrait drawing, not rank updates or item roulette.
    for(uint32_t address:{0x800224fcu,0x8004e8d8u}){
      uint32_t first=0,second=0;
      if(!read(address,first)||!read(address+4,second)||
        (first&0xffff0000u)!=0x27bd0000u||static_cast<int16_t>(first)>=0||(first&7)!=0){
        status="HUD draw prologue unsupported at "+std::to_string(address);return false;
      }
      plan.push_back({address,first,0x03e00008u}); // Our MIPS: jr ra; nop.
      plan.push_back({address+4,second,0});
    }
    // UI_ThTick_big1 and UI_ThTick_CountPickup control HUD model drawing.
    // Fruit uses CountPickup, not big1. Keep rotation/shine/gameplay running.
    // A comparison against 0x300 can never equal the 0xff0100-masked HUD flags.
    for(const auto& range:std::vector<std::pair<uint32_t,uint32_t>>{
        {0x8004ca04u,0x8004caa8u},{0x8004c718u,0x8004c850u}}){
    uint32_t comparison=0;bool hudLoad=false;
    for(uint32_t address=range.first;address<range.second;address+=4){
      uint32_t word=0;if(!read(address,word)){status="HUD visibility code unavailable";return false;}
      if((word>>26)==0x23&&(word&65535)==0x1d30)hudLoad=true;
      if(((word>>26)==9||(word>>26)==13)&&((word>>21)&31)==0&&
        ((word>>16)&31)!=0&&(word&65535)==0x100){
        if(comparison){status="HUD visibility comparison ambiguous";return false;}
        comparison=address;plan.push_back({address,word,(word&0xffff0000u)|0x300u});
      }
    }
    if(!hudLoad||!comparison){status="HUD model visibility layout unsupported";return false;}
    }
    owned=std::move(plan);return Apply(read,write);
  }
  void Restore(const Read& read,const Write& write){
    for(const auto& p:owned){uint32_t value=0;if(read(p.address,value)&&value==p.replacement)
      write(p.address,p.original);}
    owned.clear();
  }
 private:
  std::vector<Patch> owned;
  static bool Pointer(uint32_t p,uint32_t size){return ((p>>24)==0x80||(p>>24)==0xa0)&&
    (p&3)==0&&(p&0x1fffffffu)<=2*1024*1024-size;}
  bool Apply(const Read& read,const Write& write){
    for(const auto& p:owned){uint32_t value=0;if(!read(p.address,value)||
      (value!=p.replacement&&!write(p.address,p.replacement))){
      Restore(read,write);status="HUD write failed; rolled back";return false;
    }}
    status="Race HUD moved (experimental)";return true;
  }
};
}
