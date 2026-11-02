package dev.psxlab.core;

/** Follow sustainable guest PCM production without treating queue backpressure as slow execution. */
public final class ProductionAudioTempo {
    private long at,frames,blocked;
    private float tempo=1,ceiling=1;
    public void reset(long now,long sourceFrames,long blockedNanos,float requested){
        at=now;frames=sourceFrames;blocked=blockedNanos;ceiling=Float.isFinite(requested)?Math.max(1,Math.min(16,requested)):1;tempo=ceiling;
    }
    public float update(long now,long sourceFrames,long blockedNanos){
        long elapsed=now-at;if(elapsed<50_000_000L)return tempo;
        long produced=Math.max(0,sourceFrames-frames),waiting=Math.max(0,blockedNanos-blocked);
        long available=Math.max(elapsed/16,elapsed-Math.min(elapsed,waiting));
        double capacity=produced*1e9/(44100d*available);
        float target=capacity>=ceiling*.97?ceiling:(float)Math.max(.5,Math.min(ceiling,capacity*.98));
        tempo=target<tempo?target:tempo+(target-tempo)*.25f;
        at=now;frames=sourceFrames;blocked=blockedNanos;return tempo;
    }
}
