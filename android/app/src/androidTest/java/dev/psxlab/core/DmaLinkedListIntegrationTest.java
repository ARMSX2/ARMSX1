package dev.psxlab.core;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

/** Exercises the DMA-start callback and subsequent RAM edit through native CPUs. */
@RunWith(AndroidJUnit4.class)
public class DmaLinkedListIntegrationTest {
    @Test public void nativeCpuReturnsFromCyclicListAndCanRepairIt(){
        for(boolean recompile:new boolean[]{false,true})for(boolean events:new boolean[]{false,true}){
            try(Gpu gpu=new Gpu(true)){
                Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);
                // Start a self-referencing list via MMIO, then repair its header
                // with a native RAM store after the DMA-start callback returns.
                bus.write32(0x100,0x100);
                int[] program={
                    0x3c081f80,0x3c090765,0x35294b21,0xad0910f0,
                    0x34090100,0xad0910a0,0x3c090100,0x35290401,0xad0910a8,
                    0x3c090080,0xac090100,0x26100001,0x1000ffff,0
                };
                for(int i=0;i<program.length;i++)bus.write32(0x10000+i*4,program[i]);
                assertTrue("Native CPU required",cpu.enableNative(recompile,events,true));
                try{
                    cpu.run(6000);bus.flushClocks();
                    assertEquals(1,cpu.r[16]);assertEquals(0x00800000,bus.read32(0x100));
                    assertEquals(0,bus.read32(0x1f8010a8)&0x11000000);
                    assertEquals(0xffffff,bus.read32(0x1f8010a0));
                    assertTrue(bus.spu.frames>0);assertTrue(bus.dma.diagnostic().contains("linked-yields=1"));
                }finally{cpu.closeNative();}
            }
        }
    }
}
