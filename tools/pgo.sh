#!/bin/sh
#
# ARMSX — profile-guided optimisation, end to end.
#
#   tools/pgo.sh generate     build the INSTRUMENTED apk. Install it, play, then:
#   tools/pgo.sh pull         copy the .profraw files off the device (needs adb)
#   tools/pgo.sh merge        llvm-profdata merge -> build/pgo/armsx.profdata + provenance
#   tools/pgo.sh use          build the OPTIMISED apk against that profile
#   tools/pgo.sh status       what is on disk, and whether it is stale
#   tools/pgo.sh clean        throw the collected profiles away
#
# The build side lives in the PGO block of Makefile; the on-device side in frontend/pgo.c. Read
# those two before changing anything here.
#
# ────────────────────────────────────────────────────────────────────────────────────────────────
# WHY EVERY STEP ASSERTS
# ────────────────────────────────────────────────────────────────────────────────────────────────
# The sibling PS2 project has a recorded incident where a staging step produced nothing at all,
# the build still exited 0, and the wrong artifact shipped. Every stage below therefore checks
# its own output — file exists, is non-empty, has the section/symbol it is supposed to have, and
# in the profile's case actually parses — and stops the loop where it broke rather than one step
# later where the symptom is unrecognisable. `set -e` alone does not do this: adb, cp and the
# gradle wrapper all have paths that succeed while producing nothing.
#
# ────────────────────────────────────────────────────────────────────────────────────────────────
# WHAT TO PLAY WHILE THE INSTRUMENTED BUILD IS INSTALLED
# ────────────────────────────────────────────────────────────────────────────────────────────────
# The profile is only as good as the code it saw run. Aim for ~25-30 minutes total, and note the
# instrumented build is SEVERAL TIMES SLOWER than release — that is expected, it does not
# invalidate the profile (counter ratios are what matter, not wall-clock).
#
#   1. Library / menus, ~2 min. Boot, scroll the game list, open settings, back out. Cheap, and
#      it is the first thing every user touches.
#   2. A BIOS boot to the PS1 menu, ~1 min. Exercises the CD-ROM paths and the BIOS text raster
#      that nothing else covers the same way.
#   3. Xenogears, a full random battle plus a few minutes of field walking, ~8 min. Heavy 2D+3D
#      mix and long DMA display lists; the single best workload for the GPU command path.
#   4. An FMV, ~3 min. MDEC block decode + XA streaming + the CD read path, none of which any
#      other workload touches meaningfully.
#   5. Crash or Spyro, ~8 min of actual play, not the title screen. Rasterizer-bound 3D: this is
#      what weights the triangle/quad inner loops.
#   6. One more 3D title with a different renderer profile, ~5 min — Gran Turismo or Tekken 3.
#      Guards against over-fitting the whole profile to one engine's draw pattern.
#   7. Save + load a state once, and let some audio play uninterrupted for a minute (SPU reverb).
#
# Between each, press HOME. That is what triggers the flush (see frontend/pgo.c) — a session that
# is never backgrounded and never closed cleanly contributes NOTHING, because Android SIGKILLs
# the process and compiler-rt's atexit writer never runs.

set -e

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "${REPO_ROOT}"

PKG="${ARMSX_PKG:-com.nanodata.armsx}"
PGO_DIR="${PGO_DIR:-build/pgo}"
RAW_DIR="${PGO_DIR}/raw"
PROFILE="${PGO_DIR}/armsx.profdata"
PROVENANCE="${PROFILE}.provenance"
NATIVE_LIB="android/app/src/main/jniLibs/arm64-v8a/libarmsx.so"
APK_DIR="android/app/build/outputs/apk/github/debug"

JAVA_HOME="${JAVA_HOME:-/Applications/Android Studio.app/Contents/jbr/Contents/Home}"
ANDROID_HOME="${ANDROID_HOME:-$HOME/Library/Android/sdk}"
ANDROID_NDK_ROOT="${ANDROID_NDK_ROOT:-$ANDROID_HOME/ndk/29.0.14206865}"
export JAVA_HOME ANDROID_HOME ANDROID_NDK_ROOT
# platform-tools too: adb ships inside the SDK and is not on a normal PATH here, so `pull` would
# otherwise fail on a machine that has adb installed the whole time.
export PATH="${ANDROID_HOME}/cmake/3.30.5/bin:${ANDROID_HOME}/platform-tools:${PATH}"

