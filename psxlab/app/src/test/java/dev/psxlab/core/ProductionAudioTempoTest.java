package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class ProductionAudioTempoTest {
    @Test public void requestedFourTimesTracksSlowerProduction(){
        ProductionAudioTempo c=new ProductionAudioTempo();c.reset(0,0,0,4);
        assertEquals(1.225,c.update(100_000_000,5513,0),.001);
    }
    @Test public void everyFiniteSpeedRemainsAtItsCeilingWhenProductionKeepsUp(){
        for(float requested:new float[]{1.5f,2,3,4,8,16}){
            ProductionAudioTempo c=new ProductionAudioTempo();c.reset(0,0,0,requested);
            for(int i=1;i<=50;i++)assertEquals(requested,c.update(i*100_000_000L,Math.round(i*4410*requested),0),.0001);
        }
    }
    @Test public void blockedProducerIsNotMistakenForLimitedCpuCapacity(){
        ProductionAudioTempo c=new ProductionAudioTempo();c.reset(0,0,0,4);
        assertEquals(4,c.update(100_000_000,4410,75_000_000),0);
    }
    @Test public void pauseAndModeChangesResetTheMeasurementWindow(){
        ProductionAudioTempo c=new ProductionAudioTempo();c.reset(0,0,0,4);c.update(100_000_000,2205,0);
        c.reset(5_000_000_000L,2205,0,2);assertEquals(2,c.update(5_010_000_000L,3087,0),0);
        assertEquals(2,c.update(5_100_000_000L,11025,0),.001);
    }
    @Test public void recoveryRisesGraduallyAndNeverExceedsTheSelection(){
        ProductionAudioTempo c=new ProductionAudioTempo();c.reset(0,0,0,3);
        float low=c.update(100_000_000,4410,0),next=c.update(200_000_000,17640,0);
        assertTrue(next>low);assertTrue(next<3);
    }
}
