package dev.psxlab.core;

/** Accelerate guest time independently of the number of pictures the display can accept. */
public final class FastForwardPacer {
    private static final long CLOCK=33868800L;
    private long epochClocks,epochNanos,nextPresentation;
    private final java.util.concurrent.atomic.AtomicBoolean presentationPending=new java.util.concurrent.atomic.AtomicBoolean();
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
        if(!presentationPending.compareAndSet(false,true))return false;
        nextPresentation=nextPresentation==0||now-nextPresentation>=period?now+period:nextPresentation+period;
        return true;
    }
    /** Called by the GPU worker after this host presentation completes. */
    public void presentationComplete(){presentationPending.set(false);}
}
