# SPDX-License-Identifier: GPL-3.0-or-later
set(SWAN_CPU_OPTIMIZATION_MASK 7 CACHE STRING "CPU features: fused loads=1, register analysis=2, adaptive code pages=4")
if(NOT SWAN_CPU_OPTIMIZATION_MASK MATCHES "^[0-7]$")
  message(FATAL_ERROR "CPU optimization mask must be 0..7")
endif()
function(swan_cpu_replace before after)
  string(FIND "${text}" "${before}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned CPU optimization seam changed: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" text "${text}")
  set(text "${text}" PARENT_SCOPE)
endfunction()
function(swan_attach_cpu_optimization_hooks)
  if(NOT CPU_ARCH STREQUAL "aarch64")
    return()
  endif()
  get_target_property(sources core SOURCES)
  set(matched 0)
  foreach(source IN LISTS sources)
    get_filename_component(name "${source}" NAME)
    if(NOT name MATCHES "(cpu_recompiler_code_generator(_generic|_aarch64)?|cpu_recompiler_register_cache|cpu_code_cache).cpp$")
      continue()
    endif()
    set(canonical "${CMAKE_MATCH_1}")
    math(EXPR matched "${matched}+1")
    if(IS_ABSOLUTE "${source}")
      file(READ "${source}" text)
    else()
      file(READ "${PROJECT_SOURCE_DIR}/src/core/${source}" text)
    endif()
    if(canonical STREQUAL "cpu_recompiler_code_generator_generic")
      swan_cpu_replace("                                size, result);" [=[                                size, result);
        if (SwanCpuOpt::FusedLoad(cbi.instruction.bits))
        {
          result.size = RegSize_32;
          SwanCpuOpt::NoteFusedLoad();
        }]=])
      swan_cpu_replace("        EmitLoadGlobal(result.GetHostRegister(), size, ptr);" [=[#if defined(CPU_AARCH64)
        if (SwanCpuOpt::FusedLoad(cbi.instruction.bits))
        {
          EmitLoadGlobalAddress(result.host_reg, ptr);
          const auto dst = vixl::aarch64::WRegister(result.host_reg);
          const auto memory = vixl::aarch64::MemOperand(vixl::aarch64::XRegister(result.host_reg));
          if (size == RegSize_8) {
            if (SwanCpuOpt::SignedLoad(cbi.instruction.bits)) m_emit->ldrsb(dst,memory);
            else m_emit->ldrb(dst,memory);
          } else {
            if (SwanCpuOpt::SignedLoad(cbi.instruction.bits)) m_emit->ldrsh(dst,memory);
            else m_emit->ldrh(dst,memory);
          }
          result.size = RegSize_32; SwanCpuOpt::NoteFusedLoad();
        }
        else
#endif
        EmitLoadGlobal(result.GetHostRegister(), size, ptr);]=])
      swan_cpu_replace("    EmitLoadGuestMemoryFastmem(cbi, address, size, result);" [=[    EmitLoadGuestMemoryFastmem(cbi, address, size, result);
    if (SwanCpuOpt::FusedLoad(cbi.instruction.bits))
    {
      result.size = RegSize_32;
      SwanCpuOpt::NoteFusedLoad();
      return result;
    }]=])
    elseif(canonical STREQUAL "cpu_recompiler_code_generator_aarch64")
      set(whole "${text}")
      string(FIND "${whole}" "void CodeGenerator::EmitLoadGuestRAMFastmem(" constant_begin)
      string(FIND "${whole}" "void CodeGenerator::EmitLoadGuestMemoryFastmem(" constant_end)
      math(EXPR constant_length "${constant_end}-${constant_begin}")
      string(SUBSTRING "${whole}" ${constant_begin} ${constant_length} constant_original)
      set(text "${constant_original}")
      # The constant-address emitter has no fault path; its caller has already
      # verified that the entire access is direct-readable RAM.
      string(REPLACE "m_emit->ldrb(GetHostReg32(result.host_reg)," "swan_const_load8(GetHostReg32(result.host_reg)," text "${text}")
      string(REPLACE "m_emit->ldrh(GetHostReg32(result.host_reg)," "swan_const_load16(GetHostReg32(result.host_reg)," text "${text}")
      string(FIND "${text}" "{\n" body)
      math(EXPR body "${body}+2")
      string(SUBSTRING "${text}" 0 ${body} prefix)
      string(SUBSTRING "${text}" ${body} -1 suffix)
      set(text "${prefix}  auto swan_const_load8 = [&](auto reg,const auto& memory) {\n    if (SwanCpuOpt::SignedLoad(m_current_instruction->instruction.bits)) m_emit->ldrsb(reg,memory);\n    else m_emit->ldrb(reg,memory);\n  };\n  auto swan_const_load16 = [&](auto reg,const auto& memory) {\n    if (SwanCpuOpt::SignedLoad(m_current_instruction->instruction.bits)) m_emit->ldrsh(reg,memory);\n    else m_emit->ldrh(reg,memory);\n  };\n${suffix}")
      string(REPLACE "${constant_original}" "${text}" whole "${whole}")
      string(FIND "${whole}" "void CodeGenerator::EmitLoadGuestMemoryFastmem(" begin)
      string(FIND "${whole}" "void CodeGenerator::EmitLoadGuestMemorySlowmem(" end)
      math(EXPR length "${end}-${begin}")
      if(begin LESS 0 OR length LESS 1)
        message(FATAL_ERROR "ARM64 load emitter boundaries changed")
      endif()
      string(SUBSTRING "${whole}" ${begin} ${length} original)
      set(text "${original}")
      swan_cpu_replace("  // fastmem" [=[  // Native signed loads write the final 32-bit result directly.
  auto swan_load8 = [&](auto reg, const auto& memory) {
    if (SwanCpuOpt::SignedLoad(cbi.instruction.bits)) m_emit->ldrsb(reg,memory);
    else m_emit->ldrb(reg,memory);
  };
  auto swan_load16 = [&](auto reg, const auto& memory) {
    if (SwanCpuOpt::SignedLoad(cbi.instruction.bits)) m_emit->ldrsh(reg,memory);
    else m_emit->ldrh(reg,memory);
  };
  // fastmem]=])
      swan_cpu_replace("m_emit->ldrb(GetHostReg32(result.host_reg)," "swan_load8(GetHostReg32(result.host_reg),")
      swan_cpu_replace("m_emit->ldrh(GetHostReg32(result.host_reg)," "swan_load16(GetHostReg32(result.host_reg),")
      swan_cpu_replace("  EmitLoadGuestMemorySlowmem(cbi, address, size, result, true);" [=[  EmitLoadGuestMemorySlowmem(cbi, address, size, result, true);
  // Hardware registers, protected/unmapped pages and fault rewrites must
  // produce the same signed value as the direct path.
  if (SwanCpuOpt::SignedLoad(cbi.instruction.bits))
    EmitSignExtend(result.host_reg, RegSize_32, result.host_reg, size);
  else if (SwanCpuOpt::FusedLoad(cbi.instruction.bits))
    EmitZeroExtend(result.host_reg, RegSize_32, result.host_reg, size);]=])
      string(REPLACE "${original}" "${text}" text "${whole}")
    elseif(canonical STREQUAL "cpu_recompiler_code_generator")
      swan_cpu_replace("  m_block = block;" "  SwanCpuOpt::CompileScope swan_scope(block);\n  m_block = block;")
      swan_cpu_replace("  // move instruction offsets forward" "  SwanCpuOpt::Instruction(&cbi);\n  // move instruction offsets forward")
      foreach(operation IN ITEMS Or Xor Add)
        if(operation STREQUAL "Add")
          set(signature "Value CodeGenerator::AddValues(const Value& lhs, const Value& rhs, bool set_flags)")
          set(extra "!set_flags && ")
        else()
          set(signature "Value CodeGenerator::${operation}Values(const Value& lhs, const Value& rhs)")
          set(extra "")
        endif()
        swan_cpu_replace("${signature}\n{" "${signature}\n{\n  // Identity operations can return a borrowed cache value; never copy a\n  // scratch Value, whose destructor owns its register. PGXP is excluded.\n  if (${extra}SwanCpuOpt::CanAliasOperands())\n  {\n    if (lhs.HasConstantValue(0) && rhs.IsInHostRegister() && !rhs.IsScratch()) return rhs;\n    if (rhs.HasConstantValue(0) && lhs.IsInHostRegister() && !lhs.IsScratch()) return lhs;\n  }")
      endforeach()
    elseif(canonical STREQUAL "cpu_recompiler_register_cache")
      swan_cpu_replace("  Reg evict_reg = m_state.guest_reg_order[m_state.guest_reg_order_count - 1];" [=[  Reg evict_reg = m_state.guest_reg_order[m_state.guest_reg_order_count - 1];
  const Reg original = evict_reg;
  unsigned best = 0;
  for (uint32_t i = m_state.guest_reg_order_count; i > 0; --i)
  {
    const Reg candidate = m_state.guest_reg_order[i - 1];
    if (candidate == m_state.load_delay_register || candidate == m_state.next_load_delay_register) continue;
    const unsigned rank = SwanCpuOpt::EvictionRank(static_cast<unsigned>(candidate));
    if (rank > best) { best = rank; evict_reg = candidate; }
  }
  if (evict_reg != original) SwanCpuOpt::NoteGuidedEviction();]=])
      swan_cpu_replace("  FlushGuestRegister(evict_reg, true, true);" [=[  if (SwanCpuOpt::DeadAfterInstruction(static_cast<unsigned>(evict_reg)))
    InvalidateGuestRegister(evict_reg); // proven overwritten before any observer
  else FlushGuestRegister(evict_reg, true, true);]=])
      set(whole "${text}")
      string(FIND "${whole}" "Value RegisterCache::WriteGuestRegister(" begin)
      string(FIND "${whole}" "void RegisterCache::WriteGuestRegisterDelayed(" end)
      math(EXPR length "${end}-${begin}")
      string(SUBSTRING "${whole}" ${begin} ${length} original)
      set(text "${original}")
      swan_cpu_replace("  InvalidateGuestRegister(guest_reg);" [=[  // Rename a cached source only when the following straight-line code
  // overwrites it before any read, trap, memory access, coprocessor or exit.
  if (!value.IsScratch() && value.IsInHostRegister() && value.size == RegSize_32)
  {
    for (unsigned source = 1; source < 32; ++source)
    {
      auto& old = m_state.guest_reg_state[source];
      const Reg source_reg = static_cast<Reg>(source);
      if (!old.IsInHostRegister() || old.host_reg != value.host_reg ||
          source_reg == m_state.load_delay_register || source_reg == m_state.next_load_delay_register ||
          !SwanCpuOpt::MayRename(source, static_cast<unsigned>(guest_reg))) continue;
      const HostReg host = old.host_reg;
      old.Clear(); ClearRegisterFromOrder(source_reg);
      InvalidateGuestRegister(guest_reg);
      cache_value.SetHostReg(this, host, RegSize_32); cache_value.SetDirty();
      AppendRegisterToOrder(guest_reg); value.Clear(); SwanCpuOpt::NoteRename();
      return Value::FromHostReg(this, host, RegSize_32);
    }
  }
  InvalidateGuestRegister(guest_reg);]=])
      string(REPLACE "${original}" "${text}" text "${whole}")
    elseif(canonical STREQUAL "cpu_code_cache")
      swan_cpu_replace("static constexpr uint32_t RECOMPILE_FRAMES_TO_FALL_BACK_TO_INTERPRETER = 100;"
        "static constexpr uint32_t RECOMPILE_FRAMES_TO_FALL_BACK_TO_INTERPRETER = SwanCpuOpt::Enabled(4) ? 15 : 100;")
      swan_cpu_replace("static constexpr uint32_t RECOMPILE_COUNT_TO_FALL_BACK_TO_INTERPRETER = 20;"
        "static constexpr uint32_t RECOMPILE_COUNT_TO_FALL_BACK_TO_INTERPRETER = SwanCpuOpt::Enabled(4) ? 3 : 20;")
      swan_cpu_replace("void ClearState()\n{" "void ClearState()\n{\n  SwanCpuOpt::ResetCodeCache();")
      swan_cpu_replace("static void FallbackExistingBlockToInterpreter(CodeBlock* block)\n{"
        "static void FallbackExistingBlockToInterpreter(CodeBlock* block)\n{\n  SwanCpuOpt::NoteFallback();")
      swan_cpu_replace("    CodeBlock* existing_block = iter->second;" [=[    CodeBlock* existing_block = iter->second;
    if (existing_block && !existing_block->invalidated && SwanCpuOpt::GuardRequired(existing_block) &&
        !SwanCpuOpt::ValidateWords(existing_block))
    {
      InvalidateBlocksWithPageIndex(existing_block->GetStartPageIndex());
    }]=])
      swan_cpu_replace("SetFastMap(block->GetPC(), block->host_code);"
        "SetFastMap(block->GetPC(), SwanCpuOpt::GuardRequired(block) ? FastCompileBlockFunction : block->host_code);")
      swan_cpu_replace("    s_single_block_asm_dispatcher(block->host_code);" [=[    if (SwanCpuOpt::GuardRequired(block) && block->contains_loadstore_instructions &&
        std::any_of(block->instructions.begin(), block->instructions.end(), [](const auto& i){ return i.is_store_instruction; }))
    {
      // A store can modify an instruction later in this same block. Fetch it
      // through the live interpreter rather than executing a stale snapshot.
      if (!g_settings.gpu_pgxp_enable) InterpretUncachedBlock<PGXPMode::Disabled>();
      else if (g_settings.gpu_pgxp_cpu) InterpretUncachedBlock<PGXPMode::CPU>();
      else InterpretUncachedBlock<PGXPMode::Memory>();
    }
    else s_single_block_asm_dispatcher(block->host_code);]=])
      swan_cpu_replace("void InvalidateBlocksWithPageIndex(uint32_t page_index)\n{" [=[void InvalidateBlocksWithPageIndex(uint32_t page_index)
{
  if (SwanCpuOpt::Enabled(4))
  {
    SwanCpuOpt::InvalidatePage(page_index, System::GetFrameNumber());
    auto affected = std::move(m_ram_block_map[page_index]);
    m_ram_block_map[page_index].clear();
    for (CodeBlock* block : affected)
    {
      RemoveBlockFromPageMap(block);
      if (!block->invalidated) InvalidateBlock(block, true);
    }
    Bus::ClearRAMCodePage(page_index);
    return;
  }]=])
      swan_cpu_replace("  BlockMap::iterator iter = s_blocks.find(block->key.GetPC());"
        "  BlockMap::iterator iter = s_blocks.find(block->key.bits);")
      swan_cpu_replace("    Bus::SetRAMCodePage(page);" [=[    if (SwanCpuOpt::ManualPage(page)) block->can_link = false;
    else Bus::SetRAMCodePage(page);]=])
      swan_cpu_replace("    auto page_block_iter = std::find(page_blocks.begin(), page_blocks.end(), block);\n    page_blocks.erase(page_block_iter);"
        "    page_blocks.erase(std::remove(page_blocks.begin(), page_blocks.end(), block), page_blocks.end());")
      string(APPEND text [=[

// Invoked only by the diagnostic private-app probe while the VM is paused.
extern "C" bool swan_cpu_test_dispatch(uint32_t pc) {
#if WITH_RECOMPILER
  CPU::g_state.regs.pc=pc; CPU::g_state.regs.npc=pc+4;
  auto* table=CPU::CodeCache::GetFastMapPointer();
  const auto function=*CPU::CodeCache::OffsetFastMapPointer(table[pc>>16],pc);
  CPU::CodeCache::s_single_block_asm_dispatcher(function);
  return true;
#else
  return false;
#endif
}
]=])
    endif()
    string(PREPEND text "// Generated independent CPU optimizations of the pinned GPL source.\n#include \"cpu_optimizations.h\"\n")
    set(output "${PROJECT_BINARY_DIR}/swan_opt_${canonical}.cpp")
    file(WRITE "${output}" "${text}")
    get_target_property(current core SOURCES)
    list(REMOVE_ITEM current "${source}")
    set_property(TARGET core PROPERTY SOURCES "${current}")
    target_sources(core PRIVATE "${output}")
  endforeach()
  if(NOT matched EQUAL 5)
    message(FATAL_ERROR "CPU optimizations require five pinned implementation sources; found ${matched}")
  endif()
  target_sources(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cpu_optimizations.cpp")
  target_sources(swanstation_libretro PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/cpu_optimization_probe.cpp")
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_compile_definitions(core PUBLIC SWAN_CPU_OPTIMIZATION_MASK=${SWAN_CPU_OPTIMIZATION_MASK})
endfunction()
