# SPDX-License-Identifier: GPL-3.0-or-later
# Relocate SAF media in a build copy only; leave the pinned core unchanged.
function(swan_attach_state_hooks)
  file(READ "${PROJECT_SOURCE_DIR}/src/core/system.cpp" text)
  set(anchor "    std::unique_ptr<CDImage> old_media = g_cdrom.RemoveMedia(false);")
  string(FIND "${text}" "${anchor}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned save-state media loading no longer matches")
  endif()
  set(extra [=[
    // Resolve before removing media: a stale state must not eject the live disc
    // or restore against an unrelated disc through the upstream fallback.
    const auto resolved_media = SwanStateMedia::Resolve(media_filename, GetMediaFileName(),
      [](const std::string& path) { return g_host_interface->IsKnownMediaPath(path); });
    if (resolved_media.empty())
    {
      g_host_interface->ReportFormattedError("Save state disc is not part of the current game. Open its original game or playlist before loading this state.");
      return false;
    }
    if (resolved_media != media_filename)
      Log_InfoPrintf("Rebound save-state media to current SAF launch '%s'", resolved_media.c_str());
    media_filename = resolved_media;
]=])
  string(REPLACE "${anchor}" "${extra}\n${anchor}" text "${text}")
  set(fallback [=[          Log_InfoPrintf("Failed to open CD image from save state '%s': %s. Using existing image '%s', this may result in instability.",
                         media_filename.c_str(), error.GetCodeAndMessage().GetCharArray(), old_media->GetFileName().c_str());
          media = std::move(old_media);]=])
  string(FIND "${text}" "${fallback}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned save-state fallback no longer matches")
  endif()
  set(failure [=[          // Keep the mounted disc intact and let LoadState restore its backup.
          g_cdrom.InsertMedia(std::move(old_media));
          g_host_interface->ReportFormattedError("Unable to open the save state's disc '%s': %s. The state was not loaded.",
            media_filename.c_str(), error.GetCodeAndMessage().GetCharArray());
          return false;]=])
  string(REPLACE "${fallback}" "${failure}" text "${text}")
  string(PREPEND text "// Generated save-state media build copy; upstream notices remain below.\n#include \"state_media.h\"\n")
  set(output "${PROJECT_BINARY_DIR}/swan_state_system.cpp")
  file(WRITE "${output}" "${text}")
  get_target_property(sources core SOURCES)
  list(REMOVE_ITEM sources system.cpp)
  set_property(TARGET core PROPERTY SOURCES "${sources}")
  target_sources(core PRIVATE "${output}")
  target_include_directories(core PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}" "${PROJECT_SOURCE_DIR}/src/core")
endfunction()
