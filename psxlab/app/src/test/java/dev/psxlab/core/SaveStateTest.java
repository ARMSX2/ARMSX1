package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class SaveStateTest {
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
