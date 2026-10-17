// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace SwanAudio {
// Counts stereo FRAMES throughout. Storage capacity must never become a latency target.
// The caller serializes Configure/Push/Read/Clear using the audio mutex.
template<size_t CapacityFrames = 65536>
class Queue {
  static_assert(CapacityFrames > 0);
 public:
  void Configure(size_t target_frames) {
    target_ = std::clamp(target_frames, size_t(1), CapacityFrames);
    limit_ = target_ + std::min(target_, CapacityFrames - target_);
    dropped_ = trims_ = underruns_ = 0;
    Clear();
  }
  void Clear() {
    head_ = size_ = fade_remaining_ = 0;
    last_ = {};
    have_last_ = false;
  }
  void Push(const int16_t* input, size_t frames) {
    if (!frames) return;
    size_t skip = 0;
    if (frames > limit_) {
      // An oversized batch must retain its newest tail, not the oldest samples.
      skip = frames - target_;
      dropped_ += size_ + skip;
      head_ = size_ = 0;
      frames = target_;
      BeginTrim();
    } else if (size_ > limit_ - frames) {
      const size_t drop = size_ + frames - target_;
      const size_t queued_drop = std::min(size_, drop);
      head_ = (head_ + queued_drop) % CapacityFrames;
      size_ -= queued_drop;
      skip = drop - queued_drop;
      frames -= skip;
      dropped_ += drop;
      BeginTrim();
    }
    input += skip * 2;
    const size_t tail = (head_ + size_) % CapacityFrames;
    const size_t first = std::min(frames, CapacityFrames - tail);
    std::copy_n(input, first * 2, pcm_.data() + tail * 2);
    std::copy_n(input + first * 2, (frames - first) * 2, pcm_.data());
    size_ += frames;
  }
  size_t Read(int16_t* output, size_t frames) {
    const size_t copied = std::min(frames, size_);
    if (copied < frames) ++underruns_;
    for (size_t i = 0; i < copied; ++i) {
      for (size_t channel = 0; channel < 2; ++channel) {
        int32_t value = pcm_[head_ * 2 + channel];
        if (fade_remaining_) {
          // A short seam avoids a hard waveform jump when stale PCM is dropped.
          const int old_weight = static_cast<int>(fade_remaining_);
          value = (int32_t(fade_from_[channel]) * old_weight +
                   value * (65 - old_weight)) / 65;
        }
        output[i * 2 + channel] = last_[channel] = static_cast<int16_t>(value);
      }
      if (fade_remaining_) --fade_remaining_;
      head_ = (head_ + 1) % CapacityFrames;
    }
    size_ -= copied;
    std::fill_n(output + copied * 2, (frames - copied) * 2, int16_t(0));
    if (copied) have_last_ = true;
    if (copied < frames) { last_ = {}; have_last_ = true; fade_remaining_ = 0; }
    return copied;
  }
  size_t Size() const { return size_; }
  size_t Target() const { return target_; }
  size_t Limit() const { return limit_; }
  uint64_t Dropped() const { return dropped_; }
  uint64_t Trims() const { return trims_; }
  uint64_t Underruns() const { return underruns_; }
 private:
  void BeginTrim() {
    ++trims_;
    fade_from_ = last_;
    fade_remaining_ = have_last_ ? 64 : 0;
  }
  std::array<int16_t, CapacityFrames * 2> pcm_{};
  std::array<int16_t, 2> last_{}, fade_from_{};
  size_t head_ = 0, size_ = 0, target_ = std::min(size_t(1764), CapacityFrames),
         limit_ = std::min(size_t(3528), CapacityFrames), fade_remaining_ = 0;
  uint64_t dropped_ = 0, trims_ = 0, underruns_ = 0;
  bool have_last_ = false;
};
}
