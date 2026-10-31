package dev.psxlab.core;

import java.util.Arrays;

/** Bounded startup trace. Recording never formats strings or writes to disk. */
public final class StartupTiming {
    private final long[] clocks,requested,ready;
    private int count;
    public StartupTiming(int capacity){if(capacity<1)throw new IllegalArgumentException("Trace capacity");clocks=new long[capacity];requested=new long[capacity];ready=new long[capacity];}
    public synchronized void reset(){count=0;}
    public synchronized void record(long guestClocks,long requestNanos,long readyNanos){
        if(count==clocks.length)return;clocks[count]=guestClocks;requested[count]=requestNanos;ready[count]=readyNanos;count++;
    }
    public synchronized int size(){return count;}
    public String tsv(){
        long[] c,q,r;
        synchronized(this){c=Arrays.copyOf(clocks,count);q=Arrays.copyOf(requested,count);r=Arrays.copyOf(ready,count);}
        StringBuilder text=new StringBuilder("guest_clocks\trequest_ns\tgpu_ready_ns\n");
        for(int i=0;i<c.length;i++)text.append(c[i]).append('\t').append(q[i]).append('\t').append(r[i]).append('\n');
        return text.toString();
    }
}
