.ONESHELL:
.SHELLFLAGS := -ec

# ★ Pinned, not inherited. GNU make's default goal is whichever target it happens to read FIRST,
# so any rule added above `all:` silently becomes what a bare `make` builds. Adding the pgo-stamp
# helper below did exactly that — plain `make` started printing a hash instead of building the
# emulator, and nothing complained, because ./build.sh names its goals explicitly and every test
# gate does too. Pinning it here makes where a rule is written irrelevant.
.DEFAULT_GOAL := all

WASM_TARGET := $(filter wasm,$(MAKECMDGOALS))

ifeq ($(WASM_TARGET),wasm)
	CC := emcc
	CXX := em++
	SDL_STATIC := 0
	SDL_CFLAGS := -sUSE_SDL=2
	SDL_LIBS_DYNAMIC :=
	SDL_LIBS_STATIC :=
	WASM_LDFLAGS := -sUSE_SDL=2 -sALLOW_MEMORY_GROWTH=1 -sASSERTIONS=1 -sFORCE_FILESYSTEM=1 -sFULL_ES3=1 -sMIN_WEBGL_VERSION=2 -sMAX_WEBGL_VERSION=2 -sNO_EXIT_RUNTIME=0 -sEXPORTED_RUNTIME_METHODS='["FS","ccall","cwrap"]' -sEXPORTED_FUNCTIONS='["_main","_psxe_run","_external_main","_external_main_ex","_psxe_wasm_on_file","_psxe_wasm_on_error","_psxe_enqueue_launch_argument"]' --preload-file icons@/icons
endif

SDL_CONFIG ?= sdl2-config

CC ?= gcc
CXX ?= g++
AR := /usr/bin/ar
RANLIB := /usr/bin/ranlib

SDL_CFLAGS ?= $(shell $(SDL_CONFIG) --cflags 2>/dev/null)
SDL_LIBS_DYNAMIC ?= $(shell $(SDL_CONFIG) --libs 2>/dev/null)
SDL_LIBS_STATIC ?= $(shell $(SDL_CONFIG) --static-libs 2>/dev/null || $(SDL_CONFIG) --libs --static 2>/dev/null)
SDL_STATIC ?= 1
WASM_LDFLAGS ?=
USE_CHD ?= 1
HW_DEBUG ?= 0

# Presentation backends (frontend/render*.cpp). The SDL_Renderer backend is always built.
#   ARMSX_ENABLE_GL      GLES 3.0 / GL 3.3-core present path. Needs nothing but SDL at build
#                        time (entry points come from SDL_GL_GetProcAddress); on Android it
#                        additionally binds EGL to the host ANativeWindow, which needs -lEGL.
#   ARMSX_ENABLE_VULKAN  Vulkan present path. EXPERIMENTAL and compile-verified only. Needs
#                        <vulkan/vulkan.h> at build time; libvulkan is dlopen()ed at runtime
#                        through a replaceable loader, so there is no -lvulkan.
#   ARMSX_ENABLE_SHADERS RetroArch (.slangp) shader chains via librashader
#                        (frontend/render_shaders.cpp). Adds NO link-time dependency:
#                        third_party/librashader/prebuilt/arm64-v8a/liblibrashader_capi.so is
#                        dlopen()ed, so `llvm-readelf -d bin/libarmsx.so` gains nothing and a
#                        missing librashader degrades to plain presentation. Forced off below
#                        when ARMSX_ENABLE_VULKAN is off — librashader has no GLES runtime, so
#                        there would be no present path to hook.
ARMSX_ENABLE_GL ?= 1
ARMSX_ENABLE_VULKAN ?= 0
ARMSX_ENABLE_SHADERS ?= 1

PLATFORM := $(shell uname -s)
ifeq ($(WASM_TARGET),wasm)
PLATFORM := Emscripten
endif
IOS_TARGET ?= 0
IOS_SDK ?= iphoneos
IOS_DEPLOYMENT_TARGET ?= 14.0
MACOS_DEPLOYMENT_TARGET ?= 10.15
WINDOWS_TARGET ?= 0
UWP_TARGET ?= 0
PSVITA_TARGET ?= 0
VITASDK ?=
IOS_SDL_FRAMEWORK ?= $(CURDIR)/ios/Frameworks/SDL2.xcframework/ios-arm64/SDL2.framework
IOS_SDL_FRAMEWORK_PARENT := $(dir $(IOS_SDL_FRAMEWORK))
SDKROOT ?=
CONTROLLER_GENERIC ?= 0
FSUI_DIR := third_party/fsui-lib
FSUI_BUILD_DIR ?= build/fsui/native
WASM_HTML_POSTPROCESS := web/postprocess_web_html.cmake
FSUI_LINK_SYSTEM_GL ?= 1

ifeq ($(PSVITA_TARGET),1)
	ifeq ($(strip $(VITASDK)),)
$(error VITASDK must be set when PSVITA_TARGET=1)
	endif
	SDL_CONFIG := $(VITASDK)/bin/arm-vita-eabi-pkg-config
	CC := $(VITASDK)/bin/arm-vita-eabi-gcc
	CXX := $(VITASDK)/bin/arm-vita-eabi-g++
	AR := $(VITASDK)/bin/arm-vita-eabi-ar
	RANLIB := $(VITASDK)/bin/arm-vita-eabi-ranlib
	PLATFORM := Vita
	SDL_STATIC := 1
endif

ifeq ($(IOS_TARGET),1)
	SDKROOT ?= $(shell xcrun --sdk $(IOS_SDK) --show-sdk-path)
	CC ?= $(shell xcrun --sdk $(IOS_SDK) --find clang)
	CXX ?= $(shell xcrun --sdk $(IOS_SDK) --find clang++)

	# Force dynamic SDL when targeting iOS
	SDL_STATIC := 0
	SDL_CFLAGS := -isysroot $(SDKROOT) -arch arm64 -miphoneos-version-min=$(IOS_DEPLOYMENT_TARGET) \
		-F$(IOS_SDL_FRAMEWORK_PARENT) -I$(IOS_SDL_FRAMEWORK)/Headers -DIOS_TARGET -D_THREAD_SAFE
	SDL_LIBS_DYNAMIC := -isysroot $(SDKROOT) -arch arm64 -miphoneos-version-min=$(IOS_DEPLOYMENT_TARGET) \
		-F$(IOS_SDL_FRAMEWORK_PARENT) -framework SDL2
	SDL_LIBS_STATIC :=
endif

BASE_CFLAGS = -g -DLOG_USE_COLOR -I"." -I"psx" $(SDL_CFLAGS)
BASE_CFLAGS += -O3 -ffast-math -Wno-overflow -Wall -pedantic -Wno-address-of-packed-member -flto

FSUI_INCLUDE_FLAGS = \
	-I$(FSUI_DIR)/include \
	-I$(FSUI_DIR)/third_party/imgui \
	-I$(FSUI_DIR)/third_party/imgui/backends \
	-I$(FSUI_DIR)/third_party/stb

ifneq ($(WASM_TARGET),wasm)
FSUI_INCLUDE_FLAGS += -I$(FSUI_BUILD_DIR)/gladsources/fsui_glad/include
endif

FSUI_COMPILE_DEFS = -DIMGUI_FRONTEND -DFSUI_HAS_SDL2_PLATFORM -DFSUI_HAS_SDL2SURFACE_RENDERER -DFSUI_HAS_SDL2RENDERER_RENDERER -DSDL_MAIN_HANDLED

