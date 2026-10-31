cmake_minimum_required(VERSION 3.10)

project(chdr VERSION 0.3.0 LANGUAGES C)

if(CMAKE_PROJECT_NAME STREQUAL "chdr")
  option(BUILD_SHARED_LIBS "Build libchdr also as a shared library" ON)
endif()
option(INSTALL_STATIC_LIBS "Install static libraries" OFF)
option(WITH_SYSTEM_ZLIB "Use system provided zlib library" OFF)
option(WITH_SYSTEM_ZSTD "Use system provided zstd library" OFF)
option(CHDR_WANT_RAW_DATA_SECTOR "Output ECC data and sync header" ON)
option(CHDR_WANT_SUBCODE "Output CD subchannel data" ON)
option(CHDR_VERIFY_BLOCK_CRC "Verify integrity of decoded data" ON)
option(CHDR_LOWRAM_TARGET "Trade CPU for RAM on the per-hunk map (checkpointed on-demand decode instead of fully materializing it at chd_open) - for memory-constrained targets" OFF)
option(CHDR_WANT_TESTS "Build tests for the library" ON)


# CD codecs normally decode into the caller's hunk and spread the sectors in
# place, which reads that buffer back and costs CPU, but needs only a
# frame-sized scratch instead of a hunk-sized one per codec.
#
# Left unset it follows CHDR_LOWRAM_TARGET: private scratch off small-RAM
# targets (the caller's buffer is not read back, and the extra ~37 KB per open
# CD image is affordable), in-place spread on them. Set it to ON or OFF to
# pin it either way - ON is also what a caller wants whose hunk buffer is
# uncached, write-combining or a FIFO, since the buffer is then written once,
# linearly, and never read.
set(CHDR_CD_SCRATCH_BUFFER "" CACHE STRING "Decode CD sectors via a private scratch buffer (ON/OFF; default follows CHDR_LOWRAM_TARGET)")

# FLAC backend. dr_flac is the default and needs nothing extra. microflac is
# faster on MCUs (measured 1.46x on ESP32-S3, 1.41x on ESP32-P4, bit-identical
# output) and smaller, but it is C++ and Apache-2.0, so it is fetched at build
# time rather than vendored - libchdr's own tree stays BSD-3 and a default
# build never touches the network. Point CHDR_MICROFLAC_SOURCE_DIR at a local
# checkout to build offline.
set(CHDR_FLAC_BACKEND "drflac" CACHE STRING "FLAC decoder backend: drflac or microflac")
set_property(CACHE CHDR_FLAC_BACKEND PROPERTY STRINGS drflac microflac)
set(CHDR_MICROFLAC_SOURCE_DIR "" CACHE PATH "Local micro-flac checkout (skips the fetch)")
set(CHDR_MICROFLAC_GIT_TAG "" CACHE STRING "micro-flac commit to fetch (40-char SHA)")

option(BUILD_LTO "Compile libchdr with link-time optimization if supported" OFF)
if(BUILD_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT HAVE_IPO)
  if(HAVE_IPO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  endif()
endif()

if (CHDR_WANT_TESTS)
  option(BUILD_FUZZER "Build instrumented binary for fuzzing with libfuzzer, requires clang")
  if(BUILD_FUZZER)
    # Override CFLAGS early for instrumentation. Disable shared libs for instrumentation.
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address,fuzzer-no-link")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,fuzzer-no-link")
    set(BUILD_SHARED_LIBS OFF)
  endif()
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)

#--------------------------------------------------
# dependencies
#--------------------------------------------------

# lzma
if(NOT TARGET chdr-lzma)
  add_subdirectory(deps/lzma-26.02 EXCLUDE_FROM_ALL)
endif()
list(APPEND CHDR_LIBS chdr-lzma)

# zlib
if (WITH_SYSTEM_ZLIB)
  find_package(ZLIB REQUIRED)
  list(APPEND PLATFORM_LIBS ZLIB::ZLIB)
  list(APPEND CHDR_DEFINES CHDR_SYSTEM_ZLIB)
