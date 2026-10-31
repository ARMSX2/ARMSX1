package dev.psxlab.core;

import java.util.Arrays;

/** Small opt-in OSD window. Never enables the disk logger or waits for the GPU. */
public final class PerformanceStatistics {
    public static final int COUNT=33;
    private volatile boolean enabled;
    private volatile long generation=1;
    private double[] latest;
    private long started,previous,frames,cpu,idle,present,worst;
    private Sample first,last;
    public record Sample(long guestClocks,long triangles,long rectangles,long lines,long mdecBlocks,long cdSectors,
                         long[] dmaWords,int voices,int width,int height,int scale) {}
    public boolean enabled(){return enabled;}
    public long ticket(){return enabled?generation:0;}
    public synchronized void enabled(boolean value){if(enabled==value)return;enabled=value;reset();}
    public synchronized void reset(){generation++;latest=null;started=previous=frames=cpu=idle=present=worst=0;first=last=null;}
    public synchronized void completed(long ticket,long now,long cpuNanos,long idleNanos,long presentNanos,Sample sample){
        if(ticket==0||!enabled||ticket!=generation)return;
        if(started==0){started=previous=now;first=last=sample;return;}
        long interval=Math.max(0,now-previous);previous=now;worst=Math.max(worst,interval);
        frames++;cpu+=Math.max(0,cpuNanos);idle+=Math.max(0,idleNanos);present+=Math.max(0,presentNanos);last=sample;
        if(now-started<500_000_000L)return;
        double[] out=new double[COUNT];Arrays.fill(out,-1);
        out[0]=(now-started)/1e6/frames;out[1]=cpu/1e6/frames;out[2]=present/1e6/frames;out[3]=idle/1e6/frames;out[4]=worst/1e6;
        out[6]=delta(sample.guestClocks,first.guestClocks,frames);
        out[8]=delta(sample.triangles,first.triangles,frames);out[9]=delta(sample.rectangles,first.rectangles,frames);out[10]=delta(sample.lines,first.lines,frames);
        out[13]=sample.voices;out[15]=delta(sample.mdecBlocks,first.mdecBlocks,frames);out[16]=delta(sample.cdSectors,first.cdSectors,frames);
        double total=0;for(int ch=0;ch<7;ch++)total+=delta(sample.dmaWords[ch],first.dmaWords[ch],frames);
        out[17]=total;out[18]=dma(sample,2);out[19]=dma(sample,4);out[20]=dma(sample,0)+dma(sample,1);out[21]=dma(sample,3);out[22]=dma(sample,6);
        out[23]=sample.width;out[24]=sample.height;out[30]=sample.scale;out[31]=sample.width*sample.scale;out[32]=sample.height*sample.scale;
        latest=out;started=now;frames=cpu=idle=present=worst=0;first=last;
    }
    private double dma(Sample sample,int channel){return delta(sample.dmaWords[channel],first.dmaWords[channel],frames);}
    private static double delta(long end,long start,long frames){return end<start?-1:(end-start)/(double)frames;}
    /** Whole-device process CPU percentage; GPU busy remains unavailable without a driver reading. */
    public synchronized int copy(double[] out,double cpuPercent,int cores,double rssMb,double availableMb){
        if(out==null||!enabled||latest==null)return 0;
        int count=Math.min(out.length,COUNT);System.arraycopy(latest,0,out,0,count);
        if(count>25)out[25]=cpuPercent;if(count>26)out[26]=cores;if(count>27)out[27]=rssMb;if(count>28)out[28]=availableMb;
        return count;
    }
}
