package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class DmaTest {
    @Test public void otcBuildsReverseOrderingTableAndRaisesIrq(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x1f8010f0,0x0f654321);b.write32(0x1f8010f4,0x00c00000);b.write16(0x1f801074,8);
            b.write32(0x1f8010e0,0x100);b.write32(0x1f8010e4,3);b.write32(0x1f8010e8,0x11000002);
            assertEquals(0xfc,b.read32(0x100));assertEquals(0xf8,b.read32(0xfc));assertEquals(0xffffff,b.read32(0xf8));
            assertEquals(0x100,b.read32(0x1f8010e0));assertEquals(0,b.read32(0x1f8010e8)&0x11000000);assertTrue(b.interruptPending());
            assertEquals(0xc0000000,b.read32(0x1f8010f4)&0xc0000000);
            b.write32(0x1f8010f4,0x40c00000);assertEquals(0,b.read32(0x1f8010f4)&0xc0000000);
        }
    }
    @Test public void gpuLinkedListUsesCommandOrderAndStopsAtMarker(){
        for(boolean threaded:new boolean[]{false,true})try(Gpu g=new Gpu(threaded)){
            Bus b=new Bus(g);b.write32(0x100,0x03000200);b.write32(0x104,0xa0000000);b.write32(0x108,0);b.write32(0x10c,0x00010002);
            b.write32(0x200,0x01ffffff);b.write32(0x204,0x1234abcd);
            b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);b.write32(0x1f8010a8,0x01000401);
            g.write(0xc0000000);g.write(0);g.write(0x00010002);assertEquals(0x1234abcd,g.read());assertEquals(0xffffff,b.read32(0x1f8010a0));
        }
    }
    @Test public void blockDmaTransfersFromGpuToRam(){
        try(Gpu g=new Gpu(true)){
            Bus b=new Bus(g);g.write(0xa0000000);g.write(0);g.write(0x00010002);g.write(0x7c00001f);g.write(0xc0000000);g.write(0);g.write(0x00010002);
            b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);b.write32(0x1f8010a4,0x00010001);b.write32(0x1f8010a8,0x01000200);
            assertEquals(0x7c00001f,b.read32(0x100));assertEquals(0x104,b.read32(0x1f8010a0));
        }
    }
    @Test public void disabledDmaWaitsForMasterEnable(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x1f8010e0,0x100);b.write32(0x1f8010e4,1);b.write32(0x1f8010e8,0x11000002);assertEquals(0,b.read32(0x100));b.write32(0x1f8010f0,0x0f654321);assertEquals(0xffffff,b.read32(0x100));
        }
    }
    private static void startList(Bus b,int address){
        b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010f4,0x00840000);b.write16(0x1f801074,8);
        b.write32(0x1f8010a0,address);b.write32(0x1f8010a8,0x01000401);
    }
    @Test(timeout=5000) public void cyclicListYieldsWithoutThrowingOrClaimingCompletion(){
        for(boolean threaded:new boolean[]{false,true})try(Gpu g=new Gpu(threaded)){
            Bus b=new Bus(g);b.write32(0x100,0x100);startList(b,0x100);
            assertEquals(0x100,b.read32(0x1f8010a0));assertNotEquals(0,b.read32(0x1f8010a8)&0x1000000);
            assertFalse(b.interruptPending());assertEquals(1024,b.dma.clocksToEvent());
            b.dma.tick(10000);assertFalse(b.interruptPending());assertNotEquals(0,b.read32(0x1f8010a8)&0x1000000);
            b.write32(0x100,0x00800000);b.dma.tick(1024);
            assertEquals(0xffffff,b.read32(0x1f8010a0));assertEquals(0,b.read32(0x1f8010a8)&0x11000000);assertTrue(b.interruptPending());
            b.write32(0x1f8010f4,0x04840000);b.write16(0x1f801070,0);b.dma.tick(10000);assertFalse(b.interruptPending());
        }
    }
    @Test(timeout=5000) public void cpuCanRepairTwoNodeLoopInBothRamRepresentations(){
        for(boolean nativeRam:new boolean[]{false,true})try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g,new PadState(),false);if(nativeRam)b.enableNativeMemory();b.setEventScheduling(true);
            b.write32(0x100,0x200);b.write32(0x200,0x100);startList(b,0x100);
            // lui t0,0x0080; sw t0,0x100(zero); b .; nop
            int[] code={0x3c080080,0xac080100,0x1000ffff,0};for(int i=0;i<code.length;i++)b.write32(0x10000+4*i,code[i]);
            Cpu cpu=new Cpu(b);cpu.run(3000);b.flushClocks();
            assertEquals(3000,cpu.instructions);assertEquals(0,b.read32(0x1f8010a8)&0x1000000);assertTrue(b.interruptPending());
            assertTrue(b.spu.frames>0);
        }
    }
    @Test public void longValidListKeepsCommandOrderAcrossSlices(){
        for(boolean threaded:new boolean[]{false,true})try(Gpu g=new Gpu(threaded)){
            Bus b=new Bus(g);b.dma.performanceEnabled=true;
            for(int i=0;i<1200;i++){int at=0x100+8*i;b.write32(at,0x01000000|(i==1199?0xffffff:at+8));b.write32(at+4,0xe6000000|(i&3));}
            startList(b,0x100);assertFalse(b.interruptPending());assertEquals(512,b.dma.performanceWords[2]);
            b.dma.tick(1023);assertEquals(512,b.dma.performanceWords[2]);b.dma.tick(1);assertEquals(1024,b.dma.performanceWords[2]);
            b.dma.tick(1024);assertEquals(1200,b.dma.performanceWords[2]);assertEquals(3<<11,g.status()&0x1800);
            assertEquals(0xffffff,b.read32(0x1f8010a0));assertTrue(b.interruptPending());
        }
    }
    @Test public void disablePausesAndStopCancelsDeferredList(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.write32(0x100,0x100);startList(b,0x100);b.dma.tick(100);
            b.write32(0x1f8010f0,0x07654321);assertEquals(Integer.MAX_VALUE,b.dma.clocksToEvent());
            b.write32(0x100,0xffffff);b.dma.tick(10000);assertFalse(b.interruptPending());
            b.write32(0x1f8010f0,0x07654b21);b.dma.tick(923);assertFalse(b.interruptPending());
            b.write32(0x1f8010a8,0);b.dma.tick(10000);assertFalse(b.interruptPending());assertEquals(0x100,b.read32(0x1f8010a0));
            b.write32(0x1f8010a8,0x01000401);assertTrue(b.interruptPending());
        }
    }
    @Test public void listTickBatchingMatchesSingleClocks(){
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga),b=new Bus(gb);a.write32(0x100,0x100);b.write32(0x100,0x100);startList(a,0x100);startList(b,0x100);
            for(int i=0;i<4097;i++)a.dma.tick(1);b.dma.tick(4097);assertEquals(a.dma.diagnostic(),b.dma.diagnostic());
            a.write32(0x100,0xffffff);b.write32(0x100,0xffffff);a.dma.tick(1023);b.dma.tick(1023);
            assertEquals(a.read32(0x1f8010a8),b.read32(0x1f8010a8));assertEquals(a.read32(0x1f8010f4),b.read32(0x1f8010f4));
        }
    }
}
