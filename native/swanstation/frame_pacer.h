// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
namespace SwanFramePacing {
struct Options {
  bool optimal=true, vsync=true, match_refresh=false, pre_sleep=false;
  double buffer_ms=2;
};
inline double RefreshRatio(double game,double display,bool enabled) {
  if(!enabled || !std::isfinite(game) || !std::isfinite(display) || game<40 || game>75 || display<30 || display>500)return 1;
  const double multiple=std::max(1.0,std::round(display/game));
  const double ratio=display/(game*multiple);
  return std::abs(ratio-1)<=0.01?ratio:1;
}
// present is the CPU submission deadline; display is the compositor target.
// Changing headroom must change the former without moving the latter.
struct Plan {int64_t start=0,present=0,display=0,finish=0,period=0;double ratio=1;};
inline uint64_t SafeTimestamp(int64_t target,int64_t now) {
  return target>0 && now>0 && target>=now-250000000 && target<=now+250000000?uint64_t(target):0;
}
// A presentation timestamp is a "not before" request, not an exact scanout
// appointment. When the slot is anchored to actual scanout feedback, request
// between the preceding refresh and this slot. Small phase-estimation errors
// then cannot move the request across the intended refresh boundary.
inline uint64_t DisplaySlotTimestamp(int64_t slot,int64_t now,int64_t refresh) {
  if(slot<=0)return 0;
  if(refresh>=2000000&&refresh<=40000000)slot-=refresh/2;
  return SafeTimestamp(slot,now);
}
class Scheduler {
  int64_t next_=0,last_period_=0;
  double work_ns_=2000000;
public:
  void Reset() {next_=last_period_=0;work_ns_=2000000;}
  Plan Next(int64_t now,double game,double display,double speed,bool limited,bool normal,
            int subframes,int64_t vsync,const Options& options) {
    Plan out;
    if(!limited || !std::isfinite(speed) || speed<=0 || !std::isfinite(game) || game<=0) {Reset();return out;}
    out.ratio=RefreshRatio(game,display,normal&&options.vsync&&options.match_refresh);
    out.period=int64_t(1e9/(game*speed*out.ratio));
    if(out.period<=0){Reset();return {};}
    // Only rebase for a real discontinuity. Display quantization can legitimately
    // finish later than the ideal emulation deadline; rebasing for that lateness
    // loses emulated time every beat and drains the audio queue.
    if(!next_ || std::abs(out.period-last_period_)>10000 || next_<now-out.period || next_>now+out.period*2)
      next_=now+out.period;
    out.finish=next_;
    out.present=next_;
    if(options.optimal && options.vsync && normal && vsync>0 && now>=vsync && now-vsync<500000000 && display>=30 && display<=500) {
      const double tick=1e9/display;
      // Quantize only the presentation slot. The emulated clock keeps its own
      // cadence, so 50 Hz content on 60 Hz follows pulldown instead of speeding up.
      // Nearest slot gives bounded phase error. Always rounding UP compounds
      // the game/display mismatch, especially 59.817/60 and PAL 50/60.
      const double ticks=std::round(double(next_-vsync)/tick);
      out.present=vsync+int64_t(std::max(1.0,ticks)*tick);
    }
    const auto headroom=int64_t(std::clamp(options.buffer_ms,0.0,10.0)*1e6);
    const auto lead=options.vsync?headroom:0;
    if(options.optimal) {
      if(subframes>1)out.present-=out.period-out.period/subframes;
      if(options.vsync&&normal)out.display=out.present;
      out.present-=lead;
      // Subframes occupy successive refreshes ending at this frame's target.
      out.present=std::max(now,out.present);
      if(options.pre_sleep && normal && subframes==1)
        out.start=std::max(now,out.present-int64_t(work_ns_)-(options.vsync?0:headroom));
    } else out.present=0;
    next_+=out.period;last_period_=out.period;
    return out;
  }
  void Observe(double useful_ns) {
    if(!std::isfinite(useful_ns)||useful_ns<=0)return;
    // Fast rise and slow decay allow compilation spikes without permanently
    // sacrificing latency. The user buffer adds scheduling headroom separately.
    work_ns_=std::clamp(std::max(useful_ns,work_ns_*0.95),100000.0,100000000.0);
  }
};
}
