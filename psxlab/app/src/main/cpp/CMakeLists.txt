cmake_minimum_required(VERSION 3.22)
project(psxdisc LANGUAGES C CXX)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(vendor/libchdr)
add_library(psxdisc SHARED chd_jni.c)
target_link_libraries(psxdisc PRIVATE chdr-static)
target_compile_options(psxdisc PRIVATE -Wall -Wextra -Werror)

# Independently replaceable LGPL DSP module; full corresponding sources are vendored.
file(GLOB TEMPO_SOURCES CONFIGURE_DEPENDS vendor/soundtouch/source/SoundTouch/*.cpp)
add_library(psxtempo SHARED tempo_jni.cpp ${TEMPO_SOURCES})
target_include_directories(psxtempo PRIVATE vendor/soundtouch/include)
target_compile_features(psxtempo PRIVATE cxx_std_17)
target_compile_definitions(psxtempo PRIVATE SOUNDTOUCH_FLOAT_SAMPLES)
if(ANDROID_ABI STREQUAL "arm64-v8a")
  target_compile_definitions(psxtempo PRIVATE SOUNDTOUCH_USE_NEON)
endif()
target_compile_options(psxtempo PRIVATE -O3 -ffast-math)

add_library(psxgpu SHARED raster_jni.cpp texture_hash.cpp hardware_gpu.cpp vulkan_output.cpp vulkan_gpu.cpp vulkan_gpu_jni.cpp shader_chain.cpp shader_jni.cpp frame_generation_jni.cpp)
target_link_libraries(psxgpu PRIVATE EGL GLESv3 android vulkan log dl)
target_compile_features(psxgpu PRIVATE cxx_std_17)
target_compile_options(psxgpu PRIVATE -O3 -fwrapv -Wall -Wextra)

add_library(psxcore SHARED gte_jni.cpp spu_jni.cpp cpu_jni.cpp cpu_arm64.cpp mdec_jni.cpp)
target_compile_features(psxcore PRIVATE cxx_std_17)
target_compile_options(psxcore PRIVATE -O3 -fwrapv -Wall -Wextra)

# Optional MPL-2.0 postprocessing runtime. Standalone builds without it retain plain rendering.
target_compile_definitions(psxgpu PRIVATE ARMSX_ENABLE_VULKAN)
set(PSXLAB_LIBRA_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/librashader/include")
if(EXISTS "${PSXLAB_LIBRA_INCLUDE}/librashader_min.h")
    target_include_directories(psxgpu PRIVATE "${PSXLAB_LIBRA_INCLUDE}")
    target_compile_definitions(psxgpu PRIVATE ARMSX_ENABLE_SHADERS)
endif()

# RetroAchievements for the ArmSX1 host. The vendored MIT rcheevos lives in the ArmSX1 checkout;
# standalone PSX Lab builds without it simply omit the module.
set(PSXLAB_RCHEEVOS "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../third_party/rcheevos")
if(EXISTS "${PSXLAB_RCHEEVOS}/include/rc_client.h")
    file(GLOB PSXLAB_RC_SOURCES CONFIGURE_DEPENDS "${PSXLAB_RCHEEVOS}/src/rcheevos/*.c" "${PSXLAB_RCHEEVOS}/src/rapi/*.c")
    add_library(psxlab_rcheevos STATIC ${PSXLAB_RC_SOURCES}
        "${PSXLAB_RCHEEVOS}/src/rc_client.c" "${PSXLAB_RCHEEVOS}/src/rc_compat.c"
        "${PSXLAB_RCHEEVOS}/src/rc_util.c" "${PSXLAB_RCHEEVOS}/src/rc_version.c"
        "${PSXLAB_RCHEEVOS}/src/rhash/cdreader.c" "${PSXLAB_RCHEEVOS}/src/rhash/hash.c"
        "${PSXLAB_RCHEEVOS}/src/rhash/hash_disc.c" "${PSXLAB_RCHEEVOS}/src/rhash/md5.c")
    target_include_directories(psxlab_rcheevos PUBLIC "${PSXLAB_RCHEEVOS}/include" PRIVATE "${PSXLAB_RCHEEVOS}/src")
    target_compile_definitions(psxlab_rcheevos PUBLIC RC_NO_THREADS=1 RC_HASH_NO_ENCRYPTED RC_HASH_NO_ROM RC_HASH_NO_ZIP)
    target_compile_options(psxlab_rcheevos PRIVATE -O2 -fno-fast-math)
    add_library(psxachievements SHARED achievements_jni.cpp)
    target_compile_features(psxachievements PRIVATE cxx_std_17)
    target_compile_options(psxachievements PRIVATE -O2 -Wall -Wextra)
    target_link_libraries(psxachievements PRIVATE psxlab_rcheevos log)
    target_link_options(psxachievements PRIVATE -Wl,--no-undefined)
endif()
