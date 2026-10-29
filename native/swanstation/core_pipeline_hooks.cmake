# SPDX-License-Identifier: GPL-3.0-or-later
# Apply to build copies after the other adapter hooks, never the pinned checkout.
function(swan_pipeline_replace before after)
  string(FIND "${text}" "${before}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned hardware pipeline seam changed: ${before}")
  endif()
  string(REPLACE "${before}" "${after}" text "${text}")
  set(text "${text}" PARENT_SCOPE)
endfunction()
function(swan_attach_pipeline_hooks)
  get_target_property(sources core SOURCES)
  set(matched 0)
  foreach(source IN LISTS sources)
    get_filename_component(name "${source}" NAME)
    if(NOT name MATCHES "^(gpu.cpp|swan_stats_gpu_hw.cpp|swan_gl_display.cpp|gpu_hw_vulkan.cpp|swan_texture_replacements.cpp)$")
      continue()
    endif()
    math(EXPR matched "${matched}+1")
    if(IS_ABSOLUTE "${source}")
      file(READ "${source}" text)
    else()
      file(READ "${PROJECT_SOURCE_DIR}/src/core/${source}" text)
    endif()
    if(name STREQUAL "gpu.cpp")
      swan_pipeline_replace("bool GPU::Initialize(HostDisplay* host_display)\n{"
        "bool GPU::Initialize(HostDisplay* host_display)\n{\n  if(SwanGpuPipeline::OnBackend()) { m_host_display=host_display; return true; }")
      swan_pipeline_replace("void GPU::Reset(bool clear_vram)\n{"
        "void GPU::Reset(bool clear_vram)\n{\n  if(SwanGpuPipeline::OnBackend()) return;")
      swan_pipeline_replace("void GPU::UpdateSettings()\n{"
        "void GPU::UpdateSettings()\n{\n  if(SwanGpuPipeline::OnBackend()) return;")
    elseif(name STREQUAL "swan_stats_gpu_hw.cpp")
      swan_pipeline_replace("void GPU_HW::UpdateSoftwareRenderer(bool copy_vram_from_hw)\n{"
        "void GPU_HW::UpdateSoftwareRenderer(bool copy_vram_from_hw)\n{\n  if(SwanGpuPipeline::OnBackend()) return;")
    elseif(name STREQUAL "swan_gl_display.cpp")
      swan_pipeline_replace("return std::make_unique<GPU_HW_OpenGL>();"
        "return SwanGpuPipeline::Wrap(std::make_unique<GPU_HW_OpenGL>());")
    elseif(name STREQUAL "gpu_hw_vulkan.cpp")
      swan_pipeline_replace("return std::make_unique<GPU_HW_Vulkan>();"
        "return SwanGpuPipeline::Wrap(std::make_unique<GPU_HW_Vulkan>());")
      swan_pipeline_replace("void LibretroVulkanHostDisplay::DestroyResources()\n{"
        "void LibretroVulkanHostDisplay::DestroyResources()\n{\n  if(m_ri) m_ri->wait_sync_index(m_ri->handle);")
      swan_pipeline_replace("  g_vulkan_context->DeferFramebufferDestruction(m_frame_framebuffer);"
        "  m_ri->wait_sync_index(m_ri->handle); // Retire frontend reads before replacing the shared output image.\n  g_vulkan_context->DeferFramebufferDestruction(m_frame_framebuffer);")
    elseif(name STREQUAL "swan_texture_replacements.cpp")
      foreach(signature IN ITEMS "void TextureReplacements::SetGameID(std::string game_id)" "void TextureReplacements::Reload()")
        swan_pipeline_replace("${signature}\n{" "${signature}\n{\n  SwanGpuPipeline::Sync();")
      endforeach()
    endif()
    string(PREPEND text "// Generated hardware-pipeline build copy.\n#include \"gpu_pipeline.h\"\n")
    set(output "${PROJECT_BINARY_DIR}/swan_pipeline_${name}")
    file(WRITE "${output}" "${text}")
    get_target_property(current core SOURCES)
    list(REMOVE_ITEM current "${source}")
    set_property(TARGET core PROPERTY SOURCES "${current}")
    target_sources(core PRIVATE "${output}")
  endforeach()
  if(NOT matched EQUAL 5)
    message(FATAL_ERROR "Hardware pipeline hooks require all five pinned sources; found ${matched}")
  endif()
  target_sources(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/gpu_pipeline.cpp")
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${PROJECT_SOURCE_DIR}/src/core")

  file(READ "${PROJECT_SOURCE_DIR}/src/libretro/libretro_host_interface.cpp" text)
  swan_pipeline_replace("void HostInterface::UpdateSettings()\n{"
    "void HostInterface::UpdateSettings()\n{\n  SwanGpuPipeline::Sync();")
  swan_pipeline_replace("  System::RunFrame();" "  System::RunFrame();\n  if(!SwanGpuPipeline::QueuesFrames()) SwanGpuPipeline::Sync();")
  swan_pipeline_replace("const float aspect_ratio        = m_display->GetDisplayAspectRatio();"
    "const auto swan_geometry = SwanGpuPipeline::Geometry();\n  const float aspect_ratio = SwanGpuPipeline::QueuesFrames() && swan_geometry.aspect > 0 ? swan_geometry.aspect : m_display->GetDisplayAspectRatio();")
  swan_pipeline_replace("(m_display ? m_display->GetDisplayWidth() : GPU_MAX_DISPLAY_WIDTH)"
    "(SwanGpuPipeline::QueuesFrames() && SwanGpuPipeline::Geometry().aspect > 0 ? SwanGpuPipeline::Geometry().width : (m_display ? m_display->GetDisplayWidth() : GPU_MAX_DISPLAY_WIDTH))")
  swan_pipeline_replace("(m_display ? m_display->GetDisplayHeight() : GPU_MAX_DISPLAY_HEIGHT)"
    "(SwanGpuPipeline::QueuesFrames() && SwanGpuPipeline::Geometry().aspect > 0 ? SwanGpuPipeline::Geometry().height : (m_display ? m_display->GetDisplayHeight() : GPU_MAX_DISPLAY_HEIGHT))")
  swan_pipeline_replace("(m_display ? m_display->GetDisplayAspectRatio() : (g_gpu ? g_gpu->GetDisplayAspectRatio() : g_settings.GetDisplayAspectRatioValue()))"
    "(SwanGpuPipeline::QueuesFrames() && SwanGpuPipeline::Geometry().aspect > 0 ? SwanGpuPipeline::Geometry().aspect : (m_display ? m_display->GetDisplayAspectRatio() : (g_gpu ? g_gpu->GetDisplayAspectRatio() : g_settings.GetDisplayAspectRatioValue())))")
  foreach(signature IN ITEMS "bool HostInterface::UpdateSystemAVInfo(bool use_resolution_scale)" "void HostInterface::UpdateGeometry()")
    swan_pipeline_replace("${signature}\n{" "${signature}\n{\n  SwanGpuPipeline::Sync();")
  endforeach()
  # Pointer controllers share display mouse/cursor fields. Keep their original
  # synchronous path, as well as runahead's internal replay/texture semantics.
  swan_pipeline_replace("void HostInterface::UpdateControllers()\n{"
    "void HostInterface::UpdateControllers()\n{\n  if(SwanGpuPipeline::QueuesFrames()) {\n    bool serial_display = g_settings.runahead_frames > 0;\n    for(const auto type : g_settings.controller_types) serial_display |= type == ControllerType::NamcoGunCon || type == ControllerType::PlayStationMouse;\n    if(serial_display) SwanGpuPipeline::ConfigureFrames(0);\n  }")
  swan_pipeline_replace("  m_display->Render();" "  SwanGpuPipeline::Present([this]{m_display->Render();});")
  string(PREPEND text "// Generated hardware-pipeline host build copy.\n#include \"gpu_pipeline.h\"\n")
  set(output "${PROJECT_BINARY_DIR}/swan_pipeline_libretro_host.cpp")
  file(WRITE "${output}" "${text}")
  get_target_property(host_sources swanstation_libretro SOURCES)
  if(NOT "libretro_host_interface.cpp" IN_LIST host_sources)
    message(FATAL_ERROR "Pinned libretro host source no longer matches")
  endif()
  list(REMOVE_ITEM host_sources libretro_host_interface.cpp)
  set_property(TARGET swanstation_libretro PROPERTY SOURCES "${host_sources}")
  target_sources(swanstation_libretro PRIVATE "${output}")
  target_include_directories(swanstation_libretro PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${PROJECT_SOURCE_DIR}/src/libretro")
endfunction()