LIBCHDR_DIR := third_party/libchdr
LIBCHDR_BUILD_DIR :=
LIBCHDR_INCLUDE_FLAGS :=
LIBCHDR_INPUTS :=
LIBCHDR_ARCHIVE :=
LIBCHDR_LIBS :=
LIBCHDR_CONFIGURE :=
LIBCHDR_CMAKE_ARGS :=
CHD_COMPILE_DEFS :=
CHD_BUILD_DEPS :=
CHD_LINK_LIBS :=
ifeq ($(USE_CHD),1)
	ifeq ($(WASM_TARGET),wasm)
		LIBCHDR_BUILD_DIR := build/libchdr/wasm
	else ifeq ($(PSVITA_TARGET),1)
		LIBCHDR_BUILD_DIR := build/libchdr/psvita
	else ifeq ($(IOS_TARGET),1)
		LIBCHDR_BUILD_DIR := build/libchdr/ios
	else
		LIBCHDR_BUILD_DIR := build/libchdr/native
	endif

	LIBCHDR_INCLUDE_FLAGS := -I$(LIBCHDR_DIR)/include -I$(LIBCHDR_DIR)/deps/miniz-3.1.1
	LIBCHDR_INPUTS := $(shell find $(LIBCHDR_DIR) -type f 2>/dev/null)
	LIBCHDR_ARCHIVE := $(LIBCHDR_BUILD_DIR)/libchdr-static.a
	LIBCHDR_LIBS := \
		$(LIBCHDR_ARCHIVE) \
		$(LIBCHDR_BUILD_DIR)/deps/lzma-25.01/libchdr-lzma.a \
		$(LIBCHDR_BUILD_DIR)/deps/miniz-3.1.1/libminiz.a \
		$(LIBCHDR_BUILD_DIR)/deps/zstd-1.5.7/libzstd.a
	LIBCHDR_CONFIGURE := cmake
	ifeq ($(WASM_TARGET),wasm)
		LIBCHDR_CONFIGURE := emcmake cmake
	endif

	LIBCHDR_CMAKE_ARGS := -S $(LIBCHDR_DIR) -B $(LIBCHDR_BUILD_DIR) -DBUILD_SHARED_LIBS=OFF -DCHDR_WANT_RAW_DATA_SECTOR=ON -DCHDR_WANT_SUBCODE=ON -DMINIZ_ARCHIVE_APIS=ON -DMINIZ_STDIO=ON -DCMAKE_BUILD_TYPE=Release
	# Extra cmake args injectable per-platform (e.g. Android passes -DCMAKE_SYSTEM_NAME=Linux so the
	# host-macOS CMake stops adding an unsupported -arch flag to the NDK clang cross-build).
	LIBCHDR_CMAKE_ARGS += $(LIBCHDR_EXTRA_CMAKE_ARGS)
	ifneq ($(strip $(CC)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_C_COMPILER=$(CC)
	endif
	ifneq ($(strip $(CXX)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_CXX_COMPILER=$(CXX)
	endif
	ifneq ($(strip $(AR)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_AR=$(AR)
	endif
	ifneq ($(strip $(RANLIB)),)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_RANLIB=$(RANLIB)
	endif
	ifeq ($(PLATFORM),Darwin)
	ifneq ($(IOS_TARGET),1)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_OSX_DEPLOYMENT_TARGET=$(MACOS_DEPLOYMENT_TARGET)
	endif
	endif
	ifeq ($(IOS_TARGET),1)
		LIBCHDR_CMAKE_ARGS += -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=$(IOS_DEPLOYMENT_TARGET) -DCMAKE_OSX_SYSROOT=$(SDKROOT)
	endif

	CHD_COMPILE_DEFS := -DUSE_CHD
	CHD_BUILD_DEPS := $(LIBCHDR_ARCHIVE)
	CHD_LINK_LIBS := $(LIBCHDR_LIBS)
endif

# RetroAchievements (rcheevos, vendored). RC_NO_THREADS: frontend/achievements.cpp serialises
# every rc_client call behind its own lock, so the library's optional threading is dead weight.
# The hashers we do NOT need are compiled out; RC_HASH_NO_DISC is deliberately absent, because
# the PS1 disc hash (src/rhash/hash_disc.c) is exactly what game identification runs on.
RCHEEVOS_DIR := third_party/rcheevos
RCHEEVOS_INCLUDE_FLAGS := -I$(RCHEEVOS_DIR)/include
RCHEEVOS_COMPILE_DEFS := -DRC_NO_THREADS=1 -DRC_HASH_NO_ENCRYPTED -DRC_HASH_NO_ROM -DRC_HASH_NO_ZIP

BASE_CXXFLAGS = -std=c++20 $(BASE_CFLAGS) $(FSUI_INCLUDE_FLAGS) $(FSUI_COMPILE_DEFS)
BASE_CFLAGS += $(LIBCHDR_INCLUDE_FLAGS) $(CHD_COMPILE_DEFS)
BASE_CXXFLAGS += $(LIBCHDR_INCLUDE_FLAGS) $(CHD_COMPILE_DEFS)
BASE_CFLAGS += $(RCHEEVOS_INCLUDE_FLAGS) $(RCHEEVOS_COMPILE_DEFS)
BASE_CXXFLAGS += $(RCHEEVOS_INCLUDE_FLAGS) $(RCHEEVOS_COMPILE_DEFS)

# adrenotools (BSD 2-Clause, third_party/libadrenotools). Set by build.sh's android target, which
# builds the static lib and stages the hook .so files. Empty everywhere else: the loader compiles
# to its dlopen fallback when ARMSX_HAVE_ADRENOTOOLS is undefined.
ADRENOTOOLS_FLAGS ?=
BASE_CFLAGS += $(ADRENOTOOLS_FLAGS)
BASE_CXXFLAGS += $(ADRENOTOOLS_FLAGS)

ifeq ($(CONTROLLER_GENERIC),1)
	BASE_CFLAGS += -DCONTROLLER_GENERIC
	BASE_CXXFLAGS += -DCONTROLLER_GENERIC
endif

BASE_CFLAGS += -DUSE_HARDWARE
BASE_CXXFLAGS += -DUSE_HARDWARE

# The Vita has no GL/Vulkan surface we can reach through SDL, and the wasm target is served
# by SDL's own GL emulation; keep both on the SDL backend.
ifeq ($(PSVITA_TARGET),1)
	ARMSX_ENABLE_GL := 0
	ARMSX_ENABLE_VULKAN := 0
endif
ifeq ($(WASM_TARGET),wasm)
	ARMSX_ENABLE_GL := 0
	ARMSX_ENABLE_VULKAN := 0
endif

ifeq ($(ARMSX_ENABLE_GL),1)
	BASE_CFLAGS += -DARMSX_ENABLE_GL
	BASE_CXXFLAGS += -DARMSX_ENABLE_GL
endif

ifeq ($(ARMSX_ENABLE_VULKAN),1)
	BASE_CFLAGS += -DARMSX_ENABLE_VULKAN
	BASE_CXXFLAGS += -DARMSX_ENABLE_VULKAN
else
	# librashader has no GLES runtime and its C API is typed in Vulkan handles, so without
	# the Vulkan present path there is nothing to hook and no <vulkan/vulkan.h> to include.
	# frontend/render_shaders.cpp then compiles to the inert stubs, which still LINK and
	# still return safe values ("[]" and false) — android_jni.cpp calls them unconditionally.
	ARMSX_ENABLE_SHADERS := 0
endif

ifeq ($(ARMSX_ENABLE_SHADERS),1)
	BASE_CFLAGS += -DARMSX_ENABLE_SHADERS
	BASE_CXXFLAGS += -DARMSX_ENABLE_SHADERS
endif

ifeq ($(HW_DEBUG),1)
	BASE_CFLAGS += -DHW_DEBUG
	BASE_CXXFLAGS += -DHW_DEBUG
endif

ifeq ($(UWP_TARGET),1)
		BASE_CFLAGS += -DUWP_TARGET
		BASE_CXXFLAGS += -DUWP_TARGET
endif

ifeq ($(PSVITA_TARGET),1)
	BASE_CFLAGS += -DPSVITA_TARGET -D__DLL_BUILD -I$(VITASDK)/arm-vita-eabi/include -I$(VITASDK)/arm-vita-eabi/include/SDL2 -marm
	BASE_CXXFLAGS += -DPSVITA_TARGET -D__DLL_BUILD -I$(VITASDK)/arm-vita-eabi/include -I$(VITASDK)/arm-vita-eabi/include/SDL2 -marm
endif

ifeq ($(WASM_TARGET),wasm)
		BASE_CFLAGS := $(filter-out -flto,$(BASE_CFLAGS))
		BASE_CXXFLAGS := $(filter-out -flto,$(BASE_CXXFLAGS))
endif

ifeq ($(PSVITA_TARGET),1)
	BASE_CFLAGS := $(filter-out -flto,$(BASE_CFLAGS))
	BASE_CXXFLAGS := $(filter-out -flto,$(BASE_CXXFLAGS))
endif

ifeq ($(IOS_TARGET),1)
	BASE_CFLAGS += -fembed-bitcode
	BASE_CXXFLAGS += -fembed-bitcode
endif

ifeq ($(WASM_TARGET),wasm)
OS_INFO := Emscripten
else ifeq ($(PSVITA_TARGET),1)
OS_INFO := PS Vita
else ifneq ($(IOS_TARGET),1)
OS_INFO := $(shell uname -rmo)
else
OS_INFO := iOS
endif

ifeq ($(PLATFORM),Darwin)
ifneq ($(IOS_TARGET),1)
	BASE_CFLAGS += -mmacosx-version-min=$(MACOS_DEPLOYMENT_TARGET) -Wno-newline-eof
	BASE_CXXFLAGS += -mmacosx-version-min=$(MACOS_DEPLOYMENT_TARGET)
endif
endif

SHARED_EXT := .so
SHARED_LDFLAGS := -shared
SHARED_CFLAGS := $(BASE_CFLAGS) -D__DLL_BUILD -fPIC
SHARED_CXXFLAGS := $(BASE_CXXFLAGS) -D__DLL_BUILD -fPIC

ifeq ($(PLATFORM),Darwin)
	SHARED_EXT := .dylib
	SHARED_LDFLAGS := -dynamiclib
else ifeq ($(WINDOWS_TARGET),1)
	SHARED_EXT := .dll
endif

ifeq ($(IOS_TARGET),1)
	SHARED_LDFLAGS += -Wl,-install_name,@rpath/libarmsx$(SHARED_EXT)
	SHARED_CFLAGS += -DIOS_TARGET
	SHARED_CXXFLAGS += -DIOS_TARGET
endif

VERSION_TAG := $(shell git describe --always --tags --abbrev=0)
COMMIT_HASH := $(shell git rev-parse --short HEAD)

BIN_DIR := bin
ifeq ($(WASM_TARGET),wasm)
BIN_DIR := bin/wasm
else ifeq ($(PSVITA_TARGET),1)
BIN_DIR := bin/psvita
endif
OBJ_DIR := $(BIN_DIR)/obj

BIN      := $(BIN_DIR)/armsx
ifeq ($(WASM_TARGET),wasm)
BIN      := $(BIN_DIR)/armsx.html
else ifeq ($(WINDOWS_TARGET),1)
BIN      := $(BIN_DIR)/armsx.exe
endif
SHARED_BIN := $(BIN_DIR)/libarmsx$(SHARED_EXT)
VITA_NATIVE_LIB := $(BIN_DIR)/libarmsx_vita.a
RUNTIME_ICON_SRC := icons/FsuiAppIcon.png
RUNTIME_ICON_DEST := $(BIN_DIR)/icons/FsuiAppIcon.png
WINDRES ?= windres

C_SOURCES := $(wildcard psx/*.c) \
             $(wildcard psx/dev/*.c) \
             $(wildcard psx/dev/cdrom/*.c) \
             $(wildcard psx/input/*.c) \
             $(wildcard psx/disc/*.c) \
             frontend/argparse.c \
             frontend/config.c \
             frontend/diagnostics.c \
             frontend/toml.c
C_SOURCES := $(filter-out psx/dev/cdrom/chd.c,$(C_SOURCES))
C_SOURCES += frontend/gpu_hw.c
# Internal-resolution rasterizer backend ([video] renderer = "hardware"). Its whole body
# is behind #ifdef USE_HARDWARE, and it installs itself through psx/dev/gpu_backend.h, so
# the software path is untouched when the setting is off.
C_SOURCES += frontend/gpu_hw_rt.c
# GLES 3.0 rasterizer backend behind the same psx/dev/gpu_backend.h vtable. Its whole body
# is behind USE_HARDWARE + ARMSX_ENABLE_GL + __ANDROID__ and it compiles to three stubs
# everywhere else, so it is listed unconditionally like frontend/render_gl.cpp.
C_SOURCES += frontend/gpu_hw_gl.c
# frontend/gpu_profile.c identifies the mobile GPU and driver and answers "does this driver do
# X correctly". Unconditional and dependency-free (no SDL, no GL, no Vulkan headers) so the GL
# backend, the Vulkan backend and the hardware rasteriser can all consult one answer.
C_SOURCES += frontend/gpu_profile.c
# frontend/host_usage.c samples DEVICE cpu/ram/gpu for the OSD (see host_usage.h).
C_SOURCES += frontend/host_usage.c
# frontend/perf_hint.c is the ADPF CPU clock hint + emulation-thread affinity (see perf_hint.h).
# Listed unconditionally like gpu_profile.c: the whole body is behind __ANDROID__ and every entry
# point compiles to an empty function elsewhere, so no platform needs a conditional here.
C_SOURCES += frontend/perf_hint.c
# frontend/pgo.c is the profile-guided-optimisation seam (see the PGO block below, and the long
# comment at the top of the file). Listed unconditionally like gpu_profile.c: every entry point
# exists in all three PGO modes, and that is deliberate — a caller whose control flow changed
# between the instrumented build and the optimised build would lose its own profile to a CFG
# hash mismatch.
C_SOURCES += frontend/pgo.c
ifeq ($(USE_CHD),1)
C_SOURCES += psx/dev/cdrom/chd.c
endif
ifeq ($(PSVITA_TARGET),1)
C_SOURCES += frontend/vita_sdl_stubs.c
endif

# rcheevos. Listed file by file rather than wildcarded: rc_libretro.c, rc_client_external.c and
# rc_client_raintegration.c are upstream integrations ARMSX does not use, and hash_rom.c /
# hash_zip.c / hash_encrypted.c are compiled out by RCHEEVOS_COMPILE_DEFS above.
RCHEEVOS_SOURCES := \
	$(RCHEEVOS_DIR)/src/rc_client.c \
	$(RCHEEVOS_DIR)/src/rc_compat.c \
	$(RCHEEVOS_DIR)/src/rc_util.c \
	$(RCHEEVOS_DIR)/src/rc_version.c \
	$(RCHEEVOS_DIR)/src/rapi/rc_api_common.c \
	$(RCHEEVOS_DIR)/src/rapi/rc_api_editor.c \
	$(RCHEEVOS_DIR)/src/rapi/rc_api_info.c \
	$(RCHEEVOS_DIR)/src/rapi/rc_api_runtime.c \
	$(RCHEEVOS_DIR)/src/rapi/rc_api_user.c \
	$(RCHEEVOS_DIR)/src/rcheevos/alloc.c \
	$(RCHEEVOS_DIR)/src/rcheevos/condition.c \
	$(RCHEEVOS_DIR)/src/rcheevos/condset.c \
	$(RCHEEVOS_DIR)/src/rcheevos/consoleinfo.c \
	$(RCHEEVOS_DIR)/src/rcheevos/format.c \
	$(RCHEEVOS_DIR)/src/rcheevos/lboard.c \
	$(RCHEEVOS_DIR)/src/rcheevos/memref.c \
	$(RCHEEVOS_DIR)/src/rcheevos/operand.c \
	$(RCHEEVOS_DIR)/src/rcheevos/rc_validate.c \
	$(RCHEEVOS_DIR)/src/rcheevos/richpresence.c \
	$(RCHEEVOS_DIR)/src/rcheevos/runtime.c \
	$(RCHEEVOS_DIR)/src/rcheevos/runtime_progress.c \
	$(RCHEEVOS_DIR)/src/rcheevos/trigger.c \
	$(RCHEEVOS_DIR)/src/rcheevos/value.c \
	$(RCHEEVOS_DIR)/src/rhash/cdreader.c \
	$(RCHEEVOS_DIR)/src/rhash/hash.c \
	$(RCHEEVOS_DIR)/src/rhash/hash_disc.c \
	$(RCHEEVOS_DIR)/src/rhash/md5.c
C_SOURCES += $(RCHEEVOS_SOURCES)

C_SOURCES_SHARED := $(C_SOURCES)

# rcheevos back out of -ffast-math. Its float memref conversion (src/rcheevos/memref.c) produces
# and compares INFINITY/NaN, and -ffinite-math-only tells the compiler those cannot occur — which
# is exactly the assumption that would quietly mis-evaluate a float-typed achievement condition.
# Nothing here is hot enough to care.
RCHEEVOS_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(RCHEEVOS_SOURCES))
$(RCHEEVOS_OBJS): BASE_CFLAGS += -fno-fast-math

# frontend/android_jni.cpp is the in-process Android host (Compose-owned Surface + the
# kr.co.iefriends.pcsx2.NativeApp JNI surface). Its whole body is behind #if defined(__ANDROID__),
# so it compiles to an empty object everywhere else and is listed unconditionally.
# frontend/render*.cpp is the presentation backend layer (see frontend/render.h). render_gl
# and render_vk compile to empty objects unless ARMSX_ENABLE_GL / ARMSX_ENABLE_VULKAN are on,
# so they are listed unconditionally.
# frontend/achievements.cpp is the RetroAchievements (rcheevos) layer. Platform-neutral C++ —
# the Android host installs the HTTP transport into it, everything else links it inert.
# frontend/render_shaders.cpp is the librashader (.slangp) seam. Like render_gl/render_vk it
# is listed unconditionally and compiles to inert stubs without ARMSX_ENABLE_SHADERS. It adds
# no link-time dependency in EITHER configuration: librashader is dlopen()ed.
CPP_SOURCES := frontend/archive.cpp frontend/main.cpp frontend/android_jni.cpp \
               frontend/achievements.cpp \
               frontend/render.cpp frontend/render_sdl.cpp frontend/render_gl.cpp frontend/render_vk.cpp \
               frontend/render_shaders.cpp

FSUI_LIBS := \
	$(FSUI_BUILD_DIR)/libfsui-donor.a \
	$(FSUI_BUILD_DIR)/libfsui-backend-sdl.a \
	$(FSUI_BUILD_DIR)/libfsui-platform-sdl2.a \
	$(FSUI_BUILD_DIR)/libfsui-renderer-sdl2.a \
	$(FSUI_BUILD_DIR)/libfsui-renderer-sdl2surface.a \
	$(FSUI_BUILD_DIR)/libfsui-core.a \
	$(FSUI_BUILD_DIR)/libfsui_imgui.a \
	$(FSUI_BUILD_DIR)/libfsui_resources.a
ifeq ($(WASM_TARGET),wasm)
FSUI_LIBS += \
	$(FSUI_BUILD_DIR)/libfsui-renderer-opengl.a
else
FSUI_LIBS += \
	$(FSUI_BUILD_DIR)/libfsui-renderer-opengl.a \
	$(FSUI_BUILD_DIR)/libfsui_glad.a
endif

PLATFORM_EXTRA_LDFLAGS :=
PLATFORM_EXTRA_LIBS :=

ifeq ($(PLATFORM),Darwin)
	FSUI_LIBS += $(FSUI_BUILD_DIR)/libfsui-renderer-metal.a
	ifeq ($(IOS_TARGET),1)
		PLATFORM_EXTRA_LIBS += -framework Foundation -framework Metal -framework OpenGLES -framework QuartzCore -framework UIKit
	else
		PLATFORM_EXTRA_LDFLAGS += -mmacosx-version-min=$(MACOS_DEPLOYMENT_TARGET)
		PLATFORM_EXTRA_LIBS += -framework Cocoa -framework Foundation -framework IOKit -framework Metal -framework OpenGL -framework QuartzCore
	endif
else ifeq ($(WINDOWS_TARGET),1)
	PLATFORM_EXTRA_LIBS += -lsetupapi -limm32 -lversion -lwinmm -lgdi32 -lole32 -loleaut32 -lshell32 -luuid -lopengl32
ifneq ($(UWP_TARGET),1)
	PLATFORM_EXTRA_LIBS += -ldbghelp
endif
else ifneq ($(WASM_TARGET),wasm)
	PLATFORM_EXTRA_LIBS += -ldl
ifeq ($(FSUI_LINK_SYSTEM_GL),1)
	PLATFORM_EXTRA_LIBS += -lGL
endif
endif

C_OBJS := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES))
C_OBJS_SHARED := $(patsubst %.c,$(OBJ_DIR)/%.o,$(C_SOURCES_SHARED))
CPP_OBJS := $(patsubst %.cpp,$(OBJ_DIR)/%.o,$(CPP_SOURCES))
RC_SOURCES :=
ifeq ($(WINDOWS_TARGET),1)
RC_SOURCES += resources/windows/armsx.rc
endif
RC_OBJS := $(patsubst %.rc,$(OBJ_DIR)/%.o,$(RC_SOURCES))
ALL_OBJS := $(C_OBJS) $(CPP_OBJS) $(RC_OBJS)
ALL_OBJS_SHARED := $(C_OBJS_SHARED) $(CPP_OBJS)

# ================================================================================================
# Profile-guided optimisation (PGO)
# ================================================================================================
#
#   make PGO=generate shared     instrumented: counters compiled in, written on the device.
#   make PGO=use      shared     optimised against build/pgo/armsx.profdata.
#   make              shared     (PGO=off, the default) neither.
#
# ./build.sh android forwards PGO / PGO_PROFILE / PGO_ALLOW_STALE / PGO_STRICT straight through,
# and tools/pgo.sh drives the whole loop. Runtime behaviour — where the .profraw lands, and why
# it has to be written explicitly rather than at exit — is documented in frontend/pgo.c.
#
# IR instrumentation (-fprofile-generate), not frontend instrumentation
# (-fprofile-instr-generate): fewer counters, and it is the one that composes with the -flto in
# BASE_CFLAGS. The instrumentation and the profile annotation both run PRE-LINK, on the bitcode,
# so the compile line is what carries -fprofile-use; only -fprofile-generate additionally needs
# to reach the LINK line, to pull in compiler-rt's profile runtime.
#
# ── SCOPE ───────────────────────────────────────────────────────────────────────────────────────
# psx/ + frontend/ only. Everything else is either built by its own cmake (SDL, libchdr,
# librashader, adrenotools — untouched by these flags) or is rcheevos, which is listed in
# C_SOURCES but runs a handful of times a second at most: instrumenting it would add counters and
# profile bulk to code no amount of PGO will make matter. $(OBJ_DIR)/frontend/pgo.o is excluded
# too, and that one is REQUIRED, not a judgement call — it is the only file whose control flow
# legitimately differs between the instrumented and the optimised build, so feeding it its own
# profile would guarantee the hash mismatch this whole block exists to detect.
#
# ── STALENESS ───────────────────────────────────────────────────────────────────────────────────
# A profile that predates the code does not degrade to "no PGO", it degrades to "PGO pointed at
# the wrong branches", and it is measurably slower than not using one. The sibling PS2 project
# lost a large chunk of recompiler throughput to exactly this and it took real effort to find,
# because nothing said the profile was old. Three independent checks, on purpose:
#
#   1. HERE, before a single object compiles. The merged profile carries a provenance file
#      naming the commit AND a hash over psx/+frontend/ sources; if either disagrees with the
#      tree being built, this is a hard error. PGO_ALLOW_STALE=1 downgrades it to a warning.
#      The source hash is the stronger of the two: a commit hash cannot see uncommitted edits.
#   2. clang, per function. ★ The diagnostic is "function control flow change detected (hash
#      mismatch)" under -Wbackend-plugin. NOT -Wprofile-instr-out-of-date, which only covers
#      frontend instrumentation and would silently never fire for this build — assuming
#      otherwise is exactly how a stale profile gets waved through a build that "had no
#      warnings". PGO_STRICT=1 (the default) promotes it to an error.
#   3. Runtime, in logcat, from frontend/pgo.c's constructor — for a build already on a device.

PGO ?= off
PGO_DIR ?= build/pgo
PGO_PROFILE ?= $(PGO_DIR)/armsx.profdata
PGO_PROVENANCE ?= $(PGO_PROFILE).provenance
PGO_ALLOW_STALE ?= 0
PGO_STRICT ?= 1

PGO_CFLAGS :=
PGO_LDFLAGS :=
PGO_STAMP_DEFS :=

# Everything PGO is applied to, as a file list. $(wildcard) silently drops patterns that match
# nothing, which is what makes it safe to list a directory that may not exist — unlike a shell
# glob, which either errors or (worse, under zsh) expands to nothing and hands the hash below an
# EMPTY input, producing git's empty-blob hash. That value looks like a perfectly good stamp and
# compares equal to any other empty run, so the staleness guard would silently stop guarding.
# `make pgo-stamp` below is the only supported way to ask for this; do not reimplement it.
PGO_STAMP_INPUTS := $(sort $(wildcard \
	psx/*.c psx/*.h psx/dev/*.c psx/dev/*.h psx/dev/cdrom/*.c psx/dev/cdrom/*.h \
	psx/input/*.c psx/input/*.h \
	frontend/*.c frontend/*.h frontend/*.cpp frontend/*.hpp))

# tools/pgo.sh asks the Makefile for the stamp rather than computing its own. Two implementations
# of the same hash drifting apart is precisely how a staleness guard turns into a rubber stamp,
# and the first draft of that script did exactly that: its shell glob missed a directory the
# Makefile pattern also missed, hashed nothing, and got a stable-looking answer that matched
# nothing forever.
.PHONY: pgo-stamp
pgo-stamp:
	@cat $(PGO_STAMP_INPUTS) | git hash-object --stdin | cut -c1-16

ifneq ($(PGO),off)

# Hash over those sources. Deliberately the SOURCES and not the commit: the commit is what a human
# reads, this is what actually answers "is the profile describing this code" — it sees uncommitted
# edits, which a commit hash cannot. Via git hash-object because git is already a hard dependency
# here (VERSION_TAG) and shasum/sha256sum are spelled differently on macOS and Linux. Computed only
# when PGO is on: it is a few MB of I/O and every `make clean` would otherwise pay for it.
PGO_SOURCE_STAMP := $(shell cat $(PGO_STAMP_INPUTS) 2>/dev/null | git hash-object --stdin 2>/dev/null | cut -c1-16)
PGO_BUILD_COMMIT := $(shell git rev-parse HEAD 2>/dev/null)

# git's hash of empty input. If the file list came out empty this is what lands in the stamp, and
# it would compare equal to every other broken run — a guard that always says "fresh".
ifeq ($(PGO_SOURCE_STAMP),e69de29bb2d1d643)
$(error PGO=$(PGO): the source stamp hashed NOTHING (empty input). PGO_STAMP_INPUTS matched no \
files, so staleness could never be detected. Refusing to build)
endif

ifeq ($(strip $(PGO_SOURCE_STAMP)),)
$(error PGO=$(PGO): could not compute a source stamp (is git on PATH?). Refusing to build a \
profile that cannot be checked for staleness later)
endif

PGO_STAMP_DEFS := -DARMSX_PGO_BUILD_COMMIT='"$(PGO_BUILD_COMMIT)"' \
                  -DARMSX_PGO_SOURCE_STAMP='"$(PGO_SOURCE_STAMP)"'

endif

ifeq ($(PGO),generate)

PGO_CFLAGS := -fprofile-generate
PGO_LDFLAGS := -fprofile-generate
PGO_STAMP_DEFS += -DARMSX_PGO_GENERATE=1
$(info [pgo] INSTRUMENTED build — commit $(PGO_BUILD_COMMIT), source stamp $(PGO_SOURCE_STAMP))
$(info [pgo] this binary is for profiling only; it is several times slower than a release build)

else ifeq ($(PGO),use)

ifeq ($(wildcard $(PGO_PROFILE)),)
$(error PGO=use: no merged profile at $(PGO_PROFILE). Build PGO=generate, play the device \
workloads, then run: tools/pgo.sh merge)
endif

ifeq ($(wildcard $(PGO_PROVENANCE)),)
ifneq ($(PGO_ALLOW_STALE),1)
$(error PGO=use: $(PGO_PROFILE) exists but has no provenance file at $(PGO_PROVENANCE), so \
there is no way to tell what code it was recorded against. Re-merge with tools/pgo.sh merge, \
or set PGO_ALLOW_STALE=1 to build anyway)
endif
PGO_PROFILE_COMMIT := unknown
PGO_PROFILE_STAMP := unknown
else
PGO_PROFILE_COMMIT := $(shell sed -n 's/^commit=//p' $(PGO_PROVENANCE) 2>/dev/null | head -1)
PGO_PROFILE_STAMP := $(shell sed -n 's/^stamp=//p' $(PGO_PROVENANCE) 2>/dev/null | head -1)
endif

ifeq ($(strip $(PGO_PROFILE_STAMP)),$(strip $(PGO_SOURCE_STAMP)))
PGO_PROFILE_FRESH := 1
else
PGO_PROFILE_FRESH := 0
endif

ifeq ($(PGO_PROFILE_FRESH),0)
ifeq ($(PGO_ALLOW_STALE),1)
$(warning [pgo] ★ STALE PROFILE, building anyway because PGO_ALLOW_STALE=1.)
$(warning [pgo]   profile recorded from commit $(PGO_PROFILE_COMMIT) stamp $(PGO_PROFILE_STAMP))
$(warning [pgo]   tree being built is    commit $(PGO_BUILD_COMMIT) stamp $(PGO_SOURCE_STAMP))
$(warning [pgo]   a mismatched profile makes code SLOWER than no profile at all.)
else
$(error PGO=use: STALE PROFILE. $(PGO_PROFILE) was recorded from commit $(PGO_PROFILE_COMMIT) \
(source stamp $(PGO_PROFILE_STAMP)); this tree is commit $(PGO_BUILD_COMMIT) (source stamp \
$(PGO_SOURCE_STAMP)). Using it would mis-optimise rather than simply do nothing. Re-run the \
loop (tools/pgo.sh generate ...) or, if you have measured that it still helps, rebuild with \
PGO_ALLOW_STALE=1)
endif
endif

# -Wbackend-plugin is on by default; named explicitly so that turning it OFF has to be a
# deliberate act rather than a side effect of someone widening a -Wno- list.
PGO_CFLAGS := -fprofile-use=$(abspath $(PGO_PROFILE)) -Wbackend-plugin
ifeq ($(PGO_STRICT),1)
PGO_CFLAGS += -Werror=backend-plugin
endif
PGO_STAMP_DEFS += -DARMSX_PGO_USE=1 \
                  -DARMSX_PGO_PROFILE_COMMIT='"$(PGO_PROFILE_COMMIT)"' \
                  -DARMSX_PGO_PROFILE_STAMP='"$(PGO_PROFILE_STAMP)"' \
                  -DARMSX_PGO_PROFILE_FRESH=$(PGO_PROFILE_FRESH)
$(info [pgo] OPTIMISED build using $(PGO_PROFILE) (commit $(PGO_PROFILE_COMMIT), stamp $(PGO_PROFILE_STAMP)))
ifeq ($(PGO_STRICT),1)
$(info [pgo] PGO_STRICT=1: a per-function profile hash mismatch is an ERROR, not a warning)
endif

else ifneq ($(PGO),off)
$(error PGO must be one of: off, generate, use (got '$(PGO)'))
endif

# The emulator's own translation units, minus the two exclusions argued for above. $(sort) also
# de-duplicates, since ALL_OBJS and ALL_OBJS_SHARED overlap almost entirely.
PGO_OBJS := $(sort $(filter-out $(RCHEEVOS_OBJS) $(RC_OBJS) $(OBJ_DIR)/frontend/pgo.o, \
                                $(ALL_OBJS) $(ALL_OBJS_SHARED)))

$(PGO_OBJS): BASE_CFLAGS += $(PGO_CFLAGS)
$(PGO_OBJS): BASE_CXXFLAGS += $(PGO_CFLAGS)

# pgo.o gets the provenance -D's and NOT the instrumentation flags. It is the file that reports
# what this build is; it must not be described by the profile it helps collect.
$(OBJ_DIR)/frontend/pgo.o: BASE_CFLAGS += $(PGO_STAMP_DEFS)

# Force dynamic SDL when building the shared library
ifneq (,$(filter shared,$(MAKECMDGOALS)))
override SDL_STATIC := 0
BASE_CFLAGS += -D__DLL_BUILD -fPIC
BASE_CXXFLAGS += -D__DLL_BUILD -fPIC
endif

ifeq ($(SDL_STATIC),1)
	ifeq ($(SDL_LIBS_STATIC),)
$(warning Static SDL2 libraries not found; falling back to dynamic SDL2)
		SDL_STATIC := 0
	endif
endif

SDL_LIBS := $(if $(filter 1,$(SDL_STATIC)),$(SDL_LIBS_STATIC),$(SDL_LIBS_DYNAMIC))
SDL_LIBS_SHARED := $(SDL_LIBS_DYNAMIC)

.PHONY: all clean shared wasm psvita-lib test test-cpu test-cpu-spec test-gte test-cheats test-gpu test-texrep test-raster-select test-present-dst test-spu-width test-mcard-diverge test-cdrom-getlocp test-chd test-zip test-sdl-runtime test-disc-serial disc-probe

all: $(BIN)

shared: $(SHARED_BIN)

wasm: $(BIN)

psvita-lib: $(VITA_NATIVE_LIB)

TEST_CORE_SOURCES := $(wildcard psx/*.c) \
                     $(wildcard psx/dev/*.c) \
                     $(wildcard psx/dev/cdrom/*.c) \
                     $(wildcard psx/input/*.c)
TEST_CORE_SOURCES := $(filter-out psx/dev/cdrom/chd.c,$(TEST_CORE_SOURCES))

# chd.c is filtered out above because disc.c reaches it only under #ifdef USE_CHD, so a gate that
# does not want libchdr simply does not compile it. psx/dev/cdrom/pbp.c has NO such guard —
# disc.c's CD_EXT_PBP case is unconditional — so every gate that links the core also compiles
# pbp.c, which #include's miniz.h and calls into it. Without these three the whole suite fails to
# build with "'miniz.h' file not found", which is what it did from the moment PBP support landed.
# All three come from the libchdr build (miniz is one of its deps) and are empty when USE_CHD=0.
TEST_CORE_CFLAGS := $(LIBCHDR_INCLUDE_FLAGS)
TEST_CORE_LIBS := $(CHD_LINK_LIBS)
TEST_CORE_DEPS := $(CHD_BUILD_DEPS)
TEST_CPU_BIN := build/tests/cpu_differential
TEST_CHEATS_BIN := build/tests/cheat_engine
TEST_GPU_BIN := build/tests/gpu_renderer_parity
TEST_CHD_BIN := build/tests/chd_logic
TEST_ZIP_BIN := build/tests/zip_integration
TEST_SDL_BIN := build/tests/sdl_renderer_smoke
TEST_DISC_SERIAL_BIN := build/tests/disc_serial
DISC_PROBE_BIN := build/tests/disc_probe

$(TEST_CPU_BIN): tests/cpu_differential.c $(TEST_CORE_SOURCES) | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) $^ $(TEST_CORE_LIBS) -lm -o $@

test-cpu: $(TEST_CPU_BIN)
	./$(TEST_CPU_BIN)

# GTE against psx-spx, independently recomputed (tests/gte_matrix.c).
#
# Separate from cpu_differential on purpose, for the same reason gpu_texrep_parity is
# separate from test-gpu: every case above is DIFFERENTIAL (interpreter vs cached
# interpreter -- both this project's code, written from one understanding), which is
# structurally blind to a shared wrong rule. The GTE had NO gate of either kind. This one
# recomputes the UNR divide (table rebuilt from the documented formula, swept over the
# full 2^32 numerator x divisor space), RTPS/RTPT (per-step 44-bit sign-expansion, IR/SXY
# saturation and FLAG bits, both FIFOs, the sf=0 IR3 quirk, the DQA/DQB depth queue),
# NCLIP, AVSZ3/AVSZ4, MVMVA (including the documented CV=FC bug path) and the COP2
# register-file access semantics from the psx-spx pseudocode INSIDE the test, then drives
# the real emulator and compares full COP2 state. The three dispatch paths (interpreter
# switch, decode-cache handler, fetched psx_cpu_cycle in both modes) are cross-checked so
# a slip in any one of the duplicated GTE latch decoders is visible.
#
# psx/cpu.c is #include'd BY the test (unity-style) so the gate can reach the static
# internals (gte_divide, gte_write_register, psx_cpu_decode) directly -- the exhaustive
# divide sweep alone is ~4.3e9 calls, only affordable in-TU. It is therefore filtered OUT
# of the linked core sources; listing it as a prerequisite keeps the mutation-test loop
# honest (edit cpu.c -> relink -> gate must go red).
TEST_GTE_BIN := build/tests/gte_matrix
TEST_GTE_CORE_SOURCES := $(filter-out psx/cpu.c,$(TEST_CORE_SOURCES))

$(TEST_GTE_BIN): tests/gte_matrix.c psx/cpu.c psx/cpu.h $(TEST_GTE_CORE_SOURCES) | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) tests/gte_matrix.c $(TEST_GTE_CORE_SOURCES) $(TEST_CORE_LIBS) -lm -lpthread -o $@

test-gte: $(TEST_GTE_BIN)
	./$(TEST_GTE_BIN)

# R3000A against psx-spx / the R3000A manual, independently recomputed (tests/cpu_spec.c).
#
# Separate from cpu_differential for the same reason test-gte is: every case there is
# DIFFERENTIAL (interpreter vs cached interpreter -- both this project's code, one
# understanding), which is structurally blind to a rule that is wrong in both halves. This
# one re-implements the rules INSIDE the test -- load sign/zero extension, address-error
# behaviour, the LWL/LWR/SWL/SWR switch forms and the unaligned idioms they exist for, the
# load delay slot for every consumer class, the shift matrix, the ALU/overflow matrix,
# MULT/DIV including the degenerate results, branch/jump targets and link values, the
# exception model (EPC, CAUSE.BD, the SR mode stack, both vectors, RFE) and the COP0 write
# masks -- and runs every vector through BOTH execution modes.
#
# Links the whole core, like test-cpu, because the loads and stores go through the real bus.
TEST_CPU_SPEC_BIN := build/tests/cpu_spec

$(TEST_CPU_SPEC_BIN): tests/cpu_spec.c $(TEST_CORE_SOURCES) | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) $^ $(TEST_CORE_LIBS) -lm -o $@

test-cpu-spec: $(TEST_CPU_SPEC_BIN)
	./$(TEST_CPU_SPEC_BIN)

# Cheat engine (psx/cheats.c): the .cht parser, name-based arming, every implemented
# GameShark code type, and the RetroAchievements hardcore interlock. Links the whole core
# because it applies codes to a REAL psx_t, through the same RAM writer the emulator uses.
$(TEST_CHEATS_BIN): tests/cheat_engine.c $(TEST_CORE_SOURCES) | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) $^ $(TEST_CORE_LIBS) -lm -o $@

test-cheats: $(TEST_CHEATS_BIN)
	./$(TEST_CHEATS_BIN)

# psx/pgxp.c rides along because gpu.c's GP0 intake and poly parse call into it
# (inert while disabled, which the parity gate relies on).
TEST_GPU_SOURCES := tests/gpu_renderer_parity.c psx/dev/gpu.c psx/perf.c psx/pgxp.c \
                    psx/texrep.c psx/texrep_png.c \
                    frontend/gpu_hw.c frontend/gpu_hw_rt.c

# psx/dev/gpu.h is a PREREQUISITE, not a source: the accuracy helpers and the mask contract
# (PSX_GPU_MASK_WRITE / PSX_GPU_MASK_SKIP) live there and the test asserts them directly, so a
# header-only change has to relink or the gate keeps passing against a stale binary — which it
# did, silently, when the contract test was first written.
# -DARMSX_TEST_OFFSET_CENSUS is deliberately set HERE AND NOWHERE ELSE. It compiles in the
# drawing-offset census that `offset-stream-integrity` asserts on (psx/dev/gpu.h). That census
# runs once per primitive inside gpu_render_triangle() / _rect() / _flat_line(), so leaving it
# in a shipped binary would put ~1800 calls a frame in the hottest function in the emulator and
# hand a PGO run a profile shaped by test-only code. Adding this flag to any other rule, or to
# the library build, silently undoes that.
$(TEST_GPU_BIN): $(TEST_GPU_SOURCES) psx/dev/gpu.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_HARDWARE -DPSXE_DIAG_STDIO_DISABLE -DARMSX_TEST_OFFSET_CENSUS -I. -Ipsx -Ifrontend $(SDL_CFLAGS) $(TEST_GPU_SOURCES) -lm -o $@

test-gpu: $(TEST_GPU_BIN)
	./$(TEST_GPU_BIN)

# Texture dumping / replacement (psx/texrep.c), checked BEHAVIOURALLY.
#
# Separate from test-gpu on purpose. GPU_PARITY compares the two CPU rasterizers against each
# other, which is structurally blind to a mistake both make -- 0.5.12, 0.5.13 and 0.5.15 were
# each one formula written identically wrong in all three, and that gate passed through every
# one of them. Every case here pins output against the FEATURE'S contract instead: a dumped
# texture fed back as a replacement has to reproduce the original frame bit for bit.
#
# psx/dev/gpu.h and psx/texrep.h are PREREQUISITES, not sources: gpu_fetch_texel_f(),
# psx_gpu_filter_active() and the PSX_TEXREP_GLSL text the matrix case transcribes all live in
# headers, so a header-only change has to relink or the gate keeps passing against a stale
# binary -- the same trap $(TEST_GPU_BIN) documents above.
TEST_TEXREP_BIN := build/tests/gpu_texrep_parity
TEST_TEXREP_SOURCES := tests/gpu_texrep_parity.c psx/dev/gpu.c psx/perf.c psx/pgxp.c \
                       psx/texrep.c psx/texrep_png.c \
                       frontend/gpu_hw.c frontend/gpu_hw_rt.c

$(TEST_TEXREP_BIN): $(TEST_TEXREP_SOURCES) psx/dev/gpu.h psx/texrep.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_HARDWARE -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx -Ifrontend $(SDL_CFLAGS) $(TEST_TEXREP_SOURCES) -lm -o $@

test-texrep: $(TEST_TEXREP_BIN)
	./$(TEST_TEXREP_BIN)

# Host-side test for frontend/gpu_profile.c. Runs on the build machine precisely BECAUSE the
# interesting cases are hardware nobody here owns: Mali, MediaTek, ANGLE, Turnip, Xclipse. The
# rules those devices need are otherwise unverifiable until a user reports a regression.
TEST_GPU_PROFILE_BIN = bin/tests/gpu_profile_rules

$(TEST_GPU_PROFILE_BIN): tests/gpu_profile_rules.c frontend/gpu_profile.c frontend/gpu_profile.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -Wall -I. tests/gpu_profile_rules.c frontend/gpu_profile.c -o $@

test-gpu-profile: $(TEST_GPU_PROFILE_BIN)
	./$(TEST_GPU_PROFILE_BIN)

# Which rasterizer a configuration SELECTS, as opposed to what it draws. Every case in
# test-gpu compares rasterizer output, so none of them can see the GLES backend starting to
# attach on a device where it used to decline — which is exactly what took the PS1 BIOS's
# text away while all four gates stayed green. Compiles gpu_hw_gl.c on the host, where it
# takes its non-Android stub branch and pulls in no GL at all; the rule under test is a pure
# predicate placed outside that guard for this purpose.
TEST_RASTER_SELECT_BIN := build/tests/gpu_rasterizer_select

$(TEST_RASTER_SELECT_BIN): tests/gpu_rasterizer_select.c frontend/gpu_hw_gl.c frontend/gpu_hw_gl.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -Wall -DUSE_HARDWARE -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx -Ifrontend \
		tests/gpu_rasterizer_select.c frontend/gpu_hw_gl.c -lm -o $@

test-raster-select: $(TEST_RASTER_SELECT_BIN)
	./$(TEST_RASTER_SELECT_BIN)

# The blit bridge renders into a capped host framebuffer and copies it into the ANativeWindow
# with an UNSCALED row copy, so the buffer geometry it requests and the destination rect it
# fills must be the same number — SurfaceFlinger supplies the upscale. A GL backend binding the
# window resets that geometry to the window's natural size, and when the fallback ladder later
# lands on the bridge the mismatch draws the picture at a 2/3 top-left inset. compute_dst was
# never wrong, so a gate over compute_dst alone passed straight through the bug; this one pins
# all three of framebuffer size, destination rect, and the equality BETWEEN them.
# Pure rule: render.cpp only, with the backend factories stubbed in the test, so it needs no
# window, GL context or device.
TEST_PRESENT_DST_BIN := build/tests/present_dst_rect

$(TEST_PRESENT_DST_BIN): tests/present_dst_rect.c frontend/render.cpp frontend/render.h \
                         frontend/render_internal.h
	mkdir -p $(dir $@)
	$(CXX) -std=c++17 -O2 -g -Wall -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx -Ifrontend $(SDL_CFLAGS) \
		-x c++ tests/present_dst_rect.c frontend/render.cpp -lm -o $@

test-present-dst: $(TEST_PRESENT_DST_BIN)
	./$(TEST_PRESENT_DST_BIN)

# SPU register access WIDTH (psx/dev/spu.c): 8/16/32-bit against a 16-bit-wide register file.
#
# The file implemented 16- and 32-bit access only. An 8-bit read logged at FATAL and returned
# 0, an 8-bit write was dropped, and Xenogears reads voice ADSR registers a byte at a time —
# so it polled envelope state, got zero, and produced 1.1 MB of log doing it. No existing gate
# could see that: nothing crashes, and the 16-bit path everything else exercises stays correct.
#
# Links the whole core for the same reason test-cpu does — the byte-write path deliberately
# goes back through psx_spu_write16(), so key-on, key-off and the sound-RAM transfer address
# have to be reachable, not stubbed.
#
# psx/dev/spu.h is a PREREQUISITE, not a source: the register mirror's bound is derived from
# the psx_spu_t layout, so a header-only change has to relink or the gate keeps passing against
# a stale binary — the trap $(TEST_GPU_BIN) documents above.
TEST_SPU_WIDTH_BIN := build/tests/spu_register_widths

$(TEST_SPU_WIDTH_BIN): tests/spu_register_widths.c $(TEST_CORE_SOURCES) psx/dev/spu.h | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) \
		tests/spu_register_widths.c $(TEST_CORE_SOURCES) $(TEST_CORE_LIBS) -lm -o $@

test-spu-width: $(TEST_SPU_WIDTH_BIN)
	./$(TEST_SPU_WIDTH_BIN)

# Save-state <-> memory-card divergence (PSX_SS_MCARD in psx/state.c).
#
# Links the whole core because the interesting behaviour is the interaction between three
# real things: the card's serial write protocol, the state container's optional-section
# rule, and the identity phase that refuses a load before touching the machine. A gate over
# the verdict function alone would pass while the section was never written, never read, or
# marked mandatory — the last of which would brick every save state already on a device.
#
# psx/state.h and psx/dev/mcd.h are PREREQUISITES, not sources: the section id, the error
# codes and the load flag live in headers, so a header-only change has to relink or the gate
# keeps passing against a stale binary — the trap $(TEST_GPU_BIN) documents above.
TEST_MCARD_BIN := build/tests/state_mcard_divergence

$(TEST_MCARD_BIN): tests/state_mcard_divergence.c $(TEST_CORE_SOURCES) psx/state.h psx/dev/mcd.h | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) \
		tests/state_mcard_divergence.c $(TEST_CORE_SOURCES) $(TEST_CORE_LIBS) -lm -o $@

test-mcard-diverge: $(TEST_MCARD_BIN)
	./$(TEST_MCARD_BIN)

# What the drive REPORTS about itself: CdlGetlocP (psx/dev/cdrom/impl.c) and the response
# FIFO it comes back through (psx/dev/cdrom/cdrom.c).
#
# CdlGetlocP is the one command a game can sit in a loop on, so a wrong answer does not look
# like a CD bug: the emulator runs at full speed, the last frame keeps being drawn, and the
# only symptom is that input appears dead, because the game's main loop never gets past its
# poll. Nothing else in the suite can see that — the drive answers, it answers promptly, and
# every byte it answers with is well-formed. What was wrong was that the answer never
# changed, so the two properties this gate exists for are that the position MOVES and that it
# moves to somewhere useful.
#
# Links the whole core, like test-cpu, because the interesting behaviour is the interaction
# between the command state machine, the response FIFO and psx_cdrom_update()'s clock — the
# position advances on emulated time, so a gate over the response builder alone would pass
# against a drive that never moves. The disc is a 2352-byte-sector image the test writes and
# deletes itself; no real game image is involved.
#
# psx/dev/cdrom/cdrom.h is a PREREQUISITE, not a source: the hold window, the seek undershoot
# and the reported-position fields live in the header, so a header-only change has to relink
# or the gate keeps passing against a stale binary — the trap $(TEST_GPU_BIN) documents above.
TEST_GETLOCP_BIN := build/tests/cdrom_getlocp

$(TEST_GETLOCP_BIN): tests/cdrom_getlocp.c $(TEST_CORE_SOURCES) psx/dev/cdrom/cdrom.h | $(TEST_CORE_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(TEST_CORE_CFLAGS) \
		tests/cdrom_getlocp.c $(TEST_CORE_SOURCES) $(TEST_CORE_LIBS) -lm -o $@

test-cdrom-getlocp: $(TEST_GETLOCP_BIN)
	./$(TEST_GETLOCP_BIN) $(dir $(TEST_GETLOCP_BIN))

$(TEST_CHD_BIN): tests/chd_logic.c $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_CHD -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(LIBCHDR_INCLUDE_FLAGS) $< $(CHD_LINK_LIBS) -lm -o $@

test-chd: $(TEST_CHD_BIN)
	./$(TEST_CHD_BIN)

$(TEST_ZIP_BIN): tests/zip_integration.cpp frontend/archive.cpp $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CXX) -std=c++20 -O0 -DUSE_CHD -I. $(LIBCHDR_INCLUDE_FLAGS) \
		tests/zip_integration.cpp frontend/archive.cpp $(CHD_LINK_LIBS) -o $@

test-zip: $(TEST_ZIP_BIN)
	python3 tests/validate_zip.py ./$(TEST_ZIP_BIN)

$(TEST_SDL_BIN): tests/sdl_renderer_smoke.c
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g $(SDL_CFLAGS) $< $(SDL_LIBS) -o $@

test-sdl-runtime: $(TEST_SDL_BIN)
	./$(TEST_SDL_BIN)

# Disc serial identification (psx/discid.c), which is what gives a .chd its cover art, its
# per-game settings key and its achievements identity. Built with USE_CHD and the real disc
# readers so `--image <path>` can identify an actual .chd end to end; the gate itself needs no
# game image and no libchdr decode — see the header of tests/disc_serial.c.
#
# psx/discid.h is a PREREQUISITE, not a source: the buffer size and the API contract live there,
# so a header-only change has to relink or the gate keeps passing against a stale binary.
$(TEST_DISC_SERIAL_BIN): tests/disc_serial.c psx/discid.c psx/discid.h psx/perf.c \
		psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c \
		psx/dev/cdrom/pbp.c $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_CHD -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(LIBCHDR_INCLUDE_FLAGS) \
		tests/disc_serial.c psx/discid.c psx/perf.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c \
		psx/dev/cdrom/list.c psx/dev/cdrom/chd.c psx/dev/cdrom/pbp.c \
		$(CHD_LINK_LIBS) -lm -o $@

test-disc-serial: $(TEST_DISC_SERIAL_BIN)
	./$(TEST_DISC_SERIAL_BIN) $(dir $(TEST_DISC_SERIAL_BIN))

$(DISC_PROBE_BIN): tests/disc_probe.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c psx/dev/cdrom/pbp.c $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_CHD -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(LIBCHDR_INCLUDE_FLAGS) \
		tests/disc_probe.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c psx/dev/cdrom/pbp.c \
		$(CHD_LINK_LIBS) -lm -o $@

disc-probe: $(DISC_PROBE_BIN)

test:
	python3 tests/run_validation.py

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: %.c | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CC) -c $< -o $@ $(BASE_CFLAGS) \
		-DOS_INFO="$(OS_INFO)" \
		-DREP_VERSION="$(VERSION_TAG)" \
		-DREP_COMMIT_HASH="$(COMMIT_HASH)"

$(OBJ_DIR)/%.o: %.cpp | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(CXX) -c $< -o $@ $(BASE_CXXFLAGS) \
		-DOS_INFO="$(OS_INFO)" \
		-DREP_VERSION="$(VERSION_TAG)" \
		-DREP_COMMIT_HASH="$(COMMIT_HASH)"

$(OBJ_DIR)/%.o: %.rc | $(OBJ_DIR)
	mkdir -p $(dir $@)
	$(WINDRES) -I. $< $@

ifeq ($(USE_CHD),1)
$(LIBCHDR_ARCHIVE): $(LIBCHDR_INPUTS)
	mkdir -p $(LIBCHDR_BUILD_DIR)
	$(LIBCHDR_CONFIGURE) $(LIBCHDR_CMAKE_ARGS)
	cmake --build $(LIBCHDR_BUILD_DIR) --target chdr-static -j$(BUILD_JOBS)
endif

$(RUNTIME_ICON_DEST): $(RUNTIME_ICON_SRC) | $(BIN_DIR)
	mkdir -p $(dir $@)
	cp $< $@

$(BIN): $(ALL_OBJS) $(RUNTIME_ICON_DEST) $(CHD_BUILD_DEPS) | $(BIN_DIR)
	$(CXX) $(ALL_OBJS) $(CHD_LINK_LIBS) $(FSUI_LIBS) -o $(BIN) $(SDL_LIBS) $(PLATFORM_EXTRA_LDFLAGS) $(PLATFORM_EXTRA_LIBS) $(PGO_LDFLAGS) $(WASM_LDFLAGS)
ifeq ($(WASM_TARGET),wasm)
	@if [ ! -f "$(BIN)" ]; then \
		echo "WASM build did not produce $(BIN)"; \
		exit 1; \
	fi
	cmake -DINPUT_FILE="$(BIN)" -P "$(WASM_HTML_POSTPROCESS)"
endif

$(VITA_NATIVE_LIB): $(ALL_OBJS) $(RUNTIME_ICON_DEST) | $(BIN_DIR)
	$(AR) rcs $@ $(ALL_OBJS)
	$(RANLIB) $@

# $(PGO_LDFLAGS) is -fprofile-generate on an instrumented build and empty otherwise. It is what
# links compiler-rt's profile runtime in; without it the link fails on an undefined
# __llvm_profile_runtime rather than producing a .so that quietly writes nothing.
$(SHARED_BIN): $(ALL_OBJS_SHARED) $(CHD_BUILD_DEPS) | $(BIN_DIR)
	$(CXX) $(SHARED_LDFLAGS) $(ALL_OBJS_SHARED) $(CHD_LINK_LIBS) $(FSUI_LIBS) -o $(SHARED_BIN) $(SDL_LIBS_SHARED) $(PLATFORM_EXTRA_LDFLAGS) $(PLATFORM_EXTRA_LIBS) $(PGO_LDFLAGS)

clean:
	rm -rf "$(BIN_DIR)"
