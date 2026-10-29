// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/pacing_metrics.h"
#include <cassert>
#include <limits>
int main(){
  SwanPacing::Series series;SwanPacing::Cadence clock;
  assert(series.Size()==0&&series.Text("display").find("unavailable")!=std::string::npos);
  clock.Add(-2,series);clock.Add(-1,series);assert(series.Size()==0);
  clock.Add(1000000000,series);clock.Add(1016666667,series);clock.Add(1050000000,series);
  assert(series.Size()==2&&std::abs(series.Percentile(.5)-16.666667)<.001);
  assert(std::abs(series.Percentile(.99)-33.333333)<.001);
  clock.Reset();clock.Add(10000000000,series);assert(series.Size()==2); // pause gap excluded
  clock.Add(9000000000,series);assert(series.Size()==2); // out-of-order samples excluded
  series.Add(std::numeric_limits<double>::quiet_NaN());series.Add(-1);assert(series.Size()==2);
  clock.Add(10016666667,series);assert(series.Size()==3&&series.Percentile(1)<34);
  for(int i=0;i<10000;i++)series.Add(1);assert(series.Size()==4096);
  series.Clear();assert(series.Size()==0);
  assert(SwanPacing::SwapInterval(0,0,true,1)==0);
  assert(SwanPacing::SwapInterval(0,1,true,1)==1);
  assert(SwanPacing::SwapInterval(0,1,false,1)==0); // fast-forward / rewind
  assert(SwanPacing::SwapInterval(1,1,true,2)==1); // do not alter BFI/subframe path
  assert(SwanPacing::SwapInterval(0,99,true,1)==0);
}
