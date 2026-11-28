package dev.psxlab.core;

import static org.junit.Assert.*;

import org.junit.Test;

public class FrameGenerationTest {
    private static final int NTSC=0,PAL=0x100000;
    private static final float[] NONE=new float[0];

    /** One field: the display start is buffer A or B, as a double-buffered game flips between them. */
    private static FrameGeneration.Plan field(FrameGeneration g,boolean bufferB,int multiple){return g.next(NTSC,0,bufferB?256:0,multiple);}

    @Test public void sixtyFpsOnSixtyHertzIsShownAsUsual(){
        FrameGeneration g=new FrameGeneration();
        for(int i=0;i<10;i++){
            FrameGeneration.Plan p=field(g,(i&1)!=0,1);
            assertTrue(p.capture());assertFalse(p.estimate());assertArrayEquals(NONE,p.generated(),0);assertTrue(p.real());
        }
    }
    @Test public void thirtyFpsGetsOneInBetweenPictureOnTheFieldTheNewFrameArrives(){
        FrameGeneration g=new FrameGeneration();
        // Fields: A A B B A A ... The first frame and first gap teach the cadence.
        FrameGeneration.Plan first=field(g,false,1);
        assertTrue(first.capture());assertTrue(first.reset());assertTrue(first.real());
        assertTrue(field(g,false,1).ordinary());
        for(int frame=1;frame<6;frame++){
            boolean b=(frame&1)!=0;
            FrameGeneration.Plan arrive=field(g,b,1);
            assertTrue(arrive.capture());assertTrue(arrive.estimate());assertFalse(arrive.reset());
            assertArrayEquals(new float[]{0.5f},arrive.generated(),0);
            assertFalse("the newest picture waits one field",arrive.real());
            FrameGeneration.Plan hold=field(g,b,1);
            assertTrue(hold.ordinary());assertTrue(hold.real());
        }
        assertEquals(2,g.cadence());
    }
    @Test public void twentyFpsGetsTwoInBetweenPictures(){
        FrameGeneration g=new FrameGeneration();
        field(g,false,1);field(g,false,1);field(g,false,1);
        FrameGeneration.Plan arrive=field(g,true,1);
        assertArrayEquals(new float[]{1f/3},arrive.generated(),1e-6f);
        assertArrayEquals(new float[]{2f/3},field(g,true,1).generated(),1e-6f);
        assertTrue(field(g,true,1).ordinary());
        assertEquals(3,g.cadence());
    }
    @Test public void sixtyFpsOnOneHundredTwentyHertzGetsOnePictureBetweenEveryField(){
        FrameGeneration g=new FrameGeneration();
        field(g,false,2);
        for(int i=1;i<8;i++){
            FrameGeneration.Plan p=field(g,(i&1)!=0,2);
            assertTrue(p.capture());assertTrue(p.estimate());
            assertArrayEquals(new float[]{0.5f},p.generated(),0);
            assertTrue("then the newest picture on the second refresh",p.real());
        }
    }
    @Test public void thirtyFpsOnOneHundredTwentyHertzFillsFourRefreshes(){
        FrameGeneration g=new FrameGeneration();
        field(g,false,2);field(g,false,2);
        FrameGeneration.Plan arrive=field(g,true,2);
        assertArrayEquals(new float[]{0.25f,0.5f},arrive.generated(),0);assertFalse(arrive.real());
        FrameGeneration.Plan hold=field(g,true,2);
        assertFalse(hold.capture());assertArrayEquals(new float[]{0.75f},hold.generated(),0);assertTrue(hold.real());
    }
    @Test public void aVaryingFrameRateUsesTheShortestRecentGap(){
        FrameGeneration g=new FrameGeneration();
        boolean b=false;
        for(int gap:new int[]{2,3,2,3,3}){b=!b;field(g,b,1);for(int i=1;i<gap;i++)field(g,b,1);}
        assertEquals(2,g.cadence());
        // A frame that comes later than expected repeats the newest picture instead of running out of in-betweens.
        FrameGeneration.Plan arrive=field(g,!b,1);
        assertArrayEquals(new float[]{0.5f},arrive.generated(),0);
        assertTrue(field(g,!b,1).ordinary());
        assertTrue(field(g,!b,1).ordinary());
    }
    @Test public void longPausesAreNotTakenForAFrameRate(){
        FrameGeneration g=new FrameGeneration();
        field(g,false,1);
        for(int i=0;i<9;i++)field(g,false,1);
        FrameGeneration.Plan p=field(g,true,1);
        assertTrue(p.capture());assertEquals(1,g.cadence());assertArrayEquals(NONE,p.generated(),0);assertTrue(p.real());
    }
    @Test public void aGameThatNeverFlipsIsShownUnchanged(){
        FrameGeneration g=new FrameGeneration();
        assertTrue(field(g,false,1).capture());
        for(int i=0;i<20;i++){FrameGeneration.Plan p=field(g,false,2);assertTrue(p.ordinary());assertTrue(p.real());}
    }
    @Test public void aModeChangeStartsTheHistoryAgain(){
        FrameGeneration g=new FrameGeneration();
        field(g,false,1);field(g,false,1);field(g,true,1);field(g,true,1);
        assertEquals(2,g.cadence());
        FrameGeneration.Plan pal=g.next(PAL,0,0,1);
        assertTrue(pal.capture());assertTrue(pal.reset());assertFalse(pal.estimate());assertArrayEquals(NONE,pal.generated(),0);
        assertEquals("gaps from the old mode are forgotten",1,g.cadence());
    }
    @Test public void resetForgetsTheHistory(){
        FrameGeneration g=new FrameGeneration();
        field(g,false,1);field(g,false,1);field(g,true,1);
        g.reset();
        FrameGeneration.Plan p=field(g,true,1);
        assertTrue(p.capture());assertTrue(p.reset());assertFalse(p.estimate());assertTrue(p.real());
    }
    @Test public void displayMultipleNeedsAWholeMultipleOfTheGameRate(){
        assertEquals(2,FrameGeneration.displayMultiple(120f,false));
        assertEquals(2,FrameGeneration.displayMultiple(119.88f,false));
        assertEquals(2,FrameGeneration.displayMultiple(100f,true));
        assertEquals(3,FrameGeneration.displayMultiple(180f,false));
        assertEquals(1,FrameGeneration.displayMultiple(60f,false));
        assertEquals(1,FrameGeneration.displayMultiple(90f,false));
        assertEquals(1,FrameGeneration.displayMultiple(144f,false));
        assertEquals(1,FrameGeneration.displayMultiple(120f,true));
        assertEquals(1,FrameGeneration.displayMultiple(Float.NaN,false));
    }
}
