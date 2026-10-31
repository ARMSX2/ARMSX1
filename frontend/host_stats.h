#ifndef ARMSX_HOST_STATS_H
#define ARMSX_HOST_STATS_H

/*
    Layout of the performance snapshot psxe_host_stats() hands an embedded host.

    Written by ArmsxApp::publishFrameStats() in frontend/main.cpp, read by
    Java_kr_co_iefriends_pcsx2_NativeApp_getStatistics() in frontend/android_jni.cpp and
    rendered by com.armsx2.ui.GameOsd. One flat double[] rather than a struct because it has
    to cross JNI: the Kotlin side keeps a single array alive and refills it, so a 2 Hz poll
    allocates nothing.

    ⚠ APPEND ONLY. Kotlin indexes this by ordinal (GameOsd.Stat), so inserting in the middle
    silently relabels every row after it. New stats go before PSXE_HOST_STAT_COUNT.

    Every value is a real measurement: the guest-side entries are exact event counts taken at
    the site that does the work (psx/perf.h), the host-side ones are wall-clock times around
    whole frame phases. Nothing here is modelled or extrapolated. Values are per FRAME —
    averaged over the publishing window, not instantaneous — except the *_WORST entries.
*/

enum {
    /* Host wall clock, milliseconds. IDLE is what was left over after emulating and
       presenting, i.e. the headroom the frame limiter slept away; a device that cannot keep
       up reports 0 here and the sum of the other two exceeds the frame budget. */
    PSXE_HOST_STAT_FRAME_MS = 0,
    PSXE_HOST_STAT_EMU_MS,
    PSXE_HOST_STAT_PRESENT_MS,
    PSXE_HOST_STAT_IDLE_MS,
    PSXE_HOST_STAT_WORST_FRAME_MS,

    /* R3000A. Instructions is the interpreter's step count for the frame; cycles is the
       guest clocks those instructions charged, so CYCLES/INSTRUCTIONS is a real CPI. */
    PSXE_HOST_STAT_CPU_INSTRUCTIONS,
    PSXE_HOST_STAT_CPU_CYCLES,

    /* Geometry coprocessor operations executed. */
    PSXE_HOST_STAT_GTE_OPS,

    /* Software rasteriser. PIXELS is the exact inner-loop iteration count of every
       primitive drawn — for this renderer that IS its cost. VRAM_WORDS counts halfwords
       moved by the upload/download/copy/fill commands. */
    PSXE_HOST_STAT_GPU_TRIANGLES,
    PSXE_HOST_STAT_GPU_RECTS,
    PSXE_HOST_STAT_GPU_LINES,
    PSXE_HOST_STAT_GPU_PIXELS,
    PSXE_HOST_STAT_GPU_VRAM_WORDS,

    /* Mean number of SPU voices that were actually sounding (0..24). */
    PSXE_HOST_STAT_SPU_VOICES,

    /* MDEC: macroblocks, and the 8x8 blocks run through the IDCT inside them. Both sit at
       0 outside an FMV. */
    PSXE_HOST_STAT_MDEC_MACROBLOCKS,
    PSXE_HOST_STAT_MDEC_BLOCKS,

    /* Sectors pulled off the disc image. */
    PSXE_HOST_STAT_CDROM_SECTORS,

    /* DMA words moved, in total and split by the channels that carry real traffic. */
    PSXE_HOST_STAT_DMA_WORDS,
    PSXE_HOST_STAT_DMA_GPU_WORDS,
    PSXE_HOST_STAT_DMA_SPU_WORDS,
    PSXE_HOST_STAT_DMA_MDEC_WORDS,
    PSXE_HOST_STAT_DMA_CDROM_WORDS,
    PSXE_HOST_STAT_DMA_OTC_WORDS,

    /* Emulated output resolution, in guest pixels. */
    PSXE_HOST_STAT_WIDTH,
    PSXE_HOST_STAT_HEIGHT,

    /* HOST (device) resource usage — the phone, not the emulated machine. Appended, per the
       append-only rule above. A negative value means UNAVAILABLE and must render as "n/a":
       GPU busy in particular comes from vendor sysfs nodes that are root-only on most retail
       Android, so unavailable is the normal outcome there, not an error.

       CPU is a percentage of ONE core (top's convention for a process), so it can exceed 100;
       HOST_CPU_CORES gives it a ceiling to be read against. */
    PSXE_HOST_STAT_HOST_CPU_PERCENT,
    PSXE_HOST_STAT_HOST_CPU_CORES,
    PSXE_HOST_STAT_HOST_RAM_MB,
    PSXE_HOST_STAT_HOST_RAM_AVAILABLE_MB,
    PSXE_HOST_STAT_HOST_GPU_PERCENT,

    /* Internal-resolution scale the rasterizer is actually running at (1 = native).
       WIDTH/HEIGHT above are the EMULATED display size and do not change when upscaling — the
       PlayStation still thinks it is drawing 512x240 — so without this the OSD reported native
       resolution at every scale and there was no way to confirm the setting had taken. */
    PSXE_HOST_STAT_INTERNAL_SCALE,

    PSXE_HOST_STAT_COUNT
};

#endif
