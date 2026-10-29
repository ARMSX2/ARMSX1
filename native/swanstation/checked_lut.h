// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/platform.h"
#include "core/settings.h"
#ifndef SWAN_CHECKED_LUT_FASTMEM
#define SWAN_CHECKED_LUT_FASTMEM 1
#endif
namespace SwanCheckedLut {
// This experimental path emits a checked RAM lookup and branches to the pinned
// core's normal bus handler for missing/protected pages. It needs no fault-driven
// rewriting, and never changes the user's advanced rewrite or MMap setting.
inline bool Enabled() {
#if defined(CPU_AARCH64) && SWAN_CHECKED_LUT_FASTMEM
  return g_settings.IsUsingFastmem() && g_settings.cpu_fastmem_mode==CPUFastmemMode::LUT &&
         !g_settings.cpu_fastmem_rewrite;
#else
  return false;
#endif
}
}
