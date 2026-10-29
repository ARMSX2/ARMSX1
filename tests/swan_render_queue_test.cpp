// SPDX-License-Identifier: GPL-3.0-or-later
#include "../native/swanstation/gl_worker.h"
#include "../native/swanstation/performance_options.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <vector>
using namespace std::chrono_literals;
namespace SwanGLWorker { SwanRender::Queue& Commands() { static SwanRender::Queue queue(8); return queue; } }
namespace {
std::thread::id owner;
std::vector<int> commands;
void Scalar(int value) { assert(std::this_thread::get_id() == owner); commands.push_back(value); }
int Query() { assert(std::this_thread::get_id() == owner); return int(commands.size()); }
void Borrow(const int* value) { assert(std::this_thread::get_id() == owner); commands.push_back(*value); }
struct ScalarTag {}; struct QueryTag {}; struct BorrowTag {};
}
int main() {
  auto& queue = SwanGLWorker::Commands();
  assert(queue.Start([] { owner = std::this_thread::get_id(); return true; }));
  assert(owner != std::this_thread::get_id());
  auto scalar = SwanGLWorker::Wrap<ScalarTag>(&Scalar);
  auto query = SwanGLWorker::Wrap<QueryTag>(&Query);
  auto borrow = SwanGLWorker::Wrap<BorrowTag>(&Borrow);
  // Ordered scalar commands can overlap the producer without borrowing memory.
  for (int i = 0; i < 50000; ++i) scalar(i);
  assert(query() == 50000);
  for (int i = 0; i < 50000; ++i) assert(commands[size_t(i)] == i);
  int value = 87;
  borrow(&value); // Must finish before the caller can reuse its pointer.
  value = 91;
  assert(commands.back() == 87);
  assert(queue.Call([&] { return queue.Call([] { return 45; }); }) == 45);
  try { queue.Call([]() -> int { throw std::runtime_error("query failed"); }); assert(false); }
  catch (const std::runtime_error&) {}
  assert(query() == 50001); // A failed synchronous job does not kill the worker.
  assert(queue.ReadMetrics().peak <= 8);
  for (int i = 0; i < 100; ++i) scalar(i);
  queue.Stop();
  assert(commands.size() == 50101); // Stop drains all accepted commands.
  assert(!queue.Active());
  owner = std::this_thread::get_id();
  scalar(94); // Disabled mode calls the same driver directly, without a queue.
  assert(commands.back() == 94);
  // Failed initialization must not leave an owner, work item or thread behind.
  assert(!queue.Start([] { return false; }));
  assert(!queue.Active());
  for (int i = 0; i < 20; ++i) { assert(queue.Start()); queue.Flush(); queue.Stop(); }

  SwanRender::Queue bounded(2);
  std::promise<void> entered, release;
  auto allowed = release.get_future().share();
  assert(bounded.Start());
  bounded.Push([&] { entered.set_value(); allowed.wait(); });
  entered.get_future().wait();
  bounded.Push([] {}); bounded.Push([] {});
  auto blocked = std::async(std::launch::async, [&] { bounded.Push([] {}); });
  assert(blocked.wait_for(30ms) == std::future_status::timeout);
  release.set_value(); blocked.get(); bounded.Stop();
  assert(bounded.ReadMetrics().peak == 2);
  assert(bounded.ReadMetrics().waits >= 1);

  // Large owned packets must block on bytes even with empty command slots.
  SwanRender::Queue packets(32, 1024);
  std::promise<void> packet_entered, packet_release;
  auto packet_allowed = packet_release.get_future().share();
  assert(packets.Start());
  packets.Push([&] { packet_entered.set_value(); packet_allowed.wait(); });
  packet_entered.get_future().wait();
  auto payload = std::make_shared<std::vector<int>>(128, 27);
  std::weak_ptr<std::vector<int>> lifetime = payload;
  packets.Push([owned = std::move(payload)] { assert(owned->at(127) == 27); }, 512);
  packets.Push([] {}, 512);
  auto bytes_blocked = std::async(std::launch::async, [&] { packets.Push([] {}, 1); });
  assert(bytes_blocked.wait_for(30ms) == std::future_status::timeout);
  assert(!lifetime.expired());
  try { packets.Push([] {}, 1025); assert(false); } catch (const std::length_error&) {}
  packet_release.set_value(); bytes_blocked.get(); packets.Flush();
  assert(lifetime.expired());
  packets.Stop();
  assert(packets.ReadMetrics().peak_bytes == 1024);
  assert(packets.ReadMetrics().waits >= 1);
  assert(packets.Start()); packets.Stop();
  assert(packets.ReadMetrics().peak_bytes == 0);

  // A running consumer needs no signal for a new burst. With no
  // blocked producers, draining it must not signal the producer condition.
  SwanRender::Queue burst(128);
  std::promise<void> burst_entered,burst_release;
  auto burst_allowed=burst_release.get_future().share();
  assert(burst.Start());
  burst.Push([&]{burst_entered.set_value();burst_allowed.wait();});
  burst_entered.get_future().wait();
  const auto notifications_before=burst.ReadMetrics().worker_notifications;
  for(int i=0;i<100;++i)burst.Push([]{});
  assert(burst.ReadMetrics().worker_notifications==notifications_before);
  burst_release.set_value();burst.Stop();
  assert(burst.ReadMetrics().producer_notifications==0);
  assert(burst.ReadMetrics().peak_batch==16);
  assert(burst.ReadMetrics().consumer_batches < burst.ReadMetrics().submitted);
  assert(burst.ReadMetrics().heap_tasks==0);

  // The local consumer batch is still charged. Prefetch must not turn a
  // four-command/400-byte queue into four extra commands on top of the batch.
  SwanRender::Queue prefetched(4,400);
  std::promise<void> first_entered,first_release,batch_entered,batch_release;
  auto first_allowed=first_release.get_future().share();
  auto batch_allowed=batch_release.get_future().share();
  assert(prefetched.Start());
  prefetched.Push([&]{first_entered.set_value();first_allowed.wait();});
  first_entered.get_future().wait();
  prefetched.Push([&]{batch_entered.set_value();batch_allowed.wait();},100);
  for(int i=0;i<3;++i)prefetched.Push([]{},100);
  first_release.set_value();batch_entered.get_future().wait();
  prefetched.Push([]{},100); // Only the executing command's charge was retired.
  auto prefetch_blocked=std::async(std::launch::async,[&]{prefetched.Push([]{},1);});
  assert(prefetch_blocked.wait_for(30ms)==std::future_status::timeout);
  batch_release.set_value();prefetch_blocked.get();prefetched.Stop();
  assert(prefetched.ReadMetrics().peak==4 && prefetched.ReadMetrics().peak_bytes==400);

  // Concurrent producers wrap the ring repeatedly; each producer's order and
  // move-only payload ownership survive batching and capacity wakeups.
  SwanRender::Queue concurrent(3,3);
  std::array<int,4> expected{};
  assert(concurrent.Start());
  std::vector<std::future<void>> producers;
  for(int p=0;p<4;++p)producers.push_back(std::async(std::launch::async,[&,p]{
    for(int i=0;i<5000;++i)concurrent.Push([&,p,value=std::make_unique<int>(i)]{
      assert(expected[p]++==*value);
    });
  }));
  for(auto& producer:producers)producer.get();
  concurrent.Stop();
  for(int value:expected)assert(value==5000);
  assert(concurrent.ReadMetrics().peak<=3 && concurrent.ReadMetrics().peak_bytes<=3);

  // Stop must drain commands already prefetched by the consumer as well as
  // those still in ring slots, while rejecting a waiting producer.
  SwanRender::Queue local_closing(4);
  std::promise<void> gate_entered,gate_release,local_entered,local_release;
  auto gate_allowed=gate_release.get_future().share();
  auto local_allowed=local_release.get_future().share();
  int drained=0;
  assert(local_closing.Start());
  local_closing.Push([&]{gate_entered.set_value();gate_allowed.wait();});
  gate_entered.get_future().wait();
  local_closing.Push([&]{local_entered.set_value();local_allowed.wait();++drained;});
  for(int i=0;i<3;++i)local_closing.Push([&]{++drained;});
  gate_release.set_value();local_entered.get_future().wait();
  local_closing.Push([&]{++drained;});
  auto local_rejected=std::async(std::launch::async,[&]{
    try {local_closing.Push([]{});return false;}catch(const std::runtime_error&){return true;}
  });
  assert(local_rejected.wait_for(30ms)==std::future_status::timeout);
  auto local_stopped=std::async(std::launch::async,[&]{local_closing.Stop();});
  assert(local_rejected.wait_for(2s)==std::future_status::ready && local_rejected.get());
  local_release.set_value();local_stopped.get();assert(drained==5);

  // Shutdown wakes a producer blocked on capacity, without accepting its job.
  SwanRender::Queue closing(1);
  std::promise<void> closing_entered,closing_release;
  auto closing_allowed=closing_release.get_future().share();
  assert(closing.Start());
  closing.Push([&]{closing_entered.set_value();closing_allowed.wait();});
  closing_entered.get_future().wait();closing.Push([]{});
  auto rejected=std::async(std::launch::async,[&]{
    try {closing.Push([]{});return false;}catch(const std::runtime_error&){return true;}
  });
  assert(rejected.wait_for(30ms)==std::future_status::timeout);
  auto stopped=std::async(std::launch::async,[&]{closing.Stop();});
  assert(rejected.wait_for(2s)==std::future_status::ready && rejected.get());
  closing_release.set_value();stopped.get();

  SwanPerformanceOptions::Values input, options;
  assert(SwanPerformanceOptions::HardwareBackendEnabled(input));
  assert(!SwanPerformanceOptions::HardwareBackendEnabled({{"video.threaded_hardware_backend","false"}}));
  assert(!SwanPerformanceOptions::HardwareBackendEnabled({{"video.threaded_gl_submission","true"}}));
  assert(SwanPerformanceOptions::HardwareBackendEnabled({{"video.threaded_hardware_backend","true"},{"video.threaded_gl_submission","true"}}));
  SwanPerformanceOptions::Apply(input, options);
  assert(options["swanstation_CPU_ExecutionMode"] == "Recompiler");
  assert(options["swanstation_CPU_FastmemMode"] == "LUT");
  assert(options["swanstation_CPU_RecompilerBlockLinking"] == "true");
  assert(options["swanstation_GPU_UseThread"] == "true");
  assert(options["swanstation_GPU_ShaderPrecompile"] == "Lazy");
  assert(options["swanstation_CPU_FastmemRewrite"] == "false");
  assert(options["swanstation_CDROM_ReadaheadSectors"] == "8");
  input = {{"cpu.execution_mode", "cached_interpreter"}, {"cpu.fastmem_mode", "MMap"},
    {"cpu.block_linking", "false"}, {"cpu.instruction_cache", "true"},
    {"video.software_gpu_thread", "false"}, {"video.shader_precompile", "Enabled"}, {"cpu.cd_read_thread", "false"}};
  SwanPerformanceOptions::Apply(input, options);
  assert(options["swanstation_CPU_ExecutionMode"] == "CachedInterpreter");
  assert(options["swanstation_CPU_FastmemMode"] == "MMap");
  assert(options["swanstation_CPU_RecompilerBlockLinking"] == "false");
  assert(options["swanstation_CPU_RecompilerICache"] == "true");
  assert(options["swanstation_GPU_UseThread"] == "false");
  assert(options["swanstation_GPU_ShaderPrecompile"] == "Enabled");
  assert(options["swanstation_CDROM_ReadaheadSectors"] == "0");
  input = {{"cpu.fastmem_mode", "invalid"}, {"cpu.block_linking", "invalid"},
    {"video.shader_precompile", "invalid"}};
  SwanPerformanceOptions::Apply(input, options);
  assert(options["swanstation_CPU_FastmemMode"] == "LUT");
  assert(options["swanstation_CPU_RecompilerBlockLinking"] == "true");
  assert(options["swanstation_GPU_ShaderPrecompile"] == "Lazy");
  std::cout << "Rendering queue, GL barriers and performance option regressions passed\n";
}
