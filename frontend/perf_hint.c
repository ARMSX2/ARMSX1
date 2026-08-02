/*
    ARMSX — host CPU scheduling levers. See perf_hint.h for the contract and for why the ADPF
    bracket measures what it measures.

    Everything real in this file is inside #if defined(__ANDROID__). On every other platform
    the entry points are empty bodies that keep the caller in frontend/main.cpp free of
    platform #ifdefs.
*/

/* cpu_set_t / CPU_SET / sched_setaffinity live behind __USE_GNU in bionic's <sched.h>. Must
   precede every include, and guarded because the toolchain may already have defined it. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "perf_hint.h"

#if defined(__ANDROID__)

#include <dlfcn.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../psx/log.h"

/* ============================ shared: monotonic clock ==================================== */

static int64_t armsx_now_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

/* ================================== ADPF ================================================= */

/* Opaque in the NDK header too; declared here so this file needs no android/performance_hint.h
   and therefore no minimum API level. The symbols are resolved by name at run time. */
typedef struct APerformanceHintManager APerformanceHintManager;
typedef struct APerformanceHintSession APerformanceHintSession;

typedef APerformanceHintManager* (*fn_get_manager_t)(void);
typedef APerformanceHintSession* (*fn_create_session_t)(APerformanceHintManager* manager,
                                                        const int32_t* thread_ids,
                                                        size_t size,
                                                        int64_t initial_target_work_duration_nanos);
typedef int (*fn_report_actual_t)(APerformanceHintSession* session, int64_t actual_duration_nanos);
typedef int (*fn_update_target_t)(APerformanceHintSession* session, int64_t target_duration_nanos);
typedef void (*fn_close_session_t)(APerformanceHintSession* session);

static fn_get_manager_t s_fn_get_manager;
static fn_create_session_t s_fn_create_session;
static fn_report_actual_t s_fn_report_actual;
static fn_update_target_t s_fn_update_target;
static fn_close_session_t s_fn_close_session;

/* 0 = not tried yet, 1 = usable, -1 = unavailable (pre-33 device, or a ROM without the
   symbols). Resolution is attempted exactly once and the outcome is logged once. */
static int s_adpf_symbols = 0;

/* Requested state, written by the host from any thread. */
static atomic_int s_adpf_requested;
/* Set the moment the host calls the setter, so a later settings.toml default cannot overwrite
   a live choice — see the note on armsx_perf_hint_adpf_set_default(). */
static atomic_int s_adpf_host_owned;

/* Everything below is touched ONLY on the emulation thread. */
static APerformanceHintManager* s_manager;
static APerformanceHintSession* s_session;
static int32_t s_session_tid;
static int64_t s_target_ns;
static int64_t s_work_start_ns;
static int s_report_failed_logged;

static int armsx_adpf_resolve_symbols(void)
{
    void* lib;

    if (s_adpf_symbols != 0) {
        return s_adpf_symbols > 0;
    }

    /* libandroid.so is already mapped (the JNI host uses ANativeWindow), so this is a
       refcount bump rather than a load. RTLD_NOLOAD is deliberately NOT used: the plain
       command-line/SDLActivity build may reach here before anything else pulled it in. */
    lib = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);

    if (!lib) {
        /* dlerror() is consuming — read it once or the second call returns NULL. */
        const char* why = dlerror();

        s_adpf_symbols = -1;
        log_info("ADPF: libandroid.so unavailable (%s); CPU clock hint disabled",
                 why ? why : "no error reported");
        return 0;
    }

    s_fn_get_manager = (fn_get_manager_t)dlsym(lib, "APerformanceHint_getManager");
    s_fn_create_session = (fn_create_session_t)dlsym(lib, "APerformanceHint_createSession");
    s_fn_report_actual = (fn_report_actual_t)dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    s_fn_update_target = (fn_update_target_t)dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    s_fn_close_session = (fn_close_session_t)dlsym(lib, "APerformanceHint_closeSession");

    /* updateTarget and closeSession are optional in the sense that the feature still works
       without them (the target simply never changes, and the session leaks until process
       exit); the other three are not. In practice all five land or none do — the whole API
       arrived together in API 33. */
    if (!s_fn_get_manager || !s_fn_create_session || !s_fn_report_actual) {
        s_adpf_symbols = -1;
        log_info("ADPF: PerformanceHintManager not present on this device (needs Android 13); "
                 "CPU clock hint disabled");
        return 0;
    }

    s_adpf_symbols = 1;
    return 1;
}

