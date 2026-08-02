#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
abis="${ANDROID_ABIS:-x86 x86_64 armeabi-v7a arm64-v8a}"

if [ -z "${ANDROID_NDK_ROOT:-}" ]; then
    echo "ANDROID_NDK_ROOT must be set." >&2
    exit 1
fi

if [ -z "${GRADLE_BIN:-}" ]; then
    echo "GRADLE_BIN must be set to a Gradle executable." >&2
    exit 1
fi

rm -rf "$repo_root/android/app/src/main/jniLibs" \
       "$repo_root/android/native-deps"

for abi in $abis; do
    echo "Building Android ABI: $abi"
    (
        cd "$repo_root"
        ANDROID_ABI="$abi" ./build.sh android
    )
done

(
    cd "$repo_root"
    export ARMSX_SKIP_NATIVE_PREPARE=1
    # assembleGithubDebug, NOT assembleDebug. The app has a "store" flavor dimension
    # (github = sideload APK with the in-app updater, play = updater-free, for an app bundle), so
    # bare `assembleDebug` is now an aggregate that builds BOTH and the output moved from
    # app/build/outputs/apk/debug/app-debug.apk to
    # app/build/outputs/apk/github/debug/app-github-debug.apk. CI publishes the sideload APK, so it
    # wants the github flavor specifically. Anything still pointing at the old path silently
    # publishes a stale artifact left over from before the split.
    "$GRADLE_BIN" -p android --no-daemon assembleGithubDebug
)
