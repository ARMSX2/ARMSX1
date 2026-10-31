// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/cpu_diagnostics.h"
#include <cassert>
#include <cmath>
#include <iostream>
using namespace SwanCpuDiagnostics;
int main() {
  Window window;Reading result;
  Counters start{1000000000,100000000,200000000,400000000,11,22,true,true};
  window.Reset(start);
  // Two seconds of wall time: 0.4 s VM, 0.2 s GPU and 0.1 s other CPU.
  Counters end{3000000000,500000000,400000000,1100000000,11,22,true,true};
  assert(window.Read(end,result));
  assert(result.window_ms==2000&&result.vm_percent==20&&result.graphics_percent==10);
  assert(result.process_percent==35&&result.other_percent==5);
  // More than one core of process time is valid; waiting time isn't CPU time.
  window.Reset(start);end.process=3400000000;end.vm=start.vm;end.graphics=start.graphics;
  assert(window.Read(end,result)&&result.process_percent==150&&result.vm_percent==0);
  // Do not double-count the GPU when presentation shares the VM thread.
  window.Reset(start);end.graphics_tid=11;
  assert(window.Read(end,result)&&result.graphics_percent==-1&&result.other_percent==-1);
  // A missing/restarted worker cannot be interpreted as zero worker CPU usage.
  window.Reset(start);end.graphics_tid=23;
  assert(window.Read(end,result)&&result.graphics_percent==-1);
  window.Reset(start);end.graphics_tid=22;end.graphics_valid=false;
  assert(window.Read(end,result)&&result.graphics_percent==-1);
  // Invalid counters or a clock rollback discard the window; reset excludes pause.
  window.Reset(start);end.vm=0;
  assert(!window.Read(end,result));
  window.Reset(start);end=start;end.wall--;
  assert(!window.Read(end,result));
  window.Reset({});assert(!window.Read(start,result));
  window.Reset(start);end=start;end.wall+=2000000000;
  assert(window.Read(end,result)&&result.process_percent==0&&result.vm_percent==0);
  std::cout<<"CPU diagnostic units, multi-core totals, unavailable workers, pause/reset and clock regression passed\n";
}
