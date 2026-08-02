/*
    ARMSX — profile-guided optimisation (PGO) runtime seam.

    Built three ways, selected by the Makefile's PGO= switch (see the PGO block in Makefile for
    the build side and tools/pgo.sh for the driver):

        PGO=off        (default)  nothing here does anything.
        PGO=generate              -fprofile-generate. The emulator carries LLVM's counter arrays
                                  and this file owns WHERE and WHEN they are written out.
        PGO=use                   -fprofile-use=<merged.profdata>. No counters; this file only
                                  reports provenance so a stale profile is visible in logcat.

    ────────────────────────────────────────────────────────────────────────────────────────────
    WHY THIS FILE EXISTS AT ALL: LLVM WRITES THE PROFILE AT PROCESS EXIT, AND ANDROID APPS DO NOT
    EXIT
    ────────────────────────────────────────────────────────────────────────────────────────────
    compiler-rt registers its writer with atexit(). That is fine for a command-line tool. An
    Android app is SIGKILLed by the activity manager when the user swipes it away or the system
    reclaims memory, and SIGKILL cannot be caught — atexit never runs, and an instrumented build
    left to its own devices produces a .profraw file of exactly zero bytes, forever. The whole
    pipeline looks like it works right up until llvm-profdata says "empty raw profile file".

    So the writes are explicit, at the two moments the app is known to still be alive:

      1. psxe_host_set_audio_suspended(1) — the app just went off-screen. This is the LAST
         guaranteed callback before the process becomes killable, so it is the one that actually
         saves the data. Home button, screen off, task switch.
      2. the emulator session teardown, right after psx_destroy() flushes the memory cards
         (frontend/main.cpp). The clean-shutdown path.

    Both write and then RESET the counters, so each flush contributes only what happened since
    the last one. That is what makes repeated flushes safe: the filename below ends in %m, which
    puts compiler-rt in merge mode — it locks the file and adds the new counters to what is
    already there. Without the reset, a session flushed at minute 1 and again at minute 10 would
    count minute 0-1 twice and skew the profile toward the boot path.

    ────────────────────────────────────────────────────────────────────────────────────────────
    WHERE THE PROFILE GOES
    ────────────────────────────────────────────────────────────────────────────────────────────
    LLVM_PROFILE_FILE is the documented way to steer this, and it is useless here: nothing sets
    environment variables for an Android app short of a wrap.sh property. __llvm_profile_set_filename()
    is the same knob callable from inside the process, so that is what armsx_pgo_set_output_dir()
    uses, pointed at the app's own files dir (/data/data/com.nanodata.armsx/files/pgo/ — already
    proven writable, it is where files/logs/ lives).

    ★ compiler-rt does NOT create directories. Given a path whose parent is missing it fails at
      fopen and returns non-zero, and — since nothing checks that return value by default — the
      profile silently never appears. mkdir() below is load-bearing, and the return value of
      __llvm_profile_write_file() is checked and logged for the same reason.

    ────────────────────────────────────────────────────────────────────────────────────────────
    PROVENANCE (THE STALENESS GUARD)
    ────────────────────────────────────────────────────────────────────────────────────────────
    A stale profile is not a no-op — it actively pessimises code, by telling the optimiser that
    cold paths are hot. In the sibling PS2 project exactly this cost a large chunk of VU
    recompiler throughput and took real effort to find, because nothing anywhere said "this
    profile predates the code".

    So the build stamps its identity in (ARMSX_PGO_BUILD_COMMIT / ARMSX_PGO_SOURCE_STAMP), the
    instrumented build writes that identity to armsx-pgo-build.txt NEXT TO the .profraw, and
    tools/pgo.sh carries it forward into the merged profile's provenance file. The device
    therefore says which build produced the counters — no operator bookkeeping to get wrong.
    The three cross-checks that fall out of it:

      - Makefile, at PGO=use time: profile provenance vs the tree being compiled. Hard error.
      - clang, per function: "control flow change detected (hash mismatch)". This is emitted
        under -Wbackend-plugin, NOT -Wprofile-instr-out-of-date (that one only covers frontend
        instrumentation, -fprofile-instr-generate, which is not what this uses). PGO_STRICT=1
        promotes it to an error; getting that diagnostic name wrong is precisely how a stale
        profile gets waved through.
      - here, at runtime: the banner below puts the answer in logcat, so a build in someone
        else's hands can still be identified.
*/

#include "pgo.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#if defined(__ANDROID__)
#include <android/log.h>
#endif

