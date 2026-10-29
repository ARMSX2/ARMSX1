// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>

namespace SwanCpuDiagnostics {
// CPU clocks measure scheduled CPU time, not time asleep or waiting on a GPU.
// All percentages use one CPU core as 100%; process usage may exceed 100%.
struct Counters {
  uint64_t wall=0,vm=0,graphics=0,process=0;
  int vm_tid=0,graphics_tid=0;
  bool valid=false,graphics_valid=false;
};
struct Reading {
  double window_ms=0,vm_percent=0,graphics_percent=-1,process_percent=0,other_percent=-1;
  int vm_tid=0,graphics_tid=0;
};
class Window {
 public:
  void Reset(Counters now) {previous_=now;}
  bool Read(Counters now,Reading& result) {
    const auto old=previous_;previous_=now;
    if(!old.valid||!now.valid||now.wall<=old.wall||now.vm<old.vm||now.process<old.process||
       now.vm_tid!=old.vm_tid)return false;
    const double wall=double(now.wall-old.wall);
    const auto vm=now.vm-old.vm,process=now.process-old.process;
    result={};result.window_ms=wall/1e6;result.vm_percent=100.0*vm/wall;
    result.process_percent=100.0*process/wall;result.vm_tid=now.vm_tid;
    if(old.graphics_valid&&now.graphics_valid&&now.graphics_tid!=now.vm_tid&&
       old.graphics_tid==now.graphics_tid&&now.graphics>=old.graphics) {
      const auto graphics=now.graphics-old.graphics;
      result.graphics_tid=now.graphics_tid;result.graphics_percent=100.0*graphics/wall;
      // Sequential clock reads can differ slightly at the sampling boundary.
      result.other_percent=std::max(0.0,100.0*(double(process)-double(vm)-double(graphics))/wall);
    }
    return true;
  }
 private:
  Counters previous_;
};
}
