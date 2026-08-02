.ONESHELL:
.SHELLFLAGS := -ec

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

.PHONY: all clean shared wasm psvita-lib test test-cpu test-cheats test-gpu test-texrep test-raster-select test-present-dst test-spu-width test-mcard-diverge test-cdrom-getlocp test-chd test-zip test-sdl-runtime disc-probe

all: $(BIN)

shared: $(SHARED_BIN)

wasm: $(BIN)

psvita-lib: $(VITA_NATIVE_LIB)

TEST_CORE_SOURCES := $(wildcard psx/*.c) \
                     $(wildcard psx/dev/*.c) \
                     $(wildcard psx/dev/cdrom/*.c) \
                     $(wildcard psx/input/*.c)
TEST_CORE_SOURCES := $(filter-out psx/dev/cdrom/chd.c,$(TEST_CORE_SOURCES))
TEST_CPU_BIN := build/tests/cpu_differential
TEST_CHEATS_BIN := build/tests/cheat_engine
TEST_GPU_BIN := build/tests/gpu_renderer_parity
TEST_CHD_BIN := build/tests/chd_logic
TEST_ZIP_BIN := build/tests/zip_integration
TEST_SDL_BIN := build/tests/sdl_renderer_smoke
DISC_PROBE_BIN := build/tests/disc_probe

$(TEST_CPU_BIN): tests/cpu_differential.c $(TEST_CORE_SOURCES)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $^ -lm -o $@

test-cpu: $(TEST_CPU_BIN)
	./$(TEST_CPU_BIN)

# Cheat engine (psx/cheats.c): the .cht parser, name-based arming, every implemented
# GameShark code type, and the RetroAchievements hardcore interlock. Links the whole core
# because it applies codes to a REAL psx_t, through the same RAM writer the emulator uses.
$(TEST_CHEATS_BIN): tests/cheat_engine.c $(TEST_CORE_SOURCES)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $^ -lm -o $@

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
$(TEST_GPU_BIN): $(TEST_GPU_SOURCES) psx/dev/gpu.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_HARDWARE -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx -Ifrontend $(SDL_CFLAGS) $(TEST_GPU_SOURCES) -lm -o $@

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

$(TEST_SPU_WIDTH_BIN): tests/spu_register_widths.c $(TEST_CORE_SOURCES) psx/dev/spu.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx \
		tests/spu_register_widths.c $(TEST_CORE_SOURCES) -lm -o $@

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

$(TEST_MCARD_BIN): tests/state_mcard_divergence.c $(TEST_CORE_SOURCES) psx/state.h psx/dev/mcd.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx \
		tests/state_mcard_divergence.c $(TEST_CORE_SOURCES) -lm -o $@

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

$(TEST_GETLOCP_BIN): tests/cdrom_getlocp.c $(TEST_CORE_SOURCES) psx/dev/cdrom/cdrom.h
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx \
		tests/cdrom_getlocp.c $(TEST_CORE_SOURCES) -lm -o $@

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

$(DISC_PROBE_BIN): tests/disc_probe.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c $(CHD_BUILD_DEPS)
	mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -g -DUSE_CHD -DPSXE_DIAG_STDIO_DISABLE -I. -Ipsx $(LIBCHDR_INCLUDE_FLAGS) \
		tests/disc_probe.c psx/dev/cdrom/disc.c psx/dev/cdrom/cue.c psx/dev/cdrom/list.c psx/dev/cdrom/chd.c \
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
	$(CXX) $(ALL_OBJS) $(CHD_LINK_LIBS) $(FSUI_LIBS) -o $(BIN) $(SDL_LIBS) $(PLATFORM_EXTRA_LDFLAGS) $(PLATFORM_EXTRA_LIBS) $(WASM_LDFLAGS)
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

$(SHARED_BIN): $(ALL_OBJS_SHARED) $(CHD_BUILD_DEPS) | $(BIN_DIR)
	$(CXX) $(SHARED_LDFLAGS) $(ALL_OBJS_SHARED) $(CHD_LINK_LIBS) $(FSUI_LIBS) -o $(SHARED_BIN) $(SDL_LIBS_SHARED) $(PLATFORM_EXTRA_LDFLAGS) $(PLATFORM_EXTRA_LIBS)

clean:
	rm -rf "$(BIN_DIR)"
