package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class DmaChoppingTest {
    private static final int MADR=0x1f8010b0,BCR=MADR+4,CHCR=MADR+8,DPCR=0x1f8010f0,DICR=DPCR+4;
    private static Bus ready(Gpu gpu,int count,int control){
        Bus b=new Bus(gpu);CdRomTest.readFirst(b,9,0);b.write8(CdRomTest.BASE+3,0x80);
        b.write32(DPCR,0x0765c321);b.write32(DICR,0x00880000);b.write16(0x1f801074,8);
        b.write32(MADR,0x10000);b.write32(BCR,0x10000|count);b.write32(CHCR,control);return b;
    }
    @Test public void ctrControlCopies504WordsWithFinalOnlyInterrupt(){
        try(Gpu g=new Gpu(false)){
            Bus b=ready(g,504,0x11400100);
            assertEquals(0,b.cdrom.dmaBytes);assertEquals(0x01400100,b.read32(CHCR));assertFalse(b.interruptPending());
            int duration=504*40+503*16;b.dma.tick(duration-1);
            assertEquals(503*4,b.cdrom.dmaBytes);assertEquals(0x10000+503*4,b.read32(MADR));assertEquals(0x10001,b.read32(BCR));
            assertNotEquals(0,b.read32(CHCR)&0x1000000);assertFalse(b.interruptPending());
            b.dma.tick(1);assertEquals(2016,b.cdrom.dmaBytes);assertEquals(0x107e0,b.read32(MADR));assertEquals(0x10000,b.read32(BCR));
            assertEquals(0,b.read32(CHCR)&0x11000000);assertTrue(b.interruptPending());assertEquals(504,b.dma.choppedWindows);
            for(int i=0;i<2016;i++)assertEquals((9+i)&255,b.read8(0x10000+i));assertEquals(32,b.cdrom.availableBytes());
            long bytes=b.cdrom.dmaBytes;b.dma.tick(100000);b.dma.resumeCdrom();assertEquals(bytes,b.cdrom.dmaBytes);
        }
    }
    @Test public void cpuWindowDoesNotAdvanceDataAndMadrBcrRemainObservable(){
        try(Gpu g=new Gpu(false)){
            Bus b=ready(g,3,0x11400100);b.dma.tick(39);assertEquals(0,b.cdrom.dmaBytes);
            b.dma.tick(1);assertEquals(4,b.cdrom.dmaBytes);assertEquals(0x10002,b.read32(BCR));assertEquals(0x10004,b.read32(MADR));
            b.dma.tick(16);assertEquals(4,b.cdrom.dmaBytes);b.dma.tick(39);assertEquals(4,b.cdrom.dmaBytes);
            b.dma.tick(1);assertEquals(8,b.cdrom.dmaBytes);assertFalse(b.interruptPending());
            b.dma.tick(56);assertEquals(12,b.cdrom.dmaBytes);assertTrue(b.interruptPending());
        }
    }
    @Test public void windowSizeAndShortFinalWindowAreHonored(){
        try(Gpu g=new Gpu(false)){
            Bus b=ready(g,5,0x11320100); // Four DMA words, eight CPU cycles.
            b.dma.tick(159);assertEquals(0,b.cdrom.dmaBytes);b.dma.tick(1);assertEquals(16,b.cdrom.dmaBytes);
            b.dma.tick(47);assertEquals(16,b.cdrom.dmaBytes);assertFalse(b.interruptPending());
            b.dma.tick(1);assertEquals(20,b.cdrom.dmaBytes);assertEquals(2,b.dma.choppedWindows);assertTrue(b.interruptPending());
        }
    }
    @Test public void startsAfterDataRequestWithoutRestartingAtEachResume(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);CdRomTest.readFirst(b,7,0);b.write32(DPCR,0x0765c321);
            b.write32(MADR,0x20000);b.write32(BCR,4);b.write32(CHCR,0x11400100);
            b.dma.tick(1000);assertEquals(0,b.cdrom.dmaBytes);assertEquals(0,b.dma.choppedTransfers);
            b.write8(CdRomTest.BASE+3,0x80);b.dma.tick(40);assertEquals(4,b.cdrom.dmaBytes);
            for(int i=0;i<5;i++)b.dma.resumeCdrom();b.dma.tick(168);assertEquals(16,b.cdrom.dmaBytes);assertEquals(1,b.dma.choppedTransfers);
            for(int i=0;i<16;i++)assertEquals((7+i)&255,b.read8(0x20000+i));
        }
    }
    @Test public void channelDisablePausesAndStopCancelsScheduledCompletion(){
        try(Gpu g=new Gpu(false)){
            Bus b=ready(g,4,0x11400100);b.dma.tick(40);b.write32(DPCR,0x07654321);
            b.dma.tick(10000);assertEquals(4,b.cdrom.dmaBytes);assertFalse(b.interruptPending());
            b.write32(DPCR,0x0765c321);b.dma.tick(56);assertEquals(8,b.cdrom.dmaBytes);
            b.write32(CHCR,0);b.dma.tick(10000);assertEquals(8,b.cdrom.dmaBytes);assertFalse(b.interruptPending());
            b.write32(MADR,0x20000);b.write32(BCR,2);b.write32(CHCR,0x11400100);b.dma.tick(96);
            assertEquals(16,b.cdrom.dmaBytes);assertEquals(17,b.read8(0x20000));assertTrue(b.interruptPending());
        }
    }
    @Test public void backwardsCdTransferAndRamAliasInvalidateInstructionCache(){
        try(Gpu g=new Gpu(false)){
            Bus b=ready(g,2,0x11400102);b.fetchInstruction(0x80010000);b.fetchInstruction(0x8000fffc);
            b.dma.tick(96);assertEquals(0x0c0b0a09,b.fetchInstruction(0x80010000));assertEquals(0x100f0e0d,b.fetchInstruction(0x8000fffc));assertEquals(0xfff8,b.read32(MADR));
        }
    }
    @Test public void tickBatchingMatchesSingleClockProgress(){
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=ready(ga,64,0x11420100),b=ready(gb,64,0x11420100);
            int total=64*40+15*16;
            for(int i=0;i<total;i++)a.dma.tick(1);
            for(int left=total;left>0;){int n=Math.min(left,31);b.dma.tick(n);left-=n;}
            assertEquals(a.cdrom.dmaBytes,b.cdrom.dmaBytes);assertEquals(a.dma.choppedWindows,b.dma.choppedWindows);
            assertEquals(a.read32(MADR),b.read32(MADR));assertEquals(a.read32(BCR),b.read32(BCR));assertEquals(a.read32(DICR),b.read32(DICR));assertArrayEquals(a.ram,b.ram);
        }
    }
    @Test public void normalCpuBusClockProgressesChoppingAndOtherDevices(){
        try(Gpu g=new Gpu(false)){
            Bus b=ready(g,4,0x11400100);b.write16(0x1f801100,0);b.write16(0x1f801104,0);
            for(int i=0;i<208;i++)b.tick();b.flushClocks();
            assertEquals(16,b.cdrom.dmaBytes);assertEquals(208,b.read16(0x1f801100));assertTrue(b.interruptPending());
        }
    }
}
