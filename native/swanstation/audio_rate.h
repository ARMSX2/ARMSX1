// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>
namespace SwanAudio {
// Streaming stereo resampling for the <=1% host-refresh adjustment. State spans
// core audio batches; only the producer uses it, under the existing audio mutex.
class RefreshResampler {
  bool have_=false;
  std::array<int16_t,2> previous_{};
  double next_=0;
  std::vector<int16_t> output_;
public:
  void Reset() {have_=false;next_=0;output_.clear();}
  const std::vector<int16_t>& Process(const int16_t* input,size_t frames,double ratio) {
    if(!std::isfinite(ratio))ratio=1;
    ratio=std::clamp(ratio,0.99,1.01);
    output_.clear();output_.reserve(size_t(frames*2/0.99)+4);
    for(size_t i=0;i<frames;++i) {
      const std::array<int16_t,2> current{input[i*2],input[i*2+1]};
      if(!have_) {previous_=current;have_=true;output_.insert(output_.end(),current.begin(),current.end());next_=ratio;continue;}
      while(next_<=1.0) {
        for(size_t c=0;c<2;++c)output_.push_back(int16_t(std::lround(previous_[c]+(double(current[c])-previous_[c])*next_)));
        next_+=ratio;
      }
      next_-=1;previous_=current;
    }
    return output_;
  }
};
}