else()
  if(NOT TARGET miniz)
    add_subdirectory(deps/miniz-3.1.2 EXCLUDE_FROM_ALL)
    # ESP ROMs export an older miniz that would otherwise capture this one
    # at link time; applied here rather than inside deps/ so a miniz bump
    # can't drop it. No-op off ESP-IDF.
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/EspRomMinizWorkaround.cmake)
    libchdr_apply_esp_rom_miniz_workaround(miniz)
    set(CHDR_NEEDS_MINIZ_RENAME TRUE)
  endif()
  list(APPEND CHDR_LIBS miniz)
endif()

# zstd
if (WITH_SYSTEM_ZSTD)
  find_package(zstd MODULE REQUIRED)
  list(APPEND PLATFORM_LIBS zstd::zstd)
  list(APPEND CHDR_DEFINES CHDR_SYSTEM_ZSTD)
else()
  if(NOT TARGET zstd)
    add_subdirectory(deps/zstd-1.5.7 EXCLUDE_FROM_ALL)
  endif()
  list(APPEND CHDR_LIBS zstd)
endif()

#--------------------------------------------------
# options
#--------------------------------------------------

if(CHDR_WANT_RAW_DATA_SECTOR)
  list(APPEND CHDR_DEFINES WANT_RAW_DATA_SECTOR=1)
else()
  list(APPEND CHDR_DEFINES WANT_RAW_DATA_SECTOR=0)
endif()

if(CHDR_WANT_SUBCODE)
  list(APPEND CHDR_DEFINES WANT_SUBCODE=1)
else()
  list(APPEND CHDR_DEFINES WANT_SUBCODE=0)
endif()

# Only pass it through when the user actually set it; otherwise chdconfig.h's
# own default (follow LOWRAM_TARGET) applies. Defining it unconditionally here
# would silently override that.
if(NOT CHDR_CD_SCRATCH_BUFFER STREQUAL "")
  if(CHDR_CD_SCRATCH_BUFFER)
    list(APPEND CHDR_DEFINES CHDR_CD_SCRATCH_BUFFER=1)
  else()
    list(APPEND CHDR_DEFINES CHDR_CD_SCRATCH_BUFFER=0)
  endif()
endif()

if(CHDR_VERIFY_BLOCK_CRC)
  # The per-hunk CRC chdman stores covers the fully reconstituted hunk, ECC and
  # sync header included, so a build that skips ecc_generate() cannot match it.
  # The failure is content-dependent - hunks holding only audio frames have no
  # ECC to regenerate and still verify - which makes it look like sporadic file
  # corruption rather than a build misconfiguration. Refuse the combination.
  if(NOT CHDR_WANT_RAW_DATA_SECTOR)
    message(FATAL_ERROR
      "CHDR_VERIFY_BLOCK_CRC=ON requires CHDR_WANT_RAW_DATA_SECTOR=ON: without the "
      "regenerated ECC and sync header the decoded hunk cannot match the CRC stored "
      "in the file. Turn CHDR_VERIFY_BLOCK_CRC off as well to build without raw sectors.")
  endif()
  # Same reasoning for the subcode: the stored CRC covers all 2448 bytes of each
  # frame, so leaving the 96 subcode bytes unwritten cannot match it either. This
  # one is worse to diagnose - the bytes keep whatever the caller had in the
  # output buffer, so it passes against a freshly mmapped (zero) buffer and fails
  # once the caller reuses one, and it reports CHDERR_DECOMPRESSION_ERROR on data
  # that is perfectly good.
  if(NOT CHDR_WANT_SUBCODE)
    message(FATAL_ERROR
      "CHDR_VERIFY_BLOCK_CRC=ON requires CHDR_WANT_SUBCODE=ON: the stored CRC covers "
      "the subcode bytes too, so a build that does not write them cannot match it. "
      "Turn CHDR_VERIFY_BLOCK_CRC off as well to build without subcode.")
  endif()
  list(APPEND CHDR_DEFINES VERIFY_BLOCK_CRC=1)
else()
  list(APPEND CHDR_DEFINES VERIFY_BLOCK_CRC=0)
endif()

if(CHDR_LOWRAM_TARGET)
  list(APPEND CHDR_DEFINES LOWRAM_TARGET=1)
else()
  list(APPEND CHDR_DEFINES LOWRAM_TARGET=0)
endif()

#--------------------------------------------------
# chdr
#--------------------------------------------------

