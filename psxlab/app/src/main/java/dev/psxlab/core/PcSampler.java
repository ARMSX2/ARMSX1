package dev.psxlab.core;

import java.util.Arrays;
import java.util.Locale;

/** Where guest code was each time an emulation slice ended. A game stuck waiting for hardware spends
 * nearly every sample inside one small loop, while normal play spreads across many addresses. */
public final class PcSampler {
    /** The exception vector, where vblank interrupts land; it says nothing about what the game waits for. */
    static final int EXCEPTION_VECTOR=0x80000080;
    /** Wait loops are a few instructions; 64 bytes also covers a short polled function. */
    static final int WINDOW_BITS=6;
    private final int[] pcs=new int[4096];
    private int count;
    public void add(int pc){if(count<pcs.length)pcs[count++]=pc;}
    public void clear(){count=0;}
    public int samples(){return count;}
    /** The most frequent addresses with their share of samples, e.g. "8002972C 41%, 80000080 30%". */
    public String top(int limit){
        if(count==0)return "none";
        int[] sorted=Arrays.copyOf(pcs,count);Arrays.sort(sorted);
        long[] runs=new long[count];int n=0;
        for(int i=0;i<count;){int j=i;while(j<count&&sorted[j]==sorted[i])j++;runs[n++]=((long)(j-i)<<32)|(sorted[i]&0xffffffffL);i=j;}
        Arrays.sort(runs,0,n);
        StringBuilder out=new StringBuilder();
        for(int k=n-1;k>=0&&k>=n-limit;k--){if(out.length()>0)out.append(", ");out.append(String.format(Locale.ROOT,"%08X %d%%",(int)runs[k],(runs[k]>>>32)*100/count));}
        return out.toString();
    }
    /** The busiest 64-byte window outside the exception vector: base address in the high half, percent of those samples in the low half; 0 when there are too few samples. */
    public long hotWindow(){
        int[] windows=new int[count];int n=0;
        for(int i=0;i<count;i++)if(pcs[i]!=EXCEPTION_VECTOR)windows[n++]=pcs[i]>>>WINDOW_BITS;
        if(n<32)return 0;
        Arrays.sort(windows,0,n);int best=0,bestWindow=0;
        for(int i=0;i<n;){int j=i;while(j<n&&windows[j]==windows[i])j++;if(j-i>best){best=j-i;bestWindow=windows[i];}i=j;}
        return ((long)(bestWindow<<WINDOW_BITS)<<32)|(best*100L/n);
    }
}
