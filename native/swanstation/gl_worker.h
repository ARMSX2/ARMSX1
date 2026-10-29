// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "render_queue.h"

namespace SwanGLWorker {
SwanRender::Queue& Commands();
void* Procedure(const char* name, void* real);

// Driver calls returning data, or borrowing memory, are barriers. Only void
// calls whose arguments are copied scalar values may run asynchronously.
template<class Tag, class Function> struct Proxy;
template<class Tag, class Result, class... Args>
struct Proxy<Tag, Result(*)(Args...)> {
  inline static Result(*real)(Args...) = nullptr;
  static Result Run(Args... args) {
    const auto function = real;
    auto& queue = Commands();
    if (!queue.Active() || queue.IsWorker()) return function(args...);
    if constexpr (std::is_void_v<Result> && !(std::is_pointer_v<Args> || ...)) {
      queue.Push([=] { function(args...); });
    } else {
      return queue.Call([=] { return function(args...); });
    }
  }
};
template<class Tag, class Function> Function Wrap(Function real) {
  Proxy<Tag, Function>::real = real;
  return real ? &Proxy<Tag, Function>::Run : nullptr;
}
} // namespace SwanGLWorker
