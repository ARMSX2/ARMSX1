package dev.psxlab.core;
import org.junit.Test;
import java.io.*;
import static org.junit.Assert.*;
public class BiosBootTest {
    private byte[] fixture(boolean handler){
        byte[] bios=new byte[512*1024];int[] code=handler?new int[]{0x3c088000,0x35080080,0x3c092401,0x3529002a,0xad090000,0x3c0a8003,0x01400008,0}:new int[]{0x3c0a8003,0x01400008,0};
        for(int i=0;i<code.length;i++)BootTest.word(bios,i*4,code[i]);return bios;
    }
    @Test public void reachesHandoffWithInstalledHandler()throws Exception{
        try(Gpu gpu=new Gpu(false)){Bus bus=new Bus(gpu);Cpu cpu=BiosBoot.initialize(fixture(true),bus,()->false);assertEquals(0x80030000,cpu.pc);assertEquals(0x2401002a,bus.read32(0x80));}
    }
    @Test public void rejectsHandoffWithoutHandler(){
        try(Gpu gpu=new Gpu(false)){assertThrows(IOException.class,()->BiosBoot.initialize(fixture(false),new Bus(gpu),()->false));}
    }
    @Test public void bootstrapCanBeCancelled(){
        try(Gpu gpu=new Gpu(false)){assertThrows(InterruptedIOException.class,()->BiosBoot.initialize(fixture(true),new Bus(gpu),()->true));}
    }
    @Test public void loadingGamePreservesKernelAndCop0State()throws Exception{
        try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=BiosBoot.initialize(fixture(true),bus,()->false);cpu.cop0[12]=0x401;long before=cpu.instructions;
            assertSame(cpu,ExeLoader.loadInto(BootTest.executable(),bus,cpu));assertEquals(0x401,cpu.cop0[12]);assertEquals(before,cpu.instructions);assertEquals(0x2401002a,bus.read32(0x80));cpu.step();assertEquals(42,cpu.r[1]);
        }
    }
}
