package dev.psxlab.core;

import static org.junit.Assert.*;
import org.junit.Test;

public class SessionTimelineTest {
    private static final long MS=1_000_000L,FRAME=16_683_350L;
    @Test public void onTimeFramesAreNotLate(){
        SessionTimeline t=new SessionTimeline();t.start(0);
        for(int i=1;i<=300;i++)assertNull(t.frame(i*FRAME,FRAME,5*MS,8*MS,8*MS));
        t.row(300*FRAME,"x");
        assertTrue(t.rows().get(0),t.rows().get(0).contains("late 0"));
        assertTrue(t.incidents().isEmpty());
    }
    @Test public void stallSplitsEmulationOtherWorkAndOffCpuTime(){
        SessionTimeline t=new SessionTimeline();t.start(0);
        t.frame(FRAME,FRAME,0,0,0);
        String stall=t.frame(FRAME+120*MS,FRAME,90*MS,100*MS,0);
        assertNotNull(stall);
        assertTrue(stall,stall.contains("late frame 120 ms")&&stall.contains("emulation 90 ms")&&stall.contains("other frame work 10 ms")&&stall.contains("off-CPU/descheduled 20 ms"));
    }
    @Test public void shortHitchesCountWithoutIncidents(){
        SessionTimeline t=new SessionTimeline();t.start(0);
        t.frame(FRAME,FRAME,0,0,0);
        assertNull(t.frame(FRAME+40*MS,FRAME,0,0,0));
        t.row(FRAME+40*MS,"x");
        assertTrue(t.rows().get(0),t.rows().get(0).contains("late 1 worst 40 ms"));
    }
    @Test public void resumeAfterPauseIsNotAStall(){
        SessionTimeline t=new SessionTimeline();t.start(0);
        t.frame(FRAME,FRAME,0,0,0);t.resume(10_000*MS);
        assertNull(t.frame(10_000*MS+FRAME,FRAME,0,0,0));
    }
    @Test public void unpacedFramesAreNeverLate(){
        SessionTimeline t=new SessionTimeline();t.start(0);
        t.frame(FRAME,0,0,0,0);assertNull(t.frame(FRAME+500*MS,0,0,0,0));
    }
    @Test public void incidentsCarrySessionTimeAndOldestDropFirst(){
        SessionTimeline t=new SessionTimeline();t.start(1_000*MS);
        t.incident(3_500*MS,"fast-forward on");
        assertEquals("+2.5s fast-forward on",t.incidents().get(0));
        for(int i=0;i<1100;i++)t.incident(4_000*MS,"e"+i);
        assertEquals(1024,t.incidents().size());assertEquals(77,t.droppedIncidents());
        assertEquals("+3.0s e1099",t.incidents().get(1023));
    }
    @Test public void rowsAreDueEveryInterval(){
        SessionTimeline t=new SessionTimeline();assertFalse(t.due(10_000*MS));
        t.start(0);assertFalse(t.due(4_999*MS));assertTrue(t.due(5_000*MS));
        t.row(5_000*MS,"x");assertFalse(t.due(9_999*MS));
    }
}
