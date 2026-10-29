// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace SwanRender {
// Native-owned value captured when a core packet is dispatched. No pointers to
// the producer's next frame, settings objects or reusable buffers cross threads.
struct FrameContext {
  bool valid=false,limited=false,normal=false;
  int64_t present=0,display=0;
  float rate=60,aspect=4.f/3;
  int mode=0,multiple=2;
};
class FrameScope {
  FrameContext& target_;
  FrameContext previous_;
public:
  FrameScope(FrameContext& target,FrameContext value):target_(target),previous_(target){target_=value;}
  ~FrameScope(){target_=previous_;}
  FrameScope(const FrameScope&)=delete;
  FrameScope& operator=(const FrameScope&)=delete;
};
}
