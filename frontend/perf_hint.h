#ifndef ARMSX_PERF_HINT_H
#define ARMSX_PERF_HINT_H

/*
    ARMSX — host CPU scheduling levers: the ADPF clock hint and thread affinity.

    Two knobs that act on the PHONE, not on the emulated console. Neither changes a single
    emulated cycle; both change which cores the emulation thread runs on and how fast the
    kernel is willing to clock them. Both are OFF by default and both are inert everywhere
    except Android — on any other platform every function below compiles to an empty body.

    NEITHER IS A MEASURED WIN. They are levers to be A/B'd on a device; nothing in this file
    may be described as a speed-up until someone has numbers. See the notes on each half.

    ---- 1. ADPF (Android Dynamic Performance Framework) -------------------------------------

    Android's DVFS governor picks CPU frequency by looking at load. Emulation's load is bursty
    — a frame's work, then a sleep until the next vblank — and a governor reading that average
    can settle on a clock below what the burst needs. ADPF lets an app stop guessing: it
    declares "these threads have this much time per frame" and reports how long the work
    actually took, and the scheduler drives frequency from the ratio.

    ★ THE MEASUREMENT IS THE WHOLE FEATURE, AND IT IS EASY TO GET BACKWARDS. What gets reported
    must be the time the emulation thread spent WORKING — never the wall-clock frame time. A
    frame's wall clock includes the limiter sleep, so it sits at the frame period by
    construction: report that and every frame looks like it exactly consumed its budget, which
    is both a constant (no signal) and a false claim of permanent max demand. The sibling PS2
    project shipped that bug once and had to re-do it; see the bracket below.

    The bracket here is:

        armsx_perf_hint_frame_begin()   right after the pacer's sleep returns
            ... psx_update() / texture upload / rasterizer flush ...
        armsx_perf_hint_frame_end()     right after the emulated frame's work is done

    which is the same span the OSD reports as `emu`. The PRESENT is deliberately outside it:
    on this port the emulation thread also posts the frame, and a present can block in the
    buffer queue / on vblank, which is idle time wearing work's clothes. Including it would
    re-introduce a milder version of the same lie. The consequence is that a GPU-bound game
    reports a small work duration and gets no CPU boost — which is correct, because its CPU
    is not what is short.

    API 33 (Android 13) and up. Resolved with dlopen/dlsym, never linked, so an API 26 device
    loads the same .so and simply gets a silent no-op; the app's minSdk does not move.

    ---- 2. Affinity control mode ------------------------------------------------------------

    sched_setaffinity() pins the emulation thread to a subset of cores. On a big.LITTLE SoC the
    kernel's EAS scheduler will usually place a busy thread on a big core by itself, so OFF is
    the honest default and the modes here are for the cases where it does not.

    Cluster membership is DETECTED, never assumed: cpuinfo_max_freq is read for every CPU the
    process is allowed to use and the cores are grouped by that frequency. Hard-coding "cpu4-7
    are big" is wrong on 1+3+4 SoCs, wrong on the 2+6 ones, wrong whenever a core is offline,
    and wrong on every desktop.

    WHICH THREADS: only the emulation thread, which on this port is also the thread that
    presents. Nothing else is pinned, deliberately —
      * SDL's audio thread wakes briefly and often. Forcing it onto the big cluster burns
        power for work that does not need it; forcing it onto the little cluster risks an
        underrun. The scheduler already handles this shape well.
      * the librashader chain builder and the RetroAchievements HTTP workers are one-shot
        background work. Pinning them to the performance cluster puts them in direct
        contention with the emulation thread, which is the opposite of the intent.
*/

#ifdef __cplusplus
extern "C" {
#endif

/* Affinity modes. 0/1/2 are this port's set; 7 is accepted as an alias for 1 because the
   Android front-end inherited its numbering from the PS2 build, whose modes 1..6 ordered the
   EE/VU/GS threads — three threads a PlayStation emulator does not have. Anything else is
   treated as OFF. */
enum {
    ARMSX_AFFINITY_OFF = 0,          /* scheduler decides (default) */
    ARMSX_AFFINITY_PERFORMANCE = 1,  /* pin the emulation thread to the top-frequency cluster */
    ARMSX_AFFINITY_ALL = 2,          /* explicit full mask; undoes a previous pin */
    ARMSX_AFFINITY_PERFORMANCE_ALT = 7
};

/* ---- ADPF ------------------------------------------------------------------------------- */

/* Turn the hint on/off. Safe from any thread (the front-end calls it from the Android UI
   thread); the session itself is only ever created, used and closed on the emulation thread,
   so nothing here needs a lock. Calling this marks the value as host-owned, which is what
   makes armsx_perf_hint_adpf_set_default() below stop applying. */
void armsx_perf_hint_set_adpf_enabled(int enabled);
int armsx_perf_hint_adpf_enabled(void);

/* Boot-time value from settings.toml ([runtime] adpf_clock_hint). LOSES to an explicit
   armsx_perf_hint_set_adpf_enabled() from the host, because the JNI push happens before the
   core parses the file and the file's default would otherwise silently overwrite the user's
   live choice on every boot. */
void armsx_perf_hint_adpf_set_default(int enabled);

/* Emulation thread only. target_fps is the pacer's current target; <= 0 or the uncapped
   sentinel means "no deadline exists", and reporting stops until one does. */
void armsx_perf_hint_frame_begin(double target_fps);
void armsx_perf_hint_frame_end(void);

/* Abandon the in-flight measurement without reporting it. Called whenever the loop did no
   guest work (paused, no session): otherwise the pause lands inside the next frame's bracket
   and reports a multi-second "work" duration. */
void armsx_perf_hint_pause(void);

/* Close the session and forget the registered thread. Called when the emulation loop exits so
   the next run re-registers its own tid. */
void armsx_perf_hint_shutdown(void);

/* ---- Affinity --------------------------------------------------------------------------- */

/* Set from any thread (the front-end pushes it before runVMThread). The mask is only ever
   applied on the emulation thread, from armsx_affinity_apply_emulation_thread(). */
void armsx_affinity_set_mode(int mode);
int armsx_affinity_mode(void);

/* Boot-time value from settings.toml ([runtime] affinity_mode); loses to an explicit host set
   for the same reason as the ADPF default above. */
void armsx_affinity_set_default_mode(int mode);

/* Apply the current mode to the CALLING thread. Cheap enough to call once per frame: it is an
   atomic load and a comparison unless the mode actually changed, which is what gives the
   setting a live apply instead of a boot-only one. */
void armsx_affinity_apply_emulation_thread(void);

#ifdef __cplusplus
}
#endif

#endif
