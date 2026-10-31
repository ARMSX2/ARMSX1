package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class ReservedTimerIntegrationTest {
    @Test public void biosStyleReservedTimerProbesReturnOpenBusInEveryCpuMode(){
        for(int mode=0;mode<3;mode++)try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);
            int[] code={0x3c081f80,0x35081130,0x8d020000,0x95030000,0x91040000,0xad000004,0xa5000008,0xa100000c,0x1000ffff,0};
            for(int i=0;i<code.length;i++)bus.write32(0x10000+i*4,code[i]);
            if(mode!=0)assertTrue(cpu.enableNative(mode==2,true,true));
            try{cpu.jump(0x80010000);cpu.run(128);assertEquals(-1,cpu.r[2]);assertEquals(65535,cpu.r[3]);assertEquals(255,cpu.r[4]);assertEquals(0,cpu.exceptions);}
            finally{cpu.closeNative();}
        }
    }
}
