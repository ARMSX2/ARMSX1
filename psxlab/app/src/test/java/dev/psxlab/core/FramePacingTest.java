package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class FramePacingTest {
    @Test public void deadlinesFollowGuestClockRatherThanWakeTime(){
        FramePacer p=new FramePacer(0,1000000000L);
        assertEquals(1016666666L,p.deadline(564480,1010000000L));
        assertEquals(1033333333L,p.deadline(1128960,1018000000L));
    }
    @Test public void longSessionsDoNotOverflowNanosecondConversion(){
        FramePacer p=new FramePacer(0,100);
        assertEquals(3600000000100L,p.deadline(33868800L*3600,3600000000100L));
    }
    @Test public void ordinaryOversleepDoesNotMoveClockEpoch(){
        FramePacer p=new FramePacer(0,0);
        assertEquals(16666666,p.deadline(564480,20000000));
        assertEquals(33333333,p.deadline(1128960,21000000));
    }
    @Test public void hostStallResetsDeadlineInsteadOfReplayingBacklog(){
        FramePacer p=new FramePacer(0,0);
        assertEquals(500000000,p.deadline(564480,500000000));
        assertEquals(516666666,p.deadline(1128960,500000000));
    }
    @Test public void guestResetAndExplicitResetReanchorTime(){
        FramePacer p=new FramePacer(1000,0);assertEquals(200,p.deadline(0,200));
        p.reset(33868800,500);assertEquals(1000000500,p.deadline(67737600,500));
    }
    @Test public void mailboxKeepsOnlyLatestCompletedFrame(){
        LatestFrame<Integer> q=new LatestFrame<>();for(int i=0;i<1000;i++)q.offer(i);
        assertEquals(999,(int)q.take());assertEquals(999,q.replaced());assertNull(q.take());
    }
    @Test public void consumingFrameDoesNotCountAsReplacement(){
        LatestFrame<Integer> q=new LatestFrame<>();q.offer(1);assertEquals(1,(int)q.take());q.offer(2);assertEquals(2,(int)q.take());assertEquals(0,q.replaced());
    }
    @Test public void stopClearsPendingFrame(){
        LatestFrame<Integer> q=new LatestFrame<>();q.offer(1);q.clear();assertNull(q.take());q.offer(2);assertEquals(2,(int)q.take());
    }
    @Test public void guestVblankCounterAdvancesAtIrqEdge(){
        try(Gpu g=new Gpu(false)){Bus b=new Bus(g);g.tickVideo(b,1);assertEquals(0,g.vblanks);g.tickVideo(b,570000);assertEquals(1,g.vblanks);g.tickVideo(b,570000);assertEquals(2,g.vblanks);}
    }
}
