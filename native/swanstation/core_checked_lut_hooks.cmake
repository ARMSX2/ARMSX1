# SPDX-License-Identifier: GPL-3.0-or-later
# An independent extension of the pinned GPL recompiler, applied to build copies.
option(SWAN_CHECKED_LUT_FASTMEM "ARM64 checked LUT RAM accesses" ON)
function(swan_lut_replace before after)
  string(FIND "${text}" "${before}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned checked-LUT seam changed: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" text "${text}")
  set(text "${text}" PARENT_SCOPE)
endfunction()
function(swan_attach_checked_lut_hooks)
  if(NOT CPU_ARCH STREQUAL "aarch64")
    return()
  endif()
  get_target_property(sources core SOURCES)
  set(matched 0)
  foreach(source IN LISTS sources)
    get_filename_component(name "${source}" NAME)
    if(NOT name MATCHES "^cpu_recompiler_code_generator_(generic|aarch64).cpp$")
      continue()
    endif()
    math(EXPR matched "${matched}+1")
    file(READ "${PROJECT_SOURCE_DIR}/src/core/${name}" text)
    if(name MATCHES "generic")
      swan_lut_replace("use_fastmem && g_settings.cpu_fastmem_rewrite)"
        "use_fastmem && (g_settings.cpu_fastmem_rewrite || SwanCheckedLut::Enabled()))")
    else()
      # Work only on the two dynamic emitters. Constant known-RAM reads retain
      # their original implementation; the other CPU architectures are untouched.
      set(whole "${text}")
      foreach(kind IN ITEMS Load Store)
        set(begin "void CodeGenerator::Emit${kind}GuestMemoryFastmem(")
        set(end "void CodeGenerator::Emit${kind}GuestMemorySlowmem(")
        string(FIND "${whole}" "${begin}" start)
        string(FIND "${whole}" "${end}" finish)
        math(EXPR length "${finish}-${start}")
        if(start LESS 0 OR length LESS 1)
          message(FATAL_ERROR "Pinned checked-LUT emitter boundaries changed")
        endif()
        string(SUBSTRING "${whole}" ${start} ${length} original)
        set(text "${original}")
        swan_lut_replace("  m_register_cache.InhibitAllocation();" [=[  m_register_cache.InhibitAllocation();
  const bool swan_checked_lut = SwanCheckedLut::Enabled();
  if (swan_checked_lut)
  {
    // Cache isolation clears the base. Do not dereference it or bypass the bus.
    a64::Label base_ready;
    m_emit->Cbnz(GetFastmemBasePtrReg(), &base_ready);
    EmitBranch(GetCurrentFarCodePointer(), false);
    m_emit->Bind(&base_ready);
    if (size != RegSize_8)
    {
      // Unaligned accesses can cross a LUT page; use the original bus handler.
      a64::Label aligned;
      m_emit->Tst(GetHostReg32(address_reg), size == RegSize_16 ? 1 : 3);
      m_emit->B(a64::eq, &aligned);
      EmitBranch(GetCurrentFarCodePointer(), false);
      m_emit->Bind(&aligned);
    }
  }]=])
        # A null read entry is unmapped; a null write entry can also be a page
        # containing JIT code. The existing slow handler performs invalidation.
        if(kind STREQUAL "Load")
          set(lookup "    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetFastmemBasePtrReg(), GetHostReg32(RARG1), a64::LSL, 3));")
        else()
          set(lookup "    m_emit->ldr(GetHostReg64(RARG1), a64::MemOperand(GetHostReg64(RARG3), GetHostReg32(RARG1), a64::LSL, 3));")
        endif()
        swan_lut_replace("${lookup}" "${lookup}\n    if (swan_checked_lut)\n    {\n      a64::Label mapped;\n      m_emit->Cbnz(GetHostReg64(RARG1), &mapped);\n      EmitBranch(GetCurrentFarCodePointer(), false);\n      m_emit->Bind(&mapped);\n    }")
        swan_lut_replace("  m_block->loadstore_backpatch_info.push_back(bpi);"
          "  if (!swan_checked_lut) m_block->loadstore_backpatch_info.push_back(bpi);")
        string(REPLACE "${original}" "${text}" whole "${whole}")
      endforeach()
      set(text "${whole}")
    endif()
    string(PREPEND text "// Generated checked-LUT build copy of the pinned GPL core.\n#include \"checked_lut.h\"\n")
    set(output "${PROJECT_BINARY_DIR}/swan_lut_${name}")
    file(WRITE "${output}" "${text}")
    get_target_property(current core SOURCES)
    list(REMOVE_ITEM current "${source}")
    set_property(TARGET core PROPERTY SOURCES "${current}")
    target_sources(core PRIVATE "${output}")
  endforeach()
  if(NOT matched EQUAL 2)
    message(FATAL_ERROR "Checked LUT requires the two pinned ARM64 recompiler sources")
  endif()
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_compile_definitions(core PUBLIC SWAN_CHECKED_LUT_FASTMEM=$<BOOL:${SWAN_CHECKED_LUT_FASTMEM}>)
endfunction()
