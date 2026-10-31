package dev.psxlab.core;

import java.util.ArrayDeque;

/** Maps output sample positions back to the guest PCM timeline, including tempo changes.
 * SoundTouch's overlap/search can move a local waveform within its processing window.
 */
public final class AudioTimeline {
    private record Segment(double output,long source,float tempo){}
    private final ArrayDeque<Segment> segments=new ArrayDeque<>();
    private double output;
    private long source;
    public synchronized void feed(int frames,float tempo){
        if(frames<0 || !Float.isFinite(tempo) || tempo<=0)throw new IllegalArgumentException("Audio timeline input");
        if(frames==0)return;
        if(segments.isEmpty() || segments.peekLast().tempo()!=tempo)segments.addLast(new Segment(output,source,tempo));
        source+=frames;output+=frames/(double)tempo;
    }
    public synchronized long sourceAt(long played){
        if(segments.isEmpty() || played<0)return 0;
        while(segments.size()>1){Segment first=segments.removeFirst();if(segments.peekFirst().output()>played){segments.addFirst(first);break;}}
        Segment segment=segments.peekFirst();
        return Math.max(0,Math.min(source,segment.source()+Math.round((played-segment.output())*segment.tempo())));
    }
}
