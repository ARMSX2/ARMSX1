// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/cpu_instruction_analysis.h"
#include "../native/swanstation/cpu_code_page_policy.h"
#include <cassert>
#include <iostream>
using namespace SwanCpuAnalysis;
constexpr uint32_t I(unsigned op,unsigned rs,unsigned rt){return op<<26|rs<<21|rt<<16;}
constexpr uint32_t R(unsigned rs,unsigned rt,unsigned rd,unsigned fn){return rs<<21|rt<<16|rd<<11|fn;}
int main(){
  auto shift=Decode(R(0,8,9,0));assert(shift.reads==Bit(8)&&shift.writes==Bit(9)&&shift.rename_safe);
  auto add=Decode(R(8,9,10,33));assert(add.reads==(Bit(8)|Bit(9))&&add.writes==Bit(10)&&add.rename_safe);
  assert(!Decode(R(8,9,10,32)).rename_safe); // signed overflow can trap
  auto partial=Decode(I(34,4,8));assert(partial.reads==(Bit(4)|Bit(8))&&partial.writes==Bit(8)&&!partial.rename_safe);
  auto store=Decode(I(43,4,8));assert(store.reads==(Bit(4)|Bit(8))&&!store.writes&&!store.rename_safe);
  auto high=Decode(R(0,0,8,16));assert(high.reads==Bit(32)&&high.writes==Bit(8));
  auto multiply=Decode(R(8,9,0,24));assert(multiply.writes==(Bit(32)|Bit(33)));
  assert(Decode(I(4,8,9)).branch);
  assert(!Decode(I(16,4,8)).known); // coprocessor state is a conservative fence
  assert(!Decode(0).reads&&!Decode(0).writes); // architectural zero never becomes live
  CodePagePolicy<4> pages;
  assert(!pages.Invalidate(1,10));assert(!pages.Invalidate(1,11));assert(!pages.Invalidate(1,12));
  assert(pages.Invalidate(1,13)&&pages.Manual(1)&&pages.ManualCount()==1);
  assert(!pages.Invalidate(1,14)&&pages.ManualCount()==1); // sticky until cache reset
  assert(!pages.Invalidate(2,1));assert(!pages.Invalidate(2,100));assert(!pages.Invalidate(2,101));
  assert(!pages.Manual(2));assert(!pages.Invalidate(99,0)&&!pages.Manual(99));
  pages.Reset();assert(pages.ManualCount()==0&&!pages.Manual(1));
  assert(!pages.Invalidate(0,0xfffffffd));assert(!pages.Invalidate(0,0xfffffffe));
  assert(!pages.Invalidate(0,0));assert(pages.Invalidate(0,1)); // frame-counter wrap
  std::cout<<"MIPS register masks, delay-sensitive fences and adaptive code-page windows passed\n";
}
