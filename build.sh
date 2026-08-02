#!/bin/sh

set -e

# Usage:
#   ./build.sh             -> desktop build (static SDL if available)
#   ./build.sh shared      -> build shared lib (forces dynamic SDL)
#   ./build.sh ios         -> build iOS dylib using iPhone SDK + ios/Frameworks/SDL2.xcframework
#   ./build.sh macosapp    -> build desktop exe and bundle armsx.app
#   ./build.sh wasm        -> build WebAssembly target to bin/wasm using emscripten
#   ./build.sh android     -> build SDL2/libarmsx for Android and stage under android/app/src/main/jniLibs
#   ./build.sh psvita      -> build Vita native archive + Rust SDL host and package ARMSX.vpk

MODE="$1"
BUILD_JOBS="${BUILD_JOBS:-4}"
export USE_CHD="${USE_CHD:-1}"

# Profile-guided optimisation. Off unless asked for; forwarded verbatim to make by the android
# branch below, which is the only target wired for it today. See the PGO block in Makefile for
# what each value does, frontend/pgo.c for how the profile is collected on-device, and
# tools/pgo.sh for the loop that drives all of it.
#
#   PGO=generate ./build.sh android     instrumented build (slow; for collecting a profile)
#   PGO=use      ./build.sh android     optimised build against build/pgo/armsx.profdata
#
# PGO_PROFILE / PGO_ALLOW_STALE / PGO_STRICT ride along for the staleness guard. PGO_ALLOW_STALE
# is NOT defaulted here on purpose: the Makefile's default of 0 is what makes a stale profile a
# build failure instead of a silent slowdown, and defaulting it in two places is how that kind of
# guard ends up accidentally disabled.
#
# Written as if-blocks rather than `[ -n "$x" ] && ...`: under `set -e` that idiom evaluates to a
# non-zero status on the common path (the variable being unset), which is a footgun waiting for
# whoever adds the next one.
PGO="${PGO:-off}"
PGO_MAKE_ARGS="PGO=${PGO}"
if [ -n "${PGO_PROFILE:-}" ]; then
    PGO_MAKE_ARGS="${PGO_MAKE_ARGS} PGO_PROFILE=${PGO_PROFILE}"
fi
if [ -n "${PGO_ALLOW_STALE:-}" ]; then
    PGO_MAKE_ARGS="${PGO_MAKE_ARGS} PGO_ALLOW_STALE=${PGO_ALLOW_STALE}"
fi
if [ -n "${PGO_STRICT:-}" ]; then
    PGO_MAKE_ARGS="${PGO_MAKE_ARGS} PGO_STRICT=${PGO_STRICT}"
fi

build_fsui_native() {
    if [ "$(uname -s)" = "Darwin" ]; then
        MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-10.15}"
        cmake -S third_party/fsui-lib -B build/fsui/native \
            -DFSUI_BUILD_SAMPLES=OFF \
            -DFSUI_PLATFORM_BACKEND=SDL2 \
            -DFSUI_USE_SYSTEM_SDL2=ON \
            -DCMAKE_OSX_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET}"
    else
        cmake -S third_party/fsui-lib -B build/fsui/native \
            -DFSUI_BUILD_SAMPLES=OFF \
            -DFSUI_PLATFORM_BACKEND=SDL2 \
            -DFSUI_USE_SYSTEM_SDL2=ON
    fi
    cmake --build build/fsui/native -j"${BUILD_JOBS}"
}

build_fsui_wasm() {
    if ! command -v emcmake >/dev/null 2>&1 || ! command -v emcc >/dev/null 2>&1; then
        for candidate in "${EMSCRIPTEN_ROOT:-}" "${EMSDK:-}" "$HOME/emsdk" "/opt/emsdk" "/Volumes/FastDrive/linkertools/emsdk"; do
            if [ -z "${candidate}" ] || [ ! -d "${candidate}" ]; then
                continue
            fi

            if [ -x "${candidate}/upstream/emscripten/emcmake" ] && [ -x "${candidate}/upstream/emscripten/emcc" ]; then
                export EMSDK="${candidate}"
                export EMSCRIPTEN="${candidate}/upstream/emscripten"
                PATH="${candidate}/upstream/emscripten:${PATH}"
                node_bin="$(find "${candidate}/node" -type f -name node 2>/dev/null | head -n 1)"
                if [ -n "${node_bin}" ]; then
                    PATH="$(dirname "${node_bin}"):${PATH}"
                fi
                export PATH
                break
            fi

            if [ -x "${candidate}/emcmake" ] && [ -x "${candidate}/emcc" ]; then
                PATH="${candidate}:${PATH}"
                export PATH
                break
            fi
        done
    fi

    if ! command -v emcmake >/dev/null 2>&1 || ! command -v emcc >/dev/null 2>&1; then
        echo "emcmake/emcc are required for the wasm FSUI build. Set EMSCRIPTEN_ROOT, EMSDK, or install Emscripten under \$HOME/emsdk."
        exit 1
    fi

    emcmake cmake -S third_party/fsui-lib -B build/fsui/wasm \
        -DFSUI_BUILD_SAMPLES=OFF \
        -DFSUI_PLATFORM_BACKEND=SDL2
    cmake --build build/fsui/wasm -j"${BUILD_JOBS}"
}

