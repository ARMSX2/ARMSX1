package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class SaveStateTest {
    @Test public void oldCdStatesRestoreTheExistingInterruptLevelWithoutANewEdge()throws Exception{
        for(boolean pending:new boolean[]{false,true})try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g,new PadState(),false);b.write16(0x1f801074,4);
            b.cdrom.write(0,1);b.cdrom.write(2,31);b.cdrom.write(0,0);
            if(pending)CdRomTest.command(b,1);
            StateCodec.Node legacy=StateCodec.capture(b.cdrom);legacy.fields.remove("irqAsserted");
            b.write16(0x1f801070,0xfffb);StateCodec.apply(legacy,b.cdrom);b.cdrom.tick(32);
            assertFalse("Restoring a latched CD response must not create a new IRQ edge",b.interruptPending());
            CdRomTest.ack(b);CdRomTest.command(b,1);assertTrue(b.interruptPending());
        }
    }
    @Test public void deferredGpuListSurvivesSaveAndRestore()throws Exception{
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga,new PadState(),false),b=new Bus(gb,new PadState(),false);Cpu ca=new Cpu(a),cb=new Cpu(b);
            a.write32(0x100,0x100);a.write32(0x1f8010f0,0x07654b21);a.write32(0x1f8010a0,0x100);a.write32(0x1f8010a8,0x01000401);a.dma.tick(123);
            byte[] state=SaveState.capture(ca,a,ga,"dma");SaveState.restore(state,cb,b,gb,"dma");
            assertEquals(901,b.dma.clocksToEvent());assertNotEquals(0,b.read32(0x1f8010a8)&0x1000000);
            b.write32(0x100,0xffffff);b.dma.tick(900);assertNotEquals(0,b.read32(0x1f8010a8)&0x1000000);
            b.dma.tick(1);assertEquals(0,b.read32(0x1f8010a8)&0x1000000);assertEquals(0xffffff,b.read32(0x1f8010a0));
        }
    }
    @Test public void oldDmaStateDefaultsToNoDeferredSlice()throws Exception{
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);StateCodec.Node old=StateCodec.capture(b.dma);old.fields.remove("linkedDelay");
            b.write32(0x100,0x100);b.write32(0x1f8010f0,0x07654b21);b.write32(0x1f8010a0,0x100);b.write32(0x1f8010a8,0x01000401);
            StateCodec.apply(old,b.dma);assertEquals(Integer.MAX_VALUE,b.dma.clocksToEvent());
        }
    }
    @Test public void overlayMeasurementsDoNotChangeSaveStateSchemaOrGuestState()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=new Cpu(bus);
            byte[] before=SaveState.capture(cpu,bus,gpu,"osd");
            gpu.performanceEnabled=true;gpu.performanceTriangles=123;gpu.performanceRectangles=45;gpu.performanceLines=6;
            bus.dma.performanceEnabled=true;bus.dma.performanceWords[2]=789;
            assertArrayEquals(before,SaveState.capture(cpu,bus,gpu,"osd"));
            SaveState.restore(before,cpu,bus,gpu,"osd");assertEquals(123,gpu.performanceTriangles);assertEquals(789,bus.dma.performanceWords[2]);
        }
    }
    private void program(Bus bus){int[] code={0x3c088001,0x8d090200,0,0x25290001,0xad090200,0x08004000,0};for(int i=0;i<code.length;i++)bus.write32(0x10000+i*4,code[i]);}
    @Test public void resumeMatchesUninterruptedCpuRamDevicesAndPendingGpuPacket()throws Exception{
        try(Gpu ga=new Gpu(false);Gpu gb=new Gpu(false)){
            Bus a=new Bus(ga,new PadState(),false),b=new Bus(gb,new PadState(),false);Cpu ca=new Cpu(a),cb=new Cpu(b);program(a);ca.run(10000);a.flushClocks();ga.write(0x0200ff00);ga.write(0);byte[] state=SaveState.capture(ca,a,ga,"game-a");
            ca.run(14000);a.flushClocks();ga.write(8|(8<<16));int[] expectedGpu=ga.snapshot();byte[] expectedRam=a.snapshotRam();
            SaveState.restore(state,cb,b,gb,"game-a");cb.run(14000);b.flushClocks();gb.write(8|(8<<16));
            assertEquals(ca.instructions,cb.instructions);assertEquals(ca.pc,cb.pc);assertArrayEquals(ca.r,cb.r);assertArrayEquals(expectedRam,b.snapshotRam());assertArrayEquals(expectedGpu,gb.snapshot());assertEquals(a.spu.frames,b.spu.frames);assertEquals(ga.vblanks,gb.vblanks);
        }
    }
    @Test public void wrongGameBiosAndCorruptStateAreRejectedBeforeUse()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu,new PadState(),false);Cpu cpu=new Cpu(bus);byte[] bytes=SaveState.capture(cpu,bus,gpu,"one");
            try{SaveState.restore(bytes,cpu,bus,gpu,"two");fail();}catch(java.io.IOException expected){assertTrue(expected.getMessage().contains("different game"));}
            bus.bios[10]=1;try{SaveState.restore(bytes,cpu,bus,gpu,"one");fail();}catch(java.io.IOException expected){assertTrue(expected.getMessage().contains("BIOS"));}bus.bios[10]=0;
            bytes[bytes.length/2]^=0x40;try{SaveState.restore(bytes,cpu,bus,gpu,"one");fail();}catch(java.io.IOException expected){}
        }
    }
}
