package dev.psxlab.core;

/** Estimate uncapped source capacity without mistaking audio backpressure for a slow machine. */
public final class UnlimitedAudioTempo {
    private long at,frames,blocked;
    private float tempo=2;
    public void reset(long now,long sourceFrames,long blockedNanos){at=now;frames=sourceFrames;blocked=blockedNanos;tempo=2;}
    public float update(long now,long sourceFrames,long blockedNanos){
        long elapsed=now-at;if(elapsed<50_000_000L)return tempo;
        long produced=sourceFrames-frames,waiting=Math.max(0,blockedNanos-blocked);
        long available=Math.max(elapsed/16,elapsed-Math.min(elapsed,waiting));
        double capacity=produced*1e9/(44100d*available);
        float target=(float)Math.max(1,Math.min(16,capacity*.92));
        // Leave processing headroom; approach a higher capacity gradually, but react
        // immediately to a slowdown before the playback reserve drains.
        tempo=target<tempo?target:tempo+(target-tempo)*.25f;
        at=now;frames=sourceFrames;blocked=blockedNanos;return tempo;
    }
}