if(CHDR_FLAC_BACKEND STREQUAL "microflac")
  enable_language(CXX)
  set(CHDR_FLAC_BACKEND_SOURCE src/libchdr_flac_microflac.cpp)
  if(CHDR_MICROFLAC_SOURCE_DIR)
    set(_microflac_dir "${CHDR_MICROFLAC_SOURCE_DIR}")
  else()
    # Length and character class separately: CMake's regex engine has no {n}
    # repetition, so "[0-9a-f]{40}" silently matches nothing and would reject
    # every valid SHA.
    string(LENGTH "${CHDR_MICROFLAC_GIT_TAG}" _chdr_microflac_tag_len)
    if(CHDR_MICROFLAC_GIT_TAG AND (NOT _chdr_microflac_tag_len EQUAL 40
        OR NOT CHDR_MICROFLAC_GIT_TAG MATCHES "^[0-9a-fA-F]+$"))
      message(FATAL_ERROR
        "CHDR_MICROFLAC_GIT_TAG must be a 40-character commit SHA, got: "
        "${CHDR_MICROFLAC_GIT_TAG}")
    endif()
    if(NOT CHDR_MICROFLAC_GIT_TAG)
      message(FATAL_ERROR
        "CHDR_FLAC_BACKEND=microflac needs either CHDR_MICROFLAC_SOURCE_DIR (a local "
        "checkout) or CHDR_MICROFLAC_GIT_TAG (a 40-character commit SHA). Branch names "
        "are deliberately not accepted: the backend is compiled against micro-flac's "
        "internals, so the build must be reproducible.")
    endif()
    include(FetchContent)
    FetchContent_Declare(microflac
      GIT_REPOSITORY https://github.com/esphome-libs/micro-flac.git
      GIT_TAG ${CHDR_MICROFLAC_GIT_TAG})
    FetchContent_MakeAvailable(microflac)
    set(_microflac_dir "${microflac_SOURCE_DIR}")
  endif()
  set(CHDR_MICROFLAC_SOURCES
    ${_microflac_dir}/src/flac_decoder.cpp
    ${_microflac_dir}/src/decorrelation.cpp
    ${_microflac_dir}/src/frame_header.cpp
    ${_microflac_dir}/src/pcm_packing.cpp
    ${_microflac_dir}/src/crc.cpp
    ${_microflac_dir}/src/lpc.cpp)
  list(APPEND CHDR_DEFINES CHDR_FLAC_BACKEND_MICROFLAC MICRO_FLAC_DISABLE_OGG)
elseif(CHDR_FLAC_BACKEND STREQUAL "drflac")
  set(CHDR_FLAC_BACKEND_SOURCE src/libchdr_flac.c)
  set(CHDR_MICROFLAC_SOURCES "")
  set(_microflac_dir "")
else()
  message(FATAL_ERROR "CHDR_FLAC_BACKEND must be drflac or microflac, got: ${CHDR_FLAC_BACKEND}")
endif()

set(CHDR_SOURCES
  src/libchdr_bitstream.c
  src/libchdr_cdrom.c
  src/libchdr_chd.c
  src/libchdr_codec_avhuff.c
  src/libchdr_codec_cdfl.c
  src/libchdr_codec_cdlz.c
  src/libchdr_codec_cdzl.c
  src/libchdr_codec_cdzs.c
  src/libchdr_codec_flac.c
  src/libchdr_codec_huff.c
  src/libchdr_codec_lzma.c
  src/libchdr_codec_zlib.c
  src/libchdr_codec_zstd.c
  ${CHDR_FLAC_BACKEND_SOURCE}
  src/libchdr_huffman.c
  ${CHDR_MICROFLAC_SOURCES}
)


# Applies the micro-flac include paths and the C++ settings that keep its
# runtime footprint down. -fno-exceptions/-fno-rtti are what let a bare-metal
# link avoid pulling in libstdc++'s exception and RTTI machinery, and
# -fno-threadsafe-statics drops the __cxa_guard_* dependency.
function(chdr_apply_microflac target)
  if(NOT CHDR_FLAC_BACKEND STREQUAL "microflac")
    return()
  endif()
  # Public include dir only: micro-flac's src/ contains a crc.h that will
  # shadow a consumer's own header of that name, and its sources reach their
  # private headers relative to themselves anyway.
  target_include_directories(${target} PRIVATE
    ${_microflac_dir}/include)
  target_compile_options(${target} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
    $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>)
