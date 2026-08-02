#!/usr/bin/env bash

set -euo pipefail

usage() {
    cat <<'EOF'
Usage: ./build-uwp-native.sh [x64|x86] [Debug|Release]

Builds the UWP/WinRT native DLL with MinGW, then stages:
  - libarmsx.dll and icons/ into uwp/ARMSX/bin/<platform>/<config>

Environment overrides:
  BUILD_JOBS   Parallel build jobs (default: 4)
  SDL2_DIR     Override the SDL2 package path relative to repo root
  SDL_SOURCE_BUILD=1
               Force building SDL from third_party/SDL instead of probing the system
  MINGW_MAKE   Override the make command (default: mingw32-make, fallback: make)
EOF
}

platform="${1:-x64}"
configuration="${2:-Debug}"
build_jobs="${BUILD_JOBS:-4}"

case "$platform" in
    x64)
        default_sdl2_dir="SDL2-2.30.3/x86_64-w64-mingw32"
        ;;
    x86)
        default_sdl2_dir="SDL2-2.30.3/i686-w64-mingw32"
        ;;
    -h|--help)
        usage
        exit 0
        ;;
    *)
        echo "Unsupported platform '$platform'. Use x64 or x86." >&2
        exit 1
        ;;
esac

case "$configuration" in
    Debug|Release)
        ;;
    *)
        echo "Unsupported configuration '$configuration'. Use Debug or Release." >&2
        exit 1
        ;;
esac

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
fsui_build_dir="$repo_root/build/fsui/uwp-$platform"
uwp_output_dir="$repo_root/uwp/ARMSX/bin/$platform/$configuration"
uwp_icons_dir="$uwp_output_dir/icons"
sdl_build_root="$repo_root/build/sdl/uwp-$platform"
sdl_install_root="$sdl_build_root/install"

make_cmd="${MINGW_MAKE:-mingw32-make}"
if ! command -v "$make_cmd" >/dev/null 2>&1; then
    make_cmd="make"
fi
if ! command -v "$make_cmd" >/dev/null 2>&1; then
    echo "Unable to find mingw32-make or make in PATH." >&2
    exit 1
fi

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake is required." >&2
    exit 1
fi

find_sdl_dll_in_prefix() {
    local prefix="$1"
    local candidate=

    for candidate in \
        "$prefix/bin/SDL2.dll" \
        "$prefix/bin/libSDL2.dll" \
        "$prefix/lib/SDL2.dll" \
        "$prefix/lib/libSDL2.dll"; do
        if [ -f "$candidate" ]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done

    return 1
}

sanitize_sdl_cflags() {
    printf '%s\n' "$1" | sed 's/[[:space:]]-Dmain=SDL_main//g; s/^-Dmain=SDL_main[[:space:]]*//'
}

configure_sdl_from_prefix() {
    local prefix="$1"
    local include_dir=
    local lib_dir=
    local dll_path=

    if [ -d "$prefix/include/SDL2" ]; then
        include_dir="$prefix/include/SDL2"
    elif [ -d "$prefix/include" ]; then
        include_dir="$prefix/include"
    else
        return 1
    fi

    if [ -d "$prefix/lib" ]; then
        lib_dir="$prefix/lib"
    elif [ -d "$prefix/lib64" ]; then
        lib_dir="$prefix/lib64"
    else
        return 1
    fi

    dll_path="$(find_sdl_dll_in_prefix "$prefix")" || return 1

    sdl_prefix="$prefix"
    sdl_include="$include_dir"
    sdl_lib="$lib_dir"
    sdl_dll="$dll_path"
    sdl_cflags="-I$sdl_include"
    sdl_libs_dynamic="-L$sdl_lib -lSDL2"
    return 0
}

detect_system_sdl() {
    if command -v pkg-config >/dev/null 2>&1 && pkg-config --exists sdl2; then
        local prefix=
        prefix="$(pkg-config --variable=prefix sdl2 2>/dev/null || true)"
        if [ -n "$prefix" ] && configure_sdl_from_prefix "$prefix"; then
            sdl_cflags="$(sanitize_sdl_cflags "$(pkg-config --cflags sdl2)")"
            return 0
        fi
    fi

    if command -v sdl2-config >/dev/null 2>&1; then
        local prefix=
        prefix="$(sdl2-config --prefix 2>/dev/null || true)"
        if [ -n "$prefix" ] && configure_sdl_from_prefix "$prefix"; then
            sdl_cflags="$(sanitize_sdl_cflags "$(sdl2-config --cflags)")"
            return 0
        fi
    fi

    return 1
}

build_sdl_from_source() {
    mkdir -p "$sdl_build_root"

    cmake -S "$repo_root/third_party/SDL" -B "$sdl_build_root" -G "MinGW Makefiles" \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=ON \
        -DSDL_SHARED=ON \
        -DSDL_STATIC=OFF \
        -DSDL_TEST=OFF \
        -DCMAKE_INSTALL_PREFIX="$sdl_install_root"
    cmake --build "$sdl_build_root" -j"$build_jobs"
    cmake --install "$sdl_build_root"

    if ! configure_sdl_from_prefix "$sdl_install_root"; then
        echo "Built SDL, but could not locate headers/libs/DLL under $sdl_install_root" >&2
        exit 1
    fi
}

if [ -n "${SDL2_DIR:-}" ]; then
    sdl2_root="$repo_root/$SDL2_DIR"
    if ! configure_sdl_from_prefix "$sdl2_root"; then
        echo "SDL2_DIR points to an unusable SDL tree: $sdl2_root" >&2
        exit 1
    fi
elif [ "${SDL_SOURCE_BUILD:-0}" = "1" ]; then
    build_sdl_from_source
elif ! detect_system_sdl; then
    build_sdl_from_source
fi

if [ -z "${sdl_dll:-}" ]; then
    echo "Unable to resolve an SDL2 runtime DLL for UWP staging." >&2
    exit 1
fi

mkdir -p "$repo_root/bin" "$uwp_output_dir" "$uwp_icons_dir"

cmake -S "$repo_root/third_party/fsui-lib" -B "$fsui_build_dir" -G "MinGW Makefiles" \
    -DFSUI_BUILD_SAMPLES=OFF \
    -DFSUI_PLATFORM_BACKEND=SDL2 \
    -DFSUI_USE_SYSTEM_SDL2=ON \
    -DCMAKE_PREFIX_PATH="$sdl_prefix"
cmake --build "$fsui_build_dir" -j"$build_jobs"

"$make_cmd" clean
"$make_cmd" shared \
    WINDOWS_TARGET=1 \
    UWP_TARGET=1 \
    SDL_STATIC=0 \
    FSUI_BUILD_DIR="$fsui_build_dir" \
    SDL_CFLAGS="$sdl_cflags" \
    SDL_LIBS_DYNAMIC="$sdl_libs_dynamic"

cp "$repo_root/bin/libarmsx.dll" "$uwp_output_dir/"
cp -R "$repo_root/icons/." "$uwp_icons_dir/"

echo "Staged UWP native runtime to $uwp_output_dir"
