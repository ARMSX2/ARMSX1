// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
namespace SwanRender {
// Counts whole frames, including the one executing. Each accepted frame owns a
// ticket until its presentation job is destroyed, even after a driver exception
// or rejected dispatch. This bound is separate from command/byte backpressure.
class FrameQueue {
  mutable std::mutex mutex_;
  std::condition_variable space_;
  unsigned limit_=2,pending_=0,peak_=0;
  void Complete() {std::lock_guard lock(mutex_);--pending_;space_.notify_all();}
public:
  struct Ticket {
    FrameQueue* owner=nullptr;
    Ticket()=default;
    Ticket(const Ticket&)=delete;
    Ticket& operator=(const Ticket&)=delete;
    ~Ticket(){if(owner)owner->Complete();}
  };
  void Reset(unsigned limit) {
    std::lock_guard lock(mutex_);
    if(pending_)throw std::logic_error("Frame queue reset before drain");
    limit_=std::clamp(limit,1u,3u);peak_=0;
  }
  std::shared_ptr<Ticket> Acquire() {
    auto ticket=std::make_shared<Ticket>();
    std::unique_lock lock(mutex_);
    space_.wait(lock,[&]{return pending_<limit_;});
    ++pending_;peak_=std::max(peak_,pending_);ticket->owner=this;
    return ticket;
  }
  unsigned Pending() const {std::lock_guard lock(mutex_);return pending_;}
  unsigned Peak() const {std::lock_guard lock(mutex_);return peak_;}
};
}
