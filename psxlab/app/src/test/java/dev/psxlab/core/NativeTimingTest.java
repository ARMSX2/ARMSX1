package dev.psxlab.core;
import java.util.*;
import org.junit.Test;
import static org.junit.Assert.*;

public class NativeTimingTest {
    @Test public void snapshotsKeepCpuTimeWaitTimeContextAndCounterDeltas(){
        NativeTiming timing=new NativeTiming();long[] raw=new long[19*8];
        raw[0]=2;raw[1]=200;raw[2]=120;raw[3]=150;raw[4]=100;raw[5]=1234;raw[6]=42;raw[7]=0x80010000L;
        Map<String,Object> first=timing.sample(raw);assertEquals(true,first.get("first_sample"));
        Map<?,?> row=(Map<?,?>)((List<?>)first.get("stages")).get(0);
        assertEquals("cpu.compile",row.get("path"));assertEquals(80L,row.get("window_non_cpu_ns"));assertEquals("80010000",row.get("context_pc"));assertEquals(42L,row.get("tid"));assertTrue(row.get("source").toString().contains("compileArm64"));
        raw[0]=3;raw[1]=260;raw[2]=170;Map<String,Object> next=timing.sample(raw);row=(Map<?,?>)((List<?>)next.get("stages")).get(0);
        assertEquals(false,next.get("first_sample"));assertEquals(1L,row.get("window_calls"));assertEquals(60L,row.get("window_wall_ns"));assertEquals(10L,row.get("window_non_cpu_ns"));
        assertTrue(next.get("interpretation").toString().contains("not proof"));
        timing.reset();assertEquals(true,timing.sample(raw).get("first_sample"));
    }
    @Test public void unsupportedNativeTelemetryIsExplicit(){assertEquals(false,new NativeTiming().sample(new long[0]).get("available"));}
}
