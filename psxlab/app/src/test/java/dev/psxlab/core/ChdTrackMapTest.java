package dev.psxlab.core;

import static org.junit.Assert.*;
import org.junit.Test;

public class ChdTrackMapTest {
    // chdman from a BIN/CUE: data track, then audio tracks whose 2-second pregaps are stored ("V").
    private static final int[] TYPICAL={0,1001,0,0,0, 1,600,150,1,0, 1,402,150,1,0};
    @Test public void dataTrackStartsAtLbaZero(){
        ChdTrackMap m=new ChdTrackMap(TYPICAL);
        assertEquals(3,m.lastTrack());assertEquals(0,m.trackStart(1));assertEquals(0,m.chdFrame(0));assertFalse(m.audioTrack(0));
    }
    @Test public void storedPregapsPrecedeIndex1AndTracksArePadded(){
        ChdTrackMap m=new ChdTrackMap(TYPICAL);
        // Track 1 has 1001 frames, padded to 1004 in the CHD.
        assertEquals(1001+150,m.trackStart(2));
        assertEquals(1004,m.chdFrame(1001));assertEquals(1004+150,m.chdFrame(m.trackStart(2)));
        assertTrue(m.audioTrack(1001));assertEquals(2,m.trackNumber(1001));assertEquals(0,m.trackIndex(1001));assertEquals(1,m.trackIndex(1151));
        assertEquals(150,m.trackRelative(1001));assertEquals(0,m.trackRelative(1151));
        // Track 2 has 600 frames (a multiple of four): track 3's data follows directly.
        assertEquals(1001+600+150,m.trackStart(3));assertEquals(1004+600+150,m.chdFrame(m.trackStart(3)));
        assertEquals(1001+600+402,m.sectorCount());assertEquals(m.sectorCount(),m.trackStart(0));
    }
    @Test public void unstoredPregapIsSilence(){
        ChdTrackMap m=new ChdTrackMap(new int[]{0,100,0,0,0, 1,40,150,0,0});
        assertEquals(250,m.trackStart(2));
        assertEquals(-1,m.chdFrame(100));assertEquals(-1,m.chdFrame(249));assertEquals(100,m.chdFrame(250));
        assertEquals(290,m.sectorCount());
    }
    @Test public void postgapsAreSilenceAfterTheTrack(){
        ChdTrackMap m=new ChdTrackMap(new int[]{0,100,0,0,150, 1,40,0,0,0});
        assertEquals(250,m.trackStart(2));assertEquals(-1,m.chdFrame(120));assertEquals(1,m.trackNumber(120));assertEquals(100,m.chdFrame(250));
    }
    @Test public void storedTrackOnePregapStillPutsIndex1AtZero(){
        ChdTrackMap m=new ChdTrackMap(new int[]{0,1150,150,1,0});
        assertEquals(0,m.trackStart(1));assertEquals(150,m.chdFrame(0));assertEquals(1000,m.sectorCount());
    }
    @Test(expected=IllegalArgumentException.class) public void rejectsBadTables(){new ChdTrackMap(new int[]{0,0,0,0,0});}
    @Test public void describesTheLayout(){assertEquals("1 data @0, 2 audio @1151, 3 audio @1751",new ChdTrackMap(TYPICAL).describe());}
}
