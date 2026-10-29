// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "common/platform.h"
#include "core/cpu_code_cache.h"
#include "core/settings.h"
#include <cstddef>
#include <cstdint>
#ifndef SWAN_CPU_OPTIMIZATION_MASK
#define SWAN_CPU_OPTIMIZATION_MASK 7
#endif
namespace SwanCpuOpt {
constexpr unsigned Mask() {
#ifdef CPU_AARCH64
  return SWAN_CPU_OPTIMIZATION_MASK;
#else
  return 0;
#endif
}
constexpr bool Enabled(unsigned bit){return (Mask()&bit)!=0;}
inline bool SignedLoad(uint32_t instruction) {
  const auto op=instruction>>26;
  return Enabled(1)&&(op==32||op==33);
}
inline bool FusedLoad(uint32_t instruction) {
  const auto op=instruction>>26;
  return Enabled(1)&&(op==32||op==33||op==36||op==37);
}
struct Metrics {size_t fused_loads=0,guided_evictions=0,renames=0,guard_checks=0,fallbacks=0;};
Metrics ReadMetrics(); // CPU owner only.
void NoteFusedLoad();void NoteGuidedEviction();void NoteRename();void NoteFallback();
class CompileScope {
  const CPU::CodeBlock* previous;
  size_t previous_index;
public:
  explicit CompileScope(const CPU::CodeBlock* block);
  ~CompileScope();
  CompileScope(const CompileScope&)=delete;
  CompileScope& operator=(const CompileScope&)=delete;
};
void Instruction(const CPU::CodeBlockInstruction* instruction);
unsigned EvictionRank(unsigned reg);
bool MayRename(unsigned from,unsigned to);
bool CanAliasOperands();
bool DeadAfterInstruction(unsigned reg);
bool SmcEligible();
bool ManualPage(size_t page);
size_t ManualPages();
void InvalidatePage(size_t page,uint32_t frame);
void ResetCodeCache();
bool GuardRequired(const CPU::CodeBlock* block);
bool ValidateWords(const CPU::CodeBlock* block);
}