build_fsui_psvita() {
    if [ -z "${VITASDK:-}" ]; then
        echo "VITASDK must be set for PSVita builds."
        exit 1
    fi

    cmake -S cmake/psvita-fsui -B build/fsui/psvita \
        -DCMAKE_TOOLCHAIN_FILE="${VITASDK}/share/vita.toolchain.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_POSITION_INDEPENDENT_CODE=OFF \
        -DVITASDK="${VITASDK}" \
        -DFSUI_BUILD_SAMPLES=OFF \
        -DFSUI_ENABLE_INSTALL=OFF \
        -DFSUI_PLATFORM_BACKEND=SDL2 \
        -DFSUI_IMGUI_OPENGL_ES3=ON
    cmake --build build/fsui/psvita -j"${BUILD_JOBS}"

    # The Vita Rust wrapper expects the FSUI archives in the CMake root output
    # directory, so stage the real archives out of the nested fsui-lib build.
    cp build/fsui/psvita/fsui-lib/libfsui*.a build/fsui/psvita/
}

bundle_macos_app() {
		    rm -rf armsx.app/Contents/Libraries
		    mkdir -p armsx.app/Contents/MacOS
		    mkdir -p armsx.app/Contents/Resources/icons
		    cp bin/armsx armsx.app/Contents/MacOS
		    cp icons/ArmsxDesktop.icns armsx.app/Contents/Resources/armsx.icns
		    cp -R icons/. armsx.app/Contents/Resources/icons/
		    chmod 777 armsx.app/Contents/MacOS/armsx
		    dylibbundler -b -x ./armsx.app/Contents/MacOS/armsx -d ./armsx.app/Contents/Libraries/ -p @executable_path/../Libraries/ -cd
		    cp Info.plist armsx.app/Contents/Info.plist
}

stage_android_runtime_icons() {
	    mkdir -p android/app/src/main/assets/icons
	    cp -R icons/. android/app/src/main/assets/icons/
}

prepare_ios_sdl_package() {
    IOS_SDL_FRAMEWORK="$1"
    IOS_SDL_PACKAGE_ROOT="$(pwd)/build/fsui/ios-sdl-package/SDL2.framework"

    rm -rf "${IOS_SDL_PACKAGE_ROOT}"
    mkdir -p "${IOS_SDL_PACKAGE_ROOT}/Versions/A/Resources/CMake"
    mkdir -p "${IOS_SDL_PACKAGE_ROOT}/Versions/A"
    ln -s "${IOS_SDL_FRAMEWORK}/Headers" "${IOS_SDL_PACKAGE_ROOT}/Headers"
    ln -s "${IOS_SDL_FRAMEWORK}/Headers" "${IOS_SDL_PACKAGE_ROOT}/Versions/A/Headers"
    ln -s "${IOS_SDL_FRAMEWORK}/SDL2" "${IOS_SDL_PACKAGE_ROOT}/Versions/A/SDL2"
    cp "${IOS_SDL_FRAMEWORK}/CMake/sdl2-config.cmake" "${IOS_SDL_PACKAGE_ROOT}/Versions/A/Resources/CMake/"
    cp "${IOS_SDL_FRAMEWORK}/CMake/sdl2-config-version.cmake" "${IOS_SDL_PACKAGE_ROOT}/Versions/A/Resources/CMake/"

    printf '%s\n' "${IOS_SDL_PACKAGE_ROOT}/Versions/A/Resources/CMake"
}

