package dev.psxlab.core;

/** Accelerate guest time independently of the number of pictures the display can accept. */
public final class FastForwardPacer {
    private static final long CLOCK=33868800L;
    /**
     * Normal play keeps up to two host presents in flight. With one, a present that finished a few
     * milliseconds after the next guest field (FIFO vsync on a 120 Hz panel, a fence wait) discarded
     * that field outright, a visible hitch about every two seconds. Two absorbs that jitter while still
     * bounding the queue, and so the latency, behind a display that has really stalled.
     */
    static final int NORMAL_IN_FLIGHT=2;
    private long epochClocks,epochNanos,nextPresentation;
    private final java.util.concurrent.atomic.AtomicInteger inFlight=new java.util.concurrent.atomic.AtomicInteger();
    private float speed=-1;
    public void reset(){speed=-1;nextPresentation=0;}
    /** Zero speed is unlimited. Reanchor after pauses, state loads or speed changes. */
    public long waitNanos(long clocks,long now,float requested){
        float value=Float.isFinite(requested)?Math.max(0,Math.min(16,requested)):2;
        if(value!=speed||clocks<epochClocks){speed=value;epochClocks=clocks;epochNanos=now;return 0;}
        if(value==0)return 0;
        long elapsed=clocks-epochClocks;
        double guestNanos=(elapsed/CLOCK)*1e9+(elapsed%CLOCK)*1e9/CLOCK;
        long target=epochNanos+(long)(guestNanos/value);
        if(now-target>40_000_000L){epochClocks=clocks;epochNanos=now;return 0;}
        return Math.max(0,target-now);
    }
    /** Keep guest raster/device work; only omit excess host scanouts during fast-forward. */
    public boolean present(long now,float refreshHz){
        // A panel's advertised refresh can exceed the rate granted to this window.
        // One outstanding host present prevents FIFO waits from accumulating.
        double hz=Float.isFinite(refreshHz)&&refreshHz>=20?Math.min(60,refreshHz):60;
        long period=(long)(1e9/hz);
        if(nextPresentation!=0&&now<nextPresentation)return false;
        if(!acquire(1))return false;
        nextPresentation=nextPresentation==0||now-nextPresentation>=period?now+period:nextPresentation+period;
        return true;
    }
    /** Called by the GPU worker after this host presentation completes. */
    public void presentationComplete(){inFlight.updateAndGet(n->Math.max(0,n-1));}
    /** Normal guest clocks continue even when a slow window still owns the last presents. */
    public boolean presentNormally(){return acquire(NORMAL_IN_FLIGHT);}
    private boolean acquire(int limit){
        for(;;){int n=inFlight.get();if(n>=limit)return false;if(inFlight.compareAndSet(n,n+1))return true;}
    }
}
