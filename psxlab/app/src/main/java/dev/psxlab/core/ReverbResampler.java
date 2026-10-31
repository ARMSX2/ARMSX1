package dev.psxlab.core;

/** PS1 reverb's documented 39-tap half-band FIR, applied before and after 22.05 kHz work. */
final class ReverbResampler {
    // The omitted odd taps are zero, except the center tap (0x4000).
    private static final int[] TAPS={-1,2,-10,35,-103,266,-616,1332,-2960,10246,10246,-2960,1332,-616,266,-103,35,-10,2,-1};
    private final int[][] input=new int[2][64],output=new int[2][64];
    private int position;
    void input(int left,int right){input[0][position]=left;input[1][position]=right;}
    int down(int channel){return filter(input[channel],15);}
    void output(int left,int right){output[0][position]=left;output[1][position]=right;}
    int up(int channel){return filter(output[channel],14);}
    void advance(){position=(position+1)&63;}
    private int filter(int[] history,int shift){
        long sum=(long)history[(position-19)&63]*16384;
        for(int i=0;i<TAPS.length;i++)sum+=(long)history[(position-i*2)&63]*TAPS[i];
        return Spu.clamp((int)(sum>>shift));
    }
}
