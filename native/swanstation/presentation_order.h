// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
namespace SwanFramePacing {
// Queue GPU work while there is still time to render. Only handing the prepared
// buffer to the display waits for the deadline. A failed submit cannot present.
template<class Submit,class Wait,class Present>
bool PresentPreparedFrame(Submit submit,Wait wait,Present present) {
  if(!submit())return false;
  wait();
  return present();
}
enum class PresentMode { Fifo,Mailbox,Immediate };
inline PresentMode ChoosePresentMode(bool vsync,bool matched,bool mailbox,bool immediate) {
  if(vsync&&matched)return PresentMode::Fifo;
  if(mailbox)return PresentMode::Mailbox;
  if(!vsync&&immediate)return PresentMode::Immediate;
  return PresentMode::Fifo;
}
inline bool MatchedCadence(double game,double display) {
  if(!std::isfinite(game)||!std::isfinite(display)||game<=0||display<=0)return false;
  const double multiple=std::max(1.0,std::round(display/game));
  return std::abs(display-game*multiple)<=display*0.0001;
}
}
