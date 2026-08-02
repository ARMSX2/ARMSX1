#!/usr/bin/env bash

set -euo pipefail

target="${1:-}"
build_jobs="${BUILD_JOBS:-4}"
alpine_branch="${ALPINE_BRANCH:-latest-stable}"

if [ -z "$target" ]; then
    echo "Usage: $0 <x64|x32|arm64|arm32>" >&2
    exit 1
fi

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
tool_root="$repo_root/build/ci-tools"
apk_root="$tool_root/apk"
toolchain_root="$tool_root/musl"
sysroot_root="$tool_root/sysroots"
fsui_build_dir="$repo_root/build/fsui/musl-$target"
artifact_root="$repo_root/artifacts/linux-musl/$target"
package_root="$artifact_root/armsx-linux-musl-$target"
package_zip="$artifact_root/armsx-linux-musl-$target.zip"

mkdir -p "$tool_root" "$apk_root" "$toolchain_root" "$sysroot_root"

download_apk_static() {
    local apk_static="$apk_root/apk.static"
    if [ -x "$apk_static" ]; then
        printf '%s\n' "$apk_static"
        return 0
    fi

    local listing filename package_url package_path
    listing="$(curl -fsSL "https://dl-cdn.alpinelinux.org/alpine/${alpine_branch}/main/x86_64/")"
    filename="$(
        printf '%s\n' "$listing" \
            | sed -n 's/.*href="\([^"]*apk-tools-static[^"]*\.apk\)".*/\1/p' \
            | head -n1
    )"
    if [ -z "$filename" ]; then
        filename="$(
            printf '%s\n' "$listing" \
                | grep -oE 'apk-tools-static-[^"<>[:space:]]+\.apk' \
                | head -n1
        )"
    fi

    if [ -z "$filename" ]; then
        echo "Unable to locate apk-tools-static package on Alpine ${alpine_branch}." >&2
        exit 1
    fi

    package_url="https://dl-cdn.alpinelinux.org/alpine/${alpine_branch}/main/x86_64/$filename"
    package_path="$apk_root/$filename"
    curl -fsSL "$package_url" -o "$package_path"
    tar -xzf "$package_path" -C "$apk_root" sbin/apk.static
    mv "$apk_root/sbin/apk.static" "$apk_static"
    chmod +x "$apk_static"
    rm -rf "$apk_root/sbin" "$package_path"

    printf '%s\n' "$apk_static"
}

download_toolchain() {
    local toolchain_name="$1"
    local toolchain_dir="$toolchain_root/$toolchain_name-cross"
    local toolchain_archive="$toolchain_root/$toolchain_name-cross.tgz"
    if [ -x "$toolchain_dir/bin/${toolchain_name}-gcc" ]; then
        printf '%s\n' "$toolchain_dir"
        return 0
    fi

    rm -rf "$toolchain_dir" "$toolchain_archive"
    curl -fsSL "https://musl.cc/${toolchain_name}-cross.tgz" -o "$toolchain_archive"
    tar -xzf "$toolchain_archive" -C "$toolchain_root"
    if [ ! -d "$toolchain_dir" ]; then
        echo "Unable to extract musl toolchain ${toolchain_name}." >&2
        exit 1
    fi

    printf '%s\n' "$toolchain_dir"
}

configure_target() {
    case "$target" in
        x64)
            apk_arch="x86_64"
            toolchain_name="x86_64-linux-musl"
            cmake_processor="x86_64"
            sysroot_packages="sdl2-dev mesa-dev"
            ;;
        x32)
            apk_arch="x86"
            toolchain_name="i686-linux-musl"
            cmake_processor="i686"
            sysroot_packages="sdl2-dev mesa-dev"
            ;;
        arm64)
            apk_arch="aarch64"
            toolchain_name="aarch64-linux-musl"
            cmake_processor="aarch64"
            sysroot_packages="sdl2-dev mesa-dev"
            ;;
        arm32)
            apk_arch="armv7"
            toolchain_name="arm-linux-musleabihf"
            cmake_processor="arm"
            sysroot_packages="sdl2-dev mesa-dev"
            ;;
        *)
            echo "Unsupported musl target '$target'." >&2
            exit 1
            ;;
    esac
}

configure_target

toolchain_dir="$(download_toolchain "$toolchain_name")"
apk_static="$(download_apk_static)"
python3_bin="$(command -v python3 || true)"
if [ -z "$python3_bin" ]; then
    echo "python3 is required for the musl fsui build." >&2
    exit 1
fi

sysroot_dir="$sysroot_root/$target"
repo_file="$sysroot_dir/etc/apk/repositories"
rm -rf "$sysroot_dir"
mkdir -p "$sysroot_dir/etc/apk" "$sysroot_dir/var/cache/apk" "$sysroot_dir/var/lib/apk"

cat > "$repo_file" <<EOF
https://dl-cdn.alpinelinux.org/alpine/${alpine_branch}/main
https://dl-cdn.alpinelinux.org/alpine/${alpine_branch}/community
EOF

