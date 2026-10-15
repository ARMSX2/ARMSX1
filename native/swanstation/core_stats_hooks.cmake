# SPDX-License-Identifier: GPL-3.0-or-later
# Opt-in OSD hooks in build copies only. Never edit the pinned submodule.
function(swan_stats_replace before after)
  string(FIND "${text}" "${before}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned OSD hook no longer matches: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" text "${text}")
  set(text "${text}" PARENT_SCOPE)
endfunction()

function(swan_attach_statistics_hooks)
  foreach(source IN ITEMS gpu_commands.cpp gpu_hw.cpp gpu_sw.cpp dma.cpp spu.cpp mdec.cpp cdrom.cpp)
    file(READ "${PROJECT_SOURCE_DIR}/src/core/${source}" text)
    if(source STREQUAL "gpu_commands.cpp")
      swan_stats_replace("DispatchRenderCommand();"
        "SwanTelemetry::counters.Primitive(static_cast<unsigned>(m_render_command.primitive.GetValue()), m_render_command.quad_polygon, m_render_command.polyline ? GetPolyLineVertexCount() : 2);\n  DispatchRenderCommand();")
    elseif(source STREQUAL "gpu_hw.cpp")
      swan_stats_replace("m_resolution_scale = CalculateResolutionScale();"
        "m_resolution_scale = CalculateResolutionScale();\n  SwanTelemetry::counters.scale = m_resolution_scale;")
      swan_stats_replace("m_resolution_scale = resolution_scale;"
        "m_resolution_scale = resolution_scale;\n  SwanTelemetry::counters.scale = m_resolution_scale;")
    elseif(source STREQUAL "gpu_sw.cpp")
      swan_stats_replace("bool GPU_SW::Initialize(HostDisplay* host_display)\n{"
        "bool GPU_SW::Initialize(HostDisplay* host_display)\n{\n  SwanTelemetry::counters.scale = 1;")
    elseif(source STREQUAL "dma.cpp")
      foreach(direction IN ITEMS MemoryToDevice DeviceToMemory)
        set(signature "TickCount DMA::Transfer${direction}(Channel channel, uint32_t address, uint32_t increment, uint32_t word_count)\n{")
        swan_stats_replace("${signature}" "${signature}\n  SwanTelemetry::counters.Dma(static_cast<unsigned>(channel), word_count);")
      endforeach()
    elseif(source STREQUAL "spu.cpp")
      swan_stats_replace("void SPU::Execute(TickCount ticks)\n{"
        "void SPU::Execute(TickCount ticks)\n{\n  if (SwanTelemetry::counters.enabled) {\n    unsigned active = 0;\n    for (const auto& voice : m_voices) if (voice.adsr_phase != ADSRPhase::Off) active++;\n    SwanTelemetry::counters.Voices(active);\n  }")
    elseif(source STREQUAL "mdec.cpp")
      swan_stats_replace("void MDEC::CopyOutBlock()\n{"
        "void MDEC::CopyOutBlock()\n{\n  SwanTelemetry::counters.Add(14, m_status.data_output_depth >= DataOutputDepth_24Bit ? 1 : 0);\n  SwanTelemetry::counters.Add(15, m_status.data_output_depth >= DataOutputDepth_24Bit ? 6 : 1);")
    elseif(source STREQUAL "cdrom.cpp")
      # Count completed sector reads only; do not change the existing failure path.
      swan_stats_replace("if (!m_reader.WaitForReadToComplete()) { }"
        "if (!m_reader.WaitForReadToComplete()) { } else { SwanTelemetry::counters.Add(16, 1); }")
    endif()
    string(PREPEND text "// Generated OSD build copy; upstream notices remain below.\n#include \"statistics.h\"\n")
    set(output "${PROJECT_BINARY_DIR}/swan_stats_${source}")
    file(WRITE "${output}" "${text}")
    get_target_property(sources core SOURCES)
    list(REMOVE_ITEM sources "${source}")
    set_property(TARGET core PROPERTY SOURCES "${sources}")
    target_sources(core PRIVATE "${output}")
  endforeach()
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${PROJECT_SOURCE_DIR}/src/core")
endfunction()
