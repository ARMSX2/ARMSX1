// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <EGL/egl.h>
#include <string>
#ifndef ARMSX_PACING_DIAGNOSTICS
#define ARMSX_PACING_DIAGNOSTICS 0
#endif
namespace SwanPacingProbe {
void Mode(int value);
std::string Snapshot();
#if ARMSX_PACING_DIAGNOSTICS
void Begin(const std::string& session);
void Boundary(const char* reason);
void Surface(EGLDisplay display,EGLSurface surface,EGLConfig config);
int Interval(int baseline,bool normal_speed,unsigned subframes);
EGLBoolean Swap(EGLDisplay display,EGLSurface surface,int interval,EGLBoolean interval_ok,float nominal,float refresh);
#endif
}
