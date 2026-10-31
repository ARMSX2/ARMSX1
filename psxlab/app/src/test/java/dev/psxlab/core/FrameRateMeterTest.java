package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class FrameRateMeterTest {
    @Test public void countsIntervalsBetweenCompletedFields(){
        FrameRateMeter meter=new FrameRateMeter();float result=Float.NaN;
        for(int i=0;i<=30;i++)result=meter.completed(i*16_715_113L);
        assertEquals(59.8261f,result,0.001f);
    }
    @Test public void anActualStallLowersTheMeasuredRate(){
        FrameRateMeter meter=new FrameRateMeter();meter.completed(0);
        for(int i=1;i<30;i++)assertTrue(Float.isNaN(meter.completed(i*16_666_667L)));
        assertEquals(30*1e9/(30*16_666_667L+50_000_000L),meter.completed(30*16_666_667L+50_000_000L),0.001);
    }
    @Test public void resetExcludesTimeSpentPaused(){
        FrameRateMeter meter=new FrameRateMeter();meter.completed(0);meter.completed(100_000_000L);meter.reset();
        float rate=Float.NaN;for(int i=0;i<=30;i++)rate=meter.completed(20_000_000_000L+i*16_715_113L);
        assertEquals(59.8261f,rate,0.001f);
    }
    @Test public void fasterRatesAreReportedWithoutClamping(){
        FrameRateMeter meter=new FrameRateMeter();float rate=Float.NaN;for(int i=0;i<=100;i++)rate=meter.completed(i*5_000_000L);
        assertEquals(200,rate,0.001f);
    }
}
