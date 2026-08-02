/*
    ARMSX — HOST (device) resource usage, as opposed to the emulated machine's counters.

    Everything else in the stats snapshot measures the PS1: R3000A instructions, GPU primitives,
    SPU voices. This measures the phone. They answer different questions — "is the game doing a
    lot of work" versus "is this device out of headroom" — and a player chasing a slowdown needs
    the second one.

    Sampling is rate-limited internally, so calling this every frame is fine; it re-reads /proc
    at most a few times a second.

    NOTHING HERE IS MODELLED. A figure we cannot read is reported as unavailable (a negative
    value) and rendered as "n/a", never as a plausible-looking guess. GPU busy in particular is
    exposed by vendor sysfs nodes that are usually unreadable to an unprivileged app on modern
    Android, so "n/a" is the expected result on most devices rather than a failure.
*/

#ifndef ARMSX_HOST_USAGE_H
#define ARMSX_HOST_USAGE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* Process CPU as a percentage of the WHOLE DEVICE: 0..100, the same scale as gpu_percent.
       100 means every core saturated.

       This was originally percent-of-one-core (top's convention for a process), which reads as
       "77% / 800%" in the OSD. That is precise and unhelpful: it asks the reader to divide, it
       does not match the GPU figure sitting next to it, and the number it shows is not the one
       being asked ("how loaded is this phone"). Negative if unavailable. */
    double cpu_percent;
    /* Cores the OS reports as online, so the CPU figure above has a ceiling to read against. */
    int    cpu_cores;
    /* Resident set size in MiB — physical memory this process actually occupies. Negative if
       unavailable. */
    double ram_mb;
    /* Device-wide available memory in MiB (MemAvailable), for context on low-RAM handhelds.
       Negative if unavailable. */
    double ram_available_mb;
    /* GPU busy, 0..100. NEGATIVE when no readable counter exists, which is the common case. */
    double gpu_percent;
} armsx_host_usage_t;

/* Fills `out` with the latest sample. Cheap and safe to call per frame. */
void armsx_host_usage_sample(armsx_host_usage_t* out);

#ifdef __cplusplus
}
#endif

#endif /* ARMSX_HOST_USAGE_H */
