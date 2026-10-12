// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
namespace SwanPerformance {void Enable(bool);void Affinity(int);void Start();void Frame(int64_t work_ns,int64_t target_ns);void Close();double CpuPercent();}
