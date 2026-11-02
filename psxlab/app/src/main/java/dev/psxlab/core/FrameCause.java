package dev.psxlab.core;

import java.util.Locale;

/** Explains a late frame from its counter deltas against this session's running per-frame averages.
 * The verdict names where the time went; the details name what was unusual about that frame. */
public final class FrameCause {
    public static final int EMULATION_CPU=0,JIT=1,INTERPRETED=2,COMPILED=3,INVALIDATED=4,DEVICE_CALLS=5,PRIMITIVES=6,CD_SECTORS=7,FMV_BLOCKS=8,
        GPU_QUEUE_FULL=9,GPU_DRAW=10,DISPLAY_WAIT=11,DISPLAY_CONVERT=12,DISPLAY_PRESENT=13,DISPLAY_PRESENT_CPU=14,DISPLAYS=15,GC=16,COUNT=17;
    // Below these a spike is noise, however large its ratio to a quiet average.
    private static final long[] FLOOR=new long[COUNT];
    static{FLOOR[JIT]=200_000;FLOOR[INTERPRETED]=50_000;FLOOR[COMPILED]=20;FLOOR[INVALIDATED]=10;FLOOR[DEVICE_CALLS]=20_000;FLOOR[PRIMITIVES]=300;FLOOR[CD_SECTORS]=1;FLOOR[FMV_BLOCKS]=50;FLOOR[GPU_DRAW]=5_000_000;}
    private final double[] average=new double[COUNT];
    private long frames;
    /** Counter differences between two snapshots; EMULATION_CPU is already per frame and is copied. */
    public static long[] delta(long[] now,long[] before){long[] d=new long[COUNT];for(int i=0;i<COUNT;i++)d[i]=i==EMULATION_CPU?now[i]:now[i]-before[i];return d;}
    /** Fold an on-time frame into the averages; late frames are left out so they stay comparable. */
    public void add(long[] d){frames++;double weight=Math.max(1.0/256,1.0/frames);for(int i=0;i<COUNT;i++)average[i]+=(d[i]-average[i])*weight;}
    public String explain(long gap,long emulation,long work,long wait,long[] d){
        long offCpu=Math.max(0,gap-work-wait),other=Math.max(0,work-emulation);
        StringBuilder why=new StringBuilder("why: ");
        if(offCpu>=emulation&&offCpu>=other){
            why.append("Android kept the emulator thread off the CPU for ").append(ms(offCpu));
            if(d[GC]>0)why.append("; ").append(d[GC]).append(" Java garbage collection(s) ran");
            else why.append(" (heat throttling, a busy or small core, or another app)");
        }else if(emulation>=other){
            if(d[GPU_QUEUE_FULL]*3>=emulation)why.append("emulation waited ").append(ms(d[GPU_QUEUE_FULL])).append(" for the GPU thread to catch up on drawing");
            else if(d[EMULATION_CPU]>0&&d[EMULATION_CPU]*10<emulation*6)why.append("emulation got only ").append(ms(d[EMULATION_CPU])).append(" of CPU in ").append(ms(emulation)).append(": the core ran slowly or was shared (heat, small core or another app)");
            else why.append("emulation itself took ").append(ms(emulation)).append(" of CPU work");
        }else{
            why.append("frame submission took ").append(ms(other));
            if(d[GPU_QUEUE_FULL]*2>=other)why.append(", mostly waiting for the GPU thread");
        }
        String unusual=unusual(d);
        why.append(unusual.isEmpty()?"; nothing unusual in the game's work this frame":"; unusual this frame: "+unusual);
        if(frames<120)why.append(" (few frames to compare yet)");
        return why.append(" | display: ").append(display(d)).toString();
    }
    private String unusual(long[] d){
        StringBuilder out=new StringBuilder();
        spike(out,d,INTERPRETED,"interpreted %s instructions (not recompiled code)");
        spike(out,d,COMPILED,"recompiled %s blocks");
        spike(out,d,INVALIDATED,"invalidated %s code blocks (self-modifying code)");
        spike(out,d,JIT,"ran %s recompiled instructions");
        spike(out,d,DEVICE_CALLS,"%s hardware register accesses");
        spike(out,d,PRIMITIVES,"drew %s primitives (heavy effects)");
        spike(out,d,GPU_DRAW,"GPU thread drew for %s");
        if(d[CD_SECTORS]>=FLOOR[CD_SECTORS])append(out,String.format(Locale.ROOT,"read %d disc sectors (loading)",d[CD_SECTORS]));
        spike(out,d,FMV_BLOCKS,"decoded %s FMV blocks (video)");
        return out.toString();
    }
    private void spike(StringBuilder out,long[] d,int index,String format){
        double normal=average[index];
        if(d[index]<FLOOR[index]||d[index]<2*normal)return;
        String value=index==GPU_DRAW?ms(d[index]):String.format(Locale.ROOT,"%,d",d[index]);
        append(out,String.format(Locale.ROOT,format,value)+(normal>=1?String.format(Locale.ROOT," = %.1fx normal",d[index]/normal):""));
    }
    private static void append(StringBuilder out,String text){if(out.length()>0)out.append(", ");out.append(text);}
    private static String display(long[] d){
        if(d[DISPLAYS]==0)return "no frame finished presenting (GPU thread busy or display backed up)";
        return String.format(Locale.ROOT,"%d shown, waited %s behind drawing, picture copy %s, present %s (CPU %s)",d[DISPLAYS],ms(d[DISPLAY_WAIT]),ms(d[DISPLAY_CONVERT]),ms(d[DISPLAY_PRESENT]),ms(d[DISPLAY_PRESENT_CPU]));
    }
    /** A slow presentation seen on the GPU thread: {total, wait behind drawing, picture copy, present, present CPU}. */
    public static String slowPresent(long[] p){
        long total=p[0],wait=p[1],convert=p[2],present=p[3],cpu=p[4];
        String verdict;
        if(wait>=convert&&wait>=present)verdict="the GPU thread was still drawing earlier commands (heavy effects or a drawing backlog)";
        else if(convert>=present)verdict="copying the picture out of video memory was slow";
        else if(cpu*2<present)verdict="the GPU or screen was slow to accept the frame (GPU overloaded or throttled, or the display backed up)";
        else verdict="the presentation code itself used the CPU time";
        return String.format(Locale.ROOT,"slow display frame %s: waited %s behind drawing, picture copy %s, present %s (CPU %s); why: %s",ms(total),ms(wait),ms(convert),ms(present),ms(cpu),verdict);
    }
    private static String ms(long nanos){return String.format(Locale.ROOT,"%.1f ms",nanos/1e6);}
}
