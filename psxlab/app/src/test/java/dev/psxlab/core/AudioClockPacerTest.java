package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class AudioClockPacerTest {
    @Test public void emptyAndLowBuffersNeverDelayProducer(){assertEquals(0,AudioClockPacer.waitNanos(0));assertEquals(0,AudioClockPacer.waitNanos(3072));}
    @Test public void targetAndSmallJitterStayInDeadBand(){assertEquals(0,AudioClockPacer.waitNanos(4096));assertEquals(0,AudioClockPacer.waitNanos(4224));}
    @Test public void excessAudioUsesShortInterruptibleWait(){assertTrue(AudioClockPacer.waitNanos(4250)>3000000);assertTrue(AudioClockPacer.waitNanos(4250)<4000000);}
    @Test public void largeBacklogCannotCauseLongSleepOrOverflow(){assertEquals(4000000,AudioClockPacer.waitNanos(5000));assertEquals(4000000,AudioClockPacer.waitNanos(Long.MAX_VALUE));}
    @Test public void drainingBacklogRemovesThrottleImmediately(){assertEquals(4000000,AudioClockPacer.waitNanos(8000));assertEquals(0,AudioClockPacer.waitNanos(4000));}
}
