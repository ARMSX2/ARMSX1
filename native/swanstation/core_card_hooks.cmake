# SPDX-License-Identifier: GPL-3.0-or-later
# Build-only adapter extension; retain the pinned upstream checkout unchanged.
function(swan_attach_card_hooks)
  file(READ "${PROJECT_SOURCE_DIR}/src/core/settings.cpp" text)
  set(anchor "  memory_card_use_playlist_title = si.GetBoolValue(\"MemoryCards\", \"UsePlaylistTitle\", true);")
  string(FIND "${text}" "${anchor}" found)
  if(found LESS 0)
    message(FATAL_ERROR "Pinned memory-card settings no longer match")
  endif()
  set(extra [=[
  memory_card_paths[0] = si.GetStringValue("MemoryCards", "Card1Path", "");
  memory_card_paths[1] = si.GetStringValue("MemoryCards", "Card2Path", "");
]=])
  string(REPLACE "${anchor}" "${anchor}\n${extra}" text "${text}")
  set(output "${PROJECT_BINARY_DIR}/swan_card_settings.cpp")
  file(WRITE "${output}" "${text}")
  get_target_property(sources core SOURCES)
  list(REMOVE_ITEM sources settings.cpp)
  set_property(TARGET core PROPERTY SOURCES "${sources}")
  target_sources(core PRIVATE "${output}")
  target_include_directories(core PRIVATE "${PROJECT_SOURCE_DIR}/src/core")
endfunction()
