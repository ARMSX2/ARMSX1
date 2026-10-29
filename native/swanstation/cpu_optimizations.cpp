// SPDX-License-Identifier: GPL-3.0-or-later
#include "cpu_optimizations.h"
#include "cpu_instruction_analysis.h"
#include "cpu_code_page_policy.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include <limits>
namespace SwanCpuOpt {
namespace {
thread_local const CPU::CodeBlock* compiling=nullptr;
thread_local size_t at=std::numeric_limits<size_t>::max();
SwanCpuAnalysis::CodePagePolicy<Bus::RAM_8MB_CODE_PAGE_COUNT> pages;
Metrics metrics;
bool Current() {
  return Enabled(2)&&!g_settings.gpu_pgxp_enable&&compiling&&at<compiling->instructions.size()&&
    !compiling->instructions[at].is_branch_instruction&&!compiling->instructions[at].is_branch_delay_slot&&
    SwanCpuAnalysis::Decode(compiling->instructions[at].instruction.bits).known;
}
}
Metrics ReadMetrics(){return metrics;}
void NoteFusedLoad(){++metrics.fused_loads;}
void NoteGuidedEviction(){++metrics.guided_evictions;}
void NoteRename(){++metrics.renames;}
void NoteFallback(){if(Enabled(4))++metrics.fallbacks;}
CompileScope::CompileScope(const CPU::CodeBlock* block):previous(compiling),previous_index(at){compiling=block;at=0;}
CompileScope::~CompileScope(){compiling=previous;at=previous_index;}
void Instruction(const CPU::CodeBlockInstruction* instruction) {
  if(!Enabled(2)||!compiling)return;
  at=std::numeric_limits<size_t>::max();
  const auto begin=reinterpret_cast<uintptr_t>(compiling->instructions.data());
  const auto pointer=reinterpret_cast<uintptr_t>(instruction);
  if(pointer>=begin&&pointer-begin<compiling->instructions.size()*sizeof(CPU::CodeBlockInstruction)&&
      (pointer-begin)%sizeof(CPU::CodeBlockInstruction)==0)
    at=(pointer-begin)/sizeof(CPU::CodeBlockInstruction);
}
unsigned EvictionRank(unsigned reg) {
  if(!Current()||reg>=34)return 0;
  const auto bit=SwanCpuAnalysis::Bit(reg);
  const auto current=SwanCpuAnalysis::Decode(compiling->instructions[at].instruction.bits);
  if((current.reads|current.writes)&bit)return 0;
  for(size_t i=at+1;i<compiling->instructions.size()&&i<=at+64;++i) {
    const auto use=SwanCpuAnalysis::Decode(compiling->instructions[i].instruction.bits);
    if(!use.known||use.branch)return 1;
    if(use.reads&bit)return unsigned(i-at);
    if(use.writes&bit)return 66;
  }
  return 65;
}
bool CanAliasOperands(){return Current()&&SwanCpuAnalysis::Decode(compiling->instructions[at].instruction.bits).rename_safe;}
bool DeadAfterInstruction(unsigned from) {
  if(!CanAliasOperands()||!from||from>=32)return false;
  for(size_t i=at+1;i<compiling->instructions.size()&&i<=at+32;++i) {
    const auto use=SwanCpuAnalysis::Decode(compiling->instructions[i].instruction.bits);
    if(!use.rename_safe||use.branch||(use.reads&SwanCpuAnalysis::Bit(from)))return false;
    if(use.writes&SwanCpuAnalysis::Bit(from))return true;
  }
  return false;
}
bool MayRename(unsigned from,unsigned to) {
  if(from==to||!to||to>=32||!DeadAfterInstruction(from))return false;
  const auto current=SwanCpuAnalysis::Decode(compiling->instructions[at].instruction.bits);
  return (current.reads&SwanCpuAnalysis::Bit(from))&&(current.writes&SwanCpuAnalysis::Bit(to));
}
bool SmcEligible(){return Enabled(4)&&g_settings.IsUsingRecompiler()&&!g_settings.cpu_recompiler_icache;}
bool ManualPage(size_t page){return pages.Manual(page);}
size_t ManualPages(){return pages.ManualCount();}
void InvalidatePage(size_t page,uint32_t frame){if(SmcEligible())pages.Invalidate(page,frame);}
void ResetCodeCache(){pages.Reset();metrics={};}
bool GuardRequired(const CPU::CodeBlock* block) {
  if(!Enabled(4)||!block||!block->IsInRAM())return false;
  for(const auto& instruction:block->instructions)
    if(pages.Manual((instruction.pc&Bus::g_ram_mask)/HOST_PAGE_SIZE))return true;
  return false;
}
bool ValidateWords(const CPU::CodeBlock* block) {
  ++metrics.guard_checks;
  for(const auto& instruction:block->instructions) {
    uint32_t word=0;
    if(!CPU::SafeReadInstruction(instruction.pc,&word)||word!=instruction.instruction.bits)return false;
  }
  return true;
}
}
