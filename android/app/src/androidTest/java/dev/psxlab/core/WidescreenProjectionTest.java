package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

/** Geometry matches across portable, native interpreter and ARM64 compiler paths. */
public class WidescreenProjectionTest {
    @Test public void nativeGeometryMatchesReferenceForAllSelectedRatios(){assertTrue(Gte.verifyNative().contains("match"));}
    @Test public void nativeCpuUpdatesAspectOnAnAlreadyCompiledBlock(){
        for(boolean compiler:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);cpu.cop0[12]=0x40000000;
            cpu.gte.writeControl(0,4096);cpu.gte.writeControl(2,4096);cpu.gte.writeControl(4,4096);cpu.gte.writeControl(26,1000);
            cpu.gte.writeControl(24,160<<16);cpu.gte.writeControl(25,120<<16);
            cpu.gte.writeData(0,(320&65535)|(-200<<16));cpu.gte.writeData(1,1000);bus.write32(0x10000,0x4a080001);
            assertTrue(cpu.enableNative(compiler,true));
            try{
                for(float aspect:new float[]{4f/3f,16f/9f,16f/10f,2,20f/9f,64f/27f,1,4f/3f}){
                    cpu.gte.setWidescreen(true,aspect);cpu.jump(0x80010000);cpu.run(1);
                    assertEquals("compiler="+compiler+" aspect="+aspect,160+320*(4.0/3)/aspect,(short)cpu.gte.readData(14),1);
                    assertEquals(-80,(short)(cpu.gte.readData(14)>>>16));assertEquals(0,cpu.exceptions);
                }
                cpu.gte.setWidescreen(false,64f/27f);cpu.jump(0x80010000);cpu.run(1);assertEquals(480,(short)cpu.gte.readData(14));
            }finally{cpu.closeNative();}
        }
    }
}
