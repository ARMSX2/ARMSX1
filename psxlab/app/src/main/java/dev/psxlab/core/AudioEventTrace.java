package dev.psxlab.core;

import java.util.ArrayDeque;

/** Bounded diagnostic events. Times and frame positions stay absolute when the ring wraps. */
public final class AudioEventTrace {
    public static final String HEADER="sequence\tmonotonic_ns\tguest_clocks\tspu_frames\tsubmitted_source_frames\tvblank\tevent\n";
    private final int capacity;
    private final ArrayDeque<String> rows=new ArrayDeque<>();
    private long sequence;
    private java.util.function.Consumer<String> sink;
    public synchronized void setSink(java.util.function.Consumer<String> sink){this.sink=sink;}
    public AudioEventTrace(int capacity){if(capacity<1)throw new IllegalArgumentException("Trace capacity");this.capacity=capacity;}
    public synchronized void record(long monotonicNanos,long guestClocks,long spuFrames,long submittedFrames,long vblank,String event){
        if(rows.size()==capacity)rows.removeFirst();
        String row=(++sequence)+"\t"+monotonicNanos+"\t"+guestClocks+"\t"+spuFrames+"\t"+submittedFrames+"\t"+vblank+"\t"+event.replace('\t',' ').replace('\n',' ').replace('\r',' ');rows.addLast(row);if(sink!=null)sink.accept(row);
    }
    public synchronized String snapshot(){return "# total_events="+sequence+" retained="+rows.size()+" discarded="+(sequence-rows.size())+"\n"+HEADER+String.join("\n",rows)+"\n";}
}
