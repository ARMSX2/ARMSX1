package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class PcmWindowTest {
    @Test public void wrappedHistoryPreservesStereoAndAbsoluteSamplePositions(){PcmWindow p=new PcmWindow(3);p.append(new short[]{1,2,3,4},4);p.append(new short[]{5,6,7,8},4);PcmWindow.Snapshot s=p.snapshot();assertArrayEquals(new short[]{3,4,5,6,7,8},s.samples());assertEquals(1,s.firstFrame());assertEquals(4,s.endFrame());}
    @Test public void oversizedInputKeepsOnlyTheLastFrames(){PcmWindow p=new PcmWindow(2);p.append(new short[]{1,2,3,4,5,6,7,8},8);assertArrayEquals(new short[]{5,6,7,8},p.snapshot().samples());assertEquals(2,p.snapshot().firstFrame());assertEquals(4,p.snapshot().endFrame());}
    @Test public void savedSnapshotIsNotOverwrittenByTheLiveProducer(){PcmWindow p=new PcmWindow(2);p.append(new short[]{1,2,3,4},4);short[] saved=p.snapshot().samples();p.append(new short[]{5,6,7,8},4);assertArrayEquals(new short[]{1,2,3,4},saved);}
}