endfunction()

add_library(chdr-static STATIC ${CHDR_SOURCES})
target_include_directories(chdr-static INTERFACE include)
chdr_apply_microflac(chdr-static)
target_link_libraries(chdr-static PRIVATE ${CHDR_LIBS} ${PLATFORM_LIBS})
# libchdr_codec_zlib.c calls tinfl_decompress() directly, so it must be renamed
# alongside miniz's definition - otherwise on ESP-IDF this call binds to the
# ROM's older copy and we are back to the split decoder this branch fixed.
if(CHDR_NEEDS_MINIZ_RENAME)
  libchdr_apply_esp_rom_miniz_workaround(chdr-static)
endif()
# PRIVATE, deliberately. These macros are read by the installed headers as well
# as by the sources, so anything compiled against those headers has to agree
# with how the library was built - but pushing them onto consumers with PUBLIC
# injects unnamespaced names (WANT_SUBCODE, VERIFY_BLOCK_CRC, LOWRAM_TARGET)
# into every downstream compile, where they can collide. The tests in this tree
# are the only thing here that needs them, and tests/CMakeLists.txt gives them
# to those targets directly. The general fix is a generated header recording
# what the library was actually built with; until then a consumer that changes
# a default must define it on their own compile too.
target_compile_definitions(chdr-static PRIVATE ${CHDR_DEFINES})

if(MSVC)
  target_compile_definitions(chdr-static PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

if (INSTALL_STATIC_LIBS)
  install(TARGETS chdr-static ${CHDR_LIBS}
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  )
endif()

if (BUILD_SHARED_LIBS)
  add_library(chdr SHARED ${CHDR_SOURCES})
  target_include_directories(chdr INTERFACE include)
  chdr_apply_microflac(chdr)
  target_link_libraries(chdr PRIVATE ${CHDR_LIBS} ${PLATFORM_LIBS})
  if(CHDR_NEEDS_MINIZ_RENAME)
    libchdr_apply_esp_rom_miniz_workaround(chdr)
  endif()
  target_compile_definitions(chdr PRIVATE ${CHDR_DEFINES})

  if(MSVC)
    target_compile_definitions(chdr PUBLIC "CHD_DLL")
    target_compile_definitions(chdr PRIVATE "CHD_DLL_EXPORTS")
    target_compile_definitions(chdr PRIVATE _CRT_SECURE_NO_WARNINGS)
  elseif(APPLE)
    target_link_libraries(chdr PRIVATE -Wl,-dead_strip -Wl,-exported_symbol,_chd_*)
  else()
    include(CheckLinkerFlag)
    check_linker_flag(C "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/link.T" LINKER_VERSION_SCRIPT_SUPPORTED)
    if(LINKER_VERSION_SCRIPT_SUPPORTED)
      target_link_options(chdr PRIVATE "LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/link.T")
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL OpenBSD)
      target_link_libraries(chdr PRIVATE -Wl,--no-undefined)
    endif()
  endif()

  set_target_properties(chdr PROPERTIES C_VISIBILITY_PRESET hidden)
  set_target_properties(chdr PROPERTIES VISIBILITY_INLINES_HIDDEN 1)
  set_target_properties(chdr PROPERTIES PUBLIC_HEADER "include/libchdr/bitstream.h;include/libchdr/cdrom.h;include/libchdr/chd.h;include/libchdr/chdconfig.h;include/libchdr/coretypes.h;include/libchdr/huffman.h;include/libchdr/macros.h")
  set_target_properties(chdr PROPERTIES VERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}" SOVERSION ${PROJECT_VERSION_MAJOR})

  if (CMAKE_BUILD_TYPE MATCHES Release)
    #add_custom_command(TARGET chdr POST_BUILD COMMAND ${CMAKE_STRIP} libchdr.so)
  endif (CMAKE_BUILD_TYPE MATCHES Release)

  install(TARGETS chdr
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    PUBLIC_HEADER DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/libchdr"
  )

  configure_file(pkg-config.pc.in ${CMAKE_BINARY_DIR}/libchdr.pc @ONLY)
  install(FILES ${CMAKE_BINARY_DIR}/libchdr.pc DESTINATION "${CMAKE_INSTALL_LIBDIR}/pkgconfig")
endif()

if (CHDR_WANT_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
