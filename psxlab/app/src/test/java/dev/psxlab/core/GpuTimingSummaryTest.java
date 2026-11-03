package dev.psxlab.core;

import dev.psxlab.ArmSxBackend;
import org.junit.Test;
import java.lang.reflect.Method;
import static org.junit.Assert.*;

public class GpuTimingSummaryTest {
    private static String summary(long[] before,long[] now)throws Exception{
        Method method=ArmSxBackend.class.getDeclaredMethod("gpuSummary",long[].class,long[].class,long.class);
        method.setAccessible(true);return (String)method.invoke(null,before,now,500_000_000L);
    }
    @Test public void disabledQueriesAreNotReportedAsAnIdleGpu()throws Exception{
        long[] value=new long[17];value[0]=2;
        assertEquals("gpu: timing disabled (logging off)",summary(new long[17],value));
    }
    @Test public void actualDrawCallsAreReportedSeparatelyFromNestedTimingSpans()throws Exception{
        long[] before=new long[17],now=new long[17];now[0]=1;now[1]=100_000_000;now[6]=7;now[15]=2;now[16]=3;
        String text=summary(before,now);assertTrue(text,text.contains("draw 200"));
        assertTrue(text,text.contains("draw spans 14/s"));assertTrue(text,text.contains("draws/s native 4 HD 6"));
    }
    @Test public void hdBatchEndsAreReportedByReason()throws Exception{
        long[] before=new long[21],now=new long[21];now[0]=1;now[17]=5;now[18]=10;now[19]=1;now[20]=2;
        String text=summary(before,now);assertTrue(text,text.contains("HD batch ends/s texture 10 transfer 20 full 2 other 4"));
        assertFalse(summary(new long[17],now).contains("HD batch ends"));
    }
    @Test public void olderTimerSnapshotsStillFormatWithoutDrawCallCounters()throws Exception{
        long[] value=new long[15];value[0]=1;assertTrue(summary(new long[15],value).contains("draw spans 0/s"));
    }
}
