# Work around Espressif ROMs exporting their own, older miniz.
#
# ESP32 ROMs (S3/C3/C6/P4/...) bake in an older miniz and export its tinfl
# entry points from the target's ROM linker script as *absolute* symbols -
# see the "Group miniz" block in
# $IDF_PATH/components/esp_rom/<target>/ld/<target>.rom.ld, e.g.
#
#     tinfl_decompress = 0x4fc000f8;
#
# A linker-script assignment outranks an ordinary object definition, so an
# ESP-IDF link silently binds those names to ROM and drops the copies
# compiled from deps/miniz-3.1.2/miniz.c - even though both are present in
# the archive. The result is a *split decoder*: mz_inflateInit2()/mz_inflate()
# from miniz 3.1.2 build and interpret a 3.1.2-layout tinfl_decompressor, then
# hand it to a ROM tinfl_decompress() that lays that struct out differently
# (miniz 3.0 reworked the Huffman tables from tinfl_huff_table m_tables[3] to
# the flattened m_look_up/m_tree_N form, changing field offsets and total
# size). The ROM decoder writes past the end of the smaller m_decomp and
# corrupts the enclosing inflate_state.
#
# Observed on an ESP32-P4 (rev v3.1) against a 128-file CHD corpus: the first
# inflate of a stream mostly survives, then every later one fails, because the
# overrun lands on inflate_state::m_window_bits (which sits just before
# m_dict[32768]). mz_inflate() then sees m_window_bits > 0, sets
# TINFL_FLAG_PARSE_ZLIB_HEADER on a raw-deflate stream opened with
# inflateInit2(..., -MAX_WBITS), consumes exactly 2 bytes on the CMF/FLG check
# and returns MZ_DATA_ERROR. It presents as CHDERR_DECOMPRESSION_ERROR and
# looks exactly like corrupt input or a silicon/codegen bug.
#
# Renaming the colliding symbols keeps miniz.c's own definitions reachable.
# Apply the defines to every target that compiles a translation unit naming
# them - miniz.c itself, and libchdr's own sources, which call mz_crc32.
# mz_free matters independently of the decoder mismatch: bound to ROM it would
# hand ESP-IDF-heap pointers to the ROM allocator. mz_adler32 and mz_crc32 are
# benign - pure functions over a caller buffer, with no shared struct to
# disagree about - but renamed so the whole group stays consistent.
#
# Deliberately NOT patched into deps/miniz-3.1.2/miniz.h - that tree is
# vendored verbatim so it can be re-synced from upstream, and a local edit
# there would be silently dropped by the next version bump. Keep this file as
# the single definition; both build paths below include it.
#
# Regression check (cheap, no flashing) - this must print nothing:
#
#   grep -hoE '^[A-Za-z_][A-Za-z0-9_]* = 0x' \
#       "$IDF_PATH"/components/esp_rom/<target>/ld/<target>.rom*.ld \
#     | sed 's/ = 0x//' | sort -u > /tmp/rom_syms.txt
#   <target>-nm <libchdr archive> \
#     | awk '$2 ~ /^[TDBR]$/ {print $3}' | sort -u > /tmp/chdr_syms.txt
#   comm -12 /tmp/rom_syms.txt /tmp/chdr_syms.txt
#
# Re-run it after any miniz bump, and after adding any dep the ROM also
# ships - the rom.ld files list them by group.

set(LIBCHDR_ESP_ROM_MINIZ_COLLISIONS
  tinfl_decompress
  tinfl_decompress_mem_to_heap
  tinfl_decompress_mem_to_mem
  tinfl_decompress_mem_to_callback
  mz_adler32
  mz_crc32
  mz_free
)

# Apply the renames to a target that compiles miniz.c. No-op off ESP-IDF.
function(libchdr_apply_esp_rom_miniz_workaround target)
  if(NOT ESP_PLATFORM)
    return()
  endif()
  foreach(sym IN LISTS LIBCHDR_ESP_ROM_MINIZ_COLLISIONS)
    target_compile_definitions(${target} PRIVATE "${sym}=libchdr_${sym}")
  endforeach()
endfunction()
