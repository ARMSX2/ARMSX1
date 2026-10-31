package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
import java.util.Random;

public class ClockBatchTest {
    @Test public void timerEventJumpsMatchSingleClockReference(){
        Random random=new Random(147);
        for(int trial=0;trial<100;trial++)try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga),b=new Bus(gb);
            for(int timer=0;timer<3;timer++){
                int base=0x1f801100+timer*16,mode=random.nextInt(1024),target=random.nextInt(100),count=65500+random.nextInt(35);
                a.write16(base+4,mode);b.write16(base+4,mode);a.write16(base+8,target);b.write16(base+8,target);a.write16(base,count);b.write16(base,count);
            }
            int ticks=random.nextInt(10000);a.timers.cpuCycles(ticks);a.timers.dotClocks(ticks);
            for(int i=0;i<ticks;i++){b.timers.cpuCycles(1);b.timers.dotClocks(1);}
            for(int timer=0;timer<3;timer++){int base=0x1f801100+timer*16;assertEquals(b.read16(base),a.read16(base));assertEquals(b.read16(base+4),a.read16(base+4));}
            assertEquals(b.read32(0x1f801070),a.read32(0x1f801070));
        }
    }
    @Test public void mmioFlushesSubQuantumCpuTimeBeforeCounterRead(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);b.write16(0x1f801104,0);for(int i=0;i<7;i++)c.step();assertEquals(7,b.read16(0x1f801100));for(int i=0;i<26;i++)c.step();assertEquals(33,b.read16(0x1f801100));}
    }
    @Test public void clockRemaindersAreNotLostAcrossMmioFlushes(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);Cpu c=new Cpu(b);for(int i=0;i<1536;i++){c.step();if(i%17==0)b.read16(0x1f801104);}b.flushClocks();assertEquals(2,b.spu.frames);}
    }
    @Test public void uploadFencePublishesPartialBatchAndResetPreservesWrittenPixels(){
        try(Gpu g=new Gpu(true)){g.write(0xa0000000);g.write(0);g.write(0x00010004);g.write(0x03e0001f);int[] snapshot=g.snapshot();assertEquals(0xffff0000,snapshot[0]);assertEquals(0xff00ff00,snapshot[1]);assertEquals(0xff000000,snapshot[2]);g.control(0x01000000);g.write(0xe407ffff);g.write(0x680000ff);g.write(10);assertEquals(0xffff0000,g.snapshot()[10]);}
    }
    @Test public void ramFastPathsPreserveLittleEndianMirrorAndCacheIsolation(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);b.write32(0x801ffffc,0x89abcdef);assertEquals(0x89abcdef,b.read32(0xa07ffffc));assertEquals(0xcdef,b.read16(0x001ffffc));assertEquals(0x89ab,b.read16(0x003ffffe));b.cacheIsolated=true;b.write32(0x801ffffc,0);b.write16(0xa01ffffe,0);assertEquals(0x89abcdef,b.read32(0x001ffffc));}
    }
}
