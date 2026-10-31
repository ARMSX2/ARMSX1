package dev.psxlab.core;

/** Bounded stereo PCM history for an explicitly enabled audio diagnosis. */
public final class PcmWindow {
    public record Snapshot(short[] samples,long firstFrame,long endFrame){}
    private final short[] ring;
    private long samples;
    public PcmWindow(int frames){if(frames<1)throw new IllegalArgumentException("PCM history capacity");ring=new short[Math.multiplyExact(frames,2)];}
    public synchronized void append(short[] pcm,int count){
        if(count<0 || count>pcm.length || (count&1)!=0)throw new IllegalArgumentException("Stereo PCM length");
        int from=0;if(count>ring.length){from=count-ring.length;samples+=from;}
        while(from<count){int at=(int)(samples%ring.length),n=Math.min(count-from,ring.length-at);System.arraycopy(pcm,from,ring,at,n);samples+=n;from+=n;}
    }
    public synchronized Snapshot snapshot(){
        int count=(int)Math.min(samples,ring.length),at=(int)((samples-count)%ring.length),first=Math.min(count,ring.length-at);short[] out=new short[count];
        System.arraycopy(ring,at,out,0,first);if(first<count)System.arraycopy(ring,0,out,first,count-first);
        return new Snapshot(out,(samples-count)/2,samples/2);
    }
}
