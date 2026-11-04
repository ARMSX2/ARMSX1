package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

/** The native core's cached registers (cpu_jni.cpp CpuCore::latched) must match what a full device dispatch reads. */
public class RegisterLatchTest {
    /** The native formula, kept in step with CpuCore::latched. */
    static int predict(int latch,int elapsed){int advance=(latch>>16)&15,count=latch&65535;return (advance==1?count+elapsed:advance==8?count+(((latch>>20)&7)+elapsed)/8:count)&65535;}
    @Test public void timerCountsMatchTheTimerModelUntilTheNextEvent(){
        int[] modes={0,8,0x10,0x18,0x100,0x108,0x200,0x208,0x300,1,3,5,7,0x109};
        int checked=0;
        for(int i=0;i<3;i++)for(int mode:modes)for(int target:new int[]{0,7,300,65535})for(int before:new int[]{0,5,123})for(int pending:new int[]{0,3,40}){
            try(Gpu g=new Gpu(false)){
                Bus b=new Bus(g);int base=0x1f801100+i*16;b.write16(base+8,target);b.write16(base+4,mode);b.timers.cpuCycles(before);
                int window=b.timers.clocksToEvent()-pending;int latch=b.timers.nativeLatch(i,pending);
                if(latch<0){assertEquals("only the dot-clock timer is unpredictable",0,i);continue;}
                if(window<=0)continue;
                b.timers.cpuCycles(pending);
                for(int e=0;e<Math.min(window,2000);e++){
                    assertEquals("timer "+i+" mode "+Integer.toHexString(mode)+" target "+target+" at +"+e,b.read16(base),predict(latch,e));
                    b.timers.cpuCycles(1);checked++;
                }
            }
        }
        assertTrue(checked>10000);
    }
    @Test public void everyCachedRegisterHoldsAcrossItsWholeWindow(){
        try(Gpu g=new Gpu(false)){
            Bus b=new Bus(g);b.setEventScheduling(true);java.nio.ByteBuffer m=b.enableNativeMemory();
            b.write16(0x1f801114,0x100);b.write16(0x1f801124,0x200);
            ControllerTest.select(b);b.write8(0x1f801040,1);
            java.util.Random random=new java.util.Random(7);int windows=0;
            for(int step=0;step<3000;step++){
                if(step%300==0)b.write8(0x1f801040,0x42);
                b.nativeClock(1+random.nextInt(64),0);int window=m.getInt(Bus.LATCH_OFFSET);if(window<=1)continue;
                int[] latch=new int[6];for(int i=0;i<6;i++)latch[i]=m.getInt(Bus.LATCH_OFFSET+i*4);
                int e=random.nextBoolean()?window-1:random.nextInt(window);
                b.nativeFinish(e,0);b.flushClocks();windows++;
                assertEquals(b.sio0.read(4,4),latch[1]);assertEquals(b.gpu.status(),latch[2]);
                for(int i=0;i<3;i++)if(latch[3+i]>=0)assertEquals("timer "+i+" at +"+e+" of "+window,b.timers.read(0x1f801100+i*16),predict(latch[3+i],e));
            }
            assertTrue(windows>1000);
        }
    }
}
