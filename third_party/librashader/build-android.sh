#!/usr/bin/env bash
#
# Regenerate third_party/librashader/prebuilt/arm64-v8a/liblibrashader_capi.so.
#
# Run this BY HAND when the pin in NOTICE changes. It is deliberately NOT called from
# build.sh: build.sh runs under the shared /tmp/armsx-native-build.lock after a
# `make clean`, and a multi-minute Rust build inside that critical section blocks every
# other build on the machine. The .so is checked in; this script is how it got there.
#
# Takes ~1 minute warm, plus a full crates.io fetch cold.
set -euo pipefail

VERSION="${LIBRASHADER_VERSION:-0.12.0}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK="${LIBRASHADER_WORK_DIR:-${TMPDIR:-/tmp}/librashader-android-build}"

: "${ANDROID_NDK_ROOT:=${ANDROID_HOME:-$HOME/Library/Android/sdk}/ndk/29.0.14206865}"
if [ ! -d "$ANDROID_NDK_ROOT" ]; then
    echo "ANDROID_NDK_ROOT does not exist: $ANDROID_NDK_ROOT" >&2
    exit 1
fi
command -v cargo >/dev/null || { echo "cargo is required" >&2; exit 1; }

HOST_TAG="darwin-x86_64"
case "$(uname -s)" in
    Linux) HOST_TAG="linux-x86_64" ;;
esac
TC="$ANDROID_NDK_ROOT/toolchains/llvm/prebuilt/$HOST_TAG"

# API 26 matches the rest of the ARMSX Android build.
export CC_aarch64_linux_android="$TC/bin/aarch64-linux-android26-clang"
export CXX_aarch64_linux_android="$TC/bin/aarch64-linux-android26-clang++"
export AR_aarch64_linux_android="$TC/bin/llvm-ar"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$TC/bin/aarch64-linux-android26-clang"

rustup target list --installed 2>/dev/null | grep -qx aarch64-linux-android \
    || rustup target add aarch64-linux-android

mkdir -p "$WORK"
cd "$WORK"

if [ ! -d "librashader-capi-$VERSION" ]; then
    curl -sSL "https://static.crates.io/crates/librashader-capi/librashader-capi-$VERSION.crate" \
        -o "capi-$VERSION.tar.gz"
    tar xzf "capi-$VERSION.tar.gz"
fi

cd "librashader-capi-$VERSION"

# CARGO_TARGET_DIR must sit OUTSIDE the crate: cargo refuses to write into an extracted
# .crate's read-only-ish tree cleanly, and keeping it out makes the source directory
# byte-comparable with third_party/librashader/source/.
export CARGO_TARGET_DIR="$WORK/target"

# --no-default-features is MANDATORY. The crate's default = ["runtime-all"] pulls in
# D3D9/11/12, Metal and desktop OpenGL; none of those exist on Android and the D3D ones
# do not even compile there. runtime-vulkan is the only runtime ARMSX uses — see
# frontend/render_shaders.cpp for why Vulkan and not GL.
cargo build --release --target aarch64-linux-android \
    --no-default-features --features runtime-vulkan

OUT="$CARGO_TARGET_DIR/aarch64-linux-android/release/liblibrashader_capi.so"
[ -f "$OUT" ] || { echo "build produced no $OUT" >&2; exit 1; }

# Hard requirement: libvulkan must NOT be a DT_NEEDED anywhere in the ARMSX Android
# build, because custom Vulkan drivers (adrenotools/Turnip) are injected by dlopen and a
# hard link to the system loader defeats that. librashader resolves Vulkan through the
# PFN_vkGetInstanceProcAddr the caller hands it, so this holds — verify, do not assume.
if "$TC/bin/llvm-readelf" -d "$OUT" | grep -q "libvulkan"; then
    echo "REFUSING: the built .so has a libvulkan DT_NEEDED entry." >&2
    exit 1
fi

mkdir -p "$HERE/prebuilt/arm64-v8a"
cp "$OUT" "$HERE/prebuilt/arm64-v8a/liblibrashader_capi.so"

echo "staged $(ls -l "$HERE/prebuilt/arm64-v8a/liblibrashader_capi.so" | awk '{print $5}') bytes"
"$TC/bin/llvm-readelf" -d "$HERE/prebuilt/arm64-v8a/liblibrashader_capi.so" | grep NEEDED

# MPL-2.0 source availability: keep source/ in step with the binary.
echo
echo "Reminder: refresh third_party/librashader/source/*.crate from"
echo "  ~/.cargo/registry/cache/*/librashader*-$VERSION.crate"
echo "and update the pin in third_party/librashader/NOTICE."