NDK_BIN="${ANDROID_NDK_ROOT}/toolchains/llvm/prebuilt/darwin-x86_64/bin"
PROFDATA="${NDK_BIN}/llvm-profdata"
READELF="${NDK_BIN}/llvm-readelf"
NM="${NDK_BIN}/llvm-nm"

LOCK_DIR="/tmp/armsx-native-build.lock"

die() { printf '\n[pgo] FAILED: %s\n' "$*" >&2; exit 1; }
say() { printf '[pgo] %s\n' "$*"; }
rule() { printf '\n───────────────────────────────────────────────────────────────────────────────\n'; }

# assert_file <path> <what it was supposed to be>
assert_file() {
    [ -f "$1" ] || die "$2 — expected a file at $1 and there is none"
    [ -s "$1" ] || die "$2 — $1 exists but is EMPTY (0 bytes), which is the silent-failure case"
}

need_tool() {
    [ -x "$1" ] || die "missing toolchain component: $1"
}

need_adb() {
    command -v adb >/dev/null 2>&1 || die "adb is not on PATH; needed for '$1'"
}

# The repo-wide native build lock. Two concurrent native builds share bin/obj and clobber each
# other, which shows up much later as a link error in an unrelated file.
take_lock() {
    say "waiting for the native build lock (${LOCK_DIR})..."
    while ! mkdir "${LOCK_DIR}" 2>/dev/null; do sleep 15; done
    LOCK_HELD=1
    trap 'if [ "${LOCK_HELD:-0}" = "1" ]; then rmdir "'"${LOCK_DIR}"'" 2>/dev/null || true; fi' EXIT INT TERM
    say "lock acquired"
}

drop_lock() {
    if [ "${LOCK_HELD:-0}" = "1" ]; then
        rmdir "${LOCK_DIR}" 2>/dev/null || true
        LOCK_HELD=0
    fi
}

git_head() { git rev-parse HEAD 2>/dev/null || echo unknown; }

# ★ ASKS THE MAKEFILE. Do not reimplement this here.
#
# The first draft did compute its own hash, over a shell glob list that included psx/disc/ — a
# directory that does not exist. The glob matched nothing, `cat` was handed nothing, and
# git hash-object dutifully returned e69de29bb2d1d643, the hash of the empty blob. That is a
# perfectly stable-looking stamp which matches no build ever, and had it been empty on BOTH
# sides it would have matched EVERY build. The Makefile's $(wildcard) is immune to that class of
# mistake, so there is exactly one implementation and this defers to it.
source_stamp() {
    s=$(make -s pgo-stamp 2>/dev/null || true)
    case "${s}" in
        "" )                  die "could not get a source stamp from 'make pgo-stamp'" ;;
        e69de29bb2d1d643 )    die "the source stamp hashed NOTHING (git's empty-blob hash). \
PGO_STAMP_INPUTS in the Makefile matched no files, so staleness cannot be detected. Fix that \
before trusting any profile." ;;
    esac
    printf '%s' "${s}"
}

# ── build ───────────────────────────────────────────────────────────────────────────────────────

