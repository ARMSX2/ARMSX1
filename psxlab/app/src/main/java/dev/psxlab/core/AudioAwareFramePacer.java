package dev.psxlab.core;

/** Keep video deadlines stable; temporarily rebuild audio lead after starvation.
 * Output tempo never directly sets CPU speed. */
public final class AudioAwareFramePacer {
    /** Refill pace: fast enough to rebuild the lead in about 0.1 s, without the burst of fast
     * motion (and CPU spike) an unpaced refill caused on fast phones after every pause. */
    static final long REFILL_SPEED=2;
    private final FramePacer clock,refill;
    private long refillStarted;
    private boolean refilling=true;
    public AudioAwareFramePacer(long cycles,long now){clock=new FramePacer(cycles,now);refill=new FramePacer(cycles,now);refillStarted=now;}
    public long waitNanos(long cycles,long now,long audioFrames){
        if(audioFrames<2048&&!refilling){refilling=true;refill.reset(cycles,now);refillStarted=now;}
        if(refilling){
            clock.reset(cycles,now);if(audioFrames>=4096){refilling=false;return 0;}
            // A late refill re-anchors itself; never wait to make up for that.
            long target=refill.deadline(cycles,now);if(target==now){refillStarted=now;return 0;}
            return Math.max(0,refillStarted+(target-refillStarted)/REFILL_SPEED-now);
        }
        // Android's output warm-up can add a permanent lead after startup.
        // Leave normal 40 ms DSP bursts alone, but let excessive queued audio
        // catch up instead of preserving that initial speech delay forever.
        if(audioFrames>6144){
            long wait=Math.min(20000000L,(audioFrames-4096)*1000000000L/44100);
            clock.reset(cycles,now+wait);return wait;
        }
        return Math.max(0,clock.deadline(cycles,now)-now);
    }
}
