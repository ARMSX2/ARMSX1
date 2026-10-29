// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/render_task.h"
#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
using SwanRender::Task;
struct alignas(64) Aligned {
  int* count;
  void operator()() { assert(reinterpret_cast<uintptr_t>(this)%64==0); ++*count; }
};
struct Large { char bytes[256]{}; int* count; void operator()() { ++*count; } };
struct ThrowingMove {
  int* count;
  explicit ThrowingMove(int* p):count(p) {}
  ThrowingMove(const ThrowingMove&)=default;
  ThrowingMove(ThrowingMove&&) { throw std::runtime_error("move"); }
  void operator()() { ++*count; }
};
int main() {
  int count=0;
  Task<128> empty;
  try { empty(); assert(false); } catch(const std::bad_function_call&) {}
  auto owned=std::make_shared<int>(7);
  std::weak_ptr<int> lifetime=owned;
  Task<128> task([owned=std::move(owned),&count]{count+=*owned;});
  assert(!task.UsesHeap());
  Task<128> moved(std::move(task)); assert(!task && moved);
  moved(); assert(count==7);
  empty=std::move(moved); assert(!moved && empty);
  empty.Reset(); assert(lifetime.expired());
  Task<128> unique([value=std::make_unique<int>(2),&count]{count+=*value;});
  assert(!unique.UsesHeap()); unique(); assert(count==9);
  Task<128> aligned(Aligned{&count}); assert(aligned.UsesHeap());
  Task<128> large(Large{{},&count}); assert(large.UsesHeap());
  aligned(); large(); assert(count==11);
  ThrowingMove function(&count);
  Task<128> fallback(function); assert(fallback.UsesHeap());
  Task<128> heap_moved(std::move(fallback)); assert(!fallback); heap_moved();
  assert(count==12);
  try { Task<128> bad(std::move(function)); assert(false); } catch(const std::runtime_error&) {}
  // Replacing a live task releases its payload once, including heap fallback.
  owned=std::make_shared<int>(1); lifetime=owned;
  Task<128> replacement([owned=std::move(owned)]{});
  replacement=std::move(large); assert(lifetime.expired()); replacement();
  replacement=std::move(empty); assert(!replacement);
  std::cout<<"Inline and fallback command ownership, alignment and exception safety passed\n";
}
