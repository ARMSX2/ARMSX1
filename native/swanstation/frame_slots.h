// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
namespace SwanFrames {
// Fence ownership is separate from swapchain-image ownership. Present-complete
// semaphores must be indexed by acquired image, never by these rotating slots.
class Slots {
  size_t count_=2,current_=0;
  std::array<bool,3> pending_{};
public:
  void Reset(size_t count) {count_=std::clamp(count,size_t(1),size_t(3));current_=0;pending_.fill(false);}
  size_t Count() const {return count_;}
  size_t Current() const {return current_;}
  bool Pending(size_t index) const {return pending_.at(index);}
  void Submitted() {pending_.at(current_)=true;}
  void Retired(size_t index) {pending_.at(index)=false;}
  void Advance() {current_=(current_+1)%count_;}
};
}
