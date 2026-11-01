package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class UnlimitedAudioTempoTest {
    @Test public void adaptsToCapacityRatherThanAssumingSixteenTimesSpeed(){
        UnlimitedAudioTempo c=new UnlimitedAudioTempo();c.reset(0,0,0);
        for(int i=1;i<=30;i++)c.update(i*100_000_000L,i*35280L,0);
        assertEquals(7.36,c.update(3_000_000_000L,1_058_400,0),.01);
    }
    @Test public void audioThrottlingDoesNotFeedBackIntoAnEverSlowerTempo(){
        UnlimitedAudioTempo c=new UnlimitedAudioTempo();c.reset(0,0,0);
        // Source runs at 2x but spends 75% of the interval waiting for audio:
        // unblocked machine capacity is 8x, not 2x.
        for(int i=1;i<=30;i++)c.update(i*100_000_000L,i*8820L,i*75_000_000L);
        assertEquals(7.36,c.update(3_000_000_000L,264600,2_250_000_000L),.01);
    }
    @Test public void fallingCapacityLowersTempoBeforeTheReserveIsExhausted(){
        UnlimitedAudioTempo c=new UnlimitedAudioTempo();c.reset(0,0,0);
        for(int i=1;i<=20;i++)c.update(i*100_000_000L,i*35280L,0);
        assertEquals(1.84,c.update(2_100_000_000L,705600+8820,0),.001);
    }
    @Test public void resetAndCrossWindowWaitsRemainBounded(){
        UnlimitedAudioTempo c=new UnlimitedAudioTempo();c.reset(10,20,30);
        assertEquals(2,c.update(11,20,30),0);
        assertTrue(c.update(100_000_010,100000,200_000_000)<=16);
        c.reset(0,0,0);assertEquals(1,c.update(100_000_000,0,0),0);
    }
}
