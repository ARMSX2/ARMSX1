package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

/** A completion callback must not run inside the write which starts a streamed DMA. */
public class SpuDmaStreamingTest {
    private static void start(Bus b,int control){
        for(int i=0;i<6;i++)b.write32(0x18000+i*4,0x10203040+i);
        b.write32(0x1f8010f0,0x076d4321);b.write32(0x1f8010f4,0x00900000);b.write16(0x1f801074,8);
        b.spu.write(0x1f801da6,0x200);b.spu.write(0x1f801daa,0x8020);
        b.write32(0x1f8010c0,0x18000);b.write32(0x1f8010c4,0x00030002);b.write32(0x1f8010c8,control);
    }
    @Test public void blocksAdvanceRegistersAndSoundRamBeforeOneCompletionInterrupt(){
        try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);start(b,0x01000201);
            assertEquals(0x18008,b.read32(0x1f8010c0));assertEquals(0x00020002,b.read32(0x1f8010c4));assertFalse(b.interruptPending());
            b.dma.tick(63);assertEquals(0x18008,b.read32(0x1f8010c0));b.dma.tick(1);
            assertEquals(0x18010,b.read32(0x1f8010c0));assertFalse(b.interruptPending());b.dma.tick(64);
            assertEquals(0x18018,b.read32(0x1f8010c0));assertEquals(2,b.read32(0x1f8010c4));assertEquals(0,b.read32(0x1f8010c8)&0x1000000);assertTrue(b.interruptPending());
            b.spu.write(0x1f801da6,0x200);for(int i=0;i<6;i++)assertEquals(0x10203040+i,b.spu.readTransfer()|(b.spu.readTransfer()<<16));
        }
    }
    @Test public void disablingPausesAndClearingBusyCancelsTheRemainingBlocks(){
        try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);start(b,0x01000201);b.dma.tick(17);
            b.write32(0x1f8010f0,0x07654321);b.dma.tick(1000);assertEquals(0x18008,b.read32(0x1f8010c0));assertFalse(b.interruptPending());
            b.write32(0x1f8010f0,0x076d4321);b.dma.tick(46);assertEquals(0x18008,b.read32(0x1f8010c0));b.dma.tick(1);assertEquals(0x18010,b.read32(0x1f8010c0));
            b.write32(0x1f8010c8,0);b.dma.tick(1000);assertEquals(0x18010,b.read32(0x1f8010c0));assertFalse(b.interruptPending());
        }
    }
    @Test public void pendingBlocksAndTheirRemainingDelaySurviveSaveRestore()throws Exception{
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga,new PadState(),false),b=new Bus(gb,new PadState(),false);Cpu ca=new Cpu(a),cb=new Cpu(b);
            start(a,0x01000201);a.dma.tick(17);byte[] state=SaveState.capture(ca,a,ga,"spu-dma");SaveState.restore(state,cb,b,gb,"spu-dma");
            for(Bus bus:new Bus[]{a,b}){bus.dma.tick(46);assertEquals(0x18008,bus.read32(0x1f8010c0));bus.dma.tick(65);assertTrue(bus.interruptPending());}
            assertArrayEquals(SaveState.capture(ca,a,ga,"spu-dma"),SaveState.capture(cb,b,gb,"spu-dma"));
        }
    }
    @Test public void oldDmaStatesCancelAnyCurrentlyPendingSpuRequest()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus b=new Bus(gpu,new PadState(),false);StateCodec.Node legacy=StateCodec.capture(b.dma);legacy.fields.remove("spuRequest");
            start(b,0x01000201);StateCodec.apply(legacy,b.dma);b.dma.tick(1000);assertFalse(b.interruptPending());assertEquals(Integer.MAX_VALUE,b.dma.clocksToEvent());
        }
    }
    @Test public void multiBlockSpuDmaCompletionSeesThePublishedRequestState(){
        for(int mode=0;mode<(NativeCore.AVAILABLE?3:1);mode++)try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);
            int[] handler={0x3c1a0001,0x8f5b0000,0,0xaf5b0004,0x241b0001,0xaf5b0008,
                    0x3c1a1f80,0x241bfff7,0xaf5b1070,0x3c1b1090,0xaf5b10f4,0x401a7000,0,0x03400008,0x42000010};
            for(int i=0;i<handler.length;i++)bus.write32(0x80+i*4,handler[i]);
            int[] program={0xad090000,0xad4b0000,0x1000ffff,0};
            for(int i=0;i<program.length;i++)bus.write32(0x20000+i*4,program[i]);
            for(int i=0;i<16;i++)bus.write32(0x18000+i*4,0x12345678+i);
            bus.write32(0x1f8010f0,0x076d4321);bus.write32(0x1f8010f4,0x00900000);
            bus.write16(0x1f801da6,0x200);bus.write16(0x1f801daa,0x8020);
            bus.write32(0x1f8010c0,0x18000);bus.write32(0x1f8010c4,0x00800010);
            bus.write32(0x1f801074,8);cpu.cop0[12]=0x401;
            cpu.r[8]=0x1f8010c8;cpu.r[9]=0x01000201;cpu.r[10]=0x10000;cpu.r[11]=1;
            if(mode>0)assertTrue(cpu.enableNative(mode==2,true,true));cpu.jump(0x80020000);
            try{cpu.run(100000);bus.flushClocks();assertEquals("DMA callback was never reached, mode="+mode,1,bus.read32(0x10008));
                assertEquals("Completion ran before the following request-state store, mode="+mode,1,bus.read32(0x10004));}
            finally{cpu.closeNative();}
        }
    }
}

