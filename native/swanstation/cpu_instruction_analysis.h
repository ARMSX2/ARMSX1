// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace SwanCpuAnalysis {
struct Use {
  uint64_t reads=0,writes=0;
  bool known=false,branch=false,rename_safe=false;
};
constexpr uint64_t Bit(unsigned reg){return reg?uint64_t(1)<<reg:0;}
// Conservative MIPS register use. Unknown/coprocessor instructions fence the
// analysis; memory operations never qualify for destructive register reuse.
constexpr Use Decode(uint32_t word) {
  const unsigned op=word>>26,rs=(word>>21)&31,rt=(word>>16)&31,rd=(word>>11)&31,fn=word&63;
  const auto s=Bit(rs),t=Bit(rt),d=Bit(rd);
  if(op==0) {
    switch(fn) {
      case 0:case 2:case 3:return {t,d,true,false,true};
      case 4:case 6:case 7:return {s|t,d,true,false,true};
      case 8:return {s,0,true,true,false};
      case 9:return {s,d,true,true,false};
      case 16:return {Bit(32),d,true,false,false};
      case 17:return {s,Bit(32),true,false,false};
      case 18:return {Bit(33),d,true,false,false};
      case 19:return {s,Bit(33),true,false,false};
      case 24:case 25:case 26:case 27:return {s|t,Bit(32)|Bit(33),true,false,false};
      case 32:case 34:return {s|t,d,true,false,false}; // can trap
      case 33:case 35:case 36:case 37:case 38:case 39:case 42:case 43:return {s|t,d,true,false,true};
      default:return {~uint64_t(0),~uint64_t(0),false,true,false};
    }
  }
  switch(op) {
    case 1:return {s,(rt&16)?Bit(31):0,true,true,false};
    case 2:return {0,0,true,true,false};
    case 3:return {0,Bit(31),true,true,false};
    case 4:case 5:return {s|t,0,true,true,false};
    case 6:case 7:return {s,0,true,true,false};
    case 8:return {s,t,true,false,false};
    case 9:case 10:case 11:case 12:case 13:case 14:return {s,t,true,false,true};
    case 15:return {0,t,true,false,true};
    case 32:case 33:case 35:case 36:case 37:return {s,t,true,false,false};
    case 34:case 38:return {s|t,t,true,false,false};
    case 40:case 41:case 42:case 43:case 46:return {s|t,0,true,false,false};
    case 50:case 58:return {s,0,true,false,false};
    default:return {~uint64_t(0),~uint64_t(0),false,true,false};
  }
}
}
