package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class AudioRateControllerTest {
    @Test public void healthyBufferIgnoresNoisyCpuSpeed(){AudioRateController c=new AudioRateController();for(int i=0;i<100;i++)assertEquals(1,c.update(4096,i%2==0?0.6:1.2,i*50000000L),0);}
    @Test public void rateDoesNotChangeBetweenControlIntervals(){AudioRateController c=new AudioRateController();float r=c.update(0,.5,0);for(int i=1;i<50;i++)assertEquals(r,c.update(0,.5,i*1000000L),0);}
    @Test public void depletedOutputRespondsEvenWithFullCpuSpeed(){AudioRateController c=new AudioRateController();float r=1;for(int i=0;i<20;i++)r=c.update(0,1,i*50000000L);assertTrue(r<.8);assertTrue(r>=.5);}
    @Test public void highBufferBoundsCatchup(){AudioRateController c=new AudioRateController();float r=1;for(int i=0;i<100;i++)r=c.update(10000,1,i*50000000L);assertEquals(1.05,r,.00001);}
    @Test public void refillRestoresNominalRateWithoutPermanentSlowdown(){AudioRateController c=new AudioRateController();for(int i=0;i<50;i++)c.update(0,.5,i*50000000L);c.recovered();float r=0;for(int i=0;i<110;i++)r=c.update(4096,1,3000000000L+i*50000000L);assertEquals(1,r,.00001);}
    @Test public void invalidEstimateCannotProduceInvalidPlaybackRate(){AudioRateController c=new AudioRateController();for(int i=0;i<100;i++){float r=c.update(0,Double.NaN,i*50000000L);assertTrue(Float.isFinite(r));assertTrue(r>=.5 && r<=1.05);}}
    @Test public void briefOccupancyDipDoesNotRetune(){AudioRateController c=new AudioRateController();assertEquals(1,c.update(3072,.7,0),0);assertEquals(1,c.update(4096,1,50000000),0);}
    @Test public void repeatedStallsDoNotDrainOutputAtReportedFullSpeed(){
        AudioRateController c=new AudioRateController();double queued=4096;float tempo=1;
        // Every 500 ms the producer loses 40 ms. CPU's displayed average is stale.
        // Feed the previous tempo's output, as a real DSP has processing latency.
        for(int i=0;i<1200;i++){
            queued+=(i%10==9?441:2205)/tempo-2205;
            assertTrue("Output starved at interval "+i,queued>0);
            tempo=c.update((long)queued,1,i*50000000L);
        }
        assertTrue(queued<8192);assertTrue(tempo<1);
    }
    @Test public void soundTouchOutputBurstsDoNotModulateHealthyAudio(){
        AudioRateController c=new AudioRateController();
        // A 40 ms DSP batch causes a healthy ready-output queue to sawtooth.
        // Its lower edge is not a sustained production deficit.
        for(int i=0;i<400;i++)assertEquals(1,c.update(i%2==0?2048:6144,1,i*50000000L),.00001);
    }
}
