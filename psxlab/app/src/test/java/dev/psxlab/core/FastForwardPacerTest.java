package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class FastForwardPacerTest {
    @Test public void normalPresentationCannotBuildAQueueBehindAStalledDisplay(){
        FastForwardPacer p=new FastForwardPacer();assertTrue(p.presentNormally());assertTrue(p.presentNormally());
        for(int field=0;field<120;field++){p.reset();assertFalse(p.presentNormally());}
        p.presentationComplete();assertTrue(p.presentNormally());assertFalse(p.present(1_000_000_000,120));
        p.presentationComplete();p.presentationComplete();assertTrue(p.present(1_000_000_000,120));
        p.presentationComplete();p.presentationComplete();assertTrue(p.present(2_000_000_000,120));
    }
    @Test public void aPresentFinishingJustAfterTheNextFieldDoesNotDropThatField(){
        // Digimon World 3 on a 120 Hz panel: the previous present was still completing when the next
        // field arrived, so one field in ~120 was never shown and idle battle animations hitched.
        FastForwardPacer p=new FastForwardPacer();int shown=0;boolean outstanding=false,finishesLate=false;
        for(int field=0;field<600;field++){
            if(outstanding&&!finishesLate)p.presentationComplete();
            boolean accepted=p.presentNormally();if(accepted)shown++;
            if(outstanding&&finishesLate)p.presentationComplete();
            outstanding=accepted;finishesLate=field%120==60;
        }
        assertEquals("every field reaches the display",600,shown);
    }
    @Test public void allMenuMultipliersAndSupportedCustomSpeedsScaleDeadlines(){
        FastForwardPacer p=new FastForwardPacer();
        for(float speed:new float[]{1,1.5f,2,3,4,8,16}){
            assertEquals(0,p.waitNanos(0,100,speed));
            assertEquals((long)(1e9/speed),p.waitNanos(33868800,100,speed));
        }
    }
    @Test public void finiteSpeedUsesGuestTimeEvenWhenAudioIsMuted(){
        FastForwardPacer p=new FastForwardPacer();assertEquals(0,p.waitNanos(0,0,4));
        assertEquals(250_000_000L,p.waitNanos(33868800,0,4));
        assertEquals(100_000_000L,p.waitNanos(33868800,150_000_000,4));
        assertEquals(900_000_000_000L,p.waitNanos(33868800L*3600,0,4));
    }
    @Test public void unlimitedNeverSleepsAndSpeedChangesReanchor(){
        FastForwardPacer p=new FastForwardPacer();p.waitNanos(0,0,2);
        assertEquals(0,p.waitNanos(33868800,1,0));assertEquals(0,p.waitNanos(33868800L*10,2,0));
        assertEquals(0,p.waitNanos(33868800L*10,3,4));assertEquals(250_000_000,p.waitNanos(33868800L*11,3,4));
        assertEquals(0,p.waitNanos(0,4,4));p.reset();assertEquals(0,p.waitNanos(33868800,5,4));
    }
    @Test public void hostStallsDoNotCreateCatchUpBursts(){
        FastForwardPacer p=new FastForwardPacer();p.waitNanos(0,0,4);
        assertEquals(0,p.waitNanos(33868800,1_000_000_000,4));
        assertEquals(250_000_000,p.waitNanos(67737600,1_000_000_000,4));
    }
    @Test public void fourTimesGuestRateSubmitsAtDisplayRateWithoutDroppingGuestWork(){
        FastForwardPacer p=new FastForwardPacer();int pictures=0;
        for(int frame=0;frame<240;frame++)if(p.present((long)(frame*1e9/240),60)){pictures++;p.presentationComplete();}
        assertEquals(60,pictures);p.reset();assertTrue(p.present(0,60));p.presentationComplete();
        assertTrue(p.present(1_000_000_000,60));assertFalse(p.present(1_000_000_001,60));
    }
    @Test public void presentationFollowsHighRefreshAndRejectsInvalidMetadata(){
        FastForwardPacer p=new FastForwardPacer();assertTrue(p.present(0,120));p.presentationComplete();assertFalse(p.present(8_333_334,120));assertTrue(p.present(16_666_667,120));
        p.presentationComplete();p.reset();assertTrue(p.present(0,Float.NaN));assertFalse(p.present(8_333_334,Float.NaN));
    }
    @Test public void slowFifoCannotAccumulatePresentsEvenWhenRefreshMetadataIsWrong(){
        FastForwardPacer p=new FastForwardPacer();assertTrue(p.present(0,120));
        assertFalse(p.present(100_000_000,120));p.reset();assertFalse(p.present(200_000_000,120));
        p.presentationComplete();assertTrue(p.present(200_000_001,120));
    }
}
