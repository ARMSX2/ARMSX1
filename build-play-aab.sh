#!/usr/bin/env bash
#
# Build the Play (Android App Bundle) artifact, and refuse to hand one over that contains the
# in-app updater.
#
# WHY THIS EXISTS
# ---------------
# ARMSX1 ships two ways: a sideloaded APK from GitHub, which updates itself, and an app bundle for
# Play, which must not. A self-updating app is a hard Play-policy violation ("Device and Network
# Abuse" — Play has to own updates), and the failure mode is not a build error: the updater simply
# rides along and the store account takes the hit weeks later.
#
# The app already keeps them apart with a `store` flavor dimension — the real updater lives in
# android/app/src/github (plus REQUEST_INSTALL_PACKAGES and the update FileProvider in
# src/github/AndroidManifest.xml), and src/play supplies no-op stubs. That separation is enforced by
# source-set layout alone, so it can be broken silently by an ordinary-looking edit: moving the real
# UpdaterEntry back to src/main, adding the permission to the main manifest "to fix a merge", or
# flipping IN_APP_UPDATER.
#
# So this script does not trust the layout. It builds the bundle and then opens it and checks.
#
# :app:verifyPlayFlavorCleanRelease (in android/app/build.gradle) runs the same check from inside
# Gradle and finalizes every play assemble/bundle task, so it fires even when someone runs Gradle
# directly. This script is the second, independent copy of the check: it re-verifies the .aab on
# disk with nothing but unzip and grep, so a mistake in the Gradle wiring cannot also disable the
# verification. Both layers must pass.
#
# USAGE
#   ./build-play-aab.sh [-Parmsx.versionName=1.2.0 -Parmsx.versionCode=9 ...]
#
# Any extra arguments are passed straight to Gradle, which is how the release version is stamped:
#   ./build-play-aab.sh -Parmsx.versionName=1.2.0 -Parmsx.versionCode=9
#
# SIGNING IS DELIBERATELY NOT HANDLED HERE. The release buildType falls back to the repo debug
# keystore, which Play will reject. Sign the produced .aab with the upload key out of band; no
# key material belongs in this repo.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_root/android"

# The updater builds its own native libs via build.sh; leave that to the caller so two concurrent
# native builds cannot clobber each other's bin/obj (see ARMSX1_STATUS.md).
export ARMSX_SKIP_NATIVE_PREPARE="${ARMSX_SKIP_NATIVE_PREPARE:-1}"

echo "==> Building play release bundle (:app:bundlePlayRelease)"
./gradlew :app:bundlePlayRelease "$@"

aab="$(find app/build/outputs/bundle/playRelease -name '*.aab' -type f 2>/dev/null | head -n 1 || true)"
if [ -z "$aab" ]; then
    # Fail closed: "found nothing to check" must never be reported as "the check passed".
    echo "FAIL: no .aab under app/build/outputs/bundle/playRelease — nothing was verified." >&2
    exit 1
fi
echo "==> Verifying $aab"

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
# Only entries that can carry code or manifest data. An .aab holds its manifest as protobuf and its
# code as dex, both of which store strings as plain UTF-8, so a byte grep finds them. (`grep -a`
# because these are binary files.)
unzip -q -o "$aab" -d "$work" 'base/dex/*' 'base/manifest/*' 'base/root/*' 2>/dev/null || true

# Fingerprints only the real (src/github) updater can leave in an artifact. Control-tested against a
# built pair of APKs: every one of these is present in the github artifact and absent from the play
# one.
#
# NOT on this list, on purpose: "https://api.github.com/repos/ARMSX2/ARMSX1/releases". News.kt in
# src/main uses that identical URL for the in-app changelog, so it is in BOTH flavors and would fire
# on every clean play build. A guard that cries wolf gets switched off.
markers=(
    'android.permission.REQUEST_INSTALL_PACKAGES'
    '.updateprovider'
    'application/vnd.android.package-archive'
    'ARMSX1-Updater/'
    'armsx1-update.apk'
)

violations=0
for m in "${markers[@]}"; do
    if hits="$(grep -a -r -l -F -e "$m" "$work" 2>/dev/null)" && [ -n "$hits" ]; then
        echo "VIOLATION: '$m' found in:" >&2
        echo "$hits" | sed "s|^$work/|  |" >&2
        violations=$((violations + 1))
    fi
done

if [ "$violations" -ne 0 ]; then
    cat >&2 <<'EOF'

PLAY BUNDLE IS NOT CLEAN — the in-app updater reached the .aab.
A self-updating app is a Play-policy violation. DO NOT UPLOAD THIS BUILD.

The updater must exist only in android/app/src/github (code) and
android/app/src/github/AndroidManifest.xml (REQUEST_INSTALL_PACKAGES + the update FileProvider),
and every shared call site must stay behind `if (BuildConfig.IN_APP_UPDATER)`.
EOF
    exit 1
fi

echo "==> CLEAN: none of the ${#markers[@]} updater fingerprints are present."
echo "    $repo_root/android/$aab"
echo "    Sign with the Play upload key before uploading (this script does not sign)."
