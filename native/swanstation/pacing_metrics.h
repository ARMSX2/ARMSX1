// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
namespace SwanPacing {
class Series {
 public:
  void Add(double ms){if(std::isfinite(ms)&&ms>=0&&values.size()<4096)values.push_back(ms);}
  void Clear(){values.clear();}
  size_t Size() const{return values.size();}
  double Percentile(double p) const {
    if(values.empty())return 0;
    auto sorted=values;std::sort(sorted.begin(),sorted.end());
    const auto index=static_cast<size_t>(std::ceil(std::clamp(p,0.0,1.0)*sorted.size()));
    return sorted[index?index-1:0];
  }
  std::string Text(const char* label) const {
    std::ostringstream out;out<<label<<"_ms(n/p50/p95/p99/max)=";
    if(values.empty())return out.str()+"unavailable";
    out<<std::fixed<<std::setprecision(3)<<values.size()<<"/"<<Percentile(.5)<<"/"
      <<Percentile(.95)<<"/"<<Percentile(.99)<<"/"<<Percentile(1);return out.str();
  }
 private:std::vector<double> values;
};
class Cadence {
 public:
  void Reset(){last=0;}
  bool Add(int64_t ns,Series& series){
    if(ns<=0)return false;
    if(last&&ns<=last)return false;
    if(last)series.Add(double(ns-last)/1e6);
    last=ns;return true;
  }
 private:int64_t last=0;
};
inline int SwapInterval(int baseline,int requested,bool normal_speed,unsigned subframes){
  return requested==1&&normal_speed&&subframes==1?1:baseline;
}
}
