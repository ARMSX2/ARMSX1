package dev.psxlab.core;

import static org.junit.Assert.*;
import org.junit.Test;

public class VramDirtyRegionTest {
    @Test public void newRegionForcesTheFirstReadToWait(){assertTrue(new VramDirtyRegion().intersectsWrapped(900,500,1,1));}
    @Test public void cleanRegionNeverWaits(){VramDirtyRegion r=new VramDirtyRegion();r.clean();assertFalse(r.intersectsWrapped(0,0,1024,512));}
    @Test public void drawingAreaBlocksOnlyOverlappingReads(){
        VramDirtyRegion r=new VramDirtyRegion();r.clean();
        r.addDrawingArea(0|(0<<10),319|(239<<10)); // 320x240 back buffer at the origin.
        assertTrue(r.intersectsWrapped(319,239,1,1));
        assertFalse(r.intersectsWrapped(320,0,64,64));
        assertFalse(r.intersectsWrapped(0,240,320,16));
    }
    @Test public void inverseDrawingAreaDrawsNothing(){VramDirtyRegion r=new VramDirtyRegion();r.clean();r.addDrawingArea(100|(100<<10),50|(50<<10));assertTrue(r.isClean());}
    @Test public void wrappedWritesDirtyBothEdges(){
        VramDirtyRegion r=new VramDirtyRegion();r.clean();r.addWrapped(1000,0,48,8);
        assertTrue(r.intersectsWrapped(1010,0,4,4));assertTrue(r.intersectsWrapped(5,0,4,4));
    }
    @Test public void wrappedReadsSeeEveryCorner(){
        VramDirtyRegion r=new VramDirtyRegion();r.clean();r.add(0,0,3,3);
        assertTrue(r.intersectsWrapped(1020,508,8,8));
        r.clean();r.add(1020,508,1023,511);assertTrue(r.intersectsWrapped(1020,508,8,8));
        r.clean();r.add(500,200,510,210);assertFalse(r.intersectsWrapped(1020,508,8,8));
    }
    @Test public void unionCoversEveryWrite(){
        VramDirtyRegion r=new VramDirtyRegion();r.clean();r.addWrapped(10,10,5,5);r.addWrapped(600,300,10,10);
        assertEquals("10,10-609,309",r.describe());assertTrue(r.intersectsWrapped(300,150,1,1));
    }
}
