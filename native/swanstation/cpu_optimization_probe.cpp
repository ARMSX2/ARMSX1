// SPDX-License-Identifier: GPL-3.0-or-later
// Diagnostic-only execution checks. Called on the paused CPU owner, never from
// a render callback; restores RAM, CPU state and settings before returning.
#include "cpu_optimizations.h"
#include "core/cpu_core.h"
#include "core/cpu_core_private.h"
#include "core/cpu_recompiler_code_generator.h"
#include "gpu_pipeline.h"
#include "libretro.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
extern "C" bool swan_cpu_test_dispatch(uint32_t pc);
namespace {
void Require(bool okay,const char* text){if(!okay)throw std::runtime_error(text);}
constexpr uint32_t I(unsigned op,unsigned rs,unsigned rt,int imm){return op<<26|rs<<21|rt<<16|uint16_t(imm);}
constexpr uint32_t R(unsigned rs,unsigned rt,unsigned rd,unsigned sa,unsigned fn){return rs<<21|rt<<16|rd<<11|sa<<6|fn;}
struct Restore {
  CPU::State state=CPU::g_state;
  CPUFastmemMode fastmem=g_settings.cpu_fastmem_mode;
  bool linking=g_settings.cpu_recompiler_block_linking,icache=g_settings.cpu_recompiler_icache;
  bool exceptions=g_settings.cpu_recompiler_memory_exceptions,pgxp=g_settings.gpu_pgxp_enable;
  std::vector<uint8_t> ram{Bus::g_ram,Bus::g_ram+Bus::g_ram_size};
  ~Restore(){
    std::memcpy(Bus::g_ram,ram.data(),ram.size());CPU::g_state=state;
    g_settings.cpu_recompiler_block_linking=linking;g_settings.cpu_recompiler_icache=icache;
    g_settings.cpu_recompiler_memory_exceptions=exceptions;g_settings.gpu_pgxp_enable=pgxp;
    g_settings.cpu_fastmem_mode=fastmem;
    CPU::CodeCache::Flush();CPU::UpdateFastmemBase();
  }
};
CPU::State Seed(const CPU::State& original,uint32_t pc,uint32_t seed) {
  CPU::State state=original;
  for(unsigned reg=1;reg<static_cast<unsigned>(CPU::Reg::count);++reg){seed=seed*1664525u+1013904223u;state.regs.r[reg]=seed;}
  state.regs.zero=0;state.regs.pc=pc;state.regs.npc=pc+4;
  state.pending_ticks=0;state.gte_completion_tick=0;state.downcount=10000000;
  state.load_delay_reg=CPU::Reg::count;state.next_load_delay_reg=CPU::Reg::count;
  state.load_delay_value=state.next_load_delay_value=0;
  state.exception_raised=false;state.branch_was_taken=false;
  state.current_instruction_in_branch_delay_slot=false;state.current_instruction_was_branch_taken=false;
  state.next_instruction_is_branch_delay_slot=false;state.cop0_regs.sr.Isc=false;state.cop0_regs.sr.KUc=false;
  return state;
}
CPU::CodeBlock Block(const std::vector<uint32_t>& words,uint32_t pc) {
  CPU::CodeBlockKey key{};key.SetPC(pc);CPU::CodeBlock block(key);
  for(uint32_t word:words){
    CPU::CodeBlockInstruction c{};c.pc=pc;c.instruction.bits=word;
    c.is_load_instruction=CPU::IsMemoryLoadInstruction(c.instruction);
    c.is_store_instruction=CPU::IsMemoryStoreInstruction(c.instruction);
    c.has_load_delay=CPU::InstructionHasLoadDelay(c.instruction);
    block.contains_loadstore_instructions|=c.is_load_instruction||c.is_store_instruction;
    block.instructions.push_back(c);pc+=4;
  }
  return block;
}
void Equivalent(const CPU::State& expected,const CPU::State& actual) {
  // The pinned JIT synchronizes NPC lazily when leaving its dispatcher. GPRs,
  // HI/LO and the architectural PC are comparable at this block boundary.
  for(unsigned i=0;i<static_cast<unsigned>(CPU::Reg::npc);++i)
    if(expected.regs.r[i]!=actual.regs.r[i])throw std::runtime_error("Register "+std::to_string(i)+" expected "+std::to_string(expected.regs.r[i])+" got "+std::to_string(actual.regs.r[i]));
  Require(expected.load_delay_reg==actual.load_delay_reg,"Load-delay target mismatch");
  if(expected.load_delay_reg!=CPU::Reg::count)Require(expected.load_delay_value==actual.load_delay_value,"Load-delay value mismatch");
  Require(expected.pending_ticks==actual.pending_ticks,"Memory/instruction timing mismatch");
  Require(expected.exception_raised==actual.exception_raised,"Exception state mismatch");
}
void ExecutePair(const CPU::CodeBlock& block,CPU::CodeBlock::HostCodePointer code,
                 CPU::CodeCache::SingleBlockDispatcherFunction dispatch,const CPU::State& input,
                 CPU::CodeBlock::HostCodePointer unchecked_oracle=nullptr) {
  CPU::g_state=input;CPU::UpdateFastmemBase();
  if(unchecked_oracle)dispatch(unchecked_oracle);
  else CPU::CodeCache::InterpretCachedBlock<PGXPMode::Disabled>(block);
  const auto expected=CPU::g_state;
  CPU::g_state=input;CPU::UpdateFastmemBase();dispatch(code);
  Equivalent(expected,CPU::g_state);
}
}
extern "C" RETRO_API bool swan_cpu_optimization_tests(char* output,size_t capacity) {
  if(!output||!capacity)return false;
  std::snprintf(output,capacity,"{\"passed\":false,\"error\":\"Unavailable\"}");
#if defined(CPU_AARCH64) && SWAN_CPU_OPTIMIZATION_MASK
  if(!g_settings.IsUsingRecompiler()||g_settings.cpu_fastmem_mode!=CPUFastmemMode::LUT||g_settings.cpu_fastmem_rewrite)return false;
  SwanGpuPipeline::Sync();Restore restore;
  try {
    g_settings.cpu_recompiler_block_linking=false;g_settings.cpu_recompiler_icache=false;
    g_settings.cpu_recompiler_memory_exceptions=false;g_settings.gpu_pgxp_enable=false;
    CPU::CodeCache::Flush();
    JitCodeBuffer buffer;Require(buffer.Allocate(2*1024*1024,2*1024*1024),"Probe JIT allocation failed");
    CPU::Recompiler::CodeGenerator dispatcher(&buffer);
    auto dispatch=dispatcher.CompileSingleBlockDispatcher();
    unsigned loads=0,vectors=0,smc=0;
    // Compile with a RAM prediction, then exercise RAM, mirrors, BIOS,
    // scratchpad, alignment and cache-isolation fallbacks with the same code.
    const uint32_t addresses[]={0x80018000,0x80018001,0x80018FFF,0xA0018000,0x1F800040,0x1FC00000};
    for(unsigned op:{32u,33u,35u,36u,37u}) {
      auto block=Block({I(op,4,8,0),0,0},0x80016000);
      auto compile=Seed(restore.state,block.GetPC(),29);compile.regs.a0=0x80018000;
      CPU::g_state=compile;CPU::UpdateFastmemBase();
      CPU::CodeBlock::HostCodePointer code=nullptr;uint32_t size=0;
      CPU::Recompiler::CodeGenerator compiler(&buffer);
      Require(compiler.CompileBlock(&block,&code,&size),"Load probe compilation failed");
      // With memory exceptions disabled, the pinned JIT intentionally permits
      // unaligned accesses that the full interpreter traps. Compare those to
      // its unchanged unchecked bus path, rather than changing that policy.
      g_settings.cpu_fastmem_mode=CPUFastmemMode::Disabled;
      CPU::CodeBlock::HostCodePointer unchecked=nullptr;uint32_t unchecked_size=0;
      CPU::Recompiler::CodeGenerator original_bus(&buffer);
      Require(original_bus.CompileBlock(&block,&unchecked,&unchecked_size),"Unchecked reference compilation failed");
      g_settings.cpu_fastmem_mode=CPUFastmemMode::LUT;
      for(uint32_t address:addresses)for(unsigned isolated=0;isolated<2;++isolated) {
        auto input=compile;input.regs.a0=address;input.cop0_regs.sr.Isc=isolated!=0;
        for(unsigned i=0;i<8192;++i)Bus::g_ram[0x18000+i]=uint8_t(i*73+0x80);
        for(unsigned i=0;i<input.dcache.size();++i)input.dcache[i]=uint8_t(i*41+0x80);
        const unsigned alignment=op==33||op==37?2:op==35?4:1;
        ExecutePair(block,code,dispatch,input,address%alignment?unchecked:nullptr);++loads;
      }
      // Also exercise the constant RAM and ROM emitters, not only the dynamic
      // LUT path. The same architectural load-delay result must be retained.
      for(unsigned upper:{0x8001u,0xBFC0u}) {
        auto fixed=Block({I(15,0,4,int(upper)),I(13,4,4,upper==0x8001?0x8000:0),I(op,4,8,0),0,0},0x80016000);
        auto input=Seed(restore.state,fixed.GetPC(),31);CPU::g_state=input;CPU::UpdateFastmemBase();
        CPU::Recompiler::CodeGenerator constant_compiler(&buffer);
        Require(constant_compiler.CompileBlock(&fixed,&code,&size),"Constant-load compilation failed");
        ExecutePair(fixed,code,dispatch,input);++loads;
      }
    }
    const auto load_metrics=SwanCpuOpt::ReadMetrics();
    // Defined unsigned arithmetic, shifts, signed comparisons and deliberate
    // register pressure. Results and cycles must match the live interpreter.
    uint32_t random=0x37ab109d;
    auto next=[&]{random=random*1664525u+1013904223u;return random;};
    for(unsigned test=0;test<96;++test) {
      std::vector<uint32_t> words{
        I(9,4,16,1),R(16,0,8,0,37),I(13,0,16,7),R(8,16,2,0,33)};
      for(unsigned i=0;i<96;++i){
        unsigned rs=1+next()%31,rt=1+next()%31,rd=1+next()%31;
        switch(next()%7){
          case 0:words.push_back(I(9,rs,rt,int(next())));break;
          case 1:words.push_back(I(13,rs,rt,int(next())));break;
          case 2:words.push_back(I(14,rs,rt,int(next())));break;
          case 3:words.push_back(R(rs,rt,rd,0,33));break;
          case 4:words.push_back(R(rs,rt,rd,0,38));break;
          case 5:words.push_back(R(0,rt,rd,next()%32,3));break;
          default:words.push_back(R(rs,rt,rd,0,43));break;
        }
      }
      words.push_back(0);
      auto block=Block(words,0x80016000);auto input=Seed(restore.state,block.GetPC(),next());
      CPU::g_state=input;CPU::UpdateFastmemBase();
      CPU::CodeBlock::HostCodePointer code=nullptr;uint32_t size=0;
      CPU::Recompiler::CodeGenerator compiler(&buffer);
      Require(compiler.CompileBlock(&block,&code,&size),"Register probe compilation failed");
      ExecutePair(block,code,dispatch,input);++vectors;
    }
    const auto register_metrics=SwanCpuOpt::ReadMetrics();
    if(SwanCpuOpt::Enabled(1))Require(load_metrics.fused_loads>0,"Signed-load optimization was not exercised");
    if(SwanCpuOpt::Enabled(2)){
      Require(register_metrics.renames>0,"Register rename was not exercised");
      Require(register_metrics.guided_evictions>0,"Guided eviction was not exercised");
    }
    if(SwanCpuOpt::Enabled(4)) {
      CPU::CodeCache::Flush();
      constexpr uint32_t moving=0x8001C000,stable=0x8001C100,self=0x8001C200;
      auto write=[](uint32_t pc,uint32_t word){Require(CPU::SafeWriteMemoryWord(pc,word),"Probe code write failed");};
      auto program=[&](uint32_t pc,int value){write(pc,I(9,0,2,value));write(pc+4,R(31,0,0,0,8));write(pc+8,0);};
      auto run=[&](uint32_t pc,uint32_t expected){
        auto input=Seed(restore.state,pc,107);input.regs.ra=0x8001D000;
        CPU::g_state=input;CPU::UpdateFastmemBase();
        Require(swan_cpu_test_dispatch(pc),"Fast-map probe dispatch failed");
        Require(CPU::g_state.regs.v0==expected,"Stale self-modifying instruction executed");++smc;
      };
      program(moving,1);program(stable,777);run(stable,777);run(moving,1);
      for(unsigned i=2;i<15;++i){write(moving,I(9,0,2,int(i)));run(stable,777);run(moving,i);}
      Require(SwanCpuOpt::ManualPage((moving&Bus::g_ram_mask)/HOST_PAGE_SIZE),"Hot page did not switch to manual validation");
      write(stable,I(9,0,2,999));run(stable,999);
      // A store changes a later instruction in its own block. Manual pages
      // must fetch those instructions live, including the new immediate.
      write(self,I(43,5,4,8));write(self+4,0);write(self+8,I(9,0,2,2));
      write(self+12,R(31,0,0,0,8));write(self+16,0);
      auto input=Seed(restore.state,self,88);input.regs.a0=I(9,0,2,68);input.regs.a1=self;input.regs.ra=0x8001D000;
      CPU::g_state=input;CPU::UpdateFastmemBase();Require(swan_cpu_test_dispatch(self),"Self-store dispatch failed");
      Require(CPU::g_state.regs.v0==68,"Manual store block executed a stale following instruction");++smc;
    }
    std::snprintf(output,capacity,
      "{\"passed\":true,\"mask\":%u,\"load_cases\":%u,\"register_vectors\":%u,\"smc_checks\":%u,\"fused_loads\":%zu,\"guided_evictions\":%zu,\"renames\":%zu}",
      SwanCpuOpt::Mask(),loads,vectors,smc,load_metrics.fused_loads,register_metrics.guided_evictions,register_metrics.renames);
    return true;
  } catch(const std::exception& error){
    std::snprintf(output,capacity,"{\"passed\":false,\"error\":\"%s\"}",error.what());return false;
  }
#else
  return false;
#endif
}
