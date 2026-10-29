// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdint>
namespace SwanFramePacing {
// One graphics owner records driver feedback; diagnostics can read atomics from
// another thread. Pause/resume starts a new epoch so menu time is not frame jank.
class Feedback {
  uint64_t epoch_=0,previous_actual_=0,previous_desired_=0;
  uint32_t previous_id_=0;
  bool have_previous_=false;
  std::atomic<uint64_t> last_{0},frames_{0},gaps_{0},sum_{0},maximum_{0},late_{0};
  std::atomic<uint64_t> missing_{0},coalesced_{0};
public:
  struct Snapshot {uint64_t last,frames,gaps,sum,maximum,late,missing,coalesced;};
  void Reset(uint64_t epoch) {
    epoch_=epoch;previous_actual_=previous_desired_=0;
    previous_id_=0;have_previous_=false;
    last_=frames_=gaps_=sum_=maximum_=late_=0;
    missing_=coalesced_=0;
  }
  void Record(uint32_t id,uint64_t actual,uint64_t desired,uint64_t refresh) {
    if(!actual||actual<epoch_)return;
    const uint32_t advance=id-previous_id_; // modulo arithmetic covers ID wrap
    if(have_previous_&&(!advance||advance>=0x80000000u))return;
    // A history gap is unknown feedback, not proof of a long displayed frame.
    // Android may discard old timing records before the next query.
    if(have_previous_&&advance>1)missing_+=advance-1;
    if(have_previous_&&actual<=previous_actual_) {
      ++coalesced_;previous_id_=id;previous_desired_=desired;++frames_;
      return;
    }
    if(have_previous_&&advance==1) {
      const auto gap=actual-previous_actual_;
      sum_+=gap;++gaps_;maximum_=std::max(maximum_.load(),gap);
      if(refresh&&previous_desired_&&desired>previous_desired_&&gap>desired-previous_desired_+refresh/2)++late_;
    }
    previous_id_=id;have_previous_=true;
    previous_actual_=actual;previous_desired_=desired;last_=actual;++frames_;
  }
  uint64_t Last() const {return last_.load();}
  Snapshot Read() const {return {last_.load(),frames_.load(),gaps_.load(),sum_.load(),maximum_.load(),late_.load(),missing_.load(),coalesced_.load()};}
};
}
