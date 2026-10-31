package dev.psxlab.core;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;
import org.junit.Test;
import org.junit.runner.RunWith;
import static org.junit.Assert.*;

@RunWith(AndroidJUnit4.class)
public class CpuCacheEfficiencyTest {
    private static void invoke(Cpu cpu,int address){cpu.jump(address);cpu.run(64);}
    @Test public void unchangedBlocksSurviveChangesElsewhereOnTheSamePage(){
        for(boolean recompile:new boolean[]{false,true})for(boolean events:new boolean[]{false,true})try(Gpu gpu=new Gpu(false)){
            Bus bus=new Bus(gpu);Cpu cpu=new Cpu(bus);
            // A and B share a physical page. The stop loop lives on another page.
            int a=0x80010000,b=0xa0010100;
            bus.write32(a,0x24420001);bus.write32(a+4,0x03e00008);bus.write32(a+8,0);
            bus.write32(b,0x24630001);bus.write32(b+4,0x03e00008);bus.write32(b+8,0);
            bus.write32(0x12000,0x1000ffff);bus.write32(0x12004,0);cpu.r[31]=0x80012000;
            assertTrue(cpu.enableNative(recompile,events,true));
            try{
                invoke(cpu,a);invoke(cpu,b);long compiled=cpu.nativeStatistics()[0];
                long start=android.os.SystemClock.elapsedRealtimeNanos();
                for(int i=0;i<1000;i++){bus.write32(b,0x24630001+(i&1));invoke(cpu,a);}
                long elapsed=android.os.SystemClock.elapsedRealtimeNanos()-start;
                assertEquals(1001,cpu.r[2]);assertEquals(1,cpu.r[3]);
                long[] stats=cpu.nativeStatistics();
                if(!"false".equals(InstrumentationRegistry.getArguments().getString("cpuCacheOptimized"))){
                    assertEquals("Unchanged A must not be compiled again",compiled,stats[0]);assertTrue(stats[8]>=1000);
                }
                android.util.Log.i("CpuCacheEfficiencyTest","JIT="+recompile+" events="+events+" 1000 sibling writes elapsed_ns="+elapsed+" compiled_delta="+(stats[0]-compiled)+" "+cpu.executionDiagnostic());
                invoke(cpu,b);assertEquals("B's changed instruction must execute",3,cpu.r[3]);
                bus.write8(a,9);invoke(cpu,a);assertEquals(1010,cpu.r[2]);
                // A changed formerly-NOP delay slot must invalidate its containing block.
                bus.write32(a+8,0x24040037);invoke(cpu,a);assertEquals(1019,cpu.r[2]);assertEquals(55,cpu.r[4]);
                // DMA code writes use the same generation tracking as native CPU stores.
                gpu.write(0xa0000000);gpu.write(0);gpu.write(0x00010002);gpu.write(0x24420007);
                gpu.write(0xc0000000);gpu.write(0);gpu.write(0x00010002);
                bus.write32(0x1f8010f0,0x07654b21);bus.write32(0x1f8010a0,a&0x1fffff);bus.write32(0x1f8010a4,0x10001);bus.write32(0x1f8010a8,0x01000200);
                invoke(cpu,a);assertEquals("DMA replacement instruction must execute",1026,cpu.r[2]);
            }finally{cpu.closeNative();}
        }
    }
    @Test public void nativeCpuAndStateContinuationStillMatch()throws Exception{
        android.util.Log.i("CpuCacheEfficiencyTest",NativeCpuVerifier.verify());
        android.util.Log.i("CpuCacheEfficiencyTest",NativeStateVerifier.verify());
    }
}