if [ "$MODE" = "ios" ]; then
    IOS_SDK="${IOS_SDK:-iphoneos}"
    IOS_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET:-13.0}"
    IOS_SDKROOT="$(xcrun --sdk "${IOS_SDK}" --show-sdk-path)"
    IOS_CC="$(xcrun --sdk "${IOS_SDK}" --find clang)"
    IOS_CXX="$(xcrun --sdk "${IOS_SDK}" --find clang++)"

    DEFAULT_SDL_FW_ROOT="$(pwd)/ios/Frameworks/SDL2.xcframework"
    IOS_SDL_FRAMEWORK="${IOS_SDL_FRAMEWORK:-$DEFAULT_SDL_FW_ROOT}"

    # Resolve to the actual SDL2.framework path
    if [ -d "${IOS_SDL_FRAMEWORK}/SDL2.framework" ]; then
        IOS_SDL_FRAMEWORK="${IOS_SDL_FRAMEWORK}/SDL2.framework"
    elif [ -d "${IOS_SDL_FRAMEWORK}/ios-arm64/SDL2.framework" ]; then
        IOS_SDL_FRAMEWORK="${IOS_SDL_FRAMEWORK}/ios-arm64/SDL2.framework"
    elif [ ! -d "${IOS_SDL_FRAMEWORK}/Headers" ]; then
        echo "SDL2.framework not found under ${IOS_SDL_FRAMEWORK}"
        echo "Place the xcframework in ios/Frameworks or set IOS_SDL_FRAMEWORK to the SDL2.framework path."
        exit 1
    fi

    if [ ! -d "${IOS_SDL_FRAMEWORK}" ]; then
        echo "SDL2.xcframework not found at ${IOS_SDL_FRAMEWORK}"
        echo "Place the framework in ios/Frameworks (or set IOS_SDL_FRAMEWORK) and retry."
        exit 1
    fi

    IOS_SDL2_DIR="$(prepare_ios_sdl_package "${IOS_SDL_FRAMEWORK}")"

    echo "Building iOS dylib with SDK ${IOS_SDK} (${IOS_SDKROOT})"
    cmake -S third_party/fsui-lib -B build/fsui/ios \
        -DFSUI_BUILD_SAMPLES=OFF \
        -DFSUI_PLATFORM_BACKEND=SDL2 \
        -DFSUI_USE_SYSTEM_SDL2=ON \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${IOS_DEPLOYMENT_TARGET}" \
        -DCMAKE_OSX_SYSROOT="${IOS_SDKROOT}" \
        -DSDL2_DIR="${IOS_SDL2_DIR}"
    cmake --build build/fsui/ios -j"${BUILD_JOBS}"
    make clean
    IOS_ENV="IOS_TARGET=1 IOS_SDK=${IOS_SDK} IOS_DEPLOYMENT_TARGET=${IOS_DEPLOYMENT_TARGET} SDKROOT=${IOS_SDKROOT} CC=${IOS_CC} CXX=${IOS_CXX} SDL_STATIC=0 IOS_SDL_FRAMEWORK=${IOS_SDL_FRAMEWORK} FSUI_BUILD_DIR=$(pwd)/build/fsui/ios"
    eval "make shared LIBCHDR_BUILD_DIR=$(pwd)/build/libchdr/ios ${IOS_ENV}"
    echo "Copying libarmsx.dylib to ios/Frameworks/"
    cp bin/libarmsx.dylib ios/Frameworks/

elif [ "$MODE" = "shared" ]; then
    build_fsui_native
    make clean
    SDL_STATIC=0 MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-10.15}" FSUI_BUILD_DIR="$(pwd)/build/fsui/native" LIBCHDR_BUILD_DIR="$(pwd)/build/libchdr/native" make shared