build_apk() {
    mode="$1"

    take_lock
    say "native build: PGO=${mode}"
    PGO="${mode}" ./build.sh android
    drop_lock

    assert_file "${NATIVE_LIB}" "PGO=${mode} native build"

    # The .so has to actually BE what the mode claims. build.sh is a long script and a flag that
    # silently fails to reach the compiler produces a perfectly good non-PGO library.
    need_tool "${READELF}"
    has_counters=$("${READELF}" -S "${NATIVE_LIB}" 2>/dev/null | grep -c "__llvm_prf_cnts" || true)
    if [ "${mode}" = "generate" ]; then
        [ "${has_counters}" -ge 1 ] || die \
            "the 'instrumented' ${NATIVE_LIB} has NO __llvm_prf_cnts section — it is not \
instrumented, and profiling it would produce nothing. -fprofile-generate did not reach the compiler."
        need_tool "${NM}"
        "${NM}" --defined-only "${NATIVE_LIB}" 2>/dev/null | grep -q "__llvm_profile_write_file" || die \
            "instrumented ${NATIVE_LIB} does not contain __llvm_profile_write_file — the profile \
runtime was not linked in, so nothing can ever write a .profraw."
        say "verified: instrumented (__llvm_prf_cnts present, profile runtime linked)"
    else
        [ "${has_counters}" -eq 0 ] || die \
            "a PGO=${mode} build should carry NO counters, but ${NATIVE_LIB} has an \
__llvm_prf_cnts section. You are about to ship an instrumented library."
        say "verified: not instrumented"
    fi

    say "gradle: assembleGithubDebug"
    # ARMSX_SKIP_NATIVE_PREPARE=1 because build.sh already ran above, WITH the PGO flags. Without
    # it gradle's prepareNativeBinaries would run build.sh again — without them — and quietly
    # overwrite the library that was just verified.
    ( cd android && ARMSX_SKIP_NATIVE_PREPARE=1 ./gradlew :app:assembleGithubDebug --rerun-tasks )

    apk=$(ls -t "${APK_DIR}"/*.apk 2>/dev/null | head -1 || true)
    [ -n "${apk}" ] || die "gradle exited 0 but produced no .apk under ${APK_DIR}"
    assert_file "${apk}" "apk packaging"

    # And the apk has to contain the library that was verified, not a stale one from a previous
    # build sitting in the merged-jni cache.
    unzip -p "${apk}" lib/arm64-v8a/libarmsx.so > "${PGO_DIR}/.apk-lib-check.so" 2>/dev/null || true
    assert_file "${PGO_DIR}/.apk-lib-check.so" "libarmsx.so inside ${apk}"
    packaged=$("${READELF}" -S "${PGO_DIR}/.apk-lib-check.so" 2>/dev/null | grep -c "__llvm_prf_cnts" || true)
    rm -f "${PGO_DIR}/.apk-lib-check.so"
    if [ "${mode}" = "generate" ] && [ "${packaged}" -lt 1 ]; then
        die "the apk contains a libarmsx.so with no counters — packaging picked up a stale library"
    fi
    if [ "${mode}" != "generate" ] && [ "${packaged}" -ne 0 ]; then
        die "the apk contains an INSTRUMENTED libarmsx.so — packaging picked up a stale library"
    fi

    rule
    say "apk: ${apk}"
}

cmd_generate() {
    mkdir -p "${PGO_DIR}"
    build_apk generate

    # Bookkeeping for the merge step. The device writes its own copy of this next to the .profraw
    # (frontend/pgo.c) and merge cross-checks the two, so a profile collected from a DIFFERENT
    # build than the one recorded here is caught rather than merged in.
    {
        echo "commit=$(git_head)"
        echo "stamp=$(source_stamp)"
        echo "built=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    } > "${PGO_DIR}/generate.provenance"
    assert_file "${PGO_DIR}/generate.provenance" "recording what this instrumented build is"

    rule
    say "INSTRUMENTED build ready. Now:"
    say "  1. install the apk above on the device"
    say "  2. play the workloads listed at the top of this script (~25-30 min)"
    say "  3. press HOME between each one — that is what writes the counters out"
    say "  4. tools/pgo.sh pull && tools/pgo.sh merge && tools/pgo.sh use"
}

# ── collect ─────────────────────────────────────────────────────────────────────────────────────

cmd_pull() {
    need_adb pull
    mkdir -p "${RAW_DIR}"

    # run-as, not `adb pull`: files/ is app-private. It works because assembleGithubDebug is
    # debuggable. exec-out rather than shell — shell mangles binary data with CRLF translation on
    # some transports, which corrupts a .profraw into something llvm-profdata rejects.
    names=$(adb exec-out run-as "${PKG}" ls files/pgo 2>/dev/null | tr -d '\r' || true)
    [ -n "${names}" ] || die "no files under ${PKG}'s files/pgo. Either the instrumented build was \
never run, or it was never backgrounded/closed so nothing was ever flushed. Check logcat for \
'ARMSX-PGO' — the instrumented build announces itself at startup."

    got=0
    for n in ${names}; do
        case "${n}" in
            *.profraw|armsx-pgo-build.txt) ;;
            *) continue ;;
        esac
        adb exec-out run-as "${PKG}" cat "files/pgo/${n}" > "${RAW_DIR}/${n}"
        assert_file "${RAW_DIR}/${n}" "pulling files/pgo/${n} off the device"
        say "pulled ${n} ($(wc -c < "${RAW_DIR}/${n}" | tr -d ' ') bytes)"
        got=$((got + 1))
    done

    [ "${got}" -gt 0 ] || die "files/pgo exists but held no .profraw — see above"
    ls "${RAW_DIR}"/*.profraw >/dev/null 2>&1 || die \
        "pulled ${got} file(s) but not one .profraw. The counters were never written; only the \
provenance sidecar was."

    rule
    say "raw profiles in ${RAW_DIR}. Next: tools/pgo.sh merge"
}

# ── merge ───────────────────────────────────────────────────────────────────────────────────────

cmd_merge() {
    src="${1:-${RAW_DIR}}"
    need_tool "${PROFDATA}"
    [ -d "${src}" ] || die "no such directory: ${src}"
    ls "${src}"/*.profraw >/dev/null 2>&1 || die "no .profraw files in ${src}"

    # ★ Provenance cross-check, and the reason this is not just a call to llvm-profdata. The
    # device wrote armsx-pgo-build.txt alongside the counters naming the build that produced
    # them. If that disagrees with the instrumented build recorded locally, the profile came from
    # some other build of the emulator and merging it would produce exactly the stale profile
    # this whole pipeline exists to prevent.
    dev="${src}/armsx-pgo-build.txt"
    local_prov="${PGO_DIR}/generate.provenance"
    if [ -f "${dev}" ] && [ -f "${local_prov}" ]; then
        dev_stamp=$(sed -n 's/^stamp=//p' "${dev}" | head -1)
        loc_stamp=$(sed -n 's/^stamp=//p' "${local_prov}" | head -1)
        if [ -n "${dev_stamp}" ] && [ "${dev_stamp}" != "${loc_stamp}" ]; then
            die "the device's profile came from source stamp ${dev_stamp}, but the instrumented \
build made here was ${loc_stamp}. That is a profile of a DIFFERENT build of the emulator. Rebuild \
with tools/pgo.sh generate, reinstall, and collect again."
        fi
        say "provenance cross-check ok (device and local build agree: ${loc_stamp})"
        commit=$(sed -n 's/^commit=//p' "${dev}" | head -1)
        stamp="${dev_stamp}"
    elif [ -f "${local_prov}" ]; then
        say "WARNING: no armsx-pgo-build.txt from the device; trusting the local record only"
        commit=$(sed -n 's/^commit=//p' "${local_prov}" | head -1)
        stamp=$(sed -n 's/^stamp=//p' "${local_prov}" | head -1)
    elif [ -f "${dev}" ]; then
        commit=$(sed -n 's/^commit=//p' "${dev}" | head -1)
        stamp=$(sed -n 's/^stamp=//p' "${dev}" | head -1)
    else
        die "no provenance anywhere (neither ${dev} nor ${local_prov}). A profile whose origin is \
unknown cannot be checked for staleness, and an unnoticed stale profile is slower than none."
    fi

    mkdir -p "${PGO_DIR}"
    rm -f "${PROFILE}"
    "${PROFDATA}" merge -output="${PROFILE}" "${src}"/*.profraw \
        || die "llvm-profdata merge failed. A 'malformed profile' here usually means the raw file \
was truncated in transit — re-run tools/pgo.sh pull."
    assert_file "${PROFILE}" "llvm-profdata merge"

    # Merge can succeed on an empty raw profile and hand back a valid, useless, zero-function
    # .profdata. Which would then compile clean and pessimise everything it touched.
    summary=$("${PROFDATA}" show "${PROFILE}" 2>/dev/null) || die "the merged profile does not parse"
    funcs=$(printf '%s\n' "${summary}" | sed -n 's/^Total functions: *//p' | head -1)
    total=$(printf '%s\n' "${summary}" | sed -n 's/^Maximum function count: *//p' | head -1)
    [ -n "${funcs}" ] && [ "${funcs}" -gt 0 ] 2>/dev/null || die \
        "the merged profile contains ZERO functions. The app ran but never executed instrumented \
code, or the counters were reset without ever being written."
    [ -n "${total}" ] && [ "${total}" -gt 0 ] 2>/dev/null || die \
        "the merged profile's maximum function count is 0 — every counter is zero."

    if [ "${funcs}" -lt 200 ]; then
        say "WARNING: only ${funcs} functions have profile data. That is a very thin profile — it \
suggests one short session. More play time across more workloads will do better."
    fi

    {
        echo "# Provenance for ${PROFILE}. The Makefile reads this at PGO=use time and refuses to"
        echo "# build if it disagrees with the tree. Delete it and the build refuses outright."
        echo "commit=${commit:-unknown}"
        echo "stamp=${stamp:-unknown}"
        echo "merged=$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
        echo "functions=${funcs}"
        echo "raw_files=$(ls "${src}"/*.profraw | wc -l | tr -d ' ')"
    } > "${PROVENANCE}"
    assert_file "${PROVENANCE}" "writing the profile's provenance"

    rule
    say "merged ${funcs} functions -> ${PROFILE}"
    say "provenance: commit ${commit:-unknown}, source stamp ${stamp:-unknown}"
    say "next: tools/pgo.sh use"
}

# ── use ─────────────────────────────────────────────────────────────────────────────────────────

cmd_use() {
    assert_file "${PROFILE}" "no merged profile yet — run tools/pgo.sh merge"
    assert_file "${PROVENANCE}" "the profile has no provenance — re-run tools/pgo.sh merge"
    cmd_status
    # The Makefile is what actually refuses a stale profile; this only surfaces it earlier and
    # with the remedy attached.
    build_apk use
    rule
    say "OPTIMISED build ready. Install the apk above."
}

# ── status ──────────────────────────────────────────────────────────────────────────────────────

cmd_status() {
    head=$(git_head)
    stamp=$(source_stamp)
    rule
    say "tree:    commit ${head}"
    say "         source stamp ${stamp}"
    if [ -f "${PROVENANCE}" ]; then
        p_commit=$(sed -n 's/^commit=//p' "${PROVENANCE}" | head -1)
        p_stamp=$(sed -n 's/^stamp=//p' "${PROVENANCE}" | head -1)
        p_funcs=$(sed -n 's/^functions=//p' "${PROVENANCE}" | head -1)
        say "profile: ${PROFILE} (${p_funcs:-?} functions)"
        say "         recorded from commit ${p_commit}"
        say "         source stamp ${p_stamp}"
        if [ "${p_stamp}" = "${stamp}" ]; then
            say "         >>> FRESH — matches this tree"
        else
            say "         >>> ★ STALE — does NOT match this tree."
            say "         >>> PGO=use will refuse to build. A stale profile is slower than none;"
            say "         >>> re-run the loop, or override with PGO_ALLOW_STALE=1 if you have"
            say "         >>> actually measured that it still helps."
        fi
    else
        say "profile: none merged yet (${PROFILE} absent)"
    fi
    rule
}

cmd_clean() {
    rm -rf "${PGO_DIR}"
    say "removed ${PGO_DIR}"
}

case "${1:-}" in
    generate) cmd_generate ;;
    pull)     cmd_pull ;;
    merge)    shift; cmd_merge "$@" ;;
    use)      cmd_use ;;
    status)   cmd_status ;;
    clean)    cmd_clean ;;
    *)
        sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'
        exit 1
        ;;
esac
