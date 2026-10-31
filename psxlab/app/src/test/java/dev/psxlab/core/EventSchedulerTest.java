package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
import java.util.*;
import java.util.zip.CRC32;

public class EventSchedulerTest {
    @Test public void cdChildEventsStartOnSameLegacyQuantum(){
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga),b=new Bus(gb);b.setEventScheduling(true);
            for(Bus bus:new Bus[]{a,b}){bus.write8(0x1f801800,1);bus.write8(0x1f801802,31);bus.write8(0x1f801800,0);bus.write8(0x1f801801,0xa);}
            for(int i=0;i<100000;i++){
                a.tick();b.tick();
                if(i%127==0){
                    assertEquals(a.read8(0x1f801800),b.read8(0x1f801800));
                    a.write8(0x1f801800,1);b.write8(0x1f801800,1);int x=a.read8(0x1f801803),y=b.read8(0x1f801803);assertEquals(x,y);
                    if((x&7)!=0){a.write8(0x1f801803,31);b.write8(0x1f801803,31);}
                    a.write8(0x1f801800,0);b.write8(0x1f801800,0);
                    assertEquals(a.read8(0x1f801801),b.read8(0x1f801801));
                }
            }
        }
    }
    @Test public void deadlinesPreserveVideoTimersSerialAndPcm(){
        Random random=new Random(0x45564e54);
        for(int trial=0;trial<12;trial++)try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga),b=new Bus(gb);b.setEventScheduling(true);
            CRC32 ca=new CRC32(),cb=new CRC32();a.spu.setAudioSink((pcm,n)->{for(int i=0;i<n;i++){ca.update(pcm[i]&255);ca.update((pcm[i]>>>8)&255);}});b.spu.setAudioSink((pcm,n)->{for(int i=0;i<n;i++){cb.update(pcm[i]&255);cb.update((pcm[i]>>>8)&255);}});
            for(int checkpoint=0;checkpoint<40;checkpoint++){
                int mode=random.nextInt(1024),target=random.nextInt(65536),timer=random.nextInt(3),base=0x1f801100+timer*16;
                a.write16(base+4,mode);b.write16(base+4,mode);a.write16(base+8,target);b.write16(base+8,target);
                if(checkpoint%5==0){int value=random.nextBoolean()?0x08000000:0x08000008;ga.control(value);gb.control(value);}
                int cycles=1000+random.nextInt(70000);for(int c=0;c<cycles;c++){a.tick();b.tick();}
                a.flushClocks();b.flushClocks();a.spu.flushAudio();b.spu.flushAudio();
                for(int t=0;t<3;t++)for(int r=0;r<3;r++)assertEquals("timer "+t+" reg "+r,a.read32(0x1f801100+t*16+r*4),b.read32(0x1f801100+t*16+r*4));
                assertEquals(ga.fields,gb.fields);assertEquals(ga.vblanks,gb.vblanks);assertEquals(ga.status(),gb.status());assertEquals(a.spu.frames,b.spu.frames);assertEquals(ca.getValue(),cb.getValue());assertEquals(a.read32(0x1f801070),b.read32(0x1f801070));
            }
            assertTrue("Quiet intervals should avoid constant device sweeps",b.deviceDispatches<a.deviceDispatches);
        }
    }
    @Test public void serialAckEdgesMatchLegacyGrid(){
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga),b=new Bus(gb);b.setEventScheduling(true);
            for(Bus bus:new Bus[]{a,b}){bus.write16(0x1f80104e,0x88);bus.write16(0x1f801048,0xd);bus.write16(0x1f80104a,0x1003);bus.write8(0x1f801040,1);}
            for(int i=0;i<2000;i++){a.tick();b.tick();if(i%47==0){assertEquals(a.read32(0x1f801044),b.read32(0x1f801044));assertEquals(a.read32(0x1f801070),b.read32(0x1f801070));}}
            assertEquals(a.read8(0x1f801040),b.read8(0x1f801040));assertEquals(a.sio0.ackPulses,b.sio0.ackPulses);
        }
    }
}
