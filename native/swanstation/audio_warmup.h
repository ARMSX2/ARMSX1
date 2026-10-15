// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace SwanAudio {
// Stereo FRAMES, not individual samples. Called under the audio queue's existing mutex.
// No resampling, pitch changes, allocation or per-frame clock adjustments.
class Warmup {
 public:
  void Configure(unsigned sample_rate,int buffer_ms,double frame_rate,unsigned burst_frames) {
    const unsigned rate=sample_rate?sample_rate:44100;
    const auto requested=size_t(std::ceil(rate*std::clamp(buffer_ms,20,100)/1000.0));
    const double fps=std::isfinite(frame_rate)&&frame_rate>0?frame_rate:50.0;
    // Two producer frames plus one platform burst: a one-frame reserve still underruns when
    // BIOS rendering/menus delay a batch beyond a single emulated frame on Android.
    const auto cadence=size_t(std::min(double(rate)/4.0,2*std::ceil(rate/fps)+burst_frames));
    target_=std::max(requested,cadence);waiting_=true;
  }
  bool Ready(size_t queued_frames,bool muted) {
    if(muted){waiting_=true;return false;}
    if(waiting_ && queued_frames<target_)return false;
    waiting_=false;return true;
  }
  void Reset(){waiting_=true;}
  size_t Target() const {return target_;}
 private:
  size_t target_=1764;
  bool waiting_=true;
};
}
