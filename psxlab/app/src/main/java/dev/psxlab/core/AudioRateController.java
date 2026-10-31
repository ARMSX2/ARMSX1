package dev.psxlab.core;

/** Original output-occupancy and measured source-rate feedback; independent of CPU pacing. */
public final class AudioRateController {
    public static final int TARGET_FRAMES=AudioClockPacer.TARGET_FRAMES;
    private long nextUpdate,lowSince=-1;
    private double fullness=TARGET_FRAMES;
    private float rate=1;
    public float update(long frames,double productionSpeed,long now){
        if(now<nextUpdate)return rate;nextUpdate=now+50000000;
        double available=Math.max(0,frames);
        fullness+=(available-fullness)*0.20;
        double ratio=fullness/TARGET_FRAMES,desired=1;
        // A CPU average can stay at 100% while short stalls drain the output.
        // Correct that deficit from the actual queued PCM, with a quiet band.
        if(ratio<0.90 || ratio>1.10)desired=1+0.30*(ratio-1);
        if(ratio<0.90 && Double.isFinite(productionSpeed))desired=Math.min(desired,Math.max(0.5,productionSpeed));
        if(available<TARGET_FRAMES/2){if(lowSince<0)lowSince=now;}else lowSince=-1;
        boolean urgent=available<441 || lowSince>=0 && now-lowSince>=50000000;
        if(urgent)desired=Math.min(desired,0.65+0.35*available/TARGET_FRAMES);
        desired=Math.max(0.5,Math.min(1.05,desired));
        double step=urgent?0.08:0.01;
        rate=(float)Math.max(0.5,Math.min(1.05,rate+Math.max(-step,Math.min(step,desired-rate))));
        return rate;
    }
    public void recovered(){fullness=TARGET_FRAMES;nextUpdate=0;lowSince=-1;}
}
