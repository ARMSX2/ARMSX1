// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <initializer_list>
#include <map>
#include <string>
namespace SwanPerformanceOptions {
using Values = std::map<std::string, std::string>;
inline bool HardwareBackendEnabled(const Values& input) {
  const auto explicit_mode=input.find("video.threaded_hardware_backend");
  if(explicit_mode!=input.end())return explicit_mode->second!="false";
  const auto legacy=input.find("video.threaded_gl_submission");
  return legacy==input.end()||legacy->second!="true";
}
inline void Apply(const Values& input, Values& options) {
  const auto value = [&](const char* key, const char* fallback, std::initializer_list<const char*> allowed) {
    const auto it = input.find(key);
    if (it != input.end()) for (const auto* valid : allowed) if (it->second == valid) return it->second;
    return std::string(fallback);
  };
  const auto engine = value("cpu.execution_mode", "cached", {"cached", "recompiler", "interpreter", "cached_interpreter"});
  // Keep 'cached', the old Android value, as the JIT on existing installations.
  options["swanstation_CPU_ExecutionMode"] = engine == "interpreter" ? "Interpreter" :
    engine == "cached_interpreter" ? "CachedInterpreter" : "Recompiler";
  options["swanstation_CPU_FastmemMode"] = value("cpu.fastmem_mode", "LUT", {"Disabled", "LUT", "MMap"});
  options["swanstation_GPU_ShaderPrecompile"] = value("video.shader_precompile", "Lazy", {"Disabled", "Enabled", "Lazy"});
  const auto boolean = [&](const char* key, const char* option, const char* fallback) {
    options[option] = value(key, fallback, {"true", "false"});
  };
  boolean("cpu.block_linking", "swanstation_CPU_RecompilerBlockLinking", "true");
  boolean("cpu.instruction_cache", "swanstation_CPU_RecompilerICache", "false");
  boolean("cpu.fastmem_rewrite", "swanstation_CPU_FastmemRewrite", "false");
  boolean("video.software_gpu_thread", "swanstation_GPU_UseThread", "true");
  boolean("video.software_readbacks", "swanstation_GPU_UseSoftwareRendererForReadbacks", "false");
  boolean("video.texture_preload", "swanstation_TextureReplacements_PreloadTextures", "false");
  // The pinned core consumes ReadaheadSectors, not the obsolete ReadThread
  // metadata option. Zero stops the worker; eight preserves its current default.
  options["swanstation_CDROM_ReadaheadSectors"] =
    value("cpu.cd_read_thread", "true", {"true", "false"}) == "true" ? "8" : "0";
}
} // namespace SwanPerformanceOptions
