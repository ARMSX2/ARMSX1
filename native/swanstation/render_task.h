// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

namespace SwanRender {
// Owning, move-only command. Small captures travel with their queue slot rather
// than requiring a second allocation. Unusual captures keep a safe heap path.
template<size_t Capacity> class Task {
  struct Operations {
    void (*invoke)(void*);
    void (*move)(void*, void*) noexcept;
    void (*destroy)(void*) noexcept;
    bool heap;
  };
  alignas(std::max_align_t) unsigned char storage_[Capacity];
  const Operations* operations_ = nullptr;
  template<class F> static F*& Pointer(void* data) {
    return *std::launder(reinterpret_cast<F**>(data));
  }
public:
  static_assert(Capacity >= sizeof(void*));
  Task() noexcept = default;
  template<class F, class T = std::decay_t<F>,
           std::enable_if_t<!std::is_same_v<T, Task>, int> = 0>
  Task(F&& function) {
    if constexpr (sizeof(T) <= Capacity && alignof(T) <= alignof(std::max_align_t) &&
                  std::is_nothrow_move_constructible_v<T>) {
      static const Operations ops{
        [](void* p) { (*std::launder(reinterpret_cast<T*>(p)))(); },
        [](void* from, void* to) noexcept {
          auto* value = std::launder(reinterpret_cast<T*>(from));
          new (to) T(std::move(*value)); value->~T();
        },
        [](void* p) noexcept { std::launder(reinterpret_cast<T*>(p))->~T(); }, false};
      new (storage_) T(std::forward<F>(function));
      operations_ = &ops;
    } else {
      static const Operations ops{
        [](void* p) { (*Pointer<T>(p))(); },
        [](void* from, void* to) noexcept {
          new (to) T*(Pointer<T>(from)); Pointer<T>(from) = nullptr;
        },
        [](void* p) noexcept { delete Pointer<T>(p); }, true};
      new (storage_) T*(new T(std::forward<F>(function)));
      operations_ = &ops;
    }
  }
  ~Task() { Reset(); }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  Task(Task&& other) noexcept { *this = std::move(other); }
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      Reset();
      if (other.operations_) {
        other.operations_->move(other.storage_, storage_);
        operations_ = std::exchange(other.operations_, nullptr);
      }
    }
    return *this;
  }
  void Reset() noexcept {
    if (operations_) { operations_->destroy(storage_); operations_ = nullptr; }
  }
  explicit operator bool() const noexcept { return operations_ != nullptr; }
  bool UsesHeap() const noexcept { return operations_ && operations_->heap; }
  void operator()() {
    if (!operations_) throw std::bad_function_call();
    operations_->invoke(storage_);
  }
};
} // namespace SwanRender
