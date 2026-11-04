package dev.psxlab.core;

import org.junit.Test;
import static org.junit.Assert.*;

public class DisplayWindowTest {
    private static final int PAL=0x100000;
    @Test public void fullPicturesFillTheScreen(){
        assertNull(Gpu.displayWindow(0,0x260,0xc60,0x10,0x100));
        assertNull(Gpu.displayWindow(0,615,3175,16,256));
        assertNull(Gpu.displayWindow(PAL,0x274,0xc74,0x23,0x123));
        // A 224-line game is a full picture with a small border, not a window.
        assertNull(Gpu.displayWindow(0,0x260,0xc60,0x18,0xf8));
    }
    @Test public void aSmallLoadingScreenSitsInItsPlace(){
        // 128x32 in 256 mode, PAL, centred low: the shape of Alundra 2's loading screen.
        float[] w=Gpu.displayWindow(PAL,0x274+1280,0x274+2560,0x23+200,0x23+232);
        assertArrayEquals(new float[]{0.5f,200f/256,0.5f,32f/256},w,1e-6f);
    }
    @Test public void aRangePastTheEdgeMovesInside(){
        float[] w=Gpu.displayWindow(0,0x260+2000,0x260+3000,0x10+220,0x10+260);
        assertEquals(1f-1000f/2560,w[0],1e-6f);assertEquals(1f-40f/240,w[1],1e-6f);
    }
    @Test public void aRangeStartingInTheBlankingAreaStretchesAsBefore(){
        assertNull(Gpu.displayWindow(0,0,40,0,4));
        assertNull(Gpu.displayWindow(PAL,0x200,0x300,0x30,0x40));
    }
    @Test public void emptyRangesAreLeftAlone(){
        assertNull(Gpu.displayWindow(0,0xc60,0x260,0x10,0x100));
        assertNull(Gpu.displayWindow(0,0x260,0xc60,0x100,0x100));
    }
}
