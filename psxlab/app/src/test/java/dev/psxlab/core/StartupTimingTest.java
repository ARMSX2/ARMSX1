package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class StartupTimingTest {
    @Test public void boundedHistoryPreservesFirstFramesAndTheirQueueDelay(){
        StartupTiming t=new StartupTiming(2);t.record(100,1000,1200);t.record(200,2000,2900);t.record(300,3000,3100);
        assertEquals(2,t.size());assertEquals("guest_clocks\trequest_ns\tgpu_ready_ns\n100\t1000\t1200\n200\t2000\t2900\n",t.tsv());
    }
    @Test public void restartDoesNotMixPreviousBootSamples(){StartupTiming t=new StartupTiming(2);t.record(999,10,20);t.reset();t.record(1,100,110);assertEquals(1,t.size());assertFalse(t.tsv().contains("999"));}
}
