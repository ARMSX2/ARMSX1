package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class CheatsIntegrationTest {
    @Test public void cheatPatchesReachNativeRamAndInvalidateCompiledInstructions()throws Exception{
        for(boolean jit:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);bus.write32(0x10000,0x24420001);bus.write32(0x10004,0x1000ffff);bus.write32(0x10008,0);
            assertTrue(cpu.enableNative(jit,true,true));
            try{
                cpu.jump(0x80010000);cpu.run(64);assertEquals(1,cpu.r[2]);
                Cheats cheats=new Cheats();cheats.loadText("[Patch]\n80010000 0009\n30000200 0063\n");assertEquals(1,cheats.arm(true,new String[]{"Patch"}));cheats.apply(bus);
                assertEquals(99,bus.read8(0x200));cpu.jump(0x80010000);cpu.run(64);assertEquals(10,cpu.r[2]);
                long compiled=cpu.nativeStatistics()[0];for(int i=0;i<100;i++){cheats.apply(bus);cpu.jump(0x80010000);cpu.run(64);}assertEquals(910,cpu.r[2]);assertEquals(compiled,cpu.nativeStatistics()[0]);
                cheats.arm(false,null);bus.write8(0x200,2);cheats.apply(bus);assertEquals(2,bus.read8(0x200));
            }finally{cpu.closeNative();}
        }
    }
}
