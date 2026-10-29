// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <deque>
namespace SwanFramePacing {
struct PresentRequest {
  int64_t target=0,deadline=0,refresh=0;
  double base_buffer_ms=2;
  bool enabled=false;
  bool allow_drop=true; // multiple display refreshes per emulated frame already provide recovery slack
};
// All mutation belongs to the graphics owner. Only the atomic metrics/phase are
// read by the CPU scheduler. This never changes guest timing or drops GPU work.
class PresentRecovery {
  struct Pending {uint64_t id;PresentRequest request;int64_t submitted;};
  std::deque<Pending> pending_;
  bool enabled_=false,last_dropped_=false,armed_=false;
  unsigned consecutive_late_=0;
  int64_t epoch_=0,refresh_=0,last_end_=0,last_drop_=0;
  double base_=2;
  std::atomic<int64_t> extra_{0},actual_{0};
  std::atomic<uint64_t> late_{0},adjustments_{0},drops_{0};
public:
  void Reset(int64_t epoch=0) {
    pending_.clear();enabled_=last_dropped_=armed_=false;consecutive_late_=0;
    epoch_=epoch;refresh_=last_end_=last_drop_=0;base_=2;
    extra_=actual_=0;late_=adjustments_=drops_=0;
  }
  void Policy(const PresentRequest& request) {
    const bool enabled=request.enabled&&request.target>0&&request.deadline>0&&
      request.refresh>=2000000&&request.refresh<=40000000;
    if(enabled!=enabled_||std::abs(request.refresh-refresh_)>10000||request.base_buffer_ms!=base_) {
      pending_.clear();extra_=0;last_end_=last_drop_=0;
      last_dropped_=armed_=false;consecutive_late_=0;
    }
    enabled_=enabled;refresh_=request.refresh;base_=request.base_buffer_ms;
  }
  bool Skip(const PresentRequest& request,int64_t now) {
    Policy(request);
    if(!enabled_||!request.allow_drop)return false;
    const bool late_packet=now>request.deadline+request.refresh/16&&
      (last_end_>=request.deadline||now>request.target+request.refresh/2);
    const bool can_drop=!last_dropped_&&(!last_drop_||request.target-last_drop_>=request.refresh*8);
    if((armed_||late_packet)&&can_drop) {
      ++drops_;last_drop_=request.target;last_dropped_=true;
      last_end_=0;armed_=false;consecutive_late_=0;
      // Feedback for pre-recovery presents cannot trigger another recovery.
      pending_.clear();return true;
    }
    last_dropped_=false;return false;
  }
  void Submitted(uint64_t id,const PresentRequest& request,int64_t now) {
    pending_.push_back({id,request,now});
    if(pending_.size()>128)pending_.pop_front();
  }
  void Presented(const PresentRequest& request,int64_t now) {if(request.enabled&&enabled_)last_end_=now;}
  void Observe(uint64_t id,int64_t actual) {
    if(actual<=0||actual<epoch_)return;
    if(actual>actual_.load())actual_=actual;
    auto it=std::find_if(pending_.begin(),pending_.end(),[&](const auto& p){return p.id==id;});
    if(it==pending_.end())return;
    const auto sample=*it;pending_.erase(it);
    const auto& request=sample.request;
    if(!enabled_||!request.enabled||request.refresh!=refresh_||request.target<=last_drop_)return;
    if(actual-request.target<=request.refresh/2){consecutive_late_=0;armed_=false;return;}
    ++late_;armed_=++consecutive_late_>=2;
    // Earlier handoff only helps frames submitted on time. Shader/CPU overruns
    // are not evidence that a larger presentation margin would have helped.
    if(sample.submitted>request.deadline+500000)return;
    const auto base=int64_t(std::clamp(request.base_buffer_ms,0.0,10.0)*1e6);
    const auto limit=std::max<int64_t>(0,request.refresh/4-base);
    const auto next=std::min(limit,extra_.load()+request.refresh/32);
    if(next>extra_.load()){extra_=next;++adjustments_;}
  }
  int64_t ExtraMargin() const {return extra_.load();}
  int64_t LastActual() const {return actual_.load();}
  uint64_t Late() const {return late_.load();}
  uint64_t Adjustments() const {return adjustments_.load();}
  uint64_t Drops() const {return drops_.load();}
};
inline PresentRecovery& PresentationRecovery(){static PresentRecovery value;return value;}
}
