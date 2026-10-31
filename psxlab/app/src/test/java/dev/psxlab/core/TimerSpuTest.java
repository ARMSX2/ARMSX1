package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class TimerSpuTest {
    @Test public void reservedTimerSlotReadsOpenBusAndIgnoresWrites(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801120,123);
            for(int base:new int[]{0x1f801130,0x9f801130,0xbf801130}){
                for(int offset=0;offset<16;offset++){assertEquals(255,b.read8(base+offset));b.write8(base+offset,0x5a);}
                for(int offset=0;offset<16;offset+=2){assertEquals(65535,b.read16(base+offset));b.write16(base+offset,0x1234);}
                for(int offset=0;offset<16;offset+=4){assertEquals(-1,b.read32(base+offset));b.write32(base+offset,0x12345678);}
            }
            assertEquals(123,b.read16(0x1f801120));assertEquals(0,b.read32(0x1f801070));
            assertEquals(-1,b.read32(0x1f80110c));b.write32(0x1f80110c,0);assertEquals(-1,b.read32(0x1f80110c));
        }
    }
    @Test public void timerTargetResetsAndRaisesRepeatIrqs(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801074,16);b.write16(0x1f801108,3);b.write16(0x1f801104,0x58);b.timers.cpuCycles(3);
            assertEquals(0,b.read16(0x1f801100));assertTrue(b.interruptPending());assertNotEquals(0,b.read16(0x1f801104)&0x800);assertEquals(0,b.read16(0x1f801104)&0x800);
            b.write16(0x1f801070,0xffef);b.timers.cpuCycles(3);assertTrue(b.interruptPending());
        }
    }
    @Test public void timerOneShotSuppressesSecondIrqAndDiv8Counts(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801074,64);b.write16(0x1f801128,2);b.write16(0x1f801124,0x218);b.timers.cpuCycles(15);assertFalse(b.interruptPending());b.timers.cpuCycles(1);assertTrue(b.interruptPending());b.write16(0x1f801070,0xffbf);b.timers.cpuCycles(32);assertFalse(b.interruptPending());
        }
    }
    @Test public void timerExternalSourcesOnlyAdvanceOnSuppliedPulses(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801104,0x100);b.write16(0x1f801114,0x100);b.timers.cpuCycles(10);assertEquals(0,b.read16(0x1f801100));assertEquals(0,b.read16(0x1f801110));b.timers.dotClocks(3);b.timers.hblank(true);b.timers.hblank(false);assertEquals(3,b.read16(0x1f801100));assertEquals(1,b.read16(0x1f801110));
        }
    }
    @Test public void timerSyncCanPauseAndRelease(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801104,1);b.timers.hblank(true);b.timers.cpuCycles(3);assertEquals(0,b.read16(0x1f801100));b.timers.hblank(false);b.timers.cpuCycles(2);assertEquals(2,b.read16(0x1f801100));
            b.write16(0x1f801104,7);b.timers.cpuCycles(2);assertEquals(0,b.read16(0x1f801100));b.timers.hblank(true);b.timers.cpuCycles(2);assertEquals(2,b.read16(0x1f801100));
        }
    }
    @Test public void spuTransfersWrapSoundRamAndStatusReflectsMode(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write16(0x1f801da6,0xffff);for(int i=0;i<6;i++)b.write16(0x1f801da8,i+123);b.write16(0x1f801da6,0xffff);for(int i=0;i<6;i++)assertEquals(i+123,b.spu.readTransfer());
            b.write16(0x1f801daa,0x8030);assertEquals(0x2b0,b.read16(0x1f801dae));
        }
    }
    @Test public void spuDmaMovesWordsToSoundRamAndBack(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x1f8010f0,0x076d4321);b.write32(0x100,0x1234abcd);b.write16(0x1f801da6,0);b.write32(0x1f8010c0,0x100);b.write32(0x1f8010c4,0x10001);b.write32(0x1f8010c8,0x01000201);
            b.write16(0x1f801da6,0);b.write32(0x1f8010c0,0x200);b.write32(0x1f8010c4,0x10001);b.write32(0x1f8010c8,0x01000200);assertEquals(0x1234abcd,b.read32(0x200));
        }
    }
}