/* Defaults so the file compiles standalone; the Makefile supplies the real values. */
#ifndef ARMSX_PGO_BUILD_COMMIT
#define ARMSX_PGO_BUILD_COMMIT "unknown"
#endif
#ifndef ARMSX_PGO_SOURCE_STAMP
#define ARMSX_PGO_SOURCE_STAMP "unknown"
#endif
#ifndef ARMSX_PGO_PROFILE_COMMIT
#define ARMSX_PGO_PROFILE_COMMIT ""
#endif
#ifndef ARMSX_PGO_PROFILE_STAMP
#define ARMSX_PGO_PROFILE_STAMP ""
#endif
#ifndef ARMSX_PGO_PROFILE_FRESH
#define ARMSX_PGO_PROFILE_FRESH 1
#endif

static void pgo_log(const char* fmt, ...)
#if defined(__GNUC__)
    __attribute__((format(printf, 1, 2)))
#endif
;

static void pgo_log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if defined(__ANDROID__)
    __android_log_vprint(ANDROID_LOG_INFO, "ARMSX-PGO", fmt, ap);
#else
    fputs("[pgo] ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
}

int armsx_pgo_mode(void) {
#if defined(ARMSX_PGO_GENERATE)
    return ARMSX_PGO_MODE_GENERATE;
#elif defined(ARMSX_PGO_USE)
    return ARMSX_PGO_MODE_USE;
#else
    return ARMSX_PGO_MODE_OFF;
#endif
}

const char* armsx_pgo_build_commit(void) { return ARMSX_PGO_BUILD_COMMIT; }
const char* armsx_pgo_source_stamp(void) { return ARMSX_PGO_SOURCE_STAMP; }
const char* armsx_pgo_profile_commit(void) { return ARMSX_PGO_PROFILE_COMMIT; }
const char* armsx_pgo_profile_stamp(void) { return ARMSX_PGO_PROFILE_STAMP; }

int armsx_pgo_profile_is_fresh(void) {
#if defined(ARMSX_PGO_USE)
    return ARMSX_PGO_PROFILE_FRESH;
#else
    return 1;
#endif
}

/* ------------------------------------------------------------------------------------------ */

#if defined(ARMSX_PGO_GENERATE)

/* compiler-rt's profile runtime. Declared rather than #included: <profile/InstrProfiling.h> is
   not on the include path of a normal build, and these four signatures are ABI-stable. Linked in
   by -fprofile-generate on the LINK line (see the Makefile's PGO_LDFLAGS) — compiling with it
   but not linking with it fails with an undefined __llvm_profile_runtime, which is the intended
   tripwire. */
extern int  __llvm_profile_write_file(void);
extern void __llvm_profile_set_filename(const char* name);
extern void __llvm_profile_reset_counters(void);

#include <pthread.h>

static pthread_mutex_t g_pgo_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_pgo_path[1024];
static int  g_pgo_ready;
static unsigned long g_pgo_flushes;

/* Dropped next to the .profraw so the counters and the build that produced them travel together.
   tools/pgo.sh reads this back and refuses to merge a directory whose builds disagree. */
static void pgo_write_sidecar(const char* dir) {
    char path[1024];
    FILE* f;

    if ((size_t)snprintf(path, sizeof(path), "%s/armsx-pgo-build.txt", dir) >= sizeof(path)) {
        return;
    }

    f = fopen(path, "w");
    if (!f) {
        pgo_log("could not write provenance sidecar %s", path);
        return;
    }

    fprintf(f, "# Written by frontend/pgo.c. Identifies the build these .profraw files came from.\n");
    fprintf(f, "mode=generate\n");
    fprintf(f, "commit=%s\n", ARMSX_PGO_BUILD_COMMIT);
    fprintf(f, "stamp=%s\n", ARMSX_PGO_SOURCE_STAMP);
    fprintf(f, "built=%s %s\n", __DATE__, __TIME__);
    fclose(f);
}

