package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;

public class AudioTimelineTest {
    @Test public void nominalOutputTracksGuestSamplesIncludingSilence(){AudioTimeline t=new AudioTimeline();t.feed(44100,1);assertEquals(0,t.sourceAt(0));assertEquals(12345,t.sourceAt(12345));assertEquals(44100,t.sourceAt(44100));}
    @Test public void stretchedOutputDoesNotAccumulateClockDrift(){AudioTimeline t=new AudioTimeline();for(int i=0;i<100;i++)t.feed(441,.8f);assertEquals(22050,t.sourceAt(27563),1);assertEquals(44100,t.sourceAt(55125),1);}
    @Test public void tempoChangesKeepEarlierQueuedAudioOnItsOriginalClock(){AudioTimeline t=new AudioTimeline();t.feed(4410,1);t.feed(4410,.5f);t.feed(4410,1);assertEquals(4000,t.sourceAt(4000));assertEquals(6615,t.sourceAt(8820));assertEquals(10000,t.sourceAt(14410));}
    @Test public void emptyAndUnwrittenOutputCannotInventFutureGuestAudio(){AudioTimeline t=new AudioTimeline();assertEquals(0,t.sourceAt(100));t.feed(100,1);assertEquals(0,t.sourceAt(-1));assertEquals(100,t.sourceAt(10000));}
    @Test public void invalidTempoIsRejected(){AudioTimeline t=new AudioTimeline();assertThrows(IllegalArgumentException.class,()->t.feed(10,0));assertThrows(IllegalArgumentException.class,()->t.feed(10,Float.NaN));}
}