static void armsx_adpf_close_session(void)
{
    if (s_session && s_fn_close_session) {
        s_fn_close_session(s_session);
    }

    if (s_session) {
        log_info("ADPF: session closed");
    }

    s_session = NULL;
    s_session_tid = 0;
    s_target_ns = 0;
    s_work_start_ns = 0;
    s_report_failed_logged = 0;
}

/* The pacer's target for the frame about to run, as a work deadline in nanoseconds. 0 means
   "there is no deadline": the limiter is off or fast-forward is uncapped, and there is no
   honest budget to compare work against. */
static int64_t armsx_adpf_target_from_fps(double target_fps)
{
    if (!(target_fps > 0.0)) {
        return 0;
    }

    /* frontend/main.cpp expresses "uncapped" as an absurdly high target rate rather than as a
       flag. Anything up there is a sentinel, not a real refresh rate. */
    if (target_fps > 1000.0) {
        return 0;
    }

    return (int64_t)(1000000000.0 / target_fps);
}

void armsx_perf_hint_set_adpf_enabled(int enabled)
{
    atomic_store(&s_adpf_host_owned, 1);
    atomic_store(&s_adpf_requested, enabled ? 1 : 0);
}

int armsx_perf_hint_adpf_enabled(void)
{
    return atomic_load(&s_adpf_requested);
}

void armsx_perf_hint_adpf_set_default(int enabled)
{
    if (atomic_load(&s_adpf_host_owned)) {
        return;
    }

    atomic_store(&s_adpf_requested, enabled ? 1 : 0);
}

void armsx_perf_hint_frame_begin(double target_fps)
{
    int64_t target_ns;
    int32_t tid;

    s_work_start_ns = 0;

    if (!atomic_load(&s_adpf_requested)) {
        if (s_session) {
            armsx_adpf_close_session();
        }

        return;
    }

    target_ns = armsx_adpf_target_from_fps(target_fps);

    if (target_ns <= 0) {
        /* No deadline to hint against. The session is left alone rather than torn down: an
           uncapped stretch (fast-forward, limiter off) is usually seconds long and bracketed
           by normal play on either side. */
        return;
    }

    if (!armsx_adpf_resolve_symbols()) {
        return;
    }

    tid = (int32_t)syscall(SYS_gettid);

    /* A session is bound to a fixed thread set, so a different tid (a second run in the same
       process) means a new session, not an update. */
    if (s_session && s_session_tid != tid) {
        armsx_adpf_close_session();
    }

    if (!s_session) {
        if (!s_manager) {
            s_manager = s_fn_get_manager();

            if (!s_manager) {
                s_adpf_symbols = -1; /* stop retrying every frame */
                log_info("ADPF: getManager() returned null; CPU clock hint disabled");
                return;
            }
        }

        s_session = s_fn_create_session(s_manager, &tid, 1u, target_ns);

        if (!s_session) {
            s_adpf_symbols = -1; /* stop retrying every frame */
            log_info("ADPF: createSession() failed; CPU clock hint disabled");
            return;
        }

        s_session_tid = tid;
        s_target_ns = target_ns;
        log_info("ADPF: session active on the emulation thread (tid %d), target %.2f ms",
                 (int)tid, (double)target_ns / 1000000.0);
    } else if (s_fn_update_target && s_target_ns != target_ns) {
        /* Rate changes are rare (region switch, speed percentage, fast-forward on/off), so
           this is not a per-frame call in practice. */
        s_fn_update_target(s_session, target_ns);
        s_target_ns = target_ns;
    }

    s_work_start_ns = armsx_now_ns();
}

