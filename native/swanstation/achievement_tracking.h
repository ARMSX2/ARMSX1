// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string_view>

namespace SwanAchievements {
// Disabling is immediate; re-enabling a test session cannot credit its modified memory.
class TrackingPolicy {
 public:
  void Enabled(bool value) { if(enabled_!=value)++generation_;enabled_=value;if(!value)eligible_=false; }
  void Begin() { running_=true;eligible_=enabled_; }
  void End() { running_=eligible_=false; }
  bool Enabled() const { return enabled_; }
  bool Active() const { return enabled_ && running_ && eligible_; }
  bool RestartRequired() const { return enabled_ && running_ && !eligible_; }
  uint64_t Generation() const { return generation_; }
  // For a request admitted while Active(). Normal shutdown must not lose a legitimate
  // last-frame award, but an explicit Off invalidates every previously queued submission.
  bool CanDispatch(uint64_t generation) const { return enabled_ && generation==generation_; }
 private:
  bool enabled_=true,running_=false,eligible_=false;
  uint64_t generation_=0;
};
inline bool IsSubmission(std::string_view data) {
  size_t at=0;
  while((at=data.find("r=",at))!=std::string_view::npos) {
    if(at==0 || data[at-1]=='&' || data[at-1]=='?') {
      const size_t start=at+2,end=data.find('&',start);
      const auto action=data.substr(start,end==std::string_view::npos?end:end-start);
      return action=="awardachievement" || action=="submitlbentry" || action=="ping" || action=="startsession";
    }
    at+=2;
  }
  return false;
}
}
