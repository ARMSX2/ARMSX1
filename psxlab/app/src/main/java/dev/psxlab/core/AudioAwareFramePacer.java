package dev.psxlab.core;

/** Keep video deadlines stable; temporarily rebuild audio lead after starvation.
 * Output tempo never directly sets CPU speed. */
public final class AudioAwareFramePacer {
    private final FramePacer clock;
    private boolean refilling=true;
    public AudioAwareFramePacer(long cycles,long now){clock=new FramePacer(cycles,now);}
    public long waitNanos(long cycles,long now,long audioFrames){
        if(audioFrames<2048)refilling=true;
        if(refilling){clock.reset(cycles,now);if(audioFrames>=4096)refilling=false;return 0;}
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
