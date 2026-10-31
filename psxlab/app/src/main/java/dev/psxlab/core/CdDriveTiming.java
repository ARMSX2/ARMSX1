package dev.psxlab.core;

/** Guest drive clocks, separate from host file I/O and the 75/150 Hz sector clock. */
final class CdDriveTiming {
    static final int HZ=33868800,INIT=4000000,ID=33868,TOC=HZ/2;
    private static final int[] MINUTES={0,4,7,11,16,23,27,32,39,44,52,60,67,74,80};
    static int revolution(int lba){int minute=Math.min(80,Math.max(0,lba)/4500);for(int i=0;i<MINUTES.length;i++)if(minute<=MINUTES[i])return 8+i;return 23;}
    static int sector(boolean doubled){return HZ/(doubled?150:75);}
    static int pause(boolean active,boolean doubled,int lba,int remaining){
        if(!active)return 7400;
        int period=sector(doubled),elapsed=Math.max(0,period-Math.min(period,remaining));
        return Math.max(doubled?1000000:2000000,(revolution(lba)-2)*period-elapsed);
    }
    static int seek(int from,int to,boolean doubled){
        long distance=Math.abs((long)to-from);int rotation=revolution(from),period=sector(doubled);
        long clocks;
        if(to>from && distance<=rotation)clocks=Math.max(2,distance)*period;
        else if(to<=from && distance<=rotation)clocks=Math.max(1,rotation-distance)*period;
        else if(distance<7200){double threshold=330-63.1333*Math.log(Math.max(1,Math.min(72,from/4500.0)));clocks=distance<threshold?HZ/20:HZ/10;}
        else {double fraction=distance/(72.0*4500);clocks=(long)(HZ*(0.05+0.85*(0.4*Math.log(distance)/Math.log(72*4500)+0.6*fraction)));}
        // Sub-millisecond phase offset for the addressed track positions.
        int phase=(int)((Integer.toUnsignedLong(from)*31+Integer.toUnsignedLong(to)*17)%16934);
        return (int)Math.min(Integer.MAX_VALUE,clocks+16934+phase);
    }
    private CdDriveTiming(){}
}
