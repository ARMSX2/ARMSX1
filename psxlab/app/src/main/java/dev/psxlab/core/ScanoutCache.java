package dev.psxlab.core;

/** Host-only converted pixels. The GPU worker owns the image and display key. */
final class ScanoutCache {
    private Gpu.Frame frame;
    private long revision;
    private final int[] display=new int[7];
    private volatile long conversions,reuses;

    boolean matches(long contentRevision,int[] state){
        if(frame==null||revision!=contentRevision||((display[0]^state[0])&0x00ff0000)!=0)return false;
        for(int i=1;i<7;i++)if(display[i]!=state[i])return false;
        return true;
    }
    void update(long contentRevision,int[] state,Gpu.Frame converted){
        frame=converted;revision=contentRevision;System.arraycopy(state,0,display,0,7);conversions++;
    }
    void reused(){reuses++;}
    Gpu.Frame frame(){return frame;}
    long token(){return conversions;}
    String statistics(){return "converted="+conversions+" reusedPixels="+reuses;}
}
