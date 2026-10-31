package dev.psxlab.core;

/** Original monotonic guest-clock deadline, with bounded recovery after host stalls. */
public final class FramePacer {
    private static final long CLOCK=33868800L;
    private long epochNanos,epochClocks;
    public FramePacer(long clocks,long now){reset(clocks,now);}
    public void reset(long clocks,long now){epochClocks=clocks;epochNanos=now;}
    public long deadline(long clocks,long now){
        long elapsed=clocks-epochClocks;
        if(elapsed<0){reset(clocks,now);return now;}
        // Split the conversion so sessions longer than 272 seconds cannot overflow.
        long target=epochNanos+(elapsed/CLOCK)*1000000000L+(elapsed%CLOCK)*1000000000L/CLOCK;
        if(now-target>40000000L){reset(clocks,now);return now;}
        return target;
    }
}
