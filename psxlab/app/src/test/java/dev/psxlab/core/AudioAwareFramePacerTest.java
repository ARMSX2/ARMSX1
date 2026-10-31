package dev.psxlab.core;
import org.junit.Test;
import static org.junit.Assert.*;
public class AudioAwareFramePacerTest {
    @Test public void startupFillsWithoutHoldingBackCpu(){AudioAwareFramePacer p=new AudioAwareFramePacer(0,0);assertEquals(0,p.waitNanos(564480,1000,0));assertEquals(0,p.waitNanos(1128960,2000,4096));}
    @Test public void healthyAudioPreservesStableVideoDeadline(){AudioAwareFramePacer p=new AudioAwareFramePacer(0,0);p.waitNanos(0,0,4096);assertEquals(15666666,p.waitNanos(564480,1000000,4096));}
    @Test public void starvationAllowsRecoveryInsteadOfRepeatedSleeping(){AudioAwareFramePacer p=new AudioAwareFramePacer(0,0);p.waitNanos(0,0,4096);assertEquals(0,p.waitNanos(564480,1000000,1000));assertEquals(0,p.waitNanos(1128960,2000000,3000));assertEquals(0,p.waitNanos(1693440,3000000,4096));}
    @Test public void restoredBufferDoesNotCauseLongCompensatingSleep(){AudioAwareFramePacer p=new AudioAwareFramePacer(0,0);p.waitNanos(100,1000000000,4096);assertEquals(15666666,p.waitNanos(564580,1001000000,4096));}
    @Test public void excessiveStartupLeadIsDrainedWithoutCatchupBurst(){
        AudioAwareFramePacer p=new AudioAwareFramePacer(0,0);p.waitNanos(0,0,4096);
        assertEquals(20000000,p.waitNanos(564480,16666666,8192));
        // Once the excess has drained, the next frame retains its normal
        // spacing rather than rushing to recover the added wait.
        assertEquals(16666666,p.waitNanos(1128960,36666666,4096));
    }
    @Test public void healthyDspBurstsDoNotTriggerExcessLeadCorrection(){
        AudioAwareFramePacer p=new AudioAwareFramePacer(0,0);p.waitNanos(0,0,4096);
        assertEquals(15666666,p.waitNanos(564480,1000000,6144));
        assertEquals(15666666,p.waitNanos(1128960,17666667,2048),1);
    }
}
