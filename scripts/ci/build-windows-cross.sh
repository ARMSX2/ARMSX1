#!/usr/bin/env bash

set -euo pipefail

target="${1:-}"
build_jobs="${BUILD_JOBS:-4}"

if [ -z "$target" ]; then
    echo "Usage: $0 <x64|x32>" >&2
    exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
artifact_root="$repo_root/artifacts/windows/$target"
package_root="$artifact_root/armsx-windows-$target"
package_zip="$artifact_root/armsx-windows-$target.zip"
sdl_build_dir="$repo_root/build/sdl/windows-$target"
sdl_install_dir="$sdl_build_dir/install"
fsui_build_dir="$repo_root/build/fsui/ci-windows-$target"

case "$target" in
    x64)
        triplet="x86_64-w64-mingw32"
        toolchain_file="$repo_root/third_party/SDL/build-scripts/cmake-toolchain-mingw64-x86_64.cmake"
        ;;
    x32|x86)
        target="x32"
        triplet="i686-w64-mingw32"
        toolchain_file="$repo_root/third_party/SDL/build-scripts/cmake-toolchain-mingw64-i686.cmake"
        ;;
    *)
        echo "Unsupported Windows target '$target'." >&2
        exit 1
        ;;
esac

rm -rf "$artifact_root" "$sdl_build_dir" "$fsui_build_dir"
mkdir -p "$artifact_root"

cmake -S "$repo_root/third_party/SDL" -B "$sdl_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=ON \
    -DSDL_SHARED=ON \
    -DSDL_STATIC=OFF \
    -DSDL_TEST=OFF \
    -DCMAKE_INSTALL_PREFIX="$sdl_install_dir"
cmake --build "$sdl_build_dir" -j"$build_jobs"
cmake --install "$sdl_build_dir"

sdl_include="$sdl_install_dir/include/SDL2"
sdl_lib="$sdl_install_dir/lib"
sdl_dll="$(find "$sdl_install_dir" -type f \( -name 'SDL2.dll' -o -name 'libSDL2.dll' \) | head -n 1)"
if [ -z "$sdl_dll" ]; then
    echo "Unable to locate SDL2.dll in $sdl_install_dir" >&2
    exit 1
fi

cmake -S "$repo_root/third_party/fsui-lib" -B "$fsui_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
    -DFSUI_BUILD_SAMPLES=OFF \
    -DFSUI_PLATFORM_BACKEND=SDL2 \
    -DFSUI_USE_SYSTEM_SDL2=ON \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$sdl_install_dir"
cmake --build "$fsui_build_dir" -j"$build_jobs"

cd "$repo_root"
make clean
make \
    CC="$triplet-gcc" \
    CXX="$triplet-g++" \
    WINDRES="$triplet-windres" \
    WINDOWS_TARGET=1 \
    SDL_STATIC=0 \
    FSUI_BUILD_DIR="$fsui_build_dir" \
    SDL_CFLAGS="-I$sdl_include" \
    SDL_LIBS_DYNAMIC="-L$sdl_lib -lSDL2 -lsetupapi -limm32 -lversion -lwinmm -lgdi32 -lole32 -loleaut32 -lshell32 -luuid -lopengl32" \
    PLATFORM_EXTRA_LDFLAGS="-static-libstdc++ -static-libgcc"

mkdir -p "$package_root/icons"
cp "$repo_root/bin/armsx.exe" "$package_root/"
cp "$sdl_dll" "$package_root/SDL2.dll"
cp -R "$repo_root/icons/." "$package_root/icons/"

for runtime_dll in \
    "$("$triplet-g++" -print-file-name=libstdc++-6.dll)" \
    "$("$triplet-g++" -print-file-name=libwinpthread-1.dll)"; do
    if [ -n "$runtime_dll" ] && [ -f "$runtime_dll" ]; then
        cp "$runtime_dll" "$package_root/"
    fi
done

gcc_dir="$(dirname "$("$triplet-gcc" -print-libgcc-file-name)")"
for libgcc_dll in "$gcc_dir"/libgcc_s_*.dll; do
    if [ -f "$libgcc_dll" ]; then
        cp "$libgcc_dll" "$package_root/"
    fi
done

(
    cd "$artifact_root"
    zip -r "$(basename "$package_zip")" "$(basename "$package_root")"
)
