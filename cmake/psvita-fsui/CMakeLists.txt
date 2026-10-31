cmake_minimum_required(VERSION 3.16)

project(armsx_psvita_fsui LANGUAGES C CXX)

if(NOT DEFINED VITASDK)
    if(DEFINED ENV{VITASDK})
        set(VITASDK "$ENV{VITASDK}")
    else()
        message(FATAL_ERROR "VITASDK must be set for PSVita FSUI builds.")
    endif()
endif()

include_directories(BEFORE
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${VITASDK}/arm-vita-eabi/include"
    "${VITASDK}/arm-vita-eabi/include/SDL2"
)

add_library(SDL2::SDL2 INTERFACE IMPORTED GLOBAL)
target_include_directories(SDL2::SDL2 INTERFACE
    "${VITASDK}/arm-vita-eabi/include"
    "${VITASDK}/arm-vita-eabi/include/SDL2"
)
target_link_directories(SDL2::SDL2 INTERFACE "${VITASDK}/arm-vita-eabi/lib")
target_link_libraries(SDL2::SDL2 INTERFACE
    SDL2
    SceGxm_stub
    SceDisplay_stub
    SceCtrl_stub
    SceAppMgr_stub
    SceAppUtil_stub
    SceAudio_stub
    SceAudioIn_stub
    SceSysmodule_stub
    SceIofilemgr_stub
    SceCommonDialog_stub
    SceTouch_stub
    SceHid_stub
    SceMotion_stub
    ScePower_stub
    SceProcessmgr_stub
    m
)

add_compile_definitions(IMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS)
add_compile_options(-marm)

set(FSUI_BUILD_SAMPLES OFF CACHE BOOL "Build the fsui sample applications" FORCE)
set(FSUI_ENABLE_INSTALL OFF CACHE BOOL "Enable install/export rules for the fsui package" FORCE)
set(FSUI_PLATFORM_BACKEND SDL2 CACHE STRING "Platform backend to build" FORCE)
set(FSUI_IMGUI_OPENGL_ES3 ON CACHE BOOL "Build the fsui OpenGL backend in OpenGL ES 3 mode" FORCE)
set(FSUI_USE_SYSTEM_SDL2 OFF CACHE BOOL "Prefer a system SDL2 package when an SDL target is not already available" FORCE)

add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../../third_party/fsui-lib" "${CMAKE_BINARY_DIR}/fsui-lib")

if(DEFINED VITASDK AND VITASDK)
    # Vita packaging tools reject PIC-style ARM relocations such as R_ARM_BASE_PREL.
    # Keep the upstream fsui source tree untouched, but force the Vita-facing targets
    # to build as plain static libraries.
    set(_FSUI_VITA_TARGETS
        fsui_imgui
        fsui-core
        fsui-platform-sdl2
        fsui-renderer-opengl
        fsui-renderer-sdl2
        fsui-renderer-sdl2surface
        fsui-backend-sdl
        fsui-donor
        fsui_resources
        fsui_glad
        glad
    )

    foreach(_FSUI_VITA_TARGET IN LISTS _FSUI_VITA_TARGETS)
        if(TARGET ${_FSUI_VITA_TARGET})
            set_target_properties(${_FSUI_VITA_TARGET} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
        endif()
    endforeach()
endif()
