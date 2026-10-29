// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
namespace SwanCpuAnalysis {
template<size_t Count> class CodePagePolicy {
  struct Page {uint32_t first=0,hits=0;bool manual=false;};
  std::array<Page,Count> pages{};
  size_t manual_count=0;
public:
  bool Invalidate(size_t page,uint32_t frame) {
    if(page>=Count)return false;
    auto& value=pages[page];
    if(value.manual)return false;
    if(!value.hits || uint32_t(frame-value.first)>60){value.first=frame;value.hits=0;}
    if(++value.hits<4)return false;
    value.manual=true;++manual_count;return true;
  }
  bool Manual(size_t page) const {return page<Count&&pages[page].manual;}
  size_t ManualCount() const {return manual_count;}
  void Reset(){pages={};manual_count=0;}
};
}
