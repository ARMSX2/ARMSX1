package dev.psxlab.core;

/** Regulate source lead against consumed audio, rather than an independent wall clock. */
public final class AudioClockPacer {
    public static final int TARGET_FRAMES=4096;
    public static long waitNanos(long bufferedFrames){
        long excess=bufferedFrames-TARGET_FRAMES;
        if(excess<=128)return 0;
        return excess>=177?4000000L:excess*1000000000L/44100;
    }
}