elif [ "$MODE" = "android" ]; then
	    stage_android_runtime_icons
	    ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-${ANDROID_NDK_HOME:-${NDK_HOME:-}}}"
    if [ -z "${ANDROID_NDK_ROOT}" ]; then
        echo "ANDROID_NDK_ROOT (or ANDROID_NDK_HOME / NDK_HOME) must be set to a valid NDK path."
        exit 1
    fi
    if [ ! -d "${ANDROID_NDK_ROOT}" ]; then
        echo "Android NDK path ${ANDROID_NDK_ROOT} does not exist."
        exit 1
    fi

    ANDROID_ABI="${ANDROID_ABI:-arm64-v8a}"
    ANDROID_API="${ANDROID_API:-26}"
    ANDROID_PLATFORM="android-${ANDROID_API}"

    case "${ANDROID_ABI}" in
        arm64-v8a)
            ANDROID_TRIPLE="aarch64-linux-android"
            ANDROID_CXX_RUNTIME_TRIPLE="aarch64-linux-android"
            ;;
        armeabi-v7a)
            ANDROID_TRIPLE="armv7a-linux-androideabi"
            ANDROID_CXX_RUNTIME_TRIPLE="arm-linux-androideabi"
            ;;
        x86)
            ANDROID_TRIPLE="i686-linux-android"
            ANDROID_CXX_RUNTIME_TRIPLE="i686-linux-android"
            ;;
        x86_64)
            ANDROID_TRIPLE="x86_64-linux-android"
            ANDROID_CXX_RUNTIME_TRIPLE="x86_64-linux-android"
            ;;
        *)
            echo "Unsupported ANDROID_ABI '${ANDROID_ABI}'."
            exit 1
            ;;
    esac

    HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
    HOST_ARCH="$(uname -m)"
    case "${HOST_ARCH}" in
        arm64|aarch64)
            HOST_ARCH_TAG="arm64"
            ;;
        x86_64)
            HOST_ARCH_TAG="x86_64"
            ;;
        *)
            HOST_ARCH_TAG="${HOST_ARCH}"
            ;;
    esac

    HOST_TAG="${HOST_OS}-${HOST_ARCH_TAG}"
    TOOLCHAIN_DIR="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/${HOST_TAG}"
    if [ ! -d "${TOOLCHAIN_DIR}" ]; then
        # Fallback to the first available prebuilt toolchain
        TOOLCHAIN_DIR="$(ls -d "${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/"* 2>/dev/null | head -n 1)"
        if [ -z "${TOOLCHAIN_DIR}" ]; then
            echo "Unable to locate LLVM toolchain inside ${ANDROID_NDK_ROOT}."
            exit 1
        fi
        HOST_TAG="$(basename "${TOOLCHAIN_DIR}")"
    fi

    echo "Using Android NDK at ${ANDROID_NDK_ROOT} (toolchain ${HOST_TAG}, ABI ${ANDROID_ABI}, API ${ANDROID_API})"

    ANDROID_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake"
    if [ ! -f "${ANDROID_TOOLCHAIN_FILE}" ]; then
        echo "Android toolchain file not found at ${ANDROID_TOOLCHAIN_FILE}"
        exit 1
    fi

    REPO_ROOT="$(pwd)"

    SDL_BUILD_ROOT="${REPO_ROOT}/build/android/sdl/${ANDROID_ABI}"
    SDL_INSTALL_DIR="${SDL_BUILD_ROOT}/install"
    rm -rf "${SDL_BUILD_ROOT}"
    mkdir -p "${SDL_BUILD_ROOT}"

    cmake -S third_party/SDL -B "${SDL_BUILD_ROOT}" \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_TOOLCHAIN_FILE}" \
        -DANDROID_ABI="${ANDROID_ABI}" \
        -DANDROID_PLATFORM="${ANDROID_PLATFORM}" \
        -DANDROID_STL=c++_shared \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="${SDL_INSTALL_DIR}"

    cmake --build "${SDL_BUILD_ROOT}" --config Release
    cmake --install "${SDL_BUILD_ROOT}" --config Release

    SDL_LIB_PATH="${SDL_INSTALL_DIR}/lib"
    SDL_INCLUDE_PATH="${SDL_INSTALL_DIR}/include/SDL2"
    SDL_SHARED_LIB="${SDL_LIB_PATH}/libSDL2.so"
    CXX_SHARED_RUNTIME="${TOOLCHAIN_DIR}/sysroot/usr/lib/${ANDROID_CXX_RUNTIME_TRIPLE}/libc++_shared.so"
    if [ ! -f "${SDL_SHARED_LIB}" ]; then
        echo "SDL shared library not found at ${SDL_SHARED_LIB}"
        exit 1
    fi
    if [ ! -f "${CXX_SHARED_RUNTIME}" ]; then
        echo "Android C++ shared runtime not found at ${CXX_SHARED_RUNTIME}"
        exit 1
    fi

    JNI_LIB_DIR="${REPO_ROOT}/android/app/src/main/jniLibs/${ANDROID_ABI}"
    SDL_HEADER_STAGE="${REPO_ROOT}/android/native-deps/SDL2/include"
    mkdir -p "${JNI_LIB_DIR}"
    rm -rf "${SDL_HEADER_STAGE}"
    mkdir -p "${SDL_HEADER_STAGE}"
    cp -R "${SDL_INCLUDE_PATH}/." "${SDL_HEADER_STAGE}/"

    TOOLCHAIN_BIN="${TOOLCHAIN_DIR}/bin"
    CC="${TOOLCHAIN_BIN}/${ANDROID_TRIPLE}${ANDROID_API}-clang"
    CXX="${TOOLCHAIN_BIN}/${ANDROID_TRIPLE}${ANDROID_API}-clang++"
    export CC
    export CXX

    SDL_CFLAGS="-D_REENTRANT -DANDROID -I${SDL_INCLUDE_PATH}"
    # -lEGL/-lGLESv3 are what the GLES 3.0 present path (frontend/render_gl.cpp) binds to when
    # it attaches EGL directly to the Compose host's ANativeWindow. There is deliberately no
    # -lvulkan: frontend/render_vk.cpp dlopen()s the loader through a replaceable function
    # pointer so an adrenotools custom driver can be substituted at runtime.
    SDL_LIBS="-L${SDL_LIB_PATH} -lSDL2 -llog -landroid -lGLESv3 -lEGL -lOpenSLES -lm -lc++_shared"

    # Presentation backends. Vulkan is EXPERIMENTAL (compile-verified only) and is never
    # selected unless settings.toml asks for gpu_backend = "vulkan"; set
    # ARMSX_ENABLE_VULKAN=0 to leave it out of the build entirely.
    ARMSX_ENABLE_GL="${ARMSX_ENABLE_GL:-1}"
    ARMSX_ENABLE_VULKAN="${ARMSX_ENABLE_VULKAN:-1}"

    # RetroArch (.slangp) shader chains (frontend/render_shaders.cpp). librashader is
    # dlopen()ed, never linked, so this adds nothing to libarmsx.so's DT_NEEDED — but the
    # .so has to be in jniLibs for dlopen("liblibrashader_capi.so") to resolve by SONAME.
    # If it is not vendored, build with shaders compiled out rather than shipping a feature
    # that can only fail at runtime.
    LIBRASHADER_SO="${REPO_ROOT}/third_party/librashader/prebuilt/${ANDROID_ABI}/liblibrashader_capi.so"
    ARMSX_ENABLE_SHADERS="${ARMSX_ENABLE_SHADERS:-1}"
    if [ "${ARMSX_ENABLE_SHADERS}" = "1" ] && [ ! -f "${LIBRASHADER_SO}" ]; then
        echo "librashader is not vendored at ${LIBRASHADER_SO};"
        echo "building without shader chains. Run third_party/librashader/build-android.sh to add it."
        ARMSX_ENABLE_SHADERS=0
    fi

    # ---- adrenotools (BSD 2-Clause) --------------------------------------------------------
    #
    # Custom Vulkan drivers CANNOT be loaded with a plain dlopen: a Turnip pack links against
    # system libraries (libcutils and friends) that are not resolvable from an app's
    # classloader namespace, so the load fails with
    #   dlopen failed: library "libcutils.so" not found ... in namespace classloader-namespace
    # adrenotools exists to solve exactly that — it loads the driver inside a patched linker
    # namespace where the vendor ICD resolves.
    #
    # Static lib linked into libarmsx.so, plus FOUR hook .so files that must be PACKAGED (the
    # loader hands adrenotools their directory at runtime and it dlopens them by name).
    ADRENOTOOLS_BUILD_DIR="${REPO_ROOT}/build/adrenotools/android/${ANDROID_ABI}"
    cmake -S "${REPO_ROOT}/third_party/libadrenotools" -B "${ADRENOTOOLS_BUILD_DIR}" \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_ROOT}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="${ANDROID_ABI}" \
        -DANDROID_PLATFORM="android-${ANDROID_API}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF
    cmake --build "${ADRENOTOOLS_BUILD_DIR}" -j"${BUILD_JOBS}"

    # FSUI cut: the Jetpack Compose front-end owns all menus, so fsui-lib is no longer built or
    # linked. FSUI_INCLUDE_FLAGS/COMPILE_DEFS/LIBS are overridden empty (keeping only
    # -DSDL_MAIN_HANDLED, which the Android external_main entry still needs).
    if [ "${PGO}" != "off" ]; then
        echo "pgo: building with PGO=${PGO} (see tools/pgo.sh for the full loop)"
    fi

    # ${PGO_MAKE_ARGS} is intentionally unquoted: it is a list of make variable assignments and
    # has to word-split. It is "PGO=off" and nothing else unless PGO was asked for, so the normal
    # build is byte-identical to what it was before PGO existed.
    make clean
    make \
        ${PGO_MAKE_ARGS} \
        SDL_STATIC=0 \
        ARMSX_ENABLE_GL="${ARMSX_ENABLE_GL}" \
        ARMSX_ENABLE_VULKAN="${ARMSX_ENABLE_VULKAN}" \
        ARMSX_ENABLE_SHADERS="${ARMSX_ENABLE_SHADERS}" \
        SDL_CFLAGS="${SDL_CFLAGS}" \
        SDL_LIBS_DYNAMIC="${SDL_LIBS}" \
        FSUI_INCLUDE_FLAGS= \
        FSUI_COMPILE_DEFS=-DSDL_MAIN_HANDLED \
        FSUI_LIBS= \
        AR="${TOOLCHAIN_BIN}/llvm-ar" \
        RANLIB="${TOOLCHAIN_BIN}/llvm-ranlib" \
        LIBCHDR_EXTRA_CMAKE_ARGS="-DCMAKE_SYSTEM_NAME=Linux -DCMAKE_SYSTEM_PROCESSOR=aarch64" \
        LIBCHDR_BUILD_DIR="${REPO_ROOT}/build/libchdr/android/${ANDROID_ABI}" \
        PLATFORM=Android \
        OS_INFO=Android \
        PLATFORM_EXTRA_LDFLAGS= \
        PLATFORM_EXTRA_LIBS="${ADRENOTOOLS_BUILD_DIR}/libadrenotools.a ${ADRENOTOOLS_BUILD_DIR}/lib/linkernsbypass/liblinkernsbypass.a" \
        ADRENOTOOLS_FLAGS="-I${REPO_ROOT}/third_party/libadrenotools/include -DARMSX_HAVE_ADRENOTOOLS=1" \
        shared

    # The hook libraries adrenotools dlopens at runtime. They must sit in the app's
    # nativeLibraryDir, which is what CustomDriver.kt passes as hookLibDir.
    for hook in main_hook hook_impl file_redirect_hook gsl_alloc_hook; do
        if [ -f "${ADRENOTOOLS_BUILD_DIR}/src/hook/lib${hook}.so" ]; then
            cp "${ADRENOTOOLS_BUILD_DIR}/src/hook/lib${hook}.so" "${JNI_LIB_DIR}/"
        fi
    done

    cp "${SDL_SHARED_LIB}" "${JNI_LIB_DIR}/"
    cp "${CXX_SHARED_RUNTIME}" "${JNI_LIB_DIR}/"
    cp bin/libarmsx.so "${JNI_LIB_DIR}/"

    # librashader rides along as a plain payload: nothing links against it, the app never
    # System.loadLibrary()s it, and frontend/render_shaders.cpp dlopen()s it by SONAME the
    # first time a shader chain is actually switched on. A stale copy left behind after
    # ARMSX_ENABLE_SHADERS=0 would be 13 MB of dead weight in the APK, so remove it then.
    if [ "${ARMSX_ENABLE_SHADERS}" = "1" ]; then
        cp "${LIBRASHADER_SO}" "${JNI_LIB_DIR}/"
    else
        rm -f "${JNI_LIB_DIR}/liblibrashader_capi.so"
    fi

    # ---- Discord rich presence helper (libarmsx_discord.so) --------------------------------
    #
    # Its OWN shared library, linked against the Discord Social SDK and nothing else — not
    # libarmsx, not SDL. That is the whole point: it is loaded only in the :discord process
    # (see AndroidManifest), so with the feature off the proprietary SDK is not merely idle,
    # it is never mapped, and an SDK crash cannot take the emulator down mid-game.
    #
    # The application id and callback scheme come from android/app/src/main/assets/discord_creds.env
    # — the SAME file app/build.gradle reads for the manifest's intent-filter scheme. One source,
    # so the redirect the SDK sends and the scheme the OS routes back can never disagree.
    #
    # Absent SDK or absent id => the stub half of discord_bridge.cpp is built instead. It still
    # exports every JNI entry point, so DiscordNative loads, available() answers false, and the
    # Friends UI hides itself. A missing .so would instead be an UnsatisfiedLinkError per call.
    DISCORD_AAR="${REPO_ROOT}/android/app/libs/discord_partner_sdk.aar"
    DISCORD_ENV="${REPO_ROOT}/android/app/src/main/assets/discord_creds.env"
    DISCORD_SRC="${REPO_ROOT}/android/app/src/main/cpp/discord_bridge.cpp"
    DISCORD_STAGE="${REPO_ROOT}/build/android/discord/${ANDROID_ABI}"

    # Read one KEY=VALUE out of the .env, ignoring comments. The key must start the line, so the
    # explanatory comments in that file (which name the keys) cannot be picked up as values.
    discord_env_value() {
        [ -f "${DISCORD_ENV}" ] || return 0
        sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\(.*\)\$/\1/p" "${DISCORD_ENV}" \
            | tail -n 1 | tr -d '\r' | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
    }

    # Mirrors the sanitizeScheme closure in app/build.gradle: accept a bare scheme, or anything
    # written as a URI, and keep only the scheme part.
    discord_sanitize_scheme() {
        printf '%s' "$1" | sed -e 's|://.*$||' -e 's|:.*$||' -e 's|/.*$||' \
            -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//'
    }

    DISCORD_APP_ID="$(discord_env_value APPLICATION_ID)"
    DISCORD_SCHEME="$(discord_sanitize_scheme "$(discord_env_value APPLICATION_SCHEME)")"

    # An id that is not a plain integer would compile into a nonsense constant, so reject it here
    # rather than shipping a client that authorizes against garbage.
    DISCORD_WHY=""
    case "${DISCORD_APP_ID}" in
        ''|*[!0-9]*)
            if [ -n "${DISCORD_APP_ID}" ]; then
                DISCORD_WHY="APPLICATION_ID '${DISCORD_APP_ID}' is not numeric"
            fi
            DISCORD_APP_ID=""
            ;;
    esac
    if [ -z "${DISCORD_SCHEME}" ] && [ -n "${DISCORD_APP_ID}" ]; then
        DISCORD_SCHEME="discord-${DISCORD_APP_ID}"
    fi

    # -nostdlib++ then libc++_shared.so BY PATH: the NDK's clang driver otherwise links
    # libc++_static.a AND honours the -l, putting TWO C++ runtimes in one process and re-exporting
    # ~1000 libc++ symbols out of this .so. libc++_shared.so is already staged above for libarmsx,
    # so sharing it costs no APK bytes and saves ~700 KB over the static runtime.
    #
    # ★ By path, NOT "-L<sysroot>/usr/lib/<triple> -lc++_shared". That directory holds
    # libc.a / libm.a / libdl.a next to libc++_shared.so, and an explicit -L is searched BEFORE the
    # API-level directory the driver adds — so the driver's own implicit -lc/-lm/-ldl resolved to
    # the STATIC archives. This .so then carried a whole second copy of bionic and no DT_NEEDED on
    # libc.so at all, and compiler-rt's outline-atomics constructor called that copy's getauxval()
    # from .init_array at dlopen: a static libc's __libc_shared_globals are filled in by its own
    # process startup, which never runs inside a .so, so it read a null auxv and took SIGSEGV.
    # The :discord process died inside System.loadLibrary, before DiscordService.onCreate could
    # return — presenting as "The Discord helper didn't start", on a loop. libc++_shared.so carries
    # a SONAME, so DT_NEEDED still reads "libc++_shared.so"; same trick as the SDK .so below.
    #
    # Safe against the SDK because nothing but the C API crosses that boundary: the shipped .so
    # exports zero discordpp:: symbols (discordpp.h is a header-only wrapper over Discord_*), and
    # its own libc++ is ABI-namespaced as std::__Cr so it cannot collide with ours.
    #
    # --exclude-libs,ALL keeps static-archive symbols out of the dynamic table; with it and
    # -fvisibility=hidden this .so exports 15 symbols, 11 of them the JNI entry points.
    DISCORD_CXXFLAGS="-std=c++20 -fPIC -O2 -g0 -fvisibility=hidden -fvisibility-inlines-hidden -Wall -ffunction-sections -fdata-sections"
    DISCORD_LDFLAGS="-nostdlib++ ${TOOLCHAIN_DIR}/sysroot/usr/lib/${ANDROID_CXX_RUNTIME_TRIPLE}/libc++_shared.so -llog -Wl,--exclude-libs,ALL -Wl,--gc-sections"
    if [ -f "${DISCORD_AAR}" ] && [ -n "${DISCORD_APP_ID}" ]; then
        # The .aar carries a prefab module: headers AND a per-ABI .so, so nothing else has to be
        # vendored. Extracted for LINKING ONLY — the .so is deliberately not copied into jniLibs,
        # because gradle already packages it from the same .aar and two copies fail the merger.
        rm -rf "${DISCORD_STAGE}"
        mkdir -p "${DISCORD_STAGE}"
        unzip -o -q "${DISCORD_AAR}" \
            "prefab/modules/discord_partner_sdk/include/*" \
            "jni/${ANDROID_ABI}/libdiscord_partner_sdk.so" \
            -d "${DISCORD_STAGE}"

        DISCORD_INCLUDE="${DISCORD_STAGE}/prefab/modules/discord_partner_sdk/include"
        DISCORD_SDK_SO="${DISCORD_STAGE}/jni/${ANDROID_ABI}/libdiscord_partner_sdk.so"
        if [ ! -f "${DISCORD_INCLUDE}/discordpp.h" ] || [ ! -f "${DISCORD_SDK_SO}" ]; then
            DISCORD_WHY="${DISCORD_AAR} has no ${ANDROID_ABI} slice or no headers"
            DISCORD_APP_ID=""
        fi
    fi

    if [ -f "${DISCORD_AAR}" ] && [ -n "${DISCORD_APP_ID}" ]; then
        echo "discord: application ${DISCORD_APP_ID}, callback scheme ${DISCORD_SCHEME}://"
        # -fvisibility=hidden keeps discordpp.h's thousands of inline symbols out of the dynamic
        # table; JNIEXPORT is visibility("default") in the NDK's jni.h, so the entry points survive.
        # Linked against the extracted .so by path; it carries a SONAME, so what lands in
        # DT_NEEDED is "libdiscord_partner_sdk.so" and the loader finds gradle's copy at runtime.
        "${CXX}" ${DISCORD_CXXFLAGS} -shared \
            -DARMSX_HAS_DISCORD=1 \
            -DARMSX_DISCORD_APPLICATION_ID="${DISCORD_APP_ID}" \
            -DARMSX_DISCORD_CALLBACK_SCHEME="\"${DISCORD_SCHEME}\"" \
            -I"${DISCORD_INCLUDE}" \
            -o "${JNI_LIB_DIR}/libarmsx_discord.so" \
            "${DISCORD_SRC}" \
            "${DISCORD_SDK_SO}" \
            ${DISCORD_LDFLAGS}
    else
        if [ -n "${DISCORD_WHY}" ]; then
            echo "discord: ${DISCORD_WHY}; building the stub helper (feature reports unavailable)."
        elif [ ! -f "${DISCORD_AAR}" ]; then
            echo "discord: ${DISCORD_AAR} not staged; building the stub helper (feature reports unavailable)."
        else
            echo "discord: no APPLICATION_ID in ${DISCORD_ENV}; building the stub helper."
        fi
        "${CXX}" ${DISCORD_CXXFLAGS} -shared \
            -o "${JNI_LIB_DIR}/libarmsx_discord.so" \
            "${DISCORD_SRC}" \
            ${DISCORD_LDFLAGS}
    fi

