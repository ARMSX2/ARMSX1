# SPDX-License-Identifier: GPL-3.0-or-later
# Patch only the build copy; keep the pinned upstream checkout unchanged.
function(swan_attach_display_hooks)
  file(READ "${PROJECT_SOURCE_DIR}/src/core/gpu_hw_opengl.cpp" text)
  set(original [=[m_vram_texture.GetHeight() - scaled_vram_offset_y -
                                               reinterpret_field_offset - (scaled_display_height >> height_div2)]=])
  string(FIND "${text}" "${original}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned OpenGL interlaced scanout no longer matches")
  endif()
  set(corrected [=[SwanGLPresentation::DisplayVramOffsetY(
        m_vram_texture.GetHeight(), scaled_vram_offset_y, scaled_display_height,
        reinterpret_field_offset, height_div2 != 0,
        interlaced == GPU_HW::InterlacedRenderMode::InterleavedFields)]=])
  string(REPLACE "${original}" "${corrected}" text "${text}")
  string(PREPEND text "// Generated GLES scanout build copy; upstream notices remain in checkout.\n#include \"gl_presentation.h\"\n")
  set(output "${PROJECT_BINARY_DIR}/swan_gl_display.cpp")
  file(WRITE "${output}" "${text}")
  get_target_property(sources core SOURCES)
  list(REMOVE_ITEM sources gpu_hw_opengl.cpp)
  set_property(TARGET core PROPERTY SOURCES "${sources}")
  target_sources(core PRIVATE "${output}")
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${PROJECT_SOURCE_DIR}/src/core")
endfunction()
