package dev.psxlab.core;

import java.util.Arrays;

/** Original XA sound-unit decoder and documented 7:6 polyphase resampler.
 * ROM coefficients: PSX-SPX CDROM Format / 25-point Zigzag Interpolation.
 * 18.9 kHz uses sample duplication before the 37.8 kHz filter; emphasis is not modeled. */
final class XaDecoder {
    interface Sink{void frame(int left,int right);}
    private static final int[] POS={0,60,115,98},NEG={0,0,-52,-55};
    // Rows are successive historical taps; columns are the seven output phases.
    private static final int[][] ROM={
        {0,0,0,0,-1,2,-5},{0,0,0,-1,3,-8,17},{0,0,-1,3,-8,16,-35},
        {0,-2,3,-8,17,-35,70},{0,0,-2,6,-16,43,-23},{-2,3,-5,5,10,26,-68},
        {10,-19,31,-27,107,-235,347},{-34,60,-74,166,-365,635,-839},
        {65,-75,179,-424,848,-1352,2062},{-84,162,-402,882,-1571,2810,-4681},
        {52,-227,689,-1471,3021,-5882,15367},{9,306,-926,2488,-6016,21472,21472},
        {-266,-67,1272,-4532,26516,15367,-5882},{1024,-615,-1446,29883,9036,-4681,2810},
        {-2680,3229,31033,3229,-2680,2062,-1352},{9036,29883,-1446,-615,1024,-839,635},
        {26516,-4532,1272,-67,-266,347,-235},{-6016,2488,-926,306,9,-68,26},
        {3021,-1471,689,-227,52,-23,43},{-1571,882,-402,162,-84,70,-35},
        {848,-424,179,-75,65,-35,16},{-365,166,-74,60,-34,17,-8},
        {107,-27,31,-19,10,-5,2},{10,5,-5,3,-1,0,0},{-16,6,-2,0,0,0,0},
        {17,-8,3,-2,1,0,0},{-8,3,-1,0,0,0,0},{3,-1,0,0,0,0,0},{-1,0,0,0,0,0,0}
    };
    private final int[] old=new int[2],older=new int[2];
    private final int[][] decoded=new int[2][4032],history=new int[2][32];
    private final int[] counts=new int[2];
    private int position,sixstep;
    void reset(){Arrays.fill(old,0);Arrays.fill(older,0);for(int[] h:history)Arrays.fill(h,0);position=sixstep=0;}
    /** Returns decoded source frames per channel; history persists between sectors. */
    int decodeSource(byte[] raw){
        if(raw.length!=2352)throw new IllegalArgumentException("XA needs a raw 2352-byte sector");
        int coding=raw[19]&255;boolean stereo=(coding&1)!=0,eight=(coding&16)!=0;
        counts[0]=counts[1]=0;
        for(int group=0;group<18;group++){
            int base=24+group*128,units=eight?4:8;
            for(int unit=0;unit<units;unit++){
                int channel=stereo?unit&1:0,header=raw[base+4+unit]&255,shift=header&15,filter=(header>>>4)&3;
                if(shift>12)shift=9;
                for(int i=0;i<28;i++){
                    int packed=raw[base+16+i*4+(eight?unit:unit/2)]&255;
                    int sample=eight?((short)(packed<<8))>>shift:((short)(((packed>>>((unit&1)*4))&15)<<12))>>shift;
                    sample=Spu.clamp(sample+((old[channel]*POS[filter]+older[channel]*NEG[filter]+32)>>6));
                    older[channel]=old[channel];old[channel]=sample;decoded[channel][counts[channel]++]=sample;
                }
            }
        }
        if(!stereo)System.arraycopy(decoded[0],0,decoded[1],0,counts[0]);
        return counts[0];
    }
    int sourceSample(int channel,int index){return decoded[channel][index];}
    int decode(byte[] raw,Sink sink){
        int frames=decodeSource(raw),outputs=0;boolean half=(raw[19]&4)!=0;
        for(int i=0;i<frames;i++){
            outputs+=resample(decoded[0][i],decoded[1][i],sink);
            if(half)outputs+=resample(decoded[0][i],decoded[1][i],sink);
        }
        return outputs;
    }
    private int resample(int left,int right,Sink sink){
        history[0][position]=left;history[1][position]=right;position=(position+1)&31;
        if(++sixstep<6)return 0;sixstep=0;
        for(int phase=0;phase<7;phase++){
            long l=0,r=0;for(int tap=0;tap<29;tap++){int index=(position-1-tap)&31,weight=ROM[tap][phase];l+=(long)history[0][index]*weight;r+=(long)history[1][index]*weight;}
            sink.frame(Spu.clamp((int)(l>>15)),Spu.clamp((int)(r>>15)));
        }
        return 7;
    }
}