void armsx_perf_hint_frame_end(void)
{
    int64_t work_ns;

    if (!s_session || s_work_start_ns == 0) {
        return;
    }

    work_ns = armsx_now_ns() - s_work_start_ns;
    s_work_start_ns = 0;

    if (work_ns <= 0) {
        return;
    }

    /* Outliers are not frames. A save-state restore, a shader-chain build, a disc swap or a
       renderer recreate all happen inside the bracket and are hundreds of milliseconds of work
       that says nothing about how fast the game needs the CPU to be.

       The threshold is deliberately loose. A tight one (the sibling PS2 project used 4x) also
       throws away every frame of a game that is genuinely running at a quarter speed — which is
       the exact situation the hint exists for, and the feature would go silent precisely where
       it is supposed to speak. 10x of a 60 Hz budget is 166 ms, i.e. 6 fps: nobody plays at
       that, so anything above it is a stall, not a slow frame. */
    if (s_target_ns > 0 && work_ns > (s_target_ns * 10)) {
        return;
    }

    if (s_fn_report_actual(s_session, work_ns) != 0 && !s_report_failed_logged) {
        s_report_failed_logged = 1;
        log_warn("ADPF: reportActualWorkDuration() rejected a sample; hint may be inactive");
    }
}

void armsx_perf_hint_pause(void)
{
    s_work_start_ns = 0;
}

void armsx_perf_hint_shutdown(void)
{
    armsx_adpf_close_session();
    s_manager = NULL;
}

/* ================================ affinity =============================================== */

/* ARMSX_AFFINITY_OFF is 0, so zero-initialisation is the default and no static initialiser is
   needed (ATOMIC_VAR_INIT is deprecated and warns on current clang). */
static atomic_int s_affinity_mode;
static atomic_int s_affinity_host_owned;

/* No phone has more, and the array below is a stack frame. CPU_SETSIZE is 1024 in bionic. */
#define ARMSX_AFFINITY_MAX_CPUS 64

/* What THIS thread has already applied, +1 so that 0 means "nothing" — deliberately
   thread-local rather than a plain static. The Android host keeps the process alive between
   games and gives each run a fresh emulation thread, and an affinity mask belongs to a thread,
   not to a process. A process-wide "already applied mode 1" would make the second game's
   thread skip its own pin and run unpinned while the UI still said Performance cores. A new
   thread starts at 0 here and applies from scratch, which is also correct for the off case: a
   thread that was never pinned has nothing to release. */
static __thread int s_affinity_applied_mode_p1;
static int s_affinity_topology_ready;
static int s_affinity_topology_usable;
static cpu_set_t s_affinity_all;         /* the mask this process started with */
static cpu_set_t s_affinity_performance; /* the top-frequency cluster within it */

static long armsx_read_cpu_max_freq(int cpu)
{
    char path[128];
    FILE* file;
    long khz = 0;

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
    file = fopen(path, "r");

    if (!file) {
        return 0;
    }

    if (fscanf(file, "%ld", &khz) != 1) {
        khz = 0;
    }

    fclose(file);
    return khz > 0 ? khz : 0;
}

/* Group the CPUs this process may use by their maximum frequency and take the top group as
   "performance". Detected, never assumed: core INDEX says nothing about core class — the big
   cores are cpu4-7 on a 4+4, cpu6-7 plus a prime cpu7 on a 4+3+1, and there is no rule at all
   once a core is offline or the process is in a restricted cpuset.

   Reading cpuinfo_max_freq is the standard way to do this and it is not universal: some ROMs
   restrict /sys/devices/system/cpu, and every CPU reporting the same frequency is a legitimate
   answer meaning "this SoC is uniform". Both cases end with the performance mode doing
   nothing, which is the correct outcome, not a failure to paper over. */
