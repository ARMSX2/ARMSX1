# SPDX-License-Identifier: GPL-3.0-or-later
# Run after Build-SwanCore.ps1: cmake -P tests/swan_state_media_hooks_test.cmake
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
file(READ "${root}/core/swanstation/src/core/system.cpp" upstream)
file(READ "${root}/build-core/swan_state_system.cpp" generated)

string(FIND "${generated}" "SwanStateMedia::Resolve(" resolve_at)
string(FIND "${generated}" "std::unique_ptr<CDImage> old_media = g_cdrom.RemoveMedia(false);" eject_at)
if(resolve_at LESS 0 OR eject_at LESS resolve_at)
  message(FATAL_ERROR "State media must be resolved before ejecting the current disc")
endif()
string(FIND "${generated}" "this may result in instability." unsafe_fallback)
if(NOT unsafe_fallback EQUAL -1)
  message(FATAL_ERROR "Unsafe mounted-disc fallback remains enabled")
endif()
string(FIND "${generated}" "g_cdrom.InsertMedia(std::move(old_media));" restore_at)
if(restore_at LESS 0)
  message(FATAL_ERROR "Failed media opens must preserve the mounted disc")
endif()

# PBP subimage bounds/switching must remain exactly as in the pinned core.
string(FIND "${upstream}" "  if (media && header.version >= 51)" disc_start)
string(FIND "${upstream}" "  ClearMemorySaveStates();" unused_first_clear)
string(SUBSTRING "${upstream}" ${disc_start} -1 tail)
string(FIND "${tail}" "  ClearMemorySaveStates();" disc_length)
string(SUBSTRING "${tail}" 0 ${disc_length} disc_block)
string(FIND "${generated}" "${disc_block}" disc_at)
if(disc_at LESS 0)
  message(FATAL_ERROR "Original PBP subimage restoration changed")
endif()

# No serializer or file-format changes: existing states need no rewriting.
string(FIND "${upstream}" "bool SaveState(ByteStream* state)" save_start)
string(SUBSTRING "${upstream}" ${save_start} -1 save_tail)
string(FIND "${generated}" "${save_tail}" save_at)
if(save_at LESS 0)
  message(FATAL_ERROR "State serializer or unrelated later core code changed")
endif()
message(STATUS "State media hooks: ordering, safe failure, PBP indices and serializer checked")