"$apk_static" \
    --root "$sysroot_dir" \
    --arch "$apk_arch" \
    --repositories-file "$repo_file" \
    --allow-untrusted \
    --initdb \
    add \
    $sysroot_packages

config_path="$(
    find "$sysroot_dir" \( -name SDL2Config.cmake -o -name sdl2-config.cmake \) | head -n1
)"
if [ -z "$config_path" ]; then
    echo "Unable to locate SDL2 CMake package inside $sysroot_dir." >&2
    exit 1
fi
sdl2_config_dir="$(dirname "$config_path")"

toolchain_file="$tool_root/musl-$target.cmake"
cat > "$toolchain_file" <<EOF
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR ${cmake_processor})
set(CMAKE_C_COMPILER ${toolchain_dir}/bin/${toolchain_name}-gcc)
set(CMAKE_CXX_COMPILER ${toolchain_dir}/bin/${toolchain_name}-g++)
set(CMAKE_AR ${toolchain_dir}/bin/${toolchain_name}-ar)
set(CMAKE_RANLIB ${toolchain_dir}/bin/${toolchain_name}-ranlib)
set(CMAKE_SYSROOT ${sysroot_dir})
set(CMAKE_FIND_ROOT_PATH ${sysroot_dir})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

export PKG_CONFIG_SYSROOT_DIR="$sysroot_dir"
export PKG_CONFIG_LIBDIR="$sysroot_dir/usr/lib/pkgconfig:$sysroot_dir/usr/share/pkgconfig"

sdl_cflags="$(pkg-config --cflags sdl2)"
sdl_libs="$(pkg-config --libs sdl2)"
platform_extra_ldflags="-static-libstdc++ -static-libgcc"
case "$target" in
    arm32)
        platform_extra_ldflags="$platform_extra_ldflags -Wl,-Bstatic -latomic -Wl,-Bdynamic"
        ;;
    arm64)
        platform_extra_ldflags="$platform_extra_ldflags -Wl,-Bstatic -latomic -lgcc -Wl,-Bdynamic"
        ;;
esac

rm -rf "$fsui_build_dir" "$artifact_root"
mkdir -p "$artifact_root"

cmake -S "$repo_root/third_party/fsui-lib" -B "$fsui_build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$toolchain_file" \
    -DCMAKE_BUILD_TYPE=Release \
    -DFSUI_BUILD_SAMPLES=OFF \
    -DFSUI_PLATFORM_BACKEND=SDL2 \
    -DFSUI_USE_SYSTEM_SDL2=ON \
    -DPython_EXECUTABLE="$python3_bin" \
    -DPython3_EXECUTABLE="$python3_bin" \
    -DCMAKE_PREFIX_PATH="$sysroot_dir/usr" \
    -DSDL2_DIR="$sdl2_config_dir"
cmake --build "$fsui_build_dir" -j"$build_jobs"

cd "$repo_root"
make clean
make \
    CC="${toolchain_dir}/bin/${toolchain_name}-gcc" \
    CXX="${toolchain_dir}/bin/${toolchain_name}-g++" \
    AR="${toolchain_dir}/bin/${toolchain_name}-ar" \
    RANLIB="${toolchain_dir}/bin/${toolchain_name}-ranlib" \
    SDL_STATIC=0 \
    FSUI_LINK_SYSTEM_GL=0 \
    SDL_CFLAGS="$sdl_cflags" \
    SDL_LIBS_DYNAMIC="$sdl_libs" \
    FSUI_BUILD_DIR="$fsui_build_dir" \
    PLATFORM_EXTRA_LDFLAGS="$platform_extra_ldflags"

mkdir -p "$package_root/lib" "$package_root/icons"
cp "$repo_root/bin/armsx" "$package_root/armsx.bin"
cp -R "$repo_root/icons/." "$package_root/icons/"
"$repo_root/scripts/ci/copy-linux-runtime-deps.sh" "$repo_root/bin/armsx" "$package_root/lib" "$sysroot_dir/lib:$sysroot_dir/usr/lib:$sysroot_dir/usr/local/lib:$toolchain_dir"

loader_path="$(find "$sysroot_dir" -type f -name 'ld-musl-*.so.1' | head -n1)"
if [ -n "$loader_path" ]; then
    cp -L "$loader_path" "$package_root/lib/"
fi

cat > "$package_root/armsx" <<'EOF'
#!/usr/bin/env sh
set -eu
app_dir="$(cd "$(dirname "$0")" && pwd)"
export LD_LIBRARY_PATH="$app_dir/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
loader=""
for candidate in "$app_dir"/lib/ld-musl-*.so.1; do
    if [ -x "$candidate" ]; then
        loader="$candidate"
        break
    fi
done
if [ -n "$loader" ]; then
    exec "$loader" "$app_dir/armsx.bin" "$@"
fi
exec "$app_dir/armsx.bin" "$@"
EOF

chmod +x "$package_root/armsx"

(
    cd "$artifact_root"
    zip -r "$(basename "$package_zip")" "$(basename "$package_root")"
)