static void armsx_affinity_detect_topology(void)
{
    long freqs[ARMSX_AFFINITY_MAX_CPUS];
    long top = 0;
    long second = 0;
    int readable = 0;
    int cpu;
    int count_all = 0;
    int count_top = 0;

    if (s_affinity_topology_ready) {
        return;
    }

    s_affinity_topology_ready = 1;
    CPU_ZERO(&s_affinity_all);
    CPU_ZERO(&s_affinity_performance);

    /* Captured BEFORE anything is pinned, so "all cores" restores what the process was given
       rather than what this file guessed — a cpuset-restricted app must not be handed cores
       it was never allowed to use. */
    if (sched_getaffinity(0, sizeof(cpu_set_t), &s_affinity_all) != 0) {
        log_info("Affinity: sched_getaffinity() failed; affinity control disabled");
        return;
    }

    for (cpu = 0; cpu < ARMSX_AFFINITY_MAX_CPUS; ++cpu) {
        freqs[cpu] = 0;

        if (!CPU_ISSET(cpu, &s_affinity_all)) {
            continue;
        }

        ++count_all;
        freqs[cpu] = armsx_read_cpu_max_freq(cpu);

        if (freqs[cpu] > 0) {
            ++readable;

            if (freqs[cpu] > top) {
                top = freqs[cpu];
            }
        }
    }

    if (count_all <= 1) {
        log_info("Affinity: %d usable core(s); affinity control has nothing to do", count_all);
        return;
    }

    if (readable == 0 || top == 0) {
        log_info("Affinity: cpufreq is not readable on this device; "
                 "the performance-core mode will be a no-op");
        return;
    }

    for (cpu = 0; cpu < ARMSX_AFFINITY_MAX_CPUS; ++cpu) {
        if (freqs[cpu] > 0 && freqs[cpu] < top && freqs[cpu] > second) {
            second = freqs[cpu];
        }
    }

    for (cpu = 0; cpu < ARMSX_AFFINITY_MAX_CPUS; ++cpu) {
        if (freqs[cpu] == top) {
            CPU_SET(cpu, &s_affinity_performance);
        }
    }

    count_top = CPU_COUNT(&s_affinity_performance);

    /* A single prime core is a real topology (1+3+4 Snapdragon 8 Gen 2/3, Dimensity 9000).
       Pinning the emulation thread to exactly one core there hands it no room at all: the UI
       thread, the audio thread and every system service still land wherever the scheduler
       likes, and one of them landing on that core now costs a frame instead of being migrated
       away. Widening to the next tier keeps the prime core available without making it the
       only option. */
    if (count_top < 2 && second > 0) {
        for (cpu = 0; cpu < ARMSX_AFFINITY_MAX_CPUS; ++cpu) {
            if (freqs[cpu] == second) {
                CPU_SET(cpu, &s_affinity_performance);
            }
        }

        count_top = CPU_COUNT(&s_affinity_performance);
        log_info("Affinity: top cluster has a single core (%ld kHz); "
                 "widening to include the %ld kHz tier", top, second);
    }

    if (count_top >= count_all) {
        CPU_ZERO(&s_affinity_performance);
        log_info("Affinity: all %d cores report the same maximum frequency (uniform SoC); "
                 "the performance-core mode will be a no-op", count_all);
        return;
    }

    s_affinity_topology_usable = 1;
    log_info("Affinity: %d of %d cores are performance cores (%ld kHz)",
             count_top, count_all, top);
}

static int armsx_affinity_normalize(int mode)
{
    if (mode == ARMSX_AFFINITY_PERFORMANCE_ALT) {
        return ARMSX_AFFINITY_PERFORMANCE;
    }

    if (mode == ARMSX_AFFINITY_PERFORMANCE || mode == ARMSX_AFFINITY_ALL) {
        return mode;
    }

    return ARMSX_AFFINITY_OFF;
}

