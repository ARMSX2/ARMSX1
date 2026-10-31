package dev.psxlab.core;

/** Completed-field frequency, measured on the completion thread's own time boundaries. */
public final class FrameRateMeter {
    private static final long WINDOW_NANOS=500_000_000L;
    private boolean started;
    private long firstNanos;
    private int intervals;
    public synchronized void reset(){started=false;intervals=0;}
    /** NaN means no new sample yet. No nominal-rate clamp or speed adjustment is applied. */
    public synchronized float completed(long now){
        if(!started||now<firstNanos){started=true;firstNanos=now;intervals=0;return Float.NaN;}
        intervals++;long elapsed=now-firstNanos;
        if(elapsed<WINDOW_NANOS)return Float.NaN;
        float rate=(float)(intervals*1e9/elapsed);firstNanos=now;intervals=0;return rate;
    }
}