elif [ "$MODE" = "psvita" ]; then
    if [ -z "${VITASDK:-}" ]; then
        echo "VITASDK must be set for PSVita builds."
        exit 1
    fi

    if ! command -v cargo >/dev/null 2>&1; then
        echo "cargo is required for the PSVita Rust host build."
        exit 1
    fi

    export PATH="${VITASDK}/bin:${PATH}"
    build_fsui_psvita

    make clean
    VITASDK="${VITASDK}" PSVITA_TARGET=1 FSUI_BUILD_DIR="$(pwd)/build/fsui/psvita" LIBCHDR_BUILD_DIR="$(pwd)/build/libchdr/psvita" make psvita-lib

    export PKG_CONFIG_ALLOW_CROSS=1
    export PKG_CONFIG="${VITASDK}/bin/arm-vita-eabi-pkg-config"
    export CARGO_TARGET_ARMV7_SONY_VITA_NEWLIBEABIHF_LINKER=arm-vita-eabi-g++
    export ARMSX_VITA_NATIVE_DIR="$(pwd)/bin/psvita"
    export ARMSX_VITA_FSUI_DIR="$(pwd)/build/fsui/psvita"
    export ARMSX_VITA_LIBCHDR_DIR="$(pwd)/build/libchdr/psvita"

    cargo build \
        --manifest-path psvita/Cargo.toml \
        --release \
        --target armv7-sony-vita-newlibeabihf \
        -Z build-std=std,panic_abort

    VITA_PACKAGE_DIR="$(pwd)/build/psvita/package"
    VITA_TARGET_DIR="$(pwd)/psvita/target/armv7-sony-vita-newlibeabihf/release"
    VITA_ELF="${VITA_TARGET_DIR}/ARMSX_vita.elf"
    if [ ! -f "${VITA_ELF}" ] && [ -f "${VITA_TARGET_DIR}/ARMSX_vita" ]; then
        VITA_ELF="${VITA_TARGET_DIR}/ARMSX_vita"
    fi
    VITA_VELF="${VITA_PACKAGE_DIR}/eboot.velf"
    VITA_EBOOT="${VITA_PACKAGE_DIR}/eboot.bin"
    VITA_PARAM="${VITA_PACKAGE_DIR}/param.sfo"
    VITA_VPK="${VITA_PACKAGE_DIR}/ARMSX.vpk"

    if [ ! -f "${VITA_ELF}" ]; then
        echo "Expected Vita host ELF not found at ${VITA_ELF}"
        exit 1
    fi

    rm -rf "${VITA_PACKAGE_DIR}"
    mkdir -p "${VITA_PACKAGE_DIR}/sce_sys"
    cp icons/ArmsxDesktop.png "${VITA_PACKAGE_DIR}/sce_sys/icon0.png"

    vita-mksfoex -s TITLE_ID=ARMSX001 "ARMSX PSVita" "${VITA_PARAM}"
    vita-elf-create -s "${VITA_ELF}" "${VITA_VELF}"
    vita-make-fself -s "${VITA_VELF}" "${VITA_EBOOT}"
    vita-pack-vpk "${VITA_VPK}" \
        -s "${VITA_PARAM}" \
        -b "${VITA_EBOOT}" \
        -a "$(pwd)/icons=icons" \
        -a "${VITA_PACKAGE_DIR}/sce_sys=sce_sys"

    cp "${VITA_VPK}" bin/psvita/
    echo "Packaged Vita build at bin/psvita/ARMSX.vpk"

elif [ "$MODE" = "wasm" ]; then
    build_fsui_wasm
    make clean
    if command -v emmake >/dev/null 2>&1; then
        emmake make wasm FSUI_BUILD_DIR="$(pwd)/build/fsui/wasm" LIBCHDR_BUILD_DIR="$(pwd)/build/libchdr/wasm"
    else
        make wasm FSUI_BUILD_DIR="$(pwd)/build/fsui/wasm" LIBCHDR_BUILD_DIR="$(pwd)/build/libchdr/wasm"
    fi

elif [ "$MODE" = "macosapp" ]; then
    build_fsui_native
    make clean
    SDL_STATIC=0 MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-10.15}" FSUI_BUILD_DIR="$(pwd)/build/fsui/native" LIBCHDR_BUILD_DIR="$(pwd)/build/libchdr/native" make
    bundle_macos_app

else
    build_fsui_native
    make clean
    SDL_STATIC=0 MACOS_DEPLOYMENT_TARGET="${MACOS_DEPLOYMENT_TARGET:-10.15}" FSUI_BUILD_DIR="$(pwd)/build/fsui/native" LIBCHDR_BUILD_DIR="$(pwd)/build/libchdr/native" make
fi