void armsx_affinity_set_mode(int mode)
{
    atomic_store(&s_affinity_host_owned, 1);
    atomic_store(&s_affinity_mode, armsx_affinity_normalize(mode));
}

int armsx_affinity_mode(void)
{
    return atomic_load(&s_affinity_mode);
}

void armsx_affinity_set_default_mode(int mode)
{
    if (atomic_load(&s_affinity_host_owned)) {
        return;
    }

    atomic_store(&s_affinity_mode, armsx_affinity_normalize(mode));
}

void armsx_affinity_apply_emulation_thread(void)
{
    const int mode = atomic_load(&s_affinity_mode);
    const int applied = s_affinity_applied_mode_p1 - 1; /* -1 when this thread has applied none */
    const cpu_set_t* want;

    if (mode == applied) {
        return;
    }

    /* The default. Nothing has been pinned, so there is nothing to undo and no reason to go
       and read sysfs — a run that never turns the setting on must not touch the CPU topology
       or print a line about it. */
    if (mode == ARMSX_AFFINITY_OFF && applied <= ARMSX_AFFINITY_OFF) {
        s_affinity_applied_mode_p1 = mode + 1;
        return;
    }

    armsx_affinity_detect_topology();

    if (mode == ARMSX_AFFINITY_OFF) {
        /* Off does not mean "restore": a mode this thread never applied has nothing to undo,
           and re-asserting a mask it never asked for would itself be a change. Only a previous
           pin is reverted, and it is reverted to the mask captured at startup. */
        if (applied > ARMSX_AFFINITY_OFF && CPU_COUNT(&s_affinity_all) > 0) {
            if (sched_setaffinity(0, sizeof(cpu_set_t), &s_affinity_all) == 0) {
                log_info("Affinity: emulation thread released back to all cores");
            }
        }

        s_affinity_applied_mode_p1 = mode + 1;
        return;
    }

    if (mode == ARMSX_AFFINITY_PERFORMANCE && s_affinity_topology_usable) {
        want = &s_affinity_performance;
    } else if (CPU_COUNT(&s_affinity_all) > 0) {
        /* ALL, and the fallback for PERFORMANCE on a device whose clusters could not be told
           apart. Explicitly setting the full mask is not a no-op — it clears a pin left by a
           previous mode — but it never narrows anything. */
        want = &s_affinity_all;
    } else {
        s_affinity_applied_mode_p1 = mode + 1;
        return;
    }

    if (sched_setaffinity(0, sizeof(cpu_set_t), want) != 0) {
        log_warn("Affinity: sched_setaffinity() failed for mode %d; leaving placement to the scheduler",
                 mode);
    } else {
        log_info("Affinity: emulation thread pinned to %d core(s) (mode %d)",
                 CPU_COUNT(want), mode);
    }

    s_affinity_applied_mode_p1 = mode + 1;
}

#else /* !__ANDROID__ */

/* Non-Android hosts: both levers are Android platform APIs with no portable equivalent worth
   faking. sched_setaffinity() does exist on desktop Linux, but "performance cores" there is a
   different question (SMT siblings, P/E cores with their own driver hints) and answering it
   with this file's phone heuristics would be a guess, not a port. */

void armsx_perf_hint_set_adpf_enabled(int enabled) { (void)enabled; }
int armsx_perf_hint_adpf_enabled(void) { return 0; }
void armsx_perf_hint_adpf_set_default(int enabled) { (void)enabled; }
void armsx_perf_hint_frame_begin(double target_fps) { (void)target_fps; }
void armsx_perf_hint_frame_end(void) {}
void armsx_perf_hint_pause(void) {}
void armsx_perf_hint_shutdown(void) {}

void armsx_affinity_set_mode(int mode) { (void)mode; }
int armsx_affinity_mode(void) { return ARMSX_AFFINITY_OFF; }
void armsx_affinity_set_default_mode(int mode) { (void)mode; }
void armsx_affinity_apply_emulation_thread(void) {}

#endif
