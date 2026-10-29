// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <functional>
#include <memory>
class GPU;
class GPU_HW;
namespace SwanGpuPipeline {
using Dispatch = bool(*)(void(*)(void*),void*,bool,size_t);
// A successful dispatcher owns the opaque packet and executes it exactly once.
// A rejected dispatch must not execute or retain it. wait=true is a barrier.
void Configure(Dispatch dispatch);
bool OnBackend();
bool Active();
bool Failed();
struct PacketMetrics {size_t pooled=0,heap_fallbacks=0,payload_growths=0;};
PacketMetrics ReadPacketMetrics(); // CPU owner only, like Configure/Submit.
void Sync();
void ConfigureFrames(unsigned capacity);
bool QueuesFrames();
unsigned QueuedFrames();
unsigned PeakFrames();
struct DisplayGeometry {unsigned width=0,height=0;float aspect=0;};
DisplayGeometry Geometry(); // CPU-owned descriptor; never borrows backend display fields
void Present(std::function<void()> render);
std::unique_ptr<GPU> Wrap(std::unique_ptr<GPU_HW> backend);
}
