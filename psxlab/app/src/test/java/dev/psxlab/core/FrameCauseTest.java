package dev.psxlab.core;

import static org.junit.Assert.*;
import org.junit.Test;

public class FrameCauseTest {
    private static final long MS=1_000_000L;
    private static long[] frame(){long[] d=new long[FrameCause.COUNT];d[FrameCause.INTERPRETED]=1_000;d[FrameCause.PRIMITIVES]=280;d[FrameCause.DISPLAYS]=1;d[FrameCause.DISPLAY_PRESENT]=4*MS;d[FrameCause.DISPLAY_PRESENT_CPU]=MS;return d;}
    private static FrameCause warmed(){FrameCause c=new FrameCause();for(int i=0;i<300;i++)c.add(frame());return c;}
    @Test public void offCpuTimeIsBlamedOnAndroidScheduling(){
        long[] d=frame();d[FrameCause.EMULATION_CPU]=14*MS;
        String why=warmed().explain(120*MS,15*MS,20*MS,0,d);
        assertTrue(why,why.contains("off the CPU for 100.0 ms")&&why.contains("heat throttling"));
    }
    @Test public void garbageCollectionIsNamedWhenItRan(){
        long[] d=frame();d[FrameCause.GC]=1;
        String why=warmed().explain(120*MS,15*MS,20*MS,0,d);
        assertTrue(why,why.contains("1 Java garbage collection"));
    }
    @Test public void fullGpuQueueIsBlamedOnDrawingBacklog(){
        long[] d=frame();d[FrameCause.EMULATION_CPU]=40*MS;d[FrameCause.GPU_QUEUE_FULL]=60*MS;
        String why=warmed().explain(110*MS,100*MS,105*MS,0,d);
        assertTrue(why,why.contains("waited 60.0 ms for the GPU thread"));
    }
    @Test public void videoMemoryReadbackWaitsAreNamed(){
        long[] d=frame();d[FrameCause.EMULATION_CPU]=40*MS;d[FrameCause.READBACK_WAIT]=50*MS;d[FrameCause.READBACKS]=2;
        String why=warmed().explain(110*MS,100*MS,105*MS,0,d);
        assertTrue(why,why.contains("waited 50.0 ms for the GPU to finish drawing before reading video memory back (2 read(s))"));
    }
    @Test public void starvedEmulationIsBlamedOnTheCore(){
        long[] d=frame();d[FrameCause.EMULATION_CPU]=30*MS;
        String why=warmed().explain(110*MS,100*MS,105*MS,0,d);
        assertTrue(why,why.contains("got only 30.0 ms of CPU in 100.0 ms"));
    }
    @Test public void spikesAreComparedWithNormalFrames(){
        long[] d=frame();d[FrameCause.EMULATION_CPU]=95*MS;d[FrameCause.INTERPRETED]=500_000;d[FrameCause.PRIMITIVES]=2_800;d[FrameCause.CD_SECTORS]=3;
        String why=warmed().explain(110*MS,100*MS,105*MS,0,d);
        assertTrue(why,why.contains("emulation itself took 100.0 ms"));
        assertTrue(why,why.contains("interpreted 500,000 instructions (not recompiled code) = 500.0x normal"));
        assertTrue(why,why.contains("drew 2,800 primitives (heavy effects) = 10.0x normal"));
        assertTrue(why,why.contains("read 3 disc sectors (loading)"));
        assertTrue(why,why.contains("display: 1 shown"));
    }
    @Test public void normalWorkIsNotReportedAsUnusual(){
        long[] d=frame();d[FrameCause.EMULATION_CPU]=95*MS;
        String why=warmed().explain(110*MS,100*MS,105*MS,0,d);
        assertTrue(why,why.contains("nothing unusual"));
        assertFalse(why,why.contains("few frames"));
    }
    @Test public void missingPresentationIsCalledOut(){
        long[] d=frame();d[FrameCause.DISPLAYS]=0;
        assertTrue(warmed().explain(110*MS,100*MS,105*MS,0,d).contains("no frame finished presenting"));
    }
    @Test public void slowPresentVerdicts(){
        assertTrue(FrameCause.slowPresent(new long[]{80*MS,60*MS,5*MS,15*MS,2*MS}).contains("still drawing earlier commands"));
        assertTrue(FrameCause.slowPresent(new long[]{80*MS,5*MS,60*MS,15*MS,2*MS}).contains("copying the picture"));
        assertTrue(FrameCause.slowPresent(new long[]{80*MS,5*MS,5*MS,70*MS,3*MS}).contains("slow to accept the frame"));
        assertTrue(FrameCause.slowPresent(new long[]{80*MS,5*MS,5*MS,70*MS,60*MS}).contains("presentation code itself"));
    }
    @Test public void deltaKeepsPerFrameEmulationCpu(){
        long[] before=new long[FrameCause.COUNT],now=new long[FrameCause.COUNT];before[FrameCause.JIT]=10;now[FrameCause.JIT]=25;now[FrameCause.EMULATION_CPU]=7;before[FrameCause.EMULATION_CPU]=3;
        long[] d=FrameCause.delta(now,before);assertEquals(15,d[FrameCause.JIT]);assertEquals(7,d[FrameCause.EMULATION_CPU]);
    }
}