void armsx_pgo_set_output_dir(const char* dir) {
    char profile_dir[1024];

    if (!dir || !*dir) {
        return;
    }

    pthread_mutex_lock(&g_pgo_lock);
    if (g_pgo_ready) {
        pthread_mutex_unlock(&g_pgo_lock);
        return;
    }

    if ((size_t)snprintf(profile_dir, sizeof(profile_dir), "%s/pgo", dir) >= sizeof(profile_dir)) {
        pgo_log("output dir '%s' is too long; profiles disabled", dir);
        pthread_mutex_unlock(&g_pgo_lock);
        return;
    }

    /* Not optional: compiler-rt will not create this, it will just fail to open the file. */
    if (mkdir(profile_dir, 0700) != 0) {
        struct stat st;
        if (stat(profile_dir, &st) != 0) {
            pgo_log("could not create %s; profiles disabled", profile_dir);
            pthread_mutex_unlock(&g_pgo_lock);
            return;
        }
    }

    /* %m = merge pool. compiler-rt locks the file and ADDS to the counters already in it, which
       is what lets every flush — and every subsequent launch of the app — accumulate into one
       file instead of clobbering the last session. %p (pid) is deliberately NOT used: it would
       give every launch its own file and defeat that. */
    if ((size_t)snprintf(g_pgo_path, sizeof(g_pgo_path), "%s/armsx-%%m.profraw", profile_dir)
            >= sizeof(g_pgo_path)) {
        g_pgo_path[0] = '\0';
        pthread_mutex_unlock(&g_pgo_lock);
        return;
    }

    __llvm_profile_set_filename(g_pgo_path);
    pgo_write_sidecar(profile_dir);
    g_pgo_ready = 1;
    pthread_mutex_unlock(&g_pgo_lock);

    pgo_log("instrumented build; counters -> %s (commit %s, stamp %s)",
            g_pgo_path, ARMSX_PGO_BUILD_COMMIT, ARMSX_PGO_SOURCE_STAMP);
}

int armsx_pgo_flush(const char* reason) {
    int rc;
    unsigned long n;

    pthread_mutex_lock(&g_pgo_lock);
    if (!g_pgo_ready) {
        pthread_mutex_unlock(&g_pgo_lock);
        /* Loud on purpose. A silent "nothing to flush" here is the failure mode where the app
           runs for an hour and produces no profile at all. */
        pgo_log("flush(%s) BEFORE the output dir was set — nothing written",
                reason ? reason : "?");
        return 0;
    }

    rc = __llvm_profile_write_file();
    if (rc == 0) {
        /* Only the delta from here on; %m merging accumulates the rest. */
        __llvm_profile_reset_counters();
        n = ++g_pgo_flushes;
    } else {
        n = g_pgo_flushes;
    }
    pthread_mutex_unlock(&g_pgo_lock);

    if (rc != 0) {
        pgo_log("flush(%s) FAILED (rc=%d) writing %s", reason ? reason : "?", rc, g_pgo_path);
        return 0;
    }

    pgo_log("flush(%s) ok — %lu total, %s", reason ? reason : "?", n, g_pgo_path);
    return 1;
}

const char* armsx_pgo_output_path(void) {
    return g_pgo_path;
}

#else /* not an instrumented build */

void armsx_pgo_set_output_dir(const char* dir) { (void)dir; }
int armsx_pgo_flush(const char* reason) { (void)reason; return 0; }
const char* armsx_pgo_output_path(void) { return ""; }

#endif /* ARMSX_PGO_GENERATE */

/* ------------------------------------------------------------------------------------------ */

#if defined(ARMSX_PGO_GENERATE) || defined(ARMSX_PGO_USE)

/* Says what this binary is, in logcat, before anything else runs. The point is that a PGO build
   is never anonymous: an instrumented build is several times slower than a release build and a
   PGO=use build compiled against a stale profile is slower than no PGO at all, and both of those
   have been mistaken for emulator regressions. */
__attribute__((constructor))
static void armsx_pgo_banner(void) {
#if defined(ARMSX_PGO_GENERATE)
    pgo_log("BUILD IS INSTRUMENTED (-fprofile-generate) — expect it to run slow. "
            "commit %s stamp %s", ARMSX_PGO_BUILD_COMMIT, ARMSX_PGO_SOURCE_STAMP);
#else
    pgo_log("optimised with a profile — build commit %s stamp %s; profile from commit %s stamp %s",
            ARMSX_PGO_BUILD_COMMIT, ARMSX_PGO_SOURCE_STAMP,
            ARMSX_PGO_PROFILE_COMMIT, ARMSX_PGO_PROFILE_STAMP);
    if (!ARMSX_PGO_PROFILE_FRESH) {
        pgo_log("!!! THIS PROFILE IS STALE — it was recorded against different sources. "
                "Expect it to make things SLOWER, not faster. Regenerate with tools/pgo.sh.");
    }
#endif
}

#endif
