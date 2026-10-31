#!/usr/bin/env bash

set -euo pipefail

target="${1:-}"
build_jobs="${BUILD_JOBS:-4}"

if [ -z "$target" ]; then
    echo "Usage: $0 <x64|x32|arm64|arm32>" >&2
    exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
fsui_build_dir="$repo_root/build/fsui/ci-linux-$target"
artifact_root="$repo_root/artifacts/linux/$target"
package_root="$artifact_root/armsx-linux-$target"
package_zip="$artifact_root/armsx-linux-$target.zip"

cmake_args=(
    -S "$repo_root/third_party/fsui-lib"
    -B "$fsui_build_dir"
    -DFSUI_BUILD_SAMPLES=OFF
    -DFSUI_PLATFORM_BACKEND=SDL2
    -DFSUI_USE_SYSTEM_SDL2=ON
    -DCMAKE_BUILD_TYPE=Release
)

extra_cflags=""
extra_ldflags=""
pkg_config_libdir=""
dep_search_dirs=""

case "$target" in
    x64)
        cc="gcc"
        cxx="g++"
        pkg_config_libdir="/usr/lib/x86_64-linux-gnu/pkgconfig:/usr/share/pkgconfig"
        dep_search_dirs="/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu:/lib64:/usr/lib64"
        ;;
    x32|x86)
        target="x32"
        cc="gcc"
        cxx="g++"
        extra_cflags="-m32"
        extra_ldflags="-m32"
        pkg_config_libdir="/usr/lib/i386-linux-gnu/pkgconfig:/usr/share/pkgconfig"
        dep_search_dirs="/lib/i386-linux-gnu:/usr/lib/i386-linux-gnu"
        ;;
    arm64)
        cc="aarch64-linux-gnu-gcc"
        cxx="aarch64-linux-gnu-g++"
        pkg_config_libdir="/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig"
        dep_search_dirs="/lib/aarch64-linux-gnu:/usr/lib/aarch64-linux-gnu:/usr/aarch64-linux-gnu/lib"
        cmake_args+=(
            -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=aarch64
        )
        ;;
    arm32)
        cc="arm-linux-gnueabihf-gcc"
        cxx="arm-linux-gnueabihf-g++"
        pkg_config_libdir="/usr/lib/arm-linux-gnueabihf/pkgconfig:/usr/share/pkgconfig"
        dep_search_dirs="/lib/arm-linux-gnueabihf:/usr/lib/arm-linux-gnueabihf:/usr/arm-linux-gnueabihf/lib"
        cmake_args+=(
            -DCMAKE_SYSTEM_NAME=Linux
            -DCMAKE_SYSTEM_PROCESSOR=arm
        )
        ;;
    *)
        echo "Unsupported Linux target '$target'." >&2
        exit 1
        ;;
esac

export PKG_CONFIG_LIBDIR="$pkg_config_libdir"
unset PKG_CONFIG_PATH

if [ -n "$extra_cflags" ]; then
    cmake_args+=(
        -DCMAKE_C_FLAGS="$extra_cflags"
        -DCMAKE_CXX_FLAGS="$extra_cflags"
        -DCMAKE_EXE_LINKER_FLAGS="$extra_ldflags"
    )
fi

cmake_args+=(
    -DCMAKE_C_COMPILER="$cc"
    -DCMAKE_CXX_COMPILER="$cxx"
)

sdl_cflags="$(pkg-config --cflags sdl2)"
sdl_libs="$(pkg-config --libs sdl2)"

if [ -n "$extra_cflags" ]; then
    sdl_cflags="$sdl_cflags $extra_cflags"
    sdl_libs="$sdl_libs $extra_ldflags"
fi

rm -rf "$fsui_build_dir" "$artifact_root"
mkdir -p "$artifact_root"

cmake "${cmake_args[@]}"
cmake --build "$fsui_build_dir" -j"$build_jobs"

cd "$repo_root"
make clean
make \
    CC="$cc" \
    CXX="$cxx" \
    FSUI_BUILD_DIR="$fsui_build_dir" \
    SDL_STATIC=0 \
    SDL_CFLAGS="$sdl_cflags" \
    SDL_LIBS_DYNAMIC="$sdl_libs" \
    PLATFORM_EXTRA_LDFLAGS="-static-libstdc++ -static-libgcc"

mkdir -p "$package_root/lib" "$package_root/icons"
cp "$repo_root/bin/armsx" "$package_root/armsx.bin"
cp -R "$repo_root/icons/." "$package_root/icons/"
"$repo_root/scripts/ci/copy-linux-runtime-deps.sh" "$repo_root/bin/armsx" "$package_root/lib" "$dep_search_dirs"

cat > "$package_root/armsx" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
app_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$app_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
exec "$app_dir/armsx.bin" "$@"
EOF

chmod +x "$package_root/armsx"

(
    cd "$artifact_root"
    zip -r "$(basename "$package_zip")" "$(basename "$package_root")"
)
