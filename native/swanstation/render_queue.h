// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "render_task.h"
#include <array>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace SwanRender {
// Single ordered driver stream. The bounded queue prevents unlimited CPU lead
// and owns every command until execution. Start/Stop belong to the session owner.
class Queue {
public:
  explicit Queue(size_t capacity = 256, size_t byte_capacity = 16 * 1024 * 1024)
      : capacity_(capacity), byte_capacity_(byte_capacity), jobs_(capacity) {
    if (!capacity || !byte_capacity) throw std::invalid_argument("Zero rendering queue capacity");
  }
  ~Queue() { Stop(); }
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  bool Start(std::function<bool()> initialize = {}) {
    if (thread_.joinable()) return false;
    std::promise<bool> ready;
    auto result = ready.get_future();
    {
      std::lock_guard lock(mutex_);
      closing_ = false;
      peak_ = peak_bytes_ = head_ = tail_ = queued_ = 0;
      pending_ = bytes_ = 0;
      submitted_ = waits_ = blocked_ = worker_notifications_ = producer_notifications_ = 0;
      consumer_batches_ = peak_batch_ = heap_tasks_ = 0;
      waiting_ = false;
    }
    thread_ = std::thread([this, initialize = std::move(initialize), ready = std::move(ready)]() mutable {
      current_ = this;
      bool initialized = false;
      try { initialized = !initialize || initialize(); } catch (...) { initialized = false; }
      if (!initialized) { ready.set_value(false); current_ = nullptr; return; }
      active_.store(true, std::memory_order_release);
      ready.set_value(true);
      std::array<Job, 16> batch;
      for (;;) {
        size_t count = 0;
        {
          std::unique_lock lock(mutex_);
          waiting_ = true;
          work_.wait(lock, [this] { return closing_ || queued_ != 0; });
          waiting_ = false;
          if (!queued_ && closing_) break;
          while (queued_ && count < batch.size()) {
            batch[count++] = std::move(jobs_[head_]);
            head_ = (head_ + 1) % capacity_;
            --queued_;
          }
          ++consumer_batches_;
          if (count > peak_batch_) peak_batch_ = count;
        }
        for (size_t i = 0; i < count; ++i) {
          // Prefetched commands still consume both budgets until execution.
          // Retire the charge just before execution, as in the unbatched queue.
          bytes_.fetch_sub(batch[i].bytes);
          pending_.fetch_sub(1);
          if (blocked_.load() != 0) {
            // Pair with the producer's predicate/wait transition. Without this
            // handshake a capacity notification could arrive before it waits.
            std::lock_guard lock(mutex_);
            ++producer_notifications_;
            space_.notify_all();
          }
          batch[i].run();
          batch[i].run.Reset();
        }
      }
      current_ = nullptr;
    });
    if (result.get()) return true;
    thread_.join();
    return false;
  }

  bool Active() const { return active_.load(std::memory_order_acquire); }
  bool IsWorker() const { return current_ == this; }

  template<class F> void Push(F&& function, size_t bytes = 1) {
    if (bytes > byte_capacity_) throw std::length_error("Oversized rendering queue packet");
    if (IsWorker()) { function(); return; }
    Task<128> job(std::forward<F>(function));
    std::unique_lock lock(mutex_);
    if (!Active() || closing_) throw std::runtime_error("Rendering queue is closed");
    if (pending_.load() >= capacity_ || bytes > byte_capacity_ - bytes_.load()) {
      ++waits_; ++blocked_;
      try {
        space_.wait(lock, [this, bytes] { return closing_ ||
          (pending_.load() < capacity_ && bytes <= byte_capacity_ - bytes_.load()); });
      } catch (...) { --blocked_; throw; }
      --blocked_;
    }
    if (closing_) throw std::runtime_error("Rendering queue is closing");
    const bool wake_worker = waiting_ && !queued_;
    if (job.UsesHeap()) ++heap_tasks_;
    jobs_[tail_] = {std::move(job), bytes};
    tail_ = (tail_ + 1) % capacity_;
    ++queued_;
    const auto pending = pending_.fetch_add(1) + 1;
    const auto queued_bytes = bytes_.fetch_add(bytes) + bytes;
    if (queued_bytes > peak_bytes_) peak_bytes_ = queued_bytes;
    ++submitted_;
    if (pending > peak_) peak_ = pending;
    if (wake_worker) ++worker_notifications_;
    lock.unlock();
    // A nonempty queue already has a running or signalled consumer. Its next
    // predicate check sees queued work without another condition-variable wake.
    if (wake_worker) work_.notify_one();
  }

  template<class F> auto Call(F function, size_t bytes = 1) -> std::invoke_result_t<F> {
    if (!Active() || IsWorker()) return function();
    auto task = std::make_shared<std::packaged_task<std::invoke_result_t<F>()>>(std::move(function));
    auto result = task->get_future();
    Push([task] { (*task)(); }, bytes);
    return result.get();
  }
  void Flush() { Call([] {}); }

  void Stop() {
    if (!thread_.joinable()) return;
    // Session ownership prevents a renderer job from destroying its own worker.
    if (IsWorker()) std::terminate();
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    work_.notify_all();
    space_.notify_all();
    thread_.join();
    active_.store(false, std::memory_order_release);
  }

  struct Metrics {
    size_t submitted, waits, peak, peak_bytes, worker_notifications, producer_notifications;
    size_t consumer_batches, peak_batch, heap_tasks;
  };
  void ResetMetrics() {
    std::lock_guard lock(mutex_);
    if (!Active()) submitted_ = waits_ = peak_ = peak_bytes_ =
      worker_notifications_ = producer_notifications_ = consumer_batches_ = peak_batch_ = heap_tasks_ = 0;
  }
  Metrics ReadMetrics() {
    std::lock_guard lock(mutex_);
    return {submitted_, waits_, peak_, peak_bytes_, worker_notifications_, producer_notifications_,
            consumer_batches_, peak_batch_, heap_tasks_};
  }
private:
  const size_t capacity_;
  const size_t byte_capacity_;
  inline static thread_local Queue* current_ = nullptr;
  std::atomic<bool> active_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable work_, space_;
  struct Job { Task<128> run; size_t bytes = 0; };
  std::vector<Job> jobs_;
  bool closing_ = true;
  bool waiting_ = false;
  size_t head_ = 0, tail_ = 0, queued_ = 0;
  size_t submitted_ = 0, waits_ = 0, peak_ = 0;
  std::atomic<size_t> pending_{0}, bytes_{0}, blocked_{0};
  size_t peak_bytes_ = 0, worker_notifications_ = 0, producer_notifications_ = 0;
  size_t consumer_batches_ = 0, peak_batch_ = 0, heap_tasks_ = 0;
};
} // namespace SwanRender
